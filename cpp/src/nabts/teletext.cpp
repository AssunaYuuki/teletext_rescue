#include "teletext.h"

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
    {"latin", "Latin \xE2\x80\x94 Western Europe (English, German, Swedish, Italian, French, Spanish, Czech)"},
    {"latin_e", "Latin \xE2\x80\x94 Eastern Europe (Polish, Hungarian, Czech, Serbian/Croatian, Romanian)"},
    {"latin_tr", "Latin \xE2\x80\x94 Southern Europe (Turkish, Spanish, Italian)"},
    {"latin_bal", "Latin \xE2\x80\x94 Baltic (Estonian, Latvian, Lithuanian)"},
    {"cyr2", "Cyrillic \xE2\x80\x94 Russian/Bulgarian"},
    {"cyr1", "Cyrillic \xE2\x80\x94 Serbian/Croatian"},
    {"cyr3", "Cyrillic \xE2\x80\x94 Ukrainian"},
    {"greek", "Greek"},
    {"hebrew", "Hebrew"},
};
const char32_t *g2_latin() { return G2_LATIN; }
const char32_t *nabts_supp() { return NABTS_SUPP; }

static const int NAT_POS[13] = {0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};

// the national option subsets of the other G0 Latin groups (EN 300 706 table 36), by C12-C14 of the header
// (receivers sold in a region took that region's group: a Polish page says 000 and means Polish)
namespace {
const char32_t NAT_EN[13] = {0xA3, 0x24, 0x40, 0x2190, 0xBD, 0x2192, 0x2191, 0x23, 0x2014, 0xBC, 0x2016, 0xBE, 0xF7};
const char32_t NAT_DE[13] = {0x23, 0x24, 0xA7, 0xC4, 0xD6, 0xDC, 0x5E, 0x5F, 0xB0, 0xE4, 0xF6, 0xFC, 0xDF};
const char32_t NAT_SV[13] = {0x23, 0xA4, 0xC9, 0xC4, 0xD6, 0xC5, 0xDC, 0x5F, 0xE9, 0xE4, 0xF6, 0xE5, 0xFC};   // Swedish/Finnish/Hungarian
const char32_t NAT_IT[13] = {0xA3, 0x24, 0xE9, 0xB0, 0xE7, 0x2192, 0x2191, 0x23, 0xF9, 0xE0, 0xF2, 0xE8, 0xEC};
const char32_t NAT_FR[13] = {0xE9, 0xEF, 0xE0, 0xEB, 0xEA, 0xF9, 0xEE, 0x23, 0xE8, 0xE2, 0xF4, 0xFB, 0xE7};
const char32_t NAT_ES[13] = {0xE7, 0x24, 0xA1, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xBF, 0xFC, 0xF1, 0xE8, 0xE0};   // Portuguese/Spanish
const char32_t NAT_CS[13] = {0x23, 0x16F, 0x10D, 0x165, 0x17E, 0xFD, 0xED, 0x159, 0xE9, 0xE1, 0x11B, 0xFA, 0x161};   // Czech/Slovak
const char32_t NAT_PL[13] = {0x23, 0x144, 0x105, 0x1B5, 0x15A, 0x141, 0x107, 0xF3, 0x119, 0x17C, 0x15B, 0x142, 0x17A};
const char32_t NAT_SR[13] = {0x23, 0xCB, 0x10C, 0x106, 0x17D, 0x110, 0x160, 0xEB, 0x10D, 0x107, 0x17E, 0x111, 0x161};   // Serbian/Croatian/Slovenian
const char32_t NAT_RO[13] = {0x23, 0xA4, 0x162, 0xC2, 0x15E, 0x102, 0xCE, 0x131, 0x163, 0xE2, 0x15F, 0x103, 0xEE};
const char32_t NAT_TR[13] = {0x20A4, 0x11F, 0x130, 0x15E, 0xD6, 0xC7, 0xDC, 0x11E, 0x131, 0x15F, 0xF6, 0xE7, 0xFC};
const char32_t NAT_ET[13] = {0x23, 0xF5, 0x160, 0xC4, 0xD6, 0x17D, 0xDC, 0xD5, 0x161, 0xE4, 0xF6, 0x17E, 0xFC};
const char32_t NAT_LV[13] = {0x23, 0x24, 0x160, 0x117, 0x119, 0x17D, 0x10D, 0x16B, 0x161, 0x105, 0x173, 0x17E, 0x12F};   // Lettish/Lithuanian
// C12-C14 000..111 -> subset, per group
const char32_t *const GROUP_E[8] = {NAT_PL, NAT_DE, NAT_SV, NAT_IT, NAT_FR, NAT_SR, NAT_CS, NAT_RO};
const char32_t *const GROUP_TR[8] = {NAT_EN, NAT_DE, NAT_SV, NAT_IT, NAT_FR, NAT_ES, NAT_TR, NAT_EN};
const char32_t *const GROUP_BAL[8] = {NAT_EN, NAT_DE, NAT_ET, NAT_LV, NAT_FR, NAT_ES, NAT_CS, NAT_EN};
}

