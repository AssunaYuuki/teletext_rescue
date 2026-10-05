// Открыть запись .vbi «как есть»: формат и службы строк, всё читаемое — в папку «<запись>_vbi»
// и отчёт report.txt / report.json.
#pragma once
#include "json.h"
#include "util.h"
// -> report.json; формат не распознан — format null
Json vbi_auto(const std::string &src, bool again, Progress &pr);
std::string vbi_report_dir(const std::string &src);
