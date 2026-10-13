// Записи VBI, сжатые FLAC (.vbi.flac, .flac): FLAC здесь — сжатие байтов без потерь, а не звук.
// Один канал, 8 или 16 бит на отсчёт, отсчёты со знаком сдвигаются в беззнаковые (как сохраняют такие записи).
#pragma once
#include "util.h"

bool is_flac(const std::string &path);                       // по метке «fLaC» в начале файла
// распаковать в сырой файл; -> число байт. Кадры распаковываются параллельно.
uint64_t flac_to_raw(const std::string &in, const std::string &out, Progress &pr);
