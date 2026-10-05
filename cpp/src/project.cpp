#include "project.h"
#include "pagebuild.h"

bool Project::is_project(const std::string &path) { return exists(path_join(path, "pages.json")); }

bool Project::open(const std::string &path) {
    if (!is_project(path)) return false;
    proj = abspath(path); outdir = proj;
    base = path_join(proj, "pages.json"); edited_path = path_join(proj, "pages_edited.json");
    extras_path = path_join(proj, "extras.json"); quality_path = path_join(proj, "quality.json");
    meta = load_json(path_join(proj, "project.json"), Json::object());
    if (!meta.is_obj()) meta = Json::object();
    extras = load_json(extras_path, Json::object());
    stream = meta.get_str("stream");
    std::string src = meta.get_str("source");
    name = basename(src.empty() ? proj : src);
    title = "Teletext Rescue \xC2\xB7 " + name;
    system = ends_with_i(src, ".t34") ? "525-line WST" : "625-line WST";
    const Json &em = extras["_meta"];
    charset = meta.get_str("charset");
    if (charset.empty()) charset = em.get_str("g0");
    if (charset.empty()) charset = "latin";
    national = (int)em.get_num("national", 1);
    if (meta.has("charset2")) charset2 = meta["charset2"].str();
    else charset2 = charset.rfind("cyr", 0) == 0 ? "latin" : "";
    // проекты, собранные раньше, — сведения о канале из потока (один раз, потом лежат в extras.json)
    if (!extras["_meta"].has("service") && !stream.empty() && exists(stream))
        try {
            if (!extras["_meta"].is_obj()) extras["_meta"] = Json::object();
            extras["_meta"]["service"] = service_info(read_t42(stream));
            save_json(extras_path, extras, 1);
        } catch (...) {}
    return true;
}

void Project::set_charset(const std::string &cs) {
    charset = cs; meta["charset"] = cs;
    save_json(path_join(proj, "project.json"), meta, 1);
}
void Project::set_charset2(const std::string &cs) {
    charset2 = cs; meta["charset2"] = cs.empty() ? Json() : Json(cs);
    save_json(path_join(proj, "project.json"), meta, 1);
}
const Charset &Project::table(const Page *pg) const {
    int nat = (pg && pg->ex.nat >= 0) ? pg->ex.nat : national;
    return charset_table(charset, nat);
}
const Charset *Project::table2() const { return charset2.empty() ? nullptr : &charset_table(charset2, 0); }

