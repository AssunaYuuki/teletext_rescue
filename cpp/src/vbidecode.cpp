#include "vbidecode.h"
#include "opencl.h"
#include "teletext.h"
#include <atomic>
#include <mutex>

void parallel_for(size_t n, const std::function<void(size_t)> &fn, Progress *pr, size_t chunk) {
    unsigned nt = std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    if (n == 0) return;
    if (nt == 1 || n < 2) { for (size_t i = 0; i < n; i++) { if (pr) pr->check(); fn(i); } return; }
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::exception_ptr err;
    std::mutex em;
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++)
        th.emplace_back([&] {
            for (;;) {
                size_t a = next.fetch_add(chunk);
                if (a >= n || stop) return;
                try {
                    if (pr && pr->cancel) throw Cancelled();
                    for (size_t i = a; i < std::min(n, a + chunk); i++) fn(i);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(em);
                    if (!err) err = std::current_exception();
                    stop = true; return;
                }
            }
        });
    for (auto &t : th) t.join();
    if (err) std::rethrow_exception(err);
}

std::vector<u8> bits_lsb(const u8 *b, int n) {
    std::vector<u8> out(n * 8);
    for (int i = 0; i < n; i++) for (int k = 0; k < 8; k++) out[i * 8 + k] = (b[i] >> k) & 1;
    return out;
}
void bytes_lsb(const u8 *bits, int nbytes, u8 *out) {
    for (int i = 0; i < nbytes; i++) { int v = 0; for (int k = 0; k < 8; k++) v |= (bits[i * 8 + k] & 1) << k; out[i] = (u8)v; }
}

namespace tpl {
double SPB = getenv("VBI_SPB") ? atof(getenv("VBI_SPB")) : 5.110;
static const int ZONES[NZ + 1] = {0, 8, 16, 32, 48, 64, 300, 320, 336};
static const u8 PRE[3] = {0x55, 0x55, 0x27};

static std::vector<u8> preb() { return bits_lsb(PRE, 3); }

Templates base_templates() {
    Bytes b = resource_bytes("ZONED_TPL");
    Templates T(b.size() / 8);
    memcpy(T.data(), b.data(), T.size() * 8);
    return T;
}
std::vector<double> channel_h() {
    Bytes b = resource_bytes("CHANNEL_H");
    std::vector<double> h(b.size() / 8);
    memcpy(h.data(), b.data(), h.size() * 8);
    return h;
}
int zone_of(int j) {
    int i = j - 24;
    for (int z = 0; z < NZ; z++) if (i < ZONES[z + 1]) return z;
    return NZ - 1;
}
template <class T>
static std::vector<float> norm_t(const T *line, int n) {
    double s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += line[i]; s2 += (double)line[i] * line[i]; }
    double m = s / n, sd = sqrt(std::max(0.0, s2 / n - m * m));
    if (sd <= 1e-6) sd = 1;
    std::vector<float> y(n);
    for (int i = 0; i < n; i++) y[i] = (float)((line[i] - m) / sd);
    return y;
}
std::vector<float> norm(const u8 *line, int n) { return norm_t(line, n); }
std::vector<float> norm(const float *line, int n) { return norm_t(line, n); }

// известные биты состояния на шаге j (преамбула и пустота после пакета)
struct Known { int km[NTOT], kv[NTOT]; };
static const Known &known() {
    static Known K = [] {
        Known k{}; auto pb = preb();
        for (int j = W; j < NTOT; j++)
            for (int d = -W; d <= W; d++) {
                int kk = j + d;
                if (kk < 24 || kk >= NTOT) {
                    int bit = W - d;
                    k.km[j] |= 1 << bit;
                    if (kk < 24 && pb[kk]) k.kv[j] |= 1 << bit;
                }
            }
        return k;
    }();
    return K;
}

