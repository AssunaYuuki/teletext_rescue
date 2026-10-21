#include "finds.h"
#include "arjcheck.h"
#include "ttdata.h"
#include "biznestext.h"
#include "decoder.h"
#include "faxg4.h"
#include "pcxfix.h"
#include "png.h"
#include "tvinform.h"
#include "util.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace {
const char* const kTvi = "\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC";              // ТВ-информ
const char* const kBt = "\xD0\x91\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x81-\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82";   // Бизнес-Текст

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

std::string byExtension(const std::string& name)
{
    std::string e = lower(name.substr(name.find_last_of('.') == std::string::npos ? name.size() : name.find_last_of('.') + 1));
    if (e == "arj") return "ARJ archive";
    if (e == "zip") return "ZIP archive";
    if (e == "rar") return "RAR archive";
    if (e == "txt" || e == "doc" || e == "lst") return "text";
    if (e == "tif" || e == "tiff") return "TIFF image";
    if (e == "exe" || e == "com") return "DOS program";
    if (e == "gif") return "GIF image";
    if (e == "pcx") return "PCX image";
    return e.empty() ? "" : e + " file";
}

// TIFF: the compression of the first directory (3/4 = CCITT fax)
std::string tiffKind(const Bytes& d)
{
    if (d.size() < 8) return "TIFF image";
    bool le = d[0] == 'I';
    auto u16 = [&](size_t i) -> uint32_t { return i + 2 > d.size() ? 0 : le ? d[i] | d[i + 1] << 8 : d[i] << 8 | d[i + 1]; };
    auto u32 = [&](size_t i) -> uint32_t { return i + 4 > d.size() ? 0 : le ? u16(i) | u16(i + 2) << 16 : u16(i) << 16 | u16(i + 2); };
    uint32_t ifd = u32(4), w = 0, h = 0, comp = 0;
    uint32_t n = u16(ifd);
    for (uint32_t k = 0; k < n && k < 64; k++) {
        size_t e = ifd + 2 + 12 * k;
        uint32_t tag = u16(e), type = u16(e + 2), v = type == 3 ? u16(e + 8) : u32(e + 8);
        if (tag == 256) w = v;
        if (tag == 257) h = v;
        if (tag == 259) comp = v;
    }
    std::string s = comp == 4 ? "TIFF image, fax CCITT G4" : comp == 3 ? "TIFF image, fax CCITT G3" : "TIFF image";
    if (w && h) s += fmt(", %ux%u", w, h);
    return s;
}
}

std::string fileTypeOf(const std::string& path, const std::string& name)
{
    Bytes d;
    try { if (!path.empty() && exists(path)) d = read_file(path); } catch (...) {}
    if (d.size() >= 4) {
        if ((d[0] == 'I' && d[1] == 'I' && d[2] == 42) || (d[0] == 'M' && d[1] == 'M' && d[3] == 42)) {
            std::string k = tiffKind(d);
            if (k.find("fax") == std::string::npos) {   // the directory lost with the last blocks: the strips still decode
                int w = 0, h = 0;
                std::vector<uint8_t> px, t(d.begin(), d.end());
                std::string note;
                if (faxTiffDecode(t, w, h, px, note)) k = fmt("TIFF image, fax CCITT G4, %dx%d", w, h);
            }
            return k;
        }
        if (d[0] == 0x60 && d[1] == 0xEA) return "ARJ archive";
        if (d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) return "ZIP archive";
        if (d[0] == 'R' && d[1] == 'a' && d[2] == 'r' && d[3] == '!') return "RAR archive";
        if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "GIF image";
        if (d[0] == 0xFF && d[1] == 0xD8) return "JPEG image";
        if (d[0] == 'B' && d[1] == 'M') return "BMP image";
        if (d[0] == 0x0A && d[1] <= 5 && d[2] == 1) return "PCX image";
        if (d[0] == 'M' && d[1] == 'Z') return "DOS / Windows program";
        size_t text = 0, n = std::min<size_t>(d.size(), 4096);
        for (size_t i = 0; i < n; i++) text += d[i] >= 32 || d[i] == '\r' || d[i] == '\n' || d[i] == '\t';
        if (text * 100 >= n * 97) return "text";
    }
    std::string e = byExtension(name);
    return e.empty() ? "data" : e;
}

