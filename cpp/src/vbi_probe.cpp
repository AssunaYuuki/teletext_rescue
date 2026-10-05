#include "vbi_probe.h"
#include "datacast.h"
#include "nabts_slicer.h"
#include "silent_radio.h"
#include <complex>

const double FSC_NTSC = 315e6 / 88;
static const std::vector<std::pair<std::string, double>> RATES = {
    {"WST PAL teletext", 6.9375e6}, {"NABTS / WST NTSC rate", 8 * FSC_NTSC / 5}, {"VPS", 5e6},
    {"Silent Radio", FSC_NTSC * 7 / 20}, {"WSS", 5e6 / 6}, {"CC (line 21)", 32 * 15734.264}};
double zc_coherence(const float *y, double fs, double rate, int lo, int hi);
static double rate_of(const std::string &k) { for (auto &r : RATES) if (r.first == k) return r.second; return 0; }

// ---------------------------------------------------------------- доступ к строкам
// Геометрия захвата по чипам (bttv, cx88, saa7134, saa7131, ivtv/cx18, cx23885, em28xx)
// и .tbc (4 fsc, 16 бит, строка целиком от 0H).
static const double FSC_PAL4 = 4 * 4433618.75;
const std::vector<VbiFormat> &vbi_formats() {
    static const std::vector<VbiFormat> F = {
        // name             label                                         fs          len   ns   B  recs  fld    ntsc   f1  n1  f2  n2  s1 s2 scale t27 t0(мкс)
        {"bt8x8-pal",       "bt8x8 (bttv), PAL/SECAM",                    35468950.0, 2048, 2044, 1, 32,  false, false,  7, 16, 320, 16, 0, 0, 1,   0, 6.88},
        {"bt8x8-ntsc",      "bt8x8 (bttv), NTSC",                         28636363.0, 2048, 1600, 1, 32,  false, true,  10, 12, 273, 12, 0, 0, 1,   0, 8.52},
        {"cx88-pal",        "cx2388x (cx88), PAL/SECAM",                  35468950.0, 2048, 2048, 1, 36,  false, false,  6, 18, 319, 18, 0, 0, 1,   0, 6.88},
        {"cx88-ntsc",       "cx2388x (cx88), NTSC",                       28636363.0, 2048, 2048, 1, 24,  false, true,  10, 12, 273, 12, 0, 0, 1,   0, 8.52},
        {"saa7131",         "saa713x (saa7134/7131), PAL/SECAM",          27e6,       1440, 1440, 1, 32,  false, false,  7, 16, 320, 16, 0, 0, 1,   0, 9.48},
        {"d27-pal",         "ivtv / cx18 (27 MHz), PAL/SECAM",            27e6,       1440, 1440, 1, 36,  false, false,  6, 18, 318, 18, 0, 0, 1,   0, 9.19},
        {"cx23885",         "cx23885 / ivtv / cx18 / saa713x (27 MHz), NTSC", 27e6,   1440, 1440, 1, 12,  true,  true,  10, 12,   0,  0, 0, 0, 1,   0, 9.29},
        {"em28xx-pal",      "em28xx (13.5 MHz), PAL/SECAM",               13.5e6,      720,  720, 1, 36,  false, false,  6, 18, 318, 18, 0, 0, 1,   0, 9.3},
        {"em28xx-ntsc",     "em28xx (13.5 MHz), NTSC",                    13.5e6,      720,  720, 1, 24,  false, true,  10, 12, 273, 12, 0, 0, 1,   0, 9.3},
        {"4fsc16",          ".tbc VBI crop, NTSC",                        4 * 315e6 / 88, 910, 910, 2, 16, true, true,  14, 16,   0,  0, 0, 0, 64,  0.1, 0},
        {"4fsc16-pal-vbi",  ".tbc VBI crop, PAL",                         FSC_PAL4,   1135, 1135, 2, 16,  true,  false,  7, 16,   0,  0, 0, 0, 256, 0, 0},
        {"tbc-pal",         ".tbc full fields, PAL", FSC_PAL4, 1135, 1135, 2, 313, true, false, 6, 18,   0,  0, 5, 0, 256, 0, 0},
        {"tbc-ntsc",        ".tbc full fields, NTSC", 4 * 315e6 / 88, 910, 910, 2, 263, true, true, 10, 16, 0, 0, 9, 0, 64, 0.1, 0},
    };
    return F;
}
const VbiFormat *vbi_format(const std::string &name) {
    for (auto &f : vbi_formats()) if (name == f.name) return &f;
    return nullptr;
}

Rec::Rec(const std::string &p, const std::string &f) : path(p), fmt(f) {
    mf = std::make_shared<MappedFile>(p);
    uint64_t size = mf->size();
    const VbiFormat *F = vbi_format(fmt);
    if (!F) throw std::runtime_error("unknown recording format " + fmt);
    rec_lines = F->rec_lines; rec_len = F->rec_len; bytes_per = F->bytes_per; fs = F->fs; ns = F->ns;
    scale = F->scale; is_ntsc = F->ntsc; t27 = F->t27; t0_us = F->t0_us; unit = F->field_unit ? "field" : "frame";
    for (int i = 0; i < F->n1; i++) { rows.push_back(F->skip1 + i); tv[F->skip1 + i] = F->first1 + i; }
    for (int i = 0; i < F->n2; i++) { int r = rec_lines / 2 + F->skip2 + i; rows.push_back(r); tv[r] = F->first2 + i; }
    n = (int)(size / ((uint64_t)rec_lines * rec_len * bytes_per));
    if (fmt == "4fsc16") number_by_cc();
}

void Rec::line(int u, int r, float *out) const {
    uint64_t off = ((uint64_t)u * rec_lines + r) * rec_len * bytes_per;
    const u8 *p = mf->data() + off;
    if (bytes_per == 1) for (int i = 0; i < ns; i++) out[i] = p[i];
    else for (int i = 0; i < ns; i++) out[i] = (float)((p[2 * i] | (p[2 * i + 1] << 8)) / scale);
}
std::vector<float> Rec::line_h(int u, int r) const {
    auto y = line(u, r);
    if (fs == fs_h() && t0_us == 0) return y;
    int H = ns_h(); double fh = fs_h();
    std::vector<float> o(H);
    for (int k = 0; k < H; k++) {
        double x = (k / fh - t0_us * 1e-6) * fs;
        o[k] = (float)interp(std::clamp(x, 0.0, (double)ns - 1), y.data(), ns);
    }
    return o;
}
std::vector<float> Rec::line(int u, int r) const { std::vector<float> y(ns); line(u, r, y.data()); return y; }

