// Maximum-likelihood sequence estimation for 5.727 Mbit/s VBI data
// (NABTS, NTSC teletext and similar), shared by the CPU path and OpenCL.
//
// On tape the 2.86 MHz run-in is filtered down to a flat pedestal and the
// bits smear into each other, so a threshold slicer fails. Here every
// observation (two samples per bit) is modelled as a linear response to the
// 7 surrounding bits; Viterbi then finds the most likely bit sequence.
// The response H is learned from the recording itself (fastdata.cpp).
//
// Line layout (spb = samples per bit, nominally line/364):
// st = pedestal edge - 24 bits + phi; bit k is centred at
// st + k * spb. Observation step k samples the centre of bit k-3 and
// halfway to bit k-2.

#ifndef FASTDATA_SRC
#define FASTDATA_SRC(...) __VA_ARGS__
#endif
#ifndef GLOBAL
#include <cmath>
using std::floor;
using std::fabs;
#define GLOBAL
#define FN static inline
#endif

FASTDATA_SRC(

FN float fd_get(GLOBAL const unsigned char* p, int fmt, int i)
{
    if (fmt == 1) return (float)(p[2 * i] | (p[2 * i + 1] << 8));
    return (float)p[i];
}

FN float fd_at(GLOBAL const unsigned char* p, int fmt, int n, float t, float mid, float amp)
{
    int i = (int)floor(t);
    if (i < 0) i = 0;
    if (i > n - 2) i = n - 2;
    float f = t - (float)i;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    float a = fd_get(p, fmt, i), b = fd_get(p, fmt, i + 1);
    return (a + (b - a) * f - mid) / amp;
}

/* Level normalisation and pedestal edge. info: [0] edge, [1] mid, [2] amp
   amplitude, [3] 1 if the line carries a signal. */
FN void fd_prepare(GLOBAL const unsigned char* p, int fmt, int n, float spl, GLOBAL float* info)
{
    int i, k;
    info[0] = 0; info[1] = 0; info[2] = 1; info[3] = 0;
    int a = (int)(n * 0.07f), b = (int)(n * 0.97f);
    float mn = 1e30f, mx = -1e30f;
    for (i = a; i < b; i++) {
        float v = fd_get(p, fmt, i);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx - mn < 1.0f) return;
    int hist[64];
    for (k = 0; k < 64; k++) hist[k] = 0;
    float sc = 63.999f / (mx - mn);
    for (i = a; i < b; i++) hist[(int)((fd_get(p, fmt, i) - mn) * sc)]++;
    int total = b - a, cum = 0, blo = 0, bhi = 63;
    for (k = 0; k < 64; k++) { cum += hist[k]; if (cum * 100 >= total * 3) { blo = k; break; } }
    cum = 0;
    for (k = 0; k < 64; k++) { cum += hist[k]; if (cum * 100 >= total * 97) { bhi = k; break; } }
    float lo = mn + (blo + 0.5f) / sc, hi = mn + (bhi + 0.5f) / sc;
    if (hi - lo < (mx - mn) * 0.12f) return;
    float mid = 0.5f * (lo + hi), amp = 0.5f * (hi - lo);
    /* start of the run-in: the first 16-bit window whose mean level reaches
       the middle (the run-in averages to mid level even when the tape has
       filtered it flat; blanking and colour burst average far below) */
    int run = (int)(16.0f * spl / 364.0f);
    int s1 = (int)(spl * 0.33f);
    if (s1 > n - 1 - run) s1 = n - 1 - run;
    float sum = 0;
    for (k = 0; k < run; k++) sum += fd_get(p, fmt, 1 + k);
    for (i = 1; i < s1; i++) {
        if ((sum / (float)run - mid) / amp > -0.15f) {
            /* refine: exact crossing halfway between the level before the
               run-in and mid level, for a jitter-free sampling phase */
            int b0 = i - run;
            if (b0 < 1) b0 = 1;
            float base = 0;
            for (k = b0; k < i; k++) base += fd_get(p, fmt, k);
            base = i > b0 ? base / (float)(i - b0) : mid - amp;
            float t = 0.5f * (base + mid);
            float edge = (float)i + 0.5f * (float)run;
            for (k = b0; k < i + run && k < n - 1; k++) {
                float x0 = fd_get(p, fmt, k), x1 = fd_get(p, fmt, k + 1);
                if (x0 <= t && x1 > t) { edge = (float)k + (t - x0) / (x1 - x0); break; }
            }
            info[0] = edge;
            info[1] = mid;
            info[2] = amp;
            info[3] = 1;
            return;
        }
        sum += fd_get(p, fmt, i + run) - fd_get(p, fmt, i);
    }
}

/* Viterbi over 64 states (6 previous bits). H: 16 floats, H[j*2+o] is the
   weight of window bit j (0 = oldest) on observation o, H[14..15] offsets.
   scratch: nb*64 bytes. bits: nb bytes. Returns the mean residual. */
FN float fd_viterbi(GLOBAL const unsigned char* p, int fmt, int n, float spb, float phi, GLOBAL const float* H,
                    GLOBAL const float* info, int nb, GLOBAL unsigned char* scratch, GLOBAL unsigned char* bits)
{
    int k, s, w, j;
    float st = info[0] - 24.0f * spb + phi;
    float mid = info[1], amp = info[2];
    float pred0[128], pred1[128];
    for (w = 0; w < 128; w++) {
        float a0 = H[14], a1 = H[15];
        for (j = 0; j < 7; j++) {
            float sg = ((w >> (6 - j)) & 1) ? 1.0f : -1.0f;
            a0 += H[j * 2] * sg;
            a1 += H[j * 2 + 1] * sg;
        }
        pred0[w] = a0;
        pred1[w] = a1;
    }
    float cost[64], nc[64];
    for (s = 0; s < 64; s++) cost[s] = 0.0f;
    for (k = 0; k < nb; k++) {
        float c = st + (float)(k - 3) * spb;
        float y0 = fd_at(p, fmt, n, c, mid, amp);
        float y1 = fd_at(p, fmt, n, c + 0.5f * spb, mid, amp);
        for (s = 0; s < 64; s++) {
            int b = s & 1, base = s >> 1;
            int pa = base, pb = base | 32;            /* the two predecessors */
            int wa = (pa << 1) | b, wb = (pb << 1) | b;
            float da0 = y0 - pred0[wa], da1 = y1 - pred1[wa];
            float db0 = y0 - pred0[wb], db1 = y1 - pred1[wb];
            float ca = cost[pa] + da0 * da0 + da1 * da1;
            float cb = cost[pb] + db0 * db0 + db1 * db1;
            if (ca <= cb) { nc[s] = ca; scratch[k * 64 + s] = (unsigned char)pa; }
            else { nc[s] = cb; scratch[k * 64 + s] = (unsigned char)pb; }
        }
        float m = nc[0];
        for (s = 1; s < 64; s++) if (nc[s] < m) m = nc[s];
        for (s = 0; s < 64; s++) cost[s] = nc[s] - m;
    }
    int bs = 0;
    for (s = 1; s < 64; s++) if (cost[s] < cost[bs]) bs = s;
    s = bs;
    for (k = nb - 1; k >= 0; k--) {
        bits[k] = (unsigned char)(s & 1);
        s = scratch[k * 64 + s];
    }
    return 0.0f;
}

)
