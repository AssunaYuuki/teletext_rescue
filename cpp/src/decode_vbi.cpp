#include "decode_vbi.h"
#include "opencl.h"
#include "pagebuild.h"
#include "teletext.h"
#include <chrono>
#include <atomic>
#include <mutex>

float line_std(const u8 *y, int n) {
    double s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += y[i]; s2 += (double)y[i] * y[i]; }
    double m = s / n; return (float)sqrt(std::max(0.0, s2 / n - m * m));
}

int wst_par_bad(const u8 *p) { int n = 0; for (int k = 2; k < 42; k++) n += !odd_parity(p[k]); return n; }
bool wst_confident(const u8 *p) { return ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0 && wst_par_bad(p) <= 1; }
static int ham_fix_ok(u8 x) { return ham::fix[x] >= 0; }
static int score(const u8 *p) { return wst_par_bad(p) + 20 * (!ham_fix_ok(p[0]) + !ham_fix_ok(p[1])); }

MlseModel wst_mlse_model() {
    MlseModel m;
    m.spb = 35.46895 / 6.9375; m.pre[0] = 0x55; m.pre[1] = 0x55; m.pre[2] = 0x27; m.nbytes = 42;
    m.L = 3; m.R = 3; m.ntop = 3; m.refits = 2;
    m.good = [](const u8 *p) { return wst_confident(p); };
    return m;
}

VbiReader::VbiReader(const std::string &path, int lpf_, bool use_gpu, Progress &pr_)
    : mf(path), lpf(lpf_), pr(pr_), dec(tpl::base_templates()) {
    N = (int)(mf.size() / ((uint64_t)lpf * 2048));
    if (use_gpu && gpu::available()) {
        try { gpu.reset(new tpl::GpuDecoder(dec.templates())); pr.log("decoding on the GPU: " + gpu->name()); }
        catch (std::exception &e) { pr.log(std::string("GPU unavailable (") + e.what() + ") \xE2\x80\x94 decoding on the CPU"); }
    }
}
VbiReader::~VbiReader() {}

void VbiReader::set_templates(const tpl::Templates &T) { dec.set_templates(T); if (gpu) gpu->set_templates(T); }

std::vector<std::pair<int, int>> VbiReader::sample(int n_frames, int n_lines) {
    std::mt19937 rng(0);
    std::vector<int> fs(N); for (int i = 0; i < N; i++) fs[i] = i;
    std::shuffle(fs.begin(), fs.end(), rng);
    fs.resize(std::min(N, n_frames)); std::sort(fs.begin(), fs.end());
    std::vector<std::pair<int, int>> cand;
    for (int f : fs) for (int l = 0; l < lpf; l++) if (line_std(line(f, l), 2048) > 20) cand.push_back({f, l});
    std::shuffle(cand.begin(), cand.end(), rng);
    if ((int)cand.size() > n_lines) cand.resize(n_lines);
    return cand;
}

std::vector<LineRes> VbiReader::tpl_lines(const std::vector<std::pair<int, int>> &sel) {
    std::vector<LineRes> out(sel.size());
    if (gpu) {
        for (size_t a = 0; a < sel.size(); a += 20000) {
            size_t n = std::min<size_t>(20000, sel.size() - a);
            std::vector<float> L(n * 2048);
            for (size_t i = 0; i < n; i++) { const u8 *y = line(sel[a + i].first, sel[a + i].second); for (int k = 0; k < 2048; k++) L[i * 2048 + k] = y[k]; }
            auto r = gpu->decode(L, n, win_lo, win_hi);
            for (size_t i = 0; i < n; i++) out[a + i] = {sel[a + i].first, sel[a + i].second, r[i].data, r[i].off, true};
            pr.progress(a + n, sel.size());
        }
        return out;
    }
    std::atomic<size_t> done{0};
    parallel_for(sel.size(), [&](size_t i) {
        auto y = tpl::norm(line(sel[i].first, sel[i].second), 2048);
        auto r = dec.decode_fast(y, win_lo, win_hi);
        out[i] = {sel[i].first, sel[i].second, r.data, r.off, true};
        size_t d = ++done;
        if (d % 200 == 0) pr.progress(d, sel.size());
    }, &pr, 8);
    return out;
}