const Json *Project::clock() const {
    const Json *c = extras["_meta"].find("clock");
    return (c && c->is_arr() && c->size() >= 2) ? c : nullptr;
}
std::string Project::air_time(double t) const {
    const Json *c = clock();
    if (!c) return fmt("%.0f s of recording", t);
    long s = lround(c->a[0].num() + c->a[1].num() * t) % 86400;
    if (s < 0) s += 86400;
    return fmt("%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
}
std::string Project::rel(const std::string &p) const {
    std::error_code ec;
    fs::path r = fs::relative(P(p), P(outdir), ec);
    std::string s = U(r);
    if (ec || s.empty() || s.rfind("..", 0) == 0) return p;
    return replace_all(s, "\\", "/");
}

Pages Project::load_state(std::set<std::string> &edited, std::string &src) const {
    Json j = load_json(edited_path);
    if (j.is_obj() && j["edited"].truthy()) {
        Pages pages;
        for (auto &kv : j["pages"].o) pages[kv.first] = page_from_json(kv.second);
        edited.clear(); for (auto &e : j["edited"].a) edited.insert(e.str());
        src = "your edits (" + rel(edited_path) + ")";
        return pages;
    }
    if (!exists(base)) throw std::runtime_error("no " + base + " \xE2\x80\x94 the project has not been built");
    Json b = Json::parse(read_text(base));
    PagesBuild pb = build_from_json(b);
    Pages pages;
    for (auto &kv : pb) {
        Page p; p.versions = kv.second;
        if (auto e = extras.find(kv.first)) p.ex = extras_from_json(*e);
        pages[kv.first] = p;
    }
    edited.clear();
    src = "original build (" + rel(base) + ")";
    return pages;
}

void Project::save_state(const Pages &pages, const std::set<std::string> &edited) const {
    Json j = Json::object();
    Json pj = Json::object();
    for (auto &kv : pages) pj[kv.first] = page_to_json(kv.second);
    j["pages"] = pj;
    Json e = Json::array(); for (auto &x : edited) e.push(x);
    j["edited"] = e;
    write_atomic(edited_path, j.dump());
}

// ---------------------------------------------------------------- текст
bool over_fits(const std::string &ov, int c, const Charset &t) {
    if (ov.empty()) return false;
    std::string g = from_cp(t[(c & 0x7F) - 0x20 < 0 ? 0 : (c & 0x7F) - 0x20]);
    if ((c & 0x7F) < 0x20) g = "";
    char32_t base = nfd_base(ov);
    return c == 0x20 || g == ov || (!g.empty() && to_u32(g)[0] == base);
}

Over Project::fitted(const Rows &rows, const std::map<std::string, std::string> *over, const Charset &t) const {
    Over out;
    if (!over) return out;
    for (auto &kv : *over) {
        int r = 0, c = 0;
        if (sscanf(kv.first.c_str(), "%d,%d", &r, &c) != 2 || c < 0 || c > 39) continue;
        auto it = rows.find(r);
        if (it != rows.end() && over_fits(kv.second, it->second[c] & 0x7F, t)) out[{r, c}] = kv.second;
    }
    return out;
}

const std::map<std::string, std::string> *Project::overlay(const Page &pg, const Version *s) const {
    if (s && !s->s.empty()) {
        auto it = pg.ex.x26s.find(s->s);
        if (it != pg.ex.x26s.end() && !it->second.empty()) return &it->second;
    }
    return pg.ex.x26.empty() ? nullptr : &pg.ex.x26;
}

std::string Project::row_text(const Row &b, const std::map<std::string, std::string> *over, int r, const Charset &t) const {
    int key = r >= 0 ? r : 1;
    Rows rows; rows[key] = b;
    Over f = fitted(rows, over, t);
    auto m = row_texts(rows, t, table2(), &f);
    auto it = m.find(key);
    return it == m.end() ? "" : it->second;
}

static std::string usub(const std::string &s, size_t from) {
    std::u32string u = to_u32(s);
    return from >= u.size() ? "" : from_u32(u.substr(from));
}

// середина заголовка страницы: без номера страницы в начале и без даты и часов в конце
static bool date_word(const std::u32string &w) {
    if (w.empty()) return true;
    bool dig = false, other = false;
    for (char32_t c : w) { if (c >= '0' && c <= '9') dig = true; else if (c != '.' && c != '/' && c != ':' && c != '-' && c != ',') other = true; }
    if (dig && !other) return true;                              // 9  24.05  05/10/97  23:17:18
    std::u32string l; for (char32_t c : w) l += (c >= 'A' && c <= 'Z') || (c >= 0x410 && c <= 0x42F) ? c + 32 : c;
    while (!l.empty() && (l.back() == '.' || l.back() == ',')) l.pop_back();
    static const char *names[] = {
        "jan", "feb", "mar", "mär", "apr", "may", "mai", "jun", "jul", "aug", "sep", "sept", "oct", "okt", "nov", "dec", "dez",
        "january", "february", "march", "april", "june", "july", "august", "september", "october", "november", "december",
        "januar", "februar", "märz", "juni", "juli", "oktober", "dezember",
        "mo", "di", "mi", "do", "fr", "sa", "so", "mon", "tue", "wed", "thu", "fri", "sat", "sun", "lu", "ma", "me", "je", "ve", "di",
        "montag", "dienstag", "mittwoch", "donnerstag", "freitag", "samstag", "sonntag",
        "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
        "янв", "фев", "мар", "апр", "мая", "май", "июн", "июл", "авг", "сен", "сент", "окт", "ноя", "дек",
        "января", "февраля", "марта", "апреля", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря",
        "пн", "вт", "ср", "чт", "пт", "сб", "вс", "пон", "вто", "сре", "чет", "пят", "суб", "вос",
        "понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье"};
    for (auto *n : names) if (to_u32(n) == l) return true;
    return false;
}
std::string Project::header_middle(const Page &pg, const std::string &pid) const {
    const Charset &t = table(&pg);
    std::map<std::string, int> cnt;                               // самый частый текст заголовка среди версий
    for (auto &s : pg.versions) {
        auto h = s.rows.find(0);
        if (h == s.rows.end()) continue;
        Row b = h->second;
        for (int k = 0; k < 8; k++) b[k] = 0x20;                  // служебная часть
        for (int k = 32; k < 40; k++) b[k] = 0x20;                // часы
        cnt[strip(row_text(b, nullptr, 0, t))]++;
    }
    std::string best; int n = 0;
    for (auto &kv : cnt) if (kv.second > n && !kv.first.empty()) { best = kv.first; n = kv.second; }
    std::vector<std::u32string> w;
    { std::u32string u = to_u32(best), cur; for (char32_t c : u) { if (c == ' ' || c < 0x20) { if (!cur.empty()) w.push_back(cur); cur.clear(); } else cur += c; } if (!cur.empty()) w.push_back(cur); }
    size_t a = 0, e = w.size();
    if (a < e) {                                                  // номер страницы: 100, P100, 1A0
        std::u32string f = w[0]; if (!f.empty() && (f[0] == 'P' || f[0] == 'p')) f = f.substr(1);
        bool pn = f.size() == 3; for (char32_t c : f) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) pn = false;
        if (pn) a = 1;
        if (pid.size() == 3) { f = to_u32(pid); pn = true; }       // номер страницы известен
        // тот же номер ещё раз — отдельным словом или в конце слова (Кинотеатры247)
        if (pn) {
            std::vector<std::u32string> v(w.begin(), w.begin() + a);
            for (size_t i = a; i < w.size(); i++) {
                std::u32string x = w[i];
                if (x == f) continue;
                if (x.size() > f.size() && x.compare(x.size() - f.size(), f.size(), f) == 0) x = x.substr(0, x.size() - f.size());
                v.push_back(x);
            }
            w = v; e = w.size();
        }
    }
    while (e > a && date_word(w[e - 1])) e--;
    std::u32string o;
    for (size_t i = a; i < e; i++) { if (!o.empty()) o += U' '; o += w[i]; }
    return from_u32(o);
}
std::string Project::page_title(const Page &pg, const std::string &pid) const {
    std::string h = header_middle(pg, pid);
    if (!h.empty()) return h;
    // заголовок пустой — название из первых рядов страницы
    const Charset &t = table(&pg);
    for (auto &s : pg.versions) {
        auto r2 = s.rows.find(2), r3 = s.rows.find(3);
        std::string t3 = r3 != s.rows.end() ? strip(usub(row_text(r3->second, nullptr, 3, t), 24)) : "";
        if (t3.empty()) continue;
        std::string t2 = r2 != s.rows.end() ? strip(usub(row_text(r2->second, nullptr, 2, t), 24)) : "";
        if (!t2.empty() && t2.back() == '-') return t2.substr(0, t2.size() - 1) + t3;
        if (!t2.empty() && !(t2[0] >= '0' && t2[0] <= '9')) return strip(t2 + " " + t3);
        return t3;
    }
    return "";
}