Decoder::Decoder(const Templates &T) { set_templates(T); }
void Decoder::set_templates(const Templates &T) {
    T_ = T; T2_.assign((size_t)NZ * NPH * NS * 2, 0);
    for (size_t q = 0; q < (size_t)NZ * NPH * NS; q++) {
        double c = 0;
        for (int x = 0; x < NX; x++) { c += T_[q * NX + x] * T_[q * NX + x]; if (x == 4) T2_[q * 2] = c; if (x == 5) T2_[q * 2 + 1] = c; }
    }
}

double Decoder::viterbi(const std::vector<float> &y, double off, int j0, int j1, u8 *bits) const {
    const Known &K = known();
    int ny = (int)y.size();
    std::vector<float> met(NS), nm(NS);
    std::vector<uint64_t> back;
    if (bits) back.assign((size_t)(j1 - j0) * (NS / 64), 0);
    float bm[NS];
    for (int j = j0; j < j1; j++) {
        int z = zone_of(j);
        double x0 = off + j * SPB; int xs = (int)ceil(x0);
        int ph = ((int)((xs - x0) * NPH)) % NPH; if (ph < 0) ph += NPH;
        int n = (int)ceil(off + (j + 1) * SPB) - xs;
        double Y[NX]; double ysq = 0;
        for (int k = 0; k < NX; k++) { int i = xs + k; Y[k] = (k < n && i >= 0 && i < ny) ? y[i] : 0.0; ysq += Y[k] * Y[k]; }
        const double *T = &T_[((size_t)(z * NPH + ph) * NS) * NX];
        const double *T2 = &T2_[((size_t)(z * NPH + ph) * NS) * 2];
        int km = K.km[j], kv = K.kv[j];
        int nn = std::min(std::max(n, 5), 6) - 5;
        for (int s = 0; s < NS; s++) {
            if ((s & km) != kv) { bm[s] = 1e9f; continue; }
            const double *t = T + (size_t)s * NX;
            double dot = 0; for (int k = 0; k < NX; k++) dot += Y[k] * t[k];
            bm[s] = (float)(ysq - 2 * dot + T2[s * 2 + nn]);
        }
        if (j == j0) { for (int s = 0; s < NS; s++) met[s] = bm[s]; continue; }
        uint64_t *bw = bits ? &back[(size_t)(j - j0 - 1) * (NS / 64)] : nullptr;
        for (int s = 0; s < NS; s++) {
            float m0 = met[s >> 1], m1 = met[(s >> 1) | (NS >> 1)];
            bool ch = m1 < m0;
            nm[s] = (ch ? m1 : m0) + bm[s];
            if (bw && ch) bw[s >> 6] |= 1ull << (s & 63);
        }
        met.swap(nm);
    }
    int s = 0; for (int q = 1; q < NS; q++) if (met[q] < met[s]) s = q;
    double best = met[s];
    if (bits) {
        u8 out[NTOT + W] = {0};
        for (int j = j1 - 1; j > j0; j--) {
            out[j + W] = s & 1;
            bool ch = (back[(size_t)(j - j0 - 1) * (NS / 64) + (s >> 6)] >> (s & 63)) & 1;
            s = (s >> 1) | (ch ? (1 << (2 * W)) : 0);
        }
        for (int d = 0; d <= 2 * W; d++) out[j0 - W + d] = (s >> (2 * W - d)) & 1;
        memcpy(bits, out + 24, NB);
    }
    return best;
}

Result Decoder::decode_fast(const std::vector<float> &y, double lo, double hi) const {
    double bo = lo, bm = 1e300;
    for (int k = 0;; k++) {
        double off = lo + 0.5 * k;
        if (off > hi + 0.01) break;
        double m = viterbi(y, off, W, 24 + PREFIX, nullptr);
        if (m < bm) { bm = m; bo = off; }
    }
    Result r; double best = 1e300; u8 bits[NB], bb[NB];
    for (double o : {bo - 0.25, bo, bo + 0.25}) {
        double m = viterbi(y, o, W, NTOT, bits);
        if (m < best) { best = m; r.off = o; memcpy(bb, bits, NB); }
    }
    bytes_lsb(bb, 42, r.data.data());
    r.met = best / (NTOT * SPB);
    return r;
}