std::vector<LineRes> VbiReader::mlse_lines(const std::vector<std::pair<int, int>> &sel, double lo, double hi) {
    std::vector<LineRes> out(sel.size());
    MlseModel m = wst_mlse_model();
    if (gpu && sel.size() > 1500) {
        // много строк: модель канала копится на первых строках (процессор), остальное — пачками на видеокарте
        MlseDecoder D(m);
        std::vector<float> y(2048);
        for (size_t i = 0; i < warm.size() && D.n <= 150; i++) {
            const u8 *src = line(warm[i].first, warm[i].second);
            for (int k = 0; k < 2048; k++) y[k] = src[k];
            D.decode(y.data(), 2048, lo, hi, 0.5);
        }
        size_t w = 0;
        for (; w < sel.size() && w < 300 && D.n <= 150; w++) {
            const u8 *src = line(sel[w].first, sel[w].second);
            for (int k = 0; k < 2048; k++) y[k] = src[k];
            auto r = D.decode(y.data(), 2048, lo, hi, 0.5);
            out[w] = {sel[w].first, sel[w].second, {}, r.off, true};
            memcpy(out[w].b.data(), r.data.data(), 42);
        }
        if (D.n >= 30) {
            MlseBatch bd(m, D.prior, D.n, true, pr);
            if (bd.on_gpu()) {
                std::vector<float> buf;
                for (size_t a = w; a < sel.size(); a += bd.batch()) {
                    size_t n = std::min(bd.batch(), sel.size() - a);
                    buf.resize(n * 2048);
                    std::vector<const float *> rp(n);
                    for (size_t i = 0; i < n; i++) { const u8 *src = line(sel[a + i].first, sel[a + i].second); for (int k = 0; k < 2048; k++) buf[i * 2048 + k] = src[k]; rp[i] = &buf[i * 2048]; }
                    auto res = bd.decode(rp, 2048, lo, hi, nullptr, 0.5);
                    for (size_t i = 0; i < n; i++) { out[a + i] = {sel[a + i].first, sel[a + i].second, {}, res[i].off, true}; memcpy(out[a + i].b.data(), res[i].data.data(), 42); }
                    pr.progress(a + n, sel.size(), "MLSE decoder (GPU)");
                }
                return out;
            }
        }
    }
    size_t nch = (sel.size() + 39) / 40;
    std::atomic<size_t> done{0};
    parallel_for(nch, [&](size_t c) {
        MlseDecoder D(m);
        std::vector<float> y(2048);
        for (size_t i = c * 40; i < std::min(sel.size(), c * 40 + 40); i++) {
            const u8 *src = line(sel[i].first, sel[i].second);
            for (int k = 0; k < 2048; k++) y[k] = src[k];
            auto r = D.decode(y.data(), 2048, lo, hi, 0.5);
            LineRes lr{sel[i].first, sel[i].second, {}, r.off, true};
            memcpy(lr.b.data(), r.data.data(), 42);
            out[i] = lr;
        }
        size_t d = done += 40;
        pr.progress(std::min(d, sel.size()), sel.size(), "MLSE decoder");
    }, &pr);
    return out;
}

int VbiReader::train(const std::vector<LineRes> &res) {
    std::vector<std::tuple<std::vector<float>, double, std::vector<u8>>> S;
    for (auto &r : res)
        if (wst_confident(r.b.data())) S.emplace_back(tpl::norm(line(r.f, r.l), 2048), r.off, bits_lsb(r.b.data(), 42));
    if (S.size() >= 300) set_templates(tpl::train_zoned(S));
    return (int)S.size();
}

// вступление + код кадра 0x27, размытые гауссом; среднее 0, норма 1
static std::vector<double> cri_template(double spb, double &lead_off, double blur = 0.4, int lead = 1) {
    std::vector<int> bits;
    for (int b : {0x55, 0x55, 0x27}) for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1);
    int n = (int)ceil((lead + bits.size()) * spb);
    std::vector<double> v(n, -0.5);
    int prev = 0;
    for (size_t i = 0; i < bits.size(); i++) {
        if (bits[i] != prev)
            for (int x = 0; x < n; x++) v[x] += (bits[i] ? 1 : -1) * 0.5 * (1 + erf((x - (lead + (double)i) * spb) / (blur * spb * sqrt(2.0))));
        prev = bits[i];
    }
    double m = 0; for (double x : v) m += x; m /= n;
    double s = 0; for (auto &x : v) { x -= m; s += x * x; }
    s = sqrt(s); for (auto &x : v) x /= s;
    lead_off = lead * spb;
    return v;
}
static std::pair<double, int> cri_peak(const u8 *y, const std::vector<double> &t, int hi = 700) {
    int n = (int)t.size(); double best = -1e18; int bi = 0;
    for (int i = 0; i + n <= hi; i++) {
        double c = 0, s = 0, s2 = 0;
        for (int k = 0; k < n; k++) { c += y[i + k] * t[k]; s += y[i + k]; s2 += (double)y[i + k] * y[i + k]; }
        double sd = sqrt(std::max(s2 - s * s / n, 1e-9));
        double r = c / sd;
        if (r > best) { best = r; bi = i; }
    }
    return {best, bi};
}

