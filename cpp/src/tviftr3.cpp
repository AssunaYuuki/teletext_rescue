#include "tviftr3.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <array>
#include <cstdlib>

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

// the header after the run-in, bit 0 first: FB AC
static const int kHdr[16] = {1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1};
constexpr int kHdrBit = 27;              // line bit of the header (bits 11..26 = run-in)
constexpr int kDataBit = 43;             // first data bit
constexpr int kLineBits = 371;           // bits 0..370 (360 from the run-in at bit 11)
constexpr int kObs = kLineBits - kHdrBit; // bits the MLSE sees: the header and the data

static float interp(const std::vector<float>& s, double x, float outside)
{
    if (x < 0 || x > (double)s.size() - 1) return outside;
    int i = (int)x;
    double f = x - i;
    return i + 1 < (int)s.size() ? (float)(s[i] * (1 - f) + s[i + 1] * f) : s[i];
}

// finds the bit grid of a line by its header: returns the sample position of bit kHdrBit and the levels
// of 0 and 1 (from the header bits); false if the line has no fТ3 header
static bool tvi3Grid(const std::vector<float>& s, float T, double& pos, float& v0, float& v1)
{
    const int n = (int)s.size();
    // the run-in starts the line: its first rise past mid level; the header is 16 bits later. Mid level from the
    // line's lowest sample, and from the blanking level the line starts with (a line in the field sync carries an
    // equalizing pulse mid-line, far below the blanking - tvinfoorm.vbi lines 624/625/4/5)
    const float mn = *std::min_element(s.begin(), s.end()), mx = *std::max_element(s.begin(), s.end());
    int e = 0;
    while (e < n / 2 && s[e] <= mn + 0.25f * (mx - mn)) e++;
    if (e >= n / 2) return false;
    std::vector<float> head(s.begin(), s.begin() + std::min(n, 8));
    std::nth_element(head.begin(), head.begin() + head.size() / 2, head.end());
    const float base = head[head.size() / 2];
    int e2 = 0;
    while (e2 < n / 2 && s[e2] <= base + 0.25f * (mx - base)) e2++;
    if (e2 >= n / 2) e2 = e;
    double best = -2, bx = 0;
    const double lo = std::min(e, e2) + 12.0 * T, hi = std::min(std::max(e, e2) + 22.0 * T, (double)n - 17.0 * T);
    if (hi <= lo) return false;
    for (int pass = 0; pass < 2; pass++) {
        double a = pass ? bx - T / 8 : lo, b = pass ? bx + T / 8 : hi, step = pass ? T / 64 : T / 8;
        for (double x = a; x <= b; x += step) {
            float v[16], m = 0;
            for (int i = 0; i < 16; i++) m += v[i] = interp(s, x + i * T, s[0]);
            m /= 16;
            double c = 0, e = 0;
            for (int i = 0; i < 16; i++) { c += (v[i] - m) * (kHdr[i] ? 1 : -1); e += (v[i] - m) * (v[i] - m); }
            c = e > 0 ? c / std::sqrt(e * 16) : -1;
            if (c > best) { best = c; bx = x; }
        }
    }
    if (best < 0.6) return false;
    float a1 = 0, a0 = 0;
    int n1 = 0, n0 = 0;
    for (int i = 0; i < 16; i++) {
        float v = interp(s, bx + i * T, s[0]);
        if (kHdr[i]) a1 += v, n1++; else a0 += v, n0++;
    }
    v1 = a1 / n1; v0 = a0 / n0;
    if (v1 - v0 < 0.06f * (*std::max_element(s.begin(), s.end()) + 1)) return false;
    // the sliced header itself must read FB AC (one bit may be off)
    int bad = 0;
    for (int i = 0; i < 16; i++) bad += (interp(s, bx + i * T, s[0]) > (v0 + v1) / 2) != (kHdr[i] == 1);
    if (bad > 3) return false;
    pos = bx;
    return true;
}

int tvi3TestLine(const uint8_t* probe, int probeN, size_t frameBytes, size_t lineOff, int fmt, int samples, float fs)
{
    const float T = fs / kTviRateHz[2];
    if (T < 3.0f || samples < (int)(T * 300)) return 0;   // the clock needs a 27 MHz class capture
    int hits = 0;
    std::vector<float> s(samples);
    for (int f = 0; f < probeN; f++) {
        const uint8_t* p = probe + f * frameBytes + lineOff;
        for (int i = 0; i < samples; i++) s[i] = sampleAt(p, fmt, i);
        double pos; float v0, v1;
        hits += tvi3Grid(s, T, pos, v0, v1);
    }
    return hits;
}

