#include "silent_radio.h"

const double SR_BITRATE = FSC_NTSC * 7 / 20;

static int sgn(double v) { return (v > 0) - (v < 0); }

bool sr_slice_line(const std::vector<float> &y, double T, uint8_t out[16]) {
    int n = (int)y.size();
    int a = (int)(n * 0.15), b = (int)(n * 0.29);
    std::vector<double> seg(y.begin() + a, y.begin() + b);
    double lo = percentile(seg, 10), hi = percentile(seg, 90);
    if (hi - lo < 0.15 * std::max(hi, 1.0)) return false;
    double th = (lo + hi) / 2;
    std::vector<double> t;
    for (int i = a; i < b && i + 1 < n; i++) {
        double d0 = y[i] - th, d1 = y[i + 1] - th;
        if (sgn(d0) != sgn(d1)) t.push_back(i + (d0 - d1 != 0 ? d0 / (d0 - d1) : 0));
    }
    if (t.size() < 6) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = (int)t.size();
    for (int i = 0; i < m; i++) { double k = round((t[i] - t[0]) / T); sx += k; sy += t[i]; sxx += k * k; sxy += k * t[i]; }
    double den = m * sxx - sx * sx; if (den == 0) return false;
    double pa = (m * sxy - sx * sy) / den, pb = (sy - pa * sx) / m;
    if (fabs(pa - T) > 0.08 * T) return false;
    uint8_t bits[31];
    for (int i = 0; i < 31; i++) {
        double pos = pb + T / 2 + i * pa;
        if (pos >= n - 1) return false;
        bits[i] = interp(pos, y.data(), n) > th;
    }
    static const uint8_t want[15] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1};
    for (int i = 0; i < 15; i++) if (bits[i] != want[i]) return false;
    memcpy(out, bits + 15, 16);
    return true;
}

std::pair<Bytes, int> sr_read_bytes(const std::string &path, Progress &pr, int row, int parity) {
    auto fc = detect_format(path);
    if (fc.first.empty()) throw std::runtime_error("unknown recording format");
    Rec R(path, fc.first);
    double T = R.fs_h() / SR_BITRATE;
    uint8_t b16[16];
    if (row < 0) {
        int best = -1, br = -1, bp = -1;
        bool fld = R.unit == "field";
        for (int r : R.rows)
            for (int par = fld ? 0 : -1; par <= (fld ? 1 : -1); par++) {
                int ok = 0;
                for (int u = par >= 0 ? par : 0; u < std::min(R.n, 4000); u += par >= 0 ? 74 : 37) ok += sr_slice_line(R.line_h(u, r), T, b16);
                if (ok > best) { best = ok; br = r; bp = par; }
            }
        if (best <= 5) throw std::runtime_error("no Silent Radio signal (line-21 style data at 1.25 Mbit/s) found");
        row = br; parity = bp;
    }
    pr.log(fmt("Silent Radio data on line %d (record row %d), %d %ss", R.tv[row], row, R.n, R.unit.c_str()));
    Bytes out; int good = 0, bad = 0;
    for (int u = parity >= 0 ? parity : 0; u < R.n; u += parity >= 0 ? 2 : 1) {
        if (!sr_slice_line(R.line_h(u, row), T, b16)) { bad++; continue; }
        good++;
        int v0 = 0, v1 = 0; for (int i = 0; i < 8; i++) { v0 = (v0 << 1) | b16[i]; v1 = (v1 << 1) | b16[8 + i]; }
        out.push_back((u8)v0); out.push_back((u8)v1);
        if (u % 2000 < 2) pr.progress(u, R.n);
    }
    pr.log(fmt("%ss with data %d, without %d; %zu bytes", R.unit.c_str(), good, bad, out.size()));
    return {out, R.tv[row]};
}

// ---------------------------------------------------------------- пакеты
struct SrPacket { int seq; std::string zone; int copies, intact; Bytes body; };

