#include "datacast.h"
#include "vbidecode.h"
#include <complex>
#include <mutex>
#include <chrono>
#include <atomic>

namespace {
const double RATE = 8 * FSC_NTSC / 5;
const int K = 5, NP = 2, NTAP = 2 * K + 1, NS = 1 << (NTAP - 1);
const double PH[NP] = {-0.25, 0.25};
const int PKT_BITS = 272;
const double FIELD_S = 1001 / 60000.0;
const uint8_t HDR[24] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0};
using Model = std::array<double, NP * (NTAP + 1)>;   // C[p][k]

double interp_clamp(double x, const std::vector<float> &y) {
    int n = (int)y.size();
    if (x <= 0) return y[0];
    if (x >= n - 1) return y[n - 1];
    int i = (int)x; double f = x - i;
    return y[i] * (1 - f) + y[i + 1] * f;
}
int sgn(double v) { return (v > 0) - (v < 0); }

struct Line {
    double T; int lo, hi;
    Line(double fs, int ns) : T(fs / RATE), lo((int)(ns * 0.154)), hi((int)(ns * 0.967)) {}
    bool prep(const std::vector<float> &y, std::vector<double> &Y) const {
        std::vector<double> s(y.begin() + lo, y.begin() + hi);
        int w = (int)(T * 12) | 1, h = (w - 1) / 2, n = (int)s.size();
        std::vector<double> pre(n + 1, 0), d(n);
        for (int i = 0; i < n; i++) pre[i + 1] = pre[i] + s[i];
        for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n, i + h + 1); d[i] = s[i] - (pre[b] - pre[a]) / w; }
        std::complex<double> acc = 0; int nz = 0;
        for (int z = 0; z + 1 < n; z++) {
            if (sgn(d[z + 1]) == sgn(d[z])) continue;
            double den = d[z] - d[z + 1];
            double t = z + (den != 0 ? d[z] / den : 0) + lo;
            acc += std::exp(std::complex<double>(0, 2 * M_PI * t / T)); nz++;
        }
        if (nz < 20) return false;
        acc /= (double)nz;
        double t0 = std::arg(acc) / (2 * M_PI) * T;
        double c0 = t0 + ceil((lo - t0) / T) * T;
        int nb = (int)((hi - c0) / T);
        double m = 0, sd = 0; int cnt = 0;
        for (int i = lo + 60; i < hi; i++) { m += y[i]; cnt++; }
        m /= cnt; for (int i = lo + 60; i < hi; i++) sd += (y[i] - m) * (y[i] - m); sd = sqrt(sd / cnt); if (sd == 0) sd = 1;
        Y.resize((size_t)(nb + 2 * K) * NP);
        for (int i = -K; i < nb + K; i++) for (int p = 0; p < NP; p++)
            Y[(i + K) * NP + p] = (interp_clamp(c0 + (i + 0.5 + PH[p]) * T, y) - m) / sd;
        return true;
    }
};

std::vector<u8> viterbi(const std::vector<double> &Y, const Model &C) {
    int nb = (int)(Y.size() / NP), NX = 1 << NTAP;
    std::vector<float> pred(NX * NP);
    for (int x = 0; x < NX; x++) for (int p = 0; p < NP; p++) {
        double v = C[p * (NTAP + 1) + NTAP];
        for (int k = 0; k < NTAP; k++) v += (((x >> (NTAP - 1 - k)) & 1) ? 1.0 : -1.0) * C[p * (NTAP + 1) + k];
        pred[x * NP + p] = (float)v;
    }
    std::vector<float> M(NS, 0), nm(NS);
    std::vector<uint64_t> back((size_t)nb * (NS / 64), 0);
    for (int i = 0; i < nb; i++) {
        float y0 = (float)Y[i * NP], y1 = (float)Y[i * NP + 1];
        uint64_t *bw = &back[(size_t)i * (NS / 64)];
        for (int s = 0; s < NS; s++) {
            int x0 = s, x1 = s | NS;
            float a0 = y0 - pred[x0 * 2], b0 = y1 - pred[x0 * 2 + 1], a1 = y0 - pred[x1 * 2], b1 = y1 - pred[x1 * 2 + 1];
            float c0 = M[x0 >> 1] + a0 * a0 + b0 * b0, c1 = M[x1 >> 1] + a1 * a1 + b1 * b1;
            bool ch = c1 < c0;
            nm[s] = ch ? c1 : c0;
            if (ch) bw[s >> 6] |= 1ull << (s & 63);
        }
        M.swap(nm);
    }
    int s = 0; for (int q = 1; q < NS; q++) if (M[q] < M[s]) s = q;
    std::vector<u8> out(nb + NTAP - 1, 0);
    for (int i = nb - 1; i >= 0; i--) {
        out[i + NTAP - 1] = s & 1;
        int b = (back[(size_t)i * (NS / 64) + (s >> 6)] >> (s & 63)) & 1;
        s = (s >> 1) | (b << (NTAP - 2));
    }
    for (int k = 0; k < NTAP - 1; k++) out[k] = (s >> (NTAP - 2 - k)) & 1;
    return out;
}