Tvi3Slicer::Tvi3Slicer(float fs, bool live) : fs_(fs), T_(fs / kTviRateHz[2])
{
    if (live) { needVaried_ = 150; needLines_ = 750; retrainAt_ = 1500; }
}

// MLSE: the observation of bit k (its centre and the half-bit point after it) depends on bits k-2..k+2
static int ctxOf(const std::vector<uint8_t>& b, int k)
{
    auto at = [&](int i) { return i >= 0 && i < (int)b.size() ? b[i] : 0; };
    return at(k - 2) << 4 | at(k - 1) << 3 | at(k) << 2 | at(k + 1) << 1 | at(k + 2);
}

static std::vector<uint8_t> viterbi(const std::vector<float>& y, const Tvi3Slicer::Model& m)
{
    const int L = (int)y.size() / 2;
    std::vector<float> cost(16, 0), nc(16);
    std::vector<uint8_t> back((size_t)L * 16);
    const float iv0 = 1 / m.var[0], iv1 = 1 / m.var[1];
    float e[32];
    for (int k = 0; k < L; k++) {
        const float a = y[2 * k], b = y[2 * k + 1];
        for (int c = 0; c < 32; c++) {
            float d0 = a - m.mu[c][0], d1 = b - m.mu[c][1];
            e[c] = d0 * d0 * iv0 + d1 * d1 * iv1;
        }
        // state after step k = b(k-1) b(k) b(k+1) b(k+2); context of observation k = b(k-2) << 4 | state
        for (int s = 0; s < 16; s++) {
            float c0 = cost[s >> 1] + e[s], c1 = cost[8 | (s >> 1)] + e[16 | s];
            bool h = c1 < c0;
            nc[s] = h ? c1 : c0;
            back[(size_t)k * 16 + s] = h;
        }
        std::swap(cost, nc);
    }
    int st = (int)(std::min_element(cost.begin(), cost.end()) - cost.begin());
    std::vector<uint8_t> bits(L + 2, 0);
    for (int k = L - 1; k >= 0; k--) {
        bits[k + 2] = st & 1;
        int h = back[(size_t)k * 16 + st];
        st = (h << 3) | (st >> 1);
    }
    bits[1] = st & 1; bits[0] = (st >> 1) & 1;
    bits.resize(L);
    return bits;
}

static void fitModel(const std::vector<const std::vector<float>*>& ys, const std::vector<std::vector<uint8_t>>& bs, Tvi3Slicer::Model& m)
{
    double sum[32][2] = {}, n[32] = {};
    for (size_t i = 0; i < ys.size(); i++) {
        const auto& y = *ys[i];
        const auto& b = bs[i];
        for (int k = 2; k + 2 < (int)b.size() && 2 * k + 1 < (int)y.size(); k++) {
            int c = ctxOf(b, k);
            sum[c][0] += y[2 * k]; sum[c][1] += y[2 * k + 1]; n[c]++;
        }
    }
    for (int c = 0; c < 32; c++)
        for (int j = 0; j < 2; j++) m.mu[c][j] = n[c] > 5 ? (float)(sum[c][j] / n[c]) : (float)((c >> 2) & 1);
    double r[2] = {}, cnt = 0;
    for (size_t i = 0; i < ys.size(); i++) {
        const auto& y = *ys[i];
        const auto& b = bs[i];
        for (int k = 2; k + 2 < (int)b.size() && 2 * k + 1 < (int)y.size(); k++) {
            int c = ctxOf(b, k);
            for (int j = 0; j < 2; j++) { double d = y[2 * k + j] - m.mu[c][j]; r[j] += d * d; }
            cnt++;
        }
    }
    for (int j = 0; j < 2; j++) m.var[j] = cnt > 0 ? std::max(1e-4f, (float)(r[j] / cnt)) : 0.02f;
}

// the observations of a line on the bit grid of its header (levels 0 .. 1); false = no fТ3 header
static bool sampleLine(const std::vector<float>& s, float T, std::vector<float>& y)
{
    double pos; float v0, v1;
    if (!tvi3Grid(s, T, pos, v0, v1)) return false;
    y.resize(2 * kObs);
    const float sc = 1 / (v1 - v0);
    for (int k = 0; k < kObs; k++) {
        double c = pos + k * T;
        y[2 * k] = (interp(s, c, v0) - v0) * sc;
        y[2 * k + 1] = (interp(s, c + T / 2, v0) - v0) * sc;
    }
    return true;
}

static bool pulseLine(const std::vector<float>& raw, float T);

static int flipsOf(const std::vector<float>& y)
{
    int f = 0;
    for (int k = 1; k < kObs; k++) f += (y[2 * k] > 0.5f) != (y[2 * k - 2] > 0.5f);
    return f;
}

