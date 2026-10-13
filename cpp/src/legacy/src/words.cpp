// Восстановление слов. Слово, испорченное при приёме (ошибки в 1–2 битах кода знака, как с ленты: 3↔с, Я↔0,
// б↔а), заменяется словом, которое отличается от него одним-двумя такими знаками и
//   - есть во встроенном словаре языка (русский, немецкий, английский: основы и окончания), или
//   - часто встречается в этой же записи.
// Слово из словаря не меняется никогда; числа и время — тоже. Только текст (не мозаика), ряды 1–24.
#include "project.h"
#include "vbidecode.h"
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <mutex>

namespace {
bool letter(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x400 && c <= 0x4FF);
}
char32_t lower(char32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0x410 && c <= 0x42F) || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
bool upper(char32_t c) { return letter(c) && lower(c) != c; }
bool digit(char32_t c) { return c >= '0' && c <= '9'; }
char32_t norm(char32_t c) { c = lower(c); return c == U'ё' ? U'е' : c; }       // словарь без ё

// ---------------------------------------------------------------- встроенный словарь
struct Dict {
    std::unordered_set<std::u32string> stems, ends, words;     // words — слова как записаны в словаре
    size_t maxend = 0;
    bool empty() const { return stems.empty(); }
    void load(const std::string &text) {
        std::vector<std::u32string> strip;
        for (auto &line0 : split(text, '\n')) {
            std::string line = strip_cr(line0);
            if (line.rfind("#strip:", 0) == 0) { for (auto &w : split(line.substr(7), ' ')) if (!w.empty()) strip.push_back(lw(w)); continue; }
            if (line.rfind("#endings:", 0) == 0) {
                for (auto &w : split(line.substr(9), ' ')) if (!w.empty()) { auto u = lw(w); ends.insert(u); maxend = std::max(maxend, u.size()); }
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            for (auto &w0 : split(line, ' '))
                for (auto &w : split(w0, '-')) {
                    if (w.empty()) continue;
                    auto u = lw(w);
                    stems.insert(u); words.insert(u);
                    for (auto &s : strip) if (u.size() >= s.size() + 2 && u.compare(u.size() - s.size(), s.size(), s) == 0) stems.insert(u.substr(0, u.size() - s.size()));
                }
        }
        ends.insert(U"");
    }
    static std::string strip_cr(const std::string &s) { return !s.empty() && s.back() == '\r' ? s.substr(0, s.size() - 1) : s; }
    static std::u32string lw(const std::string &s) { std::u32string u = to_u32(s); for (auto &c : u) c = norm(c); return u; }
    // слово = основа + окончание (окончание может быть пустым)
    bool known(const std::u32string &w) const {
        if (stems.count(w)) return true;
        for (size_t k = 1; k <= maxend && k + 2 <= w.size(); k++)
            if (ends.count(w.substr(w.size() - k)) && stems.count(w.substr(0, w.size() - k))) return true;
        return false;
    }
};
const Dict *dict_for(const std::string &charset) {
    static Dict ru, lat;
    static std::once_flag once;
    std::call_once(once, [] {
        try { ru.load(resource_text("DICT_RU")); } catch (...) {}
        try { lat.load(resource_text("DICT_DE")); lat.load(resource_text("DICT_EN")); } catch (...) {}
    });
    const Dict *d = charset.rfind("cyr", 0) == 0 ? &ru : &lat;
    return d->empty() ? nullptr : d;
}

struct Tok { int k0, len; };
// слова ряда: буквы и цифры подряд, только в текстовом режиме
std::vector<Tok> tokens(const Row &row, const Charset &t) {
    std::vector<Tok> out;
    bool gfx = false; int st = -1;
    for (int k = 0; k <= 40; k++) {
        bool w = false;
        if (k < 40) {
            int c = row[k] & 0x7F;
            if (c < 0x20) { if (c <= 0x07) gfx = false; else if (c >= 0x10 && c <= 0x17) gfx = true; }
            else if (!gfx) { char32_t u = t[c - 0x20]; w = letter(u) || digit(u); }
        }
        if (w && st < 0) st = k;
        if (!w && st >= 0) { if (k - st >= 3) out.push_back({st, k - st}); st = -1; }
    }
    return out;
}
std::string word_of(const Row &row, const Tok &tk) {
    std::string s(tk.len, ' ');
    for (int i = 0; i < tk.len; i++) s[i] = (char)(row[tk.k0 + i] & 0x7F);
    return s;
}
char32_t U_(const Charset &t, char c) { return t[(u8)c - 0x20]; }
bool all_digits(const std::string &w, const Charset &t) { for (char c : w) if (!digit(U_(t, c))) return false; return true; }
bool all_letters(const std::string &w, const Charset &t) { for (char c : w) if (!letter(U_(t, c))) return false; return true; }
int bitdiff(int a, int b) { return __builtin_popcount((unsigned)(a ^ b)); }
std::u32string key_of(const std::string &w, const Charset &t) { std::u32string u; for (char c : w) u += norm(U_(t, c)); return u; }
}

