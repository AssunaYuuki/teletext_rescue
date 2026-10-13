#include "ssjson.h"
#include "ui/util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

const JValue* JValue::get(const std::string& k) const
{
    for (auto& kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

int JValue::i(const std::string& k, int def) const
{
    const JValue* v = get(k);
    return v && v->type == Num ? (int)v->n : (v && v->type == Bool ? v->b : def);
}

std::string JValue::str(const std::string& k) const
{
    const JValue* v = get(k);
    return v && v->type == Str ? v->s : std::string();
}

namespace {

struct Parser {
    const char* p;
    const char* e;
    std::string err;

    void ws()
    {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    bool fail(const char* m)
    {
        if (err.empty()) err = m;
        return false;
    }
    static void utf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | cp >> 6); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xE0 | cp >> 12); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool string(std::string& s)
    {
        if (*p != '"') return fail("expected string");
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u':
                    if (e - p > 4) { utf8(s, (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16)); p += 4; }
                    break;
                default: s += *p;
                }
                p++;
            } else s += *p++;
        }
        if (p >= e) return fail("unterminated string");
        p++;
        return true;
    }
    bool value(JValue& v)
    {
        ws();
        if (p >= e) return fail("unexpected end");
        if (*p == '{') {
            v.type = JValue::Obj;
            p++;
            ws();
            if (*p == '}') { p++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (*p != ':') return fail("expected ':'");
                p++;
                v.o.emplace_back(k, JValue());
                if (!value(v.o.back().second)) return false;
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (*p == '[') {
            v.type = JValue::Arr;
            p++;
            ws();
            if (*p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back())) return false;
                ws();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (*p == '"') { v.type = JValue::Str; return string(v.s); }
        if (!strncmp(p, "true", 4)) { v.type = JValue::Bool; v.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.type = JValue::Bool; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* end = nullptr;
        v.n = strtod(p, &end);
        if (end == p) return fail("bad value");
        v.type = JValue::Num;
        p = end;
        return true;
    }
};

std::string esc(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

} // namespace

bool jsonParse(const std::string& text, JValue& v, std::string& err)
{
    Parser ps{text.data(), text.data() + text.size(), {}};
    if (!ps.value(v)) { err = ps.err; return false; }
    return true;
}

static bool readFile(const std::string& path, std::string& out)
{
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = fread(&out[0], 1, n, f);
    fclose(f);
    return got == (size_t)n;
}

bool ssImportJson(const std::string& path, SsDatabase& db, std::string& err)
{
    std::string text;
    if (!readFile(path, text)) { err = "cannot read " + path; return false; }
    JValue root;
    if (!jsonParse(text, root, err)) return false;
    if (const JValue* a = root.get("channels"))
        for (auto& c : a->a) {
            SsChannel ch;
            ch.id = c.i("id");
            ch.name = c.str("name");
            ch.verified = true;
            auto& slot = db.channels[ch.id];
            if (slot.id == 0 || slot.name.empty()) slot = ch;
        }
    if (const JValue* a = root.get("themes"))
        for (auto& t : a->a) db.themes[t.i("id")] = SsTheme{t.i("id"), t.str("name")};
    if (const JValue* a = root.get("programs"))
        for (auto& p : a->a) {
            SsProgram pr;
            pr.id = p.i("id");
            pr.title = p.str("title");
            pr.theme = p.i("theme");
            pr.stereo = p.i("stereo");
            pr.cc = p.i("closedCaptioned");
            pr.bw = p.i("blackAndWhite");
            auto& slot = db.programs[pr.id];
            if (slot.id == 0 || slot.title.empty()) slot = pr;
        }
    if (const JValue* a = root.get("descriptions"))
        for (auto& d : a->a) {
            SsDescription ds;
            ds.id = d.i("id");
            ds.text = d.str("text");
            ds.ratingSystem = d.str("ratingSystem");
            ds.rating = d.str("rating");
            ds.year = d.i("year");
            ds.critic = d.i("criticRating");
            if (const JValue* adv = d.get("advisories"))
                for (auto& x : adv->a) ds.advisories.push_back(x.s);
            auto& slot = db.descriptions[ds.id];
            if (slot.id == 0 || slot.text.empty()) slot = ds;
        }
    if (const JValue* a = root.get("listings")) {
        // group the flat listings back into per-channel, per-GMT-day show lists
        std::map<std::pair<int, SsTime>, SsShowList> lists;
        for (auto& l : a->a) {
            SsSlot s;
            if (!ssParseIso(l.str("start"), s.start)) continue;
            s.duration = l.i("durationMinutes");
            s.program = l.i("program");
            s.description = l.i("description");
            s.flags = l.i("slotFlags", -1);
            if (s.flags < 0)
                s.flags = (s.description ? 0x80 : 0) | (l.i("payPerView") ? 0x40 : 0) | (l.i("joinedInProgress") ? 1 : 0);
            int ch = l.i("channel");
            SsTime day = s.start / 1440 * 1440;
            auto& sl = lists[{ch, day}];
            sl.channel = ch;
            sl.start = day;
            sl.version = -1;
            sl.verified = true;
            sl.slots.push_back(s);
        }
        for (auto& kv : lists)
            if (!db.showLists.count(kv.first)) db.showLists[kv.first] = kv.second;
    }
    if (const JValue* a = root.get("clock"))
        for (auto& c : a->a) {
            SsClock k;
            ssParseIso(c.str("time"), k.time, &k.seconds);
            k.utcOffsetMinutes = c.i("utcOffsetMinutes");
            k.dst = c.i("daylightSaving");
            db.clock.push_back(k);
        }
    if (const JValue* a = root.get("daylightSavingChanges"))
        for (auto& c : a->a) {
            SsTime s = 0, e = 0;
            ssParseIso(c.str("starts"), s);
            ssParseIso(c.str("ends"), e);
            if (std::find(db.dstChanges.begin(), db.dstChanges.end(), std::make_pair(s, e)) == db.dstChanges.end())
                db.dstChanges.push_back({s, e});
        }
    if (const JValue* a = root.get("sequenceNumbers"))
        for (auto& c : a->a) db.sequenceNumbers.push_back((uint32_t)c.n);
    return true;
}

bool ssExportJson(const std::string& path, const SsDatabase& db, const DecodeResult* extra, std::string& err)
{
    std::ostringstream o;
    o << "{\n    \"channels\": [";
    bool first = true;
    for (int id : db.channelIds()) {
        auto it = db.channels.find(id);
        o << (first ? "\n" : ",\n") << "        {\"id\": " << id << ", \"name\": "
          << esc(it != db.channels.end() ? it->second.name : "") << "}";
        first = false;
    }
    o << "\n    ],\n    \"themes\": [";
    first = true;
    for (auto& kv : db.themes) {
        o << (first ? "\n" : ",\n") << "        {\"id\": " << kv.first;
        if (!kv.second.name.empty()) o << ", \"name\": " << esc(kv.second.name);
        o << "}";
        first = false;
    }
    o << "\n    ],\n    \"programs\": [";
    first = true;
    for (auto& kv : db.programs) {
        const SsProgram& p = kv.second;
        o << (first ? "\n" : ",\n") << "        {\"id\": " << p.id;
        if (!p.title.empty()) o << ", \"title\": " << esc(p.title) << ", \"theme\": " << p.theme;
        if (p.stereo) o << ", \"stereo\": true";
        if (p.cc) o << ", \"closedCaptioned\": true";
        if (p.bw) o << ", \"blackAndWhite\": true";
        if (p.compressed && p.title.empty()) {
            o << ", \"compressedTitle\": \"";
            for (uint8_t c : p.raw) { char b[4]; snprintf(b, sizeof b, "%02x", c); o << b; }
            o << "\"";
        }
        o << "}";
        first = false;
    }
    o << "\n    ],\n    \"descriptions\": [";
    first = true;
    for (auto& kv : db.descriptions) {
        const SsDescription& d = kv.second;
        o << (first ? "\n" : ",\n") << "        {\"id\": " << d.id << ", \"text\": " << esc(d.text);
        if (!d.rating.empty()) o << ", \"ratingSystem\": " << esc(d.ratingSystem) << ", \"rating\": " << esc(d.rating);
        if (d.year) o << ", \"year\": " << d.year;
        if (d.critic) o << ", \"criticRating\": " << d.critic;
        if (d.compressed && d.text.empty()) {
            o << ", \"compressedText\": \"";
            for (uint8_t c : d.raw) { char b[4]; snprintf(b, sizeof b, "%02x", c); o << b; }
            o << "\"";
        }
        if (!d.advisories.empty()) {
            o << ", \"advisories\": [";
            for (size_t i = 0; i < d.advisories.size(); i++) o << (i ? ", " : "") << esc(d.advisories[i]);
            o << "]";
        }
        o << "}";
        first = false;
    }
    o << "\n    ],\n    \"listings\": [";
    first = true;
    for (int id : db.channelIds())
        for (auto& s : db.slotsFor(id)) {
            o << (first ? "\n" : ",\n") << "        {\"channel\": " << id << ", \"start\": \"" << ssIso(s.start)
              << "\", \"durationMinutes\": " << s.duration << ", \"program\": " << s.program;
            if (s.description) o << ", \"description\": " << s.description;
            if (s.flags & 0x01) o << ", \"joinedInProgress\": true";
            if (s.flags & 0x40) o << ", \"payPerView\": true";
            o << ", \"slotFlags\": " << s.flags << "}";
            first = false;
        }
    o << "\n    ],\n    \"clock\": [";
    first = true;
    for (auto& c : db.clock) {
        int y, mo, d, h, mi, wd;
        ssCivil(c.time, y, mo, d, h, mi, wd);
        char b[160];
        snprintf(b, sizeof b, "{\"time\": \"%04d-%02d-%02dT%02d:%02d:%02dZ\", \"utcOffsetMinutes\": %d, \"daylightSaving\": %s}",
                 y, mo, d, h, mi, c.seconds, c.utcOffsetMinutes, c.dst ? "true" : "false");
        o << (first ? "\n        " : ",\n        ") << b;
        first = false;
    }
    o << "\n    ],\n    \"daylightSavingChanges\": [";
    first = true;
    for (auto& d : db.dstChanges) {
        o << (first ? "\n" : ",\n") << "        {\"starts\": \"" << ssIso(d.first) << "\", \"ends\": \"" << ssIso(d.second)
          << "\"}";
        first = false;
    }
    o << "\n    ],\n    \"sequenceNumbers\": [";
    for (size_t i = 0; i < db.sequenceNumbers.size(); i++) o << (i ? ", " : "") << db.sequenceNumbers[i];
    o << "]";
    if (extra) {
        o << ",\n    \"silentRadio\": [";
        for (size_t i = 0; i < extra->silentRadio.size(); i++)
            o << (i ? ",\n        " : "\n        ") << "{\"frame\": " << extra->silentRadio[i].frame
              << ", \"text\": " << esc(extra->silentRadio[i].text) << "}";
        o << "\n    ],\n    \"captions\": [";
        for (size_t i = 0; i < extra->captions.size(); i++) o << (i ? ",\n        " : "\n        ") << esc(extra->captions[i]);
        o << "\n    ],\n    \"dataText\": [";
        for (size_t i = 0; i < extra->fastText.size(); i++) o << (i ? ",\n        " : "\n        ") << esc(extra->fastText[i]);
        o << "\n    ],\n    \"xds\": [";
        for (size_t i = 0; i < extra->xds.size(); i++) o << (i ? ",\n        " : "\n        ") << esc(extra->xds[i]);
        o << "\n    ]";
    }
    o << "\n}\n";
    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::string s = o.str();
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}
