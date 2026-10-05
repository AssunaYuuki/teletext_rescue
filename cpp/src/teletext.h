// Телетекст: Хэмминг 8/4 и 24/18, наборы символов G0 (EN 300 706 §15),
// разбор страницы уровня 1 в клетки экрана (§12.2).
#pragma once
#include "util.h"
#include <array>
#include <map>

// ---------------------------------------------------------------- Хэмминг
namespace ham {
extern int dec[256];        // точное кодовое слово -> 0..15, иначе -1
extern int fix[256];        // с исправлением одиночной ошибки -> 0..15, иначе -1
extern u8 enc[16];
extern const u8 CODEWORDS[16];
inline int d(u8 x) { return dec[x]; }
int h2418(u8 b0, u8 b1, u8 b2);         // 18 бит данных или -1
void enc2418(int data, u8 out[3]);
inline u8 odd(int c) { c &= 0x7F; return (u8)(popcount8(c) % 2 == 0 ? c | 0x80 : c); }
// адрес пакета (точные кодовые слова): mag 1..8 (0 -> 8), row; false — не читается
bool mrag(const u8 *p, int &mag, int &row);
}

// ---------------------------------------------------------------- наборы символов
using Charset = std::array<char32_t, 96>;
extern const std::vector<std::pair<std::string, std::string>> CHARSET_NAMES;   // ключ -> название
const Charset &charset_table(const std::string &cs, int national);
int national_of(int ctrl);              // 10-й байт заголовка (C11..C14) -> вариант C12C13C14
std::string guess_charset(const std::vector<const u8 *> &rows, int national = 1);
std::map<char32_t, int> charset_reverse(const Charset &t);
const char32_t *g2_latin();              // G2 латиница (tt_export: для записи X/26), 96 знаков с 0x20
const char32_t *nabts_supp();            // дополнительный набор NAPLPS, 96 знаков с 0x20
char32_t g2_extract(int code);           // G2 латиница для X/26 (0x20..0x7F)
char32_t diacritic_mark(int mode);       // режимы 0x11..0x1F -> комбинирующий знак (или 0)
std::string compose_nfc(char32_t base, char32_t mark);   // буква + знак -> одна буква (если есть)
char32_t nfd_base(const std::string &ch);                // первая буква разложения NFD
std::pair<char32_t, char32_t> nfd_split(const std::string &ch);  // (основа, знак) или (c, 0)
char32_t cc_special(int c);              // EIA-608 особые символы (0 — нет)

// ---------------------------------------------------------------- уровень 1
using Row = std::array<u8, 40>;
using Rows = std::map<int, Row>;           // ряд 0..24 -> 40 кодов
struct Cell {
    int r = 0, c = 0;
    std::string text = " ";   // символ (для мозаики — пусто)
    int mosaic = 0; bool sep = false;
    int fg = 7, bg = 0;
    bool flash = false, conceal = false;
    int w = 1, h = 1, bh = 1;
    bool box = false;
};
using Over = std::map<std::pair<int, int>, std::string>;   // поправки X/26 (ряд, столбец) -> символ
std::vector<Cell> level1_cells(const Rows &rows, const Charset &t, const Charset *t2, const Over *over,
                               bool *has_flash = nullptr, bool *has_box = nullptr);
std::map<int, std::string> row_texts(const Rows &rows, const Charset &t, const Charset *t2, const Over *over);
