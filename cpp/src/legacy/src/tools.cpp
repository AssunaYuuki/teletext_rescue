#include "tools.h"
#include <set>
#include "decode_vbi.h"
#include "pagebuild.h"
#include "teletext.h"

// ================================================================ VITS
static const double MB_NTSC[6] = {0.5, 1.25, 2.0, 3.0, 3.58, 4.1};
static const double MB_PAL[6] = {0.5, 1.0, 2.0, 4.0, 4.8, 5.8};

static std::vector<float> median_line(const Rec &R, int row, int par, int n = 150) {
    std::vector<int> uu;
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) uu.push_back(u);
    size_t st = std::max<size_t>(1, uu.size() / n);
    std::vector<std::vector<float>> L;
    for (size_t i = 0; i < uu.size() && L.size() < (size_t)n; i += st) L.push_back(R.line(uu[i], row));
    std::vector<float> m(R.ns);
    for (int k = 0; k < R.ns; k++) { std::vector<double> c; for (auto &l : L) c.push_back(l[k]); m[k] = (float)median(c); }
    return m;
}

// пакеты multiburst: колебания вокруг скользящего среднего (2 мкс); подряд идущие полупериоды
// одинаковой длины (±30%) — один пакет; частота по средней длине полупериода, размах по 2–98 %
static std::vector<std::pair<double, double>> mb_bursts(const std::vector<float> &m, double fs) {
    int n = (int)m.size(), w = std::max(3, (int)(fs * 2e-6)) | 1;
    std::vector<double> c(n + 1, 0), d(n);
    for (int i = 0; i < n; i++) c[i + 1] = c[i] + m[i];
    double mx = 0;
    for (int i = 0; i < n; i++) { int a = std::max(0, i - w / 2), b = std::min(n, i + w / 2 + 1); d[i] = m[i] - (c[b] - c[a]) / (b - a); }
    int i0 = (int)(n * 0.12);
    for (int i = i0; i < n - w / 2; i++) mx = std::max(mx, fabs(d[i]));
    double h = 0.12 * mx;
    std::vector<double> zc;                              // переходы с гистерезисом, точка нуля — с долей отсчёта
    int sg = 0;
    for (int i = i0; i < n - w / 2; i++) {
        int s2 = d[i] > h ? 1 : d[i] < -h ? -1 : 0;
        if (s2 && s2 != sg) {
            if (sg) {
                int k = i; while (k > i0 && (d[k - 1] > 0) == (s2 > 0)) k--;
                double den = d[k - 1] - d[k];
                zc.push_back(k - 1 + (den != 0 ? d[k - 1] / den : 0.5));
            }
            sg = s2;
        }
    }
    std::vector<std::pair<double, double>> out;
    for (size_t k = 0; k + 1 < zc.size();) {
        double hp0 = zc[k + 1] - zc[k]; size_t j = k + 1; double sum = hp0;
        while (j + 1 < zc.size()) {
            double hp = zc[j + 1] - zc[j], mean = sum / (j - k);
            if (hp < 0.7 * mean || hp > 1.3 * mean) break;
            sum += hp; j++;
        }
        int nh = (int)(j - k);
        if (nh >= 3) {
            double hp = sum / nh, f = fs / (2 * hp) / 1e6;
            int a = (int)zc[k], b = (int)zc[j];
            std::vector<double> seg(m.begin() + a, m.begin() + b);
            out.push_back({round(f * 100) / 100, percentile(seg, 98) - percentile(seg, 2)});
        }
        k = j;
    }
    return out;
}

