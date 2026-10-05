#include "teletext.h"
#include <windows.h>
#include <winnls.h>

namespace {
#include "gen_tables.inc"
}

// ================================================================ Хэмминг
namespace ham {
int dec[256], fix[256];
u8 enc[16];
const u8 CODEWORDS[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
static struct Init {
    Init() {
        for (int d = 0; d < 16; d++) {
            int b[4]; for (int i = 0; i < 4; i++) b[i] = (d >> i) & 1;
            int p1 = 1 ^ b[0] ^ b[2] ^ b[3], p2 = 1 ^ b[0] ^ b[1] ^ b[3], p3 = 1 ^ b[0] ^ b[1] ^ b[2];
            int p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3]);
            int v[8] = {p1, b[0], p2, b[1], p3, b[2], p4, b[3]};
            int w = 0; for (int i = 0; i < 8; i++) w |= v[i] << i;
            enc[d] = (u8)w;
        }
        for (int x = 0; x < 256; x++) {
            dec[x] = fix[x] = -1;
            int cand = -1, ncand = 0;
            for (int d = 0; d < 16; d++) {
                if (enc[d] == x) { dec[x] = fix[x] = d; }
                else if (popcount8(enc[d] ^ x) == 1) { cand = d; ncand++; }
            }
            if (dec[x] < 0 && ncand == 1) fix[x] = cand;
        }
    }
} init_;

static const int POS[18] = {3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23};
int h2418(u8 b0, u8 b1, u8 b2) {
    unsigned w = b0 | (b1 << 8) | (b2 << 16);
    auto bit = [&](int p) { return (w >> (p - 1)) & 1; };
    for (int k : {1, 2, 4, 8, 16}) {
        int s = 0; for (int p = 1; p < 24; p++) if (p & k) s += bit(p);
        if (s % 2 != 1) return -1;
    }
    int s = 0; for (int p = 1; p < 25; p++) s += bit(p);
    if (s % 2 != 1) return -1;
    int v = 0; for (int i = 0; i < 18; i++) v |= bit(POS[i]) << i;
    return v;
}
void enc2418(int data, u8 out[3]) {
    unsigned w = 0;
    for (int i = 0; i < 18; i++) w |= ((data >> i) & 1u) << (POS[i] - 1);
    for (int k : {1, 2, 4, 8, 16}) {
        int s = 0; for (int q = 1; q < 24; q++) if (q & k) s += (w >> (q - 1)) & 1;
        if (s % 2 != 1) w |= 1u << (k - 1);
    }
    int s = 0; for (int q = 0; q < 24; q++) s += (w >> q) & 1;
    if (s % 2 != 1) w |= 1u << 23;
    out[0] = w & 0xFF; out[1] = (w >> 8) & 0xFF; out[2] = (w >> 16) & 0xFF;
}
bool mrag(const u8 *p, int &mag, int &row) {
    int a = dec[p[0]], b = dec[p[1]];
    if (a < 0 || b < 0) return false;
    mag = (a & 7) ? (a & 7) : 8; row = (a >> 3) | (b << 1);
    return true;
}
}

// ================================================================ наборы символов
const std::vector<std::pair<std::string, std::string>> CHARSET_NAMES = {
    {"latin", "Latin (variant from the page flags)"},
    {"cyr2", "Cyrillic \xE2\x80\x94 Russian/Bulgarian"},
    {"cyr1", "Cyrillic \xE2\x80\x94 Serbian/Croatian"},
    {"cyr3", "Cyrillic \xE2\x80\x94 Ukrainian"},
};
const char32_t *g2_latin() { return G2_LATIN; }
const char32_t *nabts_supp() { return NABTS_SUPP; }

static const int NAT_POS[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};