static std::vector<int> linspace_int(double a, double b, int n) {
    std::vector<int> v;
    if (n <= 0) return v;
    if (n == 1) { v.push_back((int)a); return v; }
    for (int i = 0; i < n; i++) v.push_back((int)(a + (b - a) * i / (n - 1)));
    return v;
}

void Rec::number_by_cc() {
    auto uu = linspace_int(0, n - 2, std::min(60, n - 1));
    int best = 0, row = -1, b2[2];
    for (int r : rows)
        for (int par = 0; par < 2; par++) {
            int ok = 0;
            for (int u : uu) if (cc_slice(line(u + ((u % 2) != par), r), fs, b2)) ok++;
            if (ok > best) { best = ok; row = r; }
        }
    if (row >= 0 && best >= 0.5 * uu.size()) { for (int r : rows) tv[r] = 21 - row + r; return; }
    if (uu.size() > 40) uu.resize(40);
    int last = -1;
    for (int r : rows) {
        if (r >= 6) break;
        for (int par = 0; par < 2; par++) {
            std::vector<double> sy, bl, md;
            for (int u : uu) {
                auto y = line(u + ((u % 2) != par), r);
                sy.push_back(*std::min_element(y.begin(), y.begin() + 60));
                std::vector<double> mid(y.begin() + 200, y.begin() + 800); bl.push_back(median(mid));
                md.push_back(*std::min_element(y.begin() + 430, y.begin() + 490));
            }
            double sync = median(sy), black = median(bl), mid = median(md);
            if (black - sync > 20 && mid - sync < 0.3 * (black - sync)) last = r;
        }
    }
    if (last >= 0) for (int r : rows) tv[r] = 9 - last + r;
}

void resample27(const Rec &R, const std::vector<float> &y, float *out) {
    double mx = 1; for (float v : y) mx = std::max(mx, (double)v);
    int t0 = R.t27_start();
    for (int i = 0; i < 1440; i++) {
        double t = i / 27e6 * R.fs + t0;
        out[i] = (float)std::clamp(interp(t, y.data(), (int)y.size()) * (255.0 / mx), 0.0, 255.0);
    }
}

// ---------------------------------------------------------------- формат
static double structure(const std::function<std::vector<float>(int)> &sample, int n_units) {
    auto idx = linspace_int(0, n_units - 2, 40);
    std::vector<double> s;
    for (int i : idx) {
        auto x = sample(i), y = sample(i + 1);
        double mx = 0, my = 0; for (size_t k = 0; k < x.size(); k++) { mx += x[k]; my += y[k]; }
        mx /= x.size(); my /= y.size();
        double xy = 0, xx = 0, yy = 0;
        for (size_t k = 0; k < x.size(); k++) { double a = x[k] - mx, b = y[k] - my; xy += a * b; xx += a * a; yy += b * b; }
        s.push_back(xy / sqrt(xx * yy + 1e-9));
    }
    return median(s);
}

// Огибающая активности записей: при верной длине записи у чётных и нечётных записей она одна и та же,
// запись двойной длины (две строки) повторяет себя в половинах, половинной (пол-строки) — чередуется.
static double geometry_score(const u8 *a, uint64_t size, int rec_len, int bpp) {
    uint64_t rb = (uint64_t)rec_len * bpp, n = size / rb;
    if (n < 8) return 0;
    auto at = [&](uint64_t r, int k) -> double { const u8 *p = a + r * rb + (uint64_t)k * bpp; return bpp == 2 ? (p[0] | (p[1] << 8)) / 256.0 : p[0]; };
    int m = (int)std::min<uint64_t>(n, 3000);
    std::vector<uint64_t> idx; for (int i = 0; i < m; i++) idx.push_back((uint64_t)((double)i * (n - 1) / std::max(1, m - 1)));
    std::vector<std::vector<double>> act(m, std::vector<double>(rec_len));
    std::vector<double> lvl(m);
    for (int i = 0; i < m; i++) {
        std::vector<double> y(rec_len); for (int k = 0; k < rec_len; k++) y[k] = at(idx[i], k);
        std::vector<double> c = y; double med = median(c), s = 0;
        for (int k = 0; k < rec_len; k++) { act[i][k] = fabs(y[k] - med); s += act[i][k]; }
        lvl[i] = s / rec_len;
    }
    std::vector<double> l2 = lvl; double th = median(l2);
    std::vector<double> env(rec_len, 0), ev(rec_len, 0), od(rec_len, 0);
    for (int i = 0; i < m; i++) {
        if (lvl[i] <= th) continue;
        for (int k = 0; k < rec_len; k++) { env[k] += act[i][k]; (idx[i] % 2 ? od : ev)[k] += act[i][k]; }
    }
    auto corr = [](const double *x, const double *y, int L) {
        double mx = 0, my = 0; for (int i = 0; i < L; i++) { mx += x[i]; my += y[i]; } mx /= L; my /= L;
        double xy = 0, xx = 0, yy = 0;
        for (int i = 0; i < L; i++) { double p = x[i] - mx, q = y[i] - my; xy += p * q; xx += p * p; yy += q * q; }
        return xx > 0 && yy > 0 ? xy / sqrt(xx * yy) : 0.0;
    };
    int h = rec_len / 2;
    double selfsim = corr(env.data(), env.data() + h, h), evod = corr(ev.data(), od.data(), rec_len);
    return evod - 2 * std::max(0.0, selfsim - 0.6);
}

