#include "tvimeteo.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

// names (or folders) read on several lines: one that differs in a single character from a name read more often
// is that name with a tape error ("UFAA014.5b" -> "UFAA014.ub")
template <class Key>
static std::map<std::string, int> mergeNear(const std::map<std::string, int>& counts, Key key)
{
    std::vector<std::pair<std::string, int>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::map<std::string, int> out;
    for (auto& [name, n] : v) {
        std::string into;
        for (auto& [o, m] : out) {
            if (o.size() != name.size() || key(o) != key(name)) continue;
            int d = 0;
            for (size_t i = 0; i < o.size(); i++) d += o[i] != name[i];
            if (d == 1) { into = o; break; }
        }
        if (into.empty()) out[name] += n; else out[into] += n;
    }
    return out;
}

static bool nameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$' || c == '~';
}

Ft3Catalog ft3Catalog(const std::vector<std::vector<uint8_t>>& slots)
{
    // descriptor body: attributes, 5 bytes, size u32, name length, name (8.3). The body often starts on the
    // line before (not captured): then only the name and its length byte are here, sometimes the size too
    std::map<std::string, std::map<uint32_t, int>> sizes;   // name -> size -> copies
    std::map<std::string, int> names, folders;
    for (auto& s : slots) {
        const int n = std::min<int>((int)s.size(), 40);
        for (int p = 1; p < n; p++) {
            const int nl = s[p - 1];
            if (nl < 3 || nl > 12 || p + nl > n) continue;
            std::string nm((const char*)&s[p], nl);
            int dots = 0, dotAt = -1;
            bool ok = true;
            for (int i = 0; i < nl; i++) {
                if (nm[i] == '.') { dots++; dotAt = i; } else ok &= nameChar(nm[i]);
            }
            if (!ok || dots != 1 || dotAt < 1 || dotAt > 8 || nl - dotAt - 1 > 3) continue;
            names[nm]++;
            if (p >= 5) {
                const uint32_t size = le32(&s[p - 5]);
                if (size && size < (16u << 20)) sizes[nm][size]++;
            }
        }
        // the inserter's folder: "c:\..." up to the first non-path byte
        for (int p = 0; p + 4 <= n; p++) {
            if (!((s[p] | 0x20) >= 'a' && (s[p] | 0x20) <= 'z' && s[p + 1] == ':' && s[p + 2] == '\\')) continue;
            int e = p + 3;
            while (e < n && (nameChar((char)s[e]) || s[e] == '\\' || s[e] == '.')) e++;
            if (e - p >= 5) folders[std::string((const char*)&s[p], e - p)]++;
        }
    }
    // the extension and the stem the names share: a name one character off them is a tape error ("UFAA014.5b")
    std::map<std::string, int> ext, stem;
    for (auto& [nm, c] : names) {
        size_t d = nm.find('.');
        ext[nm.substr(d)] += c;
        stem[nm.substr(0, std::min<size_t>(d, 4))] += c;
    }
    auto fixPart = [](std::string part, const std::map<std::string, int>& votes) {
        int own = votes.count(part) ? votes.at(part) : 0;
        for (auto& [v, c] : votes) {
            if (v.size() != part.size() || c < 3 * std::max(1, own)) continue;
            int d = 0;
            for (size_t i = 0; i < v.size(); i++) d += v[i] != part[i];
            if (d == 1) return v;
        }
        return part;
    };
    std::map<std::string, int> fixedNames;
    std::map<std::string, std::map<uint32_t, int>> fixedSizes;
    for (auto& [nm, c] : names) {
        size_t d = nm.find('.');
        const size_t sl = std::min<size_t>(d, 4);
        std::string f = fixPart(nm.substr(0, sl), stem) + nm.substr(sl, d - sl) + fixPart(nm.substr(d), ext);
        fixedNames[f] += c;
        if (sizes.count(nm)) for (auto& kv : sizes[nm]) fixedSizes[f][kv.first] += kv.second;
    }
    Ft3Catalog c;
    // the digits of a numbered name are not merged ("UFAA014" and "UFAA015" are two files)
    auto merged = mergeNear(fixedNames, [](const std::string& x) { std::string d; for (char ch : x) if (ch >= '0' && ch <= '9') d += ch; return d; });
    for (auto& [nm, cnt] : merged) {
        std::map<uint32_t, int> sz;
        for (auto& [raw, m] : fixedSizes) {
            if (raw.size() != nm.size()) continue;
            int d = 0;
            bool digit = false;
            for (size_t i = 0; i < nm.size(); i++)
                if (raw[i] != nm[i]) { d++; digit |= (raw[i] >= '0' && raw[i] <= '9') || (nm[i] >= '0' && nm[i] <= '9'); }
            if (d == 0 || (d == 1 && !digit)) for (auto& kv : m) sz[kv.first] += kv.second;
        }
        Ft3Catalog::File f;
        f.name = nm; f.copies = cnt;
        int best = 0;
        for (auto& kv : sz) if (kv.second > best) { best = kv.second; f.size = kv.first; }
        c.files.push_back(f);
    }
    // a folder cut short by the end of a line ("c:\fi") is part of a longer one
    auto fm = mergeNear(folders, [](const std::string&) { return 0; });
    for (auto& [f, n] : fm) {
        bool part = false;
        for (auto& [g, m] : fm) part |= g.size() > f.size() && g.compare(0, f.size(), f) == 0;
        if (n >= 2 && !part) c.folders.push_back(f);
    }
    return c;
}

