// Japanese teletext (JTES, BT.653 system D): codeword correction, data groups, pages, text and rendering.
// Worked out on NHK_xxxx-xx-xx_JTES_teletext (テレモ, Kansai, January 1996); see jtes.h for the layers.
#include "jtes.h"
#include "fastdata.h"
#include "png.h"
#include "ui/util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "jtes_fonts.inc"
#include "jtes_mosaic.inc"

namespace {

// ---------------------------------------------------------------- (272,190) difference-set cyclic code
typedef unsigned __int128 u128;
const int N = 272, R = 82;
u128 genPoly()
{
    static const int e[] = {82, 77, 76, 71, 67, 66, 56, 52, 48, 40, 36, 34, 24, 22, 18, 10, 4, 0};
    u128 g = 0;
    for (int x : e) g |= (u128)1 << x;
    return g;
}
// syndrome of a codeword given bit by bit in sending order (= highest degree first)
u128 syndrome(const uint8_t* bits)
{
    static const u128 G = genPoly();
    u128 r = 0;
    for (int i = 0; i < N; i++) {
        r = (r << 1) | bits[i];
        if ((r >> R) & 1) r ^= G;
    }
    return r;
}
struct Key {
    uint64_t lo; uint32_t hi;
    bool operator==(const Key& o) const { return lo == o.lo && hi == o.hi; }
};
struct KeyHash { size_t operator()(const Key& k) const { return std::hash<uint64_t>()(k.lo * 31 + k.hi); } };
Key key(u128 s) { return {(uint64_t)s, (uint32_t)(s >> 64)}; }
// syndromes of every 1- and 2-bit error: position(s) in sending order
const std::unordered_map<Key, std::pair<int, int>, KeyHash>& errorTable()
{
    static std::unordered_map<Key, std::pair<int, int>, KeyHash> t;
    if (!t.empty()) return t;
    std::vector<u128> one(N);
    uint8_t b[N];
    for (int i = 0; i < N; i++) {
        memset(b, 0, sizeof b);
        b[i] = 1;
        one[i] = syndrome(b);
        t.emplace(key(one[i]), std::make_pair(i, -1));
    }
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) t.emplace(key(one[i] ^ one[j]), std::make_pair(i, j));
    return t;
}

// Majority-logic decoding of the (273,191) difference-set code (the shortened bit 272 is 0): the code is
// checked by every cyclic shift of x^-d over the perfect difference set D below, 17 checks meet on each bit and
// nowhere else, so a bit is wrong when 9 or more of its checks fail; corrects up to 8 errors per line.
bool majorityFix(uint8_t* bits, int& flipped)
{
    static const int D[17] = {0, 18, 24, 46, 50, 67, 103, 112, 115, 126, 128, 159, 166, 167, 186, 196, 201};
    const int n = 273;
    uint8_t c[n], chk[n];
    for (int d = 0; d < N; d++) c[d] = bits[N - 1 - d];
    c[N] = 0;
    flipped = 0;
    for (int it = 0; it < 12; it++) {
        for (int sft = 0; sft < n; sft++) {
            int v = 0;
            for (int d : D) v ^= c[(sft - d + n) % n];
            chk[sft] = (uint8_t)v;
        }
        std::vector<int> flip;
        for (int q = 0; q < n; q++) {
            int f = 0;
            for (int d : D) f += chk[(q + d) % n];
            if (f >= 9) flip.push_back(q);
        }
        if (flip.empty()) break;
        for (int q : flip) c[q] ^= 1;
        flipped += (int)flip.size();
    }
    if (c[N] || flipped > 8) return false;   // on rubbish it converges to some other codeword
    uint8_t t[N];
    for (int d = 0; d < N; d++) t[N - 1 - d] = c[d];
    if (syndrome(t)) return false;
    memcpy(bits, t, N);
    return true;
}

struct Pkt {
    int64_t frame; int line, field;
    int channel, b5;
    uint8_t d[22];
};

int bcd(int v) { return (v >> 4) * 10 + (v & 15); }

// ---------------------------------------------------------------- 8-unit code
enum Set { KANJI, ALNUM, HIRA, KATA, MOSA, MOSB, MOSC, MOSD, MACRO, DRCS0 = 32 };   // DRCS0 + n = DRCS-n
int set1(int fin)
{
    switch (fin) {
    case 0x4A: case 0x36: return ALNUM;
    case 0x30: case 0x37: return HIRA;
    case 0x31: case 0x38: case 0x49: return KATA;
    case 0x32: return MOSA;
    case 0x33: return MOSB;
    case 0x34: return MOSC;
    case 0x35: return MOSD;
    }
    return ALNUM;
}
// ARIB STD-B24 default macros 60..6F: G0..G3, then LS0 and LS2R
const int kMacro[16][4] = {
    {KANJI, ALNUM, HIRA, MACRO}, {KANJI, KATA, HIRA, MACRO}, {KANJI, DRCS0 + 1, HIRA, MACRO}, {MOSA, MOSC, MOSD, MACRO},
    {MOSA, MOSB, MOSD, MACRO}, {MOSA, DRCS0 + 1, MOSD, MACRO}, {DRCS0 + 1, DRCS0 + 2, DRCS0 + 3, MACRO},
    {DRCS0 + 4, DRCS0 + 5, DRCS0 + 6, MACRO}, {DRCS0 + 7, DRCS0 + 8, DRCS0 + 9, MACRO}, {DRCS0 + 10, DRCS0 + 11, DRCS0 + 12, MACRO},
    {DRCS0 + 13, DRCS0 + 14, DRCS0 + 15, MACRO}, {KANJI, DRCS0 + 2, HIRA, MACRO}, {KANJI, DRCS0 + 3, HIRA, MACRO},
    {KANJI, DRCS0 + 4, HIRA, MACRO}, {KATA, HIRA, ALNUM, MACRO}, {ALNUM, MOSA, DRCS0, MACRO}};