std::pair<std::string, Json> detect_format(const std::string &path) {
    uint64_t size = file_size(path);
    struct Cand { std::string name; double score; int rec_len; };
    std::vector<Cand> cand;
    MappedFile mf(path);
    const u8 *a = mf.data();
    // у bt8x8 NTSC отсчёты 1600–2047 каждой записи — нули; у bt8x8 PAL их нет
    bool bt_ntsc = false;
    if (size >= 2 * 65536) {
        int n = (int)(size / 65536);
        auto idx = linspace_int(0, n - 1, 20);
        long zero = 0, tot = 0;
        for (int i : idx) for (int l = 0; l < 31; l++) for (int k = 1604; k < 2040; k++) { zero += a[(uint64_t)i * 65536 + l * 2048 + k] == 0; tot++; }
        bt_ntsc = zero > 0.99 * tot;
    }
    for (auto &F : vbi_formats()) {
        uint64_t ub = (uint64_t)F.rec_len * F.bytes_per * F.rec_lines;
        bool bt = std::string(F.name).rfind("bt8x8", 0) == 0;
        if (!(size % ub == 0 || (bt && size % ub < 4))) continue;   // у bt8x8 в конце бывает обрывок счётчика кадров
        if (bt && bt_ntsc != F.ntsc) continue;
        int n = (int)(size / ub);
        if (n < 2) continue;
        Rec R(path, F.name);
        // нужные записи блока; у полных полей .tbc — только строки гашения
        auto unit = [&](int i) { std::vector<float> v; for (int r : R.rows) { auto y = R.line(i, r); v.insert(v.end(), y.begin(), y.end()); } return v; };
        double s = structure(unit, n);
        // соседние записи внутри блока похожи только при верной длине записи (гашение и вступления на одном месте)
        std::vector<double> al;
        for (int i : linspace_int(0, n - 1, 12))
            for (size_t k = 0; k + 1 < R.rows.size(); k++) {
                auto x = R.line(i, R.rows[k]), y = R.line(i, R.rows[k + 1]);
                double mx = 0, my = 0; for (int q = 0; q < R.ns; q++) { mx += x[q]; my += y[q]; }
                mx /= R.ns; my /= R.ns;
                double xy = 0, xx = 0, yy = 0;
                for (int q = 0; q < R.ns; q++) { double p = x[q] - mx, z = y[q] - my; xy += p * z; xx += p * p; yy += z * z; }
                if (xx > 1e-6 && yy > 1e-6) al.push_back(xy / sqrt(xx * yy));
            }
        double align = al.empty() ? 0 : median(al);
        double lw = 0;
        if (F.bytes_per == 2) {   // 16-битные .tbc: младшие биты нулевые (10-битный АЦП) или хотя бы гладкий сигнал
            long low = 0, tot = 0;
            for (uint64_t i = 0; i < std::min<uint64_t>(size / 2, 200000); i++) { low += (a[2 * i] & 63); tot++; }
            lw = (double)low / tot < 1 ? 1.0 : 0.0;
        }
        // такт известных служб (телетекст, NABTS, CC, VPS…) виден только при верной частоте дискретизации
        double svc = 0; std::string svc_rate;
        auto us = linspace_int(0, n - 1, 10);
        for (int r : R.rows)
            for (auto &rt : RATES) {
                if (rt.second * 1.9 > R.fs) continue;
                double c = 0;
                for (int u : us) { auto y = R.line(u, r); c += zc_coherence(y.data(), R.fs, rt.second, (int)(R.ns * 0.1), (int)(R.ns * 0.97)); }
                if (c / us.size() > svc) { svc = c / us.size(); svc_rate = rt.first; }
            }
        // служба с лучшим тактом называет систему: NABTS, CC, Silent Radio — NTSC; телетекст PAL, VPS, WSS — PAL
        bool svc_ntsc = svc_rate == "NABTS / WST NTSC rate" || svc_rate == "CC (line 21)" || svc_rate == "Silent Radio";
        double sys = svc >= 0.3 ? (svc_ntsc == F.ntsc ? 0.3 : -0.3) : 0;
        double geo = geometry_score(a, size, F.rec_len, F.bytes_per);
        geo = std::min(geo, 0.2);   // только штраф: у служб, разных на соседних строках, чёт/нечет и так расходятся
        // прочитать подписи CC на строке 21: при верном формате байты выходят с нечётной чётностью. Две записи по 720
        // из одной строки на 1440 тоже дают «такт» CC, но не дают читаемых байтов
        double ccs = 0;
        if (F.ntsc)
            for (int r : R.rows) {
                auto t = R.tv.find(r);
                if (t == R.tv.end() || (t->second != 21 && t->second != 284)) continue;
                int ok = 0, tot = 0, b[2];
                for (int u : linspace_int(0, n - 1, 200)) { tot++; if (cc_slice(R.line(u, r), R.fs, b) && (__builtin_popcount(b[0]) & 1) && (__builtin_popcount(b[1]) & 1)) ok++; }
                if (tot) ccs = std::max(ccs, (double)ok / tot);
            }
        if (getenv("TR_DEBUG")) fprintf(stderr, "%s: struct %.2f align %.2f lw %.0f svc %.2f (%s) sys %.1f geo %.2f cc %.2f\n", F.name, s, align, lw, svc, svc_rate.c_str(), sys, geo, ccs);
        cand.push_back({F.name, s + 0.5 * align + lw + svc + sys + geo + 0.6 * ccs, F.rec_len * F.bytes_per});
    }
    Json cj = Json::array();
    if (cand.empty()) return {"", cj};
    std::stable_sort(cand.begin(), cand.end(), [](auto &x, auto &y) { return x.score > y.score; });
    // почти равные оценки: строка из двух настоящих строк тоже «похожа» — берём более короткую запись
    for (size_t i = 1; i < cand.size(); i++)
        if (cand[i].score >= cand[0].score - 0.03 && cand[i].rec_len < cand[0].rec_len) std::swap(cand[0], cand[i]);
    for (auto &c : cand) { Json e = Json::array(); e.push(c.name); e.push(c.score); cj.push(e); }
    return {cand[0].name, cj};
}

// ---------------------------------------------------------------- признаки строки
static std::vector<double> box_same(const std::vector<double> &s, int w) {
    int n = (int)s.size(), h = (w - 1) / 2;
    std::vector<double> pre(n + 1, 0), out(n);
    for (int i = 0; i < n; i++) pre[i + 1] = pre[i] + s[i];
    for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n, i + h + 1); out[i] = (pre[b] - pre[a]) / w; }
    return out;
}
static int sgn(double v) { return (v > 0) - (v < 0); }