std::vector<Project::WordFix> Project::restore_words(Pages &pages, std::map<std::string, Page> *before) const {
    // 1) словарь записи: слово -> сколько раз принято (ряд с подтверждением c считается c раз)
    std::unordered_map<std::string, long> freq;
    std::map<std::pair<const Charset *, std::string>, char> distinct;
    for (auto &kv : pages) {
        if (kv.second.deleted) continue;
        const Charset &t = table(&kv.second);
        for (auto &v : kv.second.versions)
            for (auto &rb : v.rows) {
                if (rb.first < 1 || rb.first > 24) continue;
                auto c = v.c.find(rb.first);
                long w = (c != v.c.end() && c->second > 0) ? c->second : 1;
                for (auto &tk : tokens(rb.second, t)) { auto s = word_of(rb.second, tk); freq[s] += w; distinct[{&t, s}] = 1; }
            }
    }
    auto F = [&](const std::string &w) { auto it = freq.find(w); return it == freq.end() ? 0L : it->second; };
    const Dict *D = dict_for(charset);
    const bool cyr = charset.rfind("cyr", 0) == 0;
    // 1б) правдоподобие сочетаний знаков: тройки знаков по разным словам записи (каждое слово — один раз,
    // чтобы частая ошибка ленты не выглядела нормой)
    std::unordered_map<uint32_t, double> n3, n2;
    auto key3 = [](int a, int b, int c) { return (uint32_t)((a << 16) | (b << 8) | c); };
    for (auto &kv : freq) {
        const std::string &w = kv.first;
        int p2 = 1, p1 = 1;                                   // 1 — граница слова
        for (size_t i = 0; i <= w.size(); i++) {
            int c = i < w.size() ? (u8)w[i] : 2;
            n3[key3(p2, p1, c)] += 1; n2[key3(0, p2, p1)] += 1;
            p2 = p1; p1 = c;
        }
    }
    auto plaus = [&](const std::string &w) {
        double s = 0; int p2 = 1, p1 = 1;
        for (size_t i = 0; i <= w.size(); i++) {
            int c = i < w.size() ? (u8)w[i] : 2;
            auto a = n3.find(key3(p2, p1, c)); auto b = n2.find(key3(0, p2, p1));
            s += log(((a == n3.end() ? 0 : a->second) + 0.05) / ((b == n2.end() ? 0 : b->second) + 0.05 * 130));
            p2 = p1; p1 = c;
        }
        return s;
    };
    double P10 = -1e9, P50 = -1e9;
    {
        std::vector<double> a;
        for (auto &kv : freq) if (kv.first.size() >= 3) a.push_back(plaus(kv.first) / (kv.first.size() + 1));
        if (a.size() >= 50) { std::sort(a.begin(), a.end()); P10 = a[a.size() / 10]; P50 = a[a.size() / 2]; }
    }
    // 2) указатель частых слов записи: слово с выброшенными одной или двумя позициями
    std::unordered_map<std::string, std::vector<const std::string *>> idx;
    for (auto &kv : freq) {
        if (kv.second < 3) continue;
        const std::string &w = kv.first; int L = (int)w.size();
        for (int i = 0; i < L; i++) {
            std::string k = w; k[i] = 0; idx[k].push_back(&kv.first);
            if (L >= 5 && L <= 16)
                for (int j = i + 1; j < L; j++) { std::string k2 = k; k2[j] = 0; idx[k2].push_back(&kv.first); }
        }
    }
    // 3) замена для каждого разного слова (параллельно)
    struct Cand { std::string v; int nd, bits; long fv; bool dict, listed; };
    auto best_for = [&](const std::string &w, const Charset &t) -> std::string {
        int L = (int)w.size();
        if (all_digits(w, t)) return "";
        bool hasd = false, hasl = false;
        for (char c : w) { char32_t u = U_(t, c); if (digit(u)) hasd = true; else if (letter(u)) hasl = true; }
        if (!hasl) return "";
        std::u32string kw = key_of(w, t);
        if (D && D->known(kw)) return "";                                  // настоящее слово
        long f = F(w); double pw = plaus(w);
        // цифра как порча: внутри слова (буквы с обеих сторон) или одна цифра в начале перед буквами (3воим, 0олночь);
        // число после слова (Formel1, Top10) — не порча
        bool mixed = false;
        for (int i = 1; i + 1 < L; i++) if (digit(U_(t, w[i])) && letter(U_(t, w[i - 1])) && letter(U_(t, w[i + 1]))) mixed = true;
        if (cyr && L >= 4 && digit(U_(t, w[0])) && letter(U_(t, w[1])) && letter(U_(t, w[2]))) mixed = true;   // кириллица: 3↔з/с, 0↔о
        for (int i = 0; i < L; i++) if (digit(U_(t, w[i])) && i > 0 && digit(U_(t, w[i - 1]))) mixed = false;   // несколько цифр подряд — число
        // заглавные: первая буква — как у слова, остальные — как у большинства букв слова
        int nup = 0, nlet = 0; for (int i = 1; i < L; i++) { char32_t u = U_(t, w[i]); if (letter(u)) { nlet++; nup += upper(u); } }
        bool rest_up = nlet > 0 && nup * 2 > nlet;
        auto want_up = [&](int i) { char32_t u = U_(t, w[i]); if (letter(u)) return upper(u); return i == 0 ? false : rest_up; };
        auto case_ok = [&](const std::string &v) {
            for (int i = 0; i < L; i++) if (v[i] != w[i] && upper(U_(t, v[i])) != want_up(i)) return false;
            return letter(U_(t, w[0])) ? upper(U_(t, v[0])) == upper(U_(t, w[0])) : true;
        };
        std::vector<Cand> cs;
        auto consider = [&](const std::string &v, bool from_dict) {
            if (v == w || !all_letters(v, t) || !case_ok(v)) return;
            int nd = 0, bits = 0;
            for (int i = 0; i < L; i++) if (v[i] != w[i]) { int b = bitdiff((u8)v[i], (u8)w[i]); if (b > 2) return; nd++; bits += b; }
            if (nd == 0 || nd > (L >= 5 ? 2 : 1)) return;
            if (key_of(v, t) == kw) return;                                  // только регистр
            std::u32string kv = key_of(v, t);
            bool dk = D && D->known(kv), ls = D && D->words.count(kv);
            for (auto &c : cs) if (c.v == v) return;
            cs.push_back({v, nd, bits, F(v), dk || from_dict, ls});
        };
        // кандидаты из записи
        auto look = [&](const std::string &k) { auto it = idx.find(k); if (it != idx.end()) for (auto *v : it->second) consider(*v, false); };
        for (int i = 0; i < L; i++) {
            std::string k = w; k[i] = 0; look(k);
            if (L >= 5 && L <= 16) for (int j = i + 1; j < L; j++) { std::string k2 = k; k2[j] = 0; look(k2); }
        }
        // кандидаты из словаря: знаки, отличающиеся 1–2 битами
        if (D && L >= 4 && L <= 20) {
            std::vector<std::vector<int>> alt(L);
            for (int i = 0; i < L; i++)
                for (int c = 0x20; c < 0x80; c++)
                    if (c != (u8)w[i] && bitdiff(c, (u8)w[i]) <= 2 && letter(t[c - 0x20]) && upper(t[c - 0x20]) == want_up(i) && norm(t[c - 0x20]) != norm(U_(t, w[i])))
                        alt[i].push_back(c);
            std::u32string k = kw;
            auto tryk = [&](std::string v) { if (D->known(key_of(v, t))) consider(v, true); };
            for (int i = 0; i < L; i++) for (int c : alt[i]) { std::string v = w; v[i] = (char)c; tryk(v); }
            bool one = false; for (auto &c : cs) if (c.dict && c.nd == 1) one = true;
            if (!one && L >= 5 && L <= 16)
                for (int i = 0; i < L; i++) for (int c : alt[i]) for (int j = i + 1; j < L; j++) for (int c2 : alt[j]) {
                    std::string v = w; v[i] = (char)c; v[j] = (char)c2; tryk(v);
                }
        }
        if (cs.empty()) return "";
        // правило принятия
        // слово, которого нет в словаре, чаще всего просто настоящее редкое слово (имя, термин, другой язык):
        // меняется только слово с признаками порчи — цифра среди букв или неправдоподобное сочетание знаков
        bool suspicious = mixed || (L >= 5 && pw / (L + 1) < P10 && f <= 2);
        if (!suspicious) return "";
        std::vector<Cand> ok;
        for (auto &c : cs) {
            // слово словаря: при цифре в слове — любая форма; иначе — записанное в словаре или встречающееся в записи
            if (c.dict && (mixed || c.listed || c.fv > 0)) ok.push_back(c);
            else if (c.fv >= std::max(3L, 5 * f) && f <= std::max(2L, c.fv / 20) && plaus(c.v) / (L + 1) >= P50 && plaus(c.v) >= pw + 2.0) ok.push_back(c);
        }
        if (ok.empty()) return "";
        // лучший: меньше изменённых знаков, слово словаря, меньше бит, чаще в записи
        std::sort(ok.begin(), ok.end(), [](const Cand &a, const Cand &b) {
            if (a.nd != b.nd) return a.nd < b.nd;
            if (a.dict != b.dict) return a.dict;
            if (a.bits != b.bits) return a.bits < b.bits;
            return a.fv > b.fv;
        });
        if (ok.size() > 1) {
            const Cand &a = ok[0], &b = ok[1];
            if (a.nd == b.nd && a.dict == b.dict && a.bits == b.bits && a.fv < 2 * b.fv + 1) return "";   // не угадываем
        }
        return ok[0].v;
    };
    std::vector<std::pair<const Charset *, std::string>> todo;
    for (auto &d : distinct) todo.push_back(d.first);
    std::vector<std::string> res(todo.size());
    parallel_for(todo.size(), [&](size_t i) { res[i] = best_for(todo[i].second, *todo[i].first); });
    std::map<std::pair<const Charset *, std::string>, std::string> memo;
    for (size_t i = 0; i < todo.size(); i++) if (!res[i].empty()) memo[todo[i]] = res[i];
    // 4) замены в страницах
    std::map<std::string, std::map<std::pair<std::string, std::string>, int>> stat;
    for (auto &kv : pages) {
        Page &pg = kv.second;
        if (pg.deleted) continue;
        const Charset &t = table(&pg);
        bool saved = false;
        for (auto &v : pg.versions)
            for (auto &rb : v.rows) {
                if (rb.first < 1 || rb.first > 24) continue;
                for (auto &tk : tokens(rb.second, t)) {
                    std::string w = word_of(rb.second, tk);
                    auto it = memo.find({&t, w});
                    if (it == memo.end()) continue;
                    if (before && !saved) { (*before)[kv.first] = pg; saved = true; }
                    for (int i = 0; i < tk.len; i++) rb.second[tk.k0 + i] = (u8)it->second[i];
                    stat[kv.first][{w, it->second}]++;
                }
            }
    }
    // отчёт: текст в наборе знаков страницы
    std::vector<WordFix> out;
    for (auto &pk : stat) {
        const Page &pg = pages[pk.first];
        const Charset &t = table(&pg);
        auto txt = [&](const std::string &w) { std::u32string u; for (char c : w) u += U_(t, c); return from_u32(u); };
        for (auto &e : pk.second) out.push_back({pk.first, txt(e.first.first), txt(e.first.second), e.second});
    }
    return out;
}
