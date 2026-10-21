// Japanese teletext receiver core shared by the CPU path and the OpenCL kernel (as slicer_core.h): wrapped in
// JTES_SRC(...), plain C++ on the CPU, stringified and compiled by the OpenCL driver in slicer_gpu.cpp.
// After JTES Decoder 0.9.50 (core.py, receiver_hauppauge.py), in float.
//   jt_decode: 296 bits from a start and a rate - two samples a bit, MLSE over 64 states with a 7-tap channel per
//              half bit (h[0..7] first half, h[8..15] second; taps for bits t-3..t+3, then the level), run-in
//              55 55 A7 forced, 3 zero bits after; then the (272,190) code corrected by majority logic over its
//              273 cyclic checks (up to 8 bits). Out: the 272 bits after the run-in packed LSB first (34 bytes),
//              all 296 decided bits packed (37 bytes), corrected bits or -1, cost.
//   jt_cand:   the 8 best (start, samples a bit) for the run-in + service byte over start -60..80, 8 rates;
//              fit = residual of the best over the variance of the line start (packets fit under 0.4).

#ifndef JTES_SRC
#define JTES_SRC(...) __VA_ARGS__
#endif
#ifndef GLOBAL
#include <cmath>
#define GLOBAL
#define FN static inline
#endif
#ifndef JT_CONST
#define JT_CONST static const
#endif
#ifndef JT_U64
#define JT_U64 unsigned long long
#endif

JTES_SRC(

JT_CONST int jt_prefix[24] = {1,0,1,0,1,0,1,0, 1,0,1,0,1,0,1,0, 1,1,1,0,0,1,0,1};
JT_CONST int jt_off[17] = {5, 10, 20, 39, 40, 47, 78, 80, 91, 94, 103, 139, 156, 160, 182, 188, 206};
JT_CONST float jt_rates[8] = {4.667f, 4.677f, 4.687f, 4.697f, 4.707f, 4.717f, 4.727f, 4.737f};

FN float jt_at(GLOBAL const unsigned char* a, int n, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > (float)(n - 1)) t = (float)(n - 1);
    int j = (int)t;
    float q = t - (float)j;
    int j1 = j + 1 < n ? j + 1 : n - 1;
    return (float)a[j] * (1.0f - q) + (float)a[j1] * q;
}

FN int jt_ecc(unsigned char* word)
{
    unsigned char c[273], orig[273];
    int votes[273];
    for (int i = 0; i < 272; i++) { c[i] = word[271 - i]; orig[i] = c[i]; }
    c[272] = 0; orig[272] = 0;
    for (int it = 0; it < 12; it++) {
        int bad = 0;
        for (int i = 0; i < 273; i++) votes[i] = 0;
        for (int r = 0; r < 273; r++) {
            int syn = 0;
            for (int j = 0; j < 17; j++) syn ^= c[(r + jt_off[j]) % 273];
            if (syn) {
                bad++;
                for (int j = 0; j < 17; j++) votes[(r + jt_off[j]) % 273]++;
            }
        }
        if (!bad) {
            int errors = 0;
            for (int i = 0; i < 273; i++) errors += orig[i] != c[i];
            if (c[272] || errors > 8) return -1;
            for (int i = 0; i < 272; i++) word[271 - i] = c[i];
            return errors;
        }
        int flips = 0;
        for (int i = 0; i < 272; i++)
            if (votes[i] >= 9) { c[i] ^= 1; flips++; }
        if (!flips) return -1;
    }
    return -1;
}

