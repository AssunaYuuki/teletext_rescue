#include "mlse.h"
#include "opencl.h"
#include "vbidecode.h"
#include "teletext.h"
#include <mutex>

static const double PH[MLSE_NP] = {0.3, 0.5, 0.7};

struct MlseDecoder::Pre {
    int NT, NTAP, NS, L, R, K1;
    std::vector<u8> preb;
    std::vector<double> PRE_X;     // 24 × K1
    std::vector<double> PRE_XP;    // (24-L-R) × K1
    std::vector<double> PRE_A;     // K1 × K1
    std::vector<int> km, kv;
};

static std::shared_ptr<MlseDecoder::Pre> make_pre(const MlseModel &m);

// строка регрессии: соседние биты (±1, вне пакета 0) и единица
static void design_row(const double *bits_pm, int nbits, int i, int L, int R, double *out) {
    int c = 0;
    for (int d = -L; d <= R; d++, c++) { int k = i + d; out[c] = (k >= 0 && k < nbits) ? bits_pm[k] : 0.0; }
    out[c] = 1.0;
}

std::shared_ptr<MlseDecoder::Pre> make_pre(const MlseModel &m) {
    auto p = std::make_shared<MlseDecoder::Pre>();
    p->NT = m.NT(); p->NTAP = m.NTAP(); p->NS = m.NS(); p->L = m.L; p->R = m.R; p->K1 = p->NTAP + 1;
    p->preb = bits_lsb(m.pre, 3);
    double pm[24]; for (int i = 0; i < 24; i++) pm[i] = p->preb[i] * 2.0 - 1;
    p->PRE_X.resize(24 * p->K1);
    for (int i = 0; i < 24; i++) design_row(pm, 24, i, m.L, m.R, &p->PRE_X[i * p->K1]);
    int npi = 24 - m.L - m.R;
    p->PRE_XP.resize(npi * p->K1);
    for (int q = 0; q < npi; q++) design_row(pm, 24, m.L + q, m.L, m.R, &p->PRE_XP[q * p->K1]);
    p->PRE_A.assign(p->K1 * p->K1, 0);
    for (int q = 0; q < npi; q++) for (int a = 0; a < p->K1; a++) for (int b = 0; b < p->K1; b++) p->PRE_A[a * p->K1 + b] += p->PRE_XP[q * p->K1 + a] * p->PRE_XP[q * p->K1 + b];
    for (int a = 0; a < p->K1; a++) p->PRE_A[a * p->K1 + a] += 2.0;
    p->km.assign(p->NT, 0); p->kv.assign(p->NT, 0);
    for (int i = 0; i < p->NT; i++)
        for (int d = -m.L; d <= m.R; d++) {
            int k = i + d;
            if (k < 24 || k >= p->NT) {
                int bit = p->NTAP - 1 - (d + m.L);
                p->km[i] |= 1 << bit;
                if (k >= 0 && k < 24 && p->preb[k]) p->kv[i] |= 1 << bit;
            }
        }
    return p;
}

static void sample(const float *y, int n, double off, double spb, int NT, double *Y) {
    for (int i = 0; i < NT; i++) for (int p = 0; p < MLSE_NP; p++) Y[i * MLSE_NP + p] = interp(off + (i + PH[p]) * spb, y, n);
}