static std::vector<SrPacket> packets(const Bytes &data) {
    std::vector<std::pair<size_t, Bytes>> raw;
    for (size_t p = 0; p + 13 <= data.size(); p++) {
        if (data[p] != 0xFF || data[p + 5] != 0 || data[p + 6] != 3 || data[p + 9] != 0xC1) continue;
        size_t L = (data[p + 3] << 8) | data[p + 4];
        size_t at = p; p += 12;      // как finditer: совпадения не перекрываются
        if (L < 8 || at + 13 + L > data.size()) continue;
        raw.push_back({at, Bytes(data.begin() + at, data.begin() + at + 13 + L)});
    }
    std::map<std::string, std::vector<Bytes>> groups; std::vector<std::string> order;
    for (auto &pq : raw) {
        auto &q = pq.second;
        if (q[12] < 0x80) continue;
        std::string key(q.begin() + 1, q.begin() + 5); key += std::string(q.begin() + 10, q.begin() + 12);
        if (!groups.count(key)) order.push_back(key);
        groups[key].push_back(q);
    }
    std::vector<SrPacket> out;
    for (auto &key : order) {
        auto cps = groups[key];
        std::vector<Bytes> whole;
        for (auto &c : cps) { bool z = c.size() >= 6; for (size_t i = c.size() - 6; z && i < c.size(); i++) z = c[i] == 0; if (z) whole.push_back(c); }
        auto &use = whole.empty() ? cps : whole;
        size_t n = SIZE_MAX; for (auto &c : use) n = std::min(n, c.size());
        Bytes voted(n);
        for (size_t i = 0; i < n; i++) { int cnt[256] = {0}; for (auto &c : use) cnt[c[i]]++; int b = 0; for (int v = 1; v < 256; v++) if (cnt[v] > cnt[b]) b = v; voted[i] = (u8)b; }
        SrPacket p; p.seq = (voted[1] << 8) | voted[2];
        if (voted[10] == 2 && voted[11] == 3) p.zone = "A"; else if (voted[10] == 4 && voted[11] == 5) p.zone = "B";
        else if (voted[10] == 6 && voted[11] == 7) p.zone = "C"; else p.zone = fmt("%02x%02x", voted[10], voted[11]);
        p.copies = (int)cps.size(); p.intact = (int)whole.size();
        if (voted.size() > 18) p.body = Bytes(voted.begin() + 16, voted.end() - 2);
        out.push_back(p);
    }
    return out;
}

struct SrRec { int kind; Bytes script; int t; std::map<int, std::string> fields; };   // 0 script, 1 tpl, 2 ctl
static std::vector<SrRec> records(const Bytes &body) {
    std::vector<SrRec> out; size_t i = 0, n = body.size();
    while (i < n) {
        if (body[i] != 0) { i++; continue; }
        if (i + 1 >= n) break;
        int t = body[i + 1];
        if (t == 0x0C && i + 3 < n) {
            size_t L = body[i + 2] | (body[i + 3] << 8);
            size_t e = std::min(n, i + 4 + L);
            out.push_back({0, Bytes(body.begin() + i + 4, body.begin() + e), 0, {}}); i += 4 + L; continue;
        }
        if (t == 0x0A) { out.push_back({2, {}, 0, {}}); i += 5; continue; }
        if (t == 0) { i++; continue; }
        size_t j = i + 2; std::map<int, std::string> fields; int cur = -1; std::string buf;
        while (j < n && body[j] != 0x1F) {
            int c = body[j];
            if (c >= 2 && c <= 9) { if (cur >= 0) fields[cur] = buf; cur = c; buf.clear(); }
            else if (c >= 32 && c < 127) buf += (char)c;
            j++;
        }
        if (cur >= 0) fields[cur] = buf;
        out.push_back({1, {}, t, fields}); i = j + 1;
    }
    return out;
}

