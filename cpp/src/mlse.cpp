#include "mlse.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mlse {

namespace {

const double PH[NP] = {0.3, 0.5, 0.7};
const uint8_t kHam[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
                          0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};

double interp(double x, const float* y, int n)
{
    if (x < 0 || x > n - 1) return 0;
    int i = (int)x;
    if (i >= n - 1) return y[n - 1];
    double f = x - i;
    return y[i] * (1 - f) + y[i + 1] * f;
}

bool solve(std::vector<double> A, std::vector<double>& B, int n, int m)
{
    for (int c = 0; c < n; c++) {
        int p = c;
        double best = std::fabs(A[c * n + c]);
        for (int r = c + 1; r < n; r++)
            if (std::fabs(A[r * n + c]) > best) { best = std::fabs(A[r * n + c]); p = r; }
        if (best < 1e-300) return false;
        if (p != c) {
            for (int k = 0; k < n; k++) std::swap(A[c * n + k], A[p * n + k]);
            for (int k = 0; k < m; k++) std::swap(B[c * m + k], B[p * m + k]);
        }
        for (int r = 0; r < n; r++) {
            if (r == c) continue;
            double f = A[r * n + c] / A[c * n + c];
            if (f == 0) continue;
            for (int k = c; k < n; k++) A[r * n + k] -= f * A[c * n + k];
            for (int k = 0; k < m; k++) B[r * m + k] -= f * B[c * m + k];
        }
    }
    for (int r = 0; r < n; r++)
        for (int k = 0; k < m; k++) B[r * m + k] /= A[r * n + r];
    return true;
}

// regression row: neighbour bits (+-1, 0 outside the packet) and one
void designRow(const double* pm, int nbits, int i, int L, int R, double* out)
{
    int c = 0;
    for (int d = -L; d <= R; d++, c++) {
        int k = i + d;
        out[c] = (k >= 0 && k < nbits) ? pm[k] : 0.0;
    }
    out[c] = 1.0;
}

void sample(const float* y, int n, double off, double spb, int NT, double* Y)
{
    for (int i = 0; i < NT; i++)
        for (int p = 0; p < NP; p++) Y[i * NP + p] = interp(off + (i + PH[p]) * spb, y, n);
}

std::vector<double> offsets(double lo, double hi, double step)
{
    std::vector<double> v;
    int n = (int)std::ceil((hi + 1e-9 - lo) / step);
    for (int k = 0; k < n; k++) v.push_back(lo + k * step);
    return v;
}

} // namespace

struct Pre {
    int NT, NTAP, NS, L, R, K1;
    std::vector<uint8_t> preb;
    std::vector<double> prepm;   // 24, +-1
    std::vector<double> PRE_X;   // 24 x K1
    std::vector<double> PRE_XP;  // (24-L-R) x K1
    std::vector<double> PRE_A;   // K1 x K1
    std::vector<int> km, kv;     // known bits per Viterbi step
};

