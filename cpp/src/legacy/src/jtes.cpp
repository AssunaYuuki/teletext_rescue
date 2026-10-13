#include "jtes.h"

namespace {
#include "gen_jis0208.inc"
#include "gen_jtes.inc"

// ---------------------------------------------------------------- (272,190)
// Регистр синдрома 82 бита: биты 0..23, 24..51, 52..81 (три слова генератора).
using u128 = unsigned __int128;
const u128 FB = (u128)0x00a20208 | ((u128)0x08808a00 << 24) | ((u128)0x01846008 << 52);
const u128 MASK82 = (((u128)1) << 82) - 1;
// 17 ортогональных проверок для мажоритарного решения о старшем бите
const int CHECKS[17][8] = {
    {71, 76, -1}, {17, -1}, {5, 23, -1}, {21, 27, 45, -1}, {3, 25, 31, 49, -1}, {16, 20, 42, 48, 66, -1},
    {35, 52, 56, 78, -1}, {8, 44, 61, 65, -1}, {2, 11, 47, 64, 68, -1}, {10, 13, 22, 58, 75, 79, -1},
    {1, 12, 15, 24, 60, 77, 81, -1}, {30, 32, 43, 46, 55, -1}, {6, 37, 39, 50, 53, 62, -1}, {0, 7, 38, 40, 51, 54, 63, -1},
    {18, 19, 26, 57, 59, 70, 73, -1}, {9, 28, 29, 36, 67, 69, 80, -1}, {4, 14, 33, 34, 72, 74, -1}};   // 41 дважды = ни разу
// маски проверок строятся один раз и потокобезопасно (коррекция идёт параллельно)
const std::array<u128, 17> &checks() {
    static const std::array<u128, 17> cm = [] {
        std::array<u128, 17> m{};
        for (int i = 0; i < 17; i++) for (int k = 0; CHECKS[i][k] >= 0; k++) m[i] ^= ((u128)1) << CHECKS[i][k];
        return m;
    }();
    return cm;
}
inline void shift(u128 &r, int bit) {
    int nxt = (int)((r >> 81) & 1);
    if (nxt) r ^= FB;
    r = ((r << 1) & MASK82) | (u128)(nxt ^ bit);
}
inline int parity128(u128 v) { return (__builtin_popcountll((uint64_t)v) + __builtin_popcountll((uint64_t)(v >> 64))) & 1; }
}

bool jtes_syndrome_zero(const u8 *d) {
    u128 r = 0;
    for (int j = 0; j < 34; j++) for (int i = 0; i < 8; i++) shift(r, (d[j] >> i) & 1);
    return r == 0;
}

int jtes_correct(u8 *d) {
    const auto &CM = checks();
    u128 r = 0;
    for (int j = 0; j < 34; j++) for (int i = 0; i < 8; i++) shift(r, (d[j] >> i) & 1);
    if (r == 0) return 0;
    u8 save[34]; memcpy(save, d, 34);
    for (int k = 14; k >= 9; k--) {
        shift(r, 0);
        for (int j = 0; j < 34; j++) {
            u8 cb = 0;
            for (int i = 0; i < 8; i++) {
                int s = 0; for (int q = 0; q < 17; q++) s += parity128(r & CM[q]);
                int mj = s >= k;
                cb |= (u8)((((d[j] >> i) & 1) ^ mj) << i);
                shift(r, mj);
            }
            d[j] = cb;
        }
        if (r == 0) {
            // мажоритарный декодер на мусоре тоже «сходится» — к чужому слову; верим, если исправлено немного бит
            int diff = 0; for (int j = 0; j < 34; j++) diff += __builtin_popcount(save[j] ^ d[j]);
            if (diff > 10) break;
            return 15 - k;
        }
    }
    memcpy(d, save, 34);
    return -1;
}

