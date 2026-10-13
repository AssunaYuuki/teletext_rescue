#include "cc608.h"
#include "teletext.h"

namespace {
// особые знаки: 0x11/0x19, второй байт 0x30..0x3F
const char32_t SPECIAL[16] = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A, 0xE0, ' ', 0xE8, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB};
// расширенные знаки (заменяют предыдущий знак): 0x12/0x1A и 0x13/0x1B, второй байт 0x20..0x3F
const char32_t EXT1[32] = {0xC1, 0xC9, 0xD3, 0xDA, 0xDC, 0xFC, 0x2018, 0xA1, '*', 0x2019, 0x2014, 0xA9, 0x2120, 0x2022, 0x201C, 0x201D,
                           0xC0, 0xC2, 0xC7, 0xC8, 0xCA, 0xCB, 0xEB, 0xCE, 0xCF, 0xEF, 0xD4, 0xD9, 0xF9, 0xDB, 0xAB, 0xBB};
const char32_t EXT2[32] = {0xC3, 0xE3, 0xCD, 0xCC, 0xEC, 0xD2, 0xF2, 0xD5, 0xF5, '{', '}', '\\', '^', '_', '|', '~',
                           0xC4, 0xE4, 0xD6, 0xF6, 0xDF, 0xA5, 0xA4, 0x2502, 0xC5, 0xE5, 0xD8, 0xF8, 0x250C, 0x2510, 0x2514, 0x2518};
// строка PAC: (первый байт & 7, бит 0x20 второго байта) -> строка 1..15
const int PAC_ROW[8][2] = {{11, 11}, {1, 2}, {3, 4}, {12, 13}, {14, 15}, {5, 6}, {7, 8}, {9, 10}};

char32_t basic(int c) {
    if (c == 0x27) return 0x2019;
    char32_t s = cc_special(c);
    return s ? s : (char32_t)c;
}