std::string Project::service_note(const Page &pg) const {
    return pg.ex.boxed ? "service: for display over the picture (C6 flag)" : "";
}

std::string Project::version_label(const Page &pg, size_t k) const {
    const auto &V = pg.versions;
    const Version &s = V[k];
    std::vector<size_t> same;
    for (size_t i = 0; i < V.size(); i++) if (V[i].s == s.s) same.push_back(i);
    std::set<std::string> subs; for (auto &x : V) if (!x.s.empty()) subs.insert(x.s);
    std::string head;
    if (!s.s.empty() && subs.size() > 1) {
        size_t idx = std::distance(subs.begin(), subs.find(s.s));
        head = fmt("subpage %s (%zu of %zu) \xC2\xB7 ", s.s.c_str(), idx + 1, subs.size());
    }
    if (s.full) return head + "complete page \xC2\xB7 up to \xE2\x89\x88" + air_time(s.t) + " broadcast";
    size_t pos = std::find(same.begin(), same.end(), k) - same.begin();
    return head + fmt("version %zu/%zu \xC2\xB7 \xE2\x89\x88", pos + 1, same.size()) + air_time(s.t) + " broadcast";
}

// ---------------------------------------------------------------- полные страницы
static int row_dist(const Row &a, const Row &b) { int d = 0; for (int k = 0; k < 40; k++) d += (a[k] & 0x7F) != (b[k] & 0x7F); return d; }

std::vector<Version> Project::full_versions(const Page &pg) const {
    std::vector<std::string> order; std::map<std::string, std::vector<const Version *>> by;
    for (auto &s : pg.versions) { if (!by.count(s.s)) order.push_back(s.s); by[s.s].push_back(&s); }
    std::vector<Version> out;
    for (auto &sub : order) {
        auto &V = by[sub];
        const Version *last = V[0];
        for (auto *x : V) if (x->t > last->t) last = x;
        Version res; std::set<int> rset;
        for (auto *x : V) for (auto &rb : x->rows) rset.insert(rb.first);
        for (int r : rset) {
            struct Var { const Row *b; int w; double t; };
            std::vector<Var> var;
            for (auto *x : V) {
                auto it = x->rows.find(r);
                if (it == x->rows.end()) continue;
                auto c = x->c.find(r);
                int w = (c != x->c.end() && c->second) ? c->second : (x->n ? x->n : 1);
                var.push_back({&it->second, w, x->t});
            }
            if (r == 0) { auto h = last->rows.find(0); res.rows[0] = h != last->rows.end() ? h->second : *var.back().b; continue; }
            std::vector<int> score(var.size(), 0);
            for (size_t i = 0; i < var.size(); i++) for (auto &v2 : var) if (row_dist(*var[i].b, *v2.b) <= 6) score[i] += v2.w;
            size_t best = 0;
            for (size_t i = 1; i < var.size(); i++)
                if (score[i] > score[best] || (score[i] == score[best] && var[i].t > var[best].t)) best = i;
            std::vector<std::pair<const Row *, int>> grp;
            for (auto &x : pg.versions) {
                auto it = x.rows.find(r);
                if (it == x.rows.end() || row_dist(*var[best].b, it->second) > 6) continue;
                auto c = x.c.find(r);
                grp.push_back({&it->second, (c != x.c.end() && c->second) ? c->second : (x.n ? x.n : 1)});
            }
            Row row;
            for (int k = 0; k < 40; k++) {
                std::vector<std::pair<int, int>> votes;
                for (auto &g : grp) {
                    int v = (*g.first)[k] & 0x7F; bool f = false;
                    for (auto &p : votes) if (p.first == v) { p.second += g.second; f = true; break; }
                    if (!f) votes.push_back({v, g.second});
                }
                int bv = votes[0].first, bn = votes[0].second;
                for (auto &p : votes) if (p.second > bn) { bv = p.first; bn = p.second; }
                row[k] = (u8)bv;
            }
            res.rows[r] = row; res.c[r] = score[best];
        }
        res.t = last->t; res.n = 0;
        for (auto *x : V) res.n += x->n ? x->n : 1;
        res.s = sub; res.full = true;
        out.push_back(res);
    }
    return out;
}

Pages Project::full_pages(const Pages &pages) const {
    Pages out;
    for (auto &kv : pages) { Page p = kv.second; p.versions = full_versions(kv.second); out[kv.first] = p; }
    return out;
}

// ---------------------------------------------------------------- t42
static void put_mrag(Bytes &o, int mag, int row) {
    o.push_back(ham::enc[(mag & 7) | ((row & 1) << 3)]);
    o.push_back(ham::enc[row >> 1]);
}

