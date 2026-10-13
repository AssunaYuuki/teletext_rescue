#include "tvinform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

bool tviSlice(const uint8_t* line, int fmt, int n, float T, int nbits, std::vector<uint8_t>& out, float& quality)
{
    if (T < 2 || n < (int)(T * (nbits + 4))) return false;
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
    if (st < 0 || st + T * nbits > n) return false;
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
    int nb = nbits / 8;
    out.assign(nb, 0);
    quality = 1;
    for (int k = 0; k < nbits; k++) {
        double c = st2 + (k + 0.5) * Tf;
        if (c >= n) return false;
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
}