double zc_coherence(const float *y, double fs, double rate, int lo, int hi) {
    double T = fs / rate;
    std::vector<double> s(y + lo, y + hi);
    int w = std::max((int)(T * 8), (int)(fs * 2e-6)) | 1;
    double best = 0;
    std::vector<double> d1(s.size()), d2;
    double med = median(s);
    for (size_t i = 0; i < s.size(); i++) d1[i] = s[i] - med;
    auto bx = box_same(s, w); d2.resize(s.size());
    for (size_t i = 0; i < s.size(); i++) d2[i] = s[i] - bx[i];
    for (auto *d : {&d1, &d2}) {
        std::complex<double> acc = 0; double sw = 0; int nz = 0;
        for (size_t z = 0; z + 1 < d->size(); z++) {
            if (sgn((*d)[z + 1]) == sgn((*d)[z])) continue;
            double den = (*d)[z] - (*d)[z + 1];
            double t = z + (den != 0 ? (*d)[z] / den : 0);
            double sl = fabs((*d)[z + 1] - (*d)[z]);
            acc += sl * std::exp(std::complex<double>(0, 2 * M_PI * t / T)); sw += sl; nz++;
        }
        if (nz < 12) continue;
        best = std::max(best, std::abs(acc) / (sw + 1e-9));
    }
    return best;
}

static std::pair<double, double> sine_purity(const std::vector<std::vector<float>> &rows, int lo, int hi, double fs) {
    if (rows.empty()) return {0, 0};
    int M = hi - lo;
    std::vector<double> sp(M / 2 + 1, 0), x(M);
    for (auto &y : rows) {
        double m = 0; for (int i = lo; i < hi; i++) m += y[i]; m /= M;
        for (int i = 0; i < M; i++) x[i] = (y[lo + i] - m) * (0.5 - 0.5 * cos(2 * M_PI * i / (M - 1)));
        auto p = rfft_power(x);
        for (size_t k = 0; k < sp.size(); k++) sp[k] += p[k] / rows.size();
    }
    for (int k = 0; k < 3 && k < (int)sp.size(); k++) sp[k] = 0;
    int k = 0; double tot = 0;
    for (size_t q = 0; q < sp.size(); q++) { tot += sp[q]; if (sp[q] > sp[k]) k = (int)q; }
    double s = 0; for (int q = std::max(0, k - 3); q < std::min((int)sp.size(), k + 4); q++) s += sp[q];
    return {s / (tot + 1e-9), k * fs / M};
}

static std::pair<std::string, std::string> classify(double sig, double stat, const std::vector<std::pair<std::string, double>> &coh, int tvl) {
    if (sig < 0.01) return {"empty", ""};
    if (stat > 0.9) return {"test signal (VITS) or still picture", ""};
    if (coh.empty()) return {"unknown", ""};
    size_t bi = 0; for (size_t i = 1; i < coh.size(); i++) if (coh[i].second > coh[bi].second) bi = i;
    std::string name = coh[bi].first; double cn = coh[bi].second;
    auto sorted = coh;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return rate_of(a.first) < rate_of(b.first); });
    for (auto &k : sorted) {
        double q = rate_of(name) / rate_of(k.first);
        if (rate_of(k.first) < rate_of(name) && fabs(q - round(q)) < 0.02 && k.second >= 0.75 * cn) { name = k.first; cn = k.second; break; }
    }
    if (cn < 0.2) return {"picture or unknown signal", ""};
    if (name == "VPS" && tvl != 16) return {fmt("data at %.2f Mbit/s", rate_of(name) / 1e6), name};
    if (name == "WSS" && tvl != 23 && tvl != 336) return {fmt("data at %.2f Mbit/s", rate_of(name) / 1e6), name};
    return {name, name};
}

// ---------------------------------------------------------------- CC, AMOL, VITC
static bool polyfit1(const std::vector<double> &x, const std::vector<double> &y, double &a, double &b) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = (int)x.size();
    for (int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
    double den = n * sxx - sx * sx;
    if (n < 2 || den == 0) return false;
    a = (n * sxy - sx * sy) / den; b = (sy - a * sx) / n; return true;
}

static bool cc_slice_phase(const std::vector<float> &y, double T, int out[2]) {
    int n0 = 140, n1 = 360;
    double m = 0; for (int i = n0; i < n1; i++) m += y[i]; m /= (n1 - n0);
    double sd = 0; for (int i = n0; i < n1; i++) sd += (y[i] - m) * (y[i] - m); sd = sqrt(sd / (n1 - n0));
    if (sd < 4) return false;
    std::complex<double> acc = 0;
    for (int i = n0; i < n1; i++) acc += (y[i] - m) * std::exp(std::complex<double>(0, -2 * M_PI * i / T));
    double t0 = fmod(std::arg(acc) / (2 * M_PI) * T, T); if (t0 < 0) t0 += T;
    for (int k0 = (int)floor((300 - t0) / T); k0 <= (int)floor((420 - t0) / T); k0++) {
        double c0 = t0 + k0 * T;
        if (c0 + 18 * T >= y.size() - 5) continue;
        int b[19];
        for (int i = 0; i < 19; i++) b[i] = interp(c0 + i * T, y.data(), (int)y.size()) > m;
        if (!(b[0] == 0 && b[1] == 0 && b[2] == 1)) continue;
        int by[2];
        for (int j = 0; j < 2; j++) { by[j] = 0; for (int i = 0; i < 8; i++) by[j] |= b[3 + j * 8 + i] << i; }
        if (odd_parity(by[0]) && odd_parity(by[1])) { out[0] = by[0]; out[1] = by[1]; return true; }
    }
    return false;
}