// подгонка: строки idx [a, b), биты bits (±1, nbits). prior — K1×NP (транспонированный) или пусто.
// -> C (NP × K1), невязка
static double fit(const MlseDecoder::Pre &P, const double *Y, const double *bits_pm, int nbits, int a, int b,
                  const std::vector<double> *prior, double lam, std::vector<double> &C) {
    int K1 = P.K1, r = b - a;
    std::vector<double> X(r * K1), Yr(r * MLSE_NP);
    for (int q = 0; q < r; q++) {
        design_row(bits_pm, nbits, a + q, P.L, P.R, &X[q * K1]);
        for (int p = 0; p < MLSE_NP; p++) Yr[q * MLSE_NP + p] = Y[(a + q) * MLSE_NP + p];
    }
    std::vector<double> B;
    if (prior && !prior->empty() && lam > 0) {
        std::vector<double> A(K1 * K1, 0); B.assign(K1 * MLSE_NP, 0);
        for (int q = 0; q < r; q++)
            for (int i = 0; i < K1; i++) {
                for (int j = 0; j < K1; j++) A[i * K1 + j] += X[q * K1 + i] * X[q * K1 + j];
                for (int p = 0; p < MLSE_NP; p++) B[i * MLSE_NP + p] += X[q * K1 + i] * Yr[q * MLSE_NP + p];
            }
        for (int i = 0; i < K1; i++) A[i * K1 + i] += lam;
        for (int i = 0; i < K1; i++) for (int p = 0; p < MLSE_NP; p++) B[i * MLSE_NP + p] += lam * (*prior)[p * K1 + i];
        solve(A, B, K1, MLSE_NP);
    } else B = lstsq(X, Yr, r, K1, MLSE_NP);
    C.assign(MLSE_NP * K1, 0);
    for (int i = 0; i < K1; i++) for (int p = 0; p < MLSE_NP; p++) C[p * K1 + i] = B[i * MLSE_NP + p];
    double res = 0;
    for (int q = 0; q < r; q++) for (int p = 0; p < MLSE_NP; p++) {
        double v = Yr[q * MLSE_NP + p];
        for (int i = 0; i < K1; i++) v -= X[q * K1 + i] * B[i * MLSE_NP + p];
        res += v * v;
    }
    return res / (r * MLSE_NP);
}

static double viterbi_cpu(const MlseDecoder::Pre &P, const double *Y, const double *C, u8 *bits) {
    int NS = P.NS, NTAP = P.NTAP, K1 = P.K1, NT = P.NT, R = P.R;
    std::vector<float> pred((size_t)NS * MLSE_NP);
    for (int s = 0; s < NS; s++)
        for (int p = 0; p < MLSE_NP; p++) {
            double v = C[p * K1 + NTAP];
            for (int k = 0; k < NTAP; k++) v += (((s >> (NTAP - 1 - k)) & 1) ? 1.0 : -1.0) * C[p * K1 + k];
            pred[s * MLSE_NP + p] = (float)v;
        }
    std::vector<float> met(NS), nm(NS), bm(NS);
    std::vector<uint64_t> back((size_t)NT * ((NS + 63) / 64), 0);
    int NW = (NS + 63) / 64;
    for (int i = 0; i < NT; i++) {
        float y0 = (float)Y[i * 3], y1 = (float)Y[i * 3 + 1], y2 = (float)Y[i * 3 + 2];
        int km = P.km[i], kv = P.kv[i];
        for (int s = 0; s < NS; s++) {
            if ((s & km) != kv) { bm[s] = 1e9f; continue; }
            const float *q = &pred[s * 3];
            float a = y0 - q[0], b = y1 - q[1], c = y2 - q[2];
            bm[s] = a * a + b * b + c * c;
        }
        if (i == 0) { met = bm; continue; }
        uint64_t *bw = &back[(size_t)i * NW];
        int half = NS >> 1;
        for (int s = 0; s < NS; s++) {
            float m0 = met[s >> 1], m1 = met[(s >> 1) | half];
            bool ch = m1 < m0;
            nm[s] = (ch ? m1 : m0) + bm[s];
            if (ch) bw[s >> 6] |= 1ull << (s & 63);
        }
        met.swap(nm);
    }
    int s = 0; for (int q = 1; q < NS; q++) if (met[q] < met[s]) s = q;
    double best = met[s];
    std::vector<u8> out(NT + R, 0);
    for (int i = NT - 1; i > 0; i--) {
        out[i + R] = s & 1;
        bool ch = (back[(size_t)i * NW + (s >> 6)] >> (s & 63)) & 1;
        s = (s >> 1) | (ch ? (1 << (NTAP - 1)) : 0);
    }
    for (int k = 0; k <= R; k++) out[k] = (s >> (R - k)) & 1;
    memcpy(bits, out.data(), NT);
    return best;
}