static std::vector<Bytes> x26_packets(int mag, const std::map<std::string, std::string> &over) {
    std::map<int, std::vector<std::pair<int, std::string>>> byrow;
    for (auto &kv : over) { int r, c; if (sscanf(kv.first.c_str(), "%d,%d", &r, &c) == 2) byrow[r].push_back({c, kv.second}); }
    std::map<char32_t, int> g2rev;
    const char32_t *g2 = g2_latin();
    for (int i = 0; i < 96; i++) if (g2[i] != ' ' && g2[i] != '$' && g2[i] != '#' && !g2rev.count(g2[i])) g2rev[g2[i]] = 0x20 + i;
    static const std::pair<char32_t, int> DIA[] = {{0x300, 1}, {0x301, 2}, {0x302, 3}, {0x303, 4}, {0x304, 5}, {0x306, 6}, {0x307, 7},
                                                   {0x308, 8}, {0x30A, 10}, {0x327, 11}, {0x30B, 13}, {0x328, 14}, {0x30C, 15}};
    std::vector<int> trips;
    for (auto &kv : byrow) {
        int r = kv.first;
        trips.push_back((r == 24 ? 40 : 40 + r) | (0x04 << 6));
        auto L = kv.second; std::sort(L.begin(), L.end(), [](auto &a, auto &b) { return a.first < b.first; });
        for (auto &e : L) {
            std::u32string u = to_u32(e.second);
            if (u.empty()) continue;
            int mode, data;
            auto sp = nfd_split(e.second);
            int dia = 0; for (auto &d : DIA) if (d.first == sp.second) dia = d.second;
            if (u.size() == 1 && g2rev.count(u[0])) { mode = 0x0F; data = g2rev[u[0]]; }
            else if (sp.second && dia && sp.first < 0x80) { mode = 0x10 + dia; data = (int)sp.first; }
            else if (u.size() == 1 && u[0] < 0x80) { mode = 0x10; data = (int)u[0]; }
            else continue;
            trips.push_back(e.first | (mode << 6) | (data << 11));
        }
    }
    trips.push_back(63 | (0x1F << 6));
    std::vector<Bytes> pk;
    for (size_t i = 0; i < trips.size(); i += 13) {
        std::vector<int> part(trips.begin() + i, trips.begin() + std::min(trips.size(), i + 13));
        while (part.size() < 13) part.push_back(63 | (0x1F << 6));
        Bytes b; put_mrag(b, mag, 26); b.push_back(ham::enc[(i / 13) & 15]);
        for (int t : part) { u8 x[3]; ham::enc2418(t, x); b.insert(b.end(), x, x + 3); }
        pk.push_back(b);
    }
    return pk;
}

static Bytes x27_packet(int mag, const std::vector<std::string> &links) {
    Bytes out; put_mrag(out, mag, 27); out.push_back(ham::enc[0]);
    std::vector<std::string> six = {links.size() > 0 ? links[0] : "", links.size() > 1 ? links[1] : "", links.size() > 2 ? links[2] : "",
                                    links.size() > 3 ? links[3] : "", "", links.size() > 4 ? links[4] : ""};
    for (auto &p : six) {
        if (p.size() == 3) {
            int lm = ((p[0] - '0') & 7) ^ (mag & 7);
            u8 b[6] = {ham::enc[p[2] - '0'], ham::enc[p[1] - '0'], ham::enc[0xF], ham::enc[0x7 | ((lm & 1) << 3)], ham::enc[0xF], ham::enc[0x3 | ((lm >> 1) << 2)]};
            out.insert(out.end(), b, b + 6);
        } else {
            u8 b[6] = {ham::enc[0xF], ham::enc[0xF], ham::enc[0xF], ham::enc[0x7], ham::enc[0xF], ham::enc[0x3]};
            out.insert(out.end(), b, b + 6);
        }
    }
    out.push_back(ham::enc[0xF]); out.push_back(0); out.push_back(0);
    return out;
}