void addTviFinds(std::vector<Find>& out, const TviStream& t, const std::string& where)
{
    std::string svc = t.rate < 0 ? std::string(kTvi) + " (Norpak packets, 5.73 Mbit/s)" : std::string(kTvi) + (t.rate == 0 ? " (f\xD0\xA2" "1)" : t.rate == 1 ? " (f\xD0\xA2" "2)" : " (f\xD0\xA2" "3, 6.94 Mbit/s)");
    for (auto& f : t.fileInfo) {
        Find x;
        x.kind = Find::File;
        x.name = f.name; x.shortName = f.shortName != f.name ? f.shortName : "";
        x.type = fileTypeOf(f.path, f.name);
        x.service = svc; x.where = where;
        x.when = f.time.empty() ? "" : "written " + f.time;
        x.size = f.size; x.blocks = f.blocks; x.have = f.have;
        x.status = f.have >= f.blocks ? "complete" : fmt("%d of %d blocks", f.have, f.blocks);
        x.path = f.path;
        x.note = f.note;
        if (!f.missing.empty()) {
            std::string m;
            for (int b : f.missing) m += (m.empty() ? "" : " ") + fmt("%d", b);
            x.note += (x.note.empty() ? "" : "; ") + std::string("blocks not received: ") + m;
        }
        out.push_back(x);
    }
    // Norpak: the source beacon (who sends, when the data was last updated)
    for (auto& b : t.beacons) {
        Find x;
        x.kind = Find::Text;
        size_t q = b.find('"', 1);
        x.name = "source beacon: " + (q != std::string::npos ? b.substr(1, q - 1) : b);
        x.type = "service information, records D5 F3 C9 06";
        x.service = svc; x.where = where;
        size_t u = b.find("updated ");
        x.when = u != std::string::npos ? b.substr(u, 27) : "";
        size_t m = b.rfind("- ");
        x.status = m != std::string::npos ? b.substr(m + 2) : "";
        x.text = {b};
        x.note = "the source beacon of the ТВ-информ packet channel: source name, time of the last update, version word";
        out.push_back(x);
    }
    if (t.rate < 0 || t.words.empty()) return;
    // fТ3 (RTR): the stream itself, shown as soon as its lines are read (whole files are rare: most of the
    // service's lines are not captured), and the readable texts in it
    if (t.rate == 2) {
        Find x;
        x.kind = Find::Text;
        x.name = "file transfer stream (RTR): " + fmt("%zu lines read", t.words.size());
        x.type = "records 7E 80 80 80 80, CRC-32";
        x.service = svc; x.where = where;
        x.status = t.report.size() > 1 ? strip(t.report[1]) : "reading";
        if (!t.beacons.empty()) {
            size_t c = t.beacons[0].find("clock ");
            if (c != std::string::npos) x.when = t.beacons[0].substr(c + 6, t.beacons[0].find(',', c) - c - 6);
        }
        x.text = t.report;
        x.note = t.report.size() > 2 ? strip(t.report[2]) : "";
        out.push_back(x);
        if (!t.texts.empty()) {
            Find y;
            y.kind = Find::Text;
            std::string first = t.texts[0];
            for (auto& c : first) if (c == '\r' || c == '\n') c = ' ';
            y.name = "texts: " + strip(first).substr(0, 60) + "...";
            y.type = "text, ASCII (WMO bulletins)";
            y.service = svc; y.where = where;
            y.status = fmt("%zu pieces", t.texts.size());
            for (auto& p : t.texts) {
                std::string l;
                for (char c : p) {
                    if (c == '\r') continue;
                    if (c == '\n') { y.text.push_back(l); l.clear(); } else l += c;
                }
                if (!l.empty()) y.text.push_back(l);
                y.text.push_back("----");
            }
            for (auto& l : y.text) y.size += l.size() + 1;
            out.push_back(y);
        }
        return;
    }
    TviSummary s = tviSummarize(t);
    if (s.kind != TviSummary::Data && s.kind != TviSummary::Test) return;
    Find x;
    x.kind = s.kind == TviSummary::Data ? Find::Text : Find::Test;
    x.name = s.kind == TviSummary::Data ? "document" : "test message of the inserter";
    for (auto& l : s.text) if (s.kind == TviSummary::Data && !strip(l).empty()) { x.name = "document: " + strip(l).substr(0, 70) + "..."; break; }
    x.type = "text, " + s.charset;
    x.service = svc; x.where = where;
    x.status = fmt("%d lines with data", s.dataLines);
    x.size = 0;
    for (auto& l : s.text) x.size += l.size() + 1;
    x.text = s.text;
    x.note = s.what;
    out.push_back(x);
}

