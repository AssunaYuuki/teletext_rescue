#include "tvinform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

// the Hamming stream over several lines (end of the file)
static void tviJoinBuild(TviStream& s);
static TviSummary tviJoinSummary(const TviStream& s);
static float tviSwing(const uint8_t* p, int fmt, int n);

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

bool tviSlice(const uint8_t* line, int fmt, int n, float T, int nbits, std::vector<uint8_t>& out, float& quality)
{
    // a 27 MHz capture window (1440 samples) is a few fТ1 bits shorter than the line: the
    // last bits may be missing (left 0; the Hamming code / stop bits cover them)
    const int spare = std::max(1, nbits / 20);
    if (T < 2 || n < (int)(T * (nbits - spare + 4))) return false;
    std::vector<float> s(n);
    for (int i = 0; i < n; i++) s[i] = sampleAt(line, fmt, i);
    std::vector<float> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + n * 8 / 100, sorted.end());
    float lo = sorted[n * 8 / 100];
    std::nth_element(sorted.begin(), sorted.begin() + n * 92 / 100, sorted.end());
    float hi = sorted[n * 92 / 100];
    const float full = fmt == 1 ? 65535.0f : 255.0f;
    if (hi - lo < 0.10f * full) return false;
    float thr = (lo + hi) / 2;
    // data start: first sustained rising edge (pilot pulse, first bit = 1)
    int lead = (int)(T * 3), hiend = std::min(n - (int)T - 1, (int)(T * (nbits / 3)));
    int st = -1;
    for (int i = std::max(1, lead); i < hiend; i++) {
        if (s[i] > thr && s[i - 1] <= thr) {
            float m = 0;
            int w = std::max(1, (int)T);
            for (int k = 0; k < w; k++) m += s[i + k];
            if (m / w > thr) { st = i; break; }
        }
    }
    if (st < 0 || st + T * (nbits - spare) > n) return false;
    // clock recovery: fit st + k*T to the zero crossings (least squares)
    double sk = 0, sx = 0, skk = 0, skx = 0;
    int m = 0, edges = 0;
    double prevEdge = -1;
    int gapHist[64] = {0};
    int end = std::min(n - 1, st + (int)(nbits * T) + (int)T);
    for (int i = st; i < end; i++) {
        if ((s[i] - thr) * (s[i + 1] - thr) < 0) {
            float frac = (thr - s[i]) / (s[i + 1] - s[i]);
            double e = i + frac;
            edges++;
            if (prevEdge >= 0) { int gi = (int)std::lround(e - prevEdge); if (gi >= 0 && gi < 64) gapHist[gi]++; }
            prevEdge = e;
            double k = std::round((e - st) / T);
            if (std::fabs((e - st) / T - k) < 0.35) { sk += k; sx += e; skk += k * k; skx += k * e; m++; }
        }
    }
    // the most common edge gap is the one-bit period; for a higher clock
    // (NABTS/WST) it is much smaller than T, so reject that.
    int modeGap = 0, modeCnt = 0;
    for (int gi = 2; gi < 64; gi++)
        if (gapHist[gi] > modeCnt) { modeCnt = gapHist[gi]; modeGap = gi; }
    if (edges < 8 || modeGap < 0.70 * T) return false;
    double Tf = T, st2 = st;
    if (m >= 20) {
        double det = m * skk - sk * sk;
        if (det != 0) {
            double t = (m * skx - sk * sx) / det, o = (sx - t * sk) / m;
            if (t > 0.9 * T && t < 1.1 * T) { Tf = t; st2 = o; }
        }
    }
    int nb = (nbits + 7) / 8;
    out.assign(nb, 0);
    quality = 1;
    for (int k = 0; k < nbits; k++) {
        double c = st2 + (k + 0.5) * Tf;
        if (c >= n - 1) break;
        float v = s[(int)std::lround(c)];
        if (v > thr) out[k / 8] |= 1 << (k % 8);       // bit 0 sent first
        float marg = std::fabs(v - thr) / (hi - lo) * 2;
        quality = std::min(quality, marg);
    }
    // pilot: first bit must be 1
    return out[0] & 1;
}