// испытательная строка ITU (строки 17/330 и похожие): белая полоса, импульсы 2T и 20T, лестница
struct ItsMeas { double amp = 0, p2t = -1, had2t = 0, l20 = -1, c20 = 0; std::vector<double> steps; };
static bool its_measure(const std::vector<float> &m, double fs, ItsMeas &o) {
    int n = (int)m.size(); if (n < 100) return false;
    auto at = [&](double us) { return std::clamp((int)(us * 1e-6 * fs), 0, n - 1); };
    std::vector<double> b0(m.begin() + at(0.3), m.begin() + at(2.5));
    double blank = median(b0), top = *std::max_element(m.begin(), m.end());
    if (top - blank < 20) return false;
    double thr = blank + 0.7 * (top - blank);
    int bs = -1, be = -1;
    for (int i = 0; i < n;) {
        if (m[i] <= thr) { i++; continue; }
        int j = i; while (j < n && m[j] > thr) j++;
        if (j - i > fs * 4e-6) { bs = i; be = j; break; }       // полоса идёт первой (лестница бывает такой же длины)
        i = j;
    }
    if (getenv("TR_VITSDBG")) fprintf(stderr, "its: n %d blank %.1f top %.1f bar %d-%d fs %.0f\n", n, blank, top, bs, be, fs);
    if (be - bs < fs * 4e-6) return false;                       // полосы нет
    std::vector<double> bar(m.begin() + bs + (be - bs) / 5, m.begin() + be - (be - bs) / 5);
    o.amp = median(bar) - blank;
    if (o.amp < 20) return false;
    auto rel = [&](double v) { return 100 * (v - blank) / o.amp; };
    // сглаживание 0,5 мкс убирает цветовую поднесущую
    int w = std::max(3, (int)(fs * 0.5e-6)) | 1;
    std::vector<double> S(n);
    { double acc = 0; std::vector<double> c(n + 1, 0); for (int i = 0; i < n; i++) c[i + 1] = c[i] + m[i];
      for (int i = 0; i < n; i++) { int a = std::max(0, i - w / 2), b = std::min(n, i + w / 2 + 1); S[i] = (c[b] - c[a]) / (b - a); } (void)acc; }
    int pos = be;
    // 2T: узкий пик в пределах 6 мкс после полосы
    {
        int a = std::min(n - 1, be + at(0.8)), b = std::min(n, be + at(6.0)), pk = -1;
        for (int i = a; i < b; i++) if (pk < 0 || m[i] > m[pk]) pk = i;
        if (pk > 0 && rel(m[pk]) > 40) {
            double half = blank + (m[pk] - blank) / 2; int l = pk, r = pk;
            while (l > 0 && m[l] > half) l--;
            while (r < n - 1 && m[r] > half) r++;
            double had = (r - l) / fs * 1e9;
            if (had < 800) { o.p2t = rel(m[pk]); o.had2t = had; pos = r; }
        }
    }
    // 20T: широкий горб (полуширина 1–4 мкс) в пределах 12 мкс
    {
        int a = std::min(n - 1, pos + at(0.5)), b = std::min(n, pos + at(12.0)), pk = -1;
        for (int i = a; i < b; i++) if (pk < 0 || S[i] > S[pk]) pk = i;
        if (pk > 0 && rel(S[pk]) > 15) {
            double half = blank + (S[pk] - blank) / 2; int l = pk, r = pk;
            while (l > a && S[l] > half) l--;
            while (r < n - 1 && S[r] > half) r++;
            double had = (r - l) / fs * 1e6;
            if (had > 1 && had < 4 && l > a) {
                o.l20 = rel(S[pk]);
                double c = 0; for (int i = std::max(0, pk - at(0.5)); i < std::min(n, pk + at(0.5)); i++) c = std::max(c, fabs(m[i] - S[i]));
                o.c20 = 200 * c / o.amp;
                pos = r;
            }
        }
    }
    // лестница: площадки длиннее 1,5 мкс по сглаженной строке
    std::vector<double> lv;
    for (int i = pos; i < n;) {
        int j = i + 1; double s0 = S[i];
        while (j < n && fabs(S[j] - s0) < 0.04 * o.amp) j++;
        if (j - i > fs * 1.5e-6) { std::vector<double> seg(S.begin() + i + (j - i) / 4, S.begin() + j - (j - i) / 4); lv.push_back(median(seg)); }
        i = j;
    }
    // самая длинная возрастающая цепочка соседних площадок
    size_t bi = 0, bl = 0;
    for (size_t i = 0; i < lv.size();) {
        size_t j = i + 1; while (j < lv.size() && lv[j] > lv[j - 1] + 0.05 * o.amp) j++;
        if (j - i > bl) { bl = j - i; bi = i; }
        i = j;
    }
    if (bl >= 4) for (size_t i = bi + 1; i < bi + bl; i++) o.steps.push_back(100 * (lv[i] - lv[i - 1]) / o.amp);
    if (getenv("TR_VITSDBG")) { fprintf(stderr, "its: amp %.1f p2t %.1f l20 %.1f levels", o.amp, o.p2t, o.l20); for (double v : lv) fprintf(stderr, " %.0f", v); fprintf(stderr, "\n"); }
    return o.p2t > 0 || o.steps.size() >= 3;
}

Json vits_analyse(const std::string &path, const Json &res, Progress &pr) {
    auto fc = detect_format(path);
    Json out = Json::array();
    if (fc.first.empty()) return out;
    Rec R(path, fc.first);
    const double *freqs = !R.ntsc() ? MB_PAL : MB_NTSC;
    for (auto &L : res["lines"].a) {
        if (L["kind"].str().rfind("test signal", 0) != 0) continue;
        if (getenv("TR_VITSDBG")) pr.log("vits: line " + std::to_string(L["tv_line"].integer()) + " " + L["kind"].str());
        int par = L["parity"].is_null() ? -1 : L["parity"].integer();
        auto m = median_line(R, L["row"].integer(), par);
        ItsMeas im;
        if (its_measure(m, R.fs, im)) {
            Json o = Json::object(); o["tv_line"] = L["tv_line"]; o["parity"] = L["parity"]; o["kind"] = "its";
            o["bar"] = round(im.amp * 10) / 10;
            if (im.p2t > 0) { o["pulse_2t"] = round(im.p2t * 10) / 10; o["had_2t_ns"] = round(im.had2t); }
            if (im.l20 > 0) { o["pulse_20t_luma"] = round(im.l20 * 10) / 10; o["pulse_20t_colour"] = round(im.c20 * 10) / 10; }
            Json st = Json::array(); for (double v : im.steps) st.push(round(v * 10) / 10); o["steps"] = st;
            pr.log(fmt("test signal on line %d: bar %.0f, 2T %.0f%%, steps %zu", L["tv_line"].integer(), im.amp, im.p2t, im.steps.size()));
            out.push(o);
            continue;
        }
        std::map<double, double> amp;
        for (auto &b : mb_bursts(m, R.fs)) {
            if (getenv("TR_VITSDBG")) fprintf(stderr, "burst %.2f MHz amp %.1f\n", b.first, b.second);
            double k = freqs[0]; for (int q = 0; q < 6; q++) if (fabs(freqs[q] - b.first) < fabs(k - b.first)) k = freqs[q];
            if (fabs(k - b.first) < 0.25 * k) amp[k] = std::max(amp[k], b.second);
        }
        if (amp.size() < 3 || !amp.count(freqs[0])) continue;
        // высокие частоты медиана по кадрам гасит (фаза плавает) — размах по отдельным кадрам, затем медиана
        {
            std::map<double, std::vector<double>> per;
            int row = L["row"].integer(), cnt = 0;
            for (int u = par >= 0 ? par : 0; u < R.n && cnt < 60; u += std::max(1, R.n / 60) * (par >= 0 ? 2 : 1), cnt++) {
                auto y = R.line(u, row);
                std::map<double, double> one;
                for (auto &b : mb_bursts(y, R.fs)) {
                    double k = freqs[0]; for (int q = 0; q < 6; q++) if (fabs(freqs[q] - b.first) < fabs(k - b.first)) k = freqs[q];
                    if (fabs(k - b.first) < 0.15 * k) one[k] = std::max(one[k], b.second);
                }
                for (int q = 0; q < 6; q++) per[freqs[q]].push_back(one.count(freqs[q]) ? one[freqs[q]] : 0.0);
            }
            for (auto &kv : per) { double v = median(kv.second); if (v > 0) amp[kv.first] = std::max(amp.count(kv.first) ? amp[kv.first] : 0.0, v); }
        }
        double ref = amp[freqs[0]] ? amp[freqs[0]] : 1;
        Json o = Json::object(); o["tv_line"] = L["tv_line"]; o["parity"] = L["parity"]; o["kind"] = "multiburst";
        Json rd = Json::array();
        for (int q = 0; q < 6; q++) { Json e = Json::array(); e.push(freqs[q]); e.push(round(20 * log10(std::max(amp.count(freqs[q]) ? amp[freqs[q]] : 0.0, 1e-6) / ref) * 10) / 10); rd.push(e); }
        o["response_db"] = rd;
        std::string s;
        for (auto &e : rd.a) s += (s.empty() ? "" : ", ") + fmt("%g MHz %+.1f dB", e[0].num(), e[1].num());
        pr.log(fmt("multiburst on line %d: ", L["tv_line"].integer()) + s);
        out.push(o);
    }
    return out;
}