void addBtFinds(std::vector<Find>& out, const BtCatalog& c, const std::string& where, const std::string& dir,
                const FindStatus& status)
{
    std::vector<BtWritten> written;
    std::string keyNote;
    if (!dir.empty() && c.hasData()) written = c.writeFiles(dir, keyNote, status);
    addBtFinds(out, c, where, written, keyNote);
}

void addBtFinds(std::vector<Find>& out, const BtCatalog& c, const std::string& where, const std::vector<BtWritten>& written,
                const std::string& keyNote)
{
    std::string svc = std::string(kBt) + " (teletext catalogue)";
    std::map<std::string, BtWritten> got;
    for (auto& w : written) got[w.name] = w;
    for (auto& f : c.files()) {
        Find x;
        x.kind = Find::Announced;
        x.name = f.name;
        x.type = byExtension(f.name);
        x.service = svc; x.where = where + (f.page.empty() ? "" : ", page " + f.page);
        x.when = f.date;
        x.blocks = f.blocks;
        x.blockBytes = 800;
        if (f.size > 0) x.size = (uint64_t)f.size;
        x.have = 0;
        std::string s;
        for (int b : f.sent) s += (s.empty() ? "" : " ") + fmt("%d", b);
        x.status = fmt("announced %d times", f.copies) + (f.blocks > 0 ? fmt(", %d blocks", f.blocks) : "");
        x.note = (s.empty() ? std::string("") : "blocks the catalogue said were on the air: " + s + "; ") +
                 "the blocks are rows 1-20 of the catalogue pages (8-bit, encrypted unless the file is sent open)" +
                 (f.check.empty() ? "" : "; row 23: " + f.check);
        auto g = got.find(f.name);
        if (g != got.end() && g->second.rowsHave) {
            const BtWritten& w = g->second;
            x.kind = Find::File;
            x.path = w.path;
            x.unread = w.unread;
            x.have = w.blocksHave;
            x.blockBytes = w.blockBytes;
            x.type = fileTypeOf(w.path, f.name);
            x.status = fmt("%.1f%% read%s; %d of %d blocks, %d of %d rows received", w.readPct, w.checkOk ? ", checks OK" : "", w.blocksHave,
                           std::max(w.blocks, w.blocksHave), w.rowsHave, w.rows) +
                       (w.decrypted ? ", decrypted" : ", as sent (encrypted)") + (w.content.empty() ? "" : "; " + w.content) +
                       (w.check.empty() ? "" : "; check: " + w.check);
            x.note = (w.text.empty() ? std::string("") : "text: " + w.text + "; ") + keyNote + "; " + x.note;
        }
        out.push_back(x);
    }
    if (c.dataPackets()) {
        Find x;
        // not part of the receiver: a service descriptor sent in the signal with the channel data
        x.kind = Find::Text;
        x.name = "service descriptor: " + c.dataText();
        x.type = "service information, packets 8/31 (independent data lines)";
        x.service = svc; x.where = where;
        x.status = fmt("%lld packets", (long long)c.dataPackets());
        x.text = {c.dataText()};
        x.note = "service information (descriptor) of the data channel, sent with the signal";
        out.push_back(x);
    }
}

std::vector<Find> findsOf(const DecodeResult& r)
{
    if (r.finds) return *r.finds;
    return findsCompute(r, nullptr);
}

void prepareFinds(DecodeResult& r, const FindStatus& status)
{
    r.finds = std::make_shared<const std::vector<Find>>(findsCompute(r, status));
}

