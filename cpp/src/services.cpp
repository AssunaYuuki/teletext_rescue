// What a decoded recording holds: the services found, the full report (shared by both front ends).
#include "services.h"
#include "silentradio.h"
#include "finds.h"
#include "ui/util.h"
#include <cmath>
#include <map>
#include <set>

namespace ui {

std::string rec_time(double s)
{
    if (s < 0) s = 0;
    return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60));
}

double frame_rate(const DecodeResult &r) { return r.geom.pal ? 25.0 : 30000.0 / 1001.0; }

std::string file_name(const DecodeResult &r) { return basename(r.path); }

std::vector<int> led_glyph(char32_t ch) { return srGlyph(ch); }

// Gemstar lines: report, then every message with its time
std::string tvinform_text(const DecodeResult &r)
{
    std::string s;
    for (auto &t : r.tvinform) { for (auto &l : t.report) s += l + "\n"; s += "\n"; }
    return s;
}

std::string gemstar_text(const DecodeResult &r)
{
    std::string s;
    for (auto &g : r.gemstar) {
        for (auto &l : g.report) s += l + "\n";
        for (auto &m : g.messages)
            s += "[" + rec_time(m.frame / frame_rate(r)) + "] " + (m.x2 ? "2x " : "1x ") + m.hex() + (m.copies > 1 ? fmt("  (x%d)", m.copies) : "") + "\n";
        s += "\n";
    }
    return s;
}

std::string full_report(const DecodeResult &r)
{
    auto join = [](const std::vector<std::string> &v) { std::string s; for (auto &l : v) s += l + "\n"; return s; };
    std::string s = replace_all(summarize(r), "\r\n", "\n");
    if (!r.vitcReport.empty()) s += "\n=== VITC time code ===\n" + join(r.vitcReport);
    s += "\n=== Closed captions ===\n" + join(captionsTimed(r));
    s += "\n=== XDS ===\n" + join(r.xds);
    s += "\n=== Silent Radio ===\n";
    for (auto &m : r.silentRadio) s += "[" + rec_time(m.frame / frame_rate(r)) + "] " + (m.zone.empty() ? "" : "zone " + m.zone + ": ") + m.text + "\n";
    s += "\n=== AMOL (Nielsen) ===\n";
    for (auto &a : r.amol)
        for (auto &t : a.stamps) s += "[" + rec_time(t.frame / frame_rate(r)) + "] " + t.text() + (t.parityOk ? "" : "  (parity error)") + "\n";
    s += "\n=== Gemstar ===\n" + gemstar_text(r);
    s += "\n=== TV-Inform ===\n" + tvinform_text(r);
    s += "\n=== 5.7 Mbit/s data (NABTS / teletext / framing 2D) ===\n" + join(r.fastText);
    s += "\n=== StarSight command log ===\n" + join(r.db.log);
    return s;
}