// ASCII -> JIS X 0208 (full width forms)
int asc2jis(int c)
{
    if ((c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A)) return 0x2300 + c;
    static const uint16_t sym[] = {0x2121, 0x212A, 0x2149, 0x2174, 0x2170, 0x2173, 0x2175, 0x2147, 0x214A, 0x214B, 0x2176, 0x215C, 0x2124, 0x215D,
                                   0x2125, 0x213F};
    if (c >= 0x20 && c <= 0x2F) return sym[c - 0x20];
    switch (c) {
    case 0x3A: return 0x2127; case 0x3B: return 0x2128; case 0x3C: return 0x2163; case 0x3D: return 0x2161; case 0x3E: return 0x2164;
    case 0x3F: return 0x2129; case 0x40: return 0x2177; case 0x5B: return 0x214E; case 0x5C: return 0x216F; case 0x5D: return 0x214F;
    case 0x5E: return 0x2130; case 0x5F: return 0x2132; case 0x60: return 0x212E; case 0x7B: return 0x2150; case 0x7C: return 0x2143;
    case 0x7D: return 0x2151; case 0x7E: return 0x2141;
    }
    return 0x2121;
}
void utf8(std::string& s, unsigned u)
{
    if (u < 0x80) s += (char)u;
    else if (u < 0x800) { s += (char)(0xC0 | u >> 6); s += (char)(0x80 | (u & 63)); }
    else { s += (char)(0xE0 | u >> 12); s += (char)(0x80 | ((u >> 6) & 63)); s += (char)(0x80 | (u & 63)); }
}
unsigned jisUni(int code)
{
    int r = (code >> 8) - 0x20, c = (code & 0xFF) - 0x20;
    if (r < 1 || r > 94 || c < 1 || c > 94) return 0x3000;
    unsigned u = jtJisUni[(r - 1) * 94 + c - 1];
    return u ? u : 0x3013;   // 〓 for a code with no character
}
template <int CNT, int BYTES>
const uint8_t* findGlyph(const uint16_t (&codes)[CNT], const uint8_t (&bits)[CNT][BYTES], int code)
{
    auto it = std::lower_bound(codes, codes + CNT, (uint16_t)code);
    return it != codes + CNT && *it == code ? bits[it - codes] : nullptr;
}

// 0..7 full, 8 transparent, 9..15 half intensity
const uint32_t kPal[16] = {0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
                           0xFFFFFFFF, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0x808080};
const int W = 248, H = 192, TOP = 12;   // the screen: 248 x 204, a 12-line header row over 192 lines

