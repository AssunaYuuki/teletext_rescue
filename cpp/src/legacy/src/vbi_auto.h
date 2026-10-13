// Открыть запись .vbi «как есть»: формат и службы строк, всё читаемое — в папку «<запись>_vbi»
// и отчёт report.txt / report.json.
#pragma once
#include "json.h"
#include "util.h"
// -> report.json; формат не распознан — format null
Json vbi_auto(const std::string &src, bool again, Progress &pr);
std::string vbi_report_dir(const std::string &src);
// x.vbi.flac -> распакованная копия в папке результатов (один раз); остальное как есть
std::string vbi_unpacked(const std::string &src, Progress &pr);
// поток NABTS .t33 из файла: .t33 — сам файл, запись .vbi — разбирается (если ещё не разобрана) и берётся её .t33
std::string nabts_stream_of(const std::string &file, Progress &pr);
#include "vbi_probe.h"
#include <array>
// пары байтов строки 21 с верной чётностью: текст (грубо) и число пар; pairs — {c1, c2, место}
std::pair<std::string, int> cc_text(const Rec &R, int row, int parity, std::vector<std::array<int, 3>> *pairs = nullptr);
