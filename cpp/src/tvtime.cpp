#include "tvtime.h"
#include "util.h"
#include <algorithm>

namespace {
// one frame of the line: '-' no signal, '0' no symbol, 'A' 1.0 MHz (second mark), 'B' 1.67 MHz (0), 'C' 2.5 MHz (1);
// head = the 1 MHz burst at the start is there
char symbolOf(const uint8_t* p, const VbiGeometry& g, bool& head)
{
    head = false;
    const int n = std::max(8, g.validSamples);
    std::vector<float> x(n);
    for (int i = 0; i < n; i++) x[i] = g.fmt ? (p[2 * i] | p[2 * i + 1] << 8) / 256.0f : p[i];
    std::vector<float> s(x);
    std::nth_element(s.begin(), s.begin() + n / 20, s.end());
    float lo = s[n / 20];
    std::nth_element(s.begin(), s.begin() + n * 19 / 20, s.end());
    float hi = s[n * 19 / 20];
    if (hi - lo < 40) return '-';
    const float mid = (lo + hi) / 2;
    const double us = g.lineSamples * g.lineRate / 1e6;   // samples per microsecond
    int t0 = -1;
    for (int i = 1; i < n && t0 < 0; i++) if (x[i - 1] <= mid && x[i] > mid) t0 = i;
    if (t0 < 0) return '-';
    auto crossings = [&](double a, double b) {
        int i0 = std::max(1, (int)(t0 + a * us)), i1 = std::min(n, (int)(t0 + b * us));
        int c = 0;
        for (int i = i0; i < i1; i++) c += (x[i - 1] > mid) != (x[i] > mid);
        return i1 > i0 ? c : -1;
    };
    int ch = crossings(2, 15);   // 13 us of 1 MHz: 26
    head = ch >= 18 && ch <= 34;
    // the symbol: 11 us at the end, crossings of its own middle (sample.vbi: 1.0 MHz 21-22, 1.67 MHz 34-39,
    // 2.5 MHz 40-55 - it comes weaker)
    int i0 = (int)(t0 + 35 * us), i1 = std::min(n, (int)(t0 + 46 * us));
    if (i1 - i0 < 20) return '-';
    float mn = x[i0], mx = x[i0], m = 0;
    for (int i = i0; i < i1; i++) { mn = std::min(mn, x[i]); mx = std::max(mx, x[i]); m += x[i]; }
    if (mx - mn < 25) return '0';
    m /= (float)(i1 - i0);
    int c = 0;
    for (int i = i0 + 1; i < i1; i++) c += (x[i - 1] > m) != (x[i] > m);
    return c < 15 ? '0' : c < 30 ? 'A' : c < 40 ? 'B' : 'C';
}

int field(const std::string& w, std::initializer_list<int> pos)
{
    int v = 0;
    for (int p : pos) {
        if (w[p] != 'B' && w[p] != 'C') return -1;
        v = v * 2 + (w[p] == 'C');
    }
    return v;
}
}

bool tvTimeProbe(VbiFile& vf, int line, int frames)
{
    const VbiGeometry& g = vf.geom();
    std::vector<uint8_t> buf;
    int64_t n = vf.read(0, std::min<int64_t>(frames, g.frames), buf);
    int head = 0, sym = 0;
    for (int64_t f = 0; f < n; f++) {
        bool h;
        char c = symbolOf(&buf[(size_t)f * g.frameBytes() + (size_t)line * g.lineBytes], g, h);
        head += h;
        sym += c == 'A' || c == 'B' || c == 'C';
    }
    return n >= 50 && head * 10 >= n * 7 && sym * 10 >= n * 6;
}