static int hamming(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

// header frame + 5 characters of 10-bit async serial (start 1, data LSB first, stop 0)
static bool tviUart(const std::vector<uint8_t>& w, int nbits, uint16_t& header, std::vector<uint8_t>& chars)
{
    auto bit = [&](int k) { return k < nbits && k / 8 < (int)w.size() ? (w[k / 8] >> (k % 8)) & 1 : -1; };
    for (int off = 24; off <= 34; off++) {
        bool ok = true;
        uint8_t v[6];
        for (int j = 0; j < 6 && ok; j++) {
            int b0 = off + 10 * j;
            if (bit(b0) != 1 || bit(b0 + 9) == 1) { ok = false; break; }   // stop may fall past the line end
            v[j] = 0;
            for (int i = 0; i < 8; i++)
                if (bit(b0 + 1 + i) == 1) v[j] |= 1 << i;
            if (j && bit(b0 + 8) >= 0 && (__builtin_popcount(v[j]) & 1)) ok = false;   // even parity
        }
        if (!ok) continue;
        // the frames follow the sync 55 55 27; the bare 1010 idle carrier also looks like frames
        int sync = 0;
        for (int i = 0; i < 8; i++) sync |= (bit(off - 14 + i) == 1) << i;
        if (sync != 0x27) continue;
        header = v[0];
        chars.assign(v + 1, v + 6);
        for (auto& c : chars) c = ~c & 0x7f;   // the line carries the inverted levels
        return true;
    }
    return false;
}

// the same with one damaged frame allowed (a start / stop bit or the parity wrong): ok[i] tells which characters
// came whole. Such a line is taken only when the other copy of its chunk confirms it (tviBuild)
static bool tviUartTol(const std::vector<uint8_t>& w, int nbits, uint16_t& header, std::vector<uint8_t>& chars, std::vector<char>& ok, int& bad)
{
    auto bit = [&](int k) { return k < nbits && k / 8 < (int)w.size() ? (w[k / 8] >> (k % 8)) & 1 : -1; };
    int bestBad = 2, bestOff = -1;   // at most one damaged frame
    for (int off = 24; off <= 34; off++) {
        int sync = 0;
        for (int i = 0; i < 8; i++) sync |= (bit(off - 14 + i) == 1) << i;
        if (sync != 0x27) continue;
        int nb = 0;
        for (int j = 0; j < 6; j++) {
            int b0 = off + 10 * j;
            uint8_t v = 0;
            for (int i = 0; i < 8; i++)
                if (bit(b0 + 1 + i) == 1) v |= 1 << i;
            bool good = bit(b0) == 1 && bit(b0 + 9) != 1 && (j == 0 || bit(b0 + 8) < 0 || !(__builtin_popcount(v) & 1));
            nb += !good;
        }
        if (nb < bestBad) { bestBad = nb; bestOff = off; }
    }
    if (bestOff < 0) return false;
    // the bare 1010 idle carrier also frames: a line of nearly every bit flipping is no data
    int flips = 0;
    for (int k = bestOff; k + 1 < bestOff + 60 && k + 1 < nbits; k++) flips += bit(k) != bit(k + 1);
    if (flips > 54) return false;
    uint8_t v[6];
    ok.assign(5, 1);
    for (int j = 0; j < 6; j++) {
        int b0 = bestOff + 10 * j;
        v[j] = 0;
        for (int i = 0; i < 8; i++)
            if (bit(b0 + 1 + i) == 1) v[j] |= 1 << i;
        bool good = bit(b0) == 1 && bit(b0 + 9) != 1 && (j == 0 || bit(b0 + 8) < 0 || !(__builtin_popcount(v[j]) & 1));
        if (j && !good) ok[j - 1] = 0;
    }
    header = v[0];
    chars.assign(v + 1, v + 6);
    for (auto& c : chars) c = ~c & 0x7f;
    bad = bestBad;
    return true;
}

// a chunk sent but not received: the sync 27 and a whole header frame, then 2 or more character frames
// damaged or blank (a dropout on the tape; ORT 30.03.1993 line 332: the line goes to the idle level after the header)
static bool tviUartLost(const std::vector<uint8_t>& w, int nbits, uint16_t& header)
{
    auto bit = [&](int k) { return k < nbits && k / 8 < (int)w.size() ? (w[k / 8] >> (k % 8)) & 1 : -1; };
    for (int off = 24; off <= 34; off++) {
        int sync = 0;
        for (int i = 0; i < 8; i++) sync |= (bit(off - 14 + i) == 1) << i;
        if (sync != 0x27 || bit(off) != 1 || bit(off + 9) == 1) continue;
        uint8_t h = 0;
        for (int i = 0; i < 8; i++)
            if (bit(off + 1 + i) == 1) h |= 1 << i;
        if (!h) continue;   // header 00: the idle line
        int bad = 0;
        for (int j = 1; j < 6; j++) {
            int b0 = off + 10 * j;
            uint8_t v = 0;
            for (int i = 0; i < 8; i++)
                if (bit(b0 + 1 + i) == 1) v |= 1 << i;
            bad += !(bit(b0) == 1 && bit(b0 + 9) != 1 && (bit(b0 + 8) < 0 || !(__builtin_popcount(v) & 1)));
        }
        if (bad >= 2) { header = h; return true; }
        return false;
    }
    return false;
}

// 1110 + 16-bit header (ends in 0001) + 10 Hamming(7,4) codewords sent d7 d6 d5 p4 d3 p2 p1,
// low nibble first. The slicer may miss the 1110 (it skips the first bits), so the codewords
// are found by the header end and by all of them being valid.
// The encoder also sends an error-correction test: "12345" / "67890" with a deliberate
// one-bit error walking through the codewords. Since Hamming(7,4) is perfect, any 70 bits
// decode, so a word with more than one correction is taken only after a header (all 16 bits)
// already seen on clean words.
static bool tviHamming(const std::vector<uint8_t>& w, int nbits, uint16_t& header, std::vector<uint8_t>& bytes, int& fixed,
                       const std::set<uint16_t>* known = nullptr)
{
    auto bit = [&](int k) { return k >= 0 && k < nbits && k / 8 < (int)w.size() ? (w[k / 8] >> (k % 8)) & 1 : 0; };
    auto hdrAt = [&](int g0) { uint16_t h = 0; for (int i = 0; i < 16; i++) h |= bit(g0 - 16 + i) << i; return h; };
    int best = -1, bestBad = known ? 11 : 2, bestNib[10] = {0};
    for (int g0 = 12; g0 + 66 <= nbits; g0++) {
        if (bit(g0 - 4) || bit(g0 - 3) || bit(g0 - 2) || !bit(g0 - 1)) continue;
        int flips = 0;
        for (int k = g0; k + 1 < g0 + 70 && k + 1 < nbits; k++) flips += bit(k) != bit(k + 1);
        if (flips > 60) continue;   // the 1010 idle carrier
        if (known && (g0 < 16 || !known->count(hdrAt(g0)))) continue;
        int nib[10], bad = 0;
        for (int g = 0; g < 10; g++) {
            int p[8] = {0}, syn = 0;
            for (int t = 0; t < 7; t++) p[7 - t] = bit(g0 + 7 * g + t);
            for (int i = 1; i < 8; i++)
                if (p[i]) syn ^= i;
            if (syn) p[syn] ^= 1;
            nib[g] = p[3] << 3 | p[5] << 2 | p[6] << 1 | p[7];
            bad += syn != 0;
        }
        if (bad < bestBad) { best = g0; bestBad = bad; std::copy(nib, nib + 10, bestNib); }   // fewest corrections wins
    }
    if (best < 0) return false;
    fixed = bestBad;
    header = hdrAt(best);
    bytes.clear();
    for (int g = 0; g < 10; g += 2) bytes.push_back((uint8_t)~(bestNib[g + 1] << 4 | bestNib[g]));   // inverted levels
    return true;
}

// ORT 2001 (sample.vbi lines 318, 319): a few lead bits, then 12 Hamming(7,4) codewords (sent as above,
// low nibble first) = 6 bytes, the first two a header. All 12 have to check (36 parity bits), one
// correction allowed. Levels inverted as on the other kinds.
static bool tviHamming12(const std::vector<uint8_t>& w, int nbits, std::vector<uint8_t>& bytes, int& fixed)
{
    auto bit = [&](int k) { return k >= 0 && k < nbits && k / 8 < (int)w.size() ? (w[k / 8] >> (k % 8)) & 1 : 0; };
    int best = -1, bestBad = 2, bestNib[12] = {0};
    for (int g0 = 0; g0 + 84 <= nbits; g0++) {
        int flips = 0;
        for (int k = g0; k + 1 < g0 + 84; k++) flips += bit(k) != bit(k + 1);
        if (flips > 70) continue;   // the 1010 idle carrier
        int nib[12], bad = 0;
        for (int g = 0; g < 12 && bad < bestBad; g++) {
            int p[8] = {0}, syn = 0;
            for (int t = 0; t < 7; t++) p[7 - t] = bit(g0 + 7 * g + t);
            for (int i = 1; i < 8; i++)
                if (p[i]) syn ^= i;
            if (syn) p[syn] ^= 1;
            nib[g] = p[3] << 3 | p[5] << 2 | p[6] << 1 | p[7];
            bad += syn != 0;
        }
        if (bad < bestBad) { best = g0; bestBad = bad; std::copy(nib, nib + 12, bestNib); }
    }
    if (best < 0) return false;
    fixed = bestBad;
    bytes.clear();
    for (int g = 0; g < 12; g += 2) bytes.push_back((uint8_t)~(bestNib[g + 1] << 4 | bestNib[g]));
    return true;
}

template <class... A> static std::string fmt_n(const char* f, A... a)
{
    char b[256];
    snprintf(b, sizeof b, f, a...);
    return b;
}

// character sets: async chunks are 7-bit KOI-7 (SO = Cyrillic, SI = Latin); Hamming chunks are
// 8-bit, KOI8-R in the encoder's own test message («ТВ-ИНФОРМ»), CP866 (DOS) in the files it sends
enum { kCsKoi7 = 0, kCsKoi8 = 1, kCsCp866 = 2 };

// UTF-8 of one character
static void tviPut(std::string& t, uint8_t x, int cs, bool& cyr)
{
    static const char* koi = "юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ";
    static const char* dos = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмноп";   // 80..AF
    static const char* dos2 = "рстуфхцчшщъыьэюяЁё";                                  // E0..F1
    if (cs == kCsKoi7) {
        if (x == 0x0E) { cyr = true; return; }
        if (x == 0x0F) { cyr = false; return; }
        if (cyr && x >= 0x40 && x < 0x7F) x |= 0x80;
    }
    if (x >= 32 && x < 127) t += (char)x;
    else if (x == 0xFF) t += '.';   // filler (inverted 00)
    else if (cs == kCsCp866) {
        if (x >= 0x80 && x < 0xB0) t.append(dos + 2 * (x - 0x80), 2);   // 2 bytes per letter in UTF-8
        else if (x >= 0xE0 && x < 0xF2) t.append(dos2 + 2 * (x - 0xE0), 2);
        else t += '.';
    } else if (x >= 0xC0) t.append(koi + 2 * (x - 0xC0), 2);
    else if (x == 0xA3) t += "ё";
    else if (x == 0xB3) t += "Ё";
    else t += '.';
}

static std::string tviShow(const std::vector<uint8_t>& c, int cs)
{
    std::string t;
    bool cyr = false;
    for (uint8_t x : c) tviPut(t, x, cs, cyr);
    return t;
}

// KOI8-R or CP866: the one in which the common Russian letters come out
static int tviCharset(const std::vector<const TviChunk*>& ch, int kind)
{
    if (kind == 0) return kCsKoi7;
    auto common = [](uint8_t x, int cs) {
        static const char16_t* top = u"оеаинтсрвлкмдпуОЕАИНТСРВЛКМДПУ";
        char16_t u = 0;
        if (cs == kCsKoi8 && x >= 0xC0) u = u"юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ"[x - 0xC0];
        if (cs == kCsCp866 && x >= 0x80 && x < 0xB0) u = u"АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмноп"[x - 0x80];
        if (cs == kCsCp866 && x >= 0xE0 && x < 0xF0) u = u"рстуфхцчшщъыьэюя"[x - 0xE0];
        for (const char16_t* p = top; u && *p; p++)
            if (*p == u) return 1;
        return 0;
    };
    long koi = 0, dos = 0;
    for (auto* c : ch)
        for (uint8_t x : c->chars) { koi += common(x, kCsKoi8); dos += common(x, kCsCp866); }
    return dos > koi ? kCsCp866 : kCsKoi8;
}

static const char* tviCharsetName(int cs) { return cs == kCsKoi7 ? "KOI-7" : cs == kCsCp866 ? "CP866" : "KOI8-R"; }

// the characters of one channel joined into text lines (CR/LF ends a line, other control codes dropped)
static std::vector<std::string> tviLines(const std::vector<const TviChunk*>& ch, int cs)
{
    std::vector<std::string> out;
    std::string cur;
    bool cyr = false;
    uint8_t prev = 0;
    // (no [...] by frame gaps between Hamming chunks: tried on ostankino 1995 - the inserter's pace is uneven,
    // gaps of 8 frames stood inside whole words; where chunks were lost cannot be told from the frames)
    for (auto* c : ch) {
        // a chunk that was sent (its header came) but not received (a dropout on the tape): [...], else the
        // words either side run together and the text looks mixed up
        if (c->chars.empty() && c->fixed == 255) { cur += "[\xE2\x80\xA6]"; continue; }
        for (uint8_t x : c->chars) {
            if (x == 0x0D || x == 0x0A) {
                if (!cur.empty() || (x == 0x0D || prev != 0x0D)) out.push_back(cur);   // keep empty lines of a document
                cur.clear();
            } else if (x < 0x20 || x == 0x7F || x == 0xFF) {   // control codes, DEL / FF filler
                if (cs == kCsKoi7 && (x == 0x0E || x == 0x0F)) cyr = x == 0x0E;
            } else tviPut(cur, x, cs, cyr);
            prev = x;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::vector<const TviChunk*> tviChunks(const TviStream& s, int kind)
{
    std::vector<const TviChunk*> ch;
    for (auto& c : s.serial)
        if (c.kind == kind) ch.push_back(&c);
    return ch;
}

std::string tviText(const TviStream& s)
{
    std::string t;
    if (s.joined) {
        for (auto& l : s.texts) t += l + "\n";
        return t;
    }
    for (int kind = 0; kind < 2; kind++) {
        auto ch = tviChunks(s, kind);
        if (ch.empty()) continue;
        int cs = tviCharset(ch, kind);
        for (auto& l : tviLines(ch, cs)) t += l + "\n";
    }
    return t;
}

std::string tviSerialText(const TviStream& s, double fps)
{
    std::string t;
    char b[96];
    int cs[2] = {kCsKoi7, tviCharset(tviChunks(s, 1), 1)};
    const int cs3 = s.joined ? tviCharset(tviChunks(s, 3), 3) : kCsKoi8;
    for (auto& c : s.serial) {
        double sec = c.frame / (fps > 0 ? fps : 25);
        snprintf(b, sizeof b, "%02d:%02d:%05.2f  %-8lld %s %04x  ", (int)(sec / 3600), (int)(sec / 60) % 60, std::fmod(sec, 60.0),
                 (long long)c.frame, c.kind == 3 ? "stream " : c.kind ? "hamming" : "async  ", c.header);
        t += b;
        for (uint8_t x : c.chars) { snprintf(b, sizeof b, "%02x ", x); t += b; }
        t += " " + tviShow(c.chars, c.kind == 3 ? cs3 : c.kind) + "\n";
    }
    return t;
}

void tviBuild(TviStream& s)
{
    if (s.joined) { tviJoinBuild(s); return; }
    char b[200];
    s.report.clear();
    double mhz = kTviRateHz[s.rate] / 1e6;
    snprintf(b, sizeof b, "ТВ-информ (GOST R 50822-95): %zu lines, clock fТ%d=%.4f MHz, %d bits/line (%zu bytes)",
             s.words.size(), s.rate + 1, mhz, s.nbits, s.words.empty() ? 0 : s.words[0].bytes.size());
    s.report.push_back(b);
    if (s.words.empty()) return;

    // payload entropy (8 bits/byte = random; lower means real structure)
    int64_t freq[256] = {0}, tot = 0;
    for (auto& w : s.words)
        for (uint8_t x : w.bytes) { freq[x]++; tot++; }
    double H = 0;
    for (int i = 0; i < 256; i++)
        if (freq[i]) { double p = (double)freq[i] / tot; H -= p * std::log2(p); }
    s.entropy = H;

    // carousel pages per line: cluster by Hamming distance and vote each cluster
    std::map<std::pair<int, int>, std::vector<const TviWord*>> byLine;
    for (auto& w : s.words) byLine[{w.lineNo, w.field}].push_back(&w);
    size_t nb = s.words[0].bytes.size();
    const int thresh = (int)(nb * 8 * 0.09);   // ~9% of the bits
    for (auto& kv : byLine) {
        std::vector<std::pair<std::vector<uint8_t>, std::vector<const TviWord*>>> reps;
        for (auto* w : kv.second) {
            bool placed = false;
            for (auto& r : reps)
                if (hamming(w->bytes, r.first) <= thresh) { r.second.push_back(w); placed = true; break; }
            if (!placed) reps.push_back({w->bytes, {w}});
        }
        std::sort(reps.begin(), reps.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
        for (auto& r : reps) {
            if (r.second.size() < 5) break;
            TviPage pg;
            pg.lineNo = kv.first.first;
            pg.field = kv.first.second;
            pg.copies = (int)r.second.size();
            pg.bytes.assign(nb, 0);
            for (size_t j = 0; j < nb; j++) {
                int ones[8] = {0};
                for (auto* w : r.second)
                    for (int k = 0; k < 8; k++) ones[k] += (w->bytes[j] >> k) & 1;
                for (int k = 0; k < 8; k++)
                    if (ones[k] * 2 > (int)r.second.size()) pg.bytes[j] |= 1 << k;
            }
            s.pages.push_back(std::move(pg));
        }
    }
    snprintf(b, sizeof b, "  payload entropy %.2f bits/byte (8 = random; lower = real structure, likely %s)", s.entropy,
             s.entropy > 7.3 ? "encrypted/compressed" : "structured records");
    s.report.push_back(b);
    snprintf(b, sizeof b, "  %zu stable carousel pages recovered by voting repeats (payload is the proprietary AO ТВ-информ format, shown as bytes)",
             s.pages.size());
    s.report.push_back(b);
    std::sort(s.pages.begin(), s.pages.end(), [](auto& a, auto& c) { return a.copies > c.copies; });
    for (size_t i = 0; i < s.pages.size() && i < 12; i++) {
        auto& pg = s.pages[i];
        std::string hex;
        for (uint8_t x : pg.bytes) { char t[4]; snprintf(t, sizeof t, "%02x", x); hex += t; }
        snprintf(b, sizeof b, "  line %d field %d  x%-4d %s", pg.lineNo, pg.field, pg.copies, hex.c_str());
        s.report.push_back(b);
    }

    // fТ1: serial characters straight from the encoder's UART, and Hamming(7,4) bytes
    s.serial.clear();
    if (s.rate != 0) return;
    int framed[3] = {0, 0, 0}, fixedSum = 0, ecTest = 0;
    std::vector<uint8_t> prev[3];
    std::set<uint16_t> hdrs;   // headers of clean Hamming words (seen at least 3 times)
    {
        std::map<uint16_t, int> hc;
        for (auto& w : s.words) {
            uint16_t h;
            std::vector<uint8_t> by;
            int fx;
            std::vector<uint8_t> tmp;
            if (!tviUart(w.bytes, s.nbits, h, tmp) && tviHamming(w.bytes, s.nbits, h, by, fx) && fx == 0) hc[h]++;
        }
        for (auto& kv : hc)
            if (kv.second >= 3) hdrs.insert(kv.first);
    }
    // async chunks: each goes out on two lines; a line with a damaged character is kept and the other copy mends
    // it (the copies agree on every whole character, same header, a few frames apart). A third equal line is a
    // new chunk (indents of spaces), not a copy
    struct PendA { bool on = false; TviChunk c; std::vector<char> ok; int copies = 0, bad = 0; } pa;
    auto flushA = [&]() {
        if (!pa.on) return;
        // a damaged line no copy confirmed: noise or another kind of line framed by chance - left out
        if (pa.bad && pa.copies < 2) { pa.on = false; return; }
        int left = 0;
        for (char k : pa.ok) left += !k;
        pa.c.fixed = (uint8_t)left;   // characters no copy brought whole (kept as read)
        s.serial.push_back(pa.c);
        pa.on = false;
    };
    for (auto& w : s.words) {
        TviChunk c;
        int fixed = 0;
        {
            uint16_t lh;
            if (tviUartLost(w.bytes, s.nbits, lh)) {   // sent, not received: a mark for [...] (kept below if its header is the text's)
                flushA();
                TviChunk m;
                m.kind = 0; m.header = lh; m.frame = w.frame; m.fixed = 255;
                s.serial.push_back(std::move(m));
                continue;
            }
            uint16_t h;
            std::vector<uint8_t> ch;
            std::vector<char> ok;
            int bad = 0;
            if (tviUartTol(w.bytes, s.nbits, h, ch, ok, bad)) {
                framed[0]++;
                if (pa.on && pa.copies < 2 && h == pa.c.header && w.frame - pa.c.frame <= 12) {
                    int agree = 0, clash = 0;
                    for (int i = 0; i < 5; i++)
                        if (ok[i] && pa.ok[i]) (ch[i] == pa.c.chars[i] ? agree : clash)++;
                    if (!clash && agree >= 3) {
                        for (int i = 0; i < 5; i++)
                            if (!pa.ok[i] && ok[i]) { pa.c.chars[i] = ch[i]; pa.ok[i] = 1; }
                        pa.copies++;
                        continue;
                    }
                }
                flushA();
                pa.on = true;
                pa.copies = 1;
                pa.bad = bad;
                pa.c = TviChunk();
                pa.c.kind = 0;
                pa.c.header = h;
                pa.c.chars = ch;
                pa.c.frame = w.frame;
                pa.ok = ok;
                continue;
            }
        }
        if (tviHamming12(w.bytes, s.nbits, c.chars, fixed) && fixed == 0) { c.kind = 2; c.header = (uint16_t)(c.chars[0] << 8 | c.chars[1]); }
        else if (tviHamming(w.bytes, s.nbits, c.header, c.chars, fixed)) c.kind = 1;
        else if (tviHamming12(w.bytes, s.nbits, c.chars, fixed)) { c.kind = 2; c.header = (uint16_t)(c.chars[0] << 8 | c.chars[1]); }
        else if (!hdrs.empty() && tviHamming(w.bytes, s.nbits, c.header, c.chars, fixed, &hdrs)) { c.kind = 1; ecTest++; }
        else continue;
        framed[c.kind]++;
        fixedSum += fixed;
        if (c.chars == prev[c.kind]) continue;   // every chunk goes out on two lines
        prev[c.kind] = c.chars;
        c.frame = w.frame;
        c.fixed = (uint8_t)fixed;
        s.serial.push_back(std::move(c));
    }
    flushA();
    std::stable_sort(s.serial.begin(), s.serial.end(), [](const TviChunk& x, const TviChunk& y) { return x.frame < y.frame; });
    {   // marks of lost chunks: only under a header the received text uses, one for a run of them
        std::map<uint16_t, int> hc;
        for (auto& c : s.serial) if (c.kind == 0 && !c.chars.empty()) hc[c.header]++;
        std::vector<TviChunk> keep;
        for (auto& c : s.serial) {
            bool mark = c.kind == 0 && c.chars.empty() && c.fixed == 255;
            if (mark && (hc[c.header] < 10 || (!keep.empty() && keep.back().kind == 0 && keep.back().chars.empty() && keep.back().fixed == 255))) continue;
            keep.push_back(std::move(c));
        }
        s.serial = std::move(keep);
    }
    // the 6-byte Hamming kind: fill lines (a few patterns over and over) and data lines (a header and 4 bytes
    // that change; ORT 2001 each sent twice on 318, once on 319)
    if (framed[2] >= 10) {
        auto ch = tviChunks(s, 2);
        snprintf(b, sizeof b, "  Hamming channel (6 bytes): %d of %zu lines = lead bits + 12 Hamming(7,4) codewords, low nibble first, levels inverted:",
                 framed[2], s.words.size());
        s.report.push_back(b);
        std::map<std::vector<uint8_t>, int> count;
        std::map<uint16_t, std::set<std::vector<uint8_t>>> byHdr;
        std::map<uint16_t, int> perHdr;
        for (auto* c : ch) { count[c->chars]++; byHdr[c->header].insert(c->chars); perHdr[c->header]++; }
        std::vector<std::pair<int, std::vector<uint8_t>>> fill;
        for (auto& kv : count) if (kv.second >= 5) fill.push_back({kv.second, kv.first});
        std::sort(fill.rbegin(), fill.rend());
        s.report.push_back("  fill lines (the same 6 bytes over and over):");
        for (auto& f : fill) {
            std::string hex;
            for (uint8_t x : f.second) { char t[4]; snprintf(t, sizeof t, "%02X ", x); hex += t; }
            snprintf(b, sizeof b, "    x%-5d %s", f.first, hex.c_str());
            s.report.push_back(b);
        }
        for (auto& kv : byHdr) {
            if (kv.second.size() < 5) continue;   // a header whose 4 bytes keep changing: data
            std::vector<uint8_t> data;
            std::vector<uint8_t> last;
            for (auto* c : ch)
                if (c->header == kv.first && c->chars != last) { data.insert(data.end(), c->chars.begin() + 2, c->chars.end()); last = c->chars; }
            int64_t f[256] = {0};
            for (uint8_t x : data) f[x]++;
            double H = 0;
            for (int i = 0; i < 256; i++) if (f[i]) { double p = (double)f[i] / data.size(); H -= p * std::log2(p); }
            snprintf(b, sizeof b, "  data lines, header %02X %02X: %d lines, %zu different, %zu bytes, %.2f bits/byte (random-looking: probably encrypted)",
                     kv.first >> 8, kv.first & 255, perHdr[kv.first], kv.second.size(), data.size(), H);
            s.report.push_back(b);
            std::string hex;
            for (size_t i = 0; i < data.size() && i < 64; i++) { char t[4]; snprintf(t, sizeof t, "%02X ", data[i]); hex += t; }
            s.report.push_back("    " + hex + (data.size() > 64 ? "..." : ""));
        }
    }
    for (int kind = 0; kind < 2; kind++) {
        if (framed[kind] < 10) continue;
        auto ch = tviChunks(s, kind);
        int cs = tviCharset(ch, kind);
        if (kind == 0) {
            snprintf(b, sizeof b, "  serial channel: %d of %zu lines carry 10-bit async frames (start 1, 8 data bits LSB first, stop 0):",
                     framed[0], s.words.size());
            s.report.push_back(b);
            snprintf(b, sizeof b, "  a header frame + 5 characters, 7-bit KOI-7 with even parity, levels inverted, each chunk sent twice; %zu chunks", ch.size());
        } else {
            snprintf(b, sizeof b, "  Hamming channel: %d of %zu lines = 1110 + 16-bit header + 10 Hamming(7,4) codewords (%d bits corrected):",
                     framed[1], s.words.size(), fixedSum);
            s.report.push_back(b);
            snprintf(b, sizeof b, "  5 bytes per line (%s, levels inverted); %zu chunks", tviCharsetName(cs), ch.size());
            if (ecTest) {
                s.report.push_back(b);
                snprintf(b, sizeof b, "  error-correction test: %d lines carry words with a deliberate 1-bit error in several codewords (decoded)", ecTest);
            }
        }
        s.report.push_back(b);
        // a carousel: a few distinct chunks make up nearly all of the stream
        std::map<std::vector<uint8_t>, int> count;
        std::vector<std::vector<uint8_t>> order;
        for (auto* c : ch)
            if (!count[c->chars]++) order.push_back(c->chars);
        int common = 0, kinds = 0;
        for (auto& kv : count)
            if (kv.second >= 3) { common += kv.second; kinds++; }
        if (kinds <= 40 && common >= (int)ch.size() * 95 / 100) {
            snprintf(b, sizeof b, "  carousel: %d distinct chunks make up %d of %zu (the same block of characters sent over and over):", kinds,
                     common, ch.size());
            s.report.push_back(b);
            for (auto& c : order)
                if (count[c] >= 3) {
                    std::string hex;
                    for (uint8_t x : c) { char t[4]; snprintf(t, sizeof t, "%02x ", x); hex += t; }
                    snprintf(b, sizeof b, "    x%-6d %s %s", count[c], hex.c_str(), tviShow(c, cs).c_str());
                    s.report.push_back(b);
                }
        } else {
            std::vector<uint8_t> all;
            for (auto* c : ch) all.insert(all.end(), c->chars.begin(), c->chars.end());
            if (all.size() > 1024) all.resize(1024);
            s.report.push_back("  characters (start):");
            for (size_t i = 0; i < all.size(); i += 64)
                s.report.push_back("    " + tviShow(std::vector<uint8_t>(all.begin() + i, all.begin() + std::min(all.size(), i + 64)), cs));
        }
        // the text itself: for a carousel its distinct lines, most frequent first; otherwise the document
        auto lines = tviLines(ch, cs);
        if (kinds <= 40 && common >= (int)ch.size() * 95 / 100) {
            std::map<std::string, int> lc;
            std::vector<std::string> lorder;
            for (auto& l : lines)
                if (!l.empty() && !lc[l]++) lorder.push_back(l);
            std::stable_sort(lorder.begin(), lorder.end(), [&](auto& a, auto& c) { return lc[a] > lc[c]; });
            if (!lorder.empty()) {
                snprintf(b, sizeof b, "  text (%zu distinct lines):", lorder.size());
                s.report.push_back(b);
                for (size_t i = 0; i < lorder.size() && i < 12; i++) {
                    if (lc[lorder[i]] < 2 && i >= 6) break;
                    snprintf(b, sizeof b, "    x%-6d ", lc[lorder[i]]);
                    s.report.push_back(b + lorder[i].substr(0, 120));
                }
            }
        } else if (!lines.empty()) {
            snprintf(b, sizeof b, "  text (%s, %zu lines; whole text in tvinform_text.txt):", tviCharsetName(cs), lines.size());
            s.report.push_back(b);
            for (size_t i = 0; i < lines.size() && i < 40; i++) s.report.push_back("    " + lines[i]);
            if (lines.size() > 40) s.report.push_back("    ...");
        }
    }
}

TviSummary tviSummarize(const TviStream& s)
{
    if (s.joined) return tviJoinSummary(s);
    TviSummary r;
    r.lines = (int)s.words.size();
    if (s.rate < 0 || s.rate == 2) {   // Norpak packets / fТ3 file transfer records: the report is all there is
        r.kind = TviSummary::Packets;
        r.what = s.rate < 0 ? "packets (Norpak equipment, 55 55 E7)" : "file transfer records (fТ3, CRC-32)";
        for (auto& l : s.report) r.text.push_back(l);
        return r;
    }
    for (auto& c : s.serial) if (c.kind) { r.fixed += c.fixed; r.ecTest += c.fixed >= 2; }
    r.chunks = (int)s.serial.size();
    if (s.serial.empty()) {
        r.what = s.pages.size() > 1 ? fmt_n("carousel of %zu packets, no readable characters", s.pages.size()) : "idle carrier only";
        for (auto& l : s.report) r.text.push_back(l);
        return r;
    }
    bool test = true;
    for (int kind = 0; kind < 2; kind++) {
        auto ch = tviChunks(s, kind);
        if (ch.empty()) continue;
        int cs = tviCharset(ch, kind);
        r.charset += (r.charset.empty() ? "" : " + ") + std::string(tviCharsetName(cs));
        r.dataLines += (int)ch.size();
        std::map<std::vector<uint8_t>, int> count;
        for (auto* c : ch) count[c->chars]++;
        int common = 0, kinds = 0;
        for (auto& kv : count)
            if (kv.second >= 3) { common += kv.second; kinds++; }
        auto lines = tviLines(ch, cs);
        bool carousel = kinds <= 40 && common >= (int)ch.size() * 95 / 100;
        if (!carousel) test = false;
        if (carousel) {
            std::map<std::string, int> lc;
            std::vector<std::string> order;
            for (auto& l : lines)
                if (!l.empty() && !lc[l]++) order.push_back(l);
            std::stable_sort(order.begin(), order.end(), [&](auto& a, auto& b) { return lc[a] > lc[b]; });
            for (size_t i = 0; i < order.size() && i < 8; i++)
                if (lc[order[i]] >= 2 || i < 3) r.text.push_back(order[i] + fmt_n("   (x%d)", lc[order[i]]));
        } else
            for (auto& l : lines) r.text.push_back(l);
        if (kind == 0 && s.serial.size() != ch.size()) r.text.push_back("");
    }
    int lines = 0;
    for (auto& l : r.text) lines += !l.empty();
    // the 6-byte Hamming kind: data portions, not characters
    int bin = 0;
    for (auto& c : s.serial) bin += c.kind == 2;
    bool binData = false;
    for (auto& l : s.report) binData |= l.rfind("  data lines, header", 0) == 0;
    if (bin && binData) {
        for (auto& l : s.report)
            if (l.rfind("  Hamming channel (6 bytes)", 0) == 0 || l.rfind("  data lines", 0) == 0 || (l.rfind("    ", 0) == 0 && l.find(" x") == std::string::npos))
                r.text.push_back(l.substr(2));
        r.kind = TviSummary::Data;
        r.what = lines ? fmt_n("text, %d lines, and binary data (6-byte Hamming lines)", lines) : "binary data in 6-byte Hamming lines (random-looking: probably encrypted)";
        return r;
    }
    r.kind = test ? TviSummary::Test : TviSummary::Data;
    r.what = test ? "test message of the inserter (the same characters over and over)" : fmt_n("text, %d lines", lines);
    return r;
}

// (92nd - 8th percentile) of a line as a share of full scale
static float tviSwing(const uint8_t* p, int fmt, int n)
{
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i];
    std::nth_element(v.begin(), v.begin() + n * 8 / 100, v.end());
    float lo = v[n * 8 / 100];
    std::nth_element(v.begin(), v.begin() + n * 92 / 100, v.end());
    return (v[n * 92 / 100] - lo) / (fmt == 1 ? 65535.0f : 255.0f);
}

// fТ1 bits read at fТ2 come in equal pairs. Share of equal pairs at the better of the two phases.
double tviPairShare(const std::vector<uint8_t>& w, int nbits, int* phase)
{
    auto bit = [&](int k) { return (w[k / 8] >> (k % 8)) & 1; };
    double best = 0;
    for (int p = 0; p < 2; p++) {
        int eq = 0, n = 0;
        for (int k = p; k + 1 < nbits; k += 2, n++) eq += bit(k) == bit(k + 1);
        double r = n ? (double)eq / n : 0;
        if (r > best) { best = r; if (phase) *phase = p; }
    }
    return best;
}
// one bit of each pair: fТ2 bytes -> fТ1 bytes (bit 0 sent first)
std::vector<uint8_t> tviCollapse(const std::vector<uint8_t>& w, int nbits)
{
    int ph = 0;
    tviPairShare(w, nbits, &ph);
    std::vector<uint8_t> o((nbits / 2 + 7) / 8, 0);
    for (int k = 0; ph + 2 * k < nbits && k < nbits / 2; k++)
        if ((w[(ph + 2 * k) / 8] >> ((ph + 2 * k) % 8)) & 1) o[k / 8] |= 1 << (k % 8);
    return o;
}

TviLineTest tviTestLine(const uint8_t* probe, int probeN, size_t frameBytes, size_t lineOff, int fmt, int samples, float fs, int force, bool capJob)
{
    auto tviT = [&](int rate) { return fs / kTviRateHz[rate]; };   // samples per bit
    TviLineTest out;
    int rate = -1, hits = 0;
    for (int r = 0; r < 2 && rate < 0; r++) {
        if (force >= 0 && r != force) continue;
        if (force < 0 && tviT(r) < 6.0f) continue;
        int h = 0;
        float q;
        std::vector<uint8_t> w;
        int sig = 0, pre = 0;
        std::map<std::vector<uint8_t>, int> seen;   // the sliced words: a carousel sends the same ones again
        for (int i = 0; i < probeN; i++) {
            const uint8_t* p = &probe[(size_t)i * frameBytes + lineOff];
            sig += tviSwing(p, fmt, samples) >= 0.10f;
            if (!tviSlice(p, fmt, samples, tviT(r), kTviBits[r], w, q)) continue;
            h++;
            auto idle = [](uint8_t v) { return v == 0x55 || v == 0xAA; };
            int nIdle = 0;
            bool lead = false;
            for (size_t k = 0; k < w.size(); k++) {
                nIdle += idle(w[k]);
                if (k < 3 && k + 1 < w.size() && idle(w[k]) && idle(w[k + 1])) lead = true;
            }
            pre += lead || nIdle >= 4;
            seen[w]++;
        }
        int rep = 0;
        for (auto& kv : seen) if (kv.second > 1) rep += kv.second;
        // >= 60% of the probes that carry a signal slice with a pilot (the service may be on for only
        // part of a recording), and real ТВ-информ lines start with the 0101 idle carrier / 55 55 sync
        // (ORT: 5F 55 55.., ТВЦ: 55 55 27); a tape's own line signal at a similar rate does not (Piglet's Big Movie, UK VHS)
        // (field 2 of a 1990s ТЕЛЕИНФ tape: 78%, more data packets between the idle carrier; Piglet: 0%)
        bool strict = !capJob || (h * 10 >= sig * 8 && pre * 10 >= h * 7);
        // fТ1 without the idle carrier (ORT 2001, sample.vbi lines 329 / 330: white between the words): a carousel -
        // at least 3 in 10 sliced words come again in the probes, and there are at least 8 different words
        bool carousel = r == 0 && !capJob && h * 10 >= sig * 5 && rep * 10 >= h * 3 && seen.size() >= 8;
        // a carousel changes its words from frame to frame; a waveform that stays the same in every frame is a
        // constant signal whose slicing only varies with noise (europa.vbi line 16: three levels, frames differ
        // by 6% of the swing)
        if (carousel) {
            auto at = [&](const uint8_t* p, int i) { return fmt ? (p[2 * i] | p[2 * i + 1] << 8) >> 8 : (int)p[i]; };
            std::vector<double> d;
            for (int i = 0; i + 1 < probeN; i++) {
                const uint8_t* a = &probe[(size_t)i * frameBytes + lineOff];
                const uint8_t* b = &probe[(size_t)(i + 1) * frameBytes + lineOff];
                if (tviSwing(a, fmt, samples) < 0.10f || tviSwing(b, fmt, samples) < 0.10f) continue;
                int lo = 255, hi = 0;
                double s = 0;
                for (int k = samples / 20; k < samples - samples / 20; k++) {
                    int va = at(a, k);
                    lo = std::min(lo, va); hi = std::max(hi, va);
                    s += std::abs(va - at(b, k));
                }
                if (hi > lo) d.push_back(s / (samples - 2 * (samples / 20)) / (hi - lo));
            }
            if (d.size() >= 8) {
                std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
                if (d[d.size() / 2] < 0.12) carousel = false;
                if (getenv("TR_DEBUG")) fprintf(stderr, "tviTestLine carousel: frame-to-frame change %.3f of the swing\n", d[d.size() / 2]);
            }
        }
        // the same word in nearly every frame, and not the idle carrier (55 / AA): a constant test or
        // identification signal of the station (europa.vbi line 16: three levels, one word in 84% of the lines)
        int most = 0, mostIdle = 0;
        for (auto& kv : seen)
            if (kv.second > most) {
                most = kv.second;
                mostIdle = 0;
                for (uint8_t v : kv.first) mostIdle += v == 0x55 || v == 0xAA;
            }
        bool constant = h >= 10 && most * 10 >= h * 8 && mostIdle < 4;
        if (getenv("TR_DEBUG")) fprintf(stderr, "tviTestLine rate %d: sig %d sliced %d pilot %d words %zu most %d (idle bytes %d)%s\n", r, sig, h, pre, seen.size(), most, mostIdle, constant ? " constant" : "");
        if (constant && force < 0) continue;
        if ((sig * 10 >= probeN && ((h * 10 >= sig * 6 && pre * 10 >= h && strict) || carousel)) || force >= 0) { rate = r; hits = h; }
    }
    if (rate < 0) return out;
    // an fТ1 line that the fТ1 pass missed (strong colour burst in the window, ТВЦ line 19)
    // reads at fТ2 with every bit doubled: then it is fТ1, read at fТ2 and halved
    bool pairs = false;
    if (rate == 1 && force < 0) {
        int dbl = 0, ok = 0;
        float q;
        std::vector<uint8_t> w;
        for (int i = 0; i < probeN; i++)
            if (tviSlice(&probe[(size_t)i * frameBytes + lineOff], fmt, samples, tviT(1), kTviBits[1], w, q)) {
                ok++;
                dbl += tviPairShare(w, kTviBits[1]) >= 0.90;
            }
        pairs = ok && dbl >= 0.8 * ok;
    }
    out.rate = rate; out.hits = hits; out.pairs = pairs;
    return out;
}

// ---- fТ1 Hamming(7,4) stream over several lines (TviGridSlicer, tviJoin, tviJoinBuild) ------------------------

static float tviInterp(const std::vector<float>& s, float x)
{
    int i = (int)x;
    if (s.empty()) return 0;
    if (i < 0) return s[0];
    if (i + 1 >= (int)s.size()) return s.back();
    float f = x - i;
    return s[i] * (1 - f) + s[i + 1] * f;
}

void TviGridSlicer::add(const uint8_t* line, int fmt, int samples, int64_t frame, int lineNo, int field)
{
    if (samples < 8) return;
    Pending p{frame, lineNo, field, std::vector<float>(samples)};
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < samples; i++) { p.s[i] = sampleAt(line, fmt, i); lo = std::min(lo, p.s[i]); hi = std::max(hi, p.s[i]); }
    // no signal on the line in this frame: flat. (Not the swing between percentiles: a line of all 0 data is
    // white from end to end and is data too)
    if (hi - lo < 0.10f * (fmt == 1 ? 65535.0f : 255.0f)) return;
    if (trained_) { emit(p); return; }
    buf_.push_back(std::move(p));
    if (buf_.size() >= 48) train();
}

void TviGridSlicer::train()
{
    if (buf_.empty()) return;
    // threshold half way between the dark level (blanking = a 0 bit; the 20th percentile is above the sync
    // tip of an equalizing pulse) and the white level
    std::vector<float> all;
    for (auto& p : buf_)
        for (size_t i = 0; i < p.s.size(); i += 3) all.push_back(p.s[i]);
    size_t q1 = all.size() * 20 / 100, q2 = all.size() * 97 / 100;
    std::nth_element(all.begin(), all.begin() + q1, all.end());
    float dark = all[q1];
    std::nth_element(all.begin(), all.begin() + q2, all.end());
    float white = all[q2];
    thr_ = (dark + white) / 2;
    const float clip = std::max(1.0f, (white - dark) / 3);
    const int n = (int)buf_[0].s.size();
    // phase of the grid: the widest eye over the lines gathered
    double best = -1;
    for (float ph = 0; ph < T_; ph += 0.25f) {
        double m = 0;
        long c = 0;
        for (size_t i = 0; i < buf_.size(); i += 2)
            for (float x = ph; x < n - 1; x += T_) { m += std::min(std::fabs(tviInterp(buf_[i].s, x) - thr_), clip); c++; }
        if (c && m / c > best) { best = m / c; ph_ = ph; }
    }
    nbits_ = std::max(1, (int)((n - 1 - ph_) / T_) + 1);
    trained_ = true;
    for (auto& p : buf_) emit(p);
    buf_.clear();
}

void TviGridSlicer::emit(const Pending& p)
{
    TviWord w;
    w.frame = p.frame;
    w.lineNo = p.lineNo;
    w.field = p.field;
    w.bytes.assign((nbits_ + 7) / 8, 0);
    double marg = 0;
    for (int k = 0; k < nbits_; k++) {
        float v = tviInterp(p.s, ph_ + k * T_);
        if (v > thr_) w.bytes[k / 8] |= 1 << (k % 8);
        marg += std::fabs(v - thr_);
    }
    w.quality = (float)(marg / std::max(1, nbits_));
    out_.push_back(std::move(w));
}

std::vector<TviWord> TviGridSlicer::take()
{
    std::vector<TviWord> o;
    o.swap(out_);
    return o;
}

std::vector<TviWord> TviGridSlicer::finish()
{
    if (!trained_ && !buf_.empty()) train();
    return take();
}

// one codeword, its 7 bits as sent: the syndrome (0 = a codeword) and the nibble after the correction
static int tviH74(const uint8_t* b, int& nib)
{
    int p[8] = {0}, syn = 0;
    for (int t = 0; t < 7; t++) p[7 - t] = b[t];
    for (int i = 1; i < 8; i++)
        if (p[i]) syn ^= i;
    if (syn) p[syn] ^= 1;
    nib = ~(p[3] << 3 | p[5] << 2 | p[6] << 1 | p[7]) & 15;   // levels inverted
    return syn;
}

// the 16 codewords (bit t of the code = bit t as sent) and their nibbles
static const std::vector<std::pair<int, int>>& tviCodewords()
{
    static const std::vector<std::pair<int, int>> cw = [] {
        std::vector<std::pair<int, int>> v;
        for (int c = 0; c < 128; c++) {
            uint8_t b[7];
            for (int t = 0; t < 7; t++) b[t] = (c >> t) & 1;
            int nib;
            if (!tviH74(b, nib)) v.push_back({c, nib});
        }
        return v;
    }();
    return cw;
}

static inline int tviGridBit(const TviWord& w, int k) { return k >= 0 && k / 8 < (int)w.bytes.size() ? (w.bytes[k / 8] >> (k % 8)) & 1 : 0; }

// a run of Hamming codewords on a line: lead = bits of the cut first codeword that are seen, g0 = the first
// whole codeword, words = whole codewords
struct TviSeg { int lead = 0, g0 = 0, words = 0; };

// the codewords of a line: places where (nearly) every frame holds a codeword and the words change from frame
// to frame (a constant level or the 1010 idle carrier are codewords too)
static std::vector<TviSeg> tviSegments(const std::vector<const TviWord*>& ws, int nbits)
{
    std::vector<TviSeg> out;
    const int N = (int)ws.size();
    if (N < 20 || nbits < 28) return out;
    std::vector<int> valid(nbits, 0), ones(nbits, 0);
    std::vector<std::set<int>> seen(nbits);
    std::vector<uint8_t> b(nbits + 8, 0);
    for (auto* w : ws) {
        for (int k = 0; k < nbits; k++) { b[k] = (uint8_t)tviGridBit(*w, k); ones[k] += b[k]; }
        for (int g = 0; g + 7 <= nbits; g++) {
            int nib, c = 0;
            for (int t = 0; t < 7; t++) c |= b[g + t] << t;
            if (seen[g].size() < 8) seen[g].insert(c);
            valid[g] += !tviH74(&b[g], nib);
        }
    }
    auto good = [&](int g) { return g >= 0 && g + 7 <= nbits && valid[g] * 100 >= N * 85 && seen[g].size() >= 4; };
    std::vector<TviSeg> chains;
    for (int g = 0; g + 7 <= nbits; g++) {
        if (!good(g) || good(g - 7)) continue;
        TviSeg s;
        s.g0 = g;
        while (good(g + 7 * s.words)) s.words++;
        if (s.words >= 4) chains.push_back(s);
    }
    std::sort(chains.begin(), chains.end(), [](const TviSeg& a, const TviSeg& c) { return a.words > c.words; });
    for (auto& c : chains) {
        bool clash = false;
        for (auto& o : out) clash |= c.g0 < o.g0 + 7 * o.words && o.g0 < c.g0 + 7 * c.words;
        if (!clash) out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const TviSeg& a, const TviSeg& c) { return a.g0 < c.g0; });
    // the cut first codeword: the bits before g0 that change from frame to frame (the blanking before them does not)
    auto varying = [&](int k) { return ones[k] * 50 > N && ones[k] * 50 < N * 49; };
    int floor = 0;
    for (auto& s : out) {
        while (s.lead < 6 && s.g0 - s.lead - 1 >= floor && varying(s.g0 - s.lead - 1)) s.lead++;
        if (s.lead >= 4) {   // 4 or more bits are checked by the code: they must be the tail of a codeword
            int fit = 0;
            for (auto* w : ws) {
                int v = 0;
                for (int i = 0; i < s.lead; i++) v |= tviGridBit(*w, s.g0 - s.lead + i) << i;
                for (auto& cw : tviCodewords())
                    if ((cw.first >> (7 - s.lead)) == v) { fit++; break; }
            }
            if (fit * 10 < N * 9) s.lead = 0;
        }
        floor = s.g0 + 7 * s.words;
    }
    return out;
}

std::vector<TviStream> tviJoin(const std::vector<std::vector<TviWord>>& lines, int nbits)
{
    struct L { int field, lineNo; const std::vector<TviWord>* w; int words; };
    std::vector<L> ok;
    for (auto& v : lines) {
        if (v.size() < 20) continue;
        std::vector<const TviWord*> p;
        for (auto& w : v) p.push_back(&w);
        int words = 0;
        for (auto& s : tviSegments(p, nbits)) words += s.words;
        if (words >= 4) ok.push_back({v[0].field, v[0].lineNo, &v, words});
    }
    std::sort(ok.begin(), ok.end(), [](const L& a, const L& b) { return a.field != b.field ? a.field < b.field : a.lineNo < b.lineNo; });
    std::vector<TviStream> out;
    for (size_t i = 0; i < ok.size();) {
        size_t j = i + 1;
        int words = ok[i].words;
        while (j < ok.size() && ok[j].field == ok[i].field && ok[j].lineNo == ok[j - 1].lineNo + 1) words += ok[j++].words;
        // two lines or more next to each other (one line alone stays with the per-line kinds of tviBuild)
        if (j - i >= 2 && words >= 12) {
            TviStream s;
            s.rate = 0;
            s.joined = 1;
            s.nbits = nbits;
            for (size_t k = i; k < j; k++) s.words.insert(s.words.end(), ok[k].w->begin(), ok[k].w->end());
            std::stable_sort(s.words.begin(), s.words.end(), [](const TviWord& x, const TviWord& y) { return x.frame != y.frame ? x.frame < y.frame : x.lineNo < y.lineNo; });
            out.push_back(std::move(s));
        }
        i = j;
    }
    return out;
}

static bool tviVowel(int x)
{
    static const char* lat = "AEIOUYaeiouy";
    if (x <= 0) return false;
    if (x < 0x80) return std::strchr(lat, x) != nullptr;
    static const uint8_t cyr[] = {0x80, 0x85, 0x88, 0x8E, 0x93, 0x9B, 0x9D, 0x9E, 0x9F, 0xA0, 0xA5, 0xA8, 0xAE, 0xE3, 0xEB, 0xED, 0xEE, 0xEF,   // CP866
                                  0xC1, 0xC5, 0xC9, 0xCF, 0xD5, 0xD9, 0xDC, 0xC0, 0xD1, 0xE1, 0xE5, 0xE9, 0xF5, 0xF9, 0xFC, 0xE0, 0xF1};   // KOI8-R
    for (uint8_t c : cyr)
        if (c == x) return true;
    return false;
}
static bool tviLetter(int x) { return (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= 0x80 && x < 0xB0) || (x >= 0xC0 && x < 0xFF); }

// UTF-8 -> code points
static std::u32string tviU32(const char* s)
{
    std::u32string o;
    for (const unsigned char* p = (const unsigned char*)s; *p;) {
        char32_t c = *p++;
        if (c >= 0xE0) { c = (c & 15) << 12; c |= (*p++ & 63) << 6; c |= *p++ & 63; }
        else if (c >= 0xC0) { c = (c & 31) << 6; c |= *p++ & 63; }
        o += c;
    }
    return o;
}
static char32_t tviLower(char32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c == 0x401) return 0x451;
    return c;
}
// a byte as a letter (lower case) in CP866 (cs 0) or KOI8-R (cs 1); ' ' for a space, 0 for anything else
static char32_t tviChar(int x, int cs)
{
    static std::u32string dos = tviU32("АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмноп"), dos2 = tviU32("рстуфхцчшщъыьэюяЁё"),
                          koi = tviU32("юабцдефгхийклмнопярстужвьызшэщчъЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЫЗШЭЩЧЪ");
    if (x == ' ') return ' ';
    if ((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z')) return tviLower((char32_t)x);
    if (cs == 0) {
        if (x >= 0x80 && x < 0xB0) return tviLower(dos[x - 0x80]);
        if (x >= 0xE0 && x < 0xF2) return tviLower(dos2[x - 0xE0]);
    } else if (x >= 0xC0 && x < 0xFF)
        return tviLower(koi[x - 0xC0]);
    return 0;
}
// a little of the language of the texts (Russian and English words of TV and datacast): where a cut codeword reads
// two ways (Г / Д, U / R, O / H, space / '), the reading that makes common letter triples wins
static const std::set<std::u32string>& tviTrigrams()
{
    static const std::set<std::u32string> t = [] {
        std::u32string c = tviU32(
            " программа программы передача передачи время московское москва канал каналы европа новости погода "
            "телевидение информ информация реклама курс доллара рубль сегодня завтра вечер утро день ночь первый "
            "второй третий россия российский телетекст страница сообщение текст проверка тест связь данные файл "
            "приём абонент служба выпуск часы минуты секунды температура ветер давление "
            " the quick brown fox jumps over a lazy dog time news weather channel program test information page "
            "text data file europe moscow service ");
        std::set<std::u32string> s;
        for (size_t i = 0; i + 3 <= c.size(); i++) s.insert(c.substr(i, 3));
        return s;
    }();
    return t;
}
static int tviTrigramHits(const std::vector<uint8_t>& by, int prev)
{
    int best = 0;
    for (int cs = 0; cs < 2; cs++) {
        std::u32string u;
        if (prev >= 0) u += tviChar(prev, cs);
        for (uint8_t x : by) u += tviChar(x, cs);
        int h = 0;
        for (size_t i = 0; i + 3 <= u.size(); i++)
            if (u[i] && u[i + 1] && u[i + 2] && tviTrigrams().count(u.substr(i, 3))) h++;
        best = std::max(best, h);
    }
    return best;
}

// how much a piece of the stream looks like text: as 8-bit characters (CP866 / KOI8-R) or as 7-bit characters
// with even parity (the inserter's identification); prev = the byte before it (-1 = none)
static double tviTextScore(const std::vector<uint8_t>& by, int prev)
{
    double plain = 0.4 * tviTrigramHits(by, prev), par = 0;
    for (size_t i = 0; i < by.size(); i++) {
        int x = by[i];
        bool pr = (x >= 0x20 && x < 0x7F) || x == 0x0D || x == 0x0A || tviLetter(x);
        plain += pr;
        if (tviLetter(x) || (x >= '0' && x <= '9')) plain += 0.2;
        if (x == ' ') plain += 0.1;
        // a vowel between two consonants (J?MPS, D?G)
        int a = i ? by[i - 1] : prev, c = i + 1 < by.size() ? by[i + 1] : -1;
        if (a >= 0 && c >= 0 && tviLetter(a) && tviLetter(c) && !tviVowel(a) && !tviVowel(c) && tviVowel(x)) plain += 0.5;
        int y = x & 0x7F;
        if (!(__builtin_popcount(x) & 1) && ((y >= 0x20 && y < 0x7F) || y == 0x0D || y == 0x0A)) par += 1;
    }
    return std::max(plain, par);
}

static std::string tviJoinLines(const TviStream& s)
{
    std::set<std::pair<int, int>> ls;
    for (auto& w : s.words) ls.insert({w.field, w.lineNo});
    std::string t;
    int field = 0;
    for (auto& x : ls) { t += (t.empty() ? "lines " : ", ") + std::to_string(x.second); field = x.first; }
    return t + (field ? " field " + std::to_string(field) : "");
}

// a time hh:mm:ss in a text line ("" = none)
static std::string tviTimeIn(const std::string& l)
{
    for (size_t i = 0; i + 8 <= l.size(); i++) {
        auto d = [&](size_t k) { return l[i + k] >= '0' && l[i + k] <= '9'; };
        if (d(0) && d(1) && l[i + 2] == ':' && d(3) && d(4) && l[i + 5] == ':' && d(6) && d(7)) return l.substr(i, 8);
    }
    return "";
}

static std::string tviTrim(const std::string& s)
{
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static void tviJoinBuild(TviStream& s)
{
    char b[400];
    s.report.clear();
    s.serial.clear();
    s.texts.clear();
    s.pages.clear();
    std::map<std::pair<int, int>, std::vector<const TviWord*>> byLine;
    for (auto& w : s.words) byLine[{w.field, w.lineNo}].push_back(&w);
    struct LL { int field, lineNo; std::vector<TviSeg> segs; std::map<int64_t, const TviWord*> at; };
    std::vector<LL> ls;
    for (auto& kv : byLine) {
        LL l;
        l.field = kv.first.first;
        l.lineNo = kv.first.second;
        l.segs = tviSegments(kv.second, s.nbits);
        if (l.segs.empty()) continue;
        for (auto* w : kv.second) l.at[w->frame] = w;
        ls.push_back(std::move(l));
    }
    snprintf(b, sizeof b, "ТВ-информ fТ1: one Hamming(7,4) stream over %s, %zu lines read", tviJoinLines(s).c_str(), s.words.size());
    s.report.push_back(b);
    if (ls.empty()) { s.report.push_back("  no codewords found"); return; }
    // the layout of a frame
    int K = 0;
    std::string lay;
    for (auto& l : ls) {
        std::string p;
        for (auto& g : l.segs) {
            int n = g.words + (g.lead > 0);
            K += n;
            p += (p.empty() ? "" : " + ") + std::to_string(n);
        }
        lay += (lay.empty() ? "" : ", ") + fmt_n("line %d: %s", l.lineNo, p.c_str());
    }
    snprintf(b, sizeof b, "  a frame: %s = %d nibbles = %d bytes (the bytes run on over the line ends)", lay.c_str(), K, K / 2);
    s.report.push_back(b);
    s.report.push_back("  codewords sent d0 d1 d2 p d3 p p, levels inverted, low nibble first; the first codeword of every part starts in the "
                       "blanking - rebuilt from the bits that are seen (where two codewords fit them, the one that makes text is taken)");
    // the frames that have every line
    std::vector<int64_t> frames;
    for (auto& kv : ls[0].at) {
        bool all = true;
        for (size_t i = 1; i < ls.size() && all; i++) all = ls[i].at.count(kv.first) > 0;
        if (all) frames.push_back(kv.first);
    }
    long fixedSum = 0, chosen = 0;
    int prev = -1;
    int64_t prevFrame = -10;
    const auto& cws = tviCodewords();
    for (int64_t f : frames) {
        std::vector<std::vector<int>> cand;
        int fixed = 0;
        for (auto& l : ls) {
            const TviWord& w = *l.at[f];
            for (auto& g : l.segs) {
                if (g.lead > 0) {
                    int v = 0;
                    for (int i = 0; i < g.lead; i++) v |= tviGridBit(w, g.g0 - g.lead + i) << i;
                    std::vector<int> c;
                    for (auto& cw : cws)
                        if ((cw.first >> (7 - g.lead)) == v) c.push_back(cw.second);
                    if (c.empty()) c.push_back(0);
                    cand.push_back(c);
                }
                for (int k = 0; k < g.words; k++) {
                    uint8_t bb[7];
                    for (int t = 0; t < 7; t++) bb[t] = (uint8_t)tviGridBit(w, g.g0 + 7 * k + t);
                    int nib;
                    fixed += tviH74(bb, nib) != 0;
                    cand.push_back({nib});
                }
            }
        }
        // the readings of the cut codewords: the one that makes text (all of them, up to 256)
        std::vector<int> amb;
        long combos = 1;
        for (size_t i = 0; i < cand.size(); i++)
            if (cand[i].size() > 1 && combos * (long)cand[i].size() <= 256) { amb.push_back((int)i); combos *= (long)cand[i].size(); }
        if (prevFrame != f - 1) prev = -1;
        std::vector<uint8_t> best;
        double bestScore = -1;
        std::vector<int> nib(cand.size());
        for (long c = 0; c < combos; c++) {
            for (size_t i = 0; i < cand.size(); i++) nib[i] = cand[i][0];
            long r = c;
            for (int i : amb) { nib[i] = cand[i][r % cand[i].size()]; r /= (long)cand[i].size(); }
            std::vector<uint8_t> by;
            for (size_t i = 0; i + 1 < nib.size(); i += 2) by.push_back((uint8_t)(nib[i] | nib[i + 1] << 4));
            double sc = tviTextScore(by, prev);
            if (sc > bestScore) { bestScore = sc; best = by; }
        }
        chosen += combos > 1;
        fixedSum += fixed;
        TviChunk ch;
        ch.kind = 3;
        ch.frame = f;
        ch.fixed = (uint8_t)std::min(fixed, 254);
        ch.chars = best;
        if (!best.empty()) prev = best.back();
        prevFrame = f;
        s.serial.push_back(std::move(ch));
    }
    std::set<std::vector<uint8_t>> distinct;
    for (auto& c : s.serial) distinct.insert(c.chars);
    snprintf(b, sizeof b, "  %zu frames read whole, %ld bits corrected; %ld frames had a cut codeword with two readings (taken by the text); "
             "%zu different frame records", s.serial.size(), fixedSum, chosen, distinct.size());
    s.report.push_back(b);
    // the text: 8-bit characters between control / binary bytes, a line ends at CR / LF
    std::vector<const TviChunk*> ch;
    for (auto& c : s.serial) ch.push_back(&c);
    const int cs = tviCharset(ch, 3);
    auto isText = [&](int x) {
        if (x >= 0x20 && x < 0x7F) return true;
        if (cs == kCsCp866) return (x >= 0x80 && x < 0xB0) || (x >= 0xE0 && x < 0xF2);
        return (x >= 0xC0 && x < 0xFF) || x == 0xA3 || x == 0xB3;
    };
    std::vector<std::string> lines;
    std::string cur;
    bool cyr = false;
    int64_t last = -10;
    auto push = [&]() {
        int vis = 0;   // characters, not UTF-8 bytes
        for (char c : cur) vis += c != ' ' && ((unsigned char)c & 0xC0) != 0x80;
        if (vis >= 4) lines.push_back(tviTrim(cur));
        cur.clear();
    };
    for (auto& c : s.serial) {
        if (c.frame != last + 1) cur.clear();   // a frame missing: the text does not run on
        last = c.frame;
        for (uint8_t x : c.chars) {
            if (x == 0x0D || x == 0x0A) push();
            else if (isText(x)) tviPut(cur, x, cs, cyr);
            else cur.clear();                    // binary bytes before the text
        }
    }
    // a carousel sends the same lines again: each once, with how often; a line whose digits change (a clock) once
    struct Agg { std::string first, last; int n = 0; std::set<std::string> vals; };
    std::map<std::string, Agg> byKey;
    std::vector<std::string> order;
    auto mask = [](std::string k) { for (auto& c : k) if (c >= '0' && c <= '9') c = '#'; return k; };
    std::map<std::string, int> keyCount;
    for (auto& l : lines) keyCount[mask(l)]++;
    // a line cut short (a frame missing before it) is the end of a more frequent line: left out; a line with a few
    // binary bytes before it (the bytes that change every second come just before the date) is that line
    std::map<std::string, std::string> into;   // key -> key it goes into ("" = left out)
    for (auto& kv : keyCount) {
        if (keyCount.size() > 2000) break;     // a long document, not a carousel
        const std::string& k = kv.first;
        int bestN = kv.second;
        for (auto& o : keyCount) {
            if (o.first.size() == k.size() || o.second <= kv.second) continue;
            if (o.first.size() > k.size() && o.first.compare(o.first.size() - k.size(), k.size(), k) == 0) { into[k] = ""; break; }
            // up to 6 bytes before it: the 4 bytes that change every second and the 1..2 before them
            int pre = 0;   // characters before it (UTF-8)
            if (k.size() > o.first.size())
                for (size_t i = 0; i < k.size() - o.first.size(); i++) pre += ((unsigned char)k[i] & 0xC0) != 0x80;
            if (k.size() > o.first.size() && pre <= 6 && k.compare(k.size() - o.first.size(), o.first.size(), o.first) == 0 &&
                o.second > bestN) {
                into[k] = o.first;   // the most frequent line it ends with
                bestN = o.second;
            }
        }
    }
    for (auto& l0 : lines) {
        std::string l = l0, key = mask(l);
        for (int hop = 0; hop < 8; hop++) {   // through the chain of lines it goes into
            auto it = into.find(key);
            if (it == into.end()) break;
            if (it->second.empty()) { key.clear(); break; }
            l = l.substr(l.size() - it->second.size());
            key = it->second;
        }
        if (key.empty()) continue;
        Agg& a = byKey[key];
        if (!a.n++) { order.push_back(key); a.first = l; }
        a.last = l;
        a.vals.insert(l);
    }
    int most = 0;
    for (auto& kv : byKey) most = std::max(most, kv.second.n);
    for (auto& k : order) {
        Agg& a = byKey[k];
        // a carousel: a line seen once is a damaged copy of another (a bit error, a wrong reading of a cut codeword)
        if (a.n < 2 && (order.size() > 20 || most >= 3)) continue;
        std::string t = a.last;
        if (a.vals.size() >= 3) {
            std::string t0 = tviTimeIn(a.first), t1 = tviTimeIn(a.last);
            if (!t0.empty() && !t1.empty()) t += fmt_n("   (the clock runs: %s .. %s, %zu values)", t0.c_str(), t1.c_str(), a.vals.size());
            else t += fmt_n("   (%zu different)", a.vals.size());
        } else if (a.n > 1)
            t += fmt_n("   (x%d)", a.n);
        s.texts.push_back(t);
    }
    // 7-bit characters with even parity (bit 7 set in some of them): the inserter's identification
    std::map<std::string, int> pieces;
    std::vector<std::string> porder;
    for (auto& c : s.serial) {
        std::string p;
        bool high = false;
        for (size_t i = 0; i <= c.chars.size(); i++) {
            int x = i < c.chars.size() ? c.chars[i] : 0x100, y = x & 0x7F;
            bool ok = x < 0x100 && !(__builtin_popcount(x) & 1) && y >= 0x20 && y < 0x7F;
            if (ok) { p += (char)y; high |= x >= 0x80; continue; }
            if (high && p.size() >= 3 && !pieces[p]++) porder.push_back(p);
            p.clear();
            high = false;
        }
    }
    std::string pt;
    for (auto& p : porder) {
        std::set<char> kinds(p.begin(), p.end());   // not a filler of one character (the inverted '-' test line)
        if (pieces[p] >= 5 && p.size() >= 4 && kinds.size() >= 3) pt += (pt.empty() ? "" : ", ") + fmt_n("\"%s\" x%d", p.c_str(), pieces[p]);
    }
    snprintf(b, sizeof b, "  text (%s, %zu lines with characters, %zu different):", tviCharsetName(cs), lines.size(), s.texts.size());
    s.report.push_back(b);
    for (size_t i = 0; i < s.texts.size() && i < 40; i++) s.report.push_back("    " + s.texts[i]);
    if (!pt.empty()) {
        s.report.push_back("  7-bit characters with even parity (in frames of their own): " + pt);
        s.texts.push_back("7-bit with parity: " + pt);
    }
}

static TviSummary tviJoinSummary(const TviStream& s)
{
    TviSummary r;
    r.lines = (int)s.words.size();
    r.dataLines = (int)s.serial.size();
    for (auto& c : s.serial) r.fixed += c.fixed;
    r.chunks = (int)s.serial.size();
    std::vector<const TviChunk*> ch;
    for (auto& c : s.serial) ch.push_back(&c);
    r.charset = tviCharsetName(tviCharset(ch, 3));
    r.text = s.texts;
    r.kind = s.texts.empty() ? TviSummary::Idle : TviSummary::Data;
    r.what = s.texts.empty() ? "Hamming(7,4) stream over " + tviJoinLines(s) + ", no text"
                             : fmt_n("text over %s (one Hamming(7,4) stream), %zu lines", tviJoinLines(s).c_str(), s.texts.size());
    return r;
}