// перерешение байтов с ограничением (Хэмминг 8/4, нечётная чётность): соседние биты остаются как есть
static void refine_bytes(const MlseDecoder::Pre &P, const MlseModel &M, const double *Y, const double *C, u8 *bits) {
    static std::vector<int> HAM, PAR;
    static std::once_flag once;
    std::call_once(once, [] { for (int v = 0; v < 16; v++) HAM.push_back(ham::CODEWORDS[v]); for (int v = 0; v < 256; v++) if (odd_parity(v)) PAR.push_back(v); });
    int NT = P.NT, K1 = P.K1, L = P.L, R = P.R, NTAP = P.NTAP;
    std::vector<double> pm(NT); for (int i = 0; i < NT; i++) pm[i] = bits[i] * 2.0 - 1;
    std::vector<u8> data(M.nbytes);
    bytes_lsb(&bits[24], M.nbytes, data.data());
    std::vector<double> fixed;
    std::vector<int> pair;
    for (int b = 0; b < M.nbytes; b++) {
        int code = M.code(data.data(), b);
        if (!code) continue;
        // два первых байта (адрес пакета) — вместе: 16 × 16 вариантов, иначе ошибка в одном тянет другой
        bool two = b == 0 && code == 1 && M.nbytes > 1 && M.code(data.data(), 1) == 1;
        int nb = two ? 2 : 1, NB = 8 * nb;
        const std::vector<int> *cand = code == 1 ? &HAM : &PAR;
        if (two) { if (pair.empty()) for (int x : HAM) for (int y : HAM) pair.push_back(x | (y << 8)); cand = &pair; }
        int b0 = 24 + b * 8, lo = std::max(0, b0 - R), hi = std::min(NT, b0 + NB + L);
        fixed.assign((hi - lo) * MLSE_NP, 0);
        for (int i = lo; i < hi; i++)
            for (int p = 0; p < MLSE_NP; p++) {
                double v = C[p * K1 + NTAP];
                for (int k = 0; k < NTAP; k++) { int j = i - L + k; if (j >= 0 && j < NT && (j < b0 || j >= b0 + NB)) v += pm[j] * C[p * K1 + k]; }
                fixed[(i - lo) * MLSE_NP + p] = Y[i * MLSE_NP + p] - v;
            }
        double best = 1e300; int bv = data[b] | (two ? data[1] << 8 : 0);
        for (int v : *cand) {
            double e = 0;
            for (int i = lo; i < hi && e < best; i++)
                for (int p = 0; p < MLSE_NP; p++) {
                    double d = fixed[(i - lo) * MLSE_NP + p];
                    for (int q = 0; q < NB; q++) { int k = b0 + q - i + L; if (k >= 0 && k < NTAP) d -= (((v >> q) & 1) ? 1.0 : -1.0) * C[p * K1 + k]; }
                    e += d * d;
                }
            if (e < best) { best = e; bv = v; }
        }
        for (int q = 0; q < nb; q++) data[b + q] = (u8)(bv >> (8 * q));
        for (int q = 0; q < NB; q++) { bits[b0 + q] = (bv >> q) & 1; pm[b0 + q] = bits[b0 + q] * 2.0 - 1; }
        b += nb - 1;
    }
}

MlseDecoder::MlseDecoder(const MlseModel &m) : M(m), pre_(make_pre(m)) {}

static std::vector<double> offsets_range(double lo, double hi, double step) {
    std::vector<double> v;
    int n = (int)ceil((hi + 1e-9 - lo) / step);
    for (int k = 0; k < n; k++) v.push_back(lo + k * step);
    return v;
}

MlseResult MlseDecoder::decode(const float *line, int len, double lo, double hi, double step) {
    const Pre &P = *pre_;
    auto y = tpl::norm(line, len);
    int NT = P.NT, K1 = P.K1;
    std::vector<double> Y(NT * MLSE_NP), C;
    std::vector<double> prepm(24); for (int i = 0; i < 24; i++) prepm[i] = P.preb[i] * 2.0 - 1;
    std::vector<std::pair<double, double>> sc;
    for (double off : offsets_range(lo, hi, step)) {
        sample(y.data(), len, off, M.spb, NT, Y.data());
        double r;
        if (prior.empty()) r = fit(P, Y.data(), prepm.data(), 24, M.L, 24 - M.R, nullptr, 0, C);
        else {
            r = 0;
            for (int i = 0; i < 24; i++) for (int p = 0; p < MLSE_NP; p++) {
                double v = 0; for (int k = 0; k < K1; k++) v += P.PRE_X[i * K1 + k] * prior[p * K1 + k];
                double d = Y[i * MLSE_NP + p] - v; r += d * d;
            }
            r /= 24 * MLSE_NP;
        }
        sc.push_back({r, off});
    }
    std::sort(sc.begin(), sc.end());
    double best = 1e300, boff = 0; std::vector<u8> bbits(NT), bits(NT); std::vector<double> bC;
    std::vector<double> pm(NT);
    for (int t = 0; t < std::min<int>(M.ntop, (int)sc.size()); t++) {
        double off = sc[t].second;
        sample(y.data(), len, off, M.spb, NT, Y.data());
        fit(P, Y.data(), prepm.data(), 24, M.L, 24 - M.R, &prior, 2.0, C);
        double met = viterbi_cpu(P, Y.data(), C.data(), bits.data());
        for (int r = 0; r < M.refits; r++) {
            for (int i = 0; i < NT; i++) pm[i] = bits[i] * 2.0 - 1;
            fit(P, Y.data(), pm.data(), NT, M.L, NT - M.R, nullptr, 0, C);
            met = viterbi_cpu(P, Y.data(), C.data(), bits.data());
        }
        if (met < best) { best = met; boff = off; bbits = bits; bC = C; }
    }
    MlseResult res;
    res.data.resize(M.nbytes);
    bytes_lsb(&bbits[24], M.nbytes, res.data.data());
    res.off = boff; res.met = best / NT;
    res.good = M.good ? M.good(res.data.data()) : false;
    if (res.good) {
        n++; double w = 1.0 / std::min(n, 200);
        if (prior.empty()) prior = bC;
        else for (size_t i = 0; i < prior.size(); i++) prior[i] = (1 - w) * prior[i] + w * bC[i];
    }
    if (M.code && !bC.empty()) {   // после проверки годности: ограничение делает любой адрес «верным»
        sample(y.data(), len, boff, M.spb, NT, Y.data());
        refine_bytes(P, M, Y.data(), bC.data(), bbits.data());
        bytes_lsb(&bbits[24], M.nbytes, res.data.data());
    }
    return res;
}

