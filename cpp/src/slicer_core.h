// Line slicer shared by the CPU path and the OpenCL kernel.
//
// The body is wrapped in SLICER_SRC(...): on the CPU it expands to plain C++,
// in gpu_opencl.cpp it is stringified and compiled by the OpenCL driver, so
// both decoders run exactly the same algorithm.
//
// Modes:
//   0 = "telecaption" rate, 32 x fH (503 kbit/s): EIA-608 closed captions,
//       XDS and StarSight (line 16, 8-bit binary bytes, both fields).
//       7-cycle run-in, two zero bits, start bit, 16 data bits LSB first.
//   1 = Silent Radio (US4620227): preamble 101010101000011, 16 data bits,
//       80 x fH on the WTTW recording.
//
// Output per line, 20 floats:
//   [0] 1 if a run-in was found   [1] start-bit centre (sample)   [2] amplitude
//   [3] framing score             [4..19] soft bits, (level - thr) / amp
//
// `phase` shifts the data sampling point in bits; tape group delay moves the
// best point away from the run-in grid, so the decoder calibrates it.
// `anchor` > 0 pins the start bit to that sample position instead of
// searching for it; on time-base-corrected captures it is constant per line
// and a damaged run-in can otherwise cause a whole-bit slip.

#ifndef SLICER_SRC
#define SLICER_SRC(...) __VA_ARGS__
#endif
#ifndef GLOBAL
#include <cmath>
using std::floor;
using std::fabs;
#define GLOBAL
#define FN static inline
#endif

