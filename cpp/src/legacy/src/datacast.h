// Зашифрованная пакетная рассылка 5,7273 Мбит/с (вероятно, PBS National Datacast; WTTW 1989, строки 19, 20, 22):
// вступление 55 55, код кадра 2D, 34 байта (маркер, адрес, 31 байт данных, 00).
#pragma once
#include "vbi_probe.h"

double datacast_check(const Rec &R, int row, int par);
// lines — (запись, чётность или -1); пусто — найти по vbi_probe
std::string datacast_export(const std::string &src, const std::string &out, std::vector<std::pair<int, int>> lines, Progress &pr);