struct Screen {
    JtesImage* img = nullptr;               // null: text only
    std::map<std::pair<int, int>, std::vector<uint16_t>> drcs;   // (set, char) -> 24 rows of 16 dots
    struct Cell { int y, x; std::string s; };
    std::vector<Cell> text;
    char size = 'N';
    int fg = 7, bg = 8, x = 0, y = 24, oy = TOP;
    int rasH = 8, rasB = 8;                  // raster colours of the header row and of the page (8: none)
    std::vector<uint32_t> base;              // the picture plane (raster, photographic) under the text plane
    bool over = false;                       // non-spacing: transparent dots leave what is there
    std::vector<JtesFrame>* frames = nullptr;  // TIME: the screen so far is a frame of an animation
    int G[4] = {KANJI, ALNUM, HIRA, MACRO}, GL = 0, GR = 2;
    void reset(char sz, int top)
    {
        size = sz; fg = 7; bg = 8; x = 0; oy = top; y = cellH();
        G[0] = KANJI; G[1] = ALNUM; G[2] = HIRA; G[3] = MACRO; GL = 0; GR = 2;
    }
    // S small 8x12, M middle 8x24, N normal 16x24; SZX: T tall 16x48, W wide 32x24, D double 32x48, F 16x20, Q 4x4
    int cellW() const { return size == 'W' || size == 'D' ? 32 : size == 'N' || size == 'T' || size == 'F' ? 16 : size == 'Q' ? 4 : 8; }
    int cellH() const { return size == 'S' ? 12 : size == 'T' || size == 'D' ? 48 : size == 'F' ? 20 : size == 'Q' ? 4 : 24; }
    // draw a glyph of gw x gh dots (rows: gw <= 8 one byte, else two) into the current cell, then advance
    void paint(const uint8_t* rows, int gw, int gh, const std::string& t)
    {
        int cw = cellW(), ch = cellH();
        if (x + cw > W) { x = 0; y += ch; }
        if (!t.empty()) text.push_back({y + oy, x, t});
        if (img) {
            for (int yy = 0; yy < ch; yy++)
                for (int xx = 0; xx < cw; xx++) {
                    int X = x + xx, Y = y - ch + yy + oy;
                    if (X < 0 || Y < 0 || X >= img->w || Y >= img->h) continue;
                    bool on = false;
                    if (rows) {
                        int sx = xx * gw / cw, sy = yy * gh / ch;
                        on = gw <= 8 ? (rows[sy] >> (7 - sx)) & 1 : (rows[sy * 2 + sx / 8] >> (7 - sx % 8)) & 1;
                    }
                    uint32_t c = kPal[on ? fg : bg];
                    size_t at = (size_t)Y * img->w + X;
                    if (c != 0xFFFFFFFF) img->px[at] = c;
                    else if (!over && at < base.size()) img->px[at] = base[at];   // transparent: the picture shows
                }
        }
        x += cw;
    }
    void jis(int code)
    {
        std::string t;
        utf8(t, jisUni(code));
        const uint8_t* g = nullptr;
        if (size == 'M' && (g = findGlyph(jtHalfCode, jtHalfBits, code))) return paint(g, 8, 24, t);
        if (size == 'S' && (g = findGlyph(jtSmallCode, jtSmallBits, code))) return paint(g, 8, 12, t);
        paint(findGlyph(jtFullCode, jtFullBits, code), 16, 24, t);   // also squeezed into a half / small cell
    }
    // mosaic A-D: 16 x 24 glyphs; C and D are non-spacing: drawn over the cell without background, no advance
    void mosaic(int set, int c)
    {
        uint8_t rows[48] = {};
        if (c >= 0x21 && c <= 0x7E) {
            const uint16_t* g = jtMosaic[(set - MOSA) * 94 + c - 0x21];
            for (int yy = 0; yy < 24; yy++) { rows[yy * 2] = (uint8_t)(g[yy] >> 8); rows[yy * 2 + 1] = (uint8_t)g[yy]; }
        }
        over = set >= MOSC;
        int keep = bg;
        if (over) bg = 8;
        paint(rows, 16, 24, over ? "" : "\xE2\x96\x92");
        if (over) x -= cellW();
        bg = keep;
        over = false;
    }
    void character(int set, int c, int c2)
    {

        if (set == KANJI) jis(c << 8 | c2);
        else if (set == ALNUM) {
            if (size != 'M' && size != 'S') jis(asc2jis(c));
            else {   // half and small cells: the ASCII glyph of the half / small font is the JIS form
                jis(asc2jis(c));
                if (!text.empty()) { text.back().s.clear(); text.back().s += (char)c; }
            }
        } else if (set == HIRA || set == KATA) {
            // 1-byte kana sets: 21..76 = JIS row 4 / 5, 77..7E the marks both share (ゝ/ヽ ゞ/ヾ ー 。 「 」 、 ・)
            static const uint16_t hiraMarks[8] = {0x2135, 0x2136, 0x213C, 0x2123, 0x2156, 0x2157, 0x2122, 0x2126};
            static const uint16_t kataMarks[8] = {0x2133, 0x2134, 0x213C, 0x2123, 0x2156, 0x2157, 0x2122, 0x2126};
            if (c >= 0x77) jis((set == HIRA ? hiraMarks : kataMarks)[c - 0x77]);
            else jis((set == HIRA ? 0x2400 : 0x2500) | c);
        }
        else if (set >= MOSA && set <= MOSD) mosaic(set, c);
        else if (set >= DRCS0) {
            auto it = drcs.find({set - DRCS0, c});
            uint8_t rows[48] = {};
            if (it != drcs.end())
                for (int r = 0; r < 24; r++) { rows[r * 2] = (uint8_t)(it->second[r] >> 8); rows[r * 2 + 1] = (uint8_t)it->second[r]; }
            paint(rows, 16, 24, "\xE2\x96\xA1");
        } else x += cellW();
    }
    void macro(int c)
    {
        if (c < 0x60 || c > 0x6F) return;
        for (int i = 0; i < 4; i++) G[i] = kMacro[c - 0x60][i];
        GL = 0; GR = 2;
    }
    void clear()
    {
        if (img)
            for (int Y = 0; Y < img->h; Y++) {
                int ras = Y < TOP ? rasH : rasB;
                for (int X = 0; X < W; X++)
                    img->px[(size_t)Y * img->w + X] = ras != 8 ? kPal[ras] : (((X >> 3) + (Y >> 3)) & 1) ? 0x5A5D63 : 0x4A4D53;
            }
        text.erase(std::remove_if(text.begin(), text.end(), [&](const Cell& c) { return c.y >= oy; }), text.end());
    }
    void run(const std::vector<uint8_t>& b)
    {
        size_t i = 0, n = b.size();
        auto at = [&](size_t k) -> int { return k < n ? b[k] : 0x40; };
        while (i < n) {
            int c = b[i++];
            int cw = cellW(), ch = cellH();
            if (c == 0x1B) {
                int f = at(i++);
                if (f == 0x6E) GL = 2;
                else if (f == 0x6F) GL = 3;
                else if (f == 0x7E) GR = 1;
                else if (f == 0x7D) GR = 2;
                else if (f == 0x7C) GR = 3;
                else if (f >= 0x28 && f <= 0x2B) {
                    int g = f - 0x28, fin = at(i++);
                    if (fin == 0x20) { fin = at(i++); G[g] = fin == 0x70 ? MACRO : DRCS0 + std::max(0, fin - 0x40); }
                    else G[g] = set1(fin);
                } else if (f == 0x24) {
                    int g = 0;
                    if (at(i) >= 0x28 && at(i) <= 0x2B) g = at(i++) - 0x28;
                    int fin = at(i++);
                    if (fin == 0x20) { i++; G[g] = DRCS0; }
                    else G[g] = KANJI;
                }
                continue;
            }
            if (c == 0x0E) { GL = 1; continue; }
            if (c == 0x0F) { GL = 0; continue; }
            if (c == 0x19 || c == 0x1D) {
                int set = G[c == 0x19 ? 2 : 3], v = at(i++) & 0x7F, v2 = 0;
                if (set == MACRO) { macro(v); continue; }
                if (set == KANJI) v2 = at(i++) & 0x7F;
                character(set, v, v2);
                continue;
            }
            // 8C .. 8D: what is between is not shown, the position moves on one cell (as on the screen of 005-01)
            if (c == 0x8C) {
                while (i < n && b[i] != 0x8D) i++;
                i++;
                x += cw;
                continue;
            }
            switch (c) {
            case 0x1C: { int r = at(i) - 0x40, col = at(i + 1) - 0x40; i += 2; x = col * cw; y = (r + 1) * ch; continue; }
            case 0x16: x += (at(i++) - 0x40) * cw; continue;
            case 0x08: x -= cw; continue;
            case 0x09: x += cw; continue;
            case 0x0A: y += ch; continue;
            case 0x0B: y -= ch; continue;
            case 0x0D: x = 0; y += ch; continue;
            case 0x0C: clear(); x = 0; y = ch; continue;
            case 0x20: case 0xA0: for (int k = reps(cw); k > 0; k--) paint(nullptr, 16, 24, " "); continue;
            case 0x88: size = 'S'; continue;
            case 0x89: size = 'M'; continue;
            case 0x8A: size = 'N'; continue;
            case 0x8B: {   // SZX
                int v = at(i++);
                size = v == 0x41 ? 'T' : v == 0x44 ? 'W' : v == 0x45 ? 'D' : v == 0x6B ? 'F' : v == 0x60 ? 'Q' : size;
                continue;
            }
            case 0x98: rpc = at(i++) - 0x40; continue;   // RPC: the next character n times, 0 = to the end of the row
            case 0x91: case 0x92: case 0x93: case 0x94: case 0x97: i++; continue;
            case 0x90: {
                int p = at(i++);
                if (p == 0x20) i++;
                else if (p >= 0x40 && p <= 0x4F) fg = p & 15;
                else if (p >= 0x50 && p <= 0x5F) bg = p & 15;
                continue;
            }
            case 0x9B:
                while (i < n && !(b[i] >= 0x40 && b[i] <= 0x6F)) i++;
                i++;
                continue;
            case 0x9D: {   // TIME 20 P: wait (P - 40h) / 10 s; the screen so far is shown that long
                int a = at(i), w = at(i + 1);
                i += 2;
                if (a == 0x20 && frames && img) frames->push_back({*img, (w - 0x40) / 10.0});
                continue;
            }
            }
            if (c >= 0x80 && c <= 0x87) { fg = c - 0x80; continue; }
            if (c < 0x20 || c == 0x7F || c == 0xFF || (c >= 0x80 && c < 0xA0)) continue;
            int set = c < 0x80 ? G[GL] : G[GR];
            if (set == MACRO) { macro(c & 0x7F); continue; }
            int c2 = 0;
            if (set == KANJI) c2 = at(i++) & 0x7F;
            for (int k = reps(cw); k > 0; k--) character(set, c & 0x7F, c2);
        }
    }
    int rpc = -1;
    int reps(int cw)
    {
        int k = rpc < 0 ? 1 : rpc == 0 ? std::max(1, (W - x) / cw) : rpc;
        rpc = -1;
        return k;
    }
};