bool tvTimeDecode(VbiFile& vf, int line, int64_t maxFrames, TvTimeCode& out, std::atomic<bool>* cancel)
{
    const VbiGeometry& g = vf.geom();
    out = TvTimeCode();
    out.line = line;
    int64_t total = maxFrames > 0 ? std::min(maxFrames, g.frames) : g.frames;
    std::string seq;
    std::vector<uint8_t> buf;
    for (int64_t f0 = 0; f0 < total; f0 += 500) {
        if (cancel && *cancel) return false;
        int64_t n = vf.read(f0, std::min<int64_t>(500, total - f0), buf);
        for (int64_t f = 0; f < n; f++) {
            bool h;
            char c = symbolOf(&buf[(size_t)f * g.frameBytes() + (size_t)line * g.lineBytes], g, h);
            seq += c;
        }
        if (n < 500) break;
    }
    out.frames = (int64_t)seq.size();
    for (char c : seq) out.symbols += c == 'A' || c == 'B' || c == 'C';
    // a second: the mark, the gap, then 23 symbols
    for (size_t i = 0; i + 25 <= seq.size(); i++) {
        if (seq[i] != 'A' || seq[i + 1] != '0') continue;
        std::string w = seq.substr(i, 25);
        int ht = field(w, {4, 5}), hu = field(w, {6, 7, 8, 9}), mt = field(w, {10, 11, 12}), mu = field(w, {13, 14, 15, 16});
        int st = field(w, {18, 19, 20}), su = field(w, {21, 22, 23, 24}), par = field(w, {17});
        if (ht < 0 || hu < 0 || mt < 0 || mu < 0 || st < 0 || su < 0 || par < 0) continue;
        if (hu > 9 || mu > 9 || su > 9 || mt > 5 || st > 5 || ht * 10 + hu > 23) continue;
        int ones = par;
        for (int p = 18; p <= 24; p++) ones += w[p] == 'C';
        TvTimeSecond s;
        s.frame = (int64_t)i;
        s.h = ht * 10 + hu; s.m = mt * 10 + mu; s.s = st * 10 + su;
        s.parityOk = ones % 2 == 0;
        if (s.parityOk) out.seconds.push_back(s);
        i += 24;
    }
    if (out.seconds.size() < 3) return false;
    // seconds that agree with their neighbours (one second = 25 frames)
    auto sod = [](const TvTimeSecond& s) { return s.h * 3600 + s.m * 60 + s.s; };
    int agree = 0;
    for (size_t k = 1; k < out.seconds.size(); k++) {
        int64_t df = out.seconds[k].frame - out.seconds[k - 1].frame;
        agree += (int64_t)(sod(out.seconds[k]) - sod(out.seconds[k - 1])) * 25 == df;
    }
    if (agree * 2 < (int)out.seconds.size() - 1) return false;
    auto hms = [](const TvTimeSecond& s) { return fmt("%02d:%02d:%02d", s.h, s.m, s.s); };
    auto& R = out.report;
    int lineNo, fld;
    lineOf(g, line, lineNo, fld);
    R.push_back(fmt("Time code (line %d field %d): a 1 MHz reference burst, then one symbol per frame - 2.5 MHz = 1, 1.67 MHz = 0, 1.0 MHz = second mark; "
                    "25 symbols a second: hours, minutes, seconds in BCD, seconds with a parity bit", lineNo, fld));
    R.push_back(fmt("  %lld frames, %lld with a symbol; %zu seconds read (parity good), %d of %zu steps one second apart",
                    (long long)out.frames, (long long)out.symbols, out.seconds.size(), agree, out.seconds.size() - 1));
    R.push_back("  time on the air: " + hms(out.seconds.front()) + fmt(" (frame %lld)", (long long)out.seconds.front().frame) + " .. " +
                hms(out.seconds.back()) + fmt(" (frame %lld)", (long long)out.seconds.back().frame));
    // jumps (a cut in the recording, a new hour ...)
    int jumps = 0;
    for (size_t k = 1; k < out.seconds.size() && jumps < 10; k++) {
        int64_t df = out.seconds[k].frame - out.seconds[k - 1].frame;
        int64_t ds = sod(out.seconds[k]) - sod(out.seconds[k - 1]);
        if (ds * 25 != df && std::llabs(ds * 25 - df) > 50) {
            R.push_back("  jump: " + hms(out.seconds[k - 1]) + " -> " + hms(out.seconds[k]) + fmt(" at frame %lld", (long long)out.seconds[k].frame));
            jumps++;
        }
    }
    return true;
}