static const char *FONT_HEX =
    "0000000000" "00005f0000" "0007000700" "147f147f14" "242a7f2a12" "2313086462" "3649562050" "0008070300"
    "001c224100" "0041221c00" "2a1c7f1c2a" "08083e0808" "0080703000" "0808080808" "0000606000" "2010080402"
    "3e5149453e" "00427f4000" "7249494946" "2141494d33" "1814127f10" "2745454539" "3c4a494931" "4121110907"
    "3649494936" "464949291e" "0000140000" "0040340000" "0008142241" "1414141414" "0041221408" "0201590906"
    "3e415d594e" "7c1211127c" "7f49494936" "3e41414122" "7f4141413e" "7f49494941" "7f09090901" "3e41415173"
    "7f0808087f" "00417f4100" "2040413f01" "7f08142241" "7f40404040" "7f021c027f" "7f0408107f" "3e4141413e"
    "7f09090906" "3e4151215e" "7f09192946" "2649494932" "03017f0103" "3f4040403f" "1f2040201f" "3f4038403f"
    "6314081463" "0304780403" "61594d4543" "007f414141" "0204081020" "004141417f" "0402010204" "4040404040"
    "0003070800" "2054547840" "7f28444438" "3844444428" "384444287f" "3854545418" "00087e0902" "18a4a49c78"
    "7f08040478" "00447d4000" "2040403d00" "7f10284400" "00417f4000" "7c0478047c" "7c08040478" "3844444438"
    "fc18242418" "18242418fc" "7c08040408" "4854545424" "04043f4424" "3c4040207c" "1c2040201c" "3c4030403c"
    "4428102844" "4c9090907c" "4464544c44" "0008364100" "0000770000" "0041360800" "0201020402";

std::vector<int> sr_glyph(char32_t ch) {
    int c = (int)ch;
    if (c < 32 || c >= 127) c = 32;
    if (c == 32) return {0, 0};
    std::vector<int> g;
    for (int k = 0; k < 5; k++) { unsigned v; sscanf(FONT_HEX + ((c - 32) * 5 + k) * 2, "%2x", &v); g.push_back(v); }
    while (!g.empty() && g.front() == 0) g.erase(g.begin());
    while (!g.empty() && g.back() == 0) g.pop_back();
    for (auto &x : g) x &= 0x7F;
    return g;
}
static std::vector<int> text_cols(const std::string &s) {
    std::vector<int> cols;
    for (unsigned char ch : s) { auto g = sr_glyph(ch); cols.insert(cols.end(), g.begin(), g.end()); cols.push_back(0); }
    if (!cols.empty()) cols.pop_back();
    return cols;
}

struct Screen {
    std::vector<int> cols = std::vector<int>(SR_W, 0);
    void put(int x, int col) { if (x >= 0 && x < SR_W) cols[x] |= col & 0x7FFF; }
    int text(int x, const std::string &s, int row) { for (int c : text_cols(s)) { put(x, c << row); x++; } return x; }
};
static Json frame(const std::vector<int> &cols, double hold, const std::string &what = "") {
    Json f = Json::object();
    Json a = Json::array(); for (int c : cols) a.push(c);
    f["f"] = a; f["t"] = round(std::max(hold, 0.04) * 1000) / 1000; f["w"] = what;
    return f;
}