const Charset &charset_table(const std::string &cs, int national) {
    static std::map<std::pair<std::string, int>, Charset> cache;
    if (cs.rfind("cyr", 0) == 0) national = 0;
    auto key = std::make_pair(cs, national);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Charset t;
    const char32_t *cyr = cs == "cyr1" ? CYR1 : cs == "cyr2" ? CYR2 : cs == "cyr3" ? CYR3 : nullptr;
    const char32_t *const *grp = cs == "latin_e" ? GROUP_E : cs == "latin_tr" ? GROUP_TR : cs == "latin_bal" ? GROUP_BAL : nullptr;
    if (cyr) for (int i = 0; i < 96; i++) t[i] = cyr[i];
    else if (grp) {
        for (int i = 0; i < 95; i++) t[i] = 0x20 + i;
        t[95] = 0x25A0;
        const char32_t *sub = grp[national & 7];
        for (int k = 0; k < 13; k++) t[NAT_POS[k] - 0x20] = sub[k];
    } else if (cs == "greek") {   // G0 Greek: 0x40-0x7E as ISO 8859-7 0xC0-0xFE
        for (int i = 0; i < 95; i++) t[i] = 0x20 + i;
        t[95] = 0x25A0;
        for (int c = 0x40; c < 0x7F; c++) t[c - 0x20] = 0x390 + (c - 0x40);
        t[0x52 - 0x20] = 0x386;   // U+03A2 is not a letter: the set has Ά there
        t[0x3C - 0x20] = 0xAB; t[0x3E - 0x20] = 0xBB;
    } else if (cs == "hebrew") {   // G0 Hebrew: the English subset, 0x60-0x7A the 27 letters
        for (int i = 0; i < 95; i++) t[i] = 0x20 + i;
        t[95] = 0x25A0;
        for (int k = 0; k < 13; k++) t[NAT_POS[k] - 0x20] = NAT_EN[k];
        for (int c = 0x60; c <= 0x7A; c++) t[c - 0x20] = 0x5D0 + (c - 0x60);
        t[0x7B - 0x20] = 0x20AA;
    } else {
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

// words of 3+ letters that only one group's text has: Polish "i", "w", "na", "do" are the very codes of
// Russian "и", "в", "на", "до", so short words decide nothing between them
static const char *WORDS_E[] = {
    "nie", "się", "jest", "dla", "oraz", "przez", "lub", "tylko", "też", "już", "może", "będzie", "który", "która", "które",
    "kanał", "serwis", "antenowy", "antenowe", "nadajniki", "godz", "informacje", "pogoda", "telewizji", "strona", "strony",
    "wiadomości", "jutro", "dziś", "ceny", "polska", "polski", "warszawa", "kraków", "poznań", "łódź", "gdańsk",
    "jsou", "není", "také", "který", "která", "které", "nebo", "podle", "bude", "jsme", "zprávy", "počasí", "strana", "dnes", "zítra",
    "és", "nem", "egy", "hogy", "műsor", "időjárás", "oldal", "már", "csak", "budapest",
    "este", "pentru", "care", "sunt", "vremea", "știri", "nije", "ili", "kao", "što", "danas", "vrijeme", "vijesti"};
static const char *WORDS_TR[] = {"için", "ile", "bir", "hava", "durumu", "haber", "haberler", "sayfa", "türkiye", "değil", "çok",
                                 "gibi", "daha", "kadar", "sonra", "istanbul", "ankara"};
static const char *WORDS_BAL[] = {"ilm", "uudised", "eesti", "tallinn", "ning", "kõik", "või", "orai", "žinios", "lietuva", "vilnius",
                                  "ir", "laikapstākļi", "ziņas", "latvija", "rīga", "kas", "arī"};
static const char *WORDS_CYR3[] = {"года", "для", "как", "россии", "что", "это", "новости", "погода", "программа", "курс", "москва",
                                   "сегодня", "при", "над", "под", "или", "так", "все", "его", "она", "они", "было", "будет",
                                   "только", "также", "время", "страница", "рублей", "доллар", "области", "президент"};
// lower case of the letters the Latin and Greek sets hold (util lower(): ASCII and Cyrillic only)
static char32_t low32(char32_t c) {
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9)) return c + 0x20;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c == 0x130) return 'i';
    return c;
}

