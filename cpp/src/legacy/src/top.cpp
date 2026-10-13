// TOP (навигация телетекста): BTT на странице 1F0 — тип каждой из 800 страниц, ссылки на MPT (число подстраниц)
// и AIT (заголовки страниц, 12 символов). Плюс выгрузка служебных шестнадцатеричных страниц (xFF, x?E и т. п.).
#include "top.h"
#include "teletext.h"
#include <array>
#include <map>

namespace {
struct HexPage { std::map<int, std::vector<std::array<u8, 40>>> rows; int nat = 0; int headers = 0; };

// голосование по копиям строки
std::array<u8, 40> vote(const std::vector<std::array<u8, 40>> &v) {
    std::array<u8, 40> o{};
    for (int k = 0; k < 40; k++) {
        std::map<u8, int> c; for (auto &r : v) c[r[k]]++;
        int b = -1; for (auto &kv : c) if (kv.second > b) { b = kv.second; o[k] = kv.first; }
    }
    return o;
}
int hx(u8 b) { return ham::fix[b]; }

// национальные наборы G0 (позиции 23 24 40 5B 5C 5D 5E 5F 60 7B 7C 7D 7E)
const char *NAT[8][13] = {
    {"£", "$", "@", "←", "½", "→", "↑", "#", "—", "¼", "‖", "¾", "÷"},       // английский
    {"#", "$", "§", "Ä", "Ö", "Ü", "^", "_", "°", "ä", "ö", "ü", "ß"},       // немецкий
    {"#", "¤", "É", "Ä", "Ö", "Å", "Ü", "_", "é", "ä", "ö", "å", "ü"},       // шведский / финский
    {"£", "$", "é", "°", "ç", "→", "↑", "#", "ù", "à", "ò", "è", "ì"},       // итальянский
    {"é", "ï", "à", "ë", "ê", "ù", "î", "#", "è", "â", "ô", "û", "ç"},       // французский
    {"ç", "$", "¡", "á", "é", "í", "ó", "ú", "¿", "ü", "ñ", "è", "à"},       // португальский / испанский
    {"#", "ů", "č", "ť", "ž", "ý", "í", "ř", "é", "á", "ě", "ú", "š"},       // чешский / словацкий
    {"#", "$", "@", "[", "\\", "]", "^", "_", "`", "{", "|", "}", "~"}};
std::string text(const u8 *b, int n, int nat) {
    static const u8 POS[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};
    std::string s;
    for (int i = 0; i < n; i++) {
        u8 c = b[i] & 0x7F;
        if (c < 0x20) { s += ' '; continue; }
        int k = -1; for (int j = 0; j < 13; j++) if (POS[j] == c) k = j;
        if (k >= 0) s += NAT[nat & 7][k]; else s += (char)c;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
}

int top_report(const std::string &t42, const std::string &out_txt, std::string &summary) {
    MappedFile mf(t42);
    size_t n = mf.size() / 42;
    std::map<std::string, HexPage> pg;               // "1F0" -> строки
    std::string cur[9];
    for (size_t i = 0; i < n; i++) {
        const u8 *p = mf.data() + i * 42;
        int a = hx(p[0]), b = hx(p[1]); if (a < 0 || b < 0) continue;
        int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
        if (row == 0) {
            int u = hx(p[2]), t = hx(p[3]);
            if (u < 0 || t < 0) { cur[mag].clear(); continue; }
            cur[mag] = fmt("%d%X%X", mag, t, u);
            if (t >= 10 || u >= 10) {                // только шестнадцатеричные страницы
                auto &H = pg[cur[mag]]; H.headers++;
                int c = hx(p[9]); if (c >= 0) H.nat = ((c >> 1) & 1) << 2 | ((c >> 2) & 1) << 1 | ((c >> 3) & 1);   // C12 C13 C14
                std::array<u8, 40> r; memcpy(r.data(), p + 2, 40); H.rows[0].push_back(r);
            }
        } else if (row <= 25 && !cur[mag].empty() && pg.count(cur[mag])) {
            std::array<u8, 40> r; memcpy(r.data(), p + 2, 40); pg[cur[mag]].rows[row].push_back(r);
        }
    }
    std::string txt;
    int titled = 0;
    auto it = pg.find("1F0");
    if (it != pg.end() && it->second.rows.count(1) && it->second.rows.count(20)) {
        HexPage &B = it->second;
        std::vector<int> type(800, -1);
        for (int r = 1; r <= 20; r++) {
            if (!B.rows.count(r)) continue;
            auto v = vote(B.rows[r]);
            for (int k = 0; k < 40; k++) type[(r - 1) * 40 + k] = hx(v[k]);
        }
        // ссылки на MPT / AIT в строках 21–22: по 8 полубайт (магазин, десятки, единицы, 4 подкода, тип)
        std::vector<std::pair<std::string, int>> links;
        for (int r = 21; r <= 22; r++) {
            if (!B.rows.count(r)) continue;
            auto v = vote(B.rows[r]);
            for (int e = 0; e < 5; e++) {
                int m = hx(v[e * 8]), t = hx(v[e * 8 + 1]), u = hx(v[e * 8 + 2]), ty = hx(v[e * 8 + 7]);
                if (m < 1 || m > 8 || t < 0 || u < 0 || ty < 1) continue;
                links.push_back({fmt("%d%X%X", m == 8 ? 8 : m, t, u), ty});
            }
        }
        std::map<int, int> subs;                       // страница -> число подстраниц (MPT)
        std::map<int, std::string> title;              // страница -> заголовок (AIT)
        int nat = 0;
        for (auto &L : links) {
            auto q = pg.find(L.first); if (q == pg.end()) continue;
            HexPage &P = q->second;
            if (L.second == 1) {                       // MPT
                for (int r = 1; r <= 20; r++) {
                    if (!P.rows.count(r)) continue;
                    auto v = vote(P.rows[r]);
                    for (int k = 0; k < 40; k++) { int s = hx(v[k]); if (s > 0) subs[100 + (r - 1) * 40 + k] = s; }
                }
            } else if (L.second == 2) {                // AIT
                nat = P.nat;
                for (int r = 1; r <= 22; r++) {
                    if (!P.rows.count(r)) continue;
                    auto v = vote(P.rows[r]);
                    for (int e = 0; e < 2; e++) {
                        const u8 *E = v.data() + e * 20;
                        int h = hx(E[0]), t = hx(E[1]), u = hx(E[2]);
                        if (h < 1 || h > 8 || t < 0 || t > 9 || u < 0 || u > 9) continue;
                        std::string s = text(E + 8, 12, P.nat);
                        if (s.empty()) continue;
                        title[h * 100 + t * 10 + u] = s;
                    }
                }
            }
        }
        static const char *TN[16] = {"", "subtitles", "programme index", "programme index (several)", "section", "section (several)",
                                     "group", "group (several)", "page", "page (several)", "page", "page (several)", "page", "page (several)", "?", "?"};
        int inuse = 0; for (int t : type) inuse += t > 0;
        txt += "TOP navigation tables (BTT page 1F0";
        for (auto &L : links) txt += ", " + std::string(L.second == 1 ? "MPT " : L.second == 2 ? "AIT " : "table ") + L.first;
        txt += fmt(")\n%d pages in transmission, %zu titles\n\n", inuse, title.size());
        txt += "page  kind                       subpages  title\n";
        for (int i = 0; i < 800; i++) {
            int t = type[i]; if (t <= 0) continue;
            int pnum = 100 + i;
            bool head = t >= 2 && t <= 5;
            txt += fmt("%s%3d   %-26s %-9s %s\n", head ? "\n" : "", pnum, TN[t & 15], subs.count(pnum) ? std::to_string(subs[pnum]).c_str() : "",
                       title.count(pnum) ? title[pnum].c_str() : "");
        }
        titled = (int)title.size();
        summary = fmt("TOP navigation: %d pages, %d titles", inuse, titled);
        (void)nat;
    }
    // прочие шестнадцатеричные страницы (служебные потоки: 1EE, xFE, xFF …)
    std::string other;
    for (auto &kv : pg) {
        if (kv.first == "1F0") continue;
        bool linked = txt.find(kv.first) != std::string::npos;
        if (linked) continue;
        int nrows = 0; for (auto &r : kv.second.rows) if (r.first > 0) nrows++;
        if (!nrows) continue;
        other += fmt("\nPage %s: %d headers, rows", kv.first.c_str(), kv.second.headers);
        for (auto &r : kv.second.rows) if (r.first > 0) other += fmt(" %d(%zu)", r.first, r.second.size());
        other += "\n";
        for (auto &r : kv.second.rows) {
            if (r.first == 0 || r.second.empty()) continue;
            auto v = vote(r.second);
            std::string h; for (u8 c : v) h += fmt("%02X", c);
            other += fmt("  row %2d  %s\n", r.first, h.c_str());
        }
    }
    if (!other.empty()) txt += "\nOther hexadecimal (non-display) pages: data for decoders, not meant for viewing\n(most common copy of each row, raw bytes)\n" + other;
    if (txt.empty()) return 0;
    write_text(out_txt, txt);
    if (summary.empty()) summary = "hexadecimal service pages";
    return titled > 0 ? titled : 1;
}