std::vector<Find> findsCompute(const DecodeResult& r, const FindStatus& status)
{
    std::vector<Find> out;
    // the time code line (ORT 2001 line 6): the time on the air
    if (r.timeCode.size() >= 3) {
        Find x;
        x.kind = Find::Text;
        const std::string& t = r.timeCode[2];
        size_t c = t.find(": ");
        x.name = "time on the air: " + (c != std::string::npos ? t.substr(c + 2) : t);
        x.type = "time code";
        size_t p = r.timeCode[0].find(" (line ");
        x.service = "time code line";
        x.where = p != std::string::npos ? r.timeCode[0].substr(p + 2, r.timeCode[0].find(')', p) - p - 2) : "";
        size_t q = r.timeCode[1].find("; ");
        x.status = q != std::string::npos ? r.timeCode[1].substr(q + 2) : r.timeCode[1];
        x.text = r.timeCode;
        x.note = "1 MHz reference burst and one symbol per frame (2.5 MHz = 1, 1.67 MHz = 0, 1.0 MHz = second mark), 25 a second";
        out.push_back(x);
    }
    for (auto& t : r.tvinform) {
        std::string w;
        if (t.rate < 0) {
            for (auto& li : r.lines)
                if (li.kind.rfind("TV-Inform packets", 0) == 0) w += (w.empty() ? "lines " : ", ") + fmt("%d", li.lineNo) + (li.field == 2 ? "/f2" : "");
            if (w.empty()) w = "lines 7-9";
        } else if (!t.words.empty())
            w = fmt("line %d field %d", t.words[0].lineNo, t.words[0].field);
        addTviFinds(out, t, w);
    }
    // «Бизнес-Текст»: the packets of a live capture (binary rows too), else the teletext engine's stream
    std::string stem = r.path.substr(0, r.path.rfind('.'));
    std::string t42 = exists(stem + ".raw.t42") ? stem + ".raw.t42" : "";
    if (t42.empty() && !r.teletext.project.empty() && exists(path_join(r.teletext.project, "stream.t42"))) t42 = path_join(r.teletext.project, "stream.t42");
    // pages built from the live decoder's stream (decoder.cpp liveTeletext): the stream has the recording's name
    std::string own = r.teletext.project.empty() ? "" : path_join(r.teletext.project, basename(stem_path(r.path)) + ".t42");
    if (t42.empty() && !own.empty() && exists(own)) t42 = own;
    if (!t42.empty()) {
        // the data rows of a recording come from its live packets (.raw.t42) when it has them: the file decoder's
        // stream repairs parity and spoils 8-bit bytes (ntvtest: its copies agree with the live ones in 5-15 bytes
        // of 40), so a stream gives the catalogue only then (weight 0)
        auto trust = [](const std::string& p) {
            if (p.size() > 8 && p.compare(p.size() - 8, 8, ".raw.t42") == 0) return 1;
            std::string vbiDir = dirname(dirname(p));   // <recording>_vbi/teletext/stream.t42
            if (vbiDir.size() > 4 && vbiDir.compare(vbiDir.size() - 4, 4, "_vbi") == 0 && exists(vbiDir.substr(0, vbiDir.size() - 4) + ".raw.t42"))
                return 0;
            return 1;
        };
        BtCatalog c = btCatalogOf(t42, trust(t42));
        // the same transmission in other recordings next to this one (the tape played again): their copies of
        // the blocks fill the rows this one lost. A recording's live packets (.raw.t42) and the decoder's stream
        // are read both
        std::string merged;
        if (!c.empty()) {
            std::set<std::string> streams, own;
            std::string mine = stem + ".raw.t42";
            for (const std::string& p : {mine, r.teletext.project.empty() ? std::string() : path_join(r.teletext.project, "stream.t42")})
                if (!p.empty() && p != t42 && exists(p)) { streams.insert(p); own.insert(p); }
            std::error_code ec;
            for (auto& de : std::filesystem::directory_iterator(std::filesystem::u8path(dirname(r.path)), ec)) {
                std::string p = de.path().u8string();
                if (p.size() > 8 && p.compare(p.size() - 8, 8, ".raw.t42") == 0 && p != t42 && std::filesystem::u8path(p) != std::filesystem::u8path(mine)) {
                    streams.insert(p);
                    std::string st = path_join(p.substr(0, p.size() - 8) + "_vbi", "teletext/stream.t42");
                    if (exists(st)) streams.insert(st);
                }
            }
            for (auto& p : streams) {
                BtCatalog o = btCatalogOf(p, trust(p));
                if (own.count(p) || c.sameFiles(o)) {
                    c.merge(o);
                    if (!own.count(p)) merged += (merged.empty() ? "" : ", ") + basename(p);
                }
            }
        }
        std::string w;
        std::set<int> seen;
        for (auto& li : r.lines)
            if (li.kind.find("WST") != std::string::npos && seen.insert(li.lineNo + 1000 * li.field).second)
                w += (w.empty() ? "lines " : ", ") + fmt("%d", li.lineNo) + (li.field == 2 ? "/f2" : "");
        // the files: next to the teletext folder of the results
        std::string dir = !r.teletext.project.empty() ? path_join(dirname(r.teletext.project), "biznestext_files") : stem + "_biznestext_files";
        addBtFinds(out, c, (w.empty() ? "teletext lines" : w) + (merged.empty() ? "" : "; with " + merged), dir, status);
        addTtDataFinds(out, ttDataPagesOf(t42), w.empty() ? "teletext lines" : w);
    }
    return out;
}