Model train(const Rec &R, int row, const std::vector<int> &units, const Line &L, Progress *pr) {
    std::vector<std::vector<double>> Ys;
    for (int u : units) { std::vector<double> Y; if (L.prep(R.line_h(u, row), Y)) Ys.push_back(Y); if (Ys.size() >= 40) break; }
    Model C{};
    for (int p = 0; p < NP; p++) {
        double s = 0;
        for (int k = 0; k < NTAP; k++) { double d = k - K; C[p * (NTAP + 1) + k] = exp(-0.5 * pow((d - PH[p]) / 0.9, 2)); s += C[p * (NTAP + 1) + k]; }
        for (int k = 0; k < NTAP; k++) C[p * (NTAP + 1) + k] /= s * 0.6;
    }
    double resid = 0;
    for (int it = 0; it < 8; it++) {
        std::vector<double> X, Yc;
        for (auto &Y : Ys) {
            auto b = viterbi(Y, C); int nb = (int)(Y.size() / NP);
            for (int i = 0; i < nb; i++) {
                for (int j = 0; j < NTAP; j++) X.push_back(b[i + j] * 2.0 - 1);
                X.push_back(1.0);
                Yc.push_back(Y[i * NP]); Yc.push_back(Y[i * NP + 1]);
            }
        }
        int r = (int)(Yc.size() / NP);
        if (r == 0) break;
        auto B = lstsq(X, Yc, r, NTAP + 1, NP);
        for (int k = 0; k <= NTAP; k++) for (int p = 0; p < NP; p++) C[p * (NTAP + 1) + k] = B[k * NP + p];
        double ss = 0, mm = 0; std::vector<double> res;
        for (int q = 0; q < r; q++) for (int p = 0; p < NP; p++) {
            double v = Yc[q * NP + p]; for (int k = 0; k <= NTAP; k++) v -= X[q * (NTAP + 1) + k] * B[k * NP + p];
            res.push_back(v);
        }
        resid = stdev(res); (void)ss; (void)mm;
    }
    if (pr) pr->log(fmt("  channel model for line %d: residual %.3f", R.tv.at(row), resid));
    return C;
}

std::vector<int> active_units(const Rec &R, int row, int par) {
    std::vector<int> uu;
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) uu.push_back(u);
    int lo = (int)(R.ns_h() * 0.33), hi = (int)(R.ns_h() * 0.93);
    std::vector<double> sd(uu.size());
    parallel_for(uu.size(), [&](size_t i) { auto y = R.line_h(uu[i], row); std::vector<double> s(y.begin() + lo, y.begin() + hi); sd[i] = stdev(s); }, nullptr, 256);
    double q1 = percentile(sd, 10), q9 = percentile(sd, 95);
    std::vector<int> out;
    for (size_t i = 0; i < uu.size(); i++) if (sd[i] > (q1 + q9) / 2) out.push_back(uu[i]);
    return out;
}

bool find_packet(const std::vector<u8> &b, std::vector<u8> &pkt) {
    int best = 99, bs = 0;
    for (int s = 0; s < 30; s++) {
        if (s + 24 > (int)b.size()) break;
        int e = 0; for (int i = 0; i < 24; i++) e += b[s + i] != HDR[i];
        if (e < best) { best = e; bs = s; }
    }
    if (best > 3 || bs + 24 + PKT_BITS > (int)b.size()) return false;
    pkt.assign(b.begin() + bs + 24, b.begin() + bs + 24 + PKT_BITS);
    return true;
}
}

double datacast_check(const Rec &R, int row, int par) {
    auto uu = active_units(R, row, par);
    if (uu.size() < 10) return 0;
    Line L(R.fs_h(), R.ns_h());
    std::vector<int> tu;
    for (size_t i = 0; i < uu.size() && tu.size() < 40; i += std::max<size_t>(1, uu.size() / 40)) tu.push_back(uu[i]);
    Model C = train(R, row, tu, L, nullptr);
    std::vector<int> test;
    for (size_t i = 0; i < uu.size() && test.size() < 30; i += std::max<size_t>(1, uu.size() / 30)) test.push_back(uu[i]);
    int ok = 0;
    for (int u : test) {
        std::vector<double> Y;
        if (!L.prep(R.line_h(u, row), Y)) continue;
        auto b = viterbi(Y, C);
        int best = 99;
        for (int s = 0; s < 30 && s + 24 <= (int)b.size(); s++) { int e = 0; for (int i = 0; i < 24; i++) e += b[s + i] != HDR[i]; best = std::min(best, e); }
        ok += best <= 3;
    }
    return round(1000.0 * ok / test.size()) / 1000;
}

