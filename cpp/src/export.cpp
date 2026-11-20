#include "export.h"
#include <filesystem>
#include <set>
#include "ssjson.h"
#include "slicer.h"
#include "wavetop.h"
#include "project.h"
#include "pagebuild.h"

// Rows of the teletext stream that no page shows: a display packet (rows 1-24) whose text is on no page of its
// magazine - a version of a page whose header was lost, rows a service sent without a page, a row whose address was
// misread. The page builder does not guess them onto a page (guessed rows were what put pages over each other), so they
// are listed apart: in the order they first came, each text once with the copies of it that came (copies that differ
// in a few characters, as a tape reads them, count as one). A row is on a page when its text is there exactly on any
// row of the magazine's pages, or on the same row with at most 4 characters different.
static std::string unplaced_rows(const Project &P, const Pages &pages, int &listed)
{
    listed = 0;
    std::vector<Packet42> st;
    try { st = read_t42(P.stream); } catch (...) { return ""; }
    using R7 = std::array<u8, 40>;
    auto strip = [](const u8 *p) { R7 r; for (int i = 0; i < 40; i++) r[i] = p[i] & 0x7F; return r; };
    auto parityOk = [](u8 c) { return (__builtin_popcount(c) & 1) == 1; };
    std::map<int, std::vector<R7>> placed;   // magazine * 32 + row -> the rows of the pages
    std::set<std::pair<int, R7>> anyRow;     // magazine, text
    std::map<int, const Page *> pageOf;      // a page of each magazine, for its character set
    for (auto &kv : pages) {
        if (kv.first.empty() || kv.second.deleted) continue;
        int mag = (kv.first[0] - '0') % 8;
        if (kv.first[0] < '1' || kv.first[0] > '8') continue;
        pageOf.emplace(mag, &kv.second);
        for (auto &v : kv.second.versions)
            for (auto &rr : v.rows)
                if (rr.first >= 1 && rr.first <= 24) { R7 x = strip(rr.second.data()); placed[mag * 32 + rr.first].push_back(x); anyRow.insert({mag, x}); }
    }
    for (auto &kv : placed) { std::sort(kv.second.begin(), kv.second.end()); kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end()); }
    struct U { int mag, row; R7 text; Row raw; size_t first; int n; };
    std::vector<U> out;
    std::map<std::pair<int, R7>, long> seen;   // magazine * 32 + row, text -> entry of out, -1 = on a page or noise
    std::map<int, std::vector<size_t>> outOf;  // magazine * 32 + row -> entries
    long packets = 0;
    for (size_t i = 0; i < st.size(); i++) {
        const u8 *p = st[i].data();
        int a = ham::fix[p[0]], b = ham::fix[p[1]];
        if (a < 0 || b < 0) continue;
        int mag = a & 7, row = (a >> 3) | (b << 1), key = mag * 32 + row;
        if (row < 1 || row > 24) continue;
        packets++;
        R7 x = strip(p + 2);
        auto sk = std::make_pair(key, x);
        auto it = seen.find(sk);
        if (it != seen.end()) { if (it->second >= 0) out[it->second].n++; continue; }
        // noise: fewer than two words (three letters or digits in a row), or more than 8 characters failing their parity
        int words = 0, run = 0, badParity = 0;
        for (int k = 0; k < 40; k++) {
            bool letter = (x[k] >= '0' && x[k] <= '9') || (x[k] >= 'A' && x[k] != 0x7F);
            run = letter ? run + 1 : 0;
            words += run == 3;
            badParity += !parityOk(p[2 + k]);
        }
        bool onPage = words < 2 || badParity > 8 || anyRow.count({mag, x});
        if (!onPage)
            for (auto &y : placed[key]) {
                int d = 0;
                for (int k = 0; k < 40 && d <= 4; k++) d += x[k] != y[k];
                if (d <= 4) { onPage = true; break; }
            }
        if (onPage) { seen[sk] = -1; continue; }
        long into = -1;   // another copy of a row already listed
        for (size_t e : outOf[key]) {
            int d = 0;
            for (int k = 0; k < 40 && d <= 4; k++) d += x[k] != out[e].text[k];
            if (d <= 4) { into = (long)e; break; }
        }
        if (into >= 0) { out[into].n++; seen[sk] = into; continue; }
        if (out.size() >= 50000) continue;
        U u{mag, row, x, Row{}, i, 1};
        std::copy(p + 2, p + 42, u.raw.begin());
        seen[sk] = (long)out.size();
        outOf[key].push_back(out.size());
        out.push_back(u);
    }
    if (out.empty()) return "";
    std::string s = fmt("Rows of the teletext stream that no page shows: %zu different rows (%ld display packets in the stream).\n"
                        "Versions of pages whose header was lost, rows a service sent without a page, rows with a misread address.\n"
                        "In the order they first came; xN = copies received.\n\n", out.size(), packets);
    for (auto &u : out) {
        auto pg = pageOf.find(u.mag);
        const Charset &t = P.table(pg == pageOf.end() ? nullptr : pg->second);
        std::string text = P.row_text(u.raw, nullptr, u.row, t);
        s += fmt("packet %7zu  magazine %d row %2d  x%-4d| ", u.first, u.mag ? u.mag : 8, u.row, u.n) + text + "\n";
    }
    listed = (int)out.size();
    return s;
}