// ---------------------------------------------------------------- texts

// a piece of real text: bulletins are capitals, figures, spaces and / = ; the test text is plain English
static bool texty(const std::string& s)
{
    int good = 0, low = 0, sp = 0;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '/' || c == '=' || c == '\r' || c == '\n' || c == '-' || c == '.' ||
            c == '+' || c == ',' || c == ':')
            good++;
        if (c >= 'a' && c <= 'z') low++;
        sp += c == ' ' || c == '\n';
    }
    const int n = (int)s.size();
    if (!sp && n < 16) return false;
    return good * 10 >= n * 9 || ((good + low) * 10 >= n * 9 && sp * 12 >= n);
}

// a text put together from pieces: at every place the characters read there and how often
struct Cons {
    std::vector<std::vector<std::pair<char, int>>> v;
    int pieces = 1;
    explicit Cons(const std::string& t) { for (char c : t) v.push_back({{c, 1}}); }
    std::string str() const
    {
        std::string s;
        for (auto& p : v) {
            auto b = p[0];
            for (auto& q : p) if (q.second > b.second) b = q;
            s += b.first;
        }
        return s;
    }
    void add(int at, const Cons& o)   // o placed at position at (may run past the end)
    {
        for (size_t i = 0; i < o.v.size(); i++) {
            size_t k = at + i;
            if (k >= v.size()) { v.push_back(o.v[i]); continue; }
            for (auto& q : o.v[i]) {
                bool f = false;
                for (auto& r : v[k]) if (r.first == q.first) { r.second += q.second; f = true; }
                if (!f) v[k].push_back(q);
            }
        }
        pieces += o.pieces;
    }
};

std::vector<std::string> ft3Pieces(const std::vector<std::vector<uint8_t>>& slots)
{
    std::vector<std::string> P;
    std::set<std::string> seen;
    for (auto& s : slots) {
        // byte 40 of a line has only its low 6 bits (the capture window ends before the line does)
        const size_t n = std::min<size_t>(s.size(), 40);
        size_t q = 0;
        while (q < n) {
            size_t e = q;
            while (e < n && (printable(s[e]) || s[e] == '\r' || s[e] == '\n')) e++;
            if (e - q >= 8) {
                std::string t((const char*)&s[q], e - q);
                if (texty(t) && seen.insert(t).second) P.push_back(t);
            }
            q = e + 1;
        }
    }
    return P;
}

