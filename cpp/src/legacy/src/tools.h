// Инструменты: испытательные сигналы (VITS, АЧХ по multiburst), обзор строк записи bt8x8,
// служебные пакеты потока, импорт телетекста из DVB .ts (EN 300 472), AMOL и VITC.
#pragma once
#include "vbi_probe.h"

Json vits_analyse(const std::string &path, const Json &probe_res, Progress &pr);
std::vector<std::string> vits_response_text(const Json &vt);
void vits_analyse_file(const std::string &path, Progress &pr);
void vbi_lines_survey(const std::string &path, Progress &pr);
void service_packets_survey(const std::string &t42, Progress &pr);
// DVB-поток -> .t42 рядом (или out); -> число пакетов
size_t ts_to_t42(const std::string &ts, const std::string &out, Progress &pr);
// AMOL / VITC по всей записи -> текстовый отчёт; -> число прочитанных строк
int amol_report(const Rec &R, int row, int par, const std::string &out, Progress &pr);
// Разбор 48 бит AMOL I. Кадры 0..29 идут по кругу раз в секунду; в большинстве — только адрес кадра.
// Код источника и отметка времени — два раза за цикл (обычно кадры 2 и 3 или 26 и 27; 27-й несёт
// те же данные с инвертированными битами). Кадры 20, 22, 24 (копии в 21, 23, 25) у части каналов несут
// текст: 5 знаков ASCII по 7 бит (старший бит первым), без чётности. Как метку времени их читать нельзя.
struct AmolMsg {
    enum Kind { EMPTY, STAMP, ASCII, OTHER } kind = EMPTY;
    int frame = 0;
    bool inverted = false;                         // данные сообщения переданы инвертированными
    int sid = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0; bool pm = false; int spare = 0;
    std::string text;                              // 5 знаков для ASCII (непечатные — точкой)
    bool printable = false;
    uint64_t msg = 0;                              // 35 бит сообщения как приняты
    bool parity_even = false;                      // чётность битов 12..47 (сообщение + бит чётности)
};
AmolMsg amol_parse(const uint8_t b[48]);
std::string amol_stamp_text(const AmolMsg &m);    // "1/16 9:31:54 PM"
int gemstar_report(const Rec &R, int row, int par, const std::string &out, Progress &pr);
int vitc_report(const Rec &R, int row, int par, const std::string &out, Progress &pr);