std::vector<std::string> vits_response_text(const Json &vt) {
    std::vector<std::string> t;
    if (!vt.size()) return t;
    for (auto &o : vt.a) {
        if (o["kind"].str() != "its") continue;
        std::string s = fmt("Test signal on line %d: white bar %.0f units above blanking", o["tv_line"].integer(), o["bar"].num());
        if (!o["pulse_2t"].is_null()) s += fmt("; 2T pulse %.0f%% (half-amplitude width %.0f ns, nominal 200)", o["pulse_2t"].num(), o["had_2t_ns"].num());
        if (!o["pulse_20t_luma"].is_null()) {
            s += fmt("; 20T pulse: luminance %.0f%% (nominal 50), colour %.0f%% (nominal 100)", o["pulse_20t_luma"].num(), o["pulse_20t_colour"].num());
            if (o["pulse_20t_colour"].num() < 20) s += " \xE2\x80\x94 colour subcarrier missing";
        }
        if (o["steps"].size()) {
            double mn = 1e9, mx = 0; std::string st;
            for (auto &v : o["steps"].a) { mn = std::min(mn, v.num()); mx = std::max(mx, v.num()); st += (st.empty() ? "" : "/") + fmt("%.0f", v.num()); }
            s += fmt("; staircase %zu steps %s%% (linearity %.0f%%)", o["steps"].size(), st.c_str(), mx > 0 ? 100 * mn / mx : 0);
        }
        t.push_back(s);
    }
    const Json *mb = nullptr;
    for (auto &o : vt.a) if (o["kind"].str() == "multiburst") { mb = &o; break; }
    if (!mb) return t;
    const Json &o = *mb;
    t.push_back(fmt("Frequency response of the recording (multiburst test signal, line %d):", o["tv_line"].integer()));
    std::string s; double cut = 0;
    for (auto &e : o["response_db"].a) {
        double f = e[0].num(), db = e[1].num();
        s += (s.empty() ? "" : ", ") + (db > -30 ? fmt("%g MHz %+.1f dB", f, db) : fmt("%g MHz \xE2\x80\x94", f));
        if (db < -20 && (cut == 0 || f < cut)) cut = f;
    }
    t.push_back("  " + s);
    if (cut) t.push_back(fmt("  (nothing above ~%g MHz passes)", cut));
    return t;
}

void vits_analyse_file(const std::string &path, Progress &pr) {
    Progress quiet; quiet.cancel = false;
    Json res = vbi_probe(path, quiet);
    for (auto &l : vits_response_text(vits_analyse(path, res, pr))) pr.log(l);
}

