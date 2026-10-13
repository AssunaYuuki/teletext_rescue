#include "export.h"
#include "ssjson.h"
#include "slicer.h"
#include "wavetop.h"
#include "project.h"

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

    if (!r.captions.empty()) put("captions.txt", join(r.captions));
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
        std::string rep, raw;
        for (auto& t : r.tvinform) {
            rep += join(t.report);
            rep += "\n";
            for (auto& w : t.words) {
                raw += fmt("%s,%lld,%d,%d,", clockText(w.frame / fps).c_str(), (long long)w.frame, w.lineNo, w.field);
                for (uint8_t x : w.bytes) raw += fmt("%02x", x);
                raw += "\n";
            }
        }
        put("tvinform_report.txt", rep);
        put("tvinform_lines.csv", "recording,frame,line,field,bytes\n" + raw);
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
            pr.step("WaveTop files");
            Json w = wavetop_decode_packets(r.nabtsT33, field, line, basename(r.path), path_join(dir, "wavetop"), pr);
            if (w.has("records") && w["records"].num() == 0) {
                // bundles may run across the lines: one stream in packet order
                for (size_t i = 0; i < field.size(); i++) field[i] = (int)i, line[i] = 0;
                w = wavetop_decode_packets(r.nabtsT33, field, line, basename(r.path), path_join(dir, "wavetop"), pr);
            }
            if (w.has("records")) done.push_back(fmt("wavetop/ (%d records, %d files intact)", (int)w["records"].num(), (int)w["ok"].num()));
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
                auto vs = P.full_versions(kv.second);
                if (vs.empty()) vs = kv.second.versions;
                for (size_t k = 0; k < vs.size(); k++) {
                    const Charset& t = P.table(&kv.second);
                    Over x = P.fitted(vs[k].rows, P.overlay(kv.second, &vs[k]), t);
                    RenderOpts o;
                    o.reveal = true;
                    Image im = render_teletext(vs[k].rows, t, P.table2(), &x, o);
                    png_save(path_join(png, kv.first + (vs.size() > 1 ? fmt("_%zu", k + 1) : "") + ".png"), im.scaled(im.w * 2, im.h * 2));
                }
            }
            done.push_back(fmt("teletext/ (%zu pages: output.html, html/, output.t42)", pages.size()));
        }
    }
    std::string s = "Written to " + dir + ":\n";
    for (auto& d : done) s += "  " + d + "\n";
    return s;
}
