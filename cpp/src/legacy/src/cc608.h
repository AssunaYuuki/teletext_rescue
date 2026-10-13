// Подписи EIA/CEA-608 (строка 21): экран 15 строк × 32 знака для служб CC1–CC4 и текстовых T1–T4.
// Поле 1 несёт CC1, CC2, T1, T2; поле 2 — CC3, CC4, T3, T4 и XDS.
// Режимы: всплывающие (pop-on, RCL … EOC), бегущие (roll-up, RU2–RU4 + CR), рисуемые (paint-on, RDC);
// PAC (строка, отступ, цвет, курсив, подчёркивание), коды в строке, табуляция, особые и расширенные знаки.
#pragma once
#include "json.h"
#include "util.h"
#include <array>

struct Cc608Cell { char32_t c = 0; uint8_t a = 0; };    // a: цвет 0..6 | курсив 8 | подчёркнуто 16 | мигание 32
struct Cc608Screen {
    Cc608Cell m[15][32];
    void clear() { for (auto &r : m) for (auto &c : r) c = Cc608Cell(); }
    bool empty() const { for (auto &r : m) for (auto &c : r) if (c.c && c.c != ' ') return false; return true; }
    bool operator==(const Cc608Screen &o) const { for (int r = 0; r < 15; r++) for (int k = 0; k < 32; k++) if (m[r][k].c != o.m[r][k].c || m[r][k].a != o.m[r][k].a) return false; return true; }
    std::string row_text(int r) const;                    // UTF-8 без хвостовых пробелов
};

// Разбор пар байтов одного поля (без бита чётности, только пары с верной чётностью): {c1, c2, место}.
// field2 — поле 2 (службы CC3/CC4/T3/T4). rate — мест в секунду (для времени в расшифровке и SRT).
// Результат: JSON для окна и тексты (расшифровка по службам, SRT по каждой службе подписей).
struct Cc608Result {
    Json json = Json::object();
    std::string transcript;                               // все службы, по времени
    std::map<std::string, std::string> srt;               // "CC1" -> текст .srt
    std::map<std::string, int> lines;                     // служба -> строк в расшифровке
};
Cc608Result cc608_decode(const std::vector<std::array<int, 3>> &pairs, bool field2, double rate);
// Похоже ли на поле 2 (XDS, коды 0x15/0x1D)
bool cc608_looks_field2(const std::vector<std::array<int, 3>> &pairs);
