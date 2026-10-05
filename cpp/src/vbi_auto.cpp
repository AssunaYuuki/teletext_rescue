#include "vbi_auto.h"
#include <array>
#include "datacast.h"
#include "decode_vbi.h"
#include "nabts_slicer.h"
#include "pagebuild.h"
#include "silent_radio.h"
#include "teletext.h"
#include "tools.h"
#include "vbi_probe.h"
#include "vhs_wst.h"
#include "flac.h"

// x.vbi -> x_vbi; x.vbi.flac -> x_vbi
std::string vbi_report_dir(const std::string &src) {
    std::string s = abspath(src);
    if (ends_with_i(s, ".flac")) { s = s.substr(0, s.size() - 5); if (ends_with_i(s, ".vbi") || ends_with_i(s, ".u8") || ends_with_i(s, ".u16") || ends_with_i(s, ".tbc")) s = stem_path(s); return s + "_vbi"; }
    return stem_path(s) + "_vbi";
}

// пары байтов строки 21 с верной чётностью (без бита чётности); пакеты XDS в текст подписей не попадают
static std::pair<std::string, int> cc_text(const Rec &R, int row, int parity, std::vector<std::array<int, 3>> *pairs = nullptr) {
    std::string out; int good = 0; int last1 = -1, last2 = -1, b[2];
    bool xds = false;
    for (int u = parity >= 0 ? parity : 0; u < R.n; u += parity >= 0 ? 2 : 1) {
        if (!cc_slice(R.line(u, row), R.fs, b)) continue;
        if (!odd_parity(b[0]) || !odd_parity(b[1])) continue;
        good++;
        int c1 = b[0] & 0x7F, c2 = b[1] & 0x7F;
        if (pairs) pairs->push_back({c1, c2, u});
        if (c1 == 0 && c2 == 0) continue;
        if (c1 >= 0x01 && c1 <= 0x0F) { xds = c1 != 0x0F; continue; }      // XDS: начало/продолжение, 0x0F — конец
        if (c1 >= 0x10 && c1 < 0x20) {
            xds = false;
            if (c1 == last1 && c2 == last2) { last1 = last2 = -1; continue; }
            last1 = c1; last2 = c2;
            static const int ctl[] = {0x2C, 0x2F, 0x2D, 0x20, 0x25, 0x26, 0x27, 0x29};
            if ((c1 == 0x14 || c1 == 0x1C) && std::find(std::begin(ctl), std::end(ctl), c2) != std::end(ctl))
                if (!out.empty() && out.back() != '\n') out += '\n';
            continue;
        }
        if (xds) continue;
        last1 = last2 = -1;
        for (int c : {c1, c2}) if (c >= 0x20) { char32_t s = cc_special(c); out += s ? from_cp(s) : std::string(1, (char)c); }
    }
    return {out, good};
}

