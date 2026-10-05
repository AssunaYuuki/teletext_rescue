#include "json.h"
#include "util.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

bool Json::truthy() const {
    switch (t) {
    case Null: return false;
    case Bool: return b;
    case Num: return n != 0;
    case Str: return !s.empty();
    case Arr: return !a.empty();
    case Obj: return !o.empty();
    }
    return false;
}

const Json *Json::find(const std::string &k) const {
    if (t != Obj) return nullptr;
    for (auto &p : o) if (p.first == k) return &p.second;
    return nullptr;
}
Json *Json::find(const std::string &k) {
    if (t != Obj) return nullptr;
    for (auto &p : o) if (p.first == k) return &p.second;
    return nullptr;
}
const Json &Json::operator[](const std::string &k) const {
    static const Json null;
    auto p = find(k);
    return p ? *p : null;
}
Json &Json::operator[](const std::string &k) {
    if (t != Obj) { t = Obj; o.clear(); }
    if (auto p = find(k)) return *p;
    o.emplace_back(k, Json());
    return o.back().second;
}
void Json::erase(const std::string &k) {
    for (size_t i = 0; i < o.size(); i++) if (o[i].first == k) { o.erase(o.begin() + i); return; }
}

static void dump_str(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

static void dump_num(std::string &out, double v) {
    if (!std::isfinite(v)) { out += "null"; return; }
    if (v == floor(v) && fabs(v) < 1e15) { char b[32]; snprintf(b, sizeof b, "%.0f", v); out += b; return; }
    char b[40]; snprintf(b, sizeof b, "%.15g", v);
    double back = strtod(b, nullptr);
    if (back != v) snprintf(b, sizeof b, "%.17g", v);
    out += b;
}

static void dump_rec(std::string &out, const Json &j, int indent, int level) {
    auto nl = [&](int lv) { if (indent >= 0) { out += '\n'; out.append(lv * indent, ' '); } };
    switch (j.t) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: dump_num(out, j.n); break;
    case Json::Str: dump_str(out, j.s); break;
    case Json::Arr:
        out += '[';
        for (size_t i = 0; i < j.a.size(); i++) {
            if (i) out += indent >= 0 ? "," : ",";
            nl(level + 1);
            dump_rec(out, j.a[i], indent, level + 1);
        }
        if (!j.a.empty()) nl(level);
        out += ']';
        break;
    case Json::Obj:
        out += '{';
        for (size_t i = 0; i < j.o.size(); i++) {
            if (i) out += ",";
            nl(level + 1);
            dump_str(out, j.o[i].first);
            out += indent >= 0 ? ": " : ":";
            dump_rec(out, j.o[i].second, indent, level + 1);
        }
        if (!j.o.empty()) nl(level);
        out += '}';
        break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_rec(out, *this, indent, 0);
    return out;
}

namespace {
struct Parser {
    const std::string &s; size_t i = 0;
    explicit Parser(const std::string &x) : s(x) {}
    [[noreturn]] void fail(const char *m) { throw std::runtime_error(fmt("JSON: %s at %zu", m, i)); }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    Json value() {
        ws();
        if (i >= s.size()) fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            i++; Json j = Json::object(); ws();
            if (i < s.size() && s[i] == '}') { i++; return j; }
            for (;;) {
                ws(); if (s[i] != '"') fail("expected key");
                std::string k = string(); ws();
                if (s[i] != ':') fail("expected :"); i++;
                j.o.emplace_back(std::move(k), value()); ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; return j; }
                fail("expected , or }");
            }
        }
        if (c == '[') {
            i++; Json j = Json::array(); ws();
            if (i < s.size() && s[i] == ']') { i++; return j; }
            for (;;) {
                j.a.push_back(value()); ws();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; return j; }
                fail("expected , or ]");
            }
        }
        if (c == '"') return Json(string());
        if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        if (s.compare(i, 4, "null") == 0) { i += 4; return Json(); }
        if (s.compare(i, 3, "NaN") == 0) { i += 3; return Json(NAN); }
        if (s.compare(i, 8, "Infinity") == 0) { i += 8; return Json(INFINITY); }
        if (s.compare(i, 9, "-Infinity") == 0) { i += 9; return Json(-INFINITY); }
        char *e = nullptr;
        double v = strtod(s.c_str() + i, &e);
        if (e == s.c_str() + i) fail("bad value");
        i = e - s.c_str();
        return Json(v);
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else fail("bad \\u");
        }
        return v;
    }
    std::string string() {
        i++; std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2; unsigned lo = hex4(); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                out += from_cp(cp); break;
            }
            default: out += e;
            }
        }
        i++;
        return out;
    }
};
}

Json Json::parse(const std::string &text) {
    Parser p(text);
    Json j = p.value();
    return j;
}

Json load_json(const std::string &path, const Json &dflt) {
    if (!exists(path)) return dflt;
    try { return Json::parse(read_text(path)); } catch (...) { return dflt; }
}

void save_json(const std::string &path, const Json &j, int indent) { write_text(path, j.dump(indent)); }