double VbiReader::calibrate(const std::vector<std::pair<int, int>> &cand) {
    double lead; auto t = cri_template(tpl::SPB, lead);
    std::vector<double> pos;
    size_t n = std::min<size_t>(cand.size(), 1500);
    std::mutex mu;
    parallel_for(n, [&](size_t i) {
        auto r = cri_peak(line(cand[i].first, cand[i].second), t);
        if (r.first >= 0.5) { std::lock_guard<std::mutex> lk(mu); pos.push_back(r.second + lead); }
    });
    if (pos.size() < 30) return -1;
    int mx = 0; for (double p : pos) mx = std::max(mx, (int)lround(p));
    std::vector<int> h(mx + 1, 0); for (double p : pos) h[lround(p)]++;
    int best = 0, m = 0;
    for (int i = 0; i <= mx; i++) { int s = 0; for (int k = i - 4; k <= i + 4; k++) if (k >= 0 && k <= mx) s += h[k]; if (s > best) { best = s; m = i; } }
    std::vector<double> near;
    for (double p : pos) if (fabs(p - m) <= 8) near.push_back(p);
    if (near.size() < 30) return -1;
    double med = median(near);
    std::vector<double> dev; for (double p : near) dev.push_back(fabs(p - med));
    pr.log(fmt("run-in found on %zu of %zu sampled lines, offset %.1f samples (spread %.1f)", pos.size(), n, med, median(dev) * 1.4826));
    return med;
}

double VbiReader::prepare(const std::vector<std::pair<int, int>> &cand, bool train_) {
    auto rate = [](const std::vector<LineRes> &r) { size_t k = 0; for (auto &x : r) k += wst_confident(x.b.data()); return (double)k / std::max<size_t>(1, r.size()); };
    pr.step("calibrating the offset from the run-in");
    double off = calibrate(cand);
    if (off >= 0) { win_lo = off - 6; win_hi = off + 6; }
    pr.step("checking with the base templates");
    double r0 = rate(tpl_lines(cand));
    pr.log(fmt("base templates: %.0f%% of sampled lines read confidently", r0 * 100));
    double rm = -1;
    if (r0 < 0.3) {
        pr.step("unfamiliar recording: calibrating with the MLSE decoder");
        std::vector<std::pair<int, int>> sub(cand.begin(), cand.begin() + std::min<size_t>(1200, cand.size()));
        auto m = mlse_lines(sub, 90.0, 135.0);
        std::vector<LineRes> ok;
        for (auto &x : m) if (wst_confident(x.b.data())) ok.push_back(x);
        rm = (double)ok.size() / std::max<size_t>(1, m.size());
        pr.log(fmt("MLSE decoder without training: %.0f%% of sampled lines read confidently", rm * 100));
        if (ok.size() >= 100) {
            warm.clear(); for (auto &x : ok) warm.push_back({x.f, x.l});
            std::vector<double> offs; for (auto &x : ok) offs.push_back(x.off);
            double med = median(offs);
            win_lo = med - 6; win_hi = med + 6;
            pr.log(fmt("packet start offset: %.1f samples (search window %.0f\xE2\x80\x93%.0f)", med, win_lo, win_hi));
            std::vector<LineRes> refined(ok.size());
            parallel_for(ok.size(), [&](size_t i) {
                auto &x = ok[i];
                auto y = tpl::norm(line(x.f, x.l), 2048);
                std::vector<u8> bits = bits_lsb((const u8 *)"\x55\x55\x27", 3);
                auto d = bits_lsb(x.b.data(), 42); bits.insert(bits.end(), d.begin(), d.end());
                double o = tpl::best_offset(y, bits, x.off - 3, x.off + 3, 1.0);
                o = tpl::best_offset(y, bits, o - 1, o + 1, 0.25);
                refined[i] = x; refined[i].off = o;
            }, &pr);
            train(refined);
        } else if (r0 < 0.03) return -1;
    }
    double rt = r0;
    if (train_) {
        auto res = tpl_lines(cand); rt = rate(res);
        for (int it = 1; it < 6; it++) {
            pr.step(fmt("adapting templates to the recording, pass %d", it));
            if (train(res) < 300) { pr.log("too few confident lines \xE2\x80\x94 templates are left unchanged"); break; }
            res = tpl_lines(cand); double prev = rt; rt = rate(res);
            pr.log(fmt("%.0f%% of sampled lines read confidently", rt * 100));
            if (it >= 2 && rt < prev + 0.01) break;
        }
    }
    if (rm >= 0 && rm > rt + 0.05) {
        fallback = true;
        pr.log(fmt("on this recording the MLSE decoder is better (%.0f%% vs %.0f%%): it will read the lines the templates could not", rm * 100, rt * 100));
    }
    double best = std::max(rt, rm);
    return best >= 0.03 ? best : -1;
}