// ---------------------------------------------------------------- пакеты и анализ
namespace {
struct Pk {
    std::vector<int> units, lines; std::vector<u8> bits; double agree = 0;
    Bytes bytes, payload; int marker = 0, address = 0; bool fixed = false;
};

std::vector<Pk> vote(const std::vector<std::tuple<int, int, std::vector<u8>>> &pk, int maxdiff = 21) {
    size_t n = pk.size();
    std::vector<std::array<u8, 34>> P(n);
    for (size_t i = 0; i < n; i++) {
        auto &b = std::get<2>(pk[i]);
        for (int q = 0; q < 34; q++) { int v = 0; for (int k = 0; k < 8; k++) v = (v << 1) | b[q * 8 + k]; P[i][q] = (u8)v; }   // packbits (старший первым)
    }
    std::vector<int> lab(n, -1); int c = 0;
    for (size_t i = 0; i < n; i++) {
        if (lab[i] >= 0) continue;
        for (size_t j = 0; j < n; j++) {
            if (lab[j] >= 0) continue;
            int d = 0; for (int q = 0; q < 34 && d <= maxdiff; q++) d += popcount8(P[i][q] ^ P[j][q]);
            if (d <= maxdiff) lab[j] = c;
        }
        c++;
    }
    std::vector<Pk> out(c);
    std::vector<std::vector<size_t>> mem(c);
    for (size_t i = 0; i < n; i++) mem[lab[i]].push_back(i);
    for (int k = 0; k < c; k++) {
        Pk &p = out[k]; std::set<int> ls;
        std::vector<double> mean(PKT_BITS, 0);
        for (size_t i : mem[k]) { p.units.push_back(std::get<0>(pk[i])); ls.insert(std::get<1>(pk[i])); for (int q = 0; q < PKT_BITS; q++) mean[q] += std::get<2>(pk[i])[q]; }
        std::sort(p.units.begin(), p.units.end()); p.lines.assign(ls.begin(), ls.end());
        p.bits.resize(PKT_BITS); long dis = 0;
        for (int q = 0; q < PKT_BITS; q++) p.bits[q] = mean[q] / mem[k].size() > 0.5;
        for (size_t i : mem[k]) for (int q = 0; q < PKT_BITS; q++) dis += std::get<2>(pk[i])[q] != p.bits[q];
        p.agree = 1 - (double)dis / (mem[k].size() * PKT_BITS);
    }
    std::stable_sort(out.begin(), out.end(), [](const Pk &a, const Pk &b) { return a.units[0] < b.units[0]; });
    for (auto &p : out) {
        p.bytes.resize(34); bytes_lsb(p.bits.data(), 34, p.bytes.data());
        p.marker = 0; for (int i = 0; i < 4; i++) p.marker |= p.bits[i] << i;
        p.address = 0; for (int i = 0; i < 12; i++) p.address |= p.bits[4 + i] << i;
        p.payload.assign(p.bytes.begin() + 2, p.bytes.begin() + 33);
    }
    return out;
}

void fix_headers(std::vector<Pk> &pk) {
    std::map<int, int> cnt;
    for (auto &p : pk) cnt[p.marker | p.address << 4]++;
    std::vector<int> good;
    for (auto &kv : cnt) if (kv.second >= std::max(5.0, 0.002 * pk.size())) good.push_back(kv.first);
    for (auto &p : pk) {
        int h = p.marker | p.address << 4;
        if (std::find(good.begin(), good.end(), h) != good.end() || p.units.size() >= 3) continue;
        int bd = 99, bc = 0, bg = -1;
        for (int g : good) { int d = __builtin_popcount(h ^ g); if (d < bd || (d == bd && cnt[g] > bc)) { bd = d; bc = cnt[g]; bg = g; } }
        if (bg >= 0 && bd <= 2) { p.marker = bg & 15; p.address = bg >> 4; p.fixed = true; }
    }
}

std::vector<std::pair<int, int>> bursts(std::vector<int> u, int gap) {
    std::vector<std::pair<int, int>> out;
    if (u.empty()) return out;
    std::sort(u.begin(), u.end());
    out.push_back({u[0], u[0]});
    for (size_t i = 1; i < u.size(); i++) { if (u[i] - out.back().second > gap) out.push_back({u[i], u[i]}); else out.back().second = u[i]; }
    return out;
}

struct Addr { int packets = 0, copies = 0; std::set<int> lines; int first = 0, last = 0; std::vector<int> gaps; Json repeat_s; };

std::vector<int> fixed_bits(const std::vector<const Pk *> &g, size_t min_n = 50) {
    std::map<int, std::vector<const Pk *>> A;
    for (auto *p : g) A[p->address].push_back(p);
    std::set<int> common; bool first = true;
    for (auto &kv : A) {
        if (kv.second.size() < min_n) continue;
        std::vector<double> m(244, 0);
        for (auto *p : kv.second) for (int i = 0; i < 244; i++) m[i] += (p->payload[i / 8] >> (i % 8)) & 1;
        std::set<int> f;
        for (int i = 0; i < 244; i++) { double v = m[i] / kv.second.size(); if (v > 0.97 || v < 0.03) f.insert(i); }
        if (first) { common = f; first = false; }
        else { std::set<int> x; for (int i : common) if (f.count(i)) x.insert(i); common = x; }
    }
    return std::vector<int>(common.begin(), common.end());
}

std::vector<Bytes> assemble_carousel(const std::vector<Pk> &packets, size_t k = 8) {
    std::map<int, int> mc; for (auto &p : packets) mc[p.marker]++;
    std::vector<Bytes> C; std::set<Bytes> seen;
    for (auto &p : packets) {
        if (mc[p.marker] < 0.01 * packets.size() || p.units.size() < 2) continue;
        Bytes c(p.payload.begin(), p.payload.begin() + 30);
        if (seen.insert(c).second) C.push_back(c);
    }
    if (C.empty()) return {};
    std::map<Bytes, std::vector<int>> idx;
    for (size_t i = 0; i < C.size(); i++) idx[Bytes(C[i].begin(), C[i].begin() + k)].push_back((int)i);
    std::map<int, std::pair<int, int>> nxt;
    for (size_t i = 0; i < C.size(); i++) {
        auto &c = C[i]; int bj = -1, bl = 0;
        for (size_t o = 1; o + k <= c.size(); o++) {
            auto it = idx.find(Bytes(c.begin() + o, c.begin() + o + k));
            if (it == idx.end()) continue;
            for (int j : it->second) {
                int L = (int)(c.size() - o);
                if (j != (int)i && std::equal(c.begin() + o, c.end(), C[j].begin()) && (bj < 0 || L > bl)) { bj = j; bl = L; }
            }
        }
        if (bj >= 0) nxt[(int)i] = {bj, bl};
    }
    std::set<int> has_prev; for (auto &kv : nxt) has_prev.insert(kv.second.first);
    std::vector<Bytes> out;
    for (size_t s0 = 0; s0 < C.size(); s0++) {
        if (has_prev.count((int)s0)) continue;
        Bytes seq = C[s0]; int i = (int)s0; std::set<int> sn = {i};
        while (nxt.count(i) && !sn.count(nxt[i].first)) { auto jl = nxt[i]; seq.insert(seq.end(), C[jl.first].begin() + jl.second, C[jl.first].end()); sn.insert(jl.first); i = jl.first; }
        out.push_back(seq);
    }
    std::stable_sort(out.begin(), out.end(), [](const Bytes &a, const Bytes &b) { return a.size() > b.size(); });
    return out;
}

std::string hex(const Bytes &b, size_t a, size_t n, const char *sep = "") {
    std::string s;
    for (size_t i = a; i < std::min(b.size(), a + n); i++) s += fmt("%s%02x", i > a ? sep : "", b[i]);
    return s;
}
std::string join_ints(const std::vector<int> &v, const char *sep) { std::string s; for (size_t i = 0; i < v.size(); i++) s += (i ? sep : "") + std::to_string(v[i]); return s; }
std::string num_or_none(const Json &j) { return j.is_null() ? "None" : fmt("%g", j.num()); }
}