bool cc_slice(const std::vector<float> &y, double fs, int out[2]) {
    double T = fs / (32 * 15734.264);
    int n = (int)y.size();
    if (n == 910) return cc_slice_phase(y, T, out);
    // где вступление (7 периодов синуса): у одних плат оно с 12 % строки, у обрезанных по активной части — почти с начала
    int a = (int)(n * 0.12), b = (int)(n * 0.45);
    {
        int L = (int)(7 * T), best_i = -1; double best = 0;
        for (int i = 0; i + L < n / 2; i += 4) {
            std::complex<double> acc = 0; double m = 0;
            for (int k = 0; k < L; k += 2) m += y[i + k];
            m /= (L + 1) / 2;
            for (int k = 0; k < L; k += 2) acc += (y[i + k] - m) * std::exp(std::complex<double>(0, -2 * M_PI * k / T));
            if (std::abs(acc) > best) { best = std::abs(acc); best_i = i; }
        }
        if (best_i >= 0 && best_i < a) { a = std::max(0, best_i); b = std::min(n, best_i + L + (int)(T / 2)); }
    }
    std::vector<double> seg(y.begin() + a, y.begin() + b);
    double lo = percentile(seg, 5), hi = percentile(seg, 95);
    if (hi - lo < 10) return false;
    double th = (lo + hi) / 2;
    std::vector<double> t;
    for (int i = a; i < b && i + 1 < n; i++) {
        double d0 = y[i] - th, d1 = y[i + 1] - th;
        if (sgn(d0) != sgn(d1)) t.push_back(i + (d0 - d1 != 0 ? d0 / (d0 - d1) : 0));
    }
    if (t.size() < 8) return false;
    std::vector<double> k(t.size());
    for (size_t i = 0; i < t.size(); i++) k[i] = round((t[i] - t[0]) / (T / 2));
    double pa, pb;
    if (!polyfit1(k, t, pa, pb) || fabs(pa - T / 2) > 0.1 * T) return false;
    double last = pb + *std::max_element(k.begin(), k.end()) * pa;
    for (double sh : {0.5, 1.5}) {
        double p0 = last + sh * T;
        if (p0 + 18 * T >= n - 1) return false;
        int bb[19]; for (int i = 0; i < 19; i++) bb[i] = interp(p0 + i * T, y.data(), n) > th;
        bool s3 = bb[0] == 0 && bb[1] == 0 && bb[2] == 1, s2 = bb[1] == 0 && bb[2] == 1;
        if (s3 || s2) {
            int s0 = s3 ? 3 : 2;
            for (int j = 0; j < 2; j++) { out[j] = 0; for (int i = 0; i < 8; i++) out[j] |= (interp(p0 + (j * 8 + i + s0) * T, y.data(), n) > th) << i; }
            return true;
        }
    }
    return false;
}

// Общий NRZ-срез: такт по фронтам, первый фронт — подъём первого бита «1».
static bool nrz_slice(const std::vector<float> &y, double T0, int a, int b, int nbits, uint8_t *bits, double &th_out) {
    int n = (int)y.size();
    std::vector<double> seg(y.begin() + a, y.begin() + b);
    double lo = percentile(seg, 10), hi = percentile(seg, 90);
    if (hi - lo < 8) return false;
    double th = (lo + hi) / 2; th_out = th;
    std::vector<double> t;
    for (int i = a; i < b && i + 1 < n; i++) {
        double d0 = y[i] - th, d1 = y[i + 1] - th;
        if (sgn(d0) != sgn(d1)) t.push_back(i + (d0 - d1 != 0 ? d0 / (d0 - d1) : 0));
    }
    if (t.size() < 6) return false;
    std::vector<double> k(t.size());
    for (size_t i = 0; i < t.size(); i++) k[i] = round((t[i] - t[0]) / T0);
    double pa, pb;
    if (!polyfit1(k, t, pa, pb) || fabs(pa - T0) > 0.06 * T0) return false;
    for (int i = 0; i < nbits; i++) {
        double pos = pb + (i + 0.5) * pa;
        if (pos >= n - 1) return false;
        bits[i] = interp(pos, y.data(), n) > th;
    }
    return true;
}

bool amol_slice(const std::vector<float> &y, double fs, uint8_t bits[48]) {
    int n = (int)y.size();
    int a = 0, b = n;
    if (n == 910 || n == 2044 || n == 1600) a = (int)(n * 0.15);      // строки с синхроимпульсом — после вспышки
    double th;
    for (double rate : {1.0e6, 64 * 15734.264}) {
        if (!nrz_slice(y, fs / rate, a, b, 48, bits, th)) continue;
        if (bits[0] == 1 && bits[1] == 0 && bits[2] == 1 && bits[3] == 0 && bits[4] == 1 && bits[5] == 1 && bits[6] == 0) return true;
    }
    return false;
}

bool vitc_slice(const std::vector<float> &y, double fs, double fh, uint8_t out[9]) {
    int n = (int)y.size();
    uint8_t bits[90]; double th;
    int a = (n == 910 || n == 2044 || n == 1600) ? (int)(n * 0.15) : 0;
    if (!nrz_slice(y, fs / (115 * fh), a, n, 90, bits, th)) return false;
    for (int g = 0; g < 9; g++) if (bits[g * 10] != 1 || bits[g * 10 + 1] != 0) return false;
    for (int g = 0; g < 9; g++) { int v = 0; for (int i = 0; i < 8; i++) v |= bits[g * 10 + 2 + i] << i; out[g] = (uint8_t)v; }
    // CRC: x^8 + 1 по всем 82 битам (кроме самой CRC) — сумма по модулю 2 со сдвигом
    uint8_t crc = 0;
    for (int i = 0; i < 82; i++) { int pos = i % 8; crc ^= bits[i] << pos; }
    int c = 0; for (int i = 0; i < 8; i++) c |= bits[82 + i] << i;
    (void)crc; (void)c;
    int fu = out[0] & 15, ft = out[1] & 3, su = out[2] & 15, st = out[3] & 7, mu = out[4] & 15, mt = out[5] & 7, hu = out[6] & 15, ht = out[7] & 3;
    return fu <= 9 && su <= 9 && st <= 5 && mu <= 9 && mt <= 5 && hu <= 9 && ht * 10 + hu < 24 && ft * 10 + fu < 30;
}

// ---------------------------------------------------------------- обзор
static double datacast_check_safe(const Rec &R, int row, int par) { try { return datacast_check(R, row, par); } catch (...) { return 0; } }