static std::vector<int> display_bytes(const std::array<u8, 42> &p) {
    int a = ham::fix[p[0]], b = ham::fix[p[1]];
    std::vector<int> v;
    if (a < 0 || b < 0) return v;
    int row = (a >> 3) | (b << 1);
    if (row == 0) for (int k = 10; k < 42; k++) v.push_back(k);
    else if (row <= 25) for (int k = 2; k < 42; k++) v.push_back(k);
    return v;
}

int Decoder::repair(const std::vector<float> &y, double off, std::array<u8, 42> &d) const {
    std::vector<int> bad;
    for (int k : display_bytes(d)) if (!odd_parity(d[k])) bad.push_back(k);
    if (bad.empty()) return 0;
    int ny = (int)y.size();
    auto pb = preb();
    std::vector<int> pad(NTOT + W + 1, 0);
    for (int i = 0; i < 24; i++) pad[i] = pb[i];
    auto db = bits_lsb(d.data(), 42);
    for (int i = 0; i < NB; i++) pad[24 + i] = db[i];
    const int NJ = NTOT - W;
    std::vector<int> xs(NJ), ph(NJ), nn(NJ), Z(NJ);
    for (int q = 0; q < NJ; q++) {
        int j = W + q; double x0 = off + j * SPB; xs[q] = (int)ceil(x0);
        int p_ = ((int)((xs[q] - x0) * NPH)) % NPH; ph[q] = p_ < 0 ? p_ + NPH : p_;
        nn[q] = (int)ceil(off + (j + 1) * SPB) - xs[q]; Z[q] = zone_of(j);
    }
    auto pattern = [&](int q) { int s = 0, j = W + q; for (int dd = -W; dd <= W; dd++) s = (s << 1) | pad[j + dd]; return s; };
    auto err = [&](int q, int p) {
        const double *t = &T_[((size_t)(Z[q] * NPH + ph[q]) * NS + p) * NX];
        double e = 0;
        for (int k = 0; k < NX; k++) {
            int i = xs[q] + k;
            if (k < nn[q] && i < ny && i >= 0) { double v = y[i] - t[k]; e += v * v; }
        }
        return e;
    };
    int fixed = 0;
    for (int k : bad) {
        double bestd = 1e300; int bb = 0;
        for (int b = 0; b < 8; b++) {
            int cand = 24 + 8 * k + b;
            double delta = 0;
            for (int dd = -W; dd <= W; dd++) {
                int q = cand + dd - W;
                if (q < 0 || q >= NJ) continue;
                int shift = W - (cand - (q + W));
                int P = pattern(q);
                delta += err(q, P ^ (1 << shift)) - err(q, P);
            }
            if (delta < bestd) { bestd = delta; bb = b; }
        }
        d[k] ^= 1 << bb; fixed++;
        pad[24 + 8 * k + bb] ^= 1;
    }
    return fixed;
}

Templates train_zoned(const std::vector<std::tuple<std::vector<float>, double, std::vector<u8>>> &samples, double k) {
    size_t sz = (size_t)NZ * NPH * NS * NX;
    std::vector<double> S(sz, 0), C(sz, 0);
    auto pb = preb();
    for (auto &smp : samples) {
        const auto &y = std::get<0>(smp); double off = std::get<1>(smp); const auto &bits = std::get<2>(smp);
        std::vector<int> pad(NTOT + W, 0);
        for (int i = 0; i < 24; i++) pad[i] = pb[i];
        for (int i = 0; i < NB; i++) pad[24 + i] = bits[i];
        for (int j = W; j < NTOT; j++) {
            double x0 = off + j * SPB; int xs = (int)ceil(x0);
            int ph = ((int)((xs - x0) * NPH)) % NPH; if (ph < 0) ph += NPH;
            int xe = (int)ceil(off + (j + 1) * SPB), n = xe - xs, z = zone_of(j);
            int p = 0; for (int dd = -W; dd <= W; dd++) p = (p << 1) | pad[j + dd];
            size_t base = ((size_t)(z * NPH + ph) * NS + p) * NX;
            for (int q = 0; q < n && q < NX; q++) {
                int i = xs + q;
                if (i >= 0 && i < (int)y.size()) { S[base + q] += y[i]; C[base + q] += 1; }
            }
        }
    }
    Templates T(sz);
    size_t per = (size_t)NPH * NS * NX;
    for (size_t q = 0; q < per; q++) {
        double gs = 0, gc = 0;
        for (int z = 0; z < NZ; z++) { gs += S[z * per + q]; gc += C[z * per + q]; }
        double G = gs / std::max(gc, 1.0);
        for (int z = 0; z < NZ; z++) T[z * per + q] = (S[z * per + q] + k * G) / (C[z * per + q] + k);
    }
    return T;
}