void loadDrcs(Screen& s, const JtesPage& p)
{
    for (auto& u : p.units) {
        if (u.type != 0x30 || u.data.size() < 4) continue;
        int set = u.data[2] - 0x40, ch = u.data[3];
        for (size_t k = 4; k + 48 <= u.data.size(); k += 48, ch++) {
            std::vector<uint16_t> rows(24);
            for (int r = 0; r < 24; r++) rows[r] = (uint16_t)(u.data[k + 2 * r] << 8 | u.data[k + 2 * r + 1]);
            s.drcs[{set, ch}] = rows;
        }
    }
}
// Photographic (unit 38, "1-layer photographic"): a picture of blocks; header: 00, y (from the bottom of the
// screen), x, cell height, cell width (dots), cells down and across a block, 80 | blocks down (80: drawn top
// down), blocks across. Each block: a byte (80: foreground colours follow, 40: background colours, 20 / 10: two
// more lists, skipped; 08: the lists are (colour, cells) nibble pairs, 0 cells = to the end, else a colour per
// nibble; 04: dot lines follow, 02: run-length coded, else raw bits), the colours per cell, then the dot lines,
// MSB first. A run-coded line starts with white (background) and alternates; codes are read bit by bit into the
// low bits first and looked up in the tables below (runs of 32 and more add a second code); the block's code
// stream ends with an invalid code. Colour 8 = the page raster. (From the original Teletext Rescue; checked
// dot for dot against a screen of page 005-01.)
namespace mh {
const uint8_t WB[256] = {
    8,3,4,4,5,4,5,4,6,0,6,5,5,5,4,6,5,0,0,0,6,0,5,0,6,0,6,5,6,6,0,6, 7,0,0,0,0,0,0,0,8,0,7,0,0,0,0,7,0,0,0,0,7,0,0,0,6,0,7,0,7,7,0,7,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 7,0,0,0,0,0,0,0,7,0,8,0,0,0,0,7,0,0,0,0,7,0,0,0,0,0,8,0,8,8,0,8,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,8,8,0,8};
const int16_t WR[256] = {
    -2,-1,1,4,5,3,7,32,9,0,14,16,6,15,2,96,0,0,0,0,12,0,8,0,10,0,23,64,13,24,0,128, 17,0,0,0,0,0,0,0,27,0,25,0,0,0,0,30,0,0,0,0,20,0,0,0,11,0,26,0,22,28,0,160,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 18,0,0,0,0,0,0,0,19,0,256,0,0,0,0,31,0,0,0,0,21,0,0,0,0,0,320,0,192,384,0,448,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,29,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,288,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,352,0,224,416,0,480};
const uint8_t BB[256] = {
    8,3,2,2,5,5,0,0,4,0,0,0,6,6,0,0,7,0,0,0,6,7,0,0,0,0,0,0,6,6,0,0, 8,0,0,0,0,0,0,0,0,0,0,0,8,8,0,0,8,0,0,0,8,8,0,0,0,0,0,0,8,8,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0, 8,0,0,0,0,0,0,0,0,0,0,0,8,8,0,0,8,0,0,0,8,8,0,0,0,0,0,0,7,8,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 8,0,0,0,0,0,0,0,0,0,0,0,8,8,0,0,8,0,0,0,8,8,0,0,0,0,0,0,8,8,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,8,0,0,0,0,0,0,0,0,0,0, 8,0,0,0,0,0,0,0,0,0,0,0,8,8,0,0,8,0,0,0,8,8,0,0,0,0,0,0,0,8,0,0};
const int16_t BR[256] = {
    -2,3,1,2,5,6,0,0,4,0,0,0,8,32,0,0,9,0,0,0,7,11,0,0,0,0,0,0,16,-1,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,26,256,0,0,18,0,0,0,22,128,0,0,0,0,0,0,30,384,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,0,0,64,0,0,0,0,0,0,0,0,0,0, 13,0,0,0,0,0,0,0,0,0,0,0,28,320,0,0,20,0,0,0,24,192,0,0,0,0,0,0,10,448,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 12,0,0,0,0,0,0,0,0,0,0,0,27,288,0,0,19,0,0,0,23,160,0,0,0,0,0,0,31,416,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,0,0,0,0,96,0,0,0,0,0,0,0,0,0,0, 14,0,0,0,0,0,0,0,0,0,0,0,29,352,0,0,21,0,0,0,25,224,0,0,0,0,0,0,0,480,0,0};

struct Bits {
    const std::vector<uint8_t>& b;
    size_t p;
    int d = 0, n = 0, one = 1, run = 0;
    Bits(const std::vector<uint8_t>& v, size_t at) : b(v), p(at) {}
    int byte() { return p < b.size() ? b[p++] : 0; }
    int raw()
    {
        if (--n > 0) { d <<= 1; return (d & 0x80) != 0; }
        d = byte(); n = 8;
        return (d & 0x80) != 0;
    }
    int code()
    {
        const uint8_t* len = one ? BB : WB;
        const int16_t* rl = one ? BR : WR;
        int v = 0;
        for (int i = 0; i < 8;) {
            v |= raw() << i;
            if (len[v] == ++i) return rl[v];
        }
        return -2;
    }
    void next() { run = code(); if (run >= 32) { int r2 = code(); run = r2 < 0 ? r2 : run + r2; } }
    int dot()
    {
        for (int guard = 0; run <= 0 && guard < 64; guard++) {
            if (run < 0) return one;
            one = !one;
            next();
        }
        run--;
        return one;
    }
};
}