// ---------------------------------------------------------------- группы данных
namespace {
uint16_t crc16(const u8 *p, size_t n) {          // CRC-16 (отражённый 0x8408, начальное 0)
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return c;
}
int bcd(int v) { return (v >> 4) * 10 + (v & 15); }

// блоки группы с проверкой CRC: -> да, если все блоки верны
bool group_ok(const std::vector<u8> &g) {
    size_t p = 0;
    if (g.size() < 22) return false;
    while (p + 22 <= g.size()) {
        if (g[p] == 2) {                                        // STX: единица данных на несколько блоков
            int len = ((g[p + 3] & 0x7f) << 8) | g[p + 4];
            size_t n = (size_t)((len + 8 + 21) / 22) * 22;
            if (p + n > g.size()) return false;
            if (crc16(&g[p], n - 2) != (g[p + n - 2] | (g[p + n - 1] << 8))) return false;
            p += n;
        } else {                                                // SOH: блок заголовка
            if (crc16(&g[p], 20) != (g[p + 20] | (g[p + 21] << 8))) return false;
            p += 22;
        }
    }
    return true;
}
}

// ---------------------------------------------------------------- рендер страницы
namespace {
const char32_t GETA = 0x3013;          // 〓 — знак, которого нет в шрифте (мозаика, DRCS в тексте)

struct Renderer {
    const std::vector<u8> &g;
    JtesRender out;
    // курсор: x слева, y снизу (как в стандарте: 192 — строка заголовка), размер знака
    int x = 0, y = 168, w = 16, h = 24, fg = 7, bg = 8, ras = 8, rasH = 8, rasB = 8;
    bool pol = false; int hlc = 0;
    enum CT { KANJI, ALNUM, HIRA, KATA, MOSAIC, DRCS, MACRO };
    struct G { CT ct = KANJI; int num = 0; bool two = false; } gs[4];
    int gl = 0, gr = 2, gl_save = -1;
    int repeat = -1;                   // RPC: повтор следующего знака (0 — до конца строки)
    std::map<int, std::array<u8, 48>> drcs[15]; int drcs_size[15] = {0};
    // текст по строкам экрана: y -> (x -> знак)
    std::map<int, std::map<int, char32_t>> txt;
    // байты
    size_t pos = 0, end = 0;
    std::vector<u8> mac; size_t mpos = 0;     // разворачиваемый макрос