// Быстрое чтение строки по порогу: захват по вступлению 55 55, порог — посередине между его уровнями,
// код кадра 27, 42 байта по центрам битов. Принимается, только если пакет целиком безупречен:
// адрес и служебные байты — точные кодовые слова Хэмминга, все остальные байты — с нечётной чётностью.
// Такие строки не нужно читать тяжёлым декодером; остальные идут в него как раньше.
static std::atomic<long> FR[8];
static bool fast_read(const u8 *y, int n, double lo, double hi, std::array<u8, 42> &out) {
    const double spb = tpl::SPB;
    auto at = [&](double x) {
        if (x < 0 || x >= n - 1) return 0.0;
        int i = (int)x; double f = x - i; return y[i] * (1 - f) + y[i + 1] * f;
    };
    // порог — средний уровень области данных (в пакете единиц и нулей примерно поровну); вступление для уровней
    // не годится: это самая высокая частота сигнала, и тракт записи её ослабляет сильнее всего
    int a = std::max(0, (int)(lo + 16 * spb)), b = std::min(n - 1, (int)(hi + 360 * spb));
    if (b - a < 200) return false;
    double sum = 0, mn = 255, mx = 0;
    for (int i = a; i < b; i++) { sum += y[i]; mn = std::min(mn, (double)y[i]); mx = std::max(mx, (double)y[i]); }
    if (mx - mn < 40) { FR[0]++; return false; }
    const double th = sum / (b - a);
    bool framed = false;
    for (double t = std::max(0.0, lo - 2); t <= hi + 2; t += 0.25) {
        int fc = 0;
        for (int k = 0; k < 8; k++) fc |= (at(t + (16 + k + 0.5) * spb) > th) << k;
        if (fc != 0x27) continue;
        framed = true;
        for (int bb = 0; bb < 42; bb++) {
            int v = 0;
            for (int k = 0; k < 8; k++) v |= (at(t + (24 + bb * 8 + k + 0.5) * spb) > th) << k;
            out[bb] = (u8)v;
            if (bb == 1 && (ham::dec[out[0]] < 0 || ham::dec[out[1]] < 0)) break;
        }
        int ad = ham::dec[out[0]], c = ham::dec[out[1]];
        if (ad < 0 || c < 0) { FR[3]++; continue; }
        int row = (ad >> 3) | (c << 1);
        bool good = true;
        if (row == 0) {
            for (int k = 2; k < 10 && good; k++) good = ham::dec[out[k]] >= 0;
            for (int k = 10; k < 42 && good; k++) good = odd_parity(out[k]);
        } else if (row <= 25) {
            for (int k = 2; k < 42 && good; k++) good = odd_parity(out[k]);
        } else good = false;                             // служебные пакеты 26–31 — тяжёлому декодеру
        if (good) return true;
        FR[row == 0 ? 4 : 5]++;
    }
    if (!framed) FR[2]++;
    return false;
}

