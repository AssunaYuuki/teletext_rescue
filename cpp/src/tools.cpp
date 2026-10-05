#include "tools.h"
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

static std::vector<std::pair<double, double>> mb_bursts(const std::vector<float> &m, double fs) {
    int n = (int)m.size(), w = std::max(3, (int)(fs * 0.6e-6)) | 1, h = w / 2;
    std::vector<double> d(n), act(n);
    for (int i = 0; i < n; i++) d[i] = fabs(m[i] - (i ? m[i - 1] : m[0]));
    for (int i = 0; i < n; i++) { double s = 0; for (int k = i - h; k <= i + h; k++) if (k >= 0 && k < n) s += d[k]; act[i] = s / w; }
    double mx = 0; for (int i = (int)(n * 0.12); i < n; i++) mx = std::max(mx, act[i]);
    double thr = 0.15 * mx;
    std::vector<std::pair<double, double>> out;
    int i = (int)(n * 0.12);
    while (i < n) {
        if (act[i] <= thr) { i++; continue; }
        int j = i; while (j < n && act[j] > thr) j++;
        if (j - i > fs * 0.8e-6) {
            double mean = 0; for (int k = i; k < j; k++) mean += m[k]; mean /= (j - i);
            int z = 0; for (int k = i + 1; k < j; k++) z += ((m[k] - mean) > 0) != ((m[k - 1] - mean) > 0);
            double cycles = z / 2.0;
            if (cycles >= 3) {
                int k0 = i + (int)((j - i) * 0.15), k1 = i + (int)((j - i) * 0.85);
                std::vector<double> seg(m.begin() + k0, m.begin() + k1);
                out.push_back({round(cycles / ((j - i) / fs) / 1e6 * 100) / 100, percentile(seg, 98) - percentile(seg, 2)});
            }
        }
        i = j;
    }
    return out;
}

Json vits_analyse(const std::string &path, const Json &res, Progress &pr) {
    auto fc = detect_format(path);
    Json out = Json::array();
    if (fc.first.empty()) return out;
    Rec R(path, fc.first);
    const double *freqs = !R.ntsc() ? MB_PAL : MB_NTSC;
    for (auto &L : res["lines"].a) {
        if (L["kind"].str().rfind("test signal", 0) != 0) continue;
        int par = L["parity"].is_null() ? -1 : L["parity"].integer();
        auto m = median_line(R, L["row"].integer(), par);
        std::map<double, double> amp;
        for (auto &b : mb_bursts(m, R.fs)) {
            double k = freqs[0]; for (int q = 0; q < 6; q++) if (fabs(freqs[q] - b.first) < fabs(k - b.first)) k = freqs[q];
            if (fabs(k - b.first) < 0.25 * k) amp[k] = std::max(amp[k], b.second);
        }
        if (amp.size() < 3 || !amp.count(freqs[0])) continue;
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
    const Json &o = vt[0];
    t.push_back(fmt("Frequency response of the recording (multiburst test signal, line %d):", o["tv_line"].integer()));
    std::string s; double cut = 0;
    for (auto &e : o["response_db"].a) {
        double f = e[0].num(), db = e[1].num();
        s += (s.empty() ? "" : ", ") + (db > -30 ? fmt("%g MHz %+.1f dB", f, db) : fmt("%g MHz \xE2\x80\x94", f));
        if (db < -20 && (cut == 0 || f < cut)) cut = f;
    }
    t.push_back("  " + s);
    if (cut) t.push_back(fmt("  (nothing above ~%g MHz passes \xE2\x80\x94 typical for VHS luma)", cut));
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
int amol_report(const Rec &R, int row, int par, const std::string &out, Progress &pr) {
    std::string txt = fmt("AMOL (Nielsen Automated Measurement of Lineups) on line %d%s of %s\n", R.tv.at(row),
                          par < 0 ? "" : (std::string(" field ") + "AB"[par]).c_str(), basename(R.path).c_str());
    txt += "48 bits: phase reference 101, start 0110, frame address (5), source ID (6), month, day, hour, minute, second, AM/PM, spare (3), parity\n\n";
    txt += fmt("%8s  %5s  %-6s  %-22s  %s\n", R.unit.c_str(), "frame", "SID", "date and time", "bits");
    int ok = 0, pe = 0, po = 0; std::string last;
    std::map<std::string, int> stamps; std::map<int, std::map<int, int>> sid;
    uint8_t b[48];
    // для окна AMOL: все прочитанные пакеты (номер поля/кадра, 48 бит в 12 hex-знаках)
    Json pk = Json::array();
    for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) {
        if (!amol_slice(R.line(u, row), R.fs, b)) continue;
        ok++;
        auto val = [&](int a, int n) { int v = 0; for (int i = a; i < a + n; i++) v = (v << 1) | b[i]; return v; };
        int fa = val(7, 5), s = val(12, 6), mo = val(18, 4), dy = val(22, 5), hh = val(27, 4), mi = val(31, 6), ss = val(37, 6), pm = b[43];
        int ones = 0; for (int i = 12; i < 47; i++) ones += b[i];
        if ((ones + b[47]) % 2 == 0) pe++; else po++;
        sid[fa][s]++;
        std::string st = fmt("%d/%d %d:%02d:%02d %s", mo, dy, hh, mi, ss, pm ? "PM" : "AM");
        stamps[st]++;
        std::string bits; for (int i = 0; i < 48; i++) bits += (char)('0' + b[i]);
        { std::string hx; for (int i = 0; i < 48; i += 4) hx += "0123456789ABCDEF"[(b[i] << 3) | (b[i + 1] << 2) | (b[i + 2] << 1) | b[i + 3]];
          Json e = Json::array(); e.push((double)u); e.push(hx); pk.push(e); }
        std::string row_s = fmt("%8d  %5d  %-6d  %-22s  ", u, fa, s, st.c_str()) + bits;
        if (st != last || u % 300 == 0) txt += row_s + "\n";
        last = st;
        if (u % 5000 < 2) pr.progress(u, R.n);
    }
    txt += fmt("\nread %d %ss; parity even in %d, odd in %d\n", ok, R.unit.c_str(), pe, po);
    txt += "Source ID by frame address (value: times):\n";
    for (auto &f : sid) { std::string s; for (auto &v : f.second) s += fmt(" %d:%d", v.first, v.second); txt += fmt("  frame %2d:", f.first) + s + "\n"; }
    write_text(out, txt);
    Json j = Json::object();
    j["source"] = basename(R.path); j["line"] = (double)R.tv.at(row); j["field"] = par < 0 ? "" : std::string(1, "AB"[par]);
    j["unit"] = R.unit; j["rate"] = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1); j["units"] = (double)R.n;
    j["packets"] = pk; j["report"] = out;
    save_json(stem_path(out) + ".json", j);
    pr.log(fmt("AMOL line %d: %d %ss read", R.tv.at(row), ok, R.unit.c_str()));
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