std::string guess_charset(const std::vector<const u8 *> &rows, int national) {
    std::set<std::string> wl(std::begin(WORDS_LATIN), std::end(WORDS_LATIN)), wc(std::begin(WORDS_CYR), std::end(WORDS_CYR));
    std::set<std::string> we(std::begin(WORDS_E), std::end(WORDS_E)), wt(std::begin(WORDS_TR), std::end(WORDS_TR)),
        wb(std::begin(WORDS_BAL), std::end(WORDS_BAL)), wc3(std::begin(WORDS_CYR3), std::end(WORDS_CYR3));
    // 0 Western, 1 Cyrillic, 2 Eastern, 3 Southern, 4 Baltic; long words counted apart for the regional groups
    const Charset *t[5] = {&charset_table("latin", national), &charset_table("cyr2", 0), &charset_table("latin_e", national),
                           &charset_table("latin_tr", national), &charset_table("latin_bal", national)};
    long sc[5] = {0, 0, 0, 0, 0}, longC = 0;
    for (const u8 *b : rows) {
        for (int pass = 0; pass < 5; pass++) {
            std::u32string s;
            for (int k = 0; k < 40; k++) { int c = b[k] & 0x7F; s += c >= 0x20 ? low32((*t[pass])[c - 0x20]) : U' '; }
            std::u32string u = to_u32(lower(from_u32(s))), w;
            auto flush = [&]() {
                if (w.empty()) return;
                std::string x = from_u32(w);
                if (pass == 0 && wl.count(x)) sc[0]++;
                if (pass == 1 && wc.count(x)) sc[1]++;
                if (pass == 1 && wc3.count(x)) longC++;
                if (pass == 2 && we.count(x)) sc[2]++;
                if (pass == 3 && wt.count(x)) sc[3]++;
                if (pass == 4 && wb.count(x)) sc[4]++;
                w.clear();
            };
            for (char32_t c : u) { if (word_char(c)) w += c; else flush(); }
            flush();
        }
    }
    // a regional Latin group: its own words, more than the long Russian ones and the Western ones
    int best = 2;
    for (int g = 3; g < 5; g++) if (sc[g] > sc[best]) best = g;
    if (getenv("TR_CS_DEBUG"))
        fprintf(stderr, "charset words: western %ld, cyrillic %ld (long %ld), eastern %ld, southern %ld, baltic %ld\n", sc[0], sc[1], longC, sc[2], sc[3], sc[4]);
    if (sc[best] >= 6 && sc[best] > 2 * longC && sc[best] * 2 > sc[0]) return best == 2 ? "latin_e" : best == 3 ? "latin_tr" : "latin_bal";
    return (sc[1] > 1.5 * sc[0] && sc[1] >= 10) ? "cyr2" : "latin";
}

char32_t g2_extract(int code) { return code >= 0x20 && code < 0x80 ? G2_EXTRACT[code - 0x20] : 0; }
char32_t diacritic_mark(int mode) { return mode >= 0 && mode < 16 ? DIA_MARK[mode] : 0; }

