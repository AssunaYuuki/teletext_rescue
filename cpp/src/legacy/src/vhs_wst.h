// WST с сильно размытых записей VHS: MLSE с широкой моделью канала (6 бит назад, 5 вперёд),
// своя модель на каждое поле, страницы собираются по содержимому.
#pragma once
#include "pages.h"
// rows — записи кадра bt8x8 (0..31). Бросает runtime_error, если не читается.
void vhs_wst_project(const std::string &vbi, const std::string &out, const std::vector<int> &rows, Progress &pr);