std::map<std::pair<int, int>, std::array<u8, 42>> VbiReader::decode_all(const std::vector<int> *frames_in) {
    std::vector<int> frames;
    if (frames_in) frames = *frames_in; else { frames.resize(N); for (int i = 0; i < N; i++) frames[i] = i; }
    std::map<std::pair<int, int>, std::array<u8, 42>> res;
    std::map<std::pair<int, int>, double> offs;
    std::vector<std::pair<int, int>> sel;
    pr.step(fmt("finding lines with a signal in %zu frames", frames.size()));
    {
        std::vector<std::vector<std::pair<int, int>>> per(frames.size());
        parallel_for(frames.size(), [&](size_t i) {
            for (int l = 0; l < lpf; l++) if (line_std(line(frames[i], l), 2048) > 20) per[i].push_back({frames[i], l});
        }, &pr, 64);
        for (auto &v : per) sel.insert(sel.end(), v.begin(), v.end());
    }
    // быстрое чтение по порогу: безупречные строки сразу в результат, остальные — декодеру
    // сначала проба на 2000 строк: на записях с узкой полосой (VHS, SECAM, многие платы) биты расплываются и порог
    // почти ничего не читает — тогда этот проход пропускается
    bool fast_ok = false;
    if (!getenv("TR_NO_FAST") && !sel.empty()) {
        size_t m = std::min<size_t>(2000, sel.size()), k = 0; std::array<u8, 42> tmp;
        for (size_t i = 0; i < m; i++) k += fast_read(line(sel[i * (sel.size() / m)].first, sel[i * (sel.size() / m)].second), 2048, win_lo, win_hi, tmp);
        fast_ok = k >= 0.3 * m;
        if (getenv("TR_DEBUG")) pr.log(fmt("fast read probe: %zu of %zu", k, m));
    }
    if (fast_ok) {
        pr.step(fmt("reading clean lines directly: %zu lines", sel.size()));
        std::vector<char> ok(sel.size(), 0);
        std::vector<std::array<u8, 42>> fb(sel.size());
        parallel_for(sel.size(), [&](size_t i) { ok[i] = fast_read(line(sel[i].first, sel[i].second), 2048, win_lo, win_hi, fb[i]); }, &pr, 512);
        std::vector<std::pair<int, int>> rest;
        size_t nf = 0;
        for (size_t i = 0; i < sel.size(); i++) { if (ok[i]) { res[sel[i]] = fb[i]; nf++; } else rest.push_back(sel[i]); }
        if (getenv("TR_DEBUG")) pr.log(fmt("fast reject: amp %ld runin %ld framing %ld mrag %ld hdrpar %ld rowpar %ld", (long)FR[0], (long)FR[1], (long)FR[2], (long)FR[3], (long)FR[4], (long)FR[5]));
        pr.log(fmt("%zu of %zu lines read directly (%.0f%%), %zu go to the decoder", nf, sel.size(), 100.0 * nf / std::max<size_t>(1, sel.size()), rest.size()));
        sel.swap(rest);
    }
    pr.step(gpu ? fmt("decoding %zu frames on the GPU", frames.size()) : fmt("decoding %zu frames on %u cores", frames.size(), std::max(1u, std::thread::hardware_concurrency() - 1)));
    auto t0 = std::chrono::steady_clock::now();
    const size_t CH = gpu ? 8000 : 2000;
    for (size_t a = 0; a < sel.size(); a += CH) {
        std::vector<std::pair<int, int>> part(sel.begin() + a, sel.begin() + std::min(sel.size(), a + CH));
        Progress sub; sub.on_progress = nullptr;
        std::vector<LineRes> r;
        if (gpu) {
            std::vector<float> L(part.size() * 2048);
            for (size_t i = 0; i < part.size(); i++) { const u8 *y = line(part[i].first, part[i].second); for (int k = 0; k < 2048; k++) L[i * 2048 + k] = y[k]; }
            auto g = gpu->decode(L, part.size(), win_lo, win_hi);
            for (size_t i = 0; i < part.size(); i++) r.push_back({part[i].first, part[i].second, g[i].data, g[i].off, true});
        } else {
            r.resize(part.size());
            parallel_for(part.size(), [&](size_t i) {
                auto y = tpl::norm(line(part[i].first, part[i].second), 2048);
                auto d = dec.decode_fast(y, win_lo, win_hi);
                r[i] = {part[i].first, part[i].second, d.data, d.off, true};
            }, &pr, 8);
        }
        for (auto &x : r) { res[{x.f, x.l}] = x.b; offs[{x.f, x.l}] = x.off; }
        size_t done = std::min(sel.size(), a + CH);
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double left = el / done * (sel.size() - done);
        pr.progress(done, sel.size(), left >= 90 ? fmt("~%.0f min left", left / 60) : fmt("~%.0f s left", left));
    }
    if (fallback) {
        std::vector<std::pair<int, int>> weak;
        for (auto &kv : res) if (!wst_confident(kv.second.data())) weak.push_back(kv.first);
        pr.step(fmt("reading the rest with the MLSE decoder: %zu lines", weak.size()));
        int better = 0;
        for (auto &x : mlse_lines(weak, win_lo, win_hi))
            if (score(x.b.data()) < score(res[{x.f, x.l}].data())) { res[{x.f, x.l}] = x.b; offs.erase({x.f, x.l}); better++; }
        pr.log(fmt("the MLSE decoder improved %d of %zu lines", better, weak.size()));
    }
    if (do_repair) {
        std::vector<std::pair<int, int>> todo;
        for (auto &kv : res) {
            if (!offs.count(kv.first)) continue;
            auto &b = kv.second; int a = ham::fix[b[0]], c = ham::fix[b[1]];
            if (a < 0 || c < 0) continue;
            int row = (a >> 3) | (c << 1); int k0 = row == 0 ? 10 : 2;
            if (row > 25) continue;
            bool bad = false; for (int k = k0; k < 42; k++) if (!odd_parity(b[k])) { bad = true; break; }
            if (bad) todo.push_back(kv.first);
        }
        pr.step(fmt("repairing damaged bytes: %zu lines", todo.size()));
        std::atomic<int> fixed{0};
        std::vector<std::array<u8, 42>> fx(todo.size());
        parallel_for(todo.size(), [&](size_t i) {
            auto y = tpl::norm(line(todo[i].first, todo[i].second), 2048);
            fx[i] = res[todo[i]];
            fixed += dec.repair(y, offs[todo[i]], fx[i]);
        }, &pr, 64);
        for (size_t i = 0; i < todo.size(); i++) res[todo[i]] = fx[i];
        pr.log(fmt("bytes repaired: %d in %zu lines", (int)fixed, todo.size()));
    }
    return res;
}