void photo(Screen& s, const std::vector<uint8_t>& b)
{
    if (!s.img || b.size() < 10) return;
    int py = b[1], px0 = b[2], by = b[3], bx = b[4], uy = b[5] & 0x7F, ux = b[6] & 0x7F;
    int fy = b[7] & 0x7F, fx = b[8] & 0x7F, down = b[7] >> 7;
    int n = ux * uy;
    if (n <= 0 || n > 4096 || by <= 0 || bx <= 0) return;
    auto put = [&](int X, int Y, int c) {
        if (c == 8) c = s.rasB;
        if (c == 8 || X < 0 || X >= W || Y < 0 || Y >= s.img->h) return;
        s.img->px[(size_t)Y * s.img->w + X] = kPal[c & 15];
    };
    mh::Bits B(b, 9);
    int Y = (H + TOP - 1) - py;
    for (int a = 0; a < fy && B.p < b.size(); a++) {
        int X = px0;
        for (int k = 0; k < fx; k++) {
            int h = B.byte();
            std::vector<int> cf(n, 7), cb(n, 8);
            auto list = [&](std::vector<int>& v) {
                if (h & 8) {
                    size_t i = 0;
                    int c = 0;
                    while (i < v.size()) {
                        int q = B.byte(), m = q & 15;
                        c = q >> 4;
                        if (!m) break;
                        for (; m > 0 && i < v.size(); m--) v[i++] = c;
                    }
                    while (i < v.size()) v[i++] = c;
                } else
                    for (size_t i = 0; i < v.size(); i += 2) {
                        int q = B.byte();
                        v[i] = q >> 4;
                        if (i + 1 < v.size()) v[i + 1] = q & 15;
                    }
            };
            if (h & 0x80) list(cf);
            if (h & 0x40) list(cb);
            for (int e : {0x20, 0x10}) {
                if (!(h & e)) continue;
                std::vector<int> skip(n);
                list(skip);
            }
            B.n = 0;
            int yy = Y + down;
            for (int u2 = 0; u2 < uy; u2++)
                for (int b2 = 0; b2 < by; b2++) {
                    int xx = X, cell = u2 * ux;
                    B.one = 1; B.run = 0;
                    for (int u1 = 0; u1 < ux; u1++, cell++)
                        for (int b1 = 0; b1 < bx; b1++, xx++) {
                            int bit = (h & 4) ? ((h & 2) ? B.dot() : B.raw()) : 0;
                            put(xx, yy, bit ? cf[cell] : cb[cell]);
                        }
                    yy += down ? 1 : -1;
                }
            if (h & 2)
                for (int guard = 0; B.run != -2 && guard < 100000 && B.p < b.size(); guard++) B.next();
            X += ux * bx;
        }
        Y += (down ? uy : -uy) * by;
    }
}

void draw(Screen& s, const JtesPage& p)
{
    loadDrcs(s, p);
    if (p.raster >= 0) { s.rasH = p.raster >> 4; s.rasB = p.raster & 15; }
    s.reset('N', TOP);
    s.clear();
    for (auto& u : p.units)
        if (u.type == 0x38) photo(s, u.data);
    if (s.img) s.base = s.img->px;
    for (auto& u : p.units) {
        if (u.type == 0x24) {           // the header row: small characters (8 x 12) over the page
            s.reset('S', 0);
            s.run(u.data);
            s.reset('N', TOP);
        } else if (u.type == 0x20) {    // each text unit starts from the defaults (colours, sets, size, position)
            s.reset('N', TOP);
            s.run(u.data);
        }
    }
}
std::string rowsText(std::vector<Screen::Cell> cells)
{
    std::stable_sort(cells.begin(), cells.end(), [](const Screen::Cell& a, const Screen::Cell& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::string out, line;
    int y = -1000;
    for (auto& c : cells) {
        if (c.y - y >= 6) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty() || y > -1000) out += line + "\n";
            line.clear();
            y = c.y;
        }
        line += c.s;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line;
    // drop blank lines at the ends
    while (!out.empty() && out.back() == '\n') out.pop_back();
    size_t a = out.find_first_not_of('\n');
    return a == std::string::npos ? "" : out.substr(a);
}

} // namespace

std::string jtesPageName(const JtesPage& p)
{
    char b[32];
    snprintf(b, sizeof b, "%03d-%02d", p.programme, p.page);
    return b;
}

JtesImage jtesRender(const JtesPage& p)
{
    JtesImage im;
    im.w = W; im.h = H + TOP;
    im.px.assign((size_t)im.w * im.h, 0);
    Screen s;
    s.img = &im;
    draw(s, p);
    return im;
}

std::vector<JtesFrame> jtesFrames(const JtesPage& p)
{
    JtesImage im;
    im.w = W; im.h = H + TOP;
    im.px.assign((size_t)im.w * im.h, 0);
    std::vector<JtesFrame> f;
    Screen s;
    s.img = &im;
    s.frames = &f;
    draw(s, p);
    if (!f.empty()) f.push_back({im, 0});   // the screen after the last TIME (stays, then it starts again)
    return f;
}

std::string jtesText(const JtesPage& p)
{
    Screen s;
    draw(s, p);
    return rowsText(s.text);
}