const Charset &charset_table(const std::string &cs, int national) {
    static std::map<std::pair<std::string, int>, Charset> cache;
    if (cs.rfind("cyr", 0) == 0) national = 0;
    auto key = std::make_pair(cs, national);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Charset t;
    const char32_t *cyr = cs == "cyr1" ? CYR1 : cs == "cyr2" ? CYR2 : cs == "cyr3" ? CYR3 : nullptr;
    if (cyr) for (int i = 0; i < 96; i++) t[i] = cyr[i];
    else {
        for (int i = 0; i < 95; i++) t[i] = 0x20 + i;
        t[95] = 0x25A0;
        int n = (national >= 0 && national < 7) ? national : 0;
        for (int k = 0; k < 13; k++) t[NAT_POS[k] - 0x20] = NAT_SUBSETS[n][k];
    }
    return cache[key] = t;
}

int national_of(int d) { return (((d >> 1) & 1) << 2) | (((d >> 2) & 1) << 1) | ((d >> 3) & 1); }

std::map<char32_t, int> charset_reverse(const Charset &t) {
    std::map<char32_t, int> m;
    for (int i = 0; i < 96; i++) if (t[i] != 0x25A0 && !m.count(t[i])) m[t[i]] = i + 0x20;
    return m;
}

static bool word_char(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x52F) || c == 0xAA || c == 0xB5 || c == 0xBA;
}

std::string guess_charset(const std::vector<const u8 *> &rows, int national) {
    std::set<std::string> wl(std::begin(WORDS_LATIN), std::end(WORDS_LATIN)), wc(std::begin(WORDS_CYR), std::end(WORDS_CYR));
    const Charset &tl = charset_table("latin", national), &tc = charset_table("cyr2", 0);
    long sl = 0, sc = 0;
    for (const u8 *b : rows) {
        for (int pass = 0; pass < 2; pass++) {
            const Charset &t = pass ? tc : tl;
            std::u32string s;
            for (int k = 0; k < 40; k++) { int c = b[k] & 0x7F; s += c >= 0x20 ? t[c - 0x20] : U' '; }
            std::string low = lower(from_u32(s));
            std::u32string u = to_u32(low), w;
            auto flush = [&]() {
                if (!w.empty()) { std::string x = from_u32(w); if (pass ? wc.count(x) : wl.count(x)) (pass ? sc : sl)++; w.clear(); }
            };
            for (char32_t c : u) { if (word_char(c)) w += c; else flush(); }
            flush();
        }
    }
    return (sc > 1.5 * sl && sc >= 10) ? "cyr2" : "latin";
}

char32_t g2_extract(int code) { return code >= 0x20 && code < 0x80 ? G2_EXTRACT[code - 0x20] : 0; }
char32_t diacritic_mark(int mode) { return mode >= 0 && mode < 16 ? DIA_MARK[mode] : 0; }

std::string compose_nfc(char32_t base, char32_t mark) {
    std::wstring in;
    in += (wchar_t)base;
    if (mark) in += (wchar_t)mark;
    wchar_t out[16];
    int n = NormalizeString(NormalizationC, in.c_str(), (int)in.size(), out, 16);
    if (n <= 0) return from_u32(std::u32string(1, base)) + (mark ? from_cp(mark) : "");
    return narrow(std::wstring(out, n));
}
std::pair<char32_t, char32_t> nfd_split(const std::string &ch) {
    std::wstring w = widen(ch);
    wchar_t out[16];
    int n = NormalizeString(NormalizationD, w.c_str(), (int)w.size(), out, 16);
    std::u32string u = n > 0 ? to_u32(narrow(std::wstring(out, n))) : to_u32(ch);
    if (u.empty()) return {0, 0};
    return {u[0], u.size() > 1 ? u[1] : 0};
}
char32_t nfd_base(const std::string &ch) { return nfd_split(ch).first; }

char32_t cc_special(int c) {
    for (auto &p : CC_SPECIAL) if (p.first == c) return p.second;
    return 0;
}

