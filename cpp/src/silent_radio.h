// Silent Radio (WTTW Chicago, 1989): бегущая строка для светодиодных табло в строке 21.
#pragma once
#include "json.h"
#include "vbi_probe.h"

extern const double SR_BITRATE;
constexpr int SR_W = 112, SR_H = 15;
bool sr_slice_line(const std::vector<float> &y, double T, uint8_t bits[16]);
// запись -> байты строки и номер строки ТВ (row < 0 — найти самому)
std::pair<Bytes, int> sr_read_bytes(const std::string &path, Progress &pr, int row = -1, int parity = -1);
// байты -> папка: index.html (табло), text.txt, packets.json, line21_bytes.bin
std::string sr_save(const std::string &src, const std::string &out, const Bytes &data, int line, Progress &pr);
std::vector<int> sr_glyph(char32_t ch);     // шрифт 5×7 табло: столбцы, бит 0 — верх