void addTtDataFinds(std::vector<Find>& out, const TtDataPages& t, const std::string& where)
{
    for (auto& d : t.pages()) {
        Find x;
        x.kind = Find::Text;
        std::string first;
        for (auto& l : d.text) if (!strip(l).empty()) { first = strip(l).substr(0, 60); break; }
        x.name = fmt("data page %03X/%04X", d.page, d.sub) + (first.empty() ? (d.zeros ? ": zero bytes" : "") : ": " + first);
        x.type = d.text.empty() ? (d.zeros ? "data, all zero bytes" : "binary data") : "text";
        std::string h = strip(d.headerText);
        x.service = "teletext" + (h.empty() ? std::string("") : " (" + h + ")") + ", Hamming 8/4 data page";
        x.where = where + fmt(", page %03X, subcode %04X", d.page, d.sub);
        x.status = fmt("%d copies, %zu bytes", d.copies, d.data.size()) + (d.unread ? fmt(", %d not read", d.unread) : "");
        x.size = d.data.size();
        std::string hx;
        for (uint8_t b : d.head) hx += fmt("%02X ", b);
        while (hx.size() >= 3 && hx.compare(hx.size() - 3, 3, "00 ") == 0) hx.resize(hx.size() - 3);
        x.note = std::string(d.inhibit ? "display inhibited (C10); " : "") + "row 1: " + strip(hx) +
                 "; rows 2-24 carry the data, two Hamming 8/4 codewords per byte, low nibble first";
        x.text = d.text;
        if (x.text.empty()) {   // binary: as hex lines of 16 bytes
            for (size_t i = 0; i < d.data.size(); i += 16) {
                std::string l;
                for (size_t k = i; k < std::min(d.data.size(), i + 16); k++) l += fmt("%02X ", d.data[k]);
                x.text.push_back(strip(l));
            }
        }
        out.push_back(x);
    }
}

// ---------------------------------------------------------------- files Windows can open
namespace {
bool sameExt(const std::string& path, const std::string& ext)
{
    size_t dot = path.find_last_of('.'), sl = path.find_last_of("\\/");
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return false;
    return lower(path.substr(dot)) == ext;
}
}  // namespace

std::string combineReceptions(const std::string& path, const std::vector<std::string>& others, std::string& outPath)
{
    std::vector<Bytes> copies{read_file(path)};
    for (auto& o : others) copies.push_back(read_file(o));
    std::string log = "combined: " + path + "\n";
    for (auto& o : others) log += "    with: " + o + "\n";
    Bytes m = combineCopies(copies, log);
    if (m.empty()) return log;
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    std::string dir = path_join(path.substr(0, path.find_last_of("\\/")), name + "_combined");
    make_dirs(dir);
    outPath = path_join(dir, name);
    write_file(outPath, m);
    if (m.size() >= 4 && m[0] == 0x60 && m[1] == 0xEA) {
        ArjCheck c = arjCheck(m);
        log += "\nunpacked into " + name + "_files:\n" + arjExtract(c, path_join(dir, name + "_files"));
    } else {
        std::vector<long> un;
        for (size_t i = 0; i < m.size(); i++) if (!m[i]) un.push_back((long)i);
        openableCopy(outPath, true, &un);
    }
    write_file(path_join(dir, "combined.txt"), log.data(), log.size());
    return log;
}