static std::vector<Json> run_script(const Bytes &s) {
    const double HOLD = 2.0;
    std::vector<Json> out; Screen sc; int x = 0, saved = 0; bool dirty = false; size_t i = 0, n = s.size();
    int last = -1; std::vector<char> tnull;
    auto show = [&](int fx) {
        out.push_back(frame(sc.cols, 0)); out.back()["t"] = Json(); tnull.push_back(1);
        if (fx != 0x20) out.back()["fx"] = "wipe";
        last = (int)out.size() - 1; dirty = false;
    };
    while (i < n) {
        int c = s[i];
        if (c >= 0x80 && i + 1 < n) { sc.put(x, ((c & 0x7F) << 8) | s[i + 1]); x++; i += 2; dirty = true; continue; }
        if (c >= 32 && c < 127) {
            size_t j = i; std::string t;
            while (j < n && s[j] >= 32 && s[j] < 127) t += (char)s[j++];
            x = sc.text(x, t, 4); i = j; dirty = true; continue;
        }
        if (c == 0x1C && i + 2 < n) {
            double hold = (s[i + 1] | (s[i + 2] << 8)) / 256.0;
            if (dirty || last < 0) show(0x20);
            double cur = out[last]["t"].is_null() ? 0 : out[last]["t"].num();
            out[last]["t"] = round((cur + hold) * 1000) / 1000;
            i += 3; continue;
        }
        if ((c == 0x1D || c == 0x0A) && i + 1 < n) { x = s[i + 1]; if (c == 0x1D) saved = x; i += 2; continue; }
        if (c == 0x11) { x = saved; i++; continue; }
        if (c == 0x1F && i + 1 < n) {
            int a = s[i + 1];
            if (a == 0x2C) { sc = Screen(); dirty = false; }
            else if (a == 0x32 || a == 0x38) { x = sc.text(x, a == 0x32 ? "12" : "00", 4); dirty = true; }
            else if (a == 0x33) { x = sc.text(x + 2, "PM", 4); dirty = true; }
            else if (a != 0x3F && a != 0x39 && dirty) show(a);
            i += 2; continue;
        }
        if (c == 0x05) { i += 7; continue; }
        if (c == 0x06 || c == 0x07 || c == 0x1B) { i += 2; continue; }
        i++;
    }
    if (dirty) show(0x20);
    for (auto &f : out) if (f["t"].is_null()) f["t"] = HOLD;
    return out;
}

static Json two_lines(const std::string &l1, const std::string *l2, double hold) {
    std::vector<std::pair<std::string, int>> rows;
    if (l2) { rows.push_back({l1, 0}); rows.push_back({*l2, 8}); } else rows.push_back({l1, 4});
    size_t wide = 0; for (auto &r : rows) wide = std::max(wide, text_cols(r.first).size());
    std::string w; for (auto &r : rows) w += (w.empty() ? "" : " / ") + r.first;
    if (wide > SR_W) {
        Json f = Json::object(); Json lines = Json::array();
        for (auto &r : rows) { Json l = Json::object(); Json c = Json::array(); for (int v : text_cols(r.first)) c.push(v); l["cols"] = c; l["row"] = r.second; lines.push(l); }
        f["scroll2"] = lines; f["speed"] = 40; f["w"] = w; return f;
    }
    Screen sc;
    for (auto &r : rows) { auto c = text_cols(r.first); int x0 = (SR_W - (int)c.size()) / 2; for (size_t k = 0; k < c.size(); k++) sc.put(x0 + (int)k, c[k] << r.second); }
    return frame(sc.cols, hold, w);
}

static std::vector<Json> tpl_frames(int t, std::map<int, std::string> &f, std::string &league) {
    auto get = [&](int k) { auto it = f.find(k); return it == f.end() ? std::string() : it->second; };
    if (t == 0x1B) {
        std::string s = get(2);
        Json fr = Json::object(); Json c = Json::array(); for (int v : text_cols("   " + s)) c.push(v);
        fr["scroll"] = c; fr["row"] = 4; fr["speed"] = 40; fr["w"] = s;
        return {fr};
    }
    if (t == 0x30 || t == 0x31) {
        std::string lg = !get(2).empty() ? get(2) : !get(3).empty() ? get(3) : !get(4).empty() ? get(4) : league;
        std::string l1 = get(5) + " " + get(7) + "  " + get(6) + " " + get(8), l2 = lg + "  " + get(9);
        league = lg;
        return {two_lines(l1, &l2, 3.0)};
    }
    std::string l1 = get(2);
    if (f.count(3)) { std::string l2 = f[3]; return {two_lines(l1, &l2, 3.0)}; }
    return {two_lines(l1, nullptr, 3.0)};
}