std::string datacast_export(const std::string &src, const std::string &out, std::vector<std::pair<int, int>> lines, Progress &pr) {
    auto t0 = std::chrono::steady_clock::now();
    auto fc = detect_format(src);
    if (fc.first.empty()) throw std::runtime_error("unknown recording format");
    Rec R(src, fc.first);
    if (lines.empty()) {
        Json res = vbi_probe(src, pr);
        for (auto &L : res["lines"].a)
            if (L["kind"].str().rfind("encrypted datacast", 0) == 0) lines.push_back({L["row"].integer(), L["parity"].is_null() ? -1 : L["parity"].integer()});
    }
    std::vector<std::tuple<int, int, std::vector<u8>>> raw;
    for (auto &rp : lines) {
        int row = rp.first, par = rp.second;
        auto uu = active_units(R, row, par);
        if (uu.size() < 20) continue;
        pr.step(fmt("Reading line %d%s: %zu %ss with data", R.tv[row], par < 0 ? "" : (std::string(" field ") + "AB"[par]).c_str(), uu.size(), R.unit.c_str()));
        Line L(R.fs_h(), R.ns_h());
        std::vector<int> tu; for (size_t i = 0; i < uu.size(); i += std::max<size_t>(1, uu.size() / 40)) tu.push_back(uu[i]);
        Model C = train(R, row, tu, L, &pr);
        std::vector<std::vector<u8>> got(uu.size());
        std::atomic<size_t> done{0};
        parallel_for(uu.size(), [&](size_t i) {
            std::vector<double> Y;
            if (L.prep(R.line_h(uu[i], row), Y)) { auto b = viterbi(Y, C); std::vector<u8> p; if (find_packet(b, p)) got[i] = p; }
            size_t d = ++done; if (d % 500 == 0) pr.progress(d, uu.size());
        }, &pr, 16);
        for (size_t i = 0; i < uu.size(); i++) if (!got[i].empty()) raw.emplace_back(uu[i], R.tv[row], got[i]);
    }
    pr.step(fmt("Combining copies of %zu packets", raw.size()));
    auto packets = vote(raw);
    fix_headers(packets);
    std::vector<int> raw_units; { std::set<int> s; for (auto &t : raw) s.insert(std::get<0>(t)); raw_units.assign(s.begin(), s.end()); }
    // анализ
    double per = R.unit == "field" ? FIELD_S : 2 * FIELD_S;
    std::vector<int> aorder; std::map<int, Addr> A;
    for (auto &p : packets) {
        if (!A.count(p.address)) { aorder.push_back(p.address); A[p.address].first = p.units[0]; A[p.address].last = p.units.back(); }
        Addr &a = A[p.address];
        a.packets++; a.copies += (int)p.units.size(); a.lines.insert(p.lines.begin(), p.lines.end());
        a.first = std::min(a.first, p.units[0]); a.last = std::max(a.last, p.units.back());
        for (size_t i = 1; i < p.units.size(); i++) a.gaps.push_back(p.units[i] - p.units[i - 1]);
    }
    for (auto &kv : A) {
        std::vector<double> g; for (int x : kv.second.gaps) if (x > 60) g.push_back(x);
        kv.second.repeat_s = g.empty() ? Json() : Json(round(median(g) * per * 10) / 10);
    }
    std::map<int, int> gaps;
    for (auto &p : packets) for (size_t i = 1; i < p.units.size(); i++) { int x = p.units[i] - p.units[i - 1]; if (x > 60) gaps[x]++; }
    auto br = bursts(raw_units, 6);
    std::vector<double> bl, bg;
    for (auto &b : br) bl.push_back(b.second - b.first + 1);
    for (size_t i = 0; i + 1 < br.size(); i++) bg.push_back(br[i + 1].first - br[i].second);
    std::map<int, int> markers; for (auto &p : packets) markers[p.marker]++;
    std::vector<std::pair<int, int>> mlist(markers.begin(), markers.end());
    std::stable_sort(mlist.begin(), mlist.end(), [](auto &a, auto &b) { return a.second > b.second; });
    double agree = 0; for (auto &p : packets) agree += p.agree; if (!packets.empty()) agree /= packets.size();
    long copies = 0; for (auto &p : packets) copies += (long)p.units.size();
    std::vector<std::pair<int, int>> glist(gaps.begin(), gaps.end());
    std::stable_sort(glist.begin(), glist.end(), [](auto &a, auto &b) { return a.second > b.second; });
    if (glist.size() > 6) glist.resize(6);
    double entropy = 0, maxent = 0;
    if (!packets.empty()) {
        for (int i = 0; i < 31; i++) {
            int c[256] = {0}; for (auto &p : packets) c[p.payload[i]]++;
            double h = 0; for (int v = 0; v < 256; v++) if (c[v]) { double q = (double)c[v] / packets.size(); h -= q * log2(q); }
            entropy += h / 31;
        }
        maxent = log2(std::min<double>(256, packets.size()));
    }
    // каналы
    std::vector<Json> channels;
    for (auto &m : mlist) {
        std::vector<const Pk *> g;
        for (auto &p : packets) if (p.marker == m.first) g.push_back(&p);
        Json ch = Json::object();
        std::set<int> ls, as; double cp = 0;
        for (auto *p : g) { ls.insert(p->lines.begin(), p->lines.end()); as.insert(p->address); cp += p->units.size(); }
        cp /= g.size();
        ch["marker"] = fmt("%X", m.first); ch["packets"] = m.second;
        Json lj = Json::array(); for (int l : ls) lj.push(l); ch["lines"] = lj;
        ch["addresses"] = (int)as.size(); ch["copies_avg"] = round(cp * 10) / 10;
        if (m.second < 0.01 * packets.size()) ch["kind"] = "rare (probably read errors in the marker)";
        else if (cp >= 1.5) {
            std::vector<double> gp; for (auto *p : g) for (size_t i = 1; i < p->units.size(); i++) if (p->units[i] - p->units[i - 1] > 60) gp.push_back(p->units[i] - p->units[i - 1]);
            ch["kind"] = "carousel"; ch["repeat_s"] = gp.empty() ? Json() : Json(round(median(gp) * per * 10) / 10); ch["bytes"] = m.second * 31;
        } else {
            std::vector<int> seq; for (auto *p : g) seq.push_back(p->address);
            double top = 0; std::vector<std::pair<double, int>> sc;
            for (int L = 2; L < 65; L++) {
                double v = 0;
                if ((int)seq.size() > L) { int e = 0; for (size_t i = 0; i + L < seq.size(); i++) e += seq[i] == seq[i + L]; v = (double)e / (seq.size() - L); }
                sc.push_back({v, L}); top = std::max(top, v);
            }
            int best = 0;
            if (top > 0.4) { best = 1000; for (auto &s : sc) if (s.first >= 0.9 * top) best = std::min(best, s.second); }
            ch["kind"] = "stream";
            if (best) {
                std::map<std::vector<int>, int> cnt; std::vector<std::vector<int>> ord;
                for (size_t i = 0; i + best < seq.size(); i++) {
                    std::vector<int> w(seq.begin() + i, seq.begin() + i + best);
                    std::set<int> u(w.begin(), w.end());
                    if ((int)u.size() != best) continue;
                    if (!cnt.count(w)) ord.push_back(w);
                    cnt[w]++;
                }
                if (!ord.empty()) {
                    std::vector<int> order = ord[0];
                    for (auto &w : ord) if (cnt[w] > cnt[order]) order = w;
                    std::vector<double> d;
                    for (int a : order) {
                        std::vector<int> t; for (auto *p : g) if (p->address == a) t.push_back(p->units[0]);
                        std::sort(t.begin(), t.end());
                        for (size_t i = 1; i < t.size(); i++) if (t[i] - t[i - 1] > 20) d.push_back(t[i] - t[i - 1]);
                    }
                    double blk = d.empty() ? 0 : median(d) * per;
                    if (!d.empty()) { Json r = Json::array(); r.push(round(percentile(d, 15) * per * 10) / 10); r.push(round(percentile(d, 85) * per * 10) / 10); ch["block_range_s"] = r; }
                    Json bj = Json::array(); for (int a : order) bj.push(fmt("%03X", a)); ch["block"] = bj;
                    ch["block_s"] = blk ? Json(round(blk * 100) / 100) : Json();
                    if (blk) ch["bit_s"] = (int)lround(best * 31 * 8 / blk);
                }
            }
        }
        if (ch["kind"].str() == "stream") { Json fb = Json::array(); for (int b : fixed_bits(g)) fb.push(b); ch["fixed_bits"] = fb; }
        channels.push_back(ch);
    }
    auto ct = assemble_carousel(packets);
    size_t ctot = 0; for (auto &c : ct) ctot += c.size();
    // файлы
    make_dirs(out);
    { Bytes all; for (auto &c : ct) all.insert(all.end(), c.begin(), c.end()); write_file(path_join(out, "carousel_stream.bin"), all); }
    {
        std::string t;
        for (size_t i = 0; i < ct.size(); i++) {
            t += fmt("fragment %zu: %zu bytes\n", i + 1, ct[i].size());
            for (size_t o = 0; o < ct[i].size(); o += 32) t += fmt("  %05zx  ", o) + hex(ct[i], o, 32, " ") + "\n";
        }
        write_text(path_join(out, "carousel_stream.txt"), t);
    }
    { Bytes all; for (auto &p : packets) all.insert(all.end(), p.bytes.begin(), p.bytes.end()); write_file(path_join(out, "packets_34byte.bin"), all); }
    {
        std::string c = "first_" + R.unit + ",time_s,copies,vbi_lines,address (* = corrected),agree,payload_hex\r\n";
        for (auto &p : packets) c += fmt("%d,%g,%zu,", p.units[0], round(p.units[0] * per * 100) / 100, p.units.size()) + join_ints(p.lines, "/") +
                                     fmt(",%03X%s,%g,", p.address, p.fixed ? "*" : "", round(p.agree * 1000) / 1000) + hex(p.payload, 0, 31) + "\r\n";
        write_text(path_join(out, "packets.csv"), c);
    }
    std::vector<int> abusy = aorder;
    std::stable_sort(abusy.begin(), abusy.end(), [&](int a, int b) { return A[a].packets > A[b].packets; });
    {
        std::string c = "address,packets,copies,vbi_lines,first_s,last_s,repeat_s\r\n";
        for (int a : abusy) {
            auto &v = A[a];
            c += fmt("%03X,%d,%d,", a, v.packets, v.copies) + join_ints(std::vector<int>(v.lines.begin(), v.lines.end()), "/") +
                 fmt(",%g,%g,", round(v.first * per * 10) / 10, round(v.last * per * 10) / 10) + (v.repeat_s.is_null() ? "" : fmt("%g", v.repeat_s.num())) + "\r\n";
        }
        write_text(path_join(out, "addresses.csv"), c);
    }
    std::vector<std::string> L;
    L.push_back("Encrypted datacast (5.727 Mbit/s, probably PBS National Datacast) \xE2\x80\x94 " + basename(src)); L.push_back("");
    L.push_back("Packet: run-in 55 55, framing 2D, 34 bytes: marker (4 bits) + address (12 bits) + 31 bytes data + 00");
    L.push_back(fmt("Distinct packets: %zu (from %ld received copies; copies agree on %.1f%% of bits)", packets.size(), copies, agree * 100));
    { std::string ms; for (auto &m : mlist) ms += (ms.empty() ? "" : ", ") + fmt("%X x%d", m.first, m.second); L.push_back(fmt("Addresses: %zu; markers: ", A.size()) + ms); }
    L.push_back(fmt("Bursts: %zu; a burst typically lasts ", br.size()) + (bl.empty() ? "None" : fmt("%g", round(median(bl) * per * 100) / 100)) +
                " s, pauses between bursts " + (bg.empty() ? "None" : fmt("%g", round(median(bg) * per * 100) / 100)) + " s");
    { std::string cs; for (auto &g : glist) cs += (cs.empty() ? "" : ", ") + fmt("%g s (%d times)", round(g.first * per * 10) / 10, g.second); L.push_back("Carousel: a packet comes again after " + cs); }
    L.push_back(""); L.push_back("Channels (by packet type):");
    for (auto &c : channels) {
        std::string ls; for (auto &l : c["lines"].a) ls += (ls.empty() ? "" : "/") + std::to_string(l.integer());
        std::string t = fmt("  type %s: %d packets, line %s, %d addresses \xE2\x80\x94 %s", c["marker"].str().c_str(), c["packets"].integer(), ls.c_str(), c["addresses"].integer(), c["kind"].str().c_str());
        if (c["kind"].str() == "carousel")
            t += fmt(": each packet sent %g times on average, again after ~%s s (%d bytes of data in all)", c["copies_avg"].num(), num_or_none(c["repeat_s"]).c_str(), c["bytes"].integer());
        else if (c.has("block")) {
            std::string bs; for (auto &b : c["block"].a) bs += (bs.empty() ? "" : " ") + b.str();
            std::string r0 = c.has("block_range_s") ? fmt("%g", c["block_range_s"][0].num()) : num_or_none(c["block_s"]);
            std::string r1 = c.has("block_range_s") ? fmt("%g", c["block_range_s"][1].num()) : num_or_none(c["block_s"]);
            t += fmt(": blocks of %zu packets (addresses %s) every %s\xE2\x80\x93%s s \xE2\x80\x94 %zu bytes per block, ~%s bit/s", c["block"].size(), bs.c_str(), r0.c_str(), r1.c_str(),
                     31 * c["block"].size(), c.has("bit_s") ? std::to_string(c["bit_s"].integer()).c_str() : "None");
        }
        L.push_back(t);
        if (c.has("fixed_bits") && c["fixed_bits"].size()) {
            std::string fb; for (auto &b : c["fixed_bits"].a) fb += (fb.empty() ? "" : ", ") + std::to_string(b.integer());
            L.push_back("      data bits that never change for an address (an open header field): " + fb);
        }
    }
    L.push_back("");
    if (!packets.empty()) L.push_back(fmt("Data: %.2f bits per byte (max %.2f for this many packets) \xE2\x80\x94 no structure: encrypted, cannot be read without the key", entropy, maxent));
    L.push_back("");
    if (!ct.empty()) {
        L.push_back("Carousel as a byte stream: the same bytes come again in other packets shifted by whole bytes, so the");
        L.push_back(fmt("carousel is a looping file; reassembled into %zu continuous fragments, %zu bytes in all, longest %zu bytes", ct.size(), ctot, ct[0].size()));
        L.push_back("(carousel_stream.bin / .txt). The file was coded as a whole before being cut into packets; it is dense");
        L.push_back("binary: no archive signature, not a raster, no self-synchronising scrambler (taps up to 25), no short LFSR");
        L.push_back("(Berlekamp-Massey), no async start/stop framing, no 256-byte table \xE2\x80\x94 encrypted or compressed.");
        L.push_back("");
    }
    size_t summary_end = L.size();
    L.push_back("Busiest addresses (address: packets, lines, repeat period):");
    for (size_t i = 0; i < abusy.size() && i < 25; i++) {
        auto &v = A[abusy[i]];
        L.push_back(fmt("  %03X: %4d packets, line ", abusy[i], v.packets) + join_ints(std::vector<int>(v.lines.begin(), v.lines.end()), "/") + (num_or_none(v.repeat_s) == "None" ? ", does not repeat" : ", repeats every " + num_or_none(v.repeat_s) + " s"));
    }
    std::string txt; for (auto &l : L) txt += l + "\n";
    write_text(path_join(out, "report.txt"), txt);
    // packets.json
    {
        Json j = Json::object(); j["source"] = basename(src);
        Json s = Json::object();
        s["packets"] = (int)packets.size(); s["copies"] = (int)copies; s["addresses"] = (int)A.size();
        Json ch = Json::array(); for (auto &c : channels) ch.push(c); s["channels"] = ch;
        s["contig_n"] = (int)ct.size(); s["contig_bytes"] = (int)ctot; s["unit_s"] = per;
        if (!packets.empty()) { s["payload_entropy"] = round(entropy * 100) / 100; s["max_entropy"] = round(maxent * 100) / 100; }
        j["summary"] = s;
        Json ad = Json::object();
        for (int a : aorder) {
            auto &v = A[a]; Json o = Json::object();
            o["packets"] = v.packets; o["copies"] = v.copies; Json lj = Json::array(); for (int l : v.lines) lj.push(l); o["lines"] = lj;
            o["first"] = v.first; o["last"] = v.last; o["repeat_s"] = v.repeat_s;
            ad[fmt("%03X", a)] = o;
        }
        j["addresses"] = ad;
        Json pj = Json::array();
        for (auto &p : packets) {
            Json o = Json::object(); Json u = Json::array(); for (int x : p.units) u.push(x); o["units"] = u;
            Json l = Json::array(); for (int x : p.lines) l.push(x); o["lines"] = l;
            o["address"] = fmt("%03X", p.address); o["payload"] = hex(p.payload, 0, 31); o["agree"] = round(p.agree * 1000) / 1000;
            pj.push(o);
        }
        j["packets"] = pj;
        write_text(path_join(out, "packets.json"), j.dump());
    }
    // index.html
    {
        double tot = R.n * per;
        std::string rows, bars, pk;
        for (int a : abusy) { auto &v = A[a]; rows += fmt("<tr><td>%03X</td><td>%d</td><td>%d</td><td>", a, v.packets, v.copies) + join_ints(std::vector<int>(v.lines.begin(), v.lines.end()), "/") + "</td><td>" + (v.repeat_s.is_null() ? "" : fmt("%g", v.repeat_s.num())) + "</td></tr>"; }
        for (auto &b : br) bars += fmt("<div class=\"b\" style=\"left:%.3f%%;width:%.3f%%\"></div>", 100 * b.first * per / tot, std::max(0.15, 100 * (b.second - b.first + 1) * per / tot));
        for (size_t i = 0; i < packets.size() && i < 3000; i++) {
            auto &p = packets[i];
            pk += fmt("<tr><td>%.2f</td><td>%zu</td><td>%03X</td><td>", p.units[0] * per, p.units.size(), p.address) + join_ints(p.lines, "/") + "</td><td class=\"h\">" + hex(p.payload, 0, 31, " ") + "</td></tr>";
        }
        std::string summ; for (size_t i = 0; i < summary_end; i++) summ += L[i] + "\n";
        std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Datacast</title><style>\n"
            "body{background:#111;color:#ccc;font:14px sans-serif;margin:16px} h2{color:#fa4}\n"
            "table{border-collapse:collapse;font:12px Consolas,monospace} td,th{border:1px solid #333;padding:2px 6px}\n"
            ".tl{position:relative;height:24px;background:#222;margin:8px 0} .b{position:absolute;top:0;bottom:0;background:#fa4}\n"
            ".h{color:#888} pre{white-space:pre-wrap;font:13px Consolas,monospace} .wrap{max-height:50vh;overflow:auto;display:inline-block}</style></head><body>\n"
            "<h2>Encrypted datacast \xE2\x80\x94 " + html_escape(basename(src)) + "</h2><pre>" + html_escape(strip(summ)) + "</pre>\n" +
            fmt("<p>Data bursts over the recording (%.0f s):</p><div class=\"tl\">", tot) + bars + "</div>\n"
            "<h2>Addresses</h2><div class=\"wrap\"><table><tr><th>address</th><th>packets</th><th>copies</th><th>VBI line</th><th>repeat, s</th></tr>" + rows + "</table></div>\n"
            "<h2>Packets</h2><div class=\"wrap\"><table><tr><th>time, s</th><th>copies</th><th>address</th><th>line</th><th>31 bytes of data (encrypted)</th></tr>" + pk + "</table></div>\n</body></html>";
        write_text(path_join(out, "index.html"), html);
    }
    pr.log(txt);
    pr.log(fmt("done in %.0f s, written to ", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()) + out);
    return out;
}
