#include "nabts_slicer.h"
#include "teletext.h"
#include "vbidecode.h"

bool nabts_good_prefix(const u8 *p) { for (int i = 0; i < 5; i++) if (ham::dec[p[i]] < 0) return false; return true; }
static bool wst525_good(const u8 *p) { return ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0; }
// JTES: пакет исправим кодом (272,190) и не пуст (строка без сигнала читается нулями — тоже кодовое слово)
static bool jtes_good(const u8 *p) {
    int nz = 0, ff = 0; for (int i = 0; i < 34; i++) { nz += p[i] != 0; ff += p[i] == 0xFF; }
    if (nz < 6 || ff > 28) return false;
    u8 t[34]; memcpy(t, p, 34);
    return jtes_correct(t) >= 0;
}

MlseModel model525(Svc525 kind) {
    MlseModel m;
    m.spb = 27e6 / 5727272.0; m.pre[0] = 0x55; m.pre[1] = 0x55; m.L = 3; m.R = 3; m.ntop = 2; m.refits = 1;
    if (getenv("TR_L")) m.L = atoi(getenv("TR_L"));
    if (getenv("TR_R")) m.R = atoi(getenv("TR_R"));
    if (kind == Svc525::NABTS) { m.pre[2] = 0xE7; m.nbytes = 33; m.good = nabts_good_prefix; }
    else if (kind == Svc525::WST) { m.pre[2] = 0x27; m.nbytes = 34; m.good = wst525_good; }
    else { m.pre[2] = 0xA7; m.nbytes = 34; m.good = jtes_good; }
    return m;
}

// строка в сетке 27 МГц: 1440 отсчётов, как у cx23885; пакет JTES длиннее (до ~61 мкс от начала записи 4fsc),
// для него окно шире и начинается раньше
static int W27(const Rec &R, Svc525 k) { return (!getenv("TR_W1440") && k == Svc525::JTES && !(R.fs == 27e6 && R.ns == 1440)) ? 1800 : 1440; }
static void line27(const Rec &R, int u, int r, float *out, Svc525 k = Svc525::NABTS) {
    if (R.fs == 27e6 && R.ns == 1440) { R.line(u, r, out); return; }
    if (W27(R, k) == 1440) { resample27(R, R.line(u, r), out); return; }
    auto y = R.line(u, r);
    double mx = 1; for (float v : y) mx = std::max(mx, (double)v);
    double t0 = std::max(0.0, R.t27_start() - 180 * R.fs / 27e6);
    for (int i = 0; i < 1800; i++) out[i] = (float)std::clamp(interp(i / 27e6 * R.fs + t0, y.data(), (int)y.size()) * (255.0 / mx), 0.0, 255.0);
}
static bool has_signal27(const float *y) {
    double s = 0, s2 = 0; for (int i = 100; i < 1380; i++) { s += y[i]; s2 += (double)y[i] * y[i]; }
    double m = s / 1280; return sqrt(std::max(0.0, s2 / 1280 - m * m)) > 12;
}

// начало пакета: корреляция строки с эталоном «вступление + код кадра» (±1 по битам, без средней);
// -> смещение в отсчётах 27 МГц или -1
static double find_preamble(const float *y, int n, const MlseModel &M, double hi) {
    int bits[24]; for (int i = 0; i < 24; i++) bits[i] = ((M.pre[i / 8] >> (i % 8)) & 1) ? 1 : -1;
    double best = 0, boff = -1;
    for (double off = 0; off <= hi; off += 0.5) {
        double s = 0, m = 0, e = 0;
        for (int i = 0; i < 24; i++) { double v = interp(off + (i + 0.5) * M.spb, y, n); m += v; }
        m /= 24;
        for (int i = 0; i < 24; i++) { double v = interp(off + (i + 0.5) * M.spb, y, n) - m; s += v * bits[i]; e += v * v; }
        double c = e > 0 ? s / sqrt(e * 24) : 0;
        if (c > best) { best = c; boff = off; }
    }
    return best > 0.5 ? boff : -1;
}