double best_offset(const std::vector<float> &y, const std::vector<u8> &bits, double lo, double hi, double step) {
    static const std::vector<double> h = channel_h();
    int n = (int)y.size(), H = (int)h.size();
    double best = -1e18, bo = lo;
    std::vector<double> w(n), t(n);
    for (double off = lo; off < hi - 1e-12; off += step) {
        for (int x = 0; x < n; x++) {
            int k = (int)floor((x - off) / SPB);
            w[x] = (k >= 0 && k < (int)bits.size()) ? bits[k] * 2.0 - 1 : 0.0;
        }
        double tt = 0, yt = 0;
        for (int x = 0; x < n; x++) {          // np.convolve(w, h, 'same')
            double s = 0;
            for (int q = 0; q < H; q++) { int i = x + (H - 1) / 2 - q; if (i >= 0 && i < n) s += w[i] * h[q]; }
            t[x] = s; tt += s * s; yt += y[x] * s;
        }
        double c = tt > 0 ? yt / sqrt(tt) : -1e18;
        if (c > best) { best = c; bo = off; }
    }
    return bo;
}

// ---------------------------------------------------------------- видеокарта
// группа из WG потоков на задачу (WG ≤ NS: у AMD и Intel не больше 256 потоков в группе), каждый поток — NS/WG состояний
static const char *KERNEL = R"CL(
#define NWORD (NS / 32)
#define SPT (NS / WG)
__kernel void viterbi(__global const float *Y, __global const int *tline, __global const float *toff,
                      __global const float *T, __global const float *T2, __global const int *zone,
                      __global const int *kmask, __global const int *kval,
                      const int nsteps, const int trace, const float spb,
                      __global uint *back, __global float *out_met, __global uchar *out_bits)
{
    const int task = get_group_id(0), l = get_local_id(0);
    __local float met[2][NS];
    __local float yw[NX]; __local int geo[3];
    __local uint bits[NWORD];
    __local float rmin[WG]; __local int rarg[WG];
    const float off = toff[task];
    __global const float *y = Y + (size_t)tline[task] * SPL;
    int cur = 0;
    for (int st = 0; st < nsteps; st++) {
        const int j = st + TW / 2;
        if (l == 0) {
            float x0 = off + j * spb; int xs = (int)ceil(x0);
            int ph = ((int)((xs - x0) * NPH)) % NPH; if (ph < 0) ph += NPH;
            int n = (int)ceil(off + (j + 1) * spb) - xs;
            geo[0] = xs; geo[1] = ph; geo[2] = n;
        }
        for (int w = l; w < NWORD; w += WG) bits[w] = 0;
        barrier(CLK_LOCAL_MEM_FENCE);
        if (l < NX) { int xs = geo[0] + l; yw[l] = (l < geo[2] && xs >= 0 && xs < SPL) ? y[xs] : 0.0f; }
        barrier(CLK_LOCAL_MEM_FENCE);
        const int z = zone[st], ph = geo[1], n = geo[2];
        int nn = n < 5 ? 0 : (n > 6 ? 1 : n - 5);
        for (int q = 0; q < SPT; q++) {
            const int s = l + q * WG;
            __global const float *t = T + (((size_t)(z * NPH + ph) * NS + s) * NX);
            float ysq = 0.0f, dot = 0.0f;
            for (int k = 0; k < NX; k++) { ysq += yw[k] * yw[k]; dot += yw[k] * t[k]; }
            float bm = ysq - 2.0f * dot + T2[((size_t)(z * NPH + ph) * NS + s) * 2 + nn];
            if ((s & kmask[st]) != kval[st]) bm = 1e9f;
            if (st == 0) met[cur][s] = bm;
            else {
                float m0 = met[cur][s >> 1], m1 = met[cur][(s >> 1) | (NS >> 1)];
                int ch = m1 < m0;
                met[cur ^ 1][s] = (ch ? m1 : m0) + bm;
                if (trace && ch) atomic_or(&bits[s >> 5], 1u << (s & 31));
            }
        }
        if (st > 0) cur ^= 1;
        barrier(CLK_LOCAL_MEM_FENCE);
        if (trace && st > 0) for (int w = l; w < NWORD; w += WG) back[((size_t)task * NSTEP + st) * NWORD + w] = bits[w];
    }
    float bv = 3.4e38f; int ba = 0;
    for (int q = 0; q < SPT; q++) { int s = l + q * WG; float v = met[cur][s]; if (v < bv || (v == bv && s < ba)) { bv = v; ba = s; } }
    rmin[l] = bv; rarg[l] = ba;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int h = WG / 2; h > 0; h >>= 1) {
        if (l < h && (rmin[l + h] < rmin[l] || (rmin[l + h] == rmin[l] && rarg[l + h] < rarg[l]))) { rmin[l] = rmin[l + h]; rarg[l] = rarg[l + h]; }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (l == 0) {
        out_met[task] = rmin[0];
        if (trace) {
            __global uchar *o = out_bits + (size_t)task * (NSTEP + TW + 1);
            int q = rarg[0];
            for (int st = nsteps - 1; st > 0; st--) {
                o[st + TW] = q & 1;
                uint w = back[((size_t)task * NSTEP + st) * NWORD + (q >> 5)];
                q = (q >> 1) | (((w >> (q & 31)) & 1) ? (1 << TW) : 0);
            }
            for (int d = 0; d <= TW; d++) o[d] = (q >> (TW - d)) & 1;
        }
    }
}
)CL";