    explicit Renderer(const std::vector<u8> &grp) : g(grp) { out.pix.assign(248 * 204, 0xFF); }
    int get() {
        if (mpos < mac.size()) return mac[mpos++];
        if (pos >= end) return -1;
        return g[pos++];
    }
    int colour(int c) const { return c == 8 ? ras : c; }
    void px(int X, int Y, int c) { if (X >= 0 && X < 248 && Y >= 0 && Y < 204 && c >= 0) out.pix[Y * 248 + X] = (u8)c; }
    void fill_rect(int x0, int y0, int x1, int y1, int c) { for (int Y = y0; Y <= y1; Y++) for (int X = x0; X <= x1; X++) px(X, Y, c); }
    void reset(bool header) {
        gs[0] = {KANJI, 0, true}; gs[1] = {ALNUM, 0, false}; gs[2] = {HIRA, 0, false}; gs[3] = {MACRO, 0, false};
        gl = 0; gr = 2; gl_save = -1; fg = 7; bg = 8; hlc = 0; pol = false; repeat = -1;
        if (header) { w = 8; h = 12; x = 0; y = 192; ras = rasH; } else { w = 16; h = 24; x = 0; y = 168; ras = rasB; }
    }
    void newline() { x = 0; y -= h; if (y < 0) y = 192 - h; }
    void eol() { if (x + w > 248) newline(); }
    int top() const { return 204 - y - h; }                  // верх знака в пикселях сверху
    void cell(char32_t c, int kind, int code, bool nospace) {
        eol();
        int f = pol ? bg : fg, b = pol ? fg : bg;
        f = colour(f); b = nospace ? -1 : colour(b);
        JtesCell e; e.x = x; e.y = top(); e.w = w; e.h = h; e.c = c; e.fg = (u8)f; e.bg = (u8)(b < 0 ? 0xFF : b); e.kind = kind; e.code = code;
        e.underline = (hlc & 0x10) != 0;
        if (kind == 0 && b >= 0) fill_rect(e.x, e.y, e.x + w - 1, e.y + h - 1, b);
        if (kind == 1) {                                       // мозаика: шрифт 16×24, растянуть в клетку
            const uint16_t *gp = JTES_MOSAIC[code];
            for (int Y = 0; Y < h; Y++) for (int X = 0; X < w; X++) {
                int sy = Y * 24 / h, sx = X * 16 / w;
                if (gp[sy] & (0x8000 >> sx)) px(e.x + X, e.y + Y, f); else if (b >= 0) px(e.x + X, e.y + Y, b);
            }
        }
        if (kind == 2) {                                       // DRCS: строки по w/8 байт
            int set = code >> 8, cc = code & 0xff;
            auto it = drcs[set].find(cc);
            if (it != drcs[set].end()) {
                int bpr = std::max(1, w / 8), rows = drcs_size[set] == 40 ? 20 : h, y0 = e.y + (drcs_size[set] == 40 ? 2 : 0);
                for (int Y = 0; Y < rows; Y++) for (int X = 0; X < bpr * 8; X++) {
                    size_t bi = (size_t)Y * bpr + X / 8; if (bi >= 48) break;
                    if (it->second[bi] & (0x80 >> (X % 8))) px(e.x + X, y0 + Y, f); else if (b >= 0) px(e.x + X, y0 + Y, b);
                }
            }
        }
        if (e.underline) fill_rect(e.x, e.y + h - 1, e.x + w - 1, e.y + h - 1, f);
        out.cells.push_back(e);
        txt[e.y][e.x] = c;
    }
    void advance() { x += w; if (x >= 248) newline(); }
    void put(bool grset, int c) {
        bool nospace = false;
        if (c == 0x20 || c == 0x7f) {
            int f = colour(pol ? bg : fg), b = colour(pol ? fg : bg);
            eol();
            fill_rect(x, top(), x + w - 1, top() + h - 1, c == 0x7f ? f : b);
            txt[top()][x] = ' ';
        } else {
            c &= 0x7f;
            int gi = grset ? gr : gl;
            G &s = gs[gi];
            switch (s.ct) {
            case KANJI: {
                int c2 = get(); if (c2 < 0) return;
                int code = ((c << 8) | c2) & 0x7f7f, r = (code >> 8) - 0x21, k = (code & 0xff) - 0x21;
                char32_t u = (r >= 0 && r < 94 && k >= 0 && k < 94) ? JIS0208[r * 94 + k] : 0;
                nospace = (code >= 0x212d && code <= 0x2132) || code == 0x227e;   //濁点・半濁点など — без шага
                cell(u ? u : GETA, 0, code, nospace);
                break;
            }
            case ALNUM: { char32_t u = c >= 0x21 && c <= 0x7e ? (char32_t)(0xFF00 + c - 0x20) : GETA; cell(u, 0, c, false); break; }   // полноширинные латиница и цифры
            case HIRA: case KATA: {
                static const uint16_t th[8] = {0x2135, 0x2136, 0x213c, 0x2123, 0x2156, 0x2157, 0x2122, 0x2126};
                static const uint16_t tk[8] = {0x2133, 0x2134, 0x213c, 0x2123, 0x2156, 0x2157, 0x2122, 0x2126};
                int code = c >= 0x77 ? (s.ct == HIRA ? th : tk)[c - 0x77] : ((s.ct == HIRA ? 0x2400 : 0x2500) | c);
                int r = (code >> 8) - 0x21, k = (code & 0xff) - 0x21;
                char32_t u = (r >= 0 && r < 94 && k >= 0 && k < 94) ? JIS0208[r * 94 + k] : 0;
                cell(u ? u : GETA, 0, code, false);
                break;
            }
            case MOSAIC: nospace = s.num >= 2; cell(GETA, 1, s.num * 94 + (c - 0x21), nospace); break;
            case DRCS:
                if (s.two) { get(); cell(GETA, 0, 0, false); }
                else cell(GETA, 2, ((s.num - 1) << 8) | c, false);
                break;
            case MACRO:
                if (mpos >= mac.size()) { mac = macro(c); mpos = 0; }
                if (gl_save >= 0 && !grset) { gl = gl_save; gl_save = -1; }
                return;
            }
        }
        if (!grset && gl_save >= 0) { gl = gl_save; gl_save = -1; }
        if (!nospace) advance();
    }
    // стандартные макросы 0x60..0x6F: назначение наборов знаков
    std::vector<u8> macro(int c) {
        static const char *DEF[16] = {
            "\x1b$B\x1b)J\x1b*0\x1b+ p\x0f\x1b}", "\x1b$B\x1b)1\x1b*0\x1b+ p\x0f\x1b}", "\x1b$B\x1b) A\x1b*0\x1b+ p\x0f\x1b}",
            "\x1b(2\x1b)4\x1b*5\x1b+ p\x0f\x1b}", "\x1b(2\x1b)3\x1b*5\x1b+ p\x0f\x1b}", "\x1b(2\x1b) A\x1b*5\x1b+ p\x0f\x1b}",
            "\x1b( A\x1b) B\x1b* C\x1b+ p\x0f\x1b}", "\x1b( D\x1b) E\x1b* F\x1b+ p\x0f\x1b}", "\x1b( G\x1b) H\x1b* I\x1b+ p\x0f\x1b}",
            "\x1b( J\x1b) K\x1b* L\x1b+ p\x0f\x1b}", "\x1b( M\x1b) N\x1b* O\x1b+ p\x0f\x1b}", "\x1b$B\x1b) B\x1b*0\x1b+ p\x0f\x1b}",
            "\x1b$B\x1b) C\x1b*0\x1b+ p\x0f\x1b}", "\x1b$B\x1b) D\x1b*0\x1b+ p\x0f\x1b}", "\x1b(1\x1b)0\x1b*J\x1b+ p\x0f\x1b}",
            "\x1b(J\x1b)2\x1b* A\x1b+ p\x0f\x1b}"};
        std::vector<u8> v;
        if (c >= 0x60 && c <= 0x6f) for (const char *p = DEF[c - 0x60]; *p; p++) v.push_back((u8)*p);
        return v;
    }
    void esc() {
        bool two = false;
        int c = get();
        if (c == 0x24) { two = true; c = get(); }
        switch (c) {
        case 0x6e: gl = 2; gl_save = -1; return;
        case 0x6f: gl = 3; gl_save = -1; return;
        case 0x7e: gr = 1; return;
        case 0x7d: gr = 2; return;
        case 0x7c: gr = 3; return;
        }
        G *cap;
        if (c >= 0x28 && c <= 0x2b) { cap = &gs[c - 0x28]; c = get(); }
        else if (two) cap = &gs[0];
        else return;
        if (c == 0x20) {
            c = get();
            if (c == 0x70) cap->ct = MACRO;
            else { cap->ct = DRCS; cap->num = c - 0x40; cap->two = two; }
            return;
        }
        switch (c) {
        case 0x42: cap->ct = KANJI; break;
        case 0x4a: cap->ct = ALNUM; break;
        case 0x30: cap->ct = HIRA; break;
        case 0x31: cap->ct = KATA; break;
        case 0x32: case 0x33: case 0x34: case 0x35: cap->ct = MOSAIC; cap->num = c - 0x32; break;
        default: return;
        }
        cap->two = two;
    }
    void size(int c) {
        switch (c) {
        case 0x88: w = 8; h = 12; break; case 0x89: w = 8; h = 24; break; case 0x8a: w = 16; h = 24; break;
        case 0x60: w = 4; h = 4; break; case 0x6b: w = 16; h = 20; break; case 0x41: w = 16; h = 48; break;
        case 0x44: w = 32; h = 24; break; case 0x45: w = 32; h = 48; break;
        }
    }
    void set_colour(int c) { if (c >= 0x80) fg = c - 0x80; else if (c < 0x50) fg = c - 0x40; else bg = c - 0x50; }
    // текст (0x20 основной, 0x24 заголовок): управляющие коды и знаки
    void text(size_t start, size_t stop, bool header) {
        reset(header);
        pos = start; end = stop; mac.clear(); mpos = 0;
        for (int guard = 0; guard < 200000; guard++) {
            int c = get();
            if (c < 0) return;
            switch (c) {
            case 0: break;
            case 3: case 4: case 0x17: return;
            case 8: x -= w; if (x < 0) { x = 248 - w; y += h; y %= 192; } break;   // APB
            case 9: advance(); break;                                              // APF
            case 0xa: y -= h; if (y < 0) y = 192 - h; break;                       // APD
            case 0xb: y += h; y %= 192; break;                                     // APU
            case 0xc: break;                                                       // CS
            case 0xd: newline(); break;                                            // APR
            case 0xe: gl = 1; gl_save = -1; break;                                 // LS1
            case 0xf: gl = 0; gl_save = -1; break;                                 // LS0
            case 0x16: { int p = get() - 0x40; x += w * p; y -= h * (x / 248); x %= 248; if (y < 0) y = 192 - h; break; }   // PAPF
            case 0x19: gl_save = gl; gl = 2; break;                                // SS2
            case 0x1b: esc(); break;
            case 0x1c: { int p1 = get() - 0x40, p2 = get() - 0x40; x = w * p2; y = 192 - h * (p1 + 1); break; }   // APS
            case 0x1d: gl_save = gl; gl = 3; break;                                // SS3
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: set_colour(c); break;
            case 0x88: case 0x89: case 0x8a: size(c); break;
            case 0x8b: size(get()); break;                                         // SZX
            case 0x8c: { int k = 0; while (k++ < 4000) { int q = get(); if (q < 0 || q == 0x8d) break; } advance(); break; }   // MCS…MCT
            case 0x90: { int p = get(); if (p == 0x20) get(); else set_colour(p); break; }   // COL
            case 0x91: get(); break;                                               // FLC
            case 0x92: { int p = get(); if (p == 0x20) get(); break; }             // CDC
            case 0x93: pol = get() > 0x40; break;                                  // POL
            case 0x94: get(); break;                                               // WMM
            case 0x97: hlc = (hlc & 0xf0) | (get() & 0xf); break;                  // HLC
            case 0x98: repeat = get() - 0x40; break;                               // RPC
            case 0x99: hlc &= 0xef; break;                                         // SPL
            case 0x9a: hlc |= 0x10; break;                                         // STL
            case 0x9b: { int k = 0; int q; while (k++ < 64 && (q = get()) >= 0 && q != 0x20) {} get(); break; }   // CSI … 2/0 F
            case 0x9d: get(); get(); break;                                        // TIME
            default:
                if ((c >= 0x20 && c <= 0x7f) || (c >= 0xa0 && c <= 0xff)) {
                    bool grs = c >= 0xa0;
                    int cc = grs ? (c == 0xa0 ? 0x20 : c & 0x7f) : c;
                    if (repeat >= 0) {
                        int n = repeat; repeat = -1;
                        size_t p0 = pos;
                        if (n == 0) { int y0 = y; do { pos = p0; put(grs, cc); } while (y == y0 && x != 0); }
                        else for (int k = 0; k < n; k++) { pos = p0; put(grs, cc); }
                    } else put(grs, cc);
                }
            }
        }
    }
    void drcs_def(size_t p) {
        int len = ((g[p + 3] & 0x7f) << 8) | g[p + 4];
        int tm = g[p + 5], lps = g[p + 6], db7 = g[p + 7], db8 = g[p + 8];
        if ((tm & 0xf0) || db7 < 0x41 || db7 > 0x4f) return;
        int set = db7 - 0x41, sz;
        switch (lps) { case 0x6b: sz = 40; break; case 0x71: sz = 12; break; case 0x73: sz = 48; break; case 0x78: sz = 24; break; default: return; }
        if (tm & 3) drcs[set].clear();
        drcs_size[set] = sz;
        size_t q = p + 9, stop = std::min(g.size(), p + 9 + (size_t)std::max(0, len - 4));
        for (int c = db8; q + sz <= stop && c < 0x7f; c++, q += sz) { std::array<u8, 48> a{}; memcpy(a.data(), &g[q], sz); drcs[set][c] = a; }
    }
    // фото (один слой): блоки ux×uy точек bx×by, цвета переднего плана/фона на блок, биты — сырые или MH
    struct Bits {
        Renderer &R; size_t p, stop; int d = 0, n = 0;
        int one = 1, run = 0;
        int byte() { if (p >= stop) return 0; return R.g[p++]; }
        int raw() { if (--n > 0) { d <<= 1; return (d & 0x80) != 0; } d = byte(); n = 8; return (d & 0x80) != 0; }
        int mh_sub() {
            const uint8_t *bits = one ? MH_BLACK_BITS : MH_WHITE_BITS; const int16_t *rn = one ? MH_BLACK_RUN : MH_WHITE_RUN;
            int b = 0;
            for (int i = 0; i < 8;) { b |= raw() << i; if (bits[b] == ++i) return rn[b]; }
            return -2;
        }
        void mh_run() { run = mh_sub(); if (run >= 32) { int r2 = mh_sub(); run = r2 < 0 ? r2 : run + r2; } }
        int mh() { int guard = 0; while (run <= 0 && guard++ < 64) { if (run < 0) return one; one = !one; mh_run(); } run--; return one; }
    };
    void photo(size_t p) {
        int len = ((g[p + 3] & 0x7f) << 8) | g[p + 4];
        int py = g[p + 6], pxx = g[p + 7], by = g[p + 8], bx = g[p + 9], uy = g[p + 10] & 0x7f, ux = g[p + 11] & 0x7f;
        int fy = g[p + 12] & 0x7f, fx = g[p + 13] & 0x7f, sd = g[p + 12] >> 7;
        if (ux * uy <= 0 || ux * uy > 4096) return;
        Bits B{*this, p + 14, std::min(g.size(), p + 5 + (size_t)len)};
        int Y = 203 - py;
        for (int a = 0; a < fy; a++) {
            int X = pxx;
            for (int k = 0; k < fx; k++) {
                int uc1 = B.byte();
                std::vector<int> cf(ux * uy, 7), cb(ux * uy, ras);
                auto cols = [&](std::vector<int> &v, bool e, int def) {
                    if (!e) { for (auto &q : v) q = def == 8 ? ras : def; return; }
                    if (uc1 & 8) {
                        size_t i = 0; int c = 0;
                        while (i < v.size()) { int q = B.byte(); int n = q & 15; c = q >> 4; if (c == 8) c = ras; if (!n) break; for (; n > 0 && i < v.size(); n--) v[i++] = c; }
                        while (i < v.size()) v[i++] = c;
                    } else for (size_t i = 0; i < v.size(); i += 2) {
                        int q = B.byte(); int c0 = q >> 4, c1 = q & 15;
                        v[i] = c0 == 8 ? ras : c0; if (i + 1 < v.size()) v[i + 1] = c1 == 8 ? ras : c1;
                    }
                };
                cols(cf, uc1 & 0x80, 7); cols(cb, uc1 & 0x40, 8);
                auto dummy = [&](bool e) { if (!e) return; if (uc1 & 8) { while (B.byte() & 15) {} } else for (int i = 0; i < (ux * uy + 1) / 2; i++) B.byte(); };
                dummy(uc1 & 0x20); dummy(uc1 & 0x10);
                B.n = 0;
                int yy = Y + sd;
                for (int u2 = 0; u2 < uy; u2++)
                    for (int b2 = 0; b2 < by; b2++) {
                        int xx = X, blk = u2 * ux;
                        B.one = 1; B.run = 0;
                        for (int u1 = 0; u1 < ux; u1++, blk++)
                            for (int b1 = 0; b1 < bx; b1++, xx++) {
                                int bit = (uc1 & 4) ? ((uc1 & 2) ? B.mh() : B.raw()) : 0;
                                px(xx, yy, bit ? cf[blk] : cb[blk]);
                            }
                        if (sd) yy++; else yy--;
                    }
                if (uc1 & 2) { int guard = 0; while (B.run != -2 && guard++ < 100000 && B.p < B.stop) B.mh_run(); }
                X += ux * bx;
            }
            Y += (sd ? uy : -uy) * by;
        }
    }
    void run() {
        size_t p = 0;
        while (p + 22 <= g.size()) {
            if (g[p] == 1) {                                      // SOH
                if (g[p + 8] == 0x21) { rasH = g[p + 18] >> 4; rasB = g[p + 18] & 15; out.page_colours = g[p + 18]; }
                p += 22; continue;
            }
            if (g[p] != 2) { p += 22; continue; }
            int len = ((g[p + 3] & 0x7f) << 8) | g[p + 4];
            size_t n = (size_t)((len + 8 + 21) / 22) * 22;
            switch (g[p + 2]) {
            case 0x20: text(p + 5, std::min(g.size(), p + 5 + (size_t)len), false); break;
            case 0x24: text(p + 5, std::min(g.size(), p + 5 + (size_t)len), true); break;
            case 0x30: drcs_def(p); break;
            case 0x38: photo(p); break;
            }
            p += n;
        }
        // текст: строки сверху вниз (строка заголовка — первая)
        for (auto &row : txt) {
            std::u32string s; int last = -1;
            for (auto &c : row.second) { if (last >= 0 && c.first - last > 24) s += U' '; s += c.second; last = c.first; }
            std::string t = rstrip(from_u32(s));
            if (strip(t).empty()) continue;
            if (row.first < 12) out.header += (out.header.empty() ? "" : " ") + strip(t);
            else out.text += t + "\n";
        }
    }
};
}