std::vector<JtesNote> jtesTune(const JtesPage& p)
{
    std::vector<JtesNote> out;
    for (auto& u : p.units) {
        if (u.type != 0x2C) continue;
        const auto& b = u.data;
        double tempo = 0, t = 0;
        int voice = -1, volume = 6;
        for (size_t i = 0; i < b.size();) {
            int c = b[i++];
            if (c >= 0x80) {
                std::vector<int> par;
                int v = -1;
                while (i < b.size() && ((b[i] >= 0x30 && b[i] <= 0x39) || b[i] == 0x3B)) {
                    if (b[i] == 0x3B) { par.push_back(std::max(v, 0)); v = -1; }
                    else v = (v < 0 ? 0 : v * 10) + (b[i] - 0x30);
                    i++;
                }
                if (c == 0x80) tempo = par.size() > 1 ? par[1] : 0;
                else if (c == 0x81) { voice++; t = 0; volume = 6; }
                else if (c == 0x90 && !par.empty()) volume = par[0];
                else if (c == 0x84) break;
                continue;
            }
            if (i >= b.size() || b[i] < 0x40 || b[i] >= 0x80) break;   // not a note pair: the rest is damaged
            int len = b[i++] - 0x40;
            if (tempo <= 0) return {};
            double sec = len * 60.0 / (tempo * 4.0) / 16.0;           // tempo x 4 quarter notes a minute
            if (c != 0x20 && c >= 0x30) {
                JtesNote n;
                n.voice = std::max(voice, 0); n.midi = c - 0x48 + 60; n.volume = volume; n.start = t; n.length = sec;
                out.push_back(n);
            }
            t += sec;
        }
    }
    return out;
}

std::vector<uint8_t> jtesWave(const std::vector<JtesNote>& tune, int rate)
{
    double end = 0;
    for (auto& n : tune) end = std::max(end, n.start + n.length);
    size_t ns = (size_t)((end + 0.3) * rate);
    std::vector<float> mix(ns, 0.f);
    for (auto& n : tune) {
        double f = 440.0 * pow(2.0, (n.midi - 69) / 12.0), amp = 0.12 * std::max(1, n.volume) / 7.0;
        size_t a = (size_t)(n.start * rate), len = (size_t)(n.length * rate);
        for (size_t k = 0; k < len && a + k < ns; k++) {
            double tt = (double)k / rate, ph = fmod(tt * f, 1.0);
            double env = std::min(1.0, tt / 0.005) * exp(-tt * 2.5) * std::min(1.0, (double)(len - k) / (0.01 * rate));
            mix[a + k] += (float)(amp * env * (ph < 0.5 ? 1.0 : -1.0));   // square wave, like the tuner's tone generator
        }
    }
    std::vector<uint8_t> w(44 + ns * 2);
    auto put32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; k++) w[at + k] = (uint8_t)(v >> (8 * k)); };
    auto put16 = [&](size_t at, uint16_t v) { w[at] = (uint8_t)v; w[at + 1] = (uint8_t)(v >> 8); };
    memcpy(&w[0], "RIFF", 4); put32(4, (uint32_t)(36 + ns * 2)); memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
    memcpy(&w[36], "data", 4); put32(40, (uint32_t)(ns * 2));
    for (size_t k = 0; k < ns; k++) {
        int v = (int)lround(std::max(-1.f, std::min(1.f, mix[k])) * 32000);
        put16(44 + k * 2, (uint16_t)(int16_t)v);
    }
    return w;
}

