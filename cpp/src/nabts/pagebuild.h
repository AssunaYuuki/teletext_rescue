// Сборка проекта страниц из потока .t42 / .t34 (build_project, export_json, merge,
// extract_extras, quality, subtitles).
#pragma once
#include "pages.h"

using Packet42 = std::array<u8, 42>;
std::vector<Packet42> read_t42(const std::string &path);
void write_t42(const std::string &path, const std::vector<Packet42> &pk);

// Привязка пакетов к строкам записи: кадр × строка -> номер пакета (-1 — нет).
struct LineMap {
    int frames = 0, lpf = 0;
    std::vector<int32_t> v;
    int32_t at(int f, int l) const { return v[(size_t)f * lpf + l]; }
    bool empty() const { return v.empty(); }
};
void save_npy_i32(const std::string &path, const LineMap &m);
LineMap load_npy_i32(const std::string &path);

int t34_to_t42(const std::string &src, const std::string &dst, Progress &pr);
Json extract_extras(const std::vector<Packet42> &st, Progress &pr);
// канал и время передачи: пакет 8/30 (имя в строке состояния, код сети, дата и время UTC) и постоянная часть заголовка
Json service_info(const std::vector<Packet42> &st);
struct ExportOpts { bool field_reset = true, sparse = false; const LineMap *lines = nullptr; int field = 16; };
PagesBuild export_pages(const std::vector<Packet42> &st, const ExportOpts &o, Progress &pr);
PagesBuild merge_pages(const PagesBuild &good, const PagesBuild &full, const Json &extras, Progress &pr);
Json quality_map(const std::string &vbi, const std::vector<Packet42> &st, const LineMap &lp, int lpf, const Json &extras, Progress &pr);

// поток -> папка проекта (pages.json, extras.json, project.json, quality.json).
// lines/vbi — если поток получен из записи .vbi. Бросает std::runtime_error.
void build_project(const std::string &t42, const std::string &out, const std::string &lines_npy, int lpf,
                   const std::string &vbi, Progress &pr);

// Субтитры (.srt) со страницы с флагом C6.
std::vector<std::pair<std::string, int>> subtitle_pages(const std::string &t42);
struct Cue { double a, b; std::string text; };
std::vector<Cue> subtitle_cues(const std::string &t42, const std::string &page, const std::vector<double> &times,
                               const std::string &charset, const std::string &charset2);
std::vector<double> packet_times(size_t n, const std::string &lines, const std::string &vbi, int lpf, const Json *clock);
std::string to_srt(const std::vector<Cue> &cs);