// ---------------------------------------------------------------- пакетный режим
static const char *BATCH_KERNEL = R"CL(
#define SPT (NS / WG)
#define NW (NS / 32)
__kernel void viterbi(__global const float *Y, __global const float *C,
                      __global const int *km, __global const int *kv,
                      __global uint *back, __global float *out_met, __global uchar *out_bits)
{
    const int t = get_group_id(0), l = get_local_id(0);
    __local float met[2][NS]; __local uint bw[NW];
    __local float rmin[WG]; __local int rarg[WG];
    __global const float *c = C + (size_t)t * NP * (NTAP + 1);
    __global const float *y = Y + (size_t)t * NT * NP;
    float ct[NP][NTAP + 1];
    for (int p = 0; p < NP; p++) for (int k = 0; k <= NTAP; k++) ct[p][k] = c[p * (NTAP + 1) + k];
    int cur = 0;
    for (int i = 0; i < NT; i++) {
        for (int w = l; w < NW; w += WG) bw[w] = 0;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int j = 0; j < SPT; j++) {
            int s = l + j * WG;
            float bm = 0.0f;
            for (int p = 0; p < NP; p++) {
                float v = ct[p][NTAP];
                for (int k = 0; k < NTAP; k++) v += (((s >> (NTAP - 1 - k)) & 1) ? 1.0f : -1.0f) * ct[p][k];
                float d = y[i * NP + p] - v; bm += d * d;
            }
            if ((s & km[i]) != kv[i]) bm = 1e9f;
            if (i == 0) met[0][s] = bm;
            else {
                float m0 = met[cur][s >> 1], m1 = met[cur][(s >> 1) | (NS >> 1)];
                int ch = m1 < m0;
                met[cur ^ 1][s] = (ch ? m1 : m0) + bm;
                if (ch) atomic_or(&bw[s >> 5], 1u << (s & 31));
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        if (i > 0) {
            for (int w = l; w < NW; w += WG) back[((size_t)t * NT + i) * NW + w] = bw[w];
            cur ^= 1;
        }
    }
    float bmv = 3.4e38f; int ba = 0;
    for (int j = 0; j < SPT; j++) { int s = l + j * WG; float v = met[cur][s]; if (v < bmv) { bmv = v; ba = s; } }
    rmin[l] = bmv; rarg[l] = ba;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int h = WG / 2; h > 0; h >>= 1) {
        if (l < h && (rmin[l + h] < rmin[l] || (rmin[l + h] == rmin[l] && rarg[l + h] < rarg[l]))) { rmin[l] = rmin[l + h]; rarg[l] = rarg[l + h]; }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (l == 0) {
        __global uchar *o = out_bits + (size_t)t * (NT + RR);
        int q = rarg[0];
        out_met[t] = rmin[0];
        for (int i = NT - 1; i > 0; i--) {
            o[i + RR] = q & 1;
            uint w = back[((size_t)t * NT + i) * NW + (q >> 5)];
            q = (q >> 1) | (((w >> (q & 31)) & 1) ? (1 << (NTAP - 1)) : 0);
        }
        for (int k = 0; k <= RR; k++) o[k] = (q >> (RR - k)) & 1;
    }
}
)CL";


