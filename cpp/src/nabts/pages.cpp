#include "pages.h"

Row row_from_json(const Json &a) {
    Row r; r.fill(0x20);
    for (size_t i = 0; i < 40 && i < a.size(); i++) r[i] = (u8)a.a[i].integer(0x20);
    return r;
}
Json row_to_json(const Row &r) {
    Json a = Json::array();
    for (u8 v : r) a.a.push_back(Json((int)v));
    return a;
}

Json version_to_json(const Version &v) {
    Json j = Json::object();
    j["t"] = v.t; j["n"] = v.n;
    Json rows = Json::object();
    for (auto &kv : v.rows) rows[std::to_string(kv.first)] = row_to_json(kv.second);
    j["rows"] = rows;
    Json c = Json::object();
    for (auto &kv : v.c) c[std::to_string(kv.first)] = kv.second;
    j["c"] = c;
    if (!v.s.empty()) j["s"] = v.s;
    if (v.full) j["full"] = true;
    return j;
}
Version version_from_json(const Json &j) {
    Version v;
    v.t = j.get_num("t"); v.n = (int)j.get_num("n");
    if (auto r = j.find("rows")) for (auto &kv : r->o) v.rows[atoi(kv.first.c_str())] = row_from_json(kv.second);
    if (auto c = j.find("c")) for (auto &kv : c->o) v.c[atoi(kv.first.c_str())] = kv.second.integer(1);
    v.s = j.get_str("s");
    v.full = j.get_bool("full");
    return v;
}

Json build_to_json(const PagesBuild &p) {
    Json j = Json::object();
    for (auto &kv : p) {
        Json a = Json::array();
        for (auto &v : kv.second) a.a.push_back(version_to_json(v));
        j[kv.first] = a;
    }
    return j;
}
PagesBuild build_from_json(const Json &j) {
    PagesBuild p;
    for (auto &kv : j.o) {
        auto &L = p[kv.first];
        for (auto &v : kv.second.a) L.push_back(version_from_json(v));
    }
    return p;
}

static Json str_map(const std::map<std::string, std::string> &m) {
    Json j = Json::object();
    for (auto &kv : m) j[kv.first] = kv.second;
    return j;
}
static std::map<std::string, std::string> str_map(const Json &j) {
    std::map<std::string, std::string> m;
    for (auto &kv : j.o) m[kv.first] = kv.second.str();
    return m;
}

Json extras_to_json(const PageExtras &e) {
    Json j = Json::object();
    if (e.tx) { j["tx"] = e.tx; j["subpages"] = e.subpages; if (e.boxed) j["boxed"] = true; if (e.nat >= 0) j["nat"] = e.nat; }
    if (!e.flof.empty()) {
        Json a = Json::array();
        for (auto &s : e.flof) a.a.push_back(s.empty() ? Json() : Json(s));
        j["flof"] = a;
    }
    if (!e.x26.empty()) j["x26"] = str_map(e.x26);
    if (!e.x26s.empty()) {
        Json s = Json::object();
        for (auto &kv : e.x26s) s[kv.first] = str_map(kv.second);
        j["x26s"] = s;
    }
    return j;
}
PageExtras extras_from_json(const Json &j) {
    PageExtras e;
    e.tx = (int)j.get_num("tx"); e.subpages = (int)j.get_num("subpages", 1);
    e.boxed = j.get_bool("boxed"); e.nat = j.has("nat") ? (int)j.get_num("nat") : -1;
    if (auto f = j.find("flof")) if (f->is_arr()) { for (auto &x : f->a) e.flof.push_back(x.str()); e.flof.resize(5); }
    if (auto x = j.find("x26")) e.x26 = str_map(*x);
    if (auto x = j.find("x26s")) for (auto &kv : x->o) e.x26s[kv.first] = str_map(kv.second);
    return e;
}

Json page_to_json(const Page &p) {
    Json j = extras_to_json(p.ex);
    Json v = Json::array();
    for (auto &x : p.versions) v.a.push_back(version_to_json(x));
    Json out = Json::object();
    out["versions"] = v;
    out["deleted"] = p.deleted;
    for (auto &kv : j.o) out[kv.first] = kv.second;
    return out;
}
Page page_from_json(const Json &j) {
    Page p;
    if (auto v = j.find("versions")) for (auto &x : v->a) p.versions.push_back(version_from_json(x));
    p.deleted = j.get_bool("deleted");
    p.ex = extras_from_json(j);
    return p;
}