std::string tstamp(double s) { return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)); }
std::string srt_time(double s) {
    long ms = lround(s * 1000);
    return fmt("%02ld:%02ld:%02ld,%03ld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

enum Mode { NONE, POPON, ROLLUP, PAINTON };

struct Service {                                  // служба подписей (CCn)
    std::string name;
    Mode mode = NONE;
    Cc608Screen disp, nond;
    int row = 14, col = 0, base = 14, ru = 2; uint8_t attr = 0;
    bool changed = false, structural = false, text = false;
    int last_shot = -1000000;
    Cc608Screen shot_screen;                      // что было в последнем снимке
    Json shots = Json::array();
    std::vector<std::pair<int, std::string>> cues;   // структурные снимки: место, текст экрана
    std::vector<std::pair<int, std::string>> lines;  // расшифровка
    Cc608Screen &target() { return mode == POPON ? nond : disp; }
};

std::string screen_text(const Cc608Screen &s, const char *sep) {
    std::string t;
    for (int r = 0; r < 15; r++) { std::string x = strip(s.row_text(r)); if (x.empty()) continue; if (!t.empty()) t += sep; t += x; }
    return t;
}

Json screen_json(const Cc608Screen &s) {
    Json rows = Json::array();
    for (int r = 0; r < 15; r++) {
        int a = -1, b = -1;
        for (int k = 0; k < 32; k++) if (s.m[r][k].c) { if (a < 0) a = k; b = k; }
        if (a < 0) continue;
        std::string t, at;
        for (int k = a; k <= b; k++) {
            const auto &c = s.m[r][k];
            t += c.c ? from_cp(c.c) : " ";
            at += fmt("%02x", c.c ? c.a : 0x40);          // 0x40 — пустая клетка (прозрачная)
        }
        Json e = Json::array(); e.push((double)r); e.push((double)a); e.push(t); e.push(at); rows.push(e);
    }
    return rows;
}
}

std::string Cc608Screen::row_text(int r) const {
    std::string t;
    for (int k = 0; k < 32; k++) t += m[r][k].c ? from_cp(m[r][k].c) : " ";
    return rstrip(t);
}

bool cc608_looks_field2(const std::vector<std::array<int, 3>> &pairs) {
    int xds = 0, f2 = 0, f1 = 0;
    for (auto &p : pairs) {
        if (p[0] >= 0x01 && p[0] <= 0x0F) xds++;
        if ((p[0] == 0x15 || p[0] == 0x1D) && p[1] >= 0x20 && p[1] <= 0x2F) f2++;
        if ((p[0] == 0x14 || p[0] == 0x1C) && p[1] >= 0x20 && p[1] <= 0x2F) f1++;
    }
    return xds > 20 || f2 > f1;
}

Cc608Result cc608_decode(const std::vector<std::array<int, 3>> &pairs, bool field2, double rate) {
    // службы: [канал 0/1][подписи 0 / текст 1]; текст — свой экран 15×32, строки сдвигаются вверх по CR
    Service sv[2][2];
    for (int i = 0; i < 2; i++) {
        sv[i][0].name = fmt("CC%d", i + 1 + (field2 ? 2 : 0));
        sv[i][1].name = fmt("T%d", i + 1 + (field2 ? 2 : 0));
        sv[i][1].text = true; sv[i][1].mode = ROLLUP; sv[i][1].ru = 15; sv[i][1].base = 14; sv[i][1].row = 14;
    }
    int ch = 0;                  // текущий канал данных (0/1)
    bool text[2] = {false, false};
    bool xds = false;
    int p1 = -1, p2 = -1;        // предыдущая пара (повтор управляющего кода не исполняется)
    int u = 0;

    auto shot = [&](Service &s, bool force) {
        if (!s.changed && !force) return;
        if (!(s.structural || force || u - s.last_shot >= 6)) return;
        if (s.disp == s.shot_screen && !force) { s.changed = false; s.structural = false; return; }
        Json e = Json::array(); e.push((double)u); e.push(screen_json(s.disp)); s.shots.push(e);
        if (s.structural || force) s.cues.push_back({u, screen_text(s.disp, "\n")});
        s.shot_screen = s.disp; s.last_shot = u; s.changed = false; s.structural = false;
    };
    auto cur = [&]() -> Service & { return sv[ch][text[ch] ? 1 : 0]; };
    auto put = [&](char32_t c) {
        Service &s = cur();
        if (s.mode == NONE) return;
        Cc608Screen &T = s.target();
        if (s.col > 31) s.col = 31;
        T.m[s.row][s.col] = {c, s.attr};
        s.col++;
        if (&T == &s.disp) s.changed = true;
    };
    auto rollup_cr = [&](Service &s) {
        std::string done = strip(s.disp.row_text(s.base));
        if (!done.empty()) s.lines.push_back({u, done});
        int top = std::max(0, s.base - s.ru + 1);
        for (int r = top; r < s.base; r++) for (int k = 0; k < 32; k++) s.disp.m[r][k] = s.disp.m[r + 1][k];
        for (int k = 0; k < 32; k++) s.disp.m[s.base][k] = Cc608Cell();
        for (int r = 0; r < top; r++) for (int k = 0; k < 32; k++) s.disp.m[r][k] = Cc608Cell();   // вне окна — пусто
        s.col = 0; s.row = s.base; s.changed = true; s.structural = true;
    };
    auto all_shots = [&](bool force) { for (auto &a : sv) for (auto &s : a) shot(s, force); };

    for (auto &pc : pairs) {
        int c1 = pc[0], c2 = pc[1]; u = pc[2];
        if (getenv("TR_CCDUMP") && (c1 || c2)) fprintf(stderr, "%d %02X %02X %c%c\n", u, c1, c2, c1 >= 0x20 ? c1 : '.', c2 >= 0x20 ? c2 : '.');
        if (c1 == 0 && c2 == 0) { all_shots(false); continue; }
        // XDS (только поле 2): 0x01..0x0E начало/продолжение, 0x0F конец — мимо подписей
        if (c1 >= 0x01 && c1 <= 0x0F) { xds = c1 != 0x0F; continue; }
        if (c1 >= 0x10 && c1 <= 0x1F) {
            xds = false;
            if (c1 == p1 && c2 == p2) { p1 = p2 = -1; continue; }          // повтор управляющего кода
            p1 = c1; p2 = c2;
            ch = (c1 & 0x08) ? 1 : 0;
            int c = c1 & 0x07;
            if (c2 >= 0x40) {                                               // PAC: строка, отступ / цвет
                Service &s = cur();
                int row = PAC_ROW[c][(c2 >> 5) & 1] - 1;
                int a = (c2 >> 1) & 0x0F;
                if (!s.text && s.mode == ROLLUP && row != s.base) {        // окно бегущих подписей переезжает
                    Cc608Screen old = s.disp; s.disp.clear();
                    for (int k = 0; k < s.ru; k++) {
                        int from = s.base - k, to = row - k;
                        if (from >= 0 && to >= 0) for (int q = 0; q < 32; q++) s.disp.m[to][q] = old.m[from][q];
                    }
                    s.base = row; s.changed = true;
                }
                // в тексте строку задаёт CR, PAC ставит только отступ и цвет
                s.row = s.text || s.mode == ROLLUP ? s.base : row;
                s.col = a >= 8 ? (a - 8) * 4 : 0;
                s.attr = (uint8_t)((a < 7 ? a : 0) | (a == 7 ? 8 : 0) | ((c2 & 1) ? 16 : 0));
                continue;
            }
            if (c == 1 && c2 >= 0x20 && c2 <= 0x2F) {                       // код в строке: цвет, курсив, подчёркивание (как пробел)
                Service &s = cur();
                int a = (c2 >> 1) & 7;
                uint8_t keep = s.attr & 32;
                s.attr = (uint8_t)((a < 7 ? a : (s.attr & 7)) | (a == 7 ? 8 : 0) | ((c2 & 1) ? 16 : 0) | keep);
                put(' ');
                continue;
            }
            if (c == 1 && c2 >= 0x30 && c2 <= 0x3F) { put(SPECIAL[c2 - 0x30]); continue; }
            if ((c == 2 || c == 3) && c2 >= 0x20 && c2 <= 0x3F) {           // расширенный знак заменяет предыдущий
                Service &s = cur();
                if (s.col > 0) s.col--;
                put((c == 2 ? EXT1 : EXT2)[c2 - 0x20]);
                continue;
            }
            if (c == 7 && c2 >= 0x21 && c2 <= 0x23) { Service &s = cur(); s.col = std::min(31, s.col + (c2 - 0x20)); continue; }   // табуляция
            if (c == 0 && c2 >= 0x20 && c2 <= 0x2F) continue;               // цвет фона — экран без фона
            if (c == 7 && c2 >= 0x2D && c2 <= 0x2F) continue;               // прозрачный фон / чёрные знаки
            if ((c == 4 || c == 5) && c2 >= 0x20 && c2 <= 0x2F) {           // команды (0x14/0x1C, в поле 2 также 0x15/0x1D)
                Service &cap = sv[ch][0], &tx = sv[ch][1];
                switch (c2) {
                case 0x20: text[ch] = false; cap.mode = POPON; break;                                      // RCL
                case 0x21: { Service &s = cur(); if (s.col > 0) { s.col--; s.target().m[s.row][s.col] = Cc608Cell(); if (&s.target() == &s.disp) s.changed = true; } break; }   // BS
                case 0x24: { Service &s = cur(); for (int k = s.col; k < 32; k++) s.target().m[s.row][k] = Cc608Cell(); if (&s.target() == &s.disp) s.changed = true; break; }  // DER
                case 0x25: case 0x26: case 0x27: {                                                          // RU2..RU4
                    text[ch] = false;
                    if (cap.mode != ROLLUP) { cap.disp.clear(); cap.nond.clear(); cap.base = 14; cap.row = 14; cap.col = 0; cap.changed = true; cap.structural = true; }
                    cap.mode = ROLLUP; cap.ru = c2 - 0x23; cap.row = cap.base;
                    break;
                }
                case 0x28: cur().attr |= 32; break;                                                         // мигание
                case 0x29: text[ch] = false; cap.mode = PAINTON; break;                                     // RDC
                case 0x2A:                                                                                  // TR: текст заново
                    text[ch] = true; { std::string t = strip(tx.disp.row_text(tx.base)); if (!t.empty()) tx.lines.push_back({u, t}); }
                    tx.disp.clear(); tx.row = tx.base; tx.col = 0; tx.changed = true; tx.structural = true;
                    tx.lines.push_back({u, ""});
                    break;
                case 0x2B: text[ch] = true; break;                                                          // RTD
                case 0x2C:                                                                                  // EDM
                    if (cap.mode == PAINTON) { std::string t = screen_text(cap.disp, " / "); if (!t.empty()) cap.lines.push_back({u, t}); }
                    cap.disp.clear(); cap.changed = true; cap.structural = true; break;
                case 0x2D:                                                                                  // CR
                    if (text[ch]) rollup_cr(tx);
                    else if (cap.mode == ROLLUP) rollup_cr(cap);
                    else if (cap.mode == PAINTON && cap.row < 14) { cap.row++; cap.col = 0; }
                    break;
                case 0x2E: cap.nond.clear(); break;                                                         // ENM
                case 0x2F: {                                                                                // EOC
                    text[ch] = false;
                    std::swap(cap.disp, cap.nond); cap.mode = POPON; cap.changed = true; cap.structural = true;
                    std::string t = screen_text(cap.disp, " / "); if (!t.empty()) cap.lines.push_back({u, t});
                    break;
                }
                }
                all_shots(false);
                continue;
            }
            continue;
        }
        if (xds) continue;
        p1 = p2 = -1;
        for (int c : {c1, c2}) if (c >= 0x20) put(basic(c));
        all_shots(false);
    }
    for (auto &a : sv) {
        if (a[1].mode == ROLLUP) { std::string t = strip(a[1].disp.row_text(a[1].base)); if (!t.empty()) a[1].lines.push_back({u, t}); }
        for (auto &s : a) { s.structural = true; shot(s, false); }
    }

    Cc608Result R;
    Json svc = Json::array();
    std::vector<std::tuple<int, std::string, std::string>> all;     // место, служба, строка
    for (int k = 0; k < 2; k++) for (int i = 0; i < 2; i++) {         // CC1, CC2, затем T1, T2
        Service &s = sv[i][k];
        int nl = 0; for (auto &l : s.lines) if (!l.second.empty()) nl++;
        if (!nl) continue;
        Json j = Json::object(); j["name"] = s.name; j["kind"] = s.text ? "text" : "captions"; j["shots"] = s.shots;
        Json lj = Json::array(); for (auto &l : s.lines) { Json e = Json::array(); e.push((double)l.first); e.push(l.second); lj.push(e); }
        j["lines"] = lj;
        svc.push(j);
        for (auto &l : s.lines) if (!l.second.empty()) all.push_back({l.first, s.name, l.second});
        R.lines[s.name] = nl;
        if (s.text) continue;
        // SRT по структурным снимкам: экран от снимка до следующего
        std::string srt; int n = 0;
        for (size_t q = 0; q < s.cues.size(); q++) {
            if (s.cues[q].second.empty()) continue;
            double a = s.cues[q].first / rate, b = q + 1 < s.cues.size() ? s.cues[q + 1].first / rate : a + 4;
            if (b - a > 10) b = a + 10;
            if (b <= a) b = a + 0.5;
            srt += fmt("%d\n", ++n) + srt_time(a) + " --> " + srt_time(b) + "\n" + s.cues[q].second + "\n\n";
        }
        if (n) R.srt[s.name] = srt;
    }
    std::stable_sort(all.begin(), all.end(), [](auto &a, auto &b) { return std::get<0>(a) < std::get<0>(b); });
    for (auto &x : all) R.transcript += "[" + tstamp(std::get<0>(x) / rate) + "] " + std::get<1>(x) + "  " + std::get<2>(x) + "\n";
    R.json["services"] = svc;
    R.json["rate"] = rate;
    return R;
}