// перерешение байтов (refine_bytes) на видеокарте: группа на строку, кандидаты байта — по потокам.
// Тот же порядок операций в double и без FMA — результат совпадает с процессорным.
static const char *REFINE_KERNEL = R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF
#define K1 (NTAP + 1)
int wst_code(__local const uchar *p, int i, __global const int *hd) {
    if (i < 2) return 1;
    int a = hd[p[0]], b = hd[p[1]];
    if (a < 0 || b < 0) return 0;
    int row = (a >> 3) | (b << 1);
    if (row == 0) return i < 10 ? 1 : 2;
    if (row <= 25) return 2;
    if (row == 27) return i < 40 ? 1 : 0;
    if (row >= 26 && row <= 29) return i == 2 ? 1 : 0;
    return 0;
}
__kernel void refine(__global const double *Y, __global const float *C, __global const int *sel, __global uchar *bits,
                     __global const int *ham, __global const int *par, __global const int *hd)
{
    const int r = get_group_id(0), l = get_local_id(0), t = sel[r];
    __global const double *y = Y + (size_t)t * NT * NP;
    __local double c[NP * K1], pm[NT], fixed[(16 + LL + RR) * NP], re[WG];
    __local uchar bt[NT], data[NBYTES];
    __local int ri[WG];
    for (int i = l; i < NP * K1; i += WG) c[i] = (double)C[(size_t)t * NP * K1 + i];
    for (int i = l; i < NT; i += WG) { bt[i] = bits[(size_t)t * NT + i]; pm[i] = bt[i] * 2.0 - 1; }
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int b = l; b < NBYTES; b += WG) { int v = 0; for (int q = 0; q < 8; q++) v |= bt[24 + b * 8 + q] << q; data[b] = (uchar)v; }
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int b = 0; b < NBYTES; b++) {
        int code = wst_code(data, b, hd);
        if (!code) continue;
        int two = b == 0 && code == 1 && NBYTES > 1 && wst_code(data, 1, hd) == 1;
        int nb = two ? 2 : 1, NB = 8 * nb, ncand = two ? 256 : code == 1 ? 16 : 128;
        int b0 = 24 + b * 8, lo = max(0, b0 - RR), hi = min(NT, b0 + NB + LL);
        for (int idx = l; idx < (hi - lo) * NP; idx += WG) {
            int i = lo + idx / NP, p = idx % NP;
            double v = c[p * K1 + NTAP];
            for (int k = 0; k < NTAP; k++) { int j = i - LL + k; if (j >= 0 && j < NT && (j < b0 || j >= b0 + NB)) v += pm[j] * c[p * K1 + k]; }
            fixed[(i - lo) * NP + p] = y[i * NP + p] - v;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        double best = 1e300; int bi = 1 << 30;
        for (int ci = l; ci < ncand; ci += WG) {
            int v = two ? (ham[ci >> 4] | (ham[ci & 15] << 8)) : code == 1 ? ham[ci] : par[ci];
            double e = 0;
            for (int i = lo; i < hi; i++)
                for (int p = 0; p < NP; p++) {
                    double d = fixed[(i - lo) * NP + p];
                    for (int q = 0; q < NB; q++) { int k = b0 + q - i + LL; if (k >= 0 && k < NTAP) d -= (((v >> q) & 1) ? 1.0 : -1.0) * c[p * K1 + k]; }
                    e += d * d;
                }
            if (e < best) { best = e; bi = ci; }
        }
        re[l] = best; ri[l] = bi;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (int h = WG / 2; h > 0; h >>= 1) {
            if (l < h && (re[l + h] < re[l] || (re[l + h] == re[l] && ri[l + h] < ri[l]))) { re[l] = re[l + h]; ri[l] = ri[l + h]; }
            barrier(CLK_LOCAL_MEM_FENCE);
        }
        if (l == 0) {
            int ci = ri[0];
            int bv = two ? (ham[ci >> 4] | (ham[ci & 15] << 8)) : code == 1 ? ham[ci] : par[ci];
            for (int q = 0; q < nb; q++) data[b + q] = (uchar)(bv >> (8 * q));
            for (int q = 0; q < NB; q++) { bt[b0 + q] = (bv >> q) & 1; pm[b0 + q] = bt[b0 + q] * 2.0 - 1; }
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        b += nb - 1;
    }
    for (int i = l; i < NT; i += WG) bits[(size_t)t * NT + i] = bt[i];
}
)CL";