// ================================================================ обзор строк bt8x8
void vbi_lines_survey(const std::string &path, Progress &pr) {
    MappedFile mf(path);
    const int SPL = 2048, LPF = 32;
    int n = (int)(mf.size() / (SPL * LPF));
    if (n < 2) throw std::runtime_error("not a bt8x8 recording");
    auto line = [&](int f, int l) { return mf.data() + ((uint64_t)f * LPF + l) * SPL; };
    long zero = 0, tot = 0;
    for (int i = 0; i < 50; i++) { int f = (int)((long long)i * (n - 1) / 49); for (int l = 0; l < 31; l++) for (int k = 1604; k < 2040; k++) { zero += line(f, l)[k] == 0; tot++; } }
    bool ntsc = zero > 0.99 * tot;
    double fs = ntsc ? 28636363.0 : 35468950.0, fps = ntsc ? 30000.0 / 1001 : 25;
    int first[2] = {ntsc ? 10 : 7, ntsc ? 273 : 320}, valid = ntsc ? 1600 : 2044;
    pr.log("\n=== " + path);
    pr.log(fmt("%d frames = %.1f min; system: %s", n, n / fps / 60, ntsc ? "NTSC (bt8x8 28.64 MHz, 1600 samples)" : "PAL/SECAM (bt8x8 35.47 MHz)"));
    std::vector<long long> c;
    for (int i = 0; i < std::min(n, 5000); i++) { int f = (int)((long long)i * (n - 1) / std::max(1, std::min(n, 5000) - 1)); uint32_t v; memcpy(&v, line(f, 31) + 2044, 4); c.push_back(v); }
    bool mono = true; for (size_t i = 1; i < c.size(); i++) if (c[i] <= c[i - 1]) mono = false;
    if (mono) {
        long long span = c.back() - c[0] + 1;
        pr.log(fmt("driver frame counter %lld\xE2\x80\xA6%lld: recorded %d of %lld \xE2\x80\x94 ", c[0], c.back(), n, span) + (n < 0.995 * span ? fmt("lost %.0f%%", 100 * (1 - (double)n / span)) : "no losses"));
    } else pr.log("the frame counter is not monotonic \xE2\x80\x94 losses cannot be determined from it");
    int NFR = std::min(300, n - 1);
    std::map<int, std::array<long, 4>> dec;          // запись -> строк, с адресом, X/0, ряды
    std::map<int, std::map<int, long>> mags;
    if (!ntsc) {
        try {
            Progress q; q.on_log = nullptr;
            VbiReader Rd(path, LPF, true, q);
            auto cand = Rd.sample(800, 2500);
            if (!cand.empty() && Rd.prepare(cand, true) >= 0) {
                std::vector<int> frs; for (int i = 0; i < NFR; i++) frs.push_back((int)((long long)i * (n - 2) / std::max(1, NFR - 1)));
                std::sort(frs.begin(), frs.end()); frs.erase(std::unique(frs.begin(), frs.end()), frs.end());
                auto res = Rd.decode_all(&frs);
                for (auto &kv : res) {
                    auto &b = kv.second; auto &e = dec[kv.first.second]; e[0]++;
                    int a = ham::fix[b[0]], cc = ham::fix[b[1]];
                    if (a < 0 || cc < 0 || wst_par_bad(b.data()) > 10) continue;
                    e[1]++; mags[kv.first.second][(a & 7) ? (a & 7) : 8]++;
                    int r = (a >> 3) | (cc << 1); if (r == 0) e[2]++; else if (r <= 25) e[3]++;
                }
                pr.log(fmt("decoder: %s, packet start window %.0f\xE2\x80\x93%.0f", Rd.gpu_on() ? "GPU" : "CPU", Rd.win_lo, Rd.win_hi) + (Rd.fallback ? ", with the rest read by the MLSE decoder" : ""));
            } else pr.log("the teletext decoder cannot read this recording");
        } catch (Cancelled &) { throw; } catch (std::exception &e) { pr.log(std::string("decoder: ") + e.what()); }
    }
    pr.log(fmt("%4s %7s %7s %8s %5s %7s  what is carried", "rec", "TV line", "signal", "static", "MHz", "packets"));
    for (int l = 0; l < LPF; l++) {
        int tvl = first[l / 16] + l % 16;
        double sig = 0; std::vector<double> cc;
        std::vector<double> Pw;
        int act = 0;
        for (int i = 0; i < NFR; i++) {
            int f = (int)((long long)i * (n - 2) / std::max(1, NFR - 1));
            const u8 *a = line(f, l), *b = line(f + 1, l);
            double ma = 0, mb = 0; for (int k = 0; k < valid; k++) { ma += a[k]; mb += b[k]; } ma /= valid; mb /= valid;
            double sa = 0, sb = 0, ab = 0; for (int k = 0; k < valid; k++) { double x = a[k] - ma, y = b[k] - mb; sa += x * x; sb += y * y; ab += x * y; }
            if (sqrt(sa / valid) > 20) {
                act++; cc.push_back(ab / sqrt(sa * sb + 1e-9));
                if (Pw.size() < 1 || act <= 40) {
                    std::vector<double> x(valid); for (int k = 0; k < valid; k++) x[k] = a[k] - ma;
                    auto p = rfft_power(x); if (Pw.empty()) Pw.assign(p.size(), 0);
                    for (size_t k = 0; k < p.size(); k++) Pw[k] += p[k];
                }
            }
        }
        sig = (double)act / NFR;
        double stat = cc.empty() ? 0 : median(cc), mhz = 0;
        if (!Pw.empty()) { double s = 0, sw = 0; for (size_t k = 3; k < Pw.size(); k++) { double f = k * fs / valid / 1e6; s += Pw[k] * f; sw += Pw[k]; } mhz = sw ? s / sw : 0; }
        std::string kind;
        if (ntsc && ((tvl >= 22 && tvl <= 25) || (tvl >= 285 && tvl <= 288))) kind = sig > 0.02 ? "start of the picture (not VBI)" : "empty";
        else if (sig < 0.02) kind = "empty";
        else if (!ntsc && stat > 0.6 && mhz > 3.9 && mhz < 4.9) kind = "SECAM colour identification (\xE2\x80\x9C" "bottles\xE2\x80\x9D)";
        else if (stat > 0.9) kind = "test signal";
        else kind = "data";
        std::string pk = "\xE2\x80\x94", extra;
        if (dec.count(l) && dec[l][0]) {
            auto &e = dec[l]; double r = (double)e[1] / e[0]; pk = fmt("%.0f%%", r * 100);
            if (r >= 0.3 && stat < 0.9) {
                kind = "teletext";
                extra = " \xC2\xB7 magazines";
                for (auto &m : mags[l]) if (m.second >= 0.03 * e[1]) extra += " " + std::to_string(m.first);
            } else if (kind == "data") kind = std::string("data, but not readable teletext") + (tvl == 16 ? " (VPS?)" : "");
        }
        pr.log(fmt("%4d %7d %6.0f%% %8.2f %5.2f %7s  ", l, tvl, sig * 100, stat, mhz, pk.c_str()) + kind + extra);
    }
}