static std::shared_ptr<Pre> makePre(const Model& m)
{
    auto p = std::make_shared<Pre>();
    p->NT = m.NT(); p->NTAP = m.NTAP(); p->NS = m.NS(); p->L = m.L; p->R = m.R; p->K1 = p->NTAP + 1;
    for (int i = 0; i < 24; i++) p->preb.push_back((m.pre[i / 8] >> (i % 8)) & 1);
    for (int i = 0; i < 24; i++) p->prepm.push_back(p->preb[i] * 2.0 - 1);
    p->PRE_X.resize(24 * p->K1);
    for (int i = 0; i < 24; i++) designRow(p->prepm.data(), 24, i, m.L, m.R, &p->PRE_X[i * p->K1]);
    int npi = 24 - m.L - m.R;
    p->PRE_XP.resize(npi * p->K1);
    for (int q = 0; q < npi; q++) designRow(p->prepm.data(), 24, m.L + q, m.L, m.R, &p->PRE_XP[q * p->K1]);
    p->PRE_A.assign(p->K1 * p->K1, 0);
    for (int q = 0; q < npi; q++)
        for (int a = 0; a < p->K1; a++)
            for (int b = 0; b < p->K1; b++) p->PRE_A[a * p->K1 + b] += p->PRE_XP[q * p->K1 + a] * p->PRE_XP[q * p->K1 + b];
    for (int a = 0; a < p->K1; a++) p->PRE_A[a * p->K1 + a] += 2.0;
    p->km.assign(p->NT, 0);
    p->kv.assign(p->NT, 0);
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

// least squares on rows [a, b); prior (NP x K1) pulls the fit with weight lam
static void fit(const Pre& P, const double* Y, const double* pm, int nbits, int a, int b, const std::vector<double>* prior,
                double lam, std::vector<double>& C)
{
    int K1 = P.K1;
    std::vector<double> A(K1 * K1, 0), B(K1 * NP, 0), x(K1);
    for (int q = a; q < b; q++) {
        designRow(pm, nbits, q, P.L, P.R, x.data());
        for (int i = 0; i < K1; i++) {
            if (x[i] == 0) continue;
            for (int j = 0; j < K1; j++) A[i * K1 + j] += x[i] * x[j];
            for (int p = 0; p < NP; p++) B[i * NP + p] += x[i] * Y[q * NP + p];
        }
    }
    if (prior && !prior->empty() && lam > 0) {
        for (int i = 0; i < K1; i++) A[i * K1 + i] += lam;
        for (int i = 0; i < K1; i++)
            for (int p = 0; p < NP; p++) B[i * NP + p] += lam * (*prior)[p * K1 + i];
    } else {
        double tr = 0;
        for (int i = 0; i < K1; i++) tr += A[i * K1 + i];
        for (int i = 0; i < K1; i++) A[i * K1 + i] += 1e-6 * (tr / K1 + 1e-30);
    }
    if (!solve(A, B, K1, NP)) std::fill(B.begin(), B.end(), 0.0);
    C.assign(NP * K1, 0);
    for (int i = 0; i < K1; i++)
        for (int p = 0; p < NP; p++) C[p * K1 + i] = B[i * NP + p];
}

static double viterbi(const Pre& P, const double* Y, const double* C, uint8_t* bits)
{
    const int NS = P.NS, NTAP = P.NTAP, K1 = P.K1, NT = P.NT, R = P.R;
    std::vector<float> pred((size_t)NS * NP);
    for (int s = 0; s < NS; s++)
        for (int p = 0; p < NP; p++) {
            double v = C[p * K1 + NTAP];
            for (int k = 0; k < NTAP; k++) v += (((s >> (NTAP - 1 - k)) & 1) ? 1.0 : -1.0) * C[p * K1 + k];
            pred[s * NP + p] = (float)v;
        }
    const int NW = (NS + 63) / 64;
    std::vector<float> met(NS), nm(NS), bm(NS);
    std::vector<uint64_t> back((size_t)NT * NW, 0);
    const int hs = NS >> 1;
    for (int i = 0; i < NT; i++) {
        float y0 = (float)Y[i * 3], y1 = (float)Y[i * 3 + 1], y2 = (float)Y[i * 3 + 2];
        int km = P.km[i], kv = P.kv[i];
        for (int s = 0; s < NS; s++) {
            if ((s & km) != kv) { bm[s] = 1e9f; continue; }
            const float* q = &pred[s * 3];
            float a = y0 - q[0], b = y1 - q[1], c = y2 - q[2];
            bm[s] = a * a + b * b + c * c;
        }
        if (i == 0) { met = bm; continue; }
        uint64_t* bw = &back[(size_t)i * NW];
        for (int s = 0; s < NS; s++) {
            float m0 = met[s >> 1], m1 = met[(s >> 1) | hs];
            bool ch = m1 < m0;
            nm[s] = (ch ? m1 : m0) + bm[s];
            if (ch) bw[s >> 6] |= 1ull << (s & 63);
        }
        met.swap(nm);
    }
    int s = 0;
    for (int q = 1; q < NS; q++)
        if (met[q] < met[s]) s = q;
    double best = met[s];
    std::vector<uint8_t> out(NT + R, 0);
    for (int i = NT - 1; i > 0; i--) {
        out[i + R] = s & 1;
        bool ch = (back[(size_t)i * NW + (s >> 6)] >> (s & 63)) & 1;
        s = (s >> 1) | (ch ? (1 << (NTAP - 1)) : 0);
    }
    for (int k = 0; k <= R; k++) out[k] = (s >> (R - k)) & 1;
    memcpy(bits, out.data(), NT);
    return best;
}

static void bytesLsb(const uint8_t* bits, int nbytes, uint8_t* out)
{
    for (int i = 0; i < nbytes; i++) {
        int v = 0;
        for (int k = 0; k < 8; k++) v |= (bits[i * 8 + k] & 1) << k;
        out[i] = (uint8_t)v;
    }
}

// bytes with a code are re-decided among valid values; other bits stay
static void refineBytes(const Pre& P, const Model& M, const double* Y, const double* C, uint8_t* bits)
{
    static std::vector<int> HAM, PAR, PAIR;
    static std::once_flag once;
    std::call_once(once, [] {
        for (int v = 0; v < 16; v++) HAM.push_back(kHam[v]);
        for (int v = 0; v < 256; v++)
            if (oddParity((uint8_t)v)) PAR.push_back(v);
        for (int x : HAM)
            for (int y : HAM) PAIR.push_back(x | (y << 8));
    });
    const int NT = P.NT, K1 = P.K1, L = P.L, R = P.R, NTAP = P.NTAP;
    std::vector<double> pm(NT);
    for (int i = 0; i < NT; i++) pm[i] = bits[i] * 2.0 - 1;
    std::vector<uint8_t> data(M.nbytes);
    bytesLsb(&bits[24], M.nbytes, data.data());
    std::vector<double> fixed;
    for (int b = 0; b < M.nbytes; b++) {
        int code = M.code(data.data(), b);
        if (!code) continue;
        // the two address bytes go together (16 x 16): an error in one drags the other
        bool two = b == 0 && code == 1 && M.nbytes > 1 && M.code(data.data(), 1) == 1;
        int nb = two ? 2 : 1, NB = 8 * nb;
        const std::vector<int>* cand = two ? &PAIR : code == 1 ? &HAM : &PAR;
        int b0 = 24 + b * 8, lo = std::max(0, b0 - R), hi = std::min(NT, b0 + NB + L);
        fixed.assign((hi - lo) * NP, 0);
        for (int i = lo; i < hi; i++)
            for (int p = 0; p < NP; p++) {
                double v = C[p * K1 + NTAP];
                for (int k = 0; k < NTAP; k++) {
                    int j = i - L + k;
                    if (j >= 0 && j < NT && (j < b0 || j >= b0 + NB)) v += pm[j] * C[p * K1 + k];
                }
                fixed[(i - lo) * NP + p] = Y[i * NP + p] - v;
            }
        double best = 1e300;
        int bv = data[b] | (two ? data[1] << 8 : 0);
        for (int v : *cand) {
            double e = 0;
            for (int i = lo; i < hi && e < best; i++)
                for (int p = 0; p < NP; p++) {
                    double d = fixed[(i - lo) * NP + p];
                    for (int q = 0; q < NB; q++) {
                        int k = b0 + q - i + L;
                        if (k >= 0 && k < NTAP) d -= (((v >> q) & 1) ? 1.0 : -1.0) * C[p * K1 + k];
                    }
                    e += d * d;
                }
            if (e < best) { best = e; bv = v; }
        }
        for (int q = 0; q < nb; q++) data[b + q] = (uint8_t)(bv >> (8 * q));
        for (int q = 0; q < NB; q++) { bits[b0 + q] = (bv >> q) & 1; pm[b0 + q] = bits[b0 + q] * 2.0 - 1; }
        b += nb - 1;
    }
}

Engine::Engine(const Model& m) : M(m), pre_(makePre(m)) {}

Result Engine::decodeLearn(const float* y, int len, double lo, double hi, double step)
{
    const Pre& P = *pre_;
    const int NT = P.NT, K1 = P.K1;
    std::vector<double> Y(NT * NP), C;
    std::vector<std::pair<double, double>> sc;
    for (double off : offsets(lo, hi, step)) {
        sample(y, len, off, M.spb, NT, Y.data());
        double r = 0;
        if (prior.empty()) {
            fit(P, Y.data(), P.prepm.data(), 24, M.L, 24 - M.R, nullptr, 0, C);
            for (int i = M.L; i < 24 - M.R; i++)
                for (int p = 0; p < NP; p++) {
                    double v = 0;
                    for (int k = 0; k < K1; k++) v += P.PRE_X[i * K1 + k] * C[p * K1 + k];
                    double d = Y[i * NP + p] - v;
                    r += d * d;
                }
        } else
            for (int i = 0; i < 24; i++)
                for (int p = 0; p < NP; p++) {
                    double v = 0;
                    for (int k = 0; k < K1; k++) v += P.PRE_X[i * K1 + k] * prior[p * K1 + k];
                    double d = Y[i * NP + p] - v;
                    r += d * d;
                }
        sc.push_back({r, off});
    }
    std::sort(sc.begin(), sc.end());
    double best = 1e300, boff = 0;
    std::vector<uint8_t> bbits(NT), bits(NT);
    std::vector<double> bC, pm(NT);
    for (int t = 0; t < std::min<int>(M.ntop, (int)sc.size()); t++) {
        double off = sc[t].second;
        sample(y, len, off, M.spb, NT, Y.data());
        fit(P, Y.data(), P.prepm.data(), 24, M.L, 24 - M.R, &prior, 2.0, C);
        double met = viterbi(P, Y.data(), C.data(), bits.data());
        for (int r = 0; r < M.refits; r++) {
            for (int i = 0; i < NT; i++) pm[i] = bits[i] * 2.0 - 1;
            fit(P, Y.data(), pm.data(), NT, M.L, NT - M.R, nullptr, 0, C);
            met = viterbi(P, Y.data(), C.data(), bits.data());
        }
        if (met < best) { best = met; boff = off; bbits = bits; bC = C; }
    }
    Result res;
    res.data.resize(M.nbytes);
    bytesLsb(&bbits[24], M.nbytes, res.data.data());
    res.off = boff;
    res.met = best / NT;
    res.good = M.good ? M.good(res.data.data()) : false;
    res.C = bC;
    learn(res);
    if (M.code && !bC.empty()) { // after the check: the constraint makes any address "valid"
        sample(y, len, boff, M.spb, NT, Y.data());
        refineBytes(P, M, Y.data(), bC.data(), bbits.data());
        bytesLsb(&bbits[24], M.nbytes, res.data.data());
    }
    return res;
}

Result Engine::decode(const float* y, int len, double lo, double hi, double step) const
{
    const Pre& P = *pre_;
    const int NT = P.NT, K1 = P.K1;
    Result res;
    if (prior.empty()) return res;
    // 1) start offsets scored by the preamble against the prior
    double Pp[24 * NP];
    for (int i = 0; i < 24; i++)
        for (int q = 0; q < NP; q++) {
            double v = 0;
            for (int k = 0; k < K1; k++) v += P.PRE_X[i * K1 + k] * prior[q * K1 + k];
            Pp[i * NP + q] = v;
        }
    std::vector<std::pair<double, double>> sc;
    for (double off : offsets(lo, hi, step)) {
        double s = 0;
        for (int i = 0; i < 24; i++)
            for (int ph = 0; ph < NP; ph++) {
                double d = interp(off + (i + PH[ph]) * M.spb, y, len) - Pp[i * NP + ph];
                s += d * d;
            }
        sc.push_back({s, off});
    }
    std::stable_sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first < b.first; });
    const int KT = std::max(1, std::min<int>(M.ntop, (int)sc.size()));
    double best = 1e300;
    std::vector<double> Y(NT * NP), bY, C, bC, pm(NT);
    std::vector<uint8_t> bits(NT), bbits;
    for (int c = 0; c < KT; c++) {
        double off = sc[c].second;
        sample(y, len, off, M.spb, NT, Y.data());
        // 2) model from the preamble, pulled to the prior
        std::vector<double> B(K1 * NP, 0);
        int npi = 24 - M.L - M.R;
        for (int qi = 0; qi < npi; qi++)
            for (int k = 0; k < K1; k++)
                for (int ph = 0; ph < NP; ph++) B[k * NP + ph] += P.PRE_XP[qi * K1 + k] * Y[(M.L + qi) * NP + ph];
        for (int k = 0; k < K1; k++)
            for (int ph = 0; ph < NP; ph++) B[k * NP + ph] += 2.0 * prior[ph * K1 + k];
        solve(P.PRE_A, B, K1, NP);
        C.assign(NP * K1, 0);
        for (int k = 0; k < K1; k++)
            for (int ph = 0; ph < NP; ph++) C[ph * K1 + k] = B[k * NP + ph];
        double met = viterbi(P, Y.data(), C.data(), bits.data());
        // 3) model from the whole packet, Viterbi again
        for (int r = 0; r < std::max(1, M.refits); r++) {
            for (int i = 0; i < NT; i++) pm[i] = bits[i] * 2.0 - 1;
            fit(P, Y.data(), pm.data(), NT, M.L, NT - M.R, nullptr, 0, C);
            met = viterbi(P, Y.data(), C.data(), bits.data());
        }
        if (met < best) { best = met; res.off = off; bbits = bits; bC = C; bY = Y; }
    }
    res.data.resize(M.nbytes);
    bytesLsb(&bbits[24], M.nbytes, res.data.data());
    res.met = best / NT;
    res.good = M.good ? M.good(res.data.data()) : false;
    res.C = bC;
    if (M.code) {
        refineBytes(P, M, bY.data(), bC.data(), bbits.data());
        bytesLsb(&bbits[24], M.nbytes, res.data.data());
    }
    return res;
}