void Tvi3Slicer::train()
{
    // the clock: ГОСТ allows 6.9375 MHz +-25 kHz; the bit period that opens the eye best over the
    // second half of the lines with data (where a wrong period has drifted most)
    std::vector<const Pending*> var;
    for (auto& p : buf_) if (p.varied) var.push_back(&p);
    if (var.size() > 600) var.resize(600);
    if (!var.empty()) {
        const float T0 = T_;
        double bestE = -1;
        std::vector<float> y;
        for (int k = -12; k <= 12; k++) {
            const float T = T0 * (1 + k * 0.00015f);
            double e = 0; int n = 0;
            for (auto* p : var) {
                if (!sampleLine(p->raw, T, y)) continue;
                for (int b = kObs / 2; b < kObs - 4; b++) e += std::fabs(y[2 * b] - 0.5f);
                n++;
            }
            if (n && e / n > bestE) { bestE = e / n; T_ = T; }
        }
    }
    std::vector<std::vector<float>> yy;
    std::vector<std::vector<uint8_t>> bs;
    std::vector<float> y;
    for (auto& p : buf_) {
        if (!p.varied || !sampleLine(p.raw, T_, y)) continue;   // idle lines (all 0 or all 1) teach one context only
        std::vector<uint8_t> b(kObs);
        for (int k = 0; k < kObs; k++) b[k] = y[2 * k] > 0.5f;
        yy.push_back(y);
        bs.push_back(std::move(b));
        if (yy.size() >= 3000) break;
    }
    if (yy.empty())
        for (auto& p : buf_) {
            if (!sampleLine(p.raw, T_, y)) continue;
                std::vector<uint8_t> b(kObs);
            for (int k = 0; k < kObs; k++) b[k] = y[2 * k] > 0.5f;
            yy.push_back(y);
            bs.push_back(std::move(b));
        }
    std::vector<const std::vector<float>*> ys;
    for (auto& v : yy) ys.push_back(&v);
    for (int it = 0; it < 3; it++) {
        fitModel(ys, bs, m_);
        for (size_t i = 0; i < ys.size(); i++) bs[i] = viterbi(*ys[i], m_);
    }
    fitModel(ys, bs, m_);
    trained_ = true;
}

// a line of the field sync (tvinfoorm.vbi: 624, 625, 4, 5, 311, 312, 317, 318) has an equalizing pulse
// mid-line, down at sync level, then blanking: the inserter waits there - the line's data are bytes 0..12
// and 24..40, 30 bytes with no gap in the stream. Raw samples: the pulse goes ~55 below the blanking the line
// starts with, data lines stay within ~6 of it; the blanking after it is flat.
static bool pulseLine(const std::vector<float>& raw, float T)
{
    double pos; float v0, v1;
    if (raw.size() < 16 || !tvi3Grid(raw, T, pos, v0, v1)) return false;
    std::vector<float> head(raw.begin(), raw.begin() + 8);
    std::nth_element(head.begin(), head.begin() + 4, head.end());
    const float blank = head[4];
    auto at = [&](int bit) { return pos + (kDataBit - kHdrBit + bit) * (double)T; };
    float low = blank;
    for (int i = (int)at(100); i < (int)at(140) && i < (int)raw.size(); i++) low = std::min(low, raw[i]);
    double m = 0, v = 0;
    int nb = 0;
    for (int i = (int)at(17 * 8); i < (int)at(23 * 8) && i < (int)raw.size(); i++) { m += raw[i]; v += (double)raw[i] * raw[i]; nb++; }
    const bool flat = nb > 100 && v / nb - (m / nb) * (m / nb) < 64;
    return blank - low > 30 && blank - low > 0.25f * (v1 - blank) && flat;
}

void Tvi3Slicer::emit(int64_t frame, int lineNo, int field, const std::vector<float>& y, const std::vector<float>& raw)
{
    const bool pulse = pulseLine(raw, T_);
    std::vector<uint8_t> bits = viterbi(y, m_);
    TviWord w;
    w.frame = frame; w.lineNo = lineNo; w.field = field;
    w.bytes.assign(kTvi3Bytes, 0);
    for (int i = 0; i < kTvi3Bytes * 8; i++) {
        int k = kDataBit - kHdrBit + i;
        if (k < (int)bits.size() && bits[k]) w.bytes[i / 8] |= 1 << (i % 8);
    }
    if (pulse) w.bytes.erase(w.bytes.begin() + kTvi3GapFrom, w.bytes.begin() + kTvi3GapTo);
    w.quality = 1;
    out_.push_back(std::move(w));
}

void Tvi3Slicer::flush()
{
    train();
    std::vector<float> y;
    for (auto& q : buf_)
        if (sampleLine(q.raw, T_, y)) emit(q.frame, q.lineNo, q.field, y, q.raw);
    buf_.clear();
}