void decode_vbi_project(const std::string &vbi_in, const std::string &out_in, int lpf, bool use_gpu, bool repair, Progress &pr) {
    std::string VBI = abspath(vbi_in), OUT = abspath(out_in);
    make_dirs(OUT);
    VbiReader R(VBI, lpf, use_gpu, pr);
    int N = R.frames();
    if (N == 0) throw std::runtime_error(fmt("the file is smaller than one frame (%d x 2048 bytes) \xE2\x80\x94 check the format", lpf));
    pr.log(fmt("%s: %d frames (%.0f s), %d lines of 2048 samples", basename(VBI).c_str(), N, N / 25.0, lpf));
    R.do_repair = repair;
    auto cand = R.sample();
    if (cand.empty()) throw std::runtime_error("no lines with a teletext signal were found in the recording");
    if (R.prepare(cand, true) < 0)
        throw std::runtime_error("The decoder cannot read this recording: almost no line gave a valid address\n(neither with templates nor with the MLSE decoder). Probably a different capture format\n(sample rate, number of lines) or the signal is too distorted. If you have a .t42 stream\nfrom another program, open it with \xE2\x80\x9COpen stream .t42\xE2\x80\x9D.");
    auto res = R.decode_all();
    pr.step("writing the stream");
    LineMap lp; lp.frames = N; lp.lpf = lpf; lp.v.assign((size_t)N * lpf, -1);
    std::vector<Packet42> pk; int rej = 0;
    for (auto &kv : res) {
        auto &b = kv.second;
        if (ham::fix[b[0]] < 0 || ham::fix[b[1]] < 0 || wst_par_bad(b.data()) > 10) { rej++; continue; }
        lp.v[(size_t)kv.first.first * lpf + kv.first.second] = (int32_t)pk.size();
        Packet42 p; memcpy(p.data(), b.data(), 42); pk.push_back(p);
    }
    std::string t42 = path_join(OUT, "stream.t42"), lines = path_join(OUT, "line_pkt.npy");
    write_t42(t42, pk);
    save_npy_i32(lines, lp);
    pr.log(fmt("packets %zu, lines discarded %d", pk.size(), rej));
    build_project(t42, OUT, lines, lpf, VBI, pr);
}