struct MlseBatch::Impl {
    std::shared_ptr<MlseDecoder::Pre> P;
    std::unique_ptr<gpu::Kernel> k;
    gpu::Buffer km, kv;
    int WG = 0;
    std::unique_ptr<gpu::Kernel> kr;          // перерешение байтов (только code_kind == 1)
    gpu::Buffer ham, par, hd;
};

MlseBatch::MlseBatch(const MlseModel &m, const std::vector<double> &pr0, int n_prior, bool use_gpu, Progress &pr)
    : prior(pr0), n(n_prior), M(m), p(new Impl) {
    p->P = make_pre(m);
    int NS = m.NS(), NT = m.NT();
    if (use_gpu && gpu::available()) {
        try {
            p->WG = (int)std::min<size_t>(std::min(NS, 256), gpu::max_group());
            for (;;) {
                std::string defs = fmt("#define NS %d\n#define WG %d\n#define NTAP %d\n#define NT %d\n#define NP %d\n#define RR %d\n",
                                       NS, p->WG, m.NTAP(), NT, MLSE_NP, m.R);
                p->k.reset(new gpu::Kernel(defs + BATCH_KERNEL, "viterbi"));
                if ((int)p->k->group_limit() >= p->WG || p->WG <= 32) break;
                p->WG /= 2;                                    // ядру не хватает регистров на столько потоков
            }
            p->km = gpu::Buffer(NT * 4, p->P->km.data(), true);
            p->kv = gpu::Buffer(NT * 4, p->P->kv.data(), true);
            pr.log("reading on the GPU: " + gpu::device_name());
            if (m.code && m.code_kind == 1)
                try {
                    std::string d2 = fmt("#define NT %d\n#define NP %d\n#define NTAP %d\n#define LL %d\n#define RR %d\n#define NBYTES %d\n#define WG 128\n",
                                         NT, MLSE_NP, m.NTAP(), m.L, m.R, m.nbytes);
                    p->kr.reset(new gpu::Kernel(d2 + REFINE_KERNEL, "refine"));
                    if (p->kr->group_limit() < 128) throw std::runtime_error("too few threads per group");
                    std::vector<int> H, Pa, D(256);
                    for (int v = 0; v < 16; v++) H.push_back(ham::CODEWORDS[v]);
                    for (int v = 0; v < 256; v++) { if (odd_parity(v)) Pa.push_back(v); D[v] = ham::dec[v]; }
                    p->ham = gpu::Buffer(H.size() * 4, H.data(), true);
                    p->par = gpu::Buffer(Pa.size() * 4, Pa.data(), true);
                    p->hd = gpu::Buffer(D.size() * 4, D.data(), true);
                } catch (std::exception &e) { p->kr.reset(); pr.log(std::string("byte re-decision stays on the CPU (") + e.what() + ")"); }
        } catch (std::exception &e) {
            p->k.reset();
            pr.log(std::string("GPU unavailable (") + e.what() + ") \xE2\x80\x94 reading on the CPU");
        }
    }
    size_t per_task = (size_t)NT * NS / 8;
    batch_ = p->k ? std::max<size_t>(64, std::min<size_t>(4096, (1ull << 28) / per_task)) : (NS >= 2048 ? 64 : 256);
    if (p->k) {
        // самый большой буфер — обратные ссылки Витерби: KT строк-кандидатов на пачку; не больше, чем видеокарта даёт в одном буфере
        size_t back_per_row = (size_t)std::max(1, m.ntop) * NT * (NS / 32) * 4, lim = gpu::max_alloc() * 9 / 10;
        size_t b0 = batch_;
        while (batch_ > 16 && batch_ * back_per_row > lim) batch_ /= 2;
        if (batch_ != b0) pr.log(fmt("graphics card memory: reading in batches of %zu lines instead of %zu", batch_, b0));
    }
}
MlseBatch::~MlseBatch() { delete p; }
bool MlseBatch::on_gpu() const { return (bool)p->k; }