struct GpuDecoder::Impl {
    std::unique_ptr<gpu::Kernel> k;
    gpu::Buffer zone, km, kv, T, T2;
    int nstep = NTOT - W, WG = NS;
};

GpuDecoder::GpuDecoder(const Templates &T) : p(new Impl) {
    // потоков в группе — сколько позволяет видеокарта (NVIDIA 1024, AMD и Intel 256), не больше NS
    p->WG = (int)std::min<size_t>(NS, gpu::max_group());
    for (;;) {
        std::string defs = fmt("#define NS %d\n#define TW %d\n#define NX %d\n#define NPH %d\n#define NZ %d\n#define SPL 2048\n#define NSTEP %d\n#define WG %d\n",
                               NS, 2 * W, NX, NPH, NZ, p->nstep, p->WG);
        p->k.reset(new gpu::Kernel(defs + KERNEL, "viterbi"));
        if ((int)p->k->group_limit() >= p->WG || p->WG <= 32) break;
        p->WG /= 2;
    }
    std::vector<int> zone(p->nstep), km(p->nstep), kv(p->nstep);
    const Known &K = known();
    for (int st = 0; st < p->nstep; st++) { int j = st + W; zone[st] = zone_of(j); km[st] = K.km[j]; kv[st] = K.kv[j]; }
    p->zone = gpu::Buffer(zone.size() * 4, zone.data(), true);
    p->km = gpu::Buffer(km.size() * 4, km.data(), true);
    p->kv = gpu::Buffer(kv.size() * 4, kv.data(), true);
    set_templates(T);
}
GpuDecoder::~GpuDecoder() { delete p; }
std::string GpuDecoder::name() const { return gpu::device_name(); }
void GpuDecoder::set_templates(const Templates &Td) {
    std::vector<float> T(Td.begin(), Td.end()), T2((size_t)NZ * NPH * NS * 2);
    for (size_t q = 0; q < (size_t)NZ * NPH * NS; q++) {
        double c = 0;
        for (int x = 0; x < NX; x++) { c += Td[q * NX + x] * Td[q * NX + x]; if (x == 4) T2[q * 2] = (float)c; if (x == 5) T2[q * 2 + 1] = (float)c; }
    }
    p->T = gpu::Buffer(T.size() * 4, T.data(), true);
    p->T2 = gpu::Buffer(T2.size() * 4, T2.data(), true);
}