static double check(const Rec &R, int row, const std::vector<int> &uu, Svc525 kind) {
    MlseDecoder dec(model525(kind));
    int ok = 0, n = 0; float y27[1800]; int W = W27(R, kind);
    for (int u : uu) {
        auto y = R.line(u, row);
        std::vector<double> d(y.begin(), y.end());
        if (stdev(d) < 5) continue;
        line27(R, u, row, y27, kind);
        if (getenv("TR_DUMP27") && n == 2 && R.tv.at(row) == 14) { FILE *f = fopen(getenv("TR_DUMP27"), "wb"); if (f) { fwrite(y27, 4, W, f); fwrite(y.data(), 4, y.size(), f); fclose(f); } }
        // JTES: вступление бывает позже (до ~400 отсчётов), прочитан — если исправим кодом (272,190)
        MlseResult r;
        if (kind == Svc525::JTES) {           // короткое вступление: сначала найти его, потом MLSE рядом
            double o = find_preamble(y27, W, dec.M, 380);
            if (o < 0) { n++; continue; }
            r = dec.decode(y27, W, std::max(0.0, o - 6), o + 6, 0.5);
        } else r = dec.decode(y27, W, 0, 200, 1.0);
        if (getenv("TR_JDBG") && kind == Svc525::JTES && n < 6 && r.data.size() >= 34) {
            u8 t[34]; memcpy(t, r.data.data(), 34); int fx = jtes_correct(t);
            fprintf(stderr, "u%d off %.1f good %d syn0 %d fix %d: %02X %02X %02X %02X ... %02X\n", u, r.off, (int)r.good, (int)jtes_syndrome_zero(r.data.data()), fx, r.data[0], r.data[1], r.data[2], r.data[3], r.data[33]);
        }
        ok += r.good; n++;
    }
    return round(1000.0 * ok / std::max(1, n)) / 1000;
}
double nabts_check(const Rec &R, int row, const std::vector<int> &uu) { return check(R, row, uu, Svc525::NABTS); }
double jtes_check(const Rec &R, int row, const std::vector<int> &uu) { return check(R, row, uu, Svc525::JTES); }
double wst525_check(const Rec &R, int row, const std::vector<int> &uu) {
    double r = check(R, row, uu, Svc525::WST);
    // у NABTS тоже «верный адрес» в двух байтах бывает часто — WST признаётся, только если NABTS хуже
    return r;
}

static void slice_impl(const Rec &R, const std::vector<int> &lines, Svc525 kind, const std::string &out, Progress &pr,
                       std::vector<JtesPacket> *mem);