SLICER_SRC(

FN float ss_get(GLOBAL const unsigned char* p, int fmt, int i)
{
    if (fmt == 1) return (float)(p[2 * i] | (p[2 * i + 1] << 8));
    return (float)p[i];
}

FN float ss_interp(GLOBAL const unsigned char* p, int fmt, int n, float t)
{
    int i = (int)floor(t);
    if (i < 0) i = 0;
    if (i > n - 2) i = n - 2;
    float f = t - (float)i;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    float a = ss_get(p, fmt, i);
    float b = ss_get(p, fmt, i + 1);
    return a + (b - a) * f;
}

FN float ss_bit(GLOBAL const unsigned char* p, int fmt, int n, float c, float bp, float thr, float amp)
{
    (void)bp;
    return (ss_interp(p, fmt, n, c) - thr) / amp;
}

FN void slice_line(GLOBAL const unsigned char* p, int fmt, int n, float spl, int mode, float phase, float anchor,
                   GLOBAL float* out)
{
    int i, j, k;
    for (k = 0; k < 20; k++) out[k] = 0.0f;

    int a = (int)(n * 0.10f), b = (int)(n * 0.96f);
    float mn = 1e30f, mx = -1e30f;
    for (i = a; i < b; i++) {
        float v = ss_get(p, fmt, i);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx - mn < 1.0f) return;

    int hist[64];
    for (k = 0; k < 64; k++) hist[k] = 0;
    float sc = 63.999f / (mx - mn);
    for (i = a; i < b; i++) hist[(int)((ss_get(p, fmt, i) - mn) * sc)]++;
    int total = b - a, cum = 0, blo = 0, bhi = 63;
    for (k = 0; k < 64; k++) { cum += hist[k]; if (cum * 10 >= total) { blo = k; break; } }
    cum = 0;
    for (k = 0; k < 64; k++) { cum += hist[k]; if (cum * 100 >= total * 97) { bhi = k; break; } }
    float lo = mn + (blo + 0.5f) / sc, hi = mn + (bhi + 0.5f) / sc;
    float thr = 0.5f * (lo + hi);
    if (hi - lo < (mx - mn) * 0.15f) return;

    /* rising crossings of the run-in */
    float P = (mode == 0) ? spl / 32.0f : spl / 40.0f;
    float cr[48];
    int nc = 0;
    int s0 = (int)(spl * 0.06f), s1 = (int)(spl * 0.45f);
    if (s0 < 1) s0 = 1;
    if (s1 > n - 1) s1 = n - 1;
    for (i = s0; i < s1 && nc < 48; i++) {
        float x0 = ss_get(p, fmt, i - 1), x1 = ss_get(p, fmt, i);
        if (x0 < thr && x1 >= thr) cr[nc++] = (float)(i - 1) + (thr - x0) / (x1 - x0);
    }
    int bestStart = -1, bestLen = 0;
    for (i = 0; i < nc; i++) {
        int len = 1;
        float last = cr[i];
        for (j = i + 1; j < nc; j++) {
            float d = cr[j] - last;
            float mr = floor(d / P + 0.5f);
            if (d < P * 0.8f) continue;
            /* tolerate a cycle or two lost to a tape dropout */
            if (mr > 3.0f || fabs(d - mr * P) > P * 0.2f) break;
            last = cr[j];
            len++;
        }
        if (len > bestLen) { bestLen = len; bestStart = i; }
    }
    if (bestLen < 3) return;

    /* least-squares fit of crossing m -> position */
    float sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0, last = cr[bestStart];
    int mlast = 0;
    for (j = bestStart; j < nc; j++) {
        float d = cr[j] - last;
        if (j > bestStart) {
            float mr = floor(d / P + 0.5f);
            if (d < P * 0.8f) continue;
            if (mr > 3.0f || fabs(d - mr * P) > P * 0.2f) break;
        }
        last = cr[j];
        float m = floor((cr[j] - cr[bestStart]) / P + 0.5f);
        sx += m; sy += cr[j]; sxx += m * m; sxy += m * cr[j]; cnt += 1.0f;
        mlast = (int)m;
    }
    /* The bit rate is locked to line rate, so only the phase is estimated;
       fitting the period on a noisy run-in extrapolates badly over 26 bits. */
    float Pf = P;
    float r0 = (sy - P * sx) / cnt;
    (void)sxx; (void)sxy;

    /* threshold and amplitude from the run-in itself */
    int ra = (int)r0, rb = (int)(r0 + mlast * Pf);
    if (rb <= ra + 2) return;
    /* average peak and trough of every run-in cycle; on tape the run-in
       sine is asymmetric, so its mean is a poor slicing level */
    float smax = 0, smin = 0;
    int ncyc = 0;
    for (k = 0; k < mlast; k++) {
        int c0 = (int)(r0 + k * Pf), c1 = (int)(r0 + (k + 1) * Pf);
        float cmax = -1e30f, cmin = 1e30f;
        for (i = c0; i < c1 && i < n; i++) {
            float v = ss_get(p, fmt, i);
            if (v > cmax) cmax = v;
            if (v < cmin) cmin = v;
        }
        smax += cmax;
        smin += cmin;
        ncyc++;
    }
    if (ncyc == 0) return;
    thr = 0.5f * (smax + smin) / (float)ncyc;
    float amp = 0.5f * (smax - smin) / (float)ncyc;
    if (amp < 1e-3f) return;

    /* refine the phase with every crossing (rising and falling) of the
       refined level; a biased first threshold skews rising edges only */
    {
        float res = 0;
        int nres = 0;
        int w0 = (int)(r0 - Pf * 0.5f), w1 = (int)(r0 + (mlast + 0.5f) * Pf);
        if (w0 < 1) w0 = 1;
        if (w1 > n - 1) w1 = n - 1;
        for (i = w0; i < w1; i++) {
            float x0 = ss_get(p, fmt, i - 1), x1 = ss_get(p, fmt, i);
            int rising = (x0 < thr && x1 >= thr), falling = (x0 >= thr && x1 < thr);
            if (!rising && !falling) continue;
            float c = (float)(i - 1) + (thr - x0) / (x1 - x0);
            float base = r0 + (falling ? 0.5f * Pf : 0.0f);
            float m = floor((c - base) / Pf + 0.5f);
            float d = c - base - m * Pf;
            if (fabs(d) < Pf * 0.25f) { res += d; nres++; }
        }
        if (nres > 2) r0 += res / (float)nres;
    }

    float best = -1e30f, bp;
    int bs = 0;
    if (mode == 0) {
        /* bit centres sit on the run-in rising-crossing grid; start bit is
           9 periods after the first crossing, preceded by two zero bits */
        bp = Pf;
        int ka = -2, kb = 2;
        if (anchor > 0.0f) { ka = kb = (int)floor((anchor - r0) / bp + 0.5f) - 9; }
        for (k = ka; k <= kb; k++) {
            float s = ss_bit(p, fmt, n, r0 + (9 + k) * bp, bp, thr, amp)
                    - ss_bit(p, fmt, n, r0 + (8 + k) * bp, bp, thr, amp)
                    - ss_bit(p, fmt, n, r0 + (7 + k) * bp, bp, thr, amp);
            if (s > best) { best = s; bs = k; }
        }
        out[1] = r0 + (9 + bs) * bp;
        for (k = 0; k < 16; k++) out[4 + k] = ss_bit(p, fmt, n, r0 + (10 + bs + k + phase) * bp, bp, thr, amp);
    } else {
        /* Silent Radio: bits are half the run-in period wide */
        bp = Pf * 0.5f;
        int ka = -4, kb = 2;
        if (anchor > 0.0f) { ka = kb = (int)floor((anchor - r0) / bp); }
        for (k = ka; k <= kb; k += 2) {
            float s = 0;
            for (j = 0; j < 15; j++) {
                int want = (j < 9) ? ((j & 1) == 0) : (j >= 13);
                float v = ss_bit(p, fmt, n, r0 + (j + k + 0.5f) * bp, bp, thr, amp);
                s += want ? v : -v;
            }
            if (s > best) { best = s; bs = k; }
        }
        best /= 5.0f;
        out[1] = r0 + (bs + 0.5f) * bp;
        for (k = 0; k < 16; k++) out[4 + k] = ss_bit(p, fmt, n, r0 + (15 + bs + k + 0.5f + phase) * bp, bp, thr, amp);
    }
    out[0] = 1.0f;
    out[2] = amp;
    out[3] = best;
}

)