// ================================================================ уровень 1
std::vector<Cell> level1_cells(const Rows &rows, const Charset &t, const Charset *t2, const Over *over,
                               bool *has_flash_out, bool *has_box_out) {
    std::vector<Cell> out;
    bool has_flash = false, has_box = false, skip_row = false;
    for (int r = 0; r < 25; r++) {
        if (skip_row) { skip_row = false; continue; }
        auto it = rows.find(r);
        if (it == rows.end()) continue;
        const Row &b = it->second;
        int fg = 7, bg = 0; bool mos = false, sep = false;
        bool flash = false, conceal = false, hold = false, box = false;
        int held = 0x20; bool held_sep = false;
        int w = 1, h = 1; const Charset *cs = &t; int prev = -1; bool skip_col = false, row_dh = false;
        std::vector<Cell> rc;
        for (int c = 0; c < 40; c++) {
            int ch = b[c] & 0x7F;
            if (ch < 0x20) {
                if (ch == 0x09) flash = false;
                else if (ch == 0x0C) { if (w != 1 || h != 1) held = 0x20; w = 1; h = 1; }
                else if (ch == 0x18) conceal = true;
                else if (ch == 0x19) sep = false;
                else if (ch == 0x1A) sep = true;
                else if (ch == 0x1C) bg = 0;
                else if (ch == 0x1D) bg = fg;
                else if (ch == 0x1E) hold = true;
                if (!skip_col) {
                    Cell cell; cell.r = r; cell.c = c; cell.fg = fg; cell.bg = bg; cell.flash = flash; cell.conceal = conceal;
                    cell.w = 1; cell.h = h; cell.box = box;
                    if (hold && mos && held != 0x20) { cell.mosaic = held; cell.sep = held_sep; cell.text = ""; }
                    rc.push_back(cell);
                }
                skip_col = false;
                if (ch <= 0x07) { fg = ch; mos = false; conceal = false; held = 0x20; }
                else if (ch == 0x08) { flash = true; has_flash = true; }
                else if (ch == 0x0A && prev == 0x0A) box = false;
                else if (ch == 0x0B && prev == 0x0B) { box = true; has_box = true; }
                else if (ch == 0x0D || ch == 0x0E || ch == 0x0F) {
                    int nw = ch == 0x0D ? 1 : 2, nh = ch == 0x0E ? 1 : 2;
                    if (r >= 23 && nh == 2) nh = 1;
                    if (nw != w || nh != h) held = 0x20;
                    w = nw; h = nh;
                    if (h == 2) row_dh = true;
                }
                else if (ch >= 0x10 && ch <= 0x17) { fg = ch - 0x10; mos = true; conceal = false; }
                else if (ch == 0x1B && t2) cs = (cs == &t) ? t2 : &t;
                else if (ch == 0x1F) hold = false;
                prev = ch;
                continue;
            }
            prev = ch;
            if (skip_col) { skip_col = false; continue; }
            Cell cell; cell.r = r; cell.c = c; cell.fg = fg; cell.bg = bg; cell.flash = flash; cell.conceal = conceal;
            cell.w = w; cell.h = h; cell.box = box; cell.sep = sep;
            const std::string *ov = nullptr;
            if (over) { auto o = over->find({r, c}); if (o != over->end() && !o->second.empty()) ov = &o->second; }
            if (mos && (ch & 0x20) && !ov) { cell.mosaic = ch; cell.text = ""; held = ch; held_sep = sep; }
            else cell.text = ov ? *ov : from_cp((*cs)[ch - 0x20]);
            rc.push_back(cell);
            if (w == 2) skip_col = true;
        }
        if (row_dh) { for (auto &cell : rc) cell.bh = 2; skip_row = true; }
        out.insert(out.end(), rc.begin(), rc.end());
    }
    if (has_flash_out) *has_flash_out = has_flash;
    if (has_box_out) *has_box_out = has_box;
    return out;
}

std::map<int, std::string> row_texts(const Rows &rows, const Charset &t, const Charset *t2, const Over *over) {
    auto cs = level1_cells(rows, t, t2, over);
    std::map<int, std::vector<std::string>> grid;
    for (auto &cell : cs) {
        auto &line = grid[cell.r];
        if (line.empty()) line.assign(40, " ");
        if (!cell.mosaic) line[cell.c] = cell.text.empty() ? " " : cell.text;
    }
    std::map<int, std::string> out;
    for (auto &kv : grid) {
        std::string s; for (auto &x : kv.second) s += x;
        out[kv.first] = rstrip(s);
    }
    return out;
}