JtesRender jtes_render(const std::vector<u8> &group) {
    Renderer R(group);
    // фон: цвет растра заголовка (строки 0..11) и основной части
    for (size_t p = 0; p + 22 <= group.size(); p += 22)
        if (group[p] == 1 && group[p + 8] == 0x21) { R.rasH = group[p + 18] >> 4; R.rasB = group[p + 18] & 15; break; }
    R.fill_rect(0, 0, 247, 11, R.rasH == 8 ? 0 : R.rasH);
    R.fill_rect(0, 12, 247, 203, R.rasB == 8 ? 0 : R.rasB);
    try { R.run(); } catch (...) {}
    return R.out;
}

// ---------------------------------------------------------------- поток пакетов -> страницы
Json jtes_decode(const std::vector<JtesPacket> &pk, double rate, std::string &report) {
    struct Stream { bool in = false; int next = 0; std::vector<u8> buf; int f0 = 0; };
    std::map<int, Stream> st;                        // по строке ТВ (оба поля — один поток)
    std::map<int, int> modes;
    struct Pg { std::vector<u8> g; int first = 0, last = 0, copies = 0, prog = -1, page = -1; int line = 0; };
    std::map<std::pair<int, int>, Pg> pages;        // (программа, страница)
    long groups = 0, good = 0;
    auto finish = [&](Stream &s, int line) {
        if (s.buf.size() < 22) return;
        groups++;
        if (!group_ok(s.buf)) return;
        good++;
        const u8 *h = s.buf.data();
        int prog = ((h[10] >> 4) & 15) * 1000 + (h[10] & 15) * 100 + bcd(h[11]);
        int page = h[8] == 0x21 ? bcd(h[12]) : 0;     // заголовок программы — страница 0
        auto &P = pages[{prog, page}];
        if (P.copies == 0) { P.first = s.f0; P.prog = prog; P.page = page; P.line = line; }
        P.copies++; P.last = s.f0;
        P.g = s.buf;
    };
    for (auto &p : pk) {
        modes[p.d[0]]++;
        if (p.d[0] != 0x92 && p.d[0] != 0x2a) continue;          // режимы передачи страниц
        Stream &s = st[p.line];
        bool head = p.d[1] & 0x10; int ci = p.d[1] & 15;
        if (head) {
            if (s.in) finish(s, p.line);
            s.in = true; s.next = 0; s.buf.clear(); s.f0 = p.field;
        }
        if (!s.in) continue;
        if (ci != s.next) { s.in = false; continue; }
        s.next = (s.next + 1) & 15;
        for (int j = 0; j < 22; j++) s.buf.push_back((u8)(((p.d[j + 2] & 0x3f) << 2) | ((p.d[j + 1] & 0xc0) >> 6)));
        if (s.buf.size() > 64 * 1024) s.in = false;
    }
    for (auto &kv : st) if (kv.second.in) finish(kv.second, kv.first);

    report += fmt("packets %zu; data groups %ld, with correct CRC %ld; pages %zu\n", pk.size(), groups, good, pages.size());
    report += "Packet modes (first byte: count):";
    for (auto &m : modes) report += fmt(" %02X:%d", m.first, m.second);
    report += "\n\n";
    Json pj = Json::array();
    for (auto &kv : pages) {
        auto &P = kv.second;
        JtesRender r = jtes_render(P.g);
        report += fmt("=== programme %d page %d  (line %d, %d copies, first at %.1f s)\n", P.prog, P.page, P.line, P.copies, P.first / rate);
        if (!r.header.empty()) report += "[" + r.header + "]\n";
        report += r.text + "\n";
        Json e = Json::object();
        e["prog"] = (double)P.prog; e["page"] = (double)P.page; e["line"] = (double)P.line; e["copies"] = (double)P.copies;
        e["first"] = (double)P.first; e["last"] = (double)P.last; e["header"] = r.header; e["text"] = r.text;
        std::string hx; for (u8 b : P.g) hx += fmt("%02X", b);
        e["group"] = hx;
        pj.push(e);
    }
    Json j = Json::object(); j["pages"] = pj; j["groups"] = (double)groups; j["groups_ok"] = (double)good;
    return j;
}