// ================================================================ служебные пакеты
static std::string mjd_date(int m) {
    int yp = (int)((m - 15078.2) / 365.25), mp = (int)((m - 14956.1 - (int)(yp * 365.25)) / 30.6001);
    int d = m - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001), k = (mp == 14 || mp == 15) ? 1 : 0;
    return fmt("%04d-%02d-%02d", 1900 + yp + k, mp - 1 - k * 12, d);
}
static int rev8(int x) { int r = 0; for (int i = 0; i < 8; i++) r |= ((x >> i) & 1) << (7 - i); return r; }

void service_packets_survey(const std::string &path, Progress &pr) {
    auto st = read_t42(path);
    std::map<std::string, long> kinds; long bad = 0;
    std::map<std::string, std::map<std::string, long>> f1; long f2 = 0; std::map<int, long> x31;
    for (auto &p : st) {
        int a = ham::dec[p[0]], b = ham::dec[p[1]];
        if (a < 0 || b < 0) { bad++; continue; }
        int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
        static const std::map<int, std::string> K = {{0, "X/0 page headers"}, {24, "X/24 FLOF prompt row"}, {25, "X/25 row 24 replacement"},
            {26, "X/26 character enhancements"}, {27, "X/27 FLOF links"}, {28, "X/28 page enhancements"}, {29, "M/29 magazine enhancements"},
            {30, "M/30 service data"}, {31, "X/31 independent data"}};
        kinds[row >= 1 && row <= 23 ? "X/1\xE2\x80\x93" "23 page rows" : K.count(row) ? K.at(row) : fmt("X/%d", row)]++;
        if (row == 30 && mag == 8) {
            int dc = ham::dec[p[2]];
            if (dc < 0) continue;
            if ((dc >> 1) == 0) {
                f1["network code (NI)"][fmt("%04X", (rev8(p[9]) << 8) | rev8(p[10]))]++;
                auto bcd = [&](int i0) { std::vector<int> v; for (int i = i0; i < i0 + 3; i++) { v.push_back((((p[i] >> 4) & 15) - 1 + 16) % 16); v.push_back(((p[i] & 15) - 1 + 16) % 16); } return v; };
                auto mjd = bcd(12); bool ok = true; for (int i = 1; i < 6; i++) ok &= mjd[i] <= 9;
                if (ok) { int m = 0; for (int i = 1; i < 6; i++) m = m * 10 + mjd[i]; f1["date"][mjd_date(m)]++; }
                auto tm = bcd(15); ok = true; for (int v : tm) ok &= v <= 9;
                if (ok) f1["UTC time"][fmt("%d%d:%d%d", tm[0], tm[1], tm[2], tm[3])]++;
                int off = p[11]; f1["time zone"][fmt("%c%g h", off & 0x40 ? '-' : '+', ((off >> 1) & 0x1F) / 2.0)]++;
                bool allodd = true; for (int i = 22; i < 42; i++) allodd &= odd_parity(p[i]);
                if (allodd) { std::string s; for (int i = 22; i < 42; i++) s += (char)(p[i] & 0x7F); f1["status row"][strip(s)]++; }
            } else f2++;
        } else if (row == 31) x31[mag]++;
    }
    size_t n = st.size();
    pr.log(fmt("\n=== %s: %zu packets, address unreadable in %.1f%%", basename(path).c_str(), n, 100.0 * bad / std::max<size_t>(1, n)));
    std::vector<std::pair<std::string, long>> kv(kinds.begin(), kinds.end());
    std::stable_sort(kv.begin(), kv.end(), [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &k : kv) pr.log(fmt("  %-28s %8ld (%.2f%%)", k.first.c_str(), k.second, 100.0 * k.second / std::max<size_t>(1, n)));
    for (auto &k : f1) {
        std::vector<std::pair<std::string, long>> v(k.second.begin(), k.second.end());
        std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        std::string s; for (size_t i = 0; i < v.size() && i < 4; i++) s += (s.empty() ? "" : "; ") + v[i].first + fmt(" \xC3\x97%ld", v[i].second);
        pr.log("  8/30 format 1 " + k.first + ": " + s);
    }
    if (f2) pr.log(fmt("  8/30 format 2 (PDC \xE2\x80\x94 programme label): %ld", f2));
    if (!x31.empty()) { std::string s; for (auto &m : x31) s += fmt("%s'magazine %d': %ld", s.empty() ? "" : ", ", m.first, m.second); pr.log("  X/31 by magazine: {" + s + "}"); }
}

// ================================================================ DVB .ts -> .t42 (EN 300 472)
size_t ts_to_t42(const std::string &ts, const std::string &out, Progress &pr) {
    MappedFile mf(ts);
    const u8 *d = mf.data(); uint64_t n = mf.size();
    uint64_t off = 0;
    while (off < 188 && off < n && !(d[off] == 0x47 && (off + 188 >= n || d[off + 188] == 0x47))) off++;
    std::map<int, Bytes> pes;
    std::vector<Packet42> pk;
    auto flush = [&](Bytes &b) {
        if (b.size() < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xBD) { b.clear(); return; }
        size_t h = 9 + b[8];
        if (h >= b.size()) { b.clear(); return; }
        if (b[h] < 0x10 || b[h] > 0x1F) { b.clear(); return; }
        size_t i = h + 1;
        while (i + 2 <= b.size()) {
            int id = b[i], len = b[i + 1];
            if (i + 2 + len > b.size()) break;
            if ((id == 0x02 || id == 0x03) && len == 0x2C) {
                Packet42 p;
                for (int k = 0; k < 42; k++) p[k] = (u8)rev8(b[i + 4 + k]);
                pk.push_back(p);
            }
            i += 2 + len;
        }
        b.clear();
    };
    for (uint64_t p = off; p + 188 <= n; p += 188) {
        const u8 *t = d + p;
        if (t[0] != 0x47) continue;
        int pid = ((t[1] & 0x1F) << 8) | t[2];
        bool pusi = t[1] & 0x40;
        int afc = (t[3] >> 4) & 3;
        int h = 4;
        if (afc == 2) continue;
        if (afc == 3) h += 1 + t[4];
        if (h >= 188) continue;
        auto &b = pes[pid];
        if (pusi) { flush(b); if (t[h] == 0 && t[h + 1] == 0 && t[h + 2] == 1) b.assign(t + h, t + 188); }
        else if (!b.empty()) b.insert(b.end(), t + h, t + 188);
        if ((p / 188) % 200000 == 0) pr.progress(p, n);
    }
    for (auto &kv : pes) flush(kv.second);
    write_t42(out, pk);
    pr.log(fmt("DVB teletext: %zu packets written to ", pk.size()) + out);
    return pk.size();
}

// ================================================================ AMOL / VITC
static int amol_val(const uint8_t *b, int a, int n) { int v = 0; for (int i = a; i < a + n; i++) v = (v << 1) | b[i]; return v; }

AmolMsg amol_parse(const uint8_t b[48]) {
    AmolMsg m;
    m.frame = amol_val(b, 7, 5);
    for (int i = 12; i < 47; i++) m.msg = (m.msg << 1) | b[i];
    int ones = 0; for (int i = 12; i < 48; i++) ones += b[i];
    m.parity_even = ones % 2 == 0;
    if (m.msg == 0) return m;
    // текст: кадры 20..25, 5 знаков по 7 бит
    if (m.frame >= 20 && m.frame <= 25) {
        std::string t; bool ok = true;
        for (int c = 0; c < 5; c++) { int v = amol_val(b, 12 + 7 * c, 7); if (v < 0x20 || v > 0x7E) { ok = false; v = '.'; } t += (char)v; }
        m.kind = AmolMsg::ASCII; m.text = t; m.printable = ok;
        return m;
    }
    // код источника и время: как есть или инвертированное (кадр 27); запасные биты в верной форме — нули
    struct St { bool ok; int sid, mo, dy, hh, mi, ss, pm, sp; };
    auto dec = [](uint64_t v) {
        St s{false, (int)(v >> 29) & 63, (int)(v >> 25) & 15, (int)(v >> 20) & 31, (int)(v >> 16) & 15, (int)(v >> 10) & 63, (int)(v >> 4) & 63, (int)(v >> 3) & 1, (int)v & 7};
        s.ok = s.mo >= 1 && s.mo <= 12 && s.dy >= 1 && s.dy <= 31 && s.hh >= 1 && s.hh <= 12 && s.mi < 60 && s.ss < 60;
        return s;
    };
    St d = dec(m.msg), iv = dec(~m.msg & ((1ULL << 35) - 1));
    const St *w = nullptr; bool inv = false;
    if (d.ok && d.sp == 0) w = &d;
    else if (iv.ok && iv.sp == 0) { w = &iv; inv = true; }
    else if (d.ok) w = &d;
    else if (iv.ok) { w = &iv; inv = true; }
    if (!w) { m.kind = AmolMsg::OTHER; return m; }
    m.kind = AmolMsg::STAMP; m.inverted = inv;
    m.sid = w->sid; m.month = w->mo; m.day = w->dy; m.hour = w->hh; m.minute = w->mi; m.second = w->ss; m.pm = w->pm; m.spare = w->sp;
    return m;
}

std::string amol_stamp_text(const AmolMsg &m) {
    return fmt("%d/%d %d:%02d:%02d %s", m.month, m.day, m.hour, m.minute, m.second, m.pm ? "PM" : "AM");
}

int amol_report(const Rec &R, int row, int par, const std::string &out, Progress &pr) {
    std::string txt = fmt("AMOL (Nielsen Automated Measurement of Lineups) on line %d%s of %s\n", R.tv.at(row),
                          par < 0 ? "" : (std::string(" field ") + "AB"[par]).c_str(), basename(R.path).c_str());
    txt += "48 bits: phase reference 101, start 0110, frame address (5 bits, 0..29 once a second), message (35 bits), parity.\n"
           "Message: source ID (6), month, day, hour, minute, second, AM/PM, spare (3) on the time-stamp frames (usually 2/3 or 26/27,\n"
           "the second copy inverted); 5 ASCII characters on frames 20, 22, 24 (copies on 21, 23, 25); other frames carry no message.\n\n";
    double rate = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1);
    auto rec_time = [&](int u) { double s = u / rate; return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)); };
    int ok = 0, pe = 0, po = 0;
    struct FS { int n = 0, empty = 0, stamp = 0, inv = 0, ascii = 0, other = 0; };
    std::map<int, FS> fst;
    std::map<int, int> sids;
    struct Stamp { int u; AmolMsg m; double off; bool good = false; };
    std::vector<Stamp> stamps;
    std::map<std::string, std::pair<int, int>> texts;     // строка текста -> (сколько раз, первое место)
    std::vector<std::string> text_order;
    std::map<uint64_t, std::pair<int, std::set<int>>> others;
    std::string cyc; int cyc_last = -1, cyc_u = 0;           // текст текущего цикла: кадры 20, 22, 24
    auto flush_text = [&]() {
        if (cyc.size() == 15) {
            auto it = texts.find(cyc);
            if (it == texts.end()) { texts[cyc] = {1, cyc_u}; text_order.push_back(cyc); } else it->second.first++;
        }
        cyc.clear(); cyc_last = -1;
    };
    uint8_t b[48];
    Json pk = Json::array();
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) {
        if (u % 5000 < 2) pr.progress(u, R.n);
        if (!amol_slice(R.line(u, row), R.fs, b)) continue;
        ok++;
        AmolMsg m = amol_parse(b);
        FS &f = fst[m.frame]; f.n++;
        { std::string hx; for (int i = 0; i < 48; i += 4) hx += "0123456789ABCDEF"[(b[i] << 3) | (b[i + 1] << 2) | (b[i + 2] << 1) | b[i + 3]];
          Json e = Json::array(); e.push((double)u); e.push(hx); pk.push(e); }
        if (m.kind != AmolMsg::EMPTY) { if (m.parity_even) pe++; else po++; }
        switch (m.kind) {
        case AmolMsg::EMPTY: f.empty++; break;
        case AmolMsg::STAMP: {
            f.stamp++; f.inv += m.inverted;
            double T = (((m.month - 1) * 31.0 + m.day) * 24 + (m.hour % 12) + 12 * m.pm) * 3600 + m.minute * 60 + m.second;
            stamps.push_back({u, m, T - u / rate});
            break;
        }
        case AmolMsg::ASCII: {
            f.ascii++;
            if (m.frame % 2 == 0 && m.printable) {          // 20, 22, 24 — по порядку в одном цикле
                int want = cyc_last < 0 ? 20 : cyc_last + 2;
                if (m.frame != want) flush_text();
                if (m.frame == 20 || cyc_last >= 0) { if (cyc.empty()) cyc_u = u; cyc += m.text; cyc_last = m.frame; }
                if (m.frame == 24) flush_text();
            }
            break;
        }
        default: { f.other++; auto &o = others[m.msg]; o.first++; o.second.insert(m.frame); break; }
        }
    }
    flush_text();
    // часы эфира идут вместе с записью: смещение «эфир минус запись» постоянно на отрезке без склеек.
    // Отметка с ошибкой чтения (один бит месяца или минуты) выпадает из ряда соседей — её не берём.
    // В окне из соседних отметок берём самую многочисленную группу с одним смещением и кодом источника;
    // отметка верна, если входит в неё (повторяющаяся ошибка одного кадра — в меньшинстве).
    auto same = [&](size_t x, size_t y) { return fabs(stamps[x].off - stamps[y].off) <= 1.5 && stamps[x].m.sid == stamps[y].m.sid; };
    for (size_t i = 0; i < stamps.size(); i++) {
        size_t lo = i >= 16 ? i - 16 : 0, hi = std::min(stamps.size(), i + 17);
        size_t best = i; int bc = -1;
        for (size_t j = lo; j < hi; j++) {
            int c = 0; for (size_t k = lo; k < hi; k++) c += same(j, k);
            if (c > bc) { bc = c; best = j; }
        }
        int mine = 0; for (size_t k = lo; k < hi; k++) mine += same(i, k);
        stamps[i].good = same(i, best) && (mine >= 3 || hi - lo < 3);
    }
    int bad = 0;
    for (auto &st : stamps) { if (st.good) sids[st.m.sid]++; else bad++; }
    // отрезки с одним смещением
    struct Seg { size_t a, b; };
    std::vector<Seg> segs;
    for (size_t i = 0; i < stamps.size(); i++) {
        if (!stamps[i].good) continue;
        if (!segs.empty()) {
            auto &L = stamps[segs.back().b];
            if (fabs(L.off - stamps[i].off) <= 1.5 && L.m.sid == stamps[i].m.sid) { segs.back().b = i; continue; }
        }
        segs.push_back({i, i});
    }
    txt += fmt("read %d %ss\n\n", ok, R.unit.c_str());
    txt += "Frame  packets  empty  time stamp (inverted)  text  other\n";
    for (auto &kv : fst)
        txt += fmt("  %2d  %7d  %5d  %10d (%d)  %12d  %5d\n", kv.first, kv.second.n, kv.second.empty, kv.second.stamp, kv.second.inv, kv.second.ascii, kv.second.other);
    if (!sids.empty()) {
        txt += "\nSource ID (value: times):";
        for (auto &kv : sids) txt += fmt(" %d:%d", kv.first, kv.second);
        txt += "\n";
    }
    if (!text_order.empty()) {
        txt += "\nText sent on frames 20, 22, 24 (5 characters each):\n";
        for (auto &t : text_order) txt += fmt("  \"%s\"  %d times, first at %s\n", t.c_str(), texts[t].first, rec_time(texts[t].second).c_str());
    }
    if (!segs.empty()) {
        txt += "\nOn air (time stamps, usually Eastern time; each row is a stretch where the clock runs with the recording):\n";
        for (auto &g : segs) {
            auto &A = stamps[g.a], &B = stamps[g.b];
            txt += fmt("  recording %s \xE2\x80\x93 %s   on air %s \xE2\x80\x93 %s   source ID %d\n", rec_time(A.u).c_str(), rec_time(B.u).c_str(),
                       amol_stamp_text(A.m).c_str(), amol_stamp_text(B.m).c_str(), A.m.sid);
        }
    }
    if (bad) txt += fmt("\n%d time stamps were misread (out of step with the clock around them) and are left out.\n", bad);
    if (!stamps.empty()) {
        txt += fmt("\nTime stamps (each change):\n%8s  %-10s  %5s  %-3s  %s\n", R.unit.c_str(), "recording", "frame", "SID", "on air");
        std::string last;
        for (auto &st : stamps) {
            std::string t = amol_stamp_text(st.m), key = t + fmt("|%d|%d", st.m.sid, (int)st.good);
            if (key == last) continue;
            last = key;
            txt += fmt("%8d  %s  %5d  %-3d  %-20s%s%s\n", st.u, rec_time(st.u).c_str(), st.m.frame, st.m.sid, t.c_str(),
                       st.m.inverted ? "  inverted copy" : "", st.good ? "" : "  MISREAD");
        }
    }
    if (!others.empty()) {
        std::vector<std::pair<int, uint64_t>> v; for (auto &kv : others) v.push_back({kv.second.first, kv.first});
        std::sort(v.rbegin(), v.rend());
        txt += "\nOther messages (35 bits, not a time stamp or text; most frequent):\n";
        for (size_t i = 0; i < v.size() && i < 30; i++) {
            std::string fr; for (int x : others[v[i].second].second) fr += fmt(" %d", x);
            std::string bits; for (int k = 34; k >= 0; k--) bits += (char)('0' + ((v[i].second >> k) & 1));
            txt += fmt("  %s  %6d times  frames%s\n", bits.c_str(), v[i].first, fr.c_str());
        }
    }
    txt += fmt("\nParity over the message: even in %d, odd in %d packets\n", pe, po);
    write_text(out, txt);
    Json j = Json::object();
    j["source"] = basename(R.path); j["line"] = (double)R.tv.at(row); j["field"] = par < 0 ? "" : std::string(1, "AB"[par]);
    j["unit"] = R.unit; j["rate"] = rate; j["units"] = (double)R.n;
    j["packets"] = pk; j["report"] = out;
    Json tj = Json::array(); for (auto &t : text_order) tj.push(t); j["texts"] = tj;
    Json sj = Json::array();                        // проверенные отметки: место, текст, код источника, кадр
    for (auto &st : stamps) if (st.good) { Json e = Json::array(); e.push((double)st.u); e.push(amol_stamp_text(st.m)); e.push((double)st.m.sid); e.push((double)st.m.frame); sj.push(e); }
    j["stamps"] = sj;
    save_json(stem_path(out) + ".json", j);
    pr.log(fmt("AMOL line %d: %d %ss read, %zu time stamps (%d misread), %zu texts", R.tv.at(row), ok, R.unit.c_str(), stamps.size(), bad, text_order.size()));
    return ok;
}