bool jtesDecode(const std::vector<FastPacket>& in, JtesResult& out)
{
    out = JtesResult();
    const auto& table = errorTable();
    std::vector<Pkt> pk;
    uint8_t bits[N];
    for (auto& p : in) {
        if (p.kind != 'E' || p.bytes.size() < 37) continue;
        out.packets++;
        for (int k = 0; k < N; k++) bits[k] = (p.bytes[3 + k / 8] >> (k % 8)) & 1;
        u128 s = syndrome(bits);
        if (s) {
            auto it = table.find(key(s));
            int flipped = 0;
            if (it != table.end()) {
                bits[it->second.first] ^= 1;
                if (it->second.second >= 0) bits[it->second.second] ^= 1;
            } else if (!majorityFix(bits, flipped)) { out.failed++; continue; }
            out.fixed++;
        } else
            out.clean++;
        Pkt q;
        q.frame = p.frame; q.line = p.lineNo; q.field = p.field;
        auto val = [&](int from, int n) { int v = 0; for (int k = 0; k < n; k++) v |= bits[from + k] << k; return v; };
        q.channel = val(0, 8);
        q.b5 = val(8, 6);
        for (int i = 0; i < 22; i++) q.d[i] = (uint8_t)val(14 + 8 * i, 8);
        out.channels[q.channel]++;
        pk.push_back(q);
    }
    // Japanese teletext: most codewords right as received or after a small repair
    if (getenv("TR_JTESSTAT")) fprintf(stderr, "JTES: %lld lines, %lld clean, %lld fixed, %lld failed\n", (long long)out.packets, (long long)out.clean, (long long)out.fixed, (long long)out.failed);
    if (out.packets < 200 || (out.clean + out.fixed) * 10 < out.packets * 6) return false;

    std::stable_sort(pk.begin(), pk.end(), [](const Pkt& a, const Pkt& b) {
        return a.frame != b.frame ? a.frame < b.frame : a.field != b.field ? a.field < b.field : a.line < b.line;
    });
    // data groups per stream (data channel + line); packets go to their place by the continuity counter,
    // a lost packet leaves an empty slot
    struct Group { int channel, line; int64_t frame; std::vector<std::array<uint8_t, 22>> slot; std::vector<char> have; };
    std::vector<Group> groups;
    std::map<std::pair<int, int>, Group> open;
    std::map<std::pair<int, int>, int> lastC;
    for (auto& q : pk) {
        auto k = std::make_pair(q.channel, q.line);
        int c = q.b5 & 15;
        auto lc = lastC.find(k);
        int lost = lc == lastC.end() ? 0 : ((c - lc->second) & 15) - 1;
        lastC[k] = c;
        if (lost < 0) continue;            // the same packet twice
        auto og = open.find(k);
        std::array<uint8_t, 22> d;
        memcpy(d.data(), q.d, 22);
        if ((q.b5 & 0x20) && q.d[0] == 0x01) {
            if (og != open.end()) groups.push_back(std::move(og->second));
            Group g;
            g.channel = q.channel; g.line = q.line; g.frame = q.frame;
            g.slot.push_back(d); g.have.push_back(1);
            open[k] = std::move(g);
            continue;
        }
        if (og == open.end()) continue;
        Group& g = og->second;
        for (int i = 0; i < lost; i++) { g.slot.push_back({}); g.have.push_back(0); }
        g.slot.push_back(d); g.have.push_back(1);
    }
    for (auto& kv : open) groups.push_back(std::move(kv.second));
    out.groups = (int64_t)groups.size();

    // the copies of one page (data channel, programme, page): the record header is in the first packet
    auto isEnd = [](const std::array<uint8_t, 22>& d) { return d[19] == 0x17 || d[19] == 0x03 || d[19] == 0x04; };
    // a block = packets up to one whose D-byte 20 ends it; its D21-22 = CRC-16 (x^16+x^12+x^5+1, LSB first, from 0)
    // over the block: all D-bytes of the packets before, D-bytes 1-20 of the last
    auto blockOk = [&](const std::vector<std::array<uint8_t, 22>>& s, size_t a, size_t e) {
        unsigned c = 0;
        for (size_t i = a; i <= e; i++)
            for (int j = 0; j < (i == e ? 20 : 22); j++) {
                c ^= s[i][j];
                for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
            }
        return c == (unsigned)(s[e][20] | s[e][21] << 8);
    };
    std::map<std::tuple<int, int, int>, std::vector<const Group*>> copies;
    for (auto& g : groups) {
        const auto& d = g.slot[0];
        if (d[7] != 0x1E) continue;
        copies[std::make_tuple(g.channel, (d[10] & 15) * 100 + bcd(d[11]), bcd(d[12]))].push_back(&g);
    }
    std::map<std::tuple<int, int, int>, JtesPage> pages;
    for (auto& kv : copies) {
        auto& cs = kv.second;
        // copies of the same length are the same version; take the version most copies without a lost packet
        // have (a copy that lost the next group's first packet runs on into that group), then most copies,
        // the latest on a tie
        std::map<size_t, int> nlen;
        for (auto* g : cs) {
            bool whole = std::all_of(g->have.begin(), g->have.end(), [](char h) { return h != 0; });
            nlen[g->slot.size()] += 1 + (whole && g->slot.size() > 1 ? 1000 : 0);   // not a lone header packet
        }
        size_t L = 0;
        int best = -1;
        for (auto* g : cs)
            if (nlen[g->slot.size()] >= best) best = nlen[g->slot.size()], L = g->slot.size();
        std::vector<const Group*> use;
        for (auto* g : cs) if (g->slot.size() == L) use.push_back(g);
        // packet by packet: the value most copies agree on
        std::vector<std::array<uint8_t, 22>> s(L);
        std::vector<char> have(L, 0);
        for (size_t i = 0; i < L; i++) {
            std::map<std::array<uint8_t, 22>, int> votes;
            for (auto* g : use) if (g->have[i]) votes[g->slot[i]]++;
            int bv = 0;
            for (auto& v : votes) if (v.second > bv) bv = v.second, s[i] = v.first, have[i] = 1;
        }
        if (const char* dbg = getenv("TR_JTESDBG")) {
            char nm[16];
            snprintf(nm, sizeof nm, "%03d-%02d", std::get<1>(kv.first), std::get<2>(kv.first));
            if (!strcmp(dbg, nm))
                for (auto* g : cs) {
                    fprintf(stderr, "%s copy frame %lld line %d slots %zu:", nm, (long long)g->frame, g->line, g->slot.size());
                    for (size_t a = 0; a < g->slot.size();) {
                        size_t e = a;
                        while (e + 1 < g->slot.size() && !(g->have[e] && isEnd(g->slot[e]))) e++;
                        bool all = true;
                        for (size_t i = a; i <= e; i++) all &= g->have[i] != 0;
                        fprintf(stderr, " %zu-%zu:%s", a, e, !all ? "gap" : isEnd(g->slot[e]) && blockOk(g->slot, a, e) ? "ok" : "BAD");
                        a = e + 1;
                    }
                    fprintf(stderr, "\n");
                }
        }
        // blocks: the first packet is the record header block (D-bytes 8-19, CRC in 21-22); then a data unit per
        // block: STX 1F type length(2) data, rounded up to whole packets with the end code and CRC in the last 3
        // bytes. One that fails its CRC is taken whole from a copy where it passes.
        auto span = [&](const std::vector<std::array<uint8_t, 22>>& v, const std::vector<char>& hv, size_t a) -> size_t {
            if (a == 0) return 1;
            if (!hv[a] || v[a][0] != 0x02) return 0;
            size_t len = (size_t)(v[a][3] & 0x7F) << 8 | v[a][4];
            return (len + 8 + 21) / 22;
        };
        auto crcOk = [&](const std::vector<std::array<uint8_t, 22>>& v, const std::vector<char>& hv, size_t a, size_t n) {
            if (a + n > v.size()) return false;
            for (size_t i = a; i < a + n; i++) if (!hv[i]) return false;
            return blockOk(v, a, a + n - 1);
        };
        std::vector<std::vector<uint8_t>> blocks;
        bool gaps = false;
        for (size_t a = 0; a < L;) {
            size_t n = span(s, have, a);
            bool ok = n && crcOk(s, have, a, n);
            if (!ok)
                for (auto* g : use) {
                    std::vector<char> gh(g->have.begin(), g->have.end());
                    size_t m = span(g->slot, gh, a);
                    if (m && crcOk(g->slot, gh, a, m)) {
                        for (size_t i = a; i < a + m; i++) s[i] = g->slot[i], have[i] = 1;
                        n = m; ok = true;
                        break;
                    }
                }
            if (!n) {                      // the head of the block is lost: on to the next packet
                gaps = true;
                a++;
                continue;
            }
            gaps |= !ok;
            std::vector<uint8_t> blk;
            for (size_t i = a; i < a + n && i < L; i++) {
                std::array<uint8_t, 22> d = have[i] ? s[i] : std::array<uint8_t, 22>{};
                size_t from = i == 0 ? 7 : 0;
                blk.insert(blk.end(), d.begin() + from, d.end());
            }
            if (a == 0 && blk.size() > 3) blk.resize(blk.size() - 3);   // end code, CRC
            blocks.push_back(std::move(blk));
            a += n;
        }
        if (blocks.empty() || blocks[0].size() < 6 || blocks[0][0] != 0x1E) continue;
        const auto& h = blocks[0];
        JtesPage p;
        p.channel = use.back()->channel; p.line = use.back()->line; p.frame = use.back()->frame; p.gaps = gaps;
        p.seen = (int)cs.size();
        p.hi = h[1];
        p.programme = (h[3] & 15) * 100 + bcd(h[4]);
        p.page = bcd(h[5]);
        p.header = h;
        if (h.size() > 11 && h[1] == 0x21) p.raster = h[11];
        for (size_t bi = 1; bi < blocks.size(); bi++) {
            const auto& b = blocks[bi];
            size_t j = 0;
            while (j < b.size() && b[j] == 0x02) j++;
            while (j + 3 < b.size() && b[j] == 0x1F) {
                JtesUnit u;
                u.type = b[j + 1];
                size_t len = (size_t)(b[j + 2] & 0x7F) << 8 | b[j + 3];   // b8 of the first byte: link flag
                size_t end = std::min(b.size(), j + 4 + len);
                u.data.assign(b.begin() + j + 4, b.begin() + end);
                // a second header row: the group ran on into the next page (its first packet was lost)
                if (u.type == 0x24 && std::any_of(p.units.begin(), p.units.end(), [](const JtesUnit& v) { return v.type == 0x24; })) {
                    bi = blocks.size();
                    p.gaps = true;
                    break;
                }
                p.units.push_back(std::move(u));
                j = end;
            }
        }
        bool text = false;
        for (auto& u : p.units) text |= u.type == 0x20 || u.type == 0x24;
        if (!text) continue;
        pages.emplace(kv.first, std::move(p));
    }
    for (auto& kv : pages) {
        JtesPage& p = kv.second;
        for (auto& u : p.units)
            if (u.type == 0x24) {
                JtesPage h;
                h.units.push_back(u);
                p.title = jtesText(h);
                break;
            }
        out.pages.push_back(std::move(p));
    }

    char b[300];
    snprintf(b, sizeof b, "Japanese teletext (JTES, BT.653 system D): %lld lines, %lld right as received, %lld repaired (1-2 bit), %lld failed",
             (long long)out.packets, (long long)out.clean, (long long)out.fixed, (long long)out.failed);
    out.report.push_back(b);
    std::string ch;
    for (auto& kv : out.channels) { snprintf(b, sizeof b, "%s%02X (%lld)", ch.empty() ? "" : ", ", kv.first, (long long)kv.second); ch += b; }
    out.report.push_back("  data channels: " + ch);
    std::map<int, int> perProg;
    for (auto& p : out.pages) perProg[p.programme]++;
    int tunes = 0;
    for (auto& p : out.pages) tunes += !jtesTune(p).empty();
    snprintf(b, sizeof b, "  %lld data groups; %zu pages in %zu programmes; %d pages play a tune (unit 2C)", (long long)out.groups, out.pages.size(),
             perProg.size(), tunes);
    out.report.push_back(b);
    // the programme's name from its cleanest header row (no missing packets, no unknown codes)
    std::map<int, std::string> progTitle;
    std::map<int, int> progScore;
    for (auto& p : out.pages) {
        int score = (p.gaps ? 0 : 2) + (p.title.find("\xE3\x80\x93") == std::string::npos ? 1 : 0) + (p.title.empty() ? -9 : 0);
        if (!progTitle.count(p.programme) || score > progScore[p.programme]) progTitle[p.programme] = p.title, progScore[p.programme] = score;
    }
    for (auto& kv : progTitle) {
        snprintf(b, sizeof b, "    %03d (%d pages)  ", kv.first, perProg[kv.first]);
        out.report.push_back(b + kv.second);
    }
    return true;
}

