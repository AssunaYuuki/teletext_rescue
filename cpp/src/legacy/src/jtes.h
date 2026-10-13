// Японский телетекст (文字放送, система D): 296 бит на строку — вступление 16 бит, кадровый код 11100101,
// 272 бита пакета, защищённых укороченным разностным циклическим кодом (272,190) с мажоритарным декодированием.
// Пакет: 8 бит режима, 4 бита счётчика, флаг начала группы, 22 байта данных. Группа данных — страница:
// блоки по 22 байта (SOH — заголовок группы: программа, страница, цвета; STX — единицы данных: текст, заголовок,
// DRCS, фото), у каждого блока заголовка и в конце каждой единицы данных CRC-16.
#pragma once
#include "json.h"
#include "util.h"
#include <array>

// (272,190): исправить 34 байта пакета (биты младшим первым) на месте; -> число проходов (0 — без ошибок) или -1
int jtes_correct(u8 *d);
bool jtes_syndrome_zero(const u8 *d);

struct JtesPacket { int field = 0, line = 0; std::array<u8, 34> d{}; int fixed = 0; };

// Страница: что передано (номер, заголовок, текст по строкам, DRCS, фото) — для окна и HTML
struct JtesPage {
    int prog = -1, page = -1;         // программа (магазин*1000 + номер) и страница внутри неё
    int first_field = 0, copies = 0;
    std::vector<u8> group;            // блоки по 22 байта
    bool crc_ok = false;
};

// Разбор потока пакетов в группы данных и страницы; отчёт в txt, страницы в json
Json jtes_decode(const std::vector<JtesPacket> &pk, double rate, std::string &report);
// Текст страницы (UTF-8, строки через \n) и картинка 248×204 (палитра 16 цветов) — для PNG/окна
// клетка знака: место и размер в пикселях экрана 248×204, знак Unicode, цвета 0..15 (0xFF — прозрачно),
// вид: 0 — знак шрифта, 1 — мозаика, 2 — DRCS (они уже нарисованы в pix)
struct JtesCell { int x = 0, y = 0, w = 16, h = 24; char32_t c = 0; u8 fg = 7, bg = 0xFF; int kind = 0, code = 0; bool underline = false; };
struct JtesRender { std::string header, text; std::vector<u8> pix; std::vector<JtesCell> cells; int w = 248, h = 204; int page_colours = 0; };
// палитра: 0 чёрный, 1 красный, 2 зелёный, 3 жёлтый, 4 синий, 5 пурпурный, 6 голубой, 7 белый, 8 прозрачный, 9..15 — полутона
inline uint32_t jtes_rgb(int c) { int H = 255, L = 128; int v = c & 7, k = c >= 8 ? L : H; if (c == 8) return 0; return (uint32_t)(((v & 1) ? k : 0) << 16 | ((v & 2) ? k : 0) << 8 | ((v & 4) ? k : 0)); }
JtesRender jtes_render(const std::vector<u8> &group);