int gemstar_report(const Rec &R, int row, int par, const std::string &out, Progress &pr) {
    std::string txt = fmt("Gemstar 2x programme guide data on line %d%s of %s\n", R.tv.at(row), par < 0 ? "" : (std::string(" field ") + "AB"[par]).c_str(), basename(R.path).c_str());
    txt += "Format: clock run-in at the caption rate (0.5 MHz), then at twice that rate (1.007 Mbit/s) the frame code 011101101\n"
           "and 4 data bytes (LSB first). This is the TV Guide On Screen / Guide Plus+ listings stream; the bytes are encrypted\n"
           "and compressed by Gemstar, so only the raw stream can be shown.\n\n";
    int ok = 0, tot = 0; uint8_t b[4];
    std::vector<std::pair<int, uint32_t>> w;
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) {
        tot++;
        if (!gemstar2x_slice(R.line(u, row), R.fs, b)) continue;
        ok++; w.push_back({u, (uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24});
        if (u % 5000 < 2) pr.progress(u, R.n);
    }
    std::map<uint32_t, int> cnt; for (auto &x : w) cnt[x.second]++;
    int rep = 0; for (auto &kv : cnt) if (kv.second > 1) rep += kv.second;
    txt += fmt("read %d of %d %ss (%.0f%%); %zu different 4-byte words, %d of the read words occur more than once (repeated carousel)\n\n",
               ok, tot, R.unit.c_str(), 100.0 * ok / std::max(1, tot), cnt.size(), rep);
    double rate = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1);
    std::string line; int last = -100;
    for (auto &x : w) {
        if (x.first - last > (par >= 0 ? 2 : 1) * 16 || line.size() > 100) {
            if (!line.empty()) txt += line + "\n";
            double s = x.first / rate;
            line = fmt("%02d:%02d:%05.2f ", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60));
        }
        line += fmt(" %02X%02X%02X%02X", x.second & 255, (x.second >> 8) & 255, (x.second >> 16) & 255, x.second >> 24);
        last = x.first;
    }
    if (!line.empty()) txt += line + "\n";
    write_text(out, txt);
    pr.log(fmt("Gemstar line %d: %d %ss read", R.tv.at(row), ok, R.unit.c_str()));
    return ok;
}