// Доля строк, где есть вступление + код кадра 0x27 телетекста PAL (корреляция ≥ 0,5) в одном и том же месте.
// Отличает телетекст с VHS (ограниченная полоса, срез по белому) от синусов и картинки,
// на которых такт по пересечениям нуля обманывается в обе стороны.
static double wst_cri_share(const Rec &R, int row, const std::vector<int> &uu, double *pos_out = nullptr) {
    double spb = R.fs / 6.9375e6, lead = spb;
    std::vector<int> bits;
    for (int b : {0x55, 0x55, 0x27}) for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1);
    int n = (int)ceil((1 + bits.size()) * spb);
    std::vector<double> t(n, -0.5);
    int prev = 0;
    for (size_t i = 0; i < bits.size(); i++) {
        if (bits[i] != prev)
            for (int x = 0; x < n; x++) t[x] += (bits[i] ? 1 : -1) * 0.5 * (1 + erf((x - (1 + (double)i) * spb) / (0.4 * spb * sqrt(2.0))));
        prev = bits[i];
    }
    double m = 0; for (double x : t) m += x; m /= n;
    double s = 0; for (auto &x : t) { x -= m; s += x * x; } s = sqrt(s); for (auto &x : t) x /= s;
    int hi = std::min(R.ns, (int)(700 * R.fs / 35468950.0) + n);
    std::vector<double> pos;
    std::vector<float> last;
    for (int u : uu) {
        auto y = R.line(u, row);
        // неподвижный сигнал (ИТС, multiburst) тоже даёт «вступление» на одном месте — такие строки не считаются
        bool same = false;
        if (!last.empty()) {
            double ma = 0, mb = 0; int m = R.ns;
            for (int q = 0; q < m; q++) { ma += y[q]; mb += last[q]; }
            ma /= m; mb /= m;
            double ab = 0, aa = 0, bb = 0;
            for (int q = 0; q < m; q++) { double x = y[q] - ma, z = last[q] - mb; ab += x * z; aa += x * x; bb += z * z; }
            same = ab / sqrt(aa * bb + 1e-9) > 0.9;
        }
        last = y;
        if (same) continue;
        double best = -1e18; int bi = 0;
        for (int i = 0; i + n <= hi; i++) {
            double c = 0, sy = 0, s2 = 0;
            for (int k = 0; k < n; k++) { c += y[i + k] * t[k]; sy += y[i + k]; s2 += (double)y[i + k] * y[i + k]; }
            double r = c / sqrt(std::max(s2 - sy * sy / n, 1e-9));
            if (r > best) { best = r; bi = i; }
        }
        if (best >= 0.5) pos.push_back(bi + lead);
    }
    if (pos.empty()) return 0;
    double med = median(pos); int ok = 0;
    for (double p : pos) ok += fabs(p - med) <= 4 * spb / 5.11;
    if (pos_out) *pos_out = med - lead;   // начало шаблона (за бит до вступления)
    return (double)ok / uu.size();
}

static void confirm(const Rec &R, Json &res, Progress &pr) {
    for (auto &L : res["lines"].a) {
        std::string k = L["kind"].str();
        int row = L["row"].integer(), par = L["parity"].is_null() ? -1 : L["parity"].integer();
        int step = std::max(1, R.n / 400);
        if (par >= 0) step += step % 2;
        std::vector<int> uu;
        for (int u = par >= 0 ? par : 0; u < R.n && uu.size() < 300; u += step) uu.push_back(u);
        // телетекст PAL, который не узнали по такту (VHS: срез по белому, строка то с ИТС, то с телетекстом).
        // Обратно не понижаем: у изношенной головки вступления почти нет, а строка всё равно телетекст.
        if (!R.ntsc() && (k.rfind("picture", 0) == 0 || k.rfind("test signal (VITS)", 0) == 0 || k == "unknown" ||
                          k.rfind("interference", 0) == 0 || k.rfind("data at", 0) == 0)) {
            std::vector<int> u60;
            for (size_t i = 0; i < uu.size() && u60.size() < 60; i += std::max<size_t>(1, uu.size() / 60)) u60.push_back(uu[i]);
            double sh = wst_cri_share(R, row, u60);
            if (sh >= 0.25) { k = "WST PAL teletext"; L["kind"] = k; L["rate"] = k; L["signal"] = round(sh * 1000) / 1000; }
        }
        if (k == "Silent Radio") {
            double T = R.fs_h() / SR_BITRATE; int ok = 0; uint8_t b[16];
            for (int u : uu) ok += sr_slice_line(R.line_h(u, row), T, b);
            L["read"] = round(1000.0 * ok / std::max<size_t>(1, uu.size())) / 1000;
            if (ok < 0.05 * uu.size()) L["kind"] = "data at 1.25 Mbit/s (not Silent Radio framing)";
        } else if (k == "CC (line 21)") {
            int ok = 0, b[2];
            for (int u : uu) ok += cc_slice(R.line(u, row), R.fs, b);
            L["read"] = round(1000.0 * ok / std::max<size_t>(1, uu.size())) / 1000;
            if (ok < 0.05 * uu.size()) L["kind"] = "data at 0.5 Mbit/s (not CC framing)";
        } else if (k == "NABTS / WST NTSC rate") {
            std::vector<int> u60(uu.begin(), uu.begin() + std::min<size_t>(60, uu.size()));
            double r = nabts_check(R, row, u60);
            double r5 = R.ntsc() ? wst525_check(R, row, u60) : 0;
            if (r5 >= 0.3 && r5 > r) { L["read"] = r5; L["kind"] = "WST 525-line teletext"; }
            else if (r >= 0.3) { L["read"] = r; L["kind"] = "NABTS"; }
            else {
                double r2 = datacast_check_safe(R, row, par);
                if (r2 >= 0.15) { L["read"] = r2; L["kind"] = "encrypted datacast (5.73 Mbit/s, PBS National Datacast?)"; }
                else if (L["base_kind"].str() != k) { L["kind"] = L["base_kind"]; L["read"] = Json(); }
                else { L["read"] = std::max(r, r5); L["kind"] = "data at 5.73 Mbit/s (not NABTS; scrambled or unknown format)"; }
            }
        }
    }
    bool any_nabts = false;
    for (auto &L : res["lines"].a) any_nabts |= L["kind"].str() == "NABTS";
    if (any_nabts)
        for (auto &L : res["lines"].a)
            if (L["kind"].str().rfind("data at 5.73", 0) == 0 && L.get_num("read") >= 0.1) L["kind"] = "NABTS";
    if (!R.ntsc())
        for (auto &L : res["lines"].a)
            if (L["kind"].str() == "NABTS") L["label"] = "NABTS / Didon packets (55 55 E7; Antiope transport, SECAM)";
    // AMOL (Nielsen, строки 20/22) и VITC — на строках с сигналом, где служба не опознана
    double fh = R.ntsc() ? 15734.264 : 15625.0;
    for (auto &L : res["lines"].a) {
        std::string k = L["kind"].str();
        if (k == "empty" || k == "NABTS" || k == "CC (line 21)" || k == "Silent Radio" || k == "WST 525-line teletext" ||
            k == "WST PAL teletext" || k.rfind("encrypted", 0) == 0 || L.get_num("signal") < 0.01) continue;
        int row = L["row"].integer(), par = L["parity"].is_null() ? -1 : L["parity"].integer();
        std::vector<int> uu;
        int step = std::max(1, R.n / 200); if (par >= 0) step += step % 2;
        for (int u = par >= 0 ? par : 0; u < R.n && uu.size() < 120; u += step) uu.push_back(u);
        int am = 0, vi = 0; uint8_t b48[48], b9[9];
        for (int u : uu) { auto y = R.line(u, row); am += amol_slice(y, R.fs, b48); vi += vitc_slice(y, R.fs, fh, b9); }
        double ra = (double)am / std::max<size_t>(1, uu.size()), rv = (double)vi / std::max<size_t>(1, uu.size());
        if (ra >= 0.3 && ra >= rv) { L["kind"] = "AMOL (Nielsen programme ID and time stamp)"; L["read"] = round(ra * 1000) / 1000; }
        else if (rv >= 0.3) { L["kind"] = "VITC timecode"; L["read"] = round(rv * 1000) / 1000; }
    }
    for (auto &L : res["lines"].a) {
        std::string par = L["parity"].is_null() ? "" : std::string(" field ") + "AB"[L["parity"].integer()];
        std::string lab = L.has("label") ? L["label"].str() : L["kind"].str();
        pr.log(fmt("  row %2d line %3d%s: %-55s signal %4.0f%%", L["row"].integer(), L["tv_line"].integer(), par.c_str(), lab.c_str(), L["signal"].num() * 100) +
               (L["read"].is_null() || !L.has("read") ? "" : fmt("  read %.0f%%", L["read"].num() * 100)));
    }
}

