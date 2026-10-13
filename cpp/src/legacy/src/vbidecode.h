// Декодер телетекста по шаблонам (Витерби по средним формам сигнала, 2^(2W+1) состояний)
// для сырого VBI bt8x8 PAL (2048 отсчётов на строку).
#pragma once
#include "util.h"
#include <array>
#include <thread>

namespace tpl {
constexpr int W = 4, NS = 1 << (2 * W + 1), NPH = 8, NX = 6, NZ = 8, NB = 42 * 8, NTOT = 24 + NB, PREFIX = 160;
extern double SPB;
using Templates = std::vector<double>;     // NZ × NPH × NS × NX
Templates base_templates();                // встроенные (обучены на записи RTL)
std::vector<double> channel_h();           // линейная модель канала (31 отсчёт)
int zone_of(int j);
std::vector<float> norm(const u8 *line, int n);
std::vector<float> norm(const float *line, int n);

struct Result { std::array<u8, 42> data; double off = 0, met = 0; };

class Decoder {
public:
    explicit Decoder(const Templates &T);
    void set_templates(const Templates &T);
    const Templates &templates() const { return T_; }
    Result decode_fast(const std::vector<float> &y, double lo, double hi) const;   // y нормирована
    // починка байтов с ошибкой чётности; -> сколько починено
    int repair(const std::vector<float> &y, double off, std::array<u8, 42> &data) const;
    double viterbi(const std::vector<float> &y, double off, int j0, int j1, u8 *bits) const;   // bits (NB) или nullptr
private:
    Templates T_; std::vector<double> T2_;
};

// Обучение шаблонов по уверенно прочитанным строкам: (нормированная строка, смещение, 336 бит)
Templates train_zoned(const std::vector<std::tuple<std::vector<float>, double, std::vector<u8>>> &samples, double k = 5.0);
double best_offset(const std::vector<float> &y, const std::vector<u8> &bits360, double lo, double hi, double step);

// Видеокарта: тот же decode_fast для тысяч строк.
class GpuDecoder {
public:
    explicit GpuDecoder(const Templates &T);
    ~GpuDecoder();
    void set_templates(const Templates &T);
    std::string name() const;
    // lines: N строк по 2048 отсчётов (float, не нормированы) -> результаты
    std::vector<Result> decode(const std::vector<float> &lines, size_t n, double lo, double hi);
private:
    struct Impl; Impl *p;
};
}

// параллельный цикл на всех ядрах (кроме одного)
void parallel_for(size_t n, const std::function<void(size_t)> &fn, Progress *pr = nullptr, size_t chunk = 1);
std::vector<u8> bits_lsb(const u8 *bytes, int n);
void bytes_lsb(const u8 *bits, int nbytes, u8 *out);
