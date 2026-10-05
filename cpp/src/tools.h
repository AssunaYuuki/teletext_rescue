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
int vitc_report(const Rec &R, int row, int par, const std::string &out, Progress &pr);