// XDS (расширенные данные во втором поле строки 21): станция, сеть, время, передача, рейтинг.
// Пакет: класс+тип, данные, 0x0F + контрольная сумма (сумма всех байтов пакета по модулю 128 = 0)
struct XdsInfo { std::string text, summary; int packets = 0; Json events = Json::array(), top = Json::object(); int tz = -1; bool dst = false; };
static XdsInfo xds_decode(const std::vector<std::array<int, 3>> &pairs) {
    XdsInfo X;
    int cur_u = 0;
    std::map<int, std::vector<int>> open;                // класс начала (нечётный) -> байты пакета
    int cur = -1;
    std::map<std::string, std::map<std::string, int>> seen;   // поле -> значение -> раз
    std::vector<std::string> times;
    std::string last_line;
    static const char *DOW[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    auto chars = [](const std::vector<int> &v, size_t from) { std::string s; for (size_t i = from; i < v.size(); i++) if (v[i] >= 0x20) s += (char)v[i]; return strip(s); };
    auto packet = [&](const std::vector<int> &v) {
        if (v.size() < 4) return;
        int sum = 0; for (int x : v) sum += x;
        if (sum & 0x7F) return;                         // контрольная сумма
        int cls = v[0], type = v[1];
        std::vector<int> d(v.begin() + 2, v.end() - 2);   // без 0x0F и суммы
        std::string key, val;
        if (cls == 0x05 && type == 0x01) { key = "network"; val = chars(d, 0); }
        else if (cls == 0x05 && type == 0x02) {
            key = "station"; std::string c; for (int i = 0; i < 4 && i < (int)d.size(); i++) if (d[i] >= 0x20) c += (char)d[i];
            val = strip(c);
            if (d.size() >= 6) val += " (channel " + std::string(1, (char)d[4]) + std::string(1, (char)d[5]) + ")";
        }
        else if (cls == 0x05 && type == 0x03) { key = "tape delay"; if (d.size() >= 2) val = fmt("%d:%02d", d[1] & 0x1F, d[0] & 0x3F); }
        else if (cls == 0x05 && type == 0x04) { key = "transmission ID"; std::string h; for (int x : d) h += fmt("%X", x & 0x0F); val = h; }
        else if (cls == 0x07 && type == 0x01 && d.size() >= 6) {
            int mi = d[0] & 0x3F, hh = d[1] & 0x1F, dd = d[2] & 0x1F, mo = d[3] & 0x0F, dw = d[4] & 0x07, yy = (d[5] & 0x3F) + 1990;
            key = "time"; val = fmt("%04d-%02d-%02d %02d:%02d UTC", yy, mo, dd, hh, mi) + (dw >= 1 && dw <= 7 ? std::string(" ") + DOW[dw] : "");
            if (times.empty() || times.back() != val) times.push_back(val);
        }
        else if (cls == 0x07 && type == 0x04 && d.size() >= 1) { key = "time zone"; val = fmt("UTC-%d", d[0] & 0x1F) + ((d[0] & 0x20) ? " (area observes daylight saving)" : ""); }
        else if ((cls == 0x01 || cls == 0x03) && type == 0x03) { key = cls == 0x01 ? "programme" : "next programme"; val = chars(d, 0); }
        else if ((cls == 0x01 || cls == 0x03) && type == 0x01 && d.size() >= 4) {
            key = cls == 0x01 ? "programme start" : "next programme start";
            val = fmt("%02d/%02d %02d:%02d UTC", d[3] & 0x0F, d[2] & 0x1F, d[1] & 0x1F, d[0] & 0x3F);
        }
        else if ((cls == 0x01 || cls == 0x03) && type == 0x02 && d.size() >= 2) { key = "programme length"; val = fmt("%d:%02d", d[1] & 0x3F, d[0] & 0x3F); }
        else if ((cls == 0x01 || cls == 0x03) && type == 0x05 && d.size() >= 2) {
            key = "rating";
            int a = d[0], b2 = d[1];
            static const char *MPA[] = {"N/A", "G", "PG", "PG-13", "R", "NC-17", "X", "not rated"};
            static const char *TV[] = {"none", "TV-Y", "TV-Y7", "TV-G", "TV-PG", "TV-14", "TV-MA", "none"};
            if ((a & 0x18) == 0x08 || (a & 0x18) == 0x18) val = std::string("US TV ") + TV[b2 & 7];
            else val = std::string("MPA ") + MPA[a & 7];
        }
        else if ((cls == 0x01 || cls == 0x03) && type >= 0x10 && type <= 0x17) { key = fmt("description %d", type - 0x0F); val = chars(d, 0); }
        else if (cls == 0x01 && type == 0x04) { key = "programme type"; std::string h; for (int x : d) h += fmt(" %02X", x); val = strip(h); }
        else { key = fmt("class %X type %02X", cls, type); std::string h; for (int x : d) h += fmt(" %02X", x); val = strip(h); }
        if (val.empty()) return;
        X.packets++;
        seen[key][val]++;
        Json e = Json::array(); e.push((double)cur_u); e.push(key); e.push(val);
        if (key == "time" && d.size() >= 6) {             // для часов окна: UTC по полям
            Json t = Json::array();
            for (int x : {(d[5] & 0x3F) + 1990, d[3] & 0x0F, d[2] & 0x1F, d[1] & 0x1F, d[0] & 0x3F, d[4] & 0x07}) t.push((double)x);
            e.push(t);
        }
        if (key == "time zone" && d.size() >= 1) { X.tz = d[0] & 0x1F; X.dst = d[0] & 0x20; }
        X.events.push(e);
        std::string line = key + ": " + val;
        if (line != last_line) { X.text += line + "\n"; last_line = line; }
    };
    for (auto &pc : pairs) {
        int c1 = pc[0], c2 = pc[1]; cur_u = pc[2];
        if (c1 >= 0x01 && c1 <= 0x0E) {
            if (c1 & 1) { open[c1] = {c1, c2}; cur = c1; }                  // начало пакета
            else { cur = c1 - 1; if (!open.count(cur)) cur = -1; }           // продолжение
            continue;
        }
        if (c1 == 0x0F) {
            if (cur >= 0 && open.count(cur)) { auto v = open[cur]; v.push_back(c1); v.push_back(c2); packet(v); open.erase(cur); }
            cur = -1; continue;
        }
        if (c1 >= 0x10 && c1 < 0x20) { cur = -1; continue; }                // подписи прерывают XDS
        if (cur >= 0 && open.count(cur) && c1 >= 0x20) { open[cur].push_back(c1); if (c2) open[cur].push_back(c2); }
    }
    // сводка: самое частое значение каждого поля
    auto top = [&](const std::string &k) {
        auto it = seen.find(k); if (it == seen.end()) return std::string();
        std::string b; int n = 0; for (auto &v : it->second) if (v.second > n) { n = v.second; b = v.first; }
        return b;
    };
    std::vector<std::string> parts;
    for (const char *k : {"network", "station", "programme", "rating", "time zone"}) { std::string v = top(k); if (!v.empty()) parts.push_back(std::string(k) + " " + v); }
    if (!times.empty()) parts.push_back("time " + times.front() + (times.size() > 1 ? " \xE2\x80\x93 " + times.back().substr(11) : ""));
    for (auto &p : parts) X.summary += (X.summary.empty() ? "" : " \xC2\xB7 ") + p;
    for (auto &kv : seen) X.top[kv.first] = top(kv.first);
    std::string head = "XDS (extended data services, line 21 field 2): " + std::to_string(X.packets) + " packets with a valid checksum\n";
    if (!X.summary.empty()) head += "Summary: " + X.summary + "\n";
    head += "\nAll values (times received):\n";
    for (auto &kv : seen) for (auto &v : kv.second) head += fmt("  %-22s %s  (%d)\n", kv.first.c_str(), v.first.c_str(), v.second);
    head += "\nIn order of arrival (repeats folded):\n";
    X.text = head + X.text;
    return X;
}


// StarSight (электронная программа передач, разносилась станциями PBS): строка в формате подписей CC,
// два байта на поле без бита чётности. Пакет: 2C 00 1F <тип> 1C …, номер пакета — 9-й байт, общий для обоих полей.
// Содержимое сжато и зашифровано — показывается транспорт: пакеты, нумерация, потери, байты.
static Json starsight_extract(const Rec &R, const std::vector<const Json *> &group, Progress &pr) {
    Json pk = Json::array();
    long fields = 0, read = 0;
    std::map<int, int> types;
    for (auto *L : group) {
        int row = (*L)["row"].integer(), par = (*L)["parity"].is_null() ? -1 : (*L)["parity"].integer();
        std::vector<u8> s; std::vector<int> us;
        int b[2];
        for (int u = par >= 0 ? par : 0; u < R.n; u += par >= 0 ? 2 : 1) {
            fields++;
            bool ok = cc_slice(R.line(u, row), R.fs, b);
            if (ok) read++;
            s.push_back(ok ? (u8)b[0] : 3); s.push_back(ok ? (u8)b[1] : 3); us.push_back(u); us.push_back(u);
            if (u % 4000 < 2) pr.progress(u, R.n);
        }
        std::vector<size_t> hp;
        for (size_t i = 0; i + 9 < s.size(); i++) if (s[i] == 0x2C && s[i + 1] == 0x00 && s[i + 2] == 0x1F && s[i + 4] == 0x1C) hp.push_back(i);
        for (size_t k = 0; k < hp.size(); k++) {
            size_t a = hp[k], e = k + 1 < hp.size() ? hp[k + 1] : s.size();
            for (size_t i = a + 9; i + 3 < e; i++) if (s[i] == 3 && s[i + 1] == 3 && s[i + 2] == 3 && s[i + 3] == 3) { e = i; break; }   // потеряна связь
            if (e - a > 400) e = a + 400;
            std::string hx; for (size_t i = a; i < e; i++) hx += fmt("%02X", s[i]);
            Json j = Json::array();
            j.push((double)us[a]); j.push(std::string(1, par == 1 ? 'B' : 'A')); j.push((double)s[a + 8]); j.push((double)s[a + 3]); j.push(hx);
            pk.push(j); types[s[a + 3]]++;
        }
    }
    std::sort(pk.a.begin(), pk.a.end(), [](const Json &x, const Json &y) { return x[0].num() < y[0].num(); });
    Json r = Json::object();
    r["packets"] = pk; r["fields"] = (double)fields; r["read"] = (double)read;
    return r;
}

Json vbi_auto(const std::string &src_in, bool again, Progress &pr) {
    std::string src = abspath(src_in), rep_dir = vbi_report_dir(src), orig = src;
    make_dirs(rep_dir);
    // запись, сжатая FLAC: один раз распаковывается в папку результатов и дальше читается как обычная
    if (is_flac(src)) {
        std::string raw = path_join(rep_dir, basename(rep_dir.substr(0, rep_dir.size() - 4)) + ".vbi");
        if (!exists(raw) || file_size(raw) == 0) {
            pr.step("Unpacking the FLAC-compressed recording");
            std::string tmp = raw + ".part";
            flac_to_raw(src, tmp, pr);
            std::error_code ec; fs::rename(P(tmp), P(raw), ec);
            if (ec) throw std::runtime_error("cannot write " + raw);
        } else pr.log("FLAC already unpacked: " + raw);
        src = raw;
    }
    pr.step("Looking at the recording: format and what each line carries");
    Json res = vbi_probe(src, pr);
    if (res["format"].is_null()) {
        pr.log("Unknown recording format");
        Json r = Json::object(); r["file"] = orig; r["format"] = Json(); r["results"] = Json::array();
        save_json(path_join(rep_dir, "report.json"), r);
        return r;
    }
    std::string fmtname = res["format"].str();
    Rec R(src, fmtname);
    std::map<std::string, std::vector<const Json *>> by;
    for (auto &L : res["lines"].a) by[L["kind"].str()].push_back(&L);
    Json results = Json::array();
    auto add = [&](const std::string &service, const std::string &kind, const std::string &path, const std::string &note = "") {
        Json r = Json::object(); r["service"] = service; r["kind"] = kind; r["path"] = path; if (!note.empty()) r["note"] = note; results.push(r);
    };
    auto rows_of = [](const std::vector<const Json *> &v) { std::set<int> s; for (auto *L : v) s.insert((*L)["row"].integer()); return std::vector<int>(s.begin(), s.end()); };
    auto sub_try = [&](const std::function<void()> &fn, const char *what) {
        try { fn(); } catch (Cancelled &) { throw; } catch (std::exception &e) { pr.log(std::string(what) + ": " + e.what()); }
    };

    if (by.count("WST PAL teletext")) {
        std::string out = path_join(rep_dir, "teletext");
        if (!exists(path_join(out, "pages.json")) || again) {
            // декодеры WST работают в сетке bt8x8 (32 × 2048, 35,47 МГц): другие чипы сначала пересчитываются в неё
            std::string vbi = src, tmp;
            std::vector<int> wrows = rows_of(by["WST PAL teletext"]);
            if (fmtname != "bt8x8-pal") {
                pr.step("Converting the teletext lines to the bt8x8 layout");
                tmp = path_join(rep_dir, "teletext_bt8x8.tmp.vbi");
                sub_try([&] { wst_to_bt8x8(R, tmp, pr); }, "conversion");
                vbi = tmp;
                std::set<int> br;
                for (int r : wrows) {
                    int tvl = R.tv.at(r);
                    for (int t : {tvl, tvl + 313}) if (R.unit == "field" || t == tvl) {
                        if (t >= 7 && t <= 22) br.insert(t - 7);
                        if (t >= 320 && t <= 335) br.insert(16 + t - 320);
                    }
                }
                wrows.assign(br.begin(), br.end());
            }
            if (exists(vbi)) {
                // прежний результат не должен подменять новый, если декодер в этот раз не справится
                if (again) { std::error_code ec; fs::remove(P(path_join(out, "pages.json")), ec); fs::remove(P(path_join(out, "project.json")), ec); }
                pr.step("Decoding WST teletext into pages");
                sub_try([&] { decode_vbi_project(vbi, out, 32, true, true, pr); }, "teletext decoder");
                if (!exists(path_join(out, "pages.json"))) {
                    pr.step("Tape recording: reading WST with the wide-channel decoder (GPU)");
                    sub_try([&] { vhs_wst_project(vbi, out, wrows, pr); }, "tape decoder");
                }
            }
            if (!tmp.empty()) {
                std::string pj = path_join(out, "project.json");
                if (exists(pj)) { Json m = load_json(pj, Json::object()); m["source"] = src; m["source_format"] = fmtname; save_json(pj, m, 1); }
                std::error_code ec; fs::remove(P(tmp), ec);
            }
        }
        if (exists(path_join(out, "pages.json"))) {
            Json meta = load_json(path_join(out, "project.json"), Json::object());
            add("WST teletext", "teletext_project", out, !meta.get_bool("tape_decoder") ? "" : meta.get_str("note").find("not readable") != std::string::npos ? "tape decoder: page numbers not readable, pages numbered in order" : "tape decoder (wide-channel, Hamming/parity constrained)");
        }
    }
    if (by.count("WST 525-line teletext")) {
        std::string out = path_join(rep_dir, "teletext525"), t34 = path_join(rep_dir, basename(stem_path(src)) + ".t34");
        if (!exists(path_join(out, "pages.json")) || again) {
            pr.step("Reading WST teletext (525 lines)");
            sub_try([&] { slice525(R, rows_of(by["WST 525-line teletext"]), Svc525::WST, t34, pr); build_project(t34, out, "", 32, "", pr); }, "WST 525");
        }
        if (exists(path_join(out, "pages.json"))) add("WST teletext (525 lines)", "teletext_project", out);
    }
    if (by.count("NABTS")) {
        pr.step("Reading NABTS");
        std::string out = path_join(rep_dir, basename(stem_path(src)) + ".t33");
        if (!exists(out) || again) sub_try([&] { slice525(R, rows_of(by["NABTS"]), Svc525::NABTS, out, pr); }, "NABTS");
        if (exists(out)) add("NABTS", "t33", out);
    }
    if (by.count("Silent Radio")) {
        pr.step("Reading Silent Radio (LED sign service)");
        const Json *best = nullptr;
        for (auto *L : by["Silent Radio"]) if (!best || L->get_num("read") > best->get_num("read")) best = L;
        std::string out = path_join(rep_dir, "silentradio");
        if (!exists(path_join(out, "index.html")) || again)
            sub_try([&] {
                auto dl = sr_read_bytes(src, pr, (*best)["row"].integer(), (*best)["parity"].is_null() ? -1 : (*best)["parity"].integer());
                sr_save(src, out, dl.first, dl.second, pr);
            }, "Silent Radio");
        if (exists(path_join(out, "index.html"))) add("Silent Radio", "html", path_join(out, "index.html"));
    }
    for (auto &kv : by) {
        if (kv.first.rfind("encrypted datacast", 0) != 0) continue;
        std::string out = path_join(rep_dir, "datacast");
        if (!exists(path_join(out, "index.html")) || again) {
            std::vector<std::pair<int, int>> ls;
            for (auto *L : kv.second) ls.push_back({(*L)["row"].integer(), (*L)["parity"].is_null() ? -1 : (*L)["parity"].integer()});
            sub_try([&] { datacast_export(src, out, ls, pr); }, "datacast");
        }
        if (exists(path_join(out, "index.html"))) add("Encrypted datacast (packets, addresses, schedule)", "html", path_join(out, "index.html"));
        break;
    }
    if (by.count("CC (line 21)")) {
        std::map<int, std::vector<const Json *>> cc_lines;                      // строка ТВ -> её поля
        for (auto *L : by["CC (line 21)"]) cc_lines[(*L)["tv_line"].integer()].push_back(L);
        for (auto &cl : cc_lines) {
            int tvl = cl.first;
            bool all_bad = true;
            struct Res { std::string tag, p, note, text; XdsInfo X; };
            std::vector<Res> rs;
            for (auto *L : cl.second) {
                int par = (*L)["parity"].is_null() ? -1 : (*L)["parity"].integer();
                pr.step(fmt("Reading CC captions, line %d", tvl));
                std::vector<std::array<int, 3>> pairs;
                auto t = cc_text(R, (*L)["row"].integer(), par, &pairs);
                Res r; r.tag = par < 0 ? "" : std::string(1, "AB"[par]);
                r.p = path_join(rep_dir, fmt("cc_line%d%s.txt", tvl, r.tag.c_str()));
                r.text = t.first;
                int fields = par >= 0 ? R.n / 2 : R.n;
                r.note = fmt("%d intact byte pairs", t.second);
                if (t.second >= fields / 2) all_bad = false;
                else r.note += " \xE2\x80\x94 mostly not caption text (other data in the caption format)";
                r.X = xds_decode(pairs);
                rs.push_back(r);
            }
            // не подписи: может быть StarSight (пакеты «2C 00 1F ?? 1C»)
            if (all_bad) {
                pr.step(fmt("Looking for programme-guide data on line %d", tvl));
                Json ss = starsight_extract(R, cl.second, pr);
                if (ss["packets"].size() >= 3) {
                    ss["source"] = basename(src); ss["line"] = (double)tvl;
                    ss["rate"] = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1); ss["units"] = (double)R.n; ss["unit"] = R.unit;
                    std::string jp = path_join(rep_dir, fmt("starsight_line%d.json", tvl));
                    save_json(jp, ss);
                    // текстовый отчёт: пакеты по порядку
                    std::string txt = fmt("StarSight programme guide data on line %d of %s\n", tvl, basename(src).c_str());
                    txt += "Caption-format line, 2 bytes per field without parity. Packet: 2C 00 1F <type> 1C ..., byte 9 = packet number.\n";
                    txt += "The guide itself is compressed and encrypted; listed here is the transport.\n\n";
                    for (auto &q : ss["packets"].a) txt += fmt("%7.2f s  field %s  #%02X  type %02X  %3zu bytes  ", q[0].num() / ss["rate"].num(), q[1].str().c_str(), q[2].integer(), q[3].integer(), q[4].str().size() / 2) + q[4].str() + "\n";
                    write_text(path_join(rep_dir, fmt("starsight_line%d.txt", tvl)), txt);
                    add(fmt("StarSight programme guide (line %d)", tvl), "starsight", jp, fmt("%zu packets", ss["packets"].size()));
                    pr.log(fmt("StarSight on line %d: %zu packets", tvl, ss["packets"].size()));
                    continue;
                }
            }
            for (auto &r : rs) {
                write_text(r.p, r.text);
                if (r.X.packets >= 3) {
                    std::string xp = path_join(rep_dir, fmt("xds_line%d%s.txt", tvl, r.tag.c_str()));
                    write_text(xp, r.X.text);
                    Json xj = Json::object();
                    xj["source"] = basename(src); xj["line"] = (double)tvl; xj["field"] = r.tag;
                    xj["rate"] = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1); xj["units"] = (double)R.n;
                    xj["events"] = r.X.events; xj["top"] = r.X.top; xj["summary"] = r.X.summary; xj["report"] = xp;
                    if (r.X.tz >= 0) { xj["tz"] = (double)r.X.tz; xj["dst"] = r.X.dst; }
                    save_json(stem_path(xp) + ".json", xj);
                    add(fmt("XDS station / time data (line %d)", tvl), "text", xp, r.X.summary.empty() ? fmt("%d packets", r.X.packets) : r.X.summary);
                    pr.log("XDS: " + r.X.summary);
                    if (r.text.find_first_not_of(" \n") == std::string::npos) continue;   // подписей нет — только XDS
                }
                add(fmt("CC captions (line %d)", tvl), "text", r.p, r.note);
            }
        }
    }
    for (auto &kv : by) {
        bool amol = kv.first.rfind("AMOL", 0) == 0, vitc = kv.first == "VITC timecode";
        if (!amol && !vitc) continue;
        for (auto *L : kv.second) {
            int par = (*L)["parity"].is_null() ? -1 : (*L)["parity"].integer(), tvl = (*L)["tv_line"].integer();
            pr.step(fmt("Reading %s, line %d", amol ? "AMOL" : "VITC", tvl));
            std::string p = path_join(rep_dir, fmt("%s_line%d%s.txt", amol ? "amol" : "vitc", tvl, par < 0 ? "" : std::string(1, "AB"[par]).c_str()));
            int n = amol ? amol_report(R, (*L)["row"].integer(), par, p, pr) : vitc_report(R, (*L)["row"].integer(), par, p, pr);
            add(fmt("%s (line %d)", amol ? "AMOL programme ID and time" : "VITC timecode", tvl), "text", p, fmt("%d %ss read", n, R.unit.c_str()));
        }
    }
    pr.step("Measuring test signals (VITS)");
    Json vt = vits_analyse(src, res, pr);
    auto vt_text = vits_response_text(vt);
    std::vector<std::string> lines;
    lines.push_back("Recording: " + src);
    lines.push_back(fmt("Format: %s (%s) \xE2\x80\x94 %d %ss", fmtname.c_str(), vbi_format(fmtname) ? vbi_format(fmtname)->label : "?", res["units"].integer(), res["unit"].str().c_str()));
    lines.push_back(""); lines.push_back("What each line carries:");
    for (auto &L : res["lines"].a) {
        if (L["kind"].str() == "empty") continue;
        std::string par = L["parity"].is_null() ? "" : std::string(" field ") + "AB"[L["parity"].integer()];
        lines.push_back(fmt("  line %3d%-9s %s", L["tv_line"].integer(), par.c_str(), (L.has("label") ? L["label"].str() : L["kind"].str()).c_str()) +
                        (L.has("read") && !L["read"].is_null() ? fmt(" (read %.0f%%)", L["read"].num() * 100) : ""));
    }
    if (!vt_text.empty()) { lines.push_back(""); lines.insert(lines.end(), vt_text.begin(), vt_text.end()); }
    lines.push_back("");
    lines.push_back(results.size() ? "Results:" : "Nothing that the program can decode was found.");
    for (auto &r : results.a) lines.push_back("  " + r["service"].str() + ": " + r["path"].str() + (r.has("note") ? " \xE2\x80\x94 " + r["note"].str() : ""));
    std::string txt; for (auto &l : lines) txt += l + "\n";
    write_text(path_join(rep_dir, "report.txt"), txt);
    Json rep = Json::object();
    rep["file"] = orig; rep["format"] = fmtname; if (orig != src) rep["unpacked"] = src; rep["probe"] = res; rep["results"] = results; rep["vits"] = vt;
    Json vtj = Json::array(); for (auto &l : vt_text) vtj.push(l); rep["vits_text"] = vtj;
    save_json(path_join(rep_dir, "report.json"), rep, 1);
    pr.log(txt);
    return rep;
}