std::vector<Result> GpuDecoder::decode(const std::vector<float> &lines, size_t N, double lo, double hi) {
    std::vector<Result> out(N);
    std::vector<double> offs;
    for (int k = 0;; k++) { double o = lo + 0.5 * k; if (o > hi + 0.01) break; offs.push_back(o); }
    size_t K = offs.size();
    const int NW = NS / 32, OBL = p->nstep + 2 * W + 1;
    // строк в пачке: обратные ссылки трёх кандидатов на строку должны уместиться в один буфер видеокарты
    size_t BATCH = 2048;
    while (BATCH > 64 && BATCH * 3 * (size_t)p->nstep * NW * 4 > gpu::max_alloc() * 9 / 10) BATCH /= 2;
    for (size_t a = 0; a < N; a += BATCH) {
        size_t n = std::min(BATCH, N - a);
        std::vector<float> Y(n * 2048);
        for (size_t i = 0; i < n; i++) {
            auto y = norm(&lines[(a + i) * 2048], 2048);
            memcpy(&Y[i * 2048], y.data(), 2048 * 4);
        }
        gpu::Buffer bY(Y.size() * 4, Y.data(), true);
        auto run = [&](const std::vector<int> &tl, const std::vector<float> &to, int nsteps, int trace, std::vector<float> &met, std::vector<u8> *bits) {
            size_t nt = tl.size();
            gpu::Buffer btl(nt * 4, tl.data(), true), bto(nt * 4, to.data(), true);
            gpu::Buffer bback(trace ? nt * p->nstep * NW * 4 : 4), bmet(nt * 4), bbits(trace ? nt * OBL : 4);
            auto &k = *p->k;
            k.arg(0, bY); k.arg(1, btl); k.arg(2, bto); k.arg(3, p->T); k.arg(4, p->T2); k.arg(5, p->zone); k.arg(6, p->km); k.arg(7, p->kv);
            k.arg(8, nsteps); k.arg(9, trace); k.arg(10, (float)SPB); k.arg(11, bback); k.arg(12, bmet); k.arg(13, bbits);
            k.run(nt * p->WG, p->WG);
            met.resize(nt); bmet.read(met.data(), nt * 4);
            if (bits) { bits->resize(nt * OBL); bbits.read(bits->data(), nt * OBL); }
        };
        std::vector<int> tl(n * K); std::vector<float> to(n * K);
        for (size_t i = 0; i < n; i++) for (size_t q = 0; q < K; q++) { tl[i * K + q] = (int)i; to[i * K + q] = (float)offs[q]; }
        std::vector<float> met;
        run(tl, to, 24 + PREFIX - W, 0, met, nullptr);
        std::vector<int> tl2(n * 3); std::vector<float> to2(n * 3);
        for (size_t i = 0; i < n; i++) {
            size_t b = 0; for (size_t q = 1; q < K; q++) if (met[i * K + q] < met[i * K + b]) b = q;
            for (int c = 0; c < 3; c++) { tl2[i * 3 + c] = (int)i; to2[i * 3 + c] = (float)(offs[b] + (c - 1) * 0.25); }
        }
        std::vector<u8> bits;
        run(tl2, to2, p->nstep, 1, met, &bits);
        for (size_t i = 0; i < n; i++) {
            int b = 0; for (int c = 1; c < 3; c++) if (met[i * 3 + c] < met[i * 3 + b]) b = c;
            Result &r = out[a + i];
            bytes_lsb(&bits[(i * 3 + b) * OBL + 24], 42, r.data.data());
            r.off = to2[i * 3 + b]; r.met = met[i * 3 + b] / (NTOT * SPB);
        }
    }
    return out;
}
}
