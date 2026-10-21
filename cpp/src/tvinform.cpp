#include "tvinform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

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
    for (auto& c : s.serial) {
        double sec = c.frame / (fps > 0 ? fps : 25);
        snprintf(b, sizeof b, "%02d:%02d:%05.2f  %-8lld %s %04x  ", (int)(sec / 3600), (int)(sec / 60) % 60, std::fmod(sec, 60.0),
                 (long long)c.frame, c.kind ? "hamming" : "async  ", c.header);
        t += b;
        for (uint8_t x : c.chars) { snprintf(b, sizeof b, "%02x ", x); t += b; }
        t += " " + tviShow(c.chars, c.kind) + "\n";
    }
    return t;
}

void tviBuild(TviStream& s)
{
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