int vitc_report(const Rec &R, int row, int par, const std::string &out, Progress &pr) {
    double fh = R.ntsc() ? 15734.264 : 15625.0;
    std::string txt = fmt("VITC timecode on line %d%s of %s\n\n", R.tv.at(row), par < 0 ? "" : (std::string(" field ") + "AB"[par]).c_str(), basename(R.path).c_str());
    int ok = 0; uint8_t b[9]; std::string last;
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) {
        if (!vitc_slice(R.line(u, row), R.fs, fh, b)) continue;
        ok++;
        std::string tc = fmt("%d%d:%d%d:%d%d%c%d%d", b[7] & 3, b[6] & 15, b[5] & 7, b[4] & 15, b[3] & 7, b[2] & 15, (b[1] & 4) ? ';' : ':', b[1] & 3, b[0] & 15);
        std::string ub = fmt("%X%X%X%X%X%X%X%X", b[7] >> 4, b[6] >> 4, b[5] >> 4, b[4] >> 4, b[3] >> 4, b[2] >> 4, b[1] >> 4, b[0] >> 4);
        if (u % 50 == 0 || last.empty()) txt += fmt("%8d  %s  user bits %s\n", u, tc.c_str(), ub.c_str());
        last = tc;
        if (u % 5000 < 2) pr.progress(u, R.n);
    }
    txt += fmt("\nread %d %ss; last timecode %s\n", ok, R.unit.c_str(), last.c_str());
    write_text(out, txt);
    pr.log(fmt("VITC line %d: %d %ss read", R.tv.at(row), ok, R.unit.c_str()));
    return ok;
}