int jtesExport(const JtesResult& r, const std::string& dir)
{
    make_dirs(dir);
    std::string index;
    int n = 0;
    for (auto& p : r.pages) {
        std::string name = jtesPageName(p);
        if (p.channel != 0x92) { char b[8]; snprintf(b, sizeof b, "_%02X", p.channel); name += b; }
        JtesImage im = jtesRender(p);
        Image out(im.w, im.h);
        out.px = im.px;
        png_save(path_join(dir, name + ".png"), out.scaled(im.w * 3, im.h * 3));
        auto frames = jtesFrames(p);    // an animated page: every screen too, <page>_f01.png ..
        for (size_t k = 0; k < frames.size(); k++) {
            Image f(frames[k].img.w, frames[k].img.h);
            f.px = frames[k].img.px;
            char fn[16];
            snprintf(fn, sizeof fn, "_f%02zu.png", k + 1);
            png_save(path_join(dir, name + fn), f.scaled(f.w * 3, f.h * 3));
        }
        std::string text = jtesText(p);
        write_text(path_join(dir, name + ".txt"), text + "\n");
        if (getenv("TR_JTESRAW")) {     // the units as received: type, length (2), data
            Bytes raw;
            for (auto& u : p.units) {
                raw.push_back((uint8_t)u.type);
                raw.push_back((uint8_t)(u.data.size() >> 8));
                raw.push_back((uint8_t)u.data.size());
                raw.insert(raw.end(), u.data.begin(), u.data.end());
            }
            write_file(path_join(dir, name + ".units"), raw);
        }
        auto tune = jtesTune(p);
        if (!tune.empty()) {
            auto w = jtesWave(tune);
            write_file(path_join(dir, name + ".wav"), Bytes(w.begin(), w.end()));
        }
        char b[96];
        snprintf(b, sizeof b, "%s  channel %02X line %d  received %d%s%s  ", name.c_str(), p.channel, p.line, p.seen, p.gaps ? "  (packets missing)" : "",
                 tune.empty() ? "" : "  tune");
        index += b + p.title + "\n";
        n++;
    }
    std::string head;
    for (auto& l : r.report) head += l + "\n";
    write_text(path_join(dir, "index.txt"), head + "\n" + index);
    return n;
}