Bytes Project::to_t42(const Pages &pages, int &n) const {
    // байты 4..9 заголовка — самые частые в потоке (национальный набор, флаги)
    std::map<std::string, std::map<Bytes, int>> ctl;
    if (!stream.empty() && exists(stream)) {
        auto st = read_t42(stream);
        for (auto &p : st) {
            int a = ham::dec[p[0]], b = ham::dec[p[1]];
            if (a < 0 || b < 0 || ((a >> 3) | (b << 1)) != 0) continue;
            int u = ham::dec[p[2]], t = ham::dec[p[3]];
            if (u < 0 || t < 0 || u > 9 || t > 9) continue;
            bool ok = true; for (int k = 4; k < 10; k++) if (ham::dec[p[k]] < 0) ok = false;
            if (!ok) continue;
            ctl[fmt("%d%d%d", (a & 7) ? (a & 7) : 8, t, u)][Bytes(p.begin() + 4, p.begin() + 10)]++;
        }
    }
    std::map<std::string, Bytes> best; std::map<Bytes, int> common;
    for (auto &kv : ctl) {
        Bytes b; int n0 = -1;
        for (auto &c : kv.second) if (c.second > n0) { n0 = c.second; b = c.first; }
        best[kv.first] = b; common[b]++;
    }
    Bytes dflt(6, ham::enc[0]);
    { int n0 = 0; for (auto &c : common) if (c.second > n0) { n0 = c.second; dflt = c.first; } }
    Bytes out; n = 0;
    for (auto &kv : pages) {
        const Page &pg = kv.second;
        if (pg.deleted) continue;
        const std::string &pid = kv.first;
        int mag = pid[0] - '0', tens = pid[1] - '0', units = pid[2] - '0';
        for (auto &s : pg.versions) {
            Row hdr; hdr.fill(0x20);
            auto h = s.rows.find(0); if (h != s.rows.end()) hdr = h->second;
            put_mrag(out, mag, 0);
            out.push_back(ham::enc[units & 15]); out.push_back(ham::enc[tens & 15]);
            Bytes c = best.count(pid) ? best[pid] : dflt;
            if (s.s.size() == 4) {
                int v[4]; bool ok = true;
                for (int i = 0; i < 4; i++) { char ch = s.s[i]; v[i] = isdigit(ch) ? ch - '0' : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1; if (v[i] < 0) ok = false; }
                if (ok) {
                    int s4 = v[0], s3 = v[1], s2 = v[2], s1 = v[3];
                    int d1 = ham::dec[c[1]], d3 = ham::dec[c[3]];
                    c[0] = ham::enc[s1]; c[1] = ham::enc[(s2 & 7) | ((d1 < 0 ? 0 : d1) & 8)];
                    c[2] = ham::enc[s3]; c[3] = ham::enc[(s4 & 3) | ((d3 < 0 ? 0 : d3) & 12)];
                }
            }
            out.insert(out.end(), c.begin(), c.end());
            for (int k = 8; k < 40; k++) out.push_back(ham::odd(hdr[k]));
            n++;
            if (!pg.ex.flof.empty()) {
                bool any = false; for (auto &f : pg.ex.flof) if (!f.empty()) any = true;
                if (any) { Bytes x = x27_packet(mag, pg.ex.flof); out.insert(out.end(), x.begin(), x.end()); n++; }
            }
            if (!pg.ex.x26.empty()) for (auto &q : x26_packets(mag, pg.ex.x26)) { out.insert(out.end(), q.begin(), q.end()); n++; }
            for (auto &rb : s.rows) {
                if (rb.first < 1 || rb.first > 24) continue;
                put_mrag(out, mag, rb.first);
                for (int k = 0; k < 40; k++) out.push_back(ham::odd(rb.second[k]));
                n++;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- HTML
static Json versions_for_html(const Project &P, const Page &pg) {
    const Charset &t = P.table(&pg);
    Json out = Json::array();
    for (size_t k = 0; k < pg.versions.size(); k++) {
        const Version &s = pg.versions[k];
        Over x = P.fitted(s.rows, P.overlay(pg, &s), t);
        Json v = Json::object();
        v["t"] = s.t; v["air"] = P.air_time(s.t);
        Json rows = Json::object();
        for (auto &rb : s.rows) rows[std::to_string(rb.first)] = row_to_json(rb.second);
        v["rows"] = rows;
        v["label"] = P.version_label(pg, k);
        if (!x.empty()) {
            Json xo = Json::object();
            for (auto &kv : x) xo[fmt("%d,%d", kv.first.first, kv.first.second)] = kv.second;
            v["x"] = xo;
        }
        out.push(v);
    }
    return out;
}

static Json flof_json(const Page &pg) {
    if (pg.ex.flof.empty()) return Json();
    Json a = Json::array();
    for (auto &s : pg.ex.flof) a.push(s.empty() ? Json() : Json(s));
    return a;
}
static Json str_map_json(const std::map<std::string, std::string> &m) {
    if (m.empty()) return Json();
    Json o = Json::object(); for (auto &kv : m) o[kv.first] = kv.second; return o;
}

static std::pair<double, double> span(const Pages &pages) {
    double a = 1e300, b = -1e300;
    for (auto &kv : pages) for (auto &s : kv.second.versions) { a = std::min(a, s.t); b = std::max(b, s.t); }
    if (a > b) return {0, 0};
    return {a, b};
}

std::string Project::to_output_html(const Pages &pages) const {
    std::string tpl = resource_text("OUTPUT_TEMPLATE"), render = resource_text("TT_RENDER");
    Json data = Json::object(); int n = 0;
    for (auto &kv : pages) {
        const Page &pg = kv.second;
        if (pg.deleted) continue;
        Json d = Json::object();
        d["v"] = versions_for_html(*this, pg);
        d["f"] = flof_json(pg);
        d["x"] = str_map_json(pg.ex.x26);
        d["note"] = service_note(pg);
        d["cs"] = from_u32(std::u32string(table(&pg).begin(), table(&pg).end()));
        const Charset *t2 = table2();
        d["cs2"] = t2 ? Json(from_u32(std::u32string(t2->begin(), t2->end()))) : Json();
        data[kv.first] = d; n++;
    }
    auto sp = span(pages);
    std::string sub = fmt("%d pages (%s), recovered from %s; broadcast \xE2\x89\x88", n, system.c_str(), name.c_str()) +
                      air_time(sp.first) + "\xE2\x80\x93" + air_time(sp.second);
    tpl = replace_all(tpl, "__TITLE__", html_escape(title));
    tpl = replace_all(tpl, "__SUBTITLE__", sub);
    tpl = replace_all(tpl, "__RENDER__", render);
    tpl = replace_all(tpl, "__DATA__", data.dump());
    return tpl;
}

static const char *INDEX_CSS = R"CSS(
:root{--bg:#f4f4f2;--panel:#fff;--fg:#1d1d1b;--mut:#5d5c58;--acc:#1f6fc9;--line:#dcdbd6;--note:#9a5b00}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#141413;--panel:#1d1d1c;--fg:#ececea;--mut:#a3a29c;--acc:#5aa0ee;--line:#34342f;--note:#f0a640}}
:root[data-theme="dark"]{--bg:#141413;--panel:#1d1d1c;--fg:#ececea;--mut:#a3a29c;--acc:#5aa0ee;--line:#34342f;--note:#f0a640}
body{margin:0;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif;padding:16px}
.wrap{max-width:1100px;margin:auto}h1{font-size:20px;margin:0 0 4px}.sub{color:var(--mut);margin-bottom:14px}
a{color:var(--acc)}
table{border-collapse:collapse;width:100%;background:var(--panel);font-size:13px}
th,td{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;white-space:nowrap}
th{position:sticky;top:0;background:var(--panel);color:var(--mut);font-weight:600;cursor:pointer}
td.n{text-align:right;font-variant-numeric:tabular-nums}td.t{white-space:normal}
td.note{color:var(--note);white-space:normal}
.bar{display:inline-block;height:8px;background:var(--acc);border-radius:0 4px 4px 0;vertical-align:middle;margin-left:6px}
input{font:inherit;padding:4px 8px;margin-bottom:10px;border:1px solid var(--line);background:var(--panel);color:var(--fg);border-radius:4px;width:min(320px,100%)}
.scroll{overflow-x:auto}
)CSS";

static std::string index_html(const Project &P, const Pages &pages, const std::vector<std::string> &ids) {
    int mx = 1;
    for (auto &p : ids) mx = std::max(mx, pages.at(p).ex.tx);
    Pages sel; for (auto &p : ids) sel[p] = pages.at(p);
    auto sp = span(sel);
    std::string rows;
    for (auto &p : ids) {
        const Page &pg = pages.at(p);
        int tx = pg.ex.tx; auto &v = pg.versions;
        std::string when = v.size() > 1 ? P.air_time(v[0].t) + "\xE2\x80\x93" + P.air_time(v.back().t) : P.air_time(v[0].t);
        rows += "<tr><td><a href=\"" + p + ".html\"><b>" + p + "</b></a></td><td class=\"t\">" + html_escape(P.page_title(pg, p)) + "</td>" +
                fmt("<td class=\"n\" data-v=\"%d\">%d<span class=\"bar\" style=\"width:%.0fpx\"></span></td>", tx, tx, 60.0 * tx / mx) +
                fmt("<td class=\"n\">%zu</td><td class=\"n\">%d</td><td>", v.size(), pg.ex.subpages) + when + "</td>" +
                "<td class=\"note\">" + html_escape(P.service_note(pg)) + "</td></tr>";
    }
    std::string q = exists(P.quality_path) ? " \xC2\xB7 <a href=\"quality.html\">\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0 \xD0\xBA\xD0\xB0\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2\xD0\xB0 \xD0\xBB\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8B</a>" : "";
    std::string h = "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>" +
                    html_escape(P.title) + "</title><style>" + INDEX_CSS + "</style></head><body><div class=\"wrap\">\n<h1>" + html_escape(P.title) + "</h1>\n" +
                    fmt("<div class=\"sub\">%zu ", ids.size()) + u8"страниц · " + P.system + u8" · эфир ≈" + P.air_time(sp.first) + "\xE2\x80\x93" + P.air_time(sp.second) + q + "</div>\n" +
                    u8R"~(<input id="q" placeholder="Поиск: номер или название">
<div class="scroll"><table id="t"><thead><tr><th>Стр.</th><th>Раздел</th><th title="сколько раз принят заголовок страницы">Принята, раз</th>
<th title="сколько разных версий содержимого">Версий</th><th title="разных кодов подстраниц">Подстраниц</th><th>Эфир (версии)</th><th>Пометка</th></tr></thead>
<tbody>)~" + rows + R"~(</tbody></table></div></div>
<script>
const q=document.getElementById('q'),tb=document.querySelector('#t tbody');
q.oninput=()=>{const s=q.value.toLowerCase();for(const r of tb.rows)r.style.display=r.textContent.toLowerCase().includes(s)?'':'none'};
document.querySelectorAll('th').forEach((th,i)=>th.onclick=()=>{const rs=[...tb.rows],num=i>=2&&i<=4,dir=th.dataset.d=th.dataset.d=='1'?'-1':'1';
 rs.sort((a,b)=>{const x=a.cells[i],y=b.cells[i];const u=num?+(x.dataset.v||x.textContent):x.textContent,w=num?+(y.dataset.v||y.textContent):y.textContent;return (u>w?1:u<w?-1:0)*dir});rs.forEach(r=>tb.appendChild(r))});
</script></body></html>)~";
    return h;
}

static std::string quality_html(const Project &P) {
    Json Q = load_json(P.quality_path);
    if (!Q.is_obj()) return u8"<!doctype html><meta charset=\"utf-8\"><p>карты качества нет: её строит сборка из записи .vbi</p>";
    std::string data = Q["minutes"].dump();
    return std::string(u8R"~(<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Качество ленты</title><style>)~") + INDEX_CSS + u8R"~(
.viz-root{--surface-1:#fcfcfb;--text-primary:#0b0b0b;--text-secondary:#52514e;--grid:#e4e3df;--series-1:#2a78d6}
@media (prefers-color-scheme: dark){:root:where(:not([data-theme="light"])) .viz-root{--surface-1:#1a1a19;--text-primary:#fff;--text-secondary:#c3c2b7;--grid:#33332f;--series-1:#3987e5}}
:root[data-theme="dark"] .viz-root{--surface-1:#1a1a19;--text-primary:#fff;--text-secondary:#c3c2b7;--grid:#33332f;--series-1:#3987e5}
.grid3{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px;margin:12px 0}
.card{background:var(--surface-1);border:1px solid var(--line);border-radius:6px;padding:10px 12px;position:relative}
.card h3{margin:0;font-size:14px;color:var(--text-primary)}.card p{margin:2px 0 6px;font-size:12px;color:var(--text-secondary)}
svg{width:100%;height:auto;display:block}svg text{fill:var(--text-secondary);font-size:10px}
.tip{position:absolute;pointer-events:none;background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:4px 8px;font-size:12px;color:var(--text-primary);display:none;white-space:nowrap;box-shadow:0 2px 6px #0003}
</style></head><body><div class="wrap viz-root">
<h1>Карта качества ленты</h1><div class="sub">По минутам записи )~" + html_escape(P.name) + u8R"~( (в скобках — эфирное время). <a href="index.html">← к страницам</a></div>
<div class="grid3" id="charts"></div>
<h3>Таблица</h3><div class="scroll"><table id="tbl"><thead><tr><th>Минута записи</th><th>Эфир</th><th>Принято строк, %</th><th>Нечитаемо, %</th><th>Ошибки чётности, %</th></tr></thead><tbody></tbody></table></div>
</div><script>
const D=)~" + data + u8R"~(;
D.forEach(d=>d.missed=+(100-d.received).toFixed(2));
const M=[['missed','Не принято строк телетекста','доля строк VBI с телетекстом, из которых пакет не получен, %',null],
         ['unreadable','Из них нечитаемы','сигнал есть, пакет не получен ни одним декодером, %',null],
         ['parity','Ошибки чётности','доля байт с нарушенной чётностью в принятых пакетах, %',null]];
const box=document.getElementById('charts');
for(const [k,title,desc,dom] of M){
  const vals=D.map(d=>d[k]??0),W=320,H=150,L=34,B=22,T=8,R=6,n=vals.length,bw=(W-L-R)/n;
  const lo=dom?dom[0]:0,hi=dom?dom[1]:Math.max(...vals)*1.15||1,y=v=>T+(H-T-B)*(1-(Math.max(v,lo)-lo)/(hi-lo));
  let s=`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${title}">`;
  for(let i=0;i<=4;i++){const v=lo+(hi-lo)*i/4,yy=y(v);s+=`<line x1="${L}" x2="${W-R}" y1="${yy}" y2="${yy}" stroke="var(--grid)" stroke-width="1"/><text x="${L-4}" y="${yy+3}" text-anchor="end">${+v.toFixed(v<1?2:1)}</text>`}
  vals.forEach((v,i)=>{const x=L+i*bw+1,yy=y(v),h=H-B-yy;
    s+=`<path d="M${x},${H-B}V${yy+Math.min(4,h)}q0,-4 4,-4h${bw-10}q4,0 4,4V${H-B}Z" fill="var(--series-1)"/>`;
    s+=`<rect x="${L+i*bw}" y="${T}" width="${bw}" height="${H-T-B}" fill="transparent" data-i="${i}"/>`;
    if(n<=12||i%2==0)s+=`<text x="${L+i*bw+bw/2}" y="${H-8}" text-anchor="middle">${D[i].minute}</text>`});
  s+=`<line x1="${L}" x2="${W-R}" y1="${H-B}" y2="${H-B}" stroke="var(--text-secondary)" stroke-width="1"/></svg>`;
  const c=document.createElement('div');c.className='card';c.innerHTML=`<h3>${title}</h3><p>${desc}${dom?' (ось от '+dom[0]+'%)':''}</p>${s}<div class="tip"></div>`;
  const tip=c.querySelector('.tip');
  c.querySelectorAll('rect[data-i]').forEach(r=>{r.onmousemove=e=>{const d=D[+r.dataset.i];tip.style.display='block';
    tip.innerHTML=`<b>минута ${d.minute}</b> (${d.air||''})<br>${title}: ${(d[k]??0).toFixed(k=='parity'?3:1)}%`;
    const rc=c.getBoundingClientRect();tip.style.left=Math.min(e.clientX-rc.left+12,rc.width-180)+'px';tip.style.top=(e.clientY-rc.top-40)+'px'};
    r.onmouseleave=()=>tip.style.display='none'});
  box.appendChild(c);
}
document.querySelector('#tbl tbody').innerHTML=D.map(d=>`<tr><td class="n">${d.minute}</td><td>${d.air||''}</td><td class="n">${d.received.toFixed(1)}</td><td class="n">${d.unreadable.toFixed(1)}</td><td class="n">${(d.parity??0).toFixed(3)}</td></tr>`).join('');
</script></body></html>)~";
}

int Project::to_page_htmls(const Pages &pages, const std::string &dir) const {
    make_dirs(dir);
    std::error_code ec;
    for (auto &e : fs::directory_iterator(P(dir), ec)) {
        std::string f = U(e.path().filename());
        if (f.size() == 8 && isdigit((u8)f[0]) && isdigit((u8)f[1]) && isdigit((u8)f[2]) && f.substr(3) == ".html") fs::remove(e.path(), ec);
    }
    write_text(path_join(dir, "tt_render.js"), resource_text("TT_RENDER"));
    std::string tpl = resource_text("PAGE_TEMPLATE");
    std::vector<std::string> ids;
    for (auto &kv : pages) if (!kv.second.deleted) ids.push_back(kv.first);
    Json exist = Json::array(); for (auto &p : ids) exist.push(p);
    std::string exist_s = exist.dump();
    for (size_t i = 0; i < ids.size(); i++) {
        const std::string &pid = ids[i];
        const Page &pg = pages.at(pid);
        Json vers = versions_for_html(*this, pg);
        const Charset &t = table(&pg);
        std::string text;
        for (size_t k = 0; k < pg.versions.size(); k++) {
            const Version &s = pg.versions[k];
            text += "=== " + vers[k]["label"].str() + " ===\n";
            for (int r = 0; r < 25; r++) {
                auto it = s.rows.find(r);
                text += (it != s.rows.end() ? row_text(it->second, overlay(pg, &s), r, t) : "");
                text += "\n";
            }
        }
        if (!text.empty()) text.pop_back();
        std::string note = service_note(pg);
        Json extra = Json::object();
        extra["flof"] = flof_json(pg);
        extra["cs"] = from_u32(std::u32string(t.begin(), t.end()));
        const Charset *t2 = table2();
        extra["cs2"] = t2 ? Json(from_u32(std::u32string(t2->begin(), t2->end()))) : Json();
        std::string html = tpl;
        html = replace_all(html, "__PID__", pid);
        html = replace_all(html, "__PREV__", ids[(i + ids.size() - 1) % ids.size()]);
        html = replace_all(html, "__NEXT__", ids[(i + 1) % ids.size()]);
        html = replace_all(html, "__NOTE__", note.empty() ? "" : "<div class=\"note\">" + html_escape(note) + "</div>");
        html = replace_all(html, "__TEXT__", html_escape(text));
        html = replace_all(html, "__EXIST__", exist_s);
        html = replace_all(html, "__EXTRA__", extra.dump());
        html = replace_all(html, "__HINT_TITLE__", html_escape(title));
        html = replace_all(html, "__DATA__", vers.dump());
        write_text(path_join(dir, pid + ".html"), html);
    }
    write_text(path_join(dir, "index.html"), index_html(*this, pages, ids));
    write_text(path_join(dir, "quality.html"), quality_html(*this));
    return (int)ids.size();
}

std::string Project::export_all(const Pages &pages_in, bool full, bool clean) const {
    Pages pages = full || clean ? full_pages(pages_in) : pages_in;
    std::string tag = full ? "_full" : "";
    int n = 0;
    Bytes t42 = to_t42(pages, n);
    std::string out = path_join(outdir, "output" + tag + ".t42"), bak = path_join(outdir, "output.orig.t42");
    std::string note;
    if (!full && exists(out) && !exists(bak)) {
        std::error_code ec; fs::copy_file(P(out), P(bak), ec);
        note = "; the previous output.t42 was kept as output.orig.t42";
    }
    write_atomic(out, std::string(t42.begin(), t42.end()));
    write_text(path_join(outdir, "output" + tag + ".html"), to_output_html(pages));
    int k = to_page_htmls(pages, path_join(outdir, "html" + tag));
    return fmt("output%s.t42 (%d packets%s), output%s.html and %d pages in html%s/ written in ", tag.c_str(), n, clean ? ", cleaned" : "", tag.c_str(), k, tag.c_str()) + outdir + note;
}

std::pair<std::string, int> Project::export_srt(const std::string &page) const {
    std::string lines = meta.get_str("lines"), vbi = meta.get_str("source");
    int lpf = (int)meta.get_num("lpf", 32);
    if (!ends_with_i(vbi, ".vbi")) vbi = "";
    size_t n = file_size(stream) / 42;
    auto times = packet_times(n, lines, vbi, lpf, clock());
    auto cs = subtitle_cues(stream, page, times, charset, charset2);
    std::string out = path_join(outdir, "subtitles_" + page + ".srt");
    write_text(out, to_srt(cs));
    return {out, (int)cs.size()};
}

// ---------------------------------------------------------------- очищенный поток
std::string Project::squash(const Pages &pages_in) const {
    Pages pages = full_pages(pages_in);
    int n = 0; size_t np = 0, ns = 0;
    for (auto &kv : pages) if (!kv.second.deleted) { np++; ns += kv.second.versions.size(); }
    Bytes t42 = to_t42(pages, n);
    std::string out = path_join(outdir, "clean.t42");
    write_atomic(out, std::string(t42.begin(), t42.end()));
    return fmt("clean.t42: %zu pages, %zu subpages, %d packets (one assembled copy of each subpage) written in ", np, ns, n) + outdir;
}

// ---------------------------------------------------------------- канал и время
std::string Project::service_line() const {
    const Json *sv = extras["_meta"].find("service");
    if (!sv || !sv->is_obj()) return "";
    auto text = [&](const std::string &hex) {
        Row b; b.fill(0x20);
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < 40; i += 2) b[i / 2] = (u8)std::stoi(hex.substr(i, 2), nullptr, 16);
        std::string t = row_text(b, nullptr, 1, table()), o;
        for (char c : t) { if (c == ' ' && (o.empty() || o.back() == ' ')) continue; o += c; }
        return strip(o);
    };
    std::vector<std::string> parts;
    if (sv->has("status")) { std::string s = text((*sv)["status"].str()); if (!s.empty()) parts.push_back(s); }
    if (sv->has("header")) { std::string s = text((*sv)["header"].str()); if (!s.empty()) parts.push_back("header \xC2\xAB" + s + "\xC2\xBB"); }
    if (sv->has("ni")) parts.push_back("network code " + (*sv)["ni"].str());
    if (const Json *u = sv->find("utc"); u && u->is_arr() && u->size() >= 2) {
        std::string a = u->a[0].str(), b = u->a[1].str();
        std::string when = a.substr(0, 16) + (b.substr(0, 10) == a.substr(0, 10) ? "\xE2\x80\x93" + b.substr(11, 5) : " \xE2\x80\x93 " + b.substr(0, 16)) + " UTC";
        if (sv->has("offset_min")) { int m = (int)(*sv)["offset_min"].num(); when += fmt(" (local %+d:%02d)", m / 60, std::abs(m) % 60); }
        parts.push_back("on air " + when);
    }
    std::string out;
    for (auto &p : parts) out += (out.empty() ? "" : "  \xC2\xB7  ") + p;
    return out;
}