static std::string join(const std::vector<std::string>& v)
{
    std::string s;
    for (auto& l : v) s += l + "\n";
    return s;
}

static std::string clockText(double sec)
{
    int t = (int)sec;
    return fmt("%02d:%02d:%02d.%d", t / 3600, t / 60 % 60, t % 60, (int)((sec - t) * 10));
}

// Одна страница = одна настоящая подстраница. У страницы свой подкод — это она и есть; несколько копий
// во времени голосуются построчно (908 → один файл, а не двенадцать). Страница, которая крутит разные
// тексты под одним подкодом (0000), разбирается построителем проекта по содержимому.
static std::vector<Version> subpage_versions(const Project& P, const Page& pg)
{
    // the pages of a rotation the builder told apart under one subcode (Version::rot: 1tv.vbi 101 0000, three texts in
    // its right column) stay apart: voted together they made one page of rows of all three
    std::map<std::string, std::vector<const Version*>> by;
    std::vector<std::string> order, subOf;
    std::set<std::string> subs;
    for (auto& v : pg.versions) {
        std::string key = v.rot >= 0 ? v.s + "#" + std::to_string(v.rot) : v.s;
        if (!by.count(key)) { order.push_back(key); subOf.push_back(v.s); }
        by[key].push_back(&v);
        subs.insert(v.s);
    }
    if (subs.size() <= 1) return P.full_versions(pg);
    std::vector<Version> out;
    for (size_t oi = 0; oi < order.size(); oi++) {
        auto& V = by[order[oi]];
        const std::string& s = subOf[oi];
        const Version* last = V[0];
        for (auto* x : V) if (x->t > last->t) last = x;
        Version r;
        r.s = s;
        r.rot = last->rot;
        r.t = last->t;
        r.n = 0;
        r.full = true;
        for (auto* x : V) r.n += x->n ? x->n : 1;
        std::set<int> rows;
        for (auto* x : V) for (auto& rb : x->rows) rows.insert(rb.first);
        for (int rn : rows) {
            if (rn == 0) {   // шапка — из последней копии: в ней часы, номер и счётчик подстраницы
                auto it = last->rows.find(0);
                if (it != last->rows.end()) r.rows[0] = it->second;
                continue;
            }
            std::map<std::string, std::pair<int, double>> vote;   // строка -> (сколько копий, когда последний раз)
            for (auto* x : V) {
                auto it = x->rows.find(rn);
                if (it == x->rows.end()) continue;
                auto c = x->c.find(rn);
                int w = (c != x->c.end() && c->second) ? c->second : (x->n ? x->n : 1);
                auto& e = vote[std::string((const char*)it->second.data(), 40)];
                e.first += w;
                e.second = std::max(e.second, x->t);
            }
            const std::string* best = nullptr;
            int bw = -1;
            double bt = -1;
            for (auto& e : vote)
                if (e.second.first > bw || (e.second.first == bw && e.second.second > bt)) { best = &e.first; bw = e.second.first; bt = e.second.second; }
            if (!best) continue;
            Row row;
            for (int k = 0; k < 40; k++) row[k] = k < (int)best->size() ? (u8)(*best)[k] : 0x20;
            r.rows[rn] = row;
            r.c[rn] = bw;
        }
        out.push_back(std::move(r));
    }
    return out;
}

