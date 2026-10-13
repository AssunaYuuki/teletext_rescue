// Чтение строк 525-строчных служб с 5,7273 Мбит/с (NABTS — код кадра E7, 33 байта; WST 525 —
// код кадра 27, 34 байта) MLSE-детектором; строки пересчитываются в 27 МГц.
#pragma once
#include "mlse.h"
#include "vbi_probe.h"

enum class Svc525 { NABTS, WST, JTES };   // JTES — японский телетекст (код кадра A7, 34 байта, код (272,190))
MlseModel model525(Svc525 kind);
bool nabts_good_prefix(const u8 *p);
double nabts_check(const Rec &R, int row, const std::vector<int> &uu);
double wst525_check(const Rec &R, int row, const std::vector<int> &uu);
double jtes_check(const Rec &R, int row, const std::vector<int> &uu);
// rows — записи файла с этой службой; -> путь к .t33/.t34 или исключение
void slice525(const Rec &R, const std::vector<int> &rows, Svc525 kind, const std::string &out, Progress &pr);
// то же, но пакеты в память с местом (поле, строка ТВ); для JTES — после исправления кодом (272,190)
#include "jtes.h"
std::vector<JtesPacket> slice_jtes(const Rec &R, const std::vector<int> &rows, Progress &pr);