std::vector<std::string> ft3JoinTexts(const std::vector<std::string>& pieces)
{
    std::vector<std::string> P;
    std::set<std::string> seen;
    for (auto& t : pieces) if (t.size() >= 8 && seen.insert(t).second) P.push_back(t);
    // pieces of one text overlap: an 8-character run in common places them; the overlap may differ in about
    // a character in 12 (tape errors), every character is then voted between the pieces
    const int G = 8;
    std::vector<Cons> C;
    for (auto& t : P) C.emplace_back(t);
    for (int round = 0; round < 60; round++) {
        std::vector<std::string> S;
        for (auto& c : C) S.push_back(c.str());
        std::unordered_map<std::string, std::vector<std::pair<int, int>>> idx;   // gram at the start of a piece -> piece, place
        for (int i = 0; i < (int)S.size(); i++)
            for (int j = 0; j + G <= (int)S[i].size() && j <= 10; j++) idx[S[i].substr(j, G)].push_back({i, j});
        std::vector<int> order(S.size());
        for (int i = 0; i < (int)S.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return S[a].size() > S[b].size(); });
        std::vector<char> gone(S.size(), 0), touched(S.size(), 0);
        bool changed = false;
        for (int x : order) {
            if (gone[x] || touched[x]) continue;
            const std::string& sx = S[x];
            int by = -1, bs = 0, bl = -1;
            for (int k = 0; k + G <= (int)sx.size(); k++) {
                std::set<char> dv(sx.begin() + k, sx.begin() + k + G);
                if (dv.size() < 4) continue;   // "0000000" or spaces place nothing
                auto it = idx.find(sx.substr(k, G));
                if (it == idx.end()) continue;
                for (auto [y, j] : it->second) {
                    if (y == x || gone[y] || touched[y]) continue;
                    const std::string& sy = S[y];
                    const int s0 = k - j;
                    if (s0 < 0) continue;
                    const int L = std::min((int)sx.size() - s0, (int)sy.size());
                    const bool inside = s0 + (int)sy.size() <= (int)sx.size();
                    if (!inside && L < 14) continue;
                    int mm = 0;
                    for (int i = 0; i < L; i++) mm += sx[s0 + i] != sy[i];
                    if (mm > std::max(1, L / 12)) continue;
                    if (L > bl) { bl = L; by = y; bs = s0; }
                }
            }
            if (by < 0) continue;
            C[x].add(bs, C[by]);
            gone[by] = 1;
            touched[x] = 1;
            changed = true;
        }
        if (!changed) break;
        std::vector<Cons> keep;
        for (int i = 0; i < (int)C.size(); i++) if (!gone[i]) keep.push_back(std::move(C[i]));
        C.swap(keep);
    }
    std::vector<std::string> out;
    for (auto& c : C) out.push_back(c.str());
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    return out;
}

// ---------------------------------------------------------------- METAR in Russian

static const std::pair<const char*, const char*> kIcaoPrefix[] = {
    {"EG", "Великобритания"}, {"EI", "Ирландия"}, {"EN", "Норвегия"}, {"ES", "Швеция"}, {"EF", "Финляндия"}, {"EK", "Дания"},
    {"ED", "Германия"}, {"ET", "Германия"}, {"EH", "Нидерланды"}, {"EB", "Бельгия"}, {"EL", "Люксембург"}, {"EP", "Польша"},
    {"EE", "Эстония"}, {"EV", "Латвия"}, {"EY", "Литва"}, {"BI", "Исландия"}, {"LF", "Франция"}, {"LE", "Испания"},
    {"LP", "Португалия"}, {"LI", "Италия"}, {"LM", "Мальта"}, {"LS", "Швейцария"}, {"LO", "Австрия"}, {"LK", "Чехия"},
    {"LZ", "Словакия"}, {"LH", "Венгрия"}, {"LJ", "Словения"}, {"LD", "Хорватия"}, {"LY", "Сербия"}, {"LQ", "Босния"},
    {"LW", "Македония"}, {"LA", "Албания"}, {"LG", "Греция"}, {"LB", "Болгария"}, {"LR", "Румыния"}, {"LU", "Молдавия"},
    {"LT", "Турция"}, {"LC", "Кипр"}, {"LL", "Израиль"}, {"OL", "Ливан"}, {"OS", "Сирия"}, {"OJ", "Иордания"}, {"OE", "Саудовская Аравия"},
    {"UU", "Россия"}, {"UL", "Россия"}, {"UR", "Россия"}, {"UW", "Россия"}, {"US", "Россия"}, {"UN", "Россия"}, {"UH", "Россия"},
    {"UI", "Россия"}, {"UE", "Россия"}, {"UM", "Белоруссия"}, {"UK", "Украина"}, {"UG", "Грузия"}, {"UB", "Азербайджан"},
    {"UD", "Армения"}, {"UA", "Казахстан"}, {"UT", "Средняя Азия"}, {"GM", "Марокко"}, {"DA", "Алжир"}, {"DT", "Тунис"},
    {"HE", "Египет"}, {"K", "США"}, {"C", "Канада"}, {"P", "Тихий океан (США)"}, {"Y", "Австралия"}, {"Z", "Китай"}, {"R", "Япония/Корея"},
};