static const char *PAGE = R"~(<!doctype html><html><head><meta charset="utf-8"><title>Silent Radio</title>
<style>
body{background:#111;color:#ccc;font:14px sans-serif;margin:16px}
#sign{background:#000;border:10px solid #222;border-radius:6px;display:block;margin:8px 0;max-width:100%}
#list{max-height:45vh;overflow:auto;border:1px solid #333;margin-top:8px}
#list div{padding:3px 6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#list div.on{background:#523;color:#fff}
button,select{background:#222;color:#ddd;border:1px solid #444;padding:3px 10px}
#cap{color:#fa4;min-height:1.3em}
</style></head><body>
<div>Silent Radio — <span id="src"></span> · zone <select id="zone"></select>
<button id="play">Pause</button> <button id="prev">&lt;</button> <button id="next">&gt;</button>
<label><input type="checkbox" id="loop" checked> loop</label></div>
<canvas id="sign"></canvas><div id="cap"></div><div id="list"></div>
<script>var DATA=/*DATA*/;</script><script>/*PLAYER*/</script></body></html>)~";

std::string sr_save(const std::string &src, const std::string &out, const Bytes &data, int line, Progress &pr) {
    make_dirs(out);
    write_file(path_join(out, "line21_bytes.bin"), data);
    auto pk = packets(data);
    int withc = 0; for (auto &p : pk) withc += p.copies > 1;
    pr.log(fmt("packets: %zu distinct (%d with copies)", pk.size(), withc));
    std::map<std::string, Json> progs;
    for (auto &p : pk) {
        if (p.body.size() < 4) continue;
        std::vector<Json> frames; std::vector<std::string> texts; std::string league;
        for (auto &r : records(p.body)) {
            if (r.kind == 0) { auto f = run_script(r.script); frames.insert(frames.end(), f.begin(), f.end()); }
            else if (r.kind == 1) {
                auto f = tpl_frames(r.t, r.fields, league); frames.insert(frames.end(), f.begin(), f.end());
                std::string t; for (auto &kv : r.fields) if (!kv.second.empty()) t += (t.empty() ? "" : " / ") + kv.second;
                texts.push_back(t);
            }
        }
        if (frames.empty()) continue;
        std::string title = texts.empty() ? "graphics" : texts[0];
        if (title.size() > 60) title.resize(60);
        Json it = Json::object();
        it["seq"] = fmt("%04X", p.seq); it["title"] = title;
        Json tx = Json::array(); for (auto &t : texts) tx.push(t); it["text"] = tx;
        Json fr = Json::array(); for (auto &f : frames) fr.push(f); it["frames"] = fr;
        it["copies"] = p.copies;
        if (!progs.count(p.zone)) progs[p.zone] = Json::array();
        progs[p.zone].push(it);
    }
    std::string txt;
    for (auto &z : progs) {
        txt += "===== zone " + z.first + "\n";
        for (auto &it : z.second.a) { for (auto &t : it["text"].a) txt += t.str() + "\n"; txt += "\n"; }
    }
    write_text(path_join(out, "text.txt"), txt);
    Json zones = Json::object(); for (auto &z : progs) zones[z.first] = z.second;
    Json d = Json::object(); d["source"] = basename(src); d["line"] = line; d["zones"] = zones;
    std::string js = d.dump();
    write_text(path_join(out, "packets.json"), js);
    std::string html = replace_all(PAGE, "/*DATA*/", js);
    html = replace_all(html, "/*PLAYER*/", resource_text("SILENT_RADIO_PLAYER"));
    write_text(path_join(out, "index.html"), html);
    pr.log("written to " + out);
    return out;
}