void Tvi3Slicer::add(const uint8_t* line, int fmt, int samples, int64_t frame, int lineNo, int field)
{
    std::vector<float> s(samples);
    for (int i = 0; i < samples; i++) s[i] = sampleAt(line, fmt, i);
    std::vector<float> y;
    if (!sampleLine(s, T_, y)) return;
    if (trained_) {
        emit(frame, lineNo, field, y, s);
        // live: the lines with data kept for the second learning (on more lines, the clock and channel closer)
        if (retrainAt_ && flipsOf(y) >= 60) {
            buf_.push_back({frame, lineNo, field, std::move(s), true});
            if (++varied_ >= retrainAt_) { train(); buf_.clear(); retrainAt_ = 0; }
        }
        return;
    }
    Pending p{frame, lineNo, field, std::move(s), flipsOf(y) >= 60};
    varied_ += p.varied;
    buf_.push_back(std::move(p));
    if (varied_ >= needVaried_ || (int)buf_.size() >= needLines_) {
        flush();
        varied_ = 0;
    }
}

std::vector<TviWord> Tvi3Slicer::finish()
{
    if (!trained_ && !buf_.empty()) flush();
    return std::move(out_);
}

// ---------------------------------------------------------------- the stream

static uint32_t crc32(const uint8_t* p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct Tvi3Rec {
    int64_t frame = 0;
    int seq = 0, type = 0, len = 0;
    uint32_t id = 0;
    bool bodyOk = false;
    std::vector<uint8_t> body;    // as far as the stream went (len bytes when complete)
};

static std::string tvi3Hms(double sec)
{
    int t = (int)sec;
    return fmt("%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
}

void tvi3Build(TviStream& s, const std::string& dir, double fps)
{
    s.report.clear(); s.files.clear(); s.fileInfo.clear(); s.beacons.clear(); s.texts.clear();
    if (s.words.empty()) return;
    if (fps <= 0) fps = 25;
    // slots in broadcast order: frame, then its fТ3 lines by field and line (RTR: one line a field; tvinfoorm.vbi:
    // 4-5 lines a field, so the line goes into the slot too)
    int64_t f0 = s.words[0].frame, f1 = f0;
    std::map<std::pair<int, int>, int> lineOrd;   // (field, line) -> place in the frame
    for (auto& w : s.words) {
        f0 = std::min(f0, w.frame), f1 = std::max(f1, w.frame);
        lineOrd[{w.field, w.lineNo}] = 0;
    }
    int perFrame = 0;
    for (auto& kv : lineOrd) kv.second = perFrame++;
    if (const char* dp = getenv("TR_TVI3DUMP"))   // every line's 41 bytes, for study: frame field line hex
        if (FILE* df = fopen(dp, "wb")) {
            for (auto& w : s.words) {
                fprintf(df, "%lld %d %d ", (long long)w.frame, w.field, w.lineNo);
                for (size_t i = 0; i < w.bytes.size() && i < (size_t)kTvi3Bytes; i++) fprintf(df, "%02X", w.bytes[i]);
                fputc('\n', df);
            }
            fclose(df);
        }
    const int64_t N = (f1 - f0 + 1) * perFrame;
    std::vector<uint8_t> slot((size_t)N * kTvi3Bytes, 0);
    std::vector<char> has((size_t)N, 0);
    std::vector<uint8_t> slen((size_t)N, 0);   // bytes of the slot's line: 41, or 30 on a line with the mid-line pulse
    std::vector<int8_t> sfield((size_t)N, 0);
    int64_t lines = 0, idle = 0;
    for (auto& w : s.words) {
        if (w.bytes.size() != (size_t)kTvi3Bytes && w.bytes.size() != (size_t)kTvi3GapBytes) continue;
        const int n = (int)w.bytes.size();
        int64_t k = (w.frame - f0) * perFrame + lineOrd[{w.field, w.lineNo}];
        std::copy(w.bytes.begin(), w.bytes.end(), slot.begin() + k * kTvi3Bytes);
        slen[k] = (uint8_t)n;
        sfield[k] = (int8_t)w.field;
        bool any = false, all1 = true;
        for (int i = 0; i < n - 1; i++) { any |= w.bytes[i] != 0; all1 &= w.bytes[i] == 0xFF; }
        has[k] = any;
        lines++;
        idle += !any || all1;
    }
    auto S = [&](int64_t k) { return &slot[(size_t)k * kTvi3Bytes]; };
    // successor: with several fТ3 lines in a field (tvinfoorm.vbi) the stream runs on from a line to the next
    // line of the field the capture has (then on lines it does not have); with one line a field (RTR) slot k+d
    // repeats the last 2d bytes of slot k (byte 40: low 6 bits) - two bit errors are allowed in the repeat of
    // the next line or the one after
    std::vector<int64_t> succ((size_t)N, -1);
    std::vector<uint8_t> sd((size_t)N, 0);
    std::vector<char> hasPrev((size_t)N, 0);
    for (int64_t k = 0; k + 1 < N; k++)
        if (slen[k] && slen[k + 1] && (k + 1) % perFrame && sfield[k] == sfield[k + 1]) { succ[k] = k + 1; hasPrev[k + 1] = 1; }
    for (int64_t k = 0; k < N; k++) {
        if (!has[k] || succ[k] >= 0 || slen[k] != kTvi3Bytes) continue;
        const uint8_t* a = S(k);
        int bd = 0;
        for (int d = 1; d <= 7 && k + d < N; d++) {
            if (!has[k + d] || slen[k + d] != kTvi3Bytes) continue;
            const uint8_t* b = S(k + d);
            const int q = 2 * d;
            int e = 0;
            for (int i = 0; i < q - 1; i++) e += __builtin_popcount(a[41 - q + i] ^ b[i]);
            e += __builtin_popcount((a[40] ^ b[q - 1]) & 0x3F);
            if (e == 0) { bd = d; break; }
            if (e <= 2 && !bd && d <= 2) bd = d;
        }
        if (bd && !hasPrev[k + bd]) { succ[k] = k + bd; sd[k] = (uint8_t)bd; hasPrev[k + bd] = 1; }
    }
    // records in every chain of slots
    std::vector<Tvi3Rec> recs;
    int64_t hdrOk = 0;
    static const uint8_t mk[5] = {0x7E, 0x80, 0x80, 0x80, 0x80};
    std::vector<std::string> texts;
    std::set<std::string> textSeen;
    std::map<std::string, int> headings;   // "TTAAii CCCC YYGGgg" -> copies
    for (int64_t i = 0; i < N; i++) {
        if (!slen[i] || hasPrev[i]) continue;
        std::vector<uint8_t> st;
        std::vector<int64_t> at;            // slot of each byte
        for (int64_t j = i; j >= 0; j = succ[j]) {
            int n = slen[j] - 2 * sd[j];
            st.insert(st.end(), S(j), S(j) + n);
            at.insert(at.end(), n, j);
        }
        for (size_t p = 0; p + 5 + 18 <= st.size(); p++) {
            if (memcmp(&st[p], mk, 5)) continue;
            const uint8_t* h = &st[p + 5];
            if (crc32(h, 10) != le32(h + 10)) continue;
            hdrOk++;
            Tvi3Rec r;
            r.frame = f0 + at[p] / perFrame;
            r.seq = h[0] | h[1] << 8; r.id = le32(h + 2); r.type = h[6]; r.len = h[7] | h[8] << 8;
            // a block (06, or 09 with no length) is followed by its number, offset and data: keep what came
            const size_t want = (r.type == 6 || r.type == 9) ? std::max(r.len, 1108) : r.len;
            size_t b0 = p + 23, b1 = std::min(st.size(), b0 + want);
            r.body.assign(st.begin() + b0, st.begin() + b1);
            r.bodyOk = r.len > 0 && (int)r.body.size() >= r.len && crc32(r.body.data(), r.len) == le32(h + 14);
            recs.push_back(std::move(r));
        }
        // plain text runs (WMO bulletins) and abbreviated headings TTAAii CCCC YYGGgg
        size_t q = 0;
        while (q < st.size()) {
            size_t e = q;
            while (e < st.size() && ((st[e] >= 0x20 && st[e] < 0x7F) || st[e] == '\r' || st[e] == '\n')) e++;
            if (e - q >= 40) {
                std::string t(st.begin() + q, st.begin() + e);
                if (textSeen.insert(t).second) texts.push_back(t);
            }
            q = e + 1;
        }
        auto up = [&](size_t k) { return k < st.size() && st[k] >= 'A' && st[k] <= 'Z'; };
        auto dg = [&](size_t k) { return k < st.size() && st[k] >= '0' && st[k] <= '9'; };
        for (size_t k = 0; k + 16 <= st.size(); k++) {
            if (!(up(k) && up(k + 1) && up(k + 2) && up(k + 3) && dg(k + 4) && dg(k + 5))) continue;
            size_t c = k + 6 + (st[k + 6] == ' ');
            if (!(up(c) && up(c + 1) && up(c + 2) && up(c + 3))) continue;
            size_t t = c + 4 + (c + 4 < st.size() && st[c + 4] == ' ');
            bool ok = true;
            for (int z = 0; z < 6; z++) ok &= dg(t + z);
            if (!ok) continue;
            std::string hd = std::string(st.begin() + k, st.begin() + k + 6) + " " + std::string(st.begin() + c, st.begin() + c + 4) + " " +
                             std::string(st.begin() + t, st.begin() + t + 6);
            headings[hd]++;
        }
    }
    // record numbers seen against the numbers sent (u16, wraps)
    std::vector<int64_t> seqs;
    {
        std::vector<std::pair<int64_t, int>> fs;
        for (auto& r : recs) fs.push_back({r.frame, r.seq});
        std::sort(fs.begin(), fs.end());
        int64_t base = 0; int last = -1;
        for (auto& x : fs) {
            if (last >= 0 && x.second < last - 30000) base += 65536;
            if (last >= 0 && x.second > last + 30000) continue;
            last = x.second;
            seqs.push_back(base + x.second);
        }
        std::sort(seqs.begin(), seqs.end());
        seqs.erase(std::unique(seqs.begin(), seqs.end()), seqs.end());
    }
    // gaps over 3000 numbers are pauses of the service (no data on the line), not records sent elsewhere
    int64_t span = 0;
    for (size_t i = 1; i < seqs.size(); i++) span += std::min<int64_t>(seqs[i] - seqs[i - 1], 3000);
    const double seen = span > 0 ? (double)seqs.size() / (span + 1) : 0;
    // files, as the Бизнес-Текст files: each at its size from the descriptor (type 05); the blocks (06 / 09:
    // number, offset = (number - 1) x 1100, data) put in where they belong - checked blocks as they are, the
    // bytes of broken-off blocks voted between copies; bytes not received are 0, and [...] in the .txt
    struct Piece { uint32_t id, off; int64_t frame; bool ok; std::vector<uint8_t> data; };
    std::vector<Piece> pieces;
    std::map<int, int> byType, okByType;
    for (auto& r : recs) {
        byType[r.type]++;
        okByType[r.type] += r.bodyOk;
        if ((r.type != 6 && r.type != 9) || r.body.size() <= 8) continue;
        uint32_t no = le32(&r.body[0]), off = le32(&r.body[4]);
        if (no < 1 || no > 4000 || off != (no - 1) * 1100) continue;
        size_t n = std::min<size_t>(r.body.size() - 8, 1100);
        if (r.bodyOk && r.type == 6) n = std::min<size_t>((size_t)r.len - 8, 1100);
        pieces.push_back({r.id, off, r.frame, r.bodyOk && r.type == 6, std::vector<uint8_t>(r.body.begin() + 8, r.body.begin() + 8 + n)});
    }
    struct FileAcc { TviFile f; int64_t frame; int copies = 0; };
    std::vector<FileAcc> files;
    for (auto& r : recs) {
        if (r.type != 5 || !r.bodyOk || r.body.size() < 11) continue;
        int nl = r.body[10];
        if (11 + nl > (int)r.body.size()) continue;
        std::string name(r.body.begin() + 11, r.body.begin() + 11 + nl);
        uint32_t size = le32(&r.body[6]);
        if (!size || size > (64u << 20)) continue;
        FileAcc* fa = nullptr;
        for (auto& x : files)
            if (x.f.id == r.id && x.f.name == name && x.f.size == size) fa = &x;
        if (!fa) {
            files.push_back({});
            fa = &files.back();
            fa->f.id = r.id; fa->f.name = name; fa->f.shortName = name; fa->f.size = size; fa->frame = r.frame;
            fa->f.blocks = (int)((size + 1099) / 1100);
        }
        fa->copies++;
    }
    std::string fdir = path_join(dir, "tvinform_files");
    std::string list;
    int complete = 0;
    std::set<std::string> names;
    for (auto& fa : files) {
        // the pieces of this id nearest in time to this descriptor (an id is used again for a newer version)
        std::vector<std::map<uint8_t, int>> votes(fa.f.size);
        std::vector<char> sure(fa.f.size, 0);
        std::vector<uint8_t> bin(fa.f.size, 0);
        for (auto& pc : pieces) {
            if (pc.id != fa.f.id || pc.off >= fa.f.size) continue;
            const FileAcc* nearest = nullptr;
            for (auto& o : files)
                if (o.f.id == pc.id && (!nearest || std::llabs(o.frame - pc.frame) < std::llabs(nearest->frame - pc.frame))) nearest = &o;
            if (nearest != &fa) continue;
            for (size_t i = 0; i < pc.data.size() && pc.off + i < fa.f.size; i++) {
                if (pc.ok) { bin[pc.off + i] = pc.data[i]; sure[pc.off + i] = 1; }
                else votes[pc.off + i][pc.data[i]]++;
            }
        }
        size_t got = 0;
        std::vector<char> known(fa.f.size, 0);
        for (size_t i = 0; i < fa.f.size; i++) {
            if (sure[i]) { known[i] = 1; got++; continue; }
            if (votes[i].empty()) continue;
            auto bst = std::max_element(votes[i].begin(), votes[i].end(), [](auto& u, auto& v) { return u.second < v.second; });
            bin[i] = bst->first; known[i] = 1; got++;
        }
        int have = 0;
        for (int b = 0; b < fa.f.blocks; b++) {
            bool all = true;
            for (size_t i = (size_t)b * 1100; i < std::min<size_t>(fa.f.size, (size_t)(b + 1) * 1100); i++) all &= sure[i] != 0;
            have += all;
            if (!all) fa.f.missing.push_back(b + 1);
        }
        fa.f.have = have;
        complete += have == fa.f.blocks;
        std::string safe = fa.f.name;
        for (auto& c : safe) if (strchr("\\/:*?\"<>|", c)) c = '_';
        if (names.count(safe)) safe = fmt("%s_%08X_%u", safe.c_str(), fa.f.id, fa.f.size);
        names.insert(safe);
        fa.f.note = have == fa.f.blocks ? "complete, every block checked by CRC-32"
                    : fmt("%zu of %u bytes received (%d of %d blocks checked by CRC-32); bytes not received are 0, [...] in the .txt; "
                          "the rest went on VBI lines the capture does not keep", got, fa.f.size, have, fa.f.blocks);
        if (fa.f.missing.size() == (size_t)fa.f.blocks) fa.f.missing.clear();   // nothing checked: the list says nothing
        try {
            make_dirs(fdir);
            fa.f.path = path_join(fdir, safe);
            write_file(fa.f.path, bin.data(), bin.size());
            // the text in it: runs of 6+ characters, gaps as [...]
            std::string txt;
            size_t i = 0;
            auto txtByte = [](uint8_t c) { return (c >= 0x20 && c < 0x7F) || c == 13 || c == 10 || c == 9; };
            while (i < bin.size()) {
                if (!known[i]) {
                    while (i < bin.size() && !known[i]) i++;
                    if (!txt.empty() && txt.back() != 10) txt += (char)10;
                    txt += "[...]"; txt += (char)10;
                    continue;
                }
                size_t j = i;
                while (j < bin.size() && known[j] && txtByte(bin[j])) j++;
                if (j - i >= 6) {
                    if (!txt.empty() && txt.back() != 10) txt += (char)10;
                    for (size_t k = i; k < j; k++) if (bin[k] != 13) txt += (char)bin[k];
                }
                i = j > i ? j : i + 1;
            }
            write_text(fa.f.path + ".txt", txt);
            s.files.push_back(fa.f.path);
        } catch (...) { fa.f.path.clear(); }
        list += fmt("%-14s %7u bytes  id %08X  at %s  %zu bytes received, blocks %d of %d checked\n", safe.c_str(), fa.f.size, fa.f.id,
                    tvi3Hms(fa.frame / fps).c_str(), got, have, fa.f.blocks);
        s.fileInfo.push_back(fa.f);
    }
    if (!files.empty())
        try {
            write_text(path_join(fdir, "files.txt"),
                       "ТВ-информ fТ3: files announced by their descriptors (records type 05), built from the blocks received.\n"
                       "Bytes not received are zero in the file and [...] in the .txt.\n\n" + list);
        } catch (...) {}
    // beacons: DOS date and time of the inserter (year u16, month << 8 | day, hour << 8 | minute, second << 8 | 1/100)
    // (a damaged body can pass for a date: the beacons kept are those of the common date whose clock runs
    // with the recording, within 5 minutes of the median)
    struct Bc { std::string date; int64_t frame; double off; int sec; };
    std::vector<Bc> bc;
    for (auto& r : recs) {
        if (r.type != 0 || r.body.size() < 8) continue;
        const uint8_t* b = r.body.data();
        int y = b[0] | b[1] << 8, d = b[2], mo = b[3], mi = b[4], hr = b[5], cs = b[6], se = b[7];
        if (y < 1990 || y > 2030 || mo < 1 || mo > 12 || d < 1 || d > 31 || hr > 23 || mi > 59 || se > 59 || cs > 99) continue;
        int sec = hr * 3600 + mi * 60 + se;
        bc.push_back({fmt("%04d-%02d-%02d", y, mo, d), r.frame, sec - r.frame / fps, sec});
    }
    std::string firstBeacon, lastBeacon;
    int64_t bFrame0 = -1;
    double clockAtStart = -1;
    if (!bc.empty()) {
        std::map<std::string, int> dates;
        for (auto& x : bc) dates[x.date]++;
        std::string date = std::max_element(dates.begin(), dates.end(), [](auto& u, auto& v) { return u.second < v.second; })->first;
        std::vector<double> offs;
        for (auto& x : bc) if (x.date == date) offs.push_back(x.off);
        std::nth_element(offs.begin(), offs.begin() + offs.size() / 2, offs.end());
        const double med = offs[offs.size() / 2];
        int kept = 0;
        for (auto& x : bc) {
            if (x.date != date || std::fabs(x.off - med) > 300) continue;
            std::string t = x.date + " " + tvi3Hms(x.sec);
            if (firstBeacon.empty()) firstBeacon = t, bFrame0 = x.frame;
            lastBeacon = t;
            kept++;
        }
        clockAtStart = med;
        if (kept)
            s.beacons.push_back("\"TV-INFORM\" inserter clock " + firstBeacon + " .. " + lastBeacon.substr(11) + ", at " + tvi3Hms(bFrame0 / fps) +
                                " of the recording - test text TV-INFORM / THE RED FOX JUMPS OVER THE LAZY DOG");
    }
    // texts: written as they came (the stream of the captured lines only)
    std::string tpath;
    if (!texts.empty()) {
        std::string all;
        for (auto& t : texts) all += t + "\r\n----\r\n";
        try {
            make_dirs(dir);
            tpath = path_join(dir, "tvinform_ft3_texts.txt");
            write_text(tpath, all);
            s.files.push_back(tpath);
        } catch (...) { tpath.clear(); }
    }
    // the report
    auto& R = s.report;
    R.push_back("ТВ-информ fТ3 (GOST R 50822-95, 6.9375 MHz, 360 bits a line: 55 55 FB AC + 41 bytes), file transfer records");
    R.push_back(fmt("  %lld lines with the header, %lld of them idle (00 / FF); %lld record headers with a correct CRC-32", (long long)lines,
                    (long long)idle, (long long)hdrOk));
    if (!seqs.empty())
        R.push_back(fmt("  record numbers %lld..%lld (counted on past 65535): %.1f%% of them seen on these lines - the service sends on more VBI lines than the "
                        "capture keeps (ГОСТ: 11 a field), so most records and nearly all file blocks are not in the recording",
                        (long long)seqs.front(), (long long)seqs.back(), seen * 100));
    {
        static const char* tn[] = {"beacon", "?", "?", "file header (GRMC)", "?", "file description", "file block", "?", "?", "block (no CRC)"};
        std::string t = "  records:";
        for (auto& kv : byType)
            t += fmt(" %s %d", kv.first >= 0 && kv.first <= 9 ? tn[kv.first] : fmt("type %d", kv.first).c_str(), kv.second) +
                 (okByType[kv.first] ? fmt(" (%d whole)", okByType[kv.first]) : std::string());
        R.push_back(t);
    }
    if (!firstBeacon.empty()) {
        R.push_back("  inserter clock (beacon records, DOS date/time): " + firstBeacon + " .. " + lastBeacon.substr(11) +
                    fmt(" - the recording starts at about %s", tvi3Hms(std::fmod(clockAtStart + 86400, 86400)).c_str()));
    }
    R.push_back(fmt("  files announced: %zu (%d complete)", files.size(), complete));
    for (auto& fa : files)
        R.push_back(fmt("    %-14s %7u bytes  %2d blocks, %2d received   id %08X  at %s", fa.f.name.c_str(), fa.f.size, fa.f.blocks, fa.f.have, fa.f.id,
                        tvi3Hms(fa.frame / fps).c_str()));
    if (!headings.empty()) {
        std::map<std::string, int> centre;
        for (auto& kv : headings) centre[kv.first.substr(7, 4)]++;
        std::vector<std::pair<int, std::string>> c;
        for (auto& kv : centre) c.push_back({-kv.second, kv.first});
        std::sort(c.begin(), c.end());
        std::string t = fmt("  WMO bulletins: %zu headings; originating centres:", headings.size());
        for (size_t i = 0; i < c.size() && i < 16; i++) t += fmt(" %s %d", c[i].second.c_str(), -c[i].first);
        R.push_back(t);
        int shown = 0;
        for (auto& kv : headings)
            if (shown++ < 60) R.push_back("    " + kv.first);
        if ((int)headings.size() > 60) R.push_back("    ...");
    }
    if (!texts.empty()) {
        R.push_back(fmt("  readable text: %zu pieces%s", texts.size(), tpath.empty() ? "" : (", all in " + tpath).c_str()));
        int shown = 0;
        for (auto& t : texts) {
            if (shown++ >= 25) break;
            std::string x;
            for (char c : t) x += c == '\r' ? ' ' : c == '\n' ? '/' : c;
            R.push_back("    " + x.substr(0, 110));
        }
    }
    s.texts = texts;
    s.entropy = 0;
}