static const char* kSignPage = R"~(<!doctype html><html><head><meta charset="utf-8"><title>Silent Radio</title>
<style>
body{background:#111;color:#ccc;font:14px sans-serif;margin:16px}
#sign{background:#000;border:10px solid #222;border-radius:6px;display:block;margin:8px 0;max-width:100%}
#list{max-height:45vh;overflow:auto;border:1px solid #333;margin-top:8px}
#list div{padding:3px 6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#list div.on{background:#523;color:#fff}
button,select{background:#222;color:#ddd;border:1px solid #444;padding:3px 10px}
#cap{color:#fa4;min-height:1.3em}
</style></head><body>
<div>Silent Radio &mdash; <span id="src"></span> &middot; zone <select id="zone"></select>
<button id="play">Pause</button> <button id="prev">&lt;</button> <button id="next">&gt;</button>
<label><input type="checkbox" id="loop" checked> loop</label></div>
<canvas id="sign"></canvas><div id="cap"></div><div id="list"></div>
<script>var DATA=/*DATA*/;</script><script>/*PLAYER*/</script></body></html>)~";

std::string exportResult(const DecodeResult& r, const std::string& dir, Progress& pr)
{
    make_dirs(dir);
    const double fps = r.geom.pal ? 25.0 : 30000.0 / 1001.0;
    std::vector<std::string> done;
    auto put = [&](const std::string& name, const std::string& text) {
        write_text(path_join(dir, name), text);
        done.push_back(name);
    };

    pr.step("report");
    std::string rep = summarize(r);
    for (size_t p; (p = rep.find('\r')) != std::string::npos;) rep.erase(p, 1);
    put("report.txt", rep);

    if (!r.captions.empty()) put("captions.txt", join(captionsTimed(r)));
    {   // one subtitle file per caption service, timed by the tape's VITC or the place in the recording
        std::set<std::string> svcs;
        for (auto& c : r.captionCues) svcs.insert(c.svc);
        for (auto& v : svcs) put("captions_" + v + ".srt", captionsSrt(r, v));
    }
    if (!r.vitcReport.empty()) put("vitc.txt", join(r.vitcReport));
    if (!r.vpsReport.empty()) put("vps.txt", join(r.vpsReport));
    if (!r.xds.empty()) put("xds.txt", join(r.xds));
    if (!r.silentRadio.empty()) {
        std::string s;
        for (auto& m : r.silentRadio) s += "[" + clockText(m.frame / fps) + "] " + (m.zone.empty() ? "" : "zone " + m.zone + ": ") + m.text + "\n";
        put("silent_radio.txt", s);
        // the sign player of the Teletext Rescue program: packets.json + index.html
        Json zones = Json::object();
        for (auto& m : r.silentRadio) {
            Json it = Json::object();
            it["seq"] = m.seq >= 0 ? fmt("%04X", m.seq) : std::string("-");
            it["title"] = m.text.size() > 60 ? m.text.substr(0, 60) : m.text;
            Json tx = Json::array();
            for (auto& t : m.texts) tx.push(t);
            it["text"] = tx;
            Json frs = Json::array();
            for (auto& f : m.frames) {
                Json fj = Json::object();
                if (!f.scroll) {
                    Json c = Json::array();
                    for (int v : f.cols) c.push(v);
                    fj["f"] = c;
                    fj["t"] = f.hold;
                    if (f.wipe) fj["fx"] = "wipe";
                } else {
                    Json ls = Json::array();
                    for (auto& l : f.lines) {
                        Json lj = Json::object(), c = Json::array();
                        for (int v : l.first) c.push(v);
                        lj["cols"] = c;
                        lj["row"] = l.second;
                        ls.push(lj);
                    }
                    fj["scroll2"] = ls;
                    fj["speed"] = f.speed;
                }
                fj["w"] = f.what;
                frs.push(fj);
            }
            it["frames"] = frs;
            it["copies"] = m.copies;
            std::string z = m.zone.empty() ? "text" : m.zone;
            if (!zones.has(z)) zones[z] = Json::array();
            zones[z].push(it);
        }
        int srLine = 0;
        for (auto& li : r.lines) if (li.kind.rfind("Silent Radio", 0) == 0) srLine = li.lineNo;
        Json d = Json::object();
        d["source"] = basename(r.path);
        d["line"] = srLine;
        d["zones"] = zones;
        std::string js = d.dump();
        std::string sr = path_join(dir, "silent_radio");
        make_dirs(sr);
        write_text(path_join(sr, "packets.json"), js);
        std::string html = replace_all(kSignPage, "/*DATA*/", js);
        html = replace_all(html, "/*PLAYER*/", resource_text("SILENT_RADIO_PLAYER"));
        write_text(path_join(sr, "index.html"), html);
        done.push_back("silent_radio/index.html");
    }
    if (!r.amol.empty()) {
        std::string s = "recording,frame,line,field,sid,source,month,day,hour,minute,second,ampm,parity\n";
        for (auto& a : r.amol)
            for (auto& t : a.stamps)
                s += fmt("%s,%lld,%d,%d,%d,%s,%d,%d,%d,%d,%d,%s,%s\n", clockText(t.frame / fps).c_str(), (long long)t.frame, a.lineNo,
                         a.field, t.sid, amolSourceName(t.sid).c_str(), t.month, t.day, t.hour, t.minute, t.second, t.pm ? "PM" : "AM",
                         t.parityOk ? "ok" : "error");
        put("amol.csv", s);
        std::string w;
        for (auto& a : r.amol) w += join(a.report);
        put("amol_report.txt", w);
    }
    if (!r.gemstar.empty()) {
        std::string s = "recording,frame,line,field,framing,bytes,ok\n", w;
        for (auto& g : r.gemstar) {
            for (auto& x : g.words)
                s += fmt("%s,%lld,%d,%d,%s,%s,%s\n", clockText(x.frame / fps).c_str(), (long long)x.frame, g.lineNo, g.field,
                         x.x2 ? "2x" : "1x", x.hex().c_str(), x.ok ? "ok" : "parity");
            w += join(g.report);
            for (auto& m : g.messages)
                w += "[" + clockText(m.frame / fps) + "] " + (m.x2 ? "2x " : "1x ") + m.hex() + (m.copies > 1 ? fmt("  (x%d)", m.copies) : "") + "\n";
            w += "\n";
        }
        put("gemstar_words.csv", s);
        std::string sh = "recording,channel,date,start_utc,minutes,theme,misc,number,title,line1,line2\n";
        auto q = [](std::string t) {
            for (auto& c : t) if (c == '\n') c = ' ';
            std::string o = "\"";
            for (char c : t) o += c == '"' ? std::string("\"\"") : std::string(1, c);
            return o + "\"";
        };
        size_t nshows = 0;
        for (auto& g : r.gemstar)
            for (auto& x : g.shows) {
                nshows++;
                sh += fmt("%s,%d,%04d-%02d-%02d,%d:%02d,%d,%02X,%02X,%lld,", clockText(x.frame / fps).c_str(), x.channel, x.year, x.month, x.day,
                          x.start / 60, x.start % 60, x.duration, x.theme, x.misc, (long long)x.code) +
                      q(x.title) + "," + q(x.line1) + "," + q(x.line2) + "\n";
            }
        if (nshows) put("gemstar_guide.csv", sh);
        put("gemstar_report.txt", w);
    }
    if (!r.tvinform.empty()) {
        std::string rep, raw, serial, text;
        for (auto& t : r.tvinform) {
            rep += join(t.report);
            rep += "\n";
            serial += tviSerialText(t, fps);
            std::string tx = tviText(t);
            if (!tx.empty() && !t.words.empty())
                text += fmt("===== line %d field %d =====\n", t.words[0].lineNo, t.words[0].field) + tx + "\n";
            for (auto& w : t.words) {
                raw += fmt("%s,%lld,%d,%d,", clockText(w.frame / fps).c_str(), (long long)w.frame, w.lineNo, w.field);
                for (uint8_t x : w.bytes) raw += fmt("%02x", x);
                raw += "\n";
            }
        }
        put("tvinform_report.txt", rep);
        put("tvinform_lines.csv", "recording,frame,line,field,bytes\n" + raw);
        if (!serial.empty()) put("tvinform_serial.txt", "recording    frame    kind    hdr  chars (levels inverted; async = KOI-7, hamming = KOI8-R or CP866)\n" + serial);
        if (!text.empty()) put("tvinform_text.txt", text);
    }
    if (r.db.stats.packets) {
        pr.step("StarSight database");
        std::string err;
        if (ssExportJson(path_join(dir, "starsight.json"), r.db, &r, err)) done.push_back("starsight.json");
        else done.push_back("starsight.json: " + err);
        if (!r.db.log.empty()) put("starsight_log.txt", join(r.db.log));
    }
    if (!r.fastPackets.empty()) {
        put("data_57.txt", join(r.fastReport) + "\n=== recovered text ===\n" + join(r.fastText));
        Bytes x;
        for (auto& p : r.fastPackets)
            if (p.kind == 'X' && p.bytes.size() >= 37) x.insert(x.end(), p.bytes.begin() + 3, p.bytes.begin() + 37);
        if (!x.empty()) { write_file(path_join(dir, "framing2d.bin"), x); done.push_back("framing2d.bin"); }
    }
    if (!r.nabtsT33.empty()) {
        write_file(path_join(dir, "nabts.t33"), r.nabtsT33);
        done.push_back("nabts.t33");
        // WaveTop (WavePhore web-cache datacast, 1998-2000): bundles of 18 packets
        {
            std::vector<int> field = r.nabtsField, line = r.nabtsLine;
            if (field.size() < r.nabtsT33.size() / 33) {   // a .t33 opened as it is: no field of each packet, their order
                field.resize(r.nabtsT33.size() / 33); line.assign(field.size(), 0);
                for (size_t i = 0; i < field.size(); i++) field[i] = (int)i;
            }
            pr.step("WaveTop files");
            Json w = wavetop_decode_packets(r.nabtsT33, field, line, basename(r.path), path_join(dir, "wavetop"), pr);
            if (w.has("records") && w["records"].num() == 0) {
                // bundles may run across the lines: one stream in packet order
                for (size_t i = 0; i < field.size(); i++) field[i] = (int)i, line[i] = 0;
                w = wavetop_decode_packets(r.nabtsT33, field, line, basename(r.path), path_join(dir, "wavetop"), pr);
            }
            if (w.has("records")) done.push_back(fmt("wavetop/ (%d records, %d files intact)", (int)w["records"].num(), (int)w["ok"].num()));
            if (w.has("datacasts")) done.push_back(fmt("wavetop/datacast_*.bin (%d NABTS FEC datacast channels, wavetop.txt)", (int)w["datacasts"].num()));
        }
        int pages = 0;
        for (auto& rec : r.nabtsRecords) pages += rec.page != nullptr;
        if (pages) {
            pr.step("NABTS pages (PNG)");
            std::string out = path_join(dir, "nabts_pages"), txt;
            make_dirs(out);
            int n = 0;
            for (auto& rec : r.nabtsRecords) {
                txt += "=== " + nabts::record_label(rec) + fmt("  type %d  received %d  ", rec.type, rec.seen) + nabts::flags_text(rec) + " " + rec.purpose + "\n";
                if (rec.page) {
                    png_save(path_join(out, nabts::record_name(rec) + ".png"), nabts::render_page(*rec.page).scaled(1536, 1152));
                    txt += rec.text + "\n";
                    pr.progress(++n, pages, "PNG");
                }
                txt += "\n";
            }
            write_text(path_join(out, "records.txt"), txt);
            done.push_back(fmt("nabts_pages/ (%d PNG)", pages));
            pr.step("NABTS pages (HTML)");
            int h = nabts_html_export(r.nabtsRecords, path_join(dir, "nabts_html"), 256, 200, pr);
            done.push_back(fmt("nabts_html/ (%d pages)", h));
        }
    }
    if (!r.jtes.pages.empty()) {
        pr.step("Japanese teletext pages");
        int n = jtesExport(r.jtes, path_join(dir, "jtes_pages"));
        done.push_back(fmt("jtes_pages/ (%d pages: PNG, text, WAV for the tunes)", n));
    }
    if (!r.teletext.project.empty()) {
        pr.step("teletext pages");
        Project P;
        if (P.open(r.teletext.project)) {
            std::set<std::string> edited;
            std::string src;
            Pages pages = P.load_state(edited, src);
            P.outdir = path_join(dir, "teletext");
            make_dirs(P.outdir);
            P.export_all(pages, false);
            // and each page as a picture (the complete page, assembled from its versions)
            std::string png = path_join(P.outdir, "png");
            make_dirs(png);
            for (auto& kv : pages) {
                auto vs = subpage_versions(P, kv.second);
                if (vs.empty()) vs = kv.second.versions;
                for (size_t k = 0; k < vs.size(); k++) {
                    const Charset& t = P.table(&kv.second);
                    Over x = P.fitted(vs[k].rows, P.overlay(kv.second, &vs[k]), t);
                    RenderOpts o;
                    o.reveal = true;
                    Image im = render_teletext(vs[k].rows, t, P.table2(&kv.second), &x, o);
                    png_save(path_join(png, kv.first + (vs.size() > 1 ? fmt("_%zu", k + 1) : "") + ".png"), im.scaled(im.w * 2, im.h * 2));
                }
            }
            // каждая страница и подстраница — чистым текстом: pages/ (то же имя, что у PNG) и всё вместе в all.txt
            pr.step("pages as text");
            std::string txtdir = path_join(dir, "pages");
            make_dirs(txtdir);
            {   // старые .txt прошлого прохода не должны оставаться рядом с новыми
                std::error_code ec;
                for (std::filesystem::directory_iterator it(std::filesystem::u8path(txtdir), ec), end; !ec && it != end; it.increment(ec)) {
                    const std::string f = it->path().filename().u8string();
                    if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) std::filesystem::remove(it->path(), ec);
                }
            }
            int ntxt = 0;
            std::string all;
            for (auto& kv : pages) {
                if (kv.second.deleted) continue;
                auto vs = subpage_versions(P, kv.second);
                if (vs.empty()) vs = kv.second.versions;
                const Charset& t = P.table(&kv.second);
                for (size_t k = 0; k < vs.size(); k++) {
                    std::string txt;
                    int rows = 0;
                    for (int r = 0; r < 25; r++) {
                        auto ri = vs[k].rows.find(r);
                        if (ri == vs[k].rows.end()) continue;
                        std::string line = P.row_text(ri->second, P.overlay(kv.second, &vs[k]), r, t);
                        if (line.empty()) continue;
                        rows++;
                        txt += line + "\n";
                    }
                    if (rows < 2) continue;   // строки-картинки без текста не пишем, а «страница не передаётся» (2 строки) — пишем
                    std::string name = kv.first + (vs.size() > 1 ? fmt("_%zu", k + 1) : "");
                    write_text(path_join(txtdir, name + ".txt"), txt);
                    all += "===== " + name + " =====\n" + txt + "\n";
                    ntxt++;
                }
            }
            if (!all.empty()) write_text(path_join(txtdir, "all.txt"), all);
            done.push_back(fmt("pages/ (%d pages as text, all.txt)", ntxt));
            {
                pr.step("rows no page shows");
                int nu = 0;
                std::string u = unplaced_rows(P, pages, nu);
                std::string up = path_join(dir, "unplaced_rows.txt");
                if (!u.empty()) { write_text(up, u); done.push_back(fmt("unplaced_rows.txt (%d rows no page shows)", nu)); }
                else {   // a list from an earlier pass is not left beside pages that have none
                    std::error_code ec;
                    std::filesystem::remove(std::filesystem::u8path(up), ec);
                }
            }
            // один собранный экземпляр каждой подстраницы: teletext/clean.t42 — открывается программой как поток
            try { P.squash(pages); done.push_back("teletext/clean.t42 (one assembled copy of each subpage)"); }
            catch (std::exception&) {}
            done.push_back(fmt("teletext/ (%zu pages: output.html, html/, output.t42, png/)", pages.size()));
        }
    }
    std::string s = "Written to " + dir + ":\n";
    for (auto& d : done) s += "  " + d + "\n";
    return s;
}