void Engine::learn(const Result& r)
{
    if (!r.good || r.C.empty()) return;
    n_++;
    double w = 1.0 / std::min(n_, 200);
    if (prior.empty()) prior = r.C;
    else
        for (size_t i = 0; i < prior.size(); i++) prior[i] = (1 - w) * prior[i] + w * r.C[i];
}

void normalize(const float* in, int n, float* out)
{
    double s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += in[i]; s2 += (double)in[i] * in[i]; }
    double m = n ? s / n : 0, sd = n ? std::sqrt(std::max(0.0, s2 / n - m * m)) : 1;
    if (sd <= 1e-6) sd = 1;
    for (int i = 0; i < n; i++) out[i] = (float)((in[i] - m) / sd);
}

int hamExact(uint8_t b)
{
    for (int i = 0; i < 16; i++)
        if (kHam[i] == b) return i;
    return -1;
}

bool oddParity(uint8_t b) { return __builtin_popcount(b) & 1; }

int wstCode(const uint8_t* p, int i)
{
    if (i < 2) return 1;
    int a = hamExact(p[0]), b = hamExact(p[1]);
    if (a < 0 || b < 0) return 0;
    int row = (a >> 3) | (b << 1);
    if (row == 0) return i < 10 ? 1 : 2;
    if (row <= 25) return 2;
    if (row == 27) return i < 40 ? 1 : 0;
    if (row >= 26 && row <= 29) return i == 2 ? 1 : 0;
    return 0;
}

} // namespace mlse
