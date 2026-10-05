#include "nabts_slicer.h"
#include "teletext.h"
#include "vbidecode.h"

bool nabts_good_prefix(const u8 *p) { for (int i = 0; i < 5; i++) if (ham::dec[p[i]] < 0) return false; return true; }
static bool wst525_good(const u8 *p) { return ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0; }

MlseModel model525(Svc525 kind) {
    MlseModel m;
    m.spb = 27e6 / 5727272.0; m.pre[0] = 0x55; m.pre[1] = 0x55; m.L = 3; m.R = 3; m.ntop = 2; m.refits = 1;
    if (kind == Svc525::NABTS) { m.pre[2] = 0xE7; m.nbytes = 33; m.good = nabts_good_prefix; }
    else { m.pre[2] = 0x27; m.nbytes = 34; m.good = wst525_good; }
    return m;
}

static void line27(const Rec &R, int u, int r, float *out) {
    if (R.fs == 27e6 && R.ns == 1440) { R.line(u, r, out); return; }
    resample27(R, R.line(u, r), out);
}
static bool has_signal27(const float *y) {
    double s = 0, s2 = 0; for (int i = 100; i < 1380; i++) { s += y[i]; s2 += (double)y[i] * y[i]; }
    double m = s / 1280; return sqrt(std::max(0.0, s2 / 1280 - m * m)) > 12;
}

static double check(const Rec &R, int row, const std::vector<int> &uu, Svc525 kind) {
    MlseDecoder dec(model525(kind));
    int ok = 0, n = 0; float y27[1440];
    for (int u : uu) {
        auto y = R.line(u, row);
        std::vector<double> d(y.begin(), y.end());
        if (stdev(d) < 5) continue;
        resample27(R, y, y27);
        auto r = dec.decode(y27, 1440, 0, 200, 1.0);
        ok += r.good; n++;
    }
    return round(1000.0 * ok / std::max(1, n)) / 1000;
}
double nabts_check(const Rec &R, int row, const std::vector<int> &uu) { return check(R, row, uu, Svc525::NABTS); }
double wst525_check(const Rec &R, int row, const std::vector<int> &uu) {
    double r = check(R, row, uu, Svc525::WST);
    // у NABTS тоже «верный адрес» в двух байтах бывает часто — WST признаётся, только если NABTS хуже
    return r;
}

void slice525(const Rec &R, const std::vector<int> &lines, Svc525 kind, const std::string &out, Progress &pr) {
    MlseModel M = model525(kind);
    const char *what = kind == Svc525::NABTS ? "NABTS" : "WST (525 lines)";
    int nf = R.n;
    bool cx = R.fs == 27e6 && R.ns == 1440;
    double lo = 0, hi = cx ? 60 : 300;
    std::vector<int> all = cx ? R.rows : lines;
    pr.step(std::string("Finding lines with a signal for ") + what);
    std::vector<std::vector<char>> sig(nf, std::vector<char>(R.rec_lines, 0));
    parallel_for(nf, [&](size_t f) {
        float y[1440];
        for (int r : all) { line27(R, (int)f, r, y); sig[f][r] = has_signal27(y); }
    }, &pr, 64);
    std::vector<int> extra;
    if (cx)
        for (int r : R.rows) {
            if (std::find(lines.begin(), lines.end(), r) != lines.end()) continue;
            double s = 0; for (int f = 0; f < nf; f++) s += sig[f][r];
            s /= std::max(1, nf);
            if (s > 0 && s < 0.25) extra.push_back(r);
        }
    auto names = [&](const std::vector<int> &v) { std::string s; for (int r : v) s += (s.empty() ? "" : ", ") + std::to_string(R.tv.at(r)); return s; };
    pr.step(std::string("Reading ") + what + " from lines " + names(lines) + (extra.empty() ? "" : "; looking for packets on " + names(extra)));
    MlseDecoder dec(M); double off = -1;
    float y[1440];
    for (int f = 0; f < nf; f++) {
        for (int r : lines) if (sig[f][r]) { line27(R, f, r, y); off = dec.decode(y, 1440, lo, hi).off; }
        if (dec.n > 200 || f > 400) break;
    }
    if (!dec.has_prior()) throw std::runtime_error(std::string("No readable ") + what + " packets on lines " + names(lines));
    lo = std::max(0.0, off - 6); hi = off + 6;
    MlseBatch bd(M, dec.prior, dec.n, true, pr);
    std::vector<int> use = lines; use.insert(use.end(), extra.begin(), extra.end());
    std::vector<std::pair<int, int>> FL;
    for (int f = 0; f < nf; f++) {
        std::vector<int> rs;
        for (int r : use) if (sig[f][r]) rs.push_back(r);
        std::sort(rs.begin(), rs.end(), [&](int a, int b) { return R.tv.at(a) < R.tv.at(b); });
        for (int r : rs) FL.push_back({f, r});
    }
    std::vector<Bytes> D(FL.size()); std::vector<char> G(FL.size(), 0), mainl(FL.size(), 0);
    for (size_t i = 0; i < FL.size(); i++) mainl[i] = std::find(lines.begin(), lines.end(), FL[i].second) != lines.end();
    std::vector<float> buf;
    for (size_t b = 0; b < FL.size(); b += bd.batch()) {
        size_t n = std::min(bd.batch(), FL.size() - b);
        buf.resize(n * 1440);
        std::vector<const float *> rp(n);
        parallel_for(n, [&](size_t i) { line27(R, FL[b + i].first, FL[b + i].second, &buf[i * 1440]); }, &pr, 64);
        for (size_t i = 0; i < n; i++) rp[i] = &buf[i * 1440];
        std::vector<char> learn(mainl.begin() + b, mainl.begin() + b + n);
        auto res = bd.decode(rp, 1440, lo, hi, &learn);
        std::vector<double> go;
        for (size_t i = 0; i < n; i++) { D[b + i] = res[i].data; G[b + i] = res[i].good; if (res[i].good && learn[i]) go.push_back(res[i].off); }
        if (!go.empty()) { double c = median(go); lo = std::max(0.0, c - 6); hi = c + 6; }
        pr.progress(FL[b + n - 1].first, nf);
    }
    std::vector<char> keep = mainl;
    for (int r : extra)
        for (int par = 0; par < 2; par++) {
            size_t tot = 0, good = 0;
            for (size_t i = 0; i < FL.size(); i++) if (FL[i].second == r && FL[i].first % 2 == par) { tot++; good += G[i]; }
            if (tot && good * 2 >= tot)
                for (size_t i = 0; i < FL.size(); i++) if (FL[i].second == r && FL[i].first % 2 == par && G[i]) keep[i] = 1;
        }
    Bytes all_out; size_t npk = 0, ok = 0;
    for (size_t i = 0; i < FL.size(); i++) if (keep[i]) { all_out.insert(all_out.end(), D[i].begin(), D[i].end()); npk++; ok += G[i]; }
    write_file(out, all_out);
    for (int r : use) {
        size_t n = 0, g = 0;
        for (size_t i = 0; i < FL.size(); i++) if (keep[i] && FL[i].second == r) { n++; g += G[i]; }
        if (n) pr.log(fmt("line %d: %zu packets, %zu with an error-free prefix", R.tv.at(r), n, g));
    }
    pr.log(fmt("lines with signal %zu, error-free prefix %zu (%.0f%%), written to ", npk, ok, 100.0 * ok / std::max<size_t>(1, npk)) + out);
}