Json vbi_probe(const std::string &path, Progress &pr, int units) {
    auto fc = detect_format(path);
    Json res = Json::object();
    res["file"] = abspath(path);
    if (fc.first.empty()) { res["format"] = Json(); res["lines"] = Json::array(); return res; }
    Rec R(path, fc.first);
    pr.log(fmt("format: %s \xE2\x80\x94 %s (%d %ss, %.1f MHz, %d samples per line)", R.fmt.c_str(), vbi_format(R.fmt)->label, R.n, R.unit.c_str(), R.fs / 1e6, R.ns));
    auto us = linspace_int(0, R.n - 2, std::min(units, R.n - 1));
    int lo = (int)(R.ns * 0.1), hi = (int)(R.ns * 0.97);
    if (R.fmt == "4fsc16") lo = 140;
    std::vector<double> allv, stds;
    for (size_t i = 0; i < std::min<size_t>(8, us.size()); i++)
        for (int k : R.rows) {
            auto y = R.line(us[i], k);
            std::vector<double> s(y.begin() + lo, y.begin() + hi);
            allv.insert(allv.end(), s.begin(), s.end()); stds.push_back(stdev(s));
        }
    double span = percentile(allv, 99.5) - percentile(allv, 0.5);
    double thr = std::max(0.05 * span, 3 * percentile(stds, 10));
    Json lines = Json::array();
    bool fld = R.unit == "field";
    int nk = 0, nkeys = (int)R.rows.size() * (fld ? 2 : 1);
    for (int r : R.rows)
        for (int par = fld ? 0 : -1; par <= (fld ? 1 : -1); par++) {
            pr.progress(nk++, nkeys, "looking at the lines");
            std::vector<int> uu;
            for (int u : us) if (par < 0 || u % 2 == par) uu.push_back(u);
            std::vector<std::vector<float>> Y, Y2;
            for (int u : uu) Y.push_back(R.line(u, r));
            for (int u : uu) if (u + 2 < R.n) Y2.push_back(R.line(u + (par >= 0 ? 2 : 1), r));
            if (Y2.empty()) Y2.push_back(R.line(uu[0], r));
            std::vector<double> act_lvl; std::vector<char> act;
            for (auto &y : Y) { std::vector<double> s(y.begin() + lo, y.begin() + hi); act_lvl.push_back(stdev(s)); act.push_back(act_lvl.back() > thr); }
            double sig = 0; for (char a : act) sig += a; sig /= std::max<size_t>(1, act.size());
            std::vector<double> cc;
            for (size_t i = 0; i < Y2.size() && i < Y.size(); i++) {
                if (!act[i]) continue;
                double ma = 0, mb = 0; int m = hi - lo;
                for (int q = lo; q < hi; q++) { ma += Y[i][q]; mb += Y2[i][q]; }
                ma /= m; mb /= m;
                double ab = 0, aa = 0, bb = 0;
                for (int q = lo; q < hi; q++) { double x = Y[i][q] - ma, z = Y2[i][q] - mb; ab += x * z; aa += x * x; bb += z * z; }
                cc.push_back(ab / sqrt(aa * bb + 1e-9));
            }
            double stat = cc.empty() ? 0.0 : median(cc);
            std::vector<std::pair<std::string, double>> coh;
            std::vector<std::vector<float>> arows;
            for (size_t i = 0; i < Y.size() && arows.size() < 40; i++) if (act[i]) arows.push_back(Y[i]);
            if (sig > 0.01)
                for (auto &rt : RATES) {
                    double s = 0; for (auto &y : arows) s += zc_coherence(y.data(), R.fs, rt.second, lo, hi);
                    coh.push_back({rt.first, arows.empty() ? 0 : s / arows.size()});
                }
            int tvl = R.tv[r];
            auto kb = classify(sig, stat, coh, tvl);
            std::string kind = kb.first, best = kb.second;
            double cbest = 0; for (auto &c : coh) if (c.first == best) cbest = c.second;
            // строка с явным тактом телетекста PAL — данные, даже если в спектре выделяется один пик
            // (повторяющийся заполнитель); у полос SECAM лучший такт другой (синус совпадает с 5,73 Мбит/с)
            bool clear_wst = best == "WST PAL teletext" && cbest >= 0.4;
            if (!best.empty() && stat <= 0.9 && !clear_wst) {
                auto pf = sine_purity(arows, lo, hi, R.fs);
                if (pf.first > 0.5) {
                    if (!R.ntsc() && ((tvl >= 7 && tvl <= 15) || (tvl >= 320 && tvl <= 328))) kind = fmt("test signal: SECAM colour identification (%.2f MHz)", pf.second / 1e6);
                    else kind = fmt("interference (%.2f MHz sine), not data", pf.second / 1e6);
                    best = "";
                }
            }
            if ((tvl == 21 || tvl == 284) && sig > 0.01 && best.empty()) {
                int ok = 0, b2[2]; size_t m = std::min<size_t>(40, Y.size());
                for (size_t i = 0; i < m; i++) ok += cc_slice(Y[i], R.fs, b2);
                if (ok >= 0.3 * m) { kind = "CC (line 21)"; best = ""; }
            }
            // NTSC: строки 22–25 (285–288) — уже начало картинки (у кропа .tbc это записи 13–15)
            if (R.ntsc() && ((tvl >= 22 && tvl <= 25) || (tvl >= 285 && tvl <= 288)) && best.empty() &&
                (kind.rfind("test signal", 0) == 0 || kind.rfind("picture", 0) == 0 || kind == "unknown"))
                kind = "start of the picture (not VBI)";
            std::string base_kind = kind;
            if (kind.rfind("test signal", 0) == 0 || kind == "empty") {
                double md = median(act_lvl);
                std::vector<const std::vector<float> *> big;
                for (size_t i = 0; i < Y.size(); i++) if (act_lvl[i] > 2.0 * md) big.push_back(&Y[i]);
                if (big.size() >= 6) {
                    double c = 0; size_t m = std::min<size_t>(30, big.size());
                    for (size_t i = 0; i < m; i++) c += zc_coherence(big[i]->data(), R.fs, rate_of("NABTS / WST NTSC rate"), lo, hi);
                    if (c / m > 0.2) { kind = best = "NABTS / WST NTSC rate"; sig = round(1000.0 * big.size() / Y.size()) / 1000; }
                }
            }
            if (!best.empty()) {
                double base = 0; for (auto &c : coh) if (c.first == best) base = c.second;
                int ok = 0; size_t m = std::min<size_t>(160, Y.size());
                for (size_t i = 0; i < m; i++) ok += zc_coherence(Y[i].data(), R.fs, rate_of(best), lo, hi) > 0.5 * base;
                sig = (double)ok / std::max<size_t>(1, m);
            }
            Json L = Json::object();
            L["row"] = r; L["parity"] = par < 0 ? Json() : Json(par); L["tv_line"] = tvl; L["base_kind"] = base_kind;
            L["signal"] = round(sig * 1000) / 1000; L["static"] = round(stat * 1000) / 1000;
            L["rate"] = best.empty() ? Json() : Json(best); L["kind"] = kind;
            Json cj = Json::object(); for (auto &c : coh) cj[c.first] = round(c.second * 1000) / 1000;
            L["coherence"] = cj;
            lines.push(L);
        }
    res["format"] = R.fmt; res["units"] = R.n; res["unit"] = R.unit; res["candidates"] = fc.second; res["lines"] = lines;
    confirm(R, res, pr);
    return res;
}

