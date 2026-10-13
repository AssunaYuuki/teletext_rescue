// Nagravision (Syster): данные доступа в строках гашения PAL (у Premiere и НТВ+ — 14, 15, 327, 328).
// NRZ 284·fH (4,4375 Мбит/с), 28 байт младшим битом вперёд: синхро 55 D0 18 6C, номер части (код Хэмминга,
// 10 частей по кругу 15 FD 73 9B 5E B6 49 A1 02 EA), 21 байт данных, CRC-16 (0xC003, отражённый) по 22 байтам.
// Блок из 10 частей: два сообщения по 84 байта (ECM/EMM) и две части XOR, по которым восстанавливается потерянная.
#pragma once
#include "vbi_probe.h"

double syster_check(const Rec &R, int row, const std::vector<int> &uu);
// чтение строк -> отчёт (txt) и json; -> число верных строк
int syster_report(const Rec &R, const std::vector<int> &rows, const std::string &out_txt, Progress &pr);