static std::string country(const std::string& icao)
{
    for (auto& p : kIcaoPrefix) if (icao.rfind(p.first, 0) == 0) return p.second;
    return "";
}

static std::vector<std::string> tokens(const std::string& s)
{
    std::vector<std::string> t;
    std::string w;
    for (char c : s) {
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { if (!w.empty()) t.push_back(w), w.clear(); }
        else w += c;
    }
    if (!w.empty()) t.push_back(w);
    return t;
}

static bool digits(const std::string& s, size_t a, size_t n)
{
    if (a + n > s.size()) return false;
    for (size_t i = a; i < a + n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static int num(const std::string& s, size_t a, size_t n) { return atoi(s.substr(a, n).c_str()); }

static std::string compass(int d)
{
    static const char* r[] = {"северный", "северо-восточный", "восточный", "юго-восточный", "южный", "юго-западный", "западный", "северо-западный"};
    return r[((d + 22) % 360) / 45];
}

static const std::pair<const char*, const char*> kWx[] = {
    {"TS", "гроза"}, {"SH", "ливневый"}, {"FZ", "переохлаждённый"}, {"BL", "низовая метель / буря"}, {"DR", "позёмок"}, {"MI", "низкий"},
    {"BC", "клочьями"}, {"PR", "частичный"}, {"DZ", "морось"}, {"RA", "дождь"}, {"SN", "снег"}, {"SG", "снежные зёрна"}, {"PL", "ледяная крупа"},
    {"GR", "град"}, {"GS", "мелкий град"}, {"IC", "ледяные иглы"}, {"UP", "осадки"}, {"BR", "дымка"}, {"FG", "туман"}, {"FU", "дым"},
    {"VA", "вулканический пепел"}, {"DU", "пыль"}, {"SA", "песок"}, {"HZ", "мгла"}, {"PO", "пыльные вихри"}, {"SQ", "шквал"},
    {"FC", "смерч"}, {"SS", "песчаная буря"}, {"DS", "пыльная буря"},
};

static std::string weather(const std::string& t)
{
    std::string s = t, out;
    if (s.empty()) return "";
    if (s[0] == '-') out = "слабый ", s = s.substr(1);
    else if (s[0] == '+') out = "сильный ", s = s.substr(1);
    if (s.rfind("VC", 0) == 0) out += "поблизости ", s = s.substr(2);
    if (s.empty() || s.size() % 2) return "";
    std::string parts;
    for (size_t i = 0; i < s.size(); i += 2) {
        const char* w = nullptr;
        for (auto& p : kWx) if (s.compare(i, 2, p.first) == 0) w = p.second;
        if (!w) return "";
        if (!parts.empty()) parts += " ";
        parts += w;
    }
    return out + parts;
}

std::string metarRu(const std::string& report)
{
    std::vector<std::string> t = tokens(report);
    size_t i = 0;
    std::string kind = "сводка METAR";
    // a tape error turns E into J in "METAR" (MJTAR) - the word is known by its place
    if (i < t.size() && t[i].size() == 5 && t[i][0] == 'M' && t[i].compare(2, 3, "TAR") == 0) i++;
    else if (i < t.size() && t[i] == "SPECI") { kind = "внеочередная сводка SPECI"; i++; }
    if (i >= t.size() || t[i].size() != 4) return "";
    for (char c : t[i]) if (c < 'A' || c > 'Z') return "";
    const std::string icao = t[i++];
    std::vector<std::string> o;
    std::string head = "Аэродром " + icao;
    const std::string cn = country(icao);
    if (!cn.empty()) head += " (" + cn + ")";
    if (i < t.size() && t[i].size() == 7 && t[i][6] == 'Z' && digits(t[i], 0, 6)) {
        char b[96];
        snprintf(b, sizeof b, ", %s за %d-е число %02d:%02d UTC", kind.c_str(), num(t[i], 0, 2), num(t[i], 2, 2), num(t[i], 4, 2));
        head += b;
        i++;
    } else return "";
    if (i < t.size() && (t[i] == "NIL" || t[i] == "NIL=")) return head + ": сводки нет.";
    if (i < t.size() && t[i] == "AUTO") { o.push_back("автоматическая станция"); i++; }
    if (i < t.size() && t[i] == "COR") { o.push_back("исправленная"); i++; }
    for (; i < t.size(); i++) {
        std::string w = t[i];
        while (!w.empty() && w.back() == '=') w.pop_back();
        if (w.empty()) continue;
        char b[160];
        // wind dddff(Ggg)KT / MPS
        size_t u = w.find("KT"), um = w.find("MPS");
        if ((u != std::string::npos && u + 2 == w.size()) || (um != std::string::npos && um + 3 == w.size())) {
            const bool mps = um != std::string::npos;
            const std::string core = w.substr(0, mps ? um : u);
            const char* unit = mps ? "м/с" : "уз";
            if (core.size() >= 5 && (digits(core, 0, 3) || core.compare(0, 3, "VRB") == 0) && digits(core, 3, 2)) {
                int ff = num(core, 3, 2), gust = -1;
                size_t g = core.find('G');
                if (g != std::string::npos && digits(core, g + 1, 2)) gust = num(core, g + 1, 2);
                std::string s;
                if (core.compare(0, 5, "00000") == 0) s = "штиль";
                else if (core.compare(0, 3, "VRB") == 0) { snprintf(b, sizeof b, "ветер переменный, %d %s", ff, unit); s = b; }
                else {
                    int dd = num(core, 0, 3);
                    snprintf(b, sizeof b, "ветер %s (%d°), %d %s", compass(dd).c_str(), dd, ff, unit);
                    s = b;
                    if (!mps) { snprintf(b, sizeof b, " (%.0f м/с)", ff * 0.514); s += b; }
                }
                if (gust > 0) { snprintf(b, sizeof b, ", порывы до %d %s", gust, unit); s += b; }
                o.push_back(s);
                continue;
            }
        }
        if (w.size() == 7 && w[3] == 'V' && digits(w, 0, 3) && digits(w, 4, 3)) {
            snprintf(b, sizeof b, "направление меняется от %d° до %d°", num(w, 0, 3), num(w, 4, 3));
            o.push_back(b);
            continue;
        }
        if (w == "CAVOK") { o.push_back("видимость 10 км и более, облаков ниже 1500 м нет, явлений нет"); continue; }
        if (w.size() == 4 && digits(w, 0, 4)) {
            int v = num(w, 0, 4);
            if (v == 9999) o.push_back("видимость 10 км и более");
            else { snprintf(b, sizeof b, v >= 5000 ? "видимость %.0f км" : "видимость %.1f км", v / 1000.0); o.push_back(b); }
            continue;
        }
        if (w == "SKC" || w == "NSC" || w == "CLR" || w == "NCD") { o.push_back("без облаков"); continue; }
        if (w.size() >= 6 && digits(w, 3, 3) && (w.compare(0, 3, "FEW") == 0 || w.compare(0, 3, "SCT") == 0 || w.compare(0, 3, "BKN") == 0 ||
                                                  w.compare(0, 3, "OVC") == 0)) {
            const char* amt = w[0] == 'F' ? "небольшая облачность" : w[0] == 'S' ? "рассеянная облачность" : w[0] == 'B' ? "значительная облачность" : "сплошная облачность";
            snprintf(b, sizeof b, "%s на %d м", amt, num(w, 3, 3) * 30);
            std::string s = b;
            if (w.size() > 6) s += w.compare(6, 2, "CB") == 0 ? ", кучево-дождевая" : w.compare(6, 3, "TCU") == 0 ? ", мощная кучевая" : "";
            o.push_back(s);
            continue;
        }
        if (w.size() == 5 && w.compare(0, 2, "VV") == 0 && digits(w, 2, 3)) {
            snprintf(b, sizeof b, "вертикальная видимость %d м", num(w, 2, 3) * 30);
            o.push_back(b);
            continue;
        }
        // temperature / dew point TT/TdTd (M = minus); the / sometimes comes broken by the tape (12W06)
        {
            size_t a = 0;
            bool m1 = false;
            if (a < w.size() && w[a] == 'M') m1 = true, a++;
            if (digits(w, a, 2) && a + 2 < w.size()) {
                size_t sl = a + 2, c = sl + 1;
                bool m2 = false;
                if (c < w.size() && w[c] == 'M') m2 = true, c++;
                if (w[sl] != 'M' && !(w[sl] >= '0' && w[sl] <= '9') && digits(w, c, 2) && c + 2 == w.size()) {
                    int tt = num(w, a, 2) * (m1 ? -1 : 1), td = num(w, c, 2) * (m2 ? -1 : 1);
                    snprintf(b, sizeof b, "температура %+d °C, точка росы %+d °C", tt, td);
                    o.push_back(b);
                    continue;
                }
            }
        }
        if (w.size() == 5 && (w[0] == 'Q' || w[0] == 'A') && digits(w, 1, 4)) {
            if (w[0] == 'Q') snprintf(b, sizeof b, "давление %d гПа (%.0f мм рт. ст.)", num(w, 1, 4), num(w, 1, 4) * 0.750062);
            else snprintf(b, sizeof b, "давление %.2f дюйма рт. ст.", num(w, 1, 4) / 100.0);
            o.push_back(b);
            continue;
        }
        if (w == "NOSIG") { o.push_back("существенных изменений не ожидается"); continue; }
        if (w == "TEMPO" || w == "BECMG") {
            // the trend as it is sent
            std::string rest;
            for (size_t k = i; k < t.size(); k++) rest += (rest.empty() ? "" : " ") + t[k];
            o.push_back(std::string(w == "TEMPO" ? "временами: " : "ожидается: ") + rest.substr(6));
            break;
        }
        if (w == "RMK") break;
        const std::string wx = weather(w);
        if (!wx.empty()) { o.push_back(wx); continue; }
    }
    std::string s = head + ": ";
    for (size_t k = 0; k < o.size(); k++) s += (k ? "; " : "") + o[k];
    return o.empty() ? "" : s + ".";
}

// ---------------------------------------------------------------- bulletins

static bool headingAt(const std::string& s, size_t k, std::string& h, size_t& end)
{
    auto up = [&](size_t i) { return i < s.size() && s[i] >= 'A' && s[i] <= 'Z'; };
    auto dg = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
    if (!(up(k) && up(k + 1) && up(k + 2) && up(k + 3) && dg(k + 4) && dg(k + 5) && k + 6 < s.size() && s[k + 6] == ' ')) return false;
    size_t c = k + 7;
    if (!(up(c) && up(c + 1) && up(c + 2) && up(c + 3) && c + 4 < s.size() && s[c + 4] == ' ')) return false;
    size_t t = c + 5;
    for (int z = 0; z < 6; z++) if (!dg(t + z)) return false;
    if (k > 0 && (up(k - 1) || dg(k - 1))) return false;
    h = s.substr(k, 18);
    end = t + 6;
    return true;
}

std::string ft3MeteoText(const std::vector<std::string>& texts, int* reports)
{
    static const std::pair<const char*, const char*> kTT[] = {
        {"SA", "METAR (фактическая погода на аэродромах)"}, {"SP", "SPECI (внеочередные сводки аэродромов)"},
        {"SM", "SYNOP (синоптические сводки, основные сроки)"}, {"SI", "SYNOP (промежуточные сроки)"}, {"SN", "SYNOP (несрочные сроки)"},
        {"SS", "сводки с морских станций"}, {"FC", "TAF (прогнозы по аэродромам, до 12 ч)"}, {"FT", "TAF (прогнозы по аэродромам, до 24 ч)"},
        {"UA", "AIREP (донесения с бортов самолётов)"}, {"US", "TEMP (аэрологические данные)"}, {"UP", "PILOT (ветер на высотах)"},
        {"UD", "AMDAR (данные с самолётов)"}, {"FQ", "прогноз для моря"}, {"WW", "штормовые предупреждения"},
    };
    std::map<std::string, std::string> byHeading;   // heading -> text (the longest)
    std::vector<std::string> other;
    for (auto& tx : texts) {
        std::vector<std::pair<size_t, std::string>> hs;
        for (size_t k = 0; k < tx.size(); k++) {
            std::string h; size_t e;
            if (headingAt(tx, k, h, e)) hs.push_back({k, h});
        }
        if (hs.empty()) { other.push_back(tx); continue; }
        for (size_t j = 0; j < hs.size(); j++) {
            size_t a = hs[j].first, b = j + 1 < hs.size() ? hs[j + 1].first : tx.size();
            std::string body = tx.substr(a, b - a);
            auto& cur = byHeading[hs[j].second];
            if (body.size() > cur.size()) cur = body;
        }
        if (hs[0].first > 20) other.push_back(tx.substr(0, hs[0].first));
    }
    int nrep = 0;
    std::string out;
    auto decodeReports = [&](const std::string& body) {
        std::string res;
        // reports end with '=': each one, from its station code (after the heading line)
        size_t p = 0;
        while (p < body.size()) {
            size_t e = body.find('=', p);
            std::string r = body.substr(p, e == std::string::npos ? std::string::npos : e - p + 1);
            p = e == std::string::npos ? body.size() : e + 1;
            for (auto& c : r) if (c == '\r' || c == '\n') c = ' ';
            // the station code of a report: a word of 4 capitals followed by its time ddhhmmZ (or NIL)
            std::vector<std::string> t = tokens(r);
            for (size_t k = 0; k + 1 < t.size(); k++) {
                bool st = t[k].size() == 4;
                for (char c : t[k]) st &= c >= 'A' && c <= 'Z';
                if (!st) continue;
                std::string rest;
                for (size_t z = k; z < t.size(); z++) rest += (rest.empty() ? "" : " ") + t[z];
                const std::string ru = metarRu(rest);
                if (!ru.empty()) { res += "    " + rest + "\r\n      " + ru + "\r\n"; nrep++; }
                break;
            }
        }
        return res;
    };
    for (auto& [h, body] : byHeading) {
        std::string what;
        for (auto& p : kTT) if (h.compare(0, 2, p.first) == 0) what = p.second;
        out += h + (what.empty() ? "" : "   " + what) + "\r\n";
        std::string b = body.substr(std::min(body.size(), (size_t)18));
        for (size_t i = 0; i < b.size(); i++) if (b[i] == '\n' && (i == 0 || b[i - 1] != '\r')) b.insert(i++, "\r");
        while (!b.empty() && (b[0] == '\r' || b[0] == '\n' || b[0] == ' ')) b.erase(b.begin());
        out += b + "\r\n";
        if (h.compare(0, 2, "SA") == 0 || h.compare(0, 2, "SP") == 0) {
            const std::string d = decodeReports(body.substr(18));
            if (!d.empty()) out += "  по-русски:\r\n" + d;
        }
        out += "\r\n";
    }
    // METAR reports outside a heading (the heading went on a line not captured)
    std::string loose;
    for (auto& tx : other) {
        if (tx.find("Z ") == std::string::npos) continue;
        loose += decodeReports(tx);
    }
    if (!loose.empty()) out += "Сводки METAR без заголовка бюллетеня (заголовок пришёл на непойманной строке):\r\n" + loose + "\r\n";
    if (reports) *reports = nrep;
    return out;
}