// ---------------------------------------------------------------- телетекст PAL с любого чипа -> сетка bt8x8
static double cubic(double x, const float *y, int n) {
    int i = (int)floor(x);
    if (i < 1 || i + 2 >= n) return interp(x, y, n);
    double t = x - i, p0 = y[i - 1], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2];
    return p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
}

void wst_to_bt8x8(const Rec &R, const std::string &out, Progress &pr) {
    const double BT_FS = 35468950.0, BT_START = 110;   // где у bt8x8 начинается вступление (замер по пяти записям)
    bool fld = R.unit == "field";
    // положение вступления в записи источника
    std::vector<double> pos;
    for (int r : R.rows) {
        std::vector<int> uu;
        for (int u : linspace_int(0, R.n - 1, 40)) uu.push_back(u);
        double p = 0;
        if (wst_cri_share(R, r, uu, &p) >= 0.25) pos.push_back(p);
    }
    if (pos.empty()) throw std::runtime_error("no teletext run-in found on any line of the recording");
    double src0 = median(pos), k = R.fs / BT_FS;
    pr.log(fmt("%s: teletext starts at sample %.1f; resampling %.2f MHz -> 35.47 MHz (bt8x8 layout)", R.fmt.c_str(), src0, R.fs / 1e6));
    // строка ТВ -> запись источника (у форматов «по полю» поле B пронумеровано как поле A)
    std::map<int, int> by_tv; for (auto &kv : R.tv) by_tv[kv.second] = kv.first;
    int frames = fld ? R.n / 2 : R.n;
    FILE *f = _wfopen(P(out).c_str(), L"wb");
    if (!f) throw std::runtime_error("cannot write " + out);
    std::vector<u8> fr(65536);
    std::vector<float> y(R.ns);
    for (int q = 0; q < frames; q++) {
        std::fill(fr.begin(), fr.end(), 0);
        for (int b = 0; b < 32; b++) {
            int tvl = b < 16 ? 7 + b : 320 + b - 16, u = q, key = tvl;
            if (fld) { u = 2 * q + (b >= 16); if (b >= 16) key = tvl - 313; }
            auto it = by_tv.find(key);
            if (it == by_tv.end()) continue;
            R.line(u, it->second, y.data());
            u8 *d = fr.data() + b * 2048;
            for (int j = 0; j < 2044; j++) d[j] = (u8)std::clamp(lround(cubic(src0 + (j - BT_START) * k, y.data(), R.ns)), 0L, 255L);
        }
        uint32_t c = (uint32_t)q; memcpy(fr.data() + 65532, &c, 4);
        if (fwrite(fr.data(), 1, fr.size(), f) != fr.size()) { fclose(f); throw std::runtime_error("cannot write " + out); }
        if (q % 500 == 0) pr.progress(q, frames, "converting to the bt8x8 layout");
    }
    fclose(f);
}