void slice525(const Rec &R, const std::vector<int> &lines, Svc525 kind, const std::string &out, Progress &pr) { slice_impl(R, lines, kind, out, pr, nullptr); }
std::vector<JtesPacket> slice_jtes(const Rec &R, const std::vector<int> &lines, Progress &pr) {
    std::vector<JtesPacket> v; slice_impl(R, lines, Svc525::JTES, "", pr, &v); return v;
}
static void slice_impl(const Rec &R, const std::vector<int> &lines, Svc525 kind, const std::string &out, Progress &pr,
                       std::vector<JtesPacket> *mem) {
    MlseModel M = model525(kind);
    const char *what = kind == Svc525::NABTS ? "NABTS" : kind == Svc525::WST ? "WST (525 lines)" : "Japanese teletext";
    int nf = R.n;
    bool cx = R.fs == 27e6 && R.ns == 1440;
    // поиск места пакета: на первых строках шире (у записей cx23885 пакет начинается по-разному), потом ±6 вокруг найденного
    double lo = 0, hi = kind == Svc525::JTES ? 380 : cx ? 200 : 300;
    const int W = W27(R, kind);
    std::vector<int> all = cx ? R.rows : lines;
    pr.step(std::string("Finding lines with a signal for ") + what);
    std::vector<std::vector<char>> sig(nf, std::vector<char>(R.rec_lines, 0));
    parallel_for(nf, [&](size_t f) {
        float y[1800];
        for (int r : all) { line27(R, (int)f, r, y, kind); sig[f][r] = has_signal27(y); }
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
    std::vector<double> goodoff;                     // смещения только прочитанных строк (последняя может быть мусором)
    float y[1800];
    for (int f = 0; f < nf; f++) {
        for (int r : lines) if (sig[f][r]) {
            line27(R, f, r, y, kind);
            if (kind == Svc525::JTES) { double o = find_preamble(y, W, M, hi); if (o >= 0) { auto q = dec.decode(y, W, std::max(0.0, o - 6), o + 6, 0.5); if (q.good) goodoff.push_back(q.off); } }
            else { auto q = dec.decode(y, W, lo, hi); if (q.good) goodoff.push_back(q.off); }
        }
        if (dec.n > 200 || f > 400) break;
    }
    if (!dec.has_prior() || goodoff.empty()) throw std::runtime_error(std::string("No readable ") + what + " packets on lines " + names(lines));
    off = median(goodoff);
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
        buf.resize(n * W);
        std::vector<const float *> rp(n);
        parallel_for(n, [&](size_t i) { line27(R, FL[b + i].first, FL[b + i].second, &buf[i * W], kind); }, &pr, 64);
        for (size_t i = 0; i < n; i++) rp[i] = &buf[i * W];
        std::vector<char> learn(mainl.begin() + b, mainl.begin() + b + n);
        auto res = bd.decode(rp, W, lo, hi, &learn);
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
    if (mem) {                                       // JTES: исправить кодом (272,190), негодные отбросить
        std::vector<JtesPacket> v(FL.size()); std::vector<char> okp(FL.size(), 0);
        parallel_for(FL.size(), [&](size_t i) {
            if (!keep[i] || D[i].size() < 34) return;
            JtesPacket p; p.field = FL[i].first; p.line = R.tv.at(FL[i].second);
            memcpy(p.d.data(), D[i].data(), 34);
            if (getenv("TR_JTESDUMP") && i < 40) { std::string h; for (int k = 0; k < 34; k++) h += fmt("%02X ", D[i][k]); fprintf(stderr, "%d L%d %s\n", p.field, p.line, h.c_str()); }
            p.fixed = jtes_correct(p.d.data());
            if (p.fixed >= 0) { v[i] = p; okp[i] = 1; }
        }, &pr, 256);
        size_t fixed = 0;
        for (size_t i = 0; i < FL.size(); i++) if (keep[i]) { npk++; if (okp[i]) { mem->push_back(v[i]); ok++; fixed += v[i].fixed > 0; } }
        pr.log(fmt("Japanese teletext: %zu lines with signal, %zu packets passed the (272,190) code (%zu after correction)", npk, ok, fixed));
        return;
    }
    Bytes posb;                                      // <out>.pos: поле и строка ТВ каждого пакета (по 2 × 4 байта)
    for (size_t i = 0; i < FL.size(); i++) if (keep[i]) {
        all_out.insert(all_out.end(), D[i].begin(), D[i].end()); npk++; ok += G[i];
        uint32_t v[2] = {(uint32_t)FL[i].first, (uint32_t)R.tv.at(FL[i].second)};
        for (uint32_t x : v) for (int k = 0; k < 4; k++) posb.push_back((u8)(x >> (8 * k)));
    }
    write_file(out, all_out);
    write_file(out + ".pos", posb);
    for (int r : use) {
        size_t n = 0, g = 0;
        for (size_t i = 0; i < FL.size(); i++) if (keep[i] && FL[i].second == r) { n++; g += G[i]; }
        if (n) pr.log(fmt("line %d: %zu packets, %zu with an error-free prefix", R.tv.at(r), n, g));
    }
    pr.log(fmt("lines with signal %zu, error-free prefix %zu (%.0f%%), written to ", npk, ok, 100.0 * ok / std::max<size_t>(1, npk)) + out);
}