std::string openableCopy(const std::string& path, bool refresh, const std::vector<long>* unread)
{
    if (path.empty() || !exists(path)) return "";
    if (!refresh && exists(path + ".txt")) return path + ".txt";
    if (!refresh && exists(path + ".png")) return path + ".png";
    Bytes d;
    try { d = read_file(path); } catch (...) { return ""; }
    if (d.size() < 8) return "";
    auto copyAs = [&](const std::string& ext) -> std::string {
        if (sameExt(path, ext)) return path;
        std::string out = path + ext;
        try { write_file(out, d); } catch (...) { return ""; }
        return out;
    };
    auto pngOf = [&](const Image& im) -> std::string {
        try { png_save(path + ".png", im); } catch (...) { return ""; }
        return path + ".png";
    };
    // pictures
    if ((d[0] == 'I' && d[1] == 'I' && d[2] == 42) || (d[0] == 'M' && d[1] == 'M' && d[3] == 42)) {
        int w = 0, h = 0;
        std::vector<uint8_t> px, t(d.begin(), d.end());
        std::string note;
        if (faxTiffDecode(t, w, h, px, note) && w > 0 && h > 0) {   // fax G4: Windows' viewer often cannot
            Image im(w, h, 0xFFFFFF);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[(size_t)y * w + x]) im.set(x, y, 0);
            return pngOf(im);
        }
        return copyAs(".tif");
    }
    if (d[0] == 0x0A && d[1] <= 5 && d[2] == 1) {
        Image im;   // put together again where bytes were lost (the line above fills the lines lost)
        if (pcxDecode(d, unread ? *unread : std::vector<long>(), im, true)) { copyAs(".pcx"); return pngOf(im); }
        return copyAs(".pcx");
    }
    if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return copyAs(".gif");
    if (d[0] == 0xFF && d[1] == 0xD8) return copyAs(".jpg");
    if (d[0] == 'B' && d[1] == 'M') return copyAs(".bmp");
    // archives (7-Zip, WinRAR open them all; Windows itself ZIP)
    if (d[0] == 0x60 && d[1] == 0xEA) return copyAs(".arj");
    if (d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) return copyAs(".zip");
    if (d[0] == 'R' && d[1] == 'a' && d[2] == 'r' && d[3] == '!') return copyAs(".rar");
    if (d[2] == '-' && d[3] == 'l' && (d[4] == 'h' || d[4] == 'z') && d[6] == '-') return copyAs(".lzh");
    if (d[0] == 0x1A && d[1] >= 1 && d[1] <= 11 && d.size() > 16) {   // ARC: a DOS name of up to 12 characters, 0 at its end
        int k = 2;
        while (k < 15 && d[k] > 0x20 && d[k] < 0x7F) k++;
        if (k > 2 && d[k] == 0) return copyAs(".arc");
    }
    if (d[0] == 'Z' && d[1] == 'O' && d[2] == 'O') return copyAs(".zoo");
    if (d[0] == 0x1F && d[1] == 0x8B) return copyAs(".gz");
    // documents
    if (d[0] == 0xD0 && d[1] == 0xCF && d[2] == 0x11 && d[3] == 0xE0) return copyAs(".doc");   // Word 97+ / Excel
    if (d[0] == 0x31 && d[1] == 0xBE && d[2] == 0 && d[3] == 0) return copyAs(".doc");         // Word for Windows 1-2
    if (d[0] == 0xDB && d[1] == 0xA5) return copyAs(".doc");                                   // Word 2.0
    if (!memcmp(d.data(), "{\\rtf", 5)) return copyAs(".rtf");
    if (!memcmp(d.data(), "%PDF", 4)) return copyAs(".pdf");
    if (!memcmp(d.data(), "%!PS", 4)) return copyAs(".ps");
    if (d[0] == 0x09 && d[1] == 0x04 && d[2] == 0x06 && d[3] == 0) return copyAs(".xls");      // Excel 4
    // DOS text (CP866) -> UTF-8 .txt for Notepad
    size_t text = 0, n = std::min<size_t>(d.size(), 4096);
    for (size_t i = 0; i < n; i++) text += d[i] >= 32 || d[i] == '\r' || d[i] == '\n' || d[i] == '\t' || d[i] == 0x1A;
    if (text * 100 >= n * 97) {
        std::string s;
        for (uint8_t c : d) if (c != 0x1A) s += c == '\r' || c == '\n' || c == '\t' ? std::string(1, (char)c) : utf8Of866(c);
        try { write_file(path + ".txt", s.data(), s.size()); } catch (...) { return ""; }
        return path + ".txt";
    }
    return "";
}