#include "gen_compose.inc"
// буква + знак ударения <-> одна буква (таблица из Юникода: латиница, кириллица, греческий)
std::string compose_nfc(char32_t base, char32_t mark) {
    if (mark) for (auto &c : COMPOSE) if (c[0] == base && c[1] == mark) return from_cp(c[2]);
    return from_u32(std::u32string(1, base)) + (mark ? from_cp(mark) : "");
}
std::pair<char32_t, char32_t> nfd_split(const std::string &ch) {
    std::u32string u = to_u32(ch);
    if (u.empty()) return {0, 0};
    if (u.size() == 1) for (auto &c : COMPOSE) if (c[2] == u[0]) return {c[0], c[1]};
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
    struct PageWord { size_t i, j; std::u32string la; double sc; bool en, cap, upper, ok; };
    std::vector<PageWord> words;   // words of the page that may be Latin (see the end)
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
        std::vector<std::pair<int, const Charset *>> src;   // per cell of rc: its code and set (letters only), else 0
        auto mark = [&](int code) { while (src.size() < rc.size()) src.push_back({0, nullptr}); if (!src.empty() && code) src.back() = {code, cs}; };
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
            mark(!ov && !cell.mosaic && ch >= 0x40 && ch <= 0x7E ? ch : 0);
            if (w == 2) skip_col = true;
        }
        while (src.size() < rc.size()) src.push_back({0, nullptr});
        // English words on a Cyrillic page (EUR, CHF, http, BaltCom) are sent with the same codes as Russian
        // ones: the words of the row are collected here and judged with the whole page below
        if (t[0x21] >= 0x400) {
            const Charset &lat = charset_table("latin", 0);
            for (size_t i = 0; i < rc.size();) {
                if (!src[i].first || src[i].second != &t) { i++; continue; }
                size_t j = i;
                std::u32string cy, la;
                while (j < rc.size() && src[j].first && src[j].second == &t && rc[j].r == rc[i].r) {
                    cy += t[src[j].first - 0x20]; la += lat[src[j].first - 0x20]; j++;
                }
                if (j - i >= 3) {
                    PageWord w{out.size() + i, out.size() + j, la, 0, false, false, true, true};
                    w.sc = latin_score(cy, la, &w.en);
                    std::map<char32_t, int> n;
                    for (size_t k = 0; k < la.size(); k++) {
                        char32_t c = la[k] | 0x20;
                        n[c]++;
                        w.cap |= k > 0 && la[k] >= 'A' && la[k] <= 'Z' && la[k - 1] >= 'a' && la[k - 1] <= 'z';
                        w.upper &= la[k] >= 'A' && la[k] <= 'Z';
                        // й inside a word (j) and я (q without u) are Russian; a run of one letter is a drawing
                        if ((c == 'j' && k > 0) || (c == 'q' && (k + 1 >= la.size() || (la[k + 1] | 0x20) != 'u'))) w.ok = false;
                    }
                    for (auto &kv : n) if (la.size() >= 4 && kv.second * 2 > (int)la.size()) w.ok = false;
                    if (n.size() < 2) w.ok = false;
                    if (w.ok && w.sc > -50) words.push_back(w);
                }
                i = j;
            }
        }
        if (row_dh) { for (auto &cell : rc) cell.bh = 2; skip_row = true; }
        out.insert(out.end(), rc.begin(), rc.end());
    }
    // a word is Latin when its letter pairs are clearly English (1.5), or a little and the English dictionary
    // knows it (0.7) or it has a capital inside; on a page with two such sure words (a table of currencies, an
    // English lesson) also a word at 0.9, a short capitals one (USD) at 0.6. A word the Russian dictionary
    // knows stays Cyrillic. (Wikipedia words: no Russian word over 0.9; Russian names and acronyms
    // - Херсон, ООН - reach 1.2, so they turn only on such pages.)
    std::set<std::u32string> sure;
    for (auto &w : words) if (w.sc >= 1.5 || (w.en && w.sc >= 0.7)) sure.insert(w.la);
    bool latinPage = sure.size() >= 2;
    for (auto &w : words) {
        bool lat = w.sc >= 1.5 || (w.en && w.sc >= 0.7) || (w.cap && w.sc >= 0.5) ||
                   (latinPage && (w.sc >= 0.9 || (w.en && w.sc >= 0.5) || (w.upper && w.la.size() <= 4 && w.sc >= 0.6)));
        if (lat)
            for (size_t k = w.i; k < w.j && k < out.size(); k++) out[k].text = from_cp(w.la[k - w.i]);
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