// what each detected line carries, grouped by service
std::vector<Service> services_of(const DecodeResult &r)
{
    std::vector<Service> out;
    std::map<std::string, std::map<int, std::set<int>>> where;   // kind -> line -> fields
    std::vector<std::string> order;
    for (auto &li : r.lines) {
        if (!where.count(li.kind)) order.push_back(li.kind);
        where[li.kind][li.lineNo].insert(li.field);
    }
    auto where_text = [&](const std::string &k) {
        std::string s;
        for (auto &kv : where[k]) {
            if (!s.empty()) s += ", ";
            s += fmt("line %d", kv.first) + (kv.second.size() == 2 ? " (both fields)" : fmt(" field %d", *kv.second.begin()));
        }
        return s;
    };
    for (auto &k : order) {
        Service s;
        s.where = where_text(k);
        if (k == "StarSight") {
            const SsStats &st = r.db.stats;
            int good = st.crcOk + st.fixedSingle + st.fixedChase + st.fixedCombine;
            s.name = "StarSight programme guide"; s.kind = "StarSight"; s.window = ServiceWindow::StarSight;
            s.note = fmt("%d packets, %d good; %zu channels, %zu show lists", st.packets, good, r.db.channelIds().size(), r.db.showLists.size());
        } else if (k == "Captions") {
            size_t n = 0; for (auto &l : r.captions) n += !l.empty() && l.rfind("---", 0) != 0;
            s.name = "CC captions"; s.kind = "CC captions"; s.window = ServiceWindow::Captions;
            s.note = fmt("%zu lines of text", n);
        } else if (k.rfind("XDS", 0) == 0) {
            size_t n = 0; for (auto &l : r.xds) n += l.rfind("XDS", 0) == 0;
            s.name = "XDS station data"; s.kind = "CC XDS"; s.window = ServiceWindow::Xds;
            s.note = fmt("%zu packets", n);
        } else if (k.rfind("Silent Radio", 0) == 0) {
            s.name = "Silent Radio"; s.kind = "Silent Radio"; s.window = ServiceWindow::Sign;
            s.note = r.silentRadio.empty() ? "idle carrier, no messages" : fmt("%zu messages", r.silentRadio.size());
        } else if (k == "Teletext stream") {
            s.name = "Teletext pages"; s.kind = "WST teletext"; s.window = ServiceWindow::Teletext;
            s.where = "packet stream";
            s.note = fmt("%d pages, %d versions", r.teletext.pages, r.teletext.subpages) + (r.teletext.service.empty() ? "" : " \xC2\xB7 " + r.teletext.service);
        } else if (k == "DVB teletext") {
            s.name = "DVB teletext pages"; s.kind = "WST teletext"; s.window = ServiceWindow::Teletext;
            s.where = "transport stream";
            s.note = fmt("%d pages, %d versions", r.teletext.pages, r.teletext.subpages) + (r.teletext.service.empty() ? "" : " \xC2\xB7 " + r.teletext.service);
        } else if (k.rfind("AMOL", 0) == 0) {
            size_t n = 0; for (auto &a : r.amol) n += a.stamps.size();
            s.name = "AMOL (Nielsen)"; s.kind = "AMOL"; s.window = ServiceWindow::Amol;
            s.note = n ? fmt("%zu seconds of station clock", n) : "frame counter and data words";
        } else if (k == "VITC time code") {
            s.name = "VITC time code"; s.kind = "VITC"; s.window = ServiceWindow::Vitc;
            s.note = r.vitc.empty() ? "no readable code" : r.vitc.front().text() + " .. " + r.vitc.back().text();
        } else if (k == "TV-Inform") {
            size_t pg = 0; for (auto &t : r.tvinform) pg += t.pages.size();
            s.name = "TV-Inform (\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC)"; s.kind = "TV-Inform"; s.window = ServiceWindow::Tvinform;
            s.note = fmt("%zu carousel pages, GOST R 50822-95", pg);
        } else if (k == "Japanese teletext (JTES)") {
            std::set<int> progs;
            for (auto& pg : r.jtes.pages) progs.insert(pg.programme);
            s.name = "Japanese teletext (\xE6\x96\x87\xE5\xAD\x97\xE5\xA4\x9A\xE9\x87\x8D\xE6\x94\xBE\xE9\x80\x81)"; s.kind = "teletext"; s.window = ServiceWindow::Jtes;
            s.note = fmt("%zu pages in %zu programmes", r.jtes.pages.size(), progs.size());
        } else if (k == "Gemstar") {
            size_t n = 0; for (auto &g : r.gemstar) n += g.messages.size();
            s.name = "Gemstar (VCR Plus+ / Guide Plus+)"; s.kind = "Gemstar"; s.window = ServiceWindow::Gemstar;
            s.note = fmt("%zu distinct messages, 2x and 1x", n);
        } else if (k == "NABTS" || k == "WST teletext" || k.rfind("Encrypted datacast", 0) == 0) {
            char want = k == "NABTS" ? 'N' : k == "WST teletext" ? 'W' : 'X';
            size_t n = 0; for (auto &p : r.fastPackets) n += p.kind == want;
            s.dataKind = want == 'N' ? "NABTS" : want == 'W' ? "WST teletext" : "framing 2D";
            s.name = want == 'X' ? "Encrypted datacast (framing 2D)" : k; s.kind = want == 'N' ? "NABTS" : want == 'W' ? "WST teletext" : "encrypted datacast";
            s.window = ServiceWindow::Data;
            s.note = fmt("%zu packets at 5.73 Mbit/s", n);
            if (want == 'W' && r.teletext.pages > 0) {
                s.name = "WST teletext pages"; s.window = ServiceWindow::Teletext;
                s.note = fmt("%d pages, %d versions", r.teletext.pages, r.teletext.subpages);
            }
            if (want == 'N' && !r.nabtsRecords.empty()) {
                int pages = 0; for (auto &x : r.nabtsRecords) pages += x.page != nullptr;
                s.name = "NABTS teletext (NAPLPS)"; s.window = ServiceWindow::Nabts;
                s.note = fmt("%zu packets, %zu records, %d pages", n, r.nabtsRecords.size(), pages);
            }
        } else if (k == "WST PAL teletext" && !r.teletext.project.empty()) {
            s.name = "Teletext pages"; s.kind = "WST teletext"; s.window = ServiceWindow::Teletext;
            s.note = "625-line WST";
            for (auto &x : r.legacy.results) if (x.kind == "teletext_project" && !x.note.empty()) s.note += " \xC2\xB7 " + x.note;
        } else {
            continue;   // unreadable 8-bit data: shown in the line map and the report only
        }
        out.push_back(s);
    }
    // files and texts of their own (ТВ-информ files and documents, «Бизнес-Текст» catalogue): first
    {
        auto fs = findsOf(r);
        if (!fs.empty()) {
            size_t files = 0, got = 0, texts = 0;
            for (auto &f : fs) { files += f.kind == Find::File || f.kind == Find::Announced; got += f.kind == Find::File; texts += f.kind == Find::Text; }
            Service s;
            s.name = "Found files and texts"; s.kind = "files"; s.window = ServiceWindow::Finds;
            s.where = fs.front().service;
            s.note = fmt("%zu files (%zu received), %zu texts: ", files, got, texts) + fs.front().name;
            out.insert(out.begin(), s);
        }
    }
    // everything else the Teletext Rescue engine saved in the results folder (VPS, Syster, TOP, ...)
    for (auto &x : r.legacy.results) {
        if (x.kind == "teletext_project") continue;
        Service s;
        s.name = x.service; s.kind = x.service; s.note = x.note.empty() ? basename(x.path) : x.note;
        s.where = "results folder"; s.window = ServiceWindow::LegacyFile; s.dataKind = x.path;
        out.push_back(s);
    }
    return out;
}

}