std::vector<MlseResult> MlseBatch::decode(const std::vector<const float *> &rows, int len, double lo, double hi,
                                          const std::vector<char> *learn, double step) {
    const auto &P = *p->P;
    int NT = P.NT, K1 = P.K1, nr = (int)rows.size();
    std::vector<MlseResult> out(nr);
    if (nr == 0) return out;
    auto offs = offsets_range(lo, hi, step);
    std::vector<double> Pp(24 * MLSE_NP);
    for (int i = 0; i < 24; i++) for (int q = 0; q < MLSE_NP; q++) {
        double v = 0; for (int k = 0; k < K1; k++) v += P.PRE_X[i * K1 + k] * prior[q * K1 + k];
        Pp[i * MLSE_NP + q] = v;
    }
    const int KT = std::max(1, M.ntop);       // смещений-кандидатов на строку
    int nt = KT * nr;
    std::vector<double> cand(nt);
    std::vector<float> Yf((size_t)nt * NT * MLSE_NP), Cf((size_t)nt * MLSE_NP * K1);
    std::vector<std::vector<double>> Ys(nt);
    // 1) смещения и отсчёты; 2) модель по преамбуле с притяжением к априорной
    parallel_for(nr, [&](size_t r) {
        auto y = tpl::norm(rows[r], len);
        std::vector<std::pair<double, int>> sc;
        for (size_t q = 0; q < offs.size(); q++) {
            double s = 0;
            for (int i = 0; i < 24; i++) for (int ph = 0; ph < MLSE_NP; ph++) {
                double d = interp(offs[q] + (i + PH[ph]) * M.spb, y.data(), len) - Pp[i * MLSE_NP + ph]; s += d * d;
            }
            sc.push_back({s / (24 * MLSE_NP), (int)q});
        }
        std::stable_sort(sc.begin(), sc.end(), [](auto &a, auto &b) { return a.first < b.first; });
        for (int c = 0; c < KT; c++) {
            int t = (int)r * KT + c;
            cand[t] = offs[sc[std::min<size_t>(c, sc.size() - 1)].second];
            Ys[t].resize(NT * MLSE_NP);
            sample(y.data(), len, cand[t], M.spb, NT, Ys[t].data());
            std::vector<double> B(K1 * MLSE_NP, 0);
            int npi = 24 - M.L - M.R;
            for (int qi = 0; qi < npi; qi++)
                for (int k = 0; k < K1; k++)
                    for (int ph = 0; ph < MLSE_NP; ph++) B[k * MLSE_NP + ph] += P.PRE_XP[qi * K1 + k] * Ys[t][(M.L + qi) * MLSE_NP + ph];
            for (int k = 0; k < K1; k++) for (int ph = 0; ph < MLSE_NP; ph++) B[k * MLSE_NP + ph] += 2.0 * prior[ph * K1 + k];
            solve(P.PRE_A, B, K1, MLSE_NP);
            for (int k = 0; k < K1; k++) for (int ph = 0; ph < MLSE_NP; ph++) Cf[(size_t)t * MLSE_NP * K1 + ph * K1 + k] = (float)B[k * MLSE_NP + ph];
            for (int i = 0; i < NT * MLSE_NP; i++) Yf[(size_t)t * NT * MLSE_NP + i] = (float)Ys[t][i];
        }
    });
    std::vector<u8> bits((size_t)nt * NT);
    std::vector<double> met(nt);
    auto vit = [&]() {
        if (p->k) try {
            gpu::Buffer bY(Yf.size() * 4, Yf.data(), true), bC(Cf.size() * 4, Cf.data(), true);
            gpu::Buffer bback((size_t)nt * NT * (P.NS / 32) * 4), bmet(nt * 4), bbits((size_t)nt * (NT + M.R));
            auto &k = *p->k;
            k.arg(0, bY); k.arg(1, bC); k.arg(2, p->km); k.arg(3, p->kv); k.arg(4, bback); k.arg(5, bmet); k.arg(6, bbits);
            k.run((size_t)nt * p->WG, p->WG);
            std::vector<float> mf(nt); bmet.read(mf.data(), nt * 4);
            std::vector<u8> bb((size_t)nt * (NT + M.R)); bbits.read(bb.data(), bb.size());
            for (int t = 0; t < nt; t++) { met[t] = mf[t]; memcpy(&bits[(size_t)t * NT], &bb[(size_t)t * (NT + M.R)], NT); }
            return;
        } catch (std::exception &) { p->k.reset(); }      // мало памяти видеокарты — дальше на процессоре
        {
            parallel_for(nt, [&](size_t t) {
                std::vector<double> C(Cf.begin() + t * MLSE_NP * K1, Cf.begin() + (t + 1) * MLSE_NP * K1);
                met[t] = viterbi_cpu(P, Ys[t].data(), C.data(), &bits[t * NT]);
            });
        }
    };
    vit();
    // 3) модель по всему пакету, Витерби ещё раз (M.refits раз)
    for (int rf = 0; rf < std::max(1, M.refits); rf++) {
    parallel_for(nt, [&](size_t t) {
        std::vector<double> pm(NT), C;
        for (int i = 0; i < NT; i++) pm[i] = bits[t * NT + i] * 2.0 - 1;
        int a = M.L, b = NT - M.R, r = b - a;
        std::vector<double> A(K1 * K1, 0), B(K1 * MLSE_NP, 0), x(K1);
        for (int q = 0; q < r; q++) {
            design_row(pm.data(), NT, a + q, M.L, M.R, x.data());
            for (int i = 0; i < K1; i++) {
                for (int j = 0; j < K1; j++) A[i * K1 + j] += x[i] * x[j];
                for (int ph = 0; ph < MLSE_NP; ph++) B[i * MLSE_NP + ph] += x[i] * Ys[t][(a + q) * MLSE_NP + ph];
            }
        }
        for (int i = 0; i < K1; i++) A[i * K1 + i] += 1e-6;
        solve(A, B, K1, MLSE_NP);
        for (int k = 0; k < K1; k++) for (int ph = 0; ph < MLSE_NP; ph++) Cf[t * MLSE_NP * K1 + ph * K1 + k] = (float)B[k * MLSE_NP + ph];
    });
    vit();
    }
    auto pick = [&](int r) { int b = 0; for (int c = 1; c < KT; c++) if (met[r * KT + c] < met[r * KT + b]) b = c; return r * KT + b; };
    for (int r = 0; r < nr; r++) {
        int t = pick(r);
        MlseResult &res = out[r];
        res.data.resize(M.nbytes);
        bytes_lsb(&bits[(size_t)t * NT + 24], M.nbytes, res.data.data());
        res.off = cand[t]; res.met = met[t] / NT;
        res.good = M.good ? M.good(res.data.data()) : false;
        if (res.good && (!learn || (*learn)[r])) {
            n++; double w = 1.0 / std::min(n, 200);
            for (int i = 0; i < MLSE_NP * K1; i++) prior[i] = (1 - w) * prior[i] + w * Cf[(size_t)t * MLSE_NP * K1 + i];
        }
    }
    if (M.code && p->kr && p->k && !getenv("TR_CPU_REFINE")) {
        try {
            std::vector<int> sel(nr);
            for (int r = 0; r < nr; r++) sel[r] = pick(r);
            std::vector<double> Yd((size_t)nt * NT * MLSE_NP);
            for (int t = 0; t < nt; t++) memcpy(&Yd[(size_t)t * NT * MLSE_NP], Ys[t].data(), NT * MLSE_NP * 8);
            gpu::Buffer bY(Yd.size() * 8, Yd.data(), true), bC(Cf.size() * 4, Cf.data(), true), bs(nr * 4, sel.data(), true);
            gpu::Buffer bb(bits.size(), bits.data());
            auto &k = *p->kr;
            k.arg(0, bY); k.arg(1, bC); k.arg(2, bs); k.arg(3, bb); k.arg(4, p->ham); k.arg(5, p->par); k.arg(6, p->hd);
            k.run((size_t)nr * 128, 128);
            bb.read(bits.data(), bits.size());
            for (int r = 0; r < nr; r++) bytes_lsb(&bits[(size_t)sel[r] * NT + 24], M.nbytes, out[r].data.data());
            return out;
        } catch (std::exception &) { p->kr.reset(); }      // не вышло — как раньше, на процессоре
    }
    if (M.code)
        parallel_for(nr, [&](size_t r) {
            MlseResult &res = out[r];
            int t = pick((int)r);
            std::vector<double> C(Cf.begin() + (size_t)t * MLSE_NP * K1, Cf.begin() + (size_t)(t + 1) * MLSE_NP * K1);
            refine_bytes(P, M, Ys[t].data(), C.data(), &bits[(size_t)t * NT]);
            bytes_lsb(&bits[(size_t)t * NT + 24], M.nbytes, res.data.data());
        });
    return out;
}