// back: 299 words of 64 bits (scratch); word34 / bits37 / cost / ec out
FN void jt_decode(GLOBAL const unsigned char* a, int n, float start, float spb, GLOBAL const float* h,
                  GLOBAL JT_U64* back, GLOBAL unsigned char* word34, GLOBAL unsigned char* bits37,
                  GLOBAL float* costOut, GLOBAL int* ecOut)
{
    float pred[256];
    for (int v = 0; v < 128; v++)
        for (int p = 0; p < 2; p++) {
            float s = h[p * 8 + 7];
            for (int k = 0; k < 7; k++) s += h[p * 8 + k] * (float)((v >> (6 - k)) & 1);
            pred[v * 2 + p] = s;
        }
    float d[64], nd[64];
    for (int i = 0; i < 64; i++) d[i] = 1e30f;
    d[0] = 0.0f;
    for (int nn = 0; nn < 299; nn++) {
        int t = nn - 3;
        float y0 = 0.0f, y1 = 0.0f;
        if (t >= 0 && t < 296) {
            y0 = jt_at(a, n, start + ((float)(2 * t) + 0.5f) * spb * 0.5f);
            y1 = jt_at(a, n, start + ((float)(2 * t + 1) + 0.5f) * spb * 0.5f);
        }
        JT_U64 bk = 0;
        for (int st = 0; st < 64; st++) {
            nd[st] = 1e30f;
            int b = st & 1;
            if (nn < 24 && b != jt_prefix[nn]) continue;
            if (nn >= 296 && b) continue;
            int prev = st >> 1;
            float low = d[prev], high = d[prev + 32];
            if (t >= 0 && t < 296) {
                float e;
                e = y0 - pred[st * 2]; low += e * e;
                e = y1 - pred[st * 2 + 1]; low += e * e;
                e = y0 - pred[(st + 64) * 2]; high += e * e;
                e = y1 - pred[(st + 64) * 2 + 1]; high += e * e;
            }
            if (low < nd[st]) nd[st] = low;
            if (high < nd[st]) { nd[st] = high; bk |= ((JT_U64)1) << st; }
        }
        back[nn] = bk;
        for (int i = 0; i < 64; i++) d[i] = nd[i];
    }
    int st = 0;
    for (int i = 1; i < 64; i++) if (d[i] < d[st]) st = i;
    float cost = d[st] / 592.0f;
    unsigned char bits[296];
    for (int nn = 298; nn >= 0; nn--) {
        if (nn < 296) bits[nn] = (unsigned char)(st & 1);
        int prev = st >> 1;
        st = ((back[nn] >> st) & 1) ? prev + 32 : prev;
    }
    for (int k = 0; k < 37; k++) {
        int v = 0;
        for (int i = 0; i < 8; i++) v |= bits[k * 8 + i] << i;
        bits37[k] = (unsigned char)v;
    }
    unsigned char w[272];
    for (int i = 0; i < 272; i++) w[i] = bits[24 + i];
    int ec = jt_ecc(w);
    for (int k = 0; k < 34; k++) {
        int v = 0;
        for (int i = 0; i < 8; i++) v |= w[k * 8 + i] << i;
        word34[k] = (unsigned char)v;
    }
    *costOut = cost;
    *ecOut = ec;
}

// pred: 6 services x 26 bits x 2 halves; costs: 1128 floats scratch; out: 8 starts, 8 rates, count, fit
FN void jt_cand(GLOBAL const unsigned char* a, int n, GLOBAL const float* pred, GLOBAL float* costs,
                GLOBAL float* outSt, GLOBAL float* outSpb, GLOBAL int* outN, GLOBAL float* outFit)
{
    int idx = 0;
    for (int r = 0; r < 8; r++) {
        float spb = jt_rates[r];
        for (int s = -60; s <= 80; s++, idx++) {
            float y[52];
            int act[52];
            int count = 0;
            for (int i = 0; i < 26; i++)
                for (int ph = 0; ph < 2; ph++) {
                    float t = (float)s + ((float)(3 + i) + (ph ? 0.75f : 0.25f)) * spb;
                    act[i * 2 + ph] = t >= 0.0f;
                    count += t >= 0.0f;
                    y[i * 2 + ph] = jt_at(a, n, t);
                }
            float best = 1e30f;
            for (int sv = 0; sv < 6; sv++) {
                float tot = 0.0f;
                for (int k = 0; k < 52; k++)
                    if (act[k]) { float e = y[k] - pred[sv * 52 + k]; tot += e * e; }
                tot /= (float)(count > 0 ? count : 1);
                if (tot < best) best = tot;
            }
            costs[idx] = best;
        }
    }
    int m = n < 300 ? n : 300;
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < m; i++) { float v = (float)a[i]; s1 += v; s2 += v * v; }
    float var = s2 / (float)m - (s1 / (float)m) * (s1 / (float)m);
    float lowest = 1e30f;
    for (int i = 0; i < 1128; i++) if (costs[i] < lowest) lowest = costs[i];
    *outFit = lowest / (var > 1.0f ? var : 1.0f);
    // in cost order (ties: the earlier one), each far enough from those taken
    int got = 0;
    for (int pass = 0; pass < 1128 && got < 8; pass++) {
        int bi = -1;
        float bv = 1e30f;
        for (int i = 0; i < 1128; i++) if (costs[i] < bv) { bv = costs[i]; bi = i; }
        if (bi < 0) break;
        costs[bi] = 2e30f;
        float st = (float)(bi % 141 - 60), spb = jt_rates[bi / 141];
        int far = 1;
        for (int k = 0; k < got; k++)
            if (!(fabs(st - outSt[k]) >= 1.5f || fabs(spb - outSpb[k]) >= 0.015f)) { far = 0; break; }
        if (far) { outSt[got] = st; outSpb[got] = spb; got++; }
    }
    *outN = got;
}

)
