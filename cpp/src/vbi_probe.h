// Что за запись VBI и что в каждой её строке + AMOL, VITC, WST 525 строк.
// Форматы (чипы захвата и .tbc) — таблица VBI_FORMATS в vbi_probe.cpp, формат выбирается автоматически:
//   bt8x8-pal/ntsc  — bttv: 32 записи × 2048 на кадр, 35,47 / 28,64 МГц (строки 7–22, 320–335 / 10–21, 273–284);
//   cx88-pal/ntsc   — cx2388x: 2048 на строку, 18 / 12 строк на поле (6–23, 319–336 / 10–21, 273–284);
//   saa7131         — saa713x PAL: 1440 при 27 МГц, 16 строк на поле;
//   d27-pal         — ivtv, cx18 PAL: 1440 при 27 МГц, 18 строк на поле (6–23, 318–335);
//   cx23885         — cx23885, ivtv, cx18, saa713x NTSC: 12 записей × 1440 на поле, 27 МГц, строки 10–21;
//   em28xx-pal/ntsc — 720 при 13,5 МГц, 18 / 12 строк на поле;
//   4fsc16          — кроп .tbc NTSC: 16 записей × 910 по 16 бит на поле, 14,318 МГц;
//   4fsc16-pal-vbi  — кроп .tbc PAL: 16 записей × 1135 по 16 бит на поле, 17,73 МГц, строки 7–22;
//   tbc-pal/ntsc    — полные поля .tbc: 313 × 1135 / 263 × 910 по 16 бит.
#pragma once
#include "json.h"
#include "util.h"

extern const double FSC_NTSC;

struct VbiFormat {
    const char *name, *label;
    double fs; int rec_len, ns, bytes_per, rec_lines;   // частота, длина записи, полезных отсчётов, байт, записей в блоке
    bool field_unit, ntsc;                               // блок = поле (иначе кадр из двух полей); система
    int first1, n1, first2, n2;                          // строки ТВ: первая и число записей поля 1 и поля 2
    int skip1, skip2;                                    // номер записи первой нужной строки в поле 1 / поле 2 (для полных полей .tbc)
    double scale, t27;                                   // делитель 16-битных отсчётов; начало окна 27 МГц (доля строки)
    double t0_us;                                        // время отсчёта 0 записи от 0H, мкс (v4l2 offset / fs; у .tbc 0)
};
const std::vector<VbiFormat> &vbi_formats();
const VbiFormat *vbi_format(const std::string &name);

struct Rec {
    std::string path, fmt;
    double fs = 0; int ns = 0; std::vector<int> rows; std::map<int, int> tv;
    double scale = 1; std::string unit; int n = 0;
    int rec_lines = 0, rec_len = 0, bytes_per = 1;   // геометрия файла
    bool is_ntsc = false; double t27 = 0;
    Rec(const std::string &path, const std::string &fmt);
    std::vector<float> line(int u, int r) const;
    void line(int u, int r, float *out) const;
    int t27_start() const { return (int)(ns * t27); }
    // строка в сетке .tbc от 0H (4 fsc: 910 отсчётов NTSC, 1135 PAL) — для слайсеров, привязанных к месту в строке
    double t0_us = 0;
    double fs_h() const { return is_ntsc ? 4 * 315e6 / 88 : 4 * 4433618.75; }
    int ns_h() const { return is_ntsc ? 910 : 1135; }
    std::vector<float> line_h(int u, int r) const;
    bool ntsc() const { return is_ntsc; }
    std::shared_ptr<MappedFile> mf;
private:
    void number_by_cc();
};

std::pair<std::string, Json> detect_format(const std::string &path);   // формат или "", кандидаты
Json vbi_probe(const std::string &path, Progress &pr, int units = 240);
double zc_coherence(const float *y, double fs, double rate, int lo, int hi);

// EIA-608: 2 байта или пусто
bool cc_slice(const std::vector<float> &y, double fs, int out[2]);
// AMOL I (48 бит, ~1 Мбит/с, NRZ): биты или пусто
bool amol_slice(const std::vector<float> &y, double fs, uint8_t bits[48]);
// VITC (90 бит, 115 fH): 9 байт (8 данных + CRC) или пусто
bool vitc_slice(const std::vector<float> &y, double fs, double fh, uint8_t bytes[9]);
// строка -> 1440 отсчётов 27 МГц (для NABTS / WST 525)
void resample27(const Rec &R, const std::vector<float> &y, float *out);
// телетекст PAL с любого чипа -> временная запись bt8x8 (32 × 2048, 35,47 МГц) для декодеров WST
void wst_to_bt8x8(const Rec &R, const std::string &out, Progress &pr);
