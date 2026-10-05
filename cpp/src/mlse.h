// MLSE-детектор с подгонкой линейной модели канала под каждую строку
// (WST PAL, NABTS, лента — один алгоритм с разными параметрами).
#pragma once
#include "util.h"

struct MlseModel {
    double spb = 0;            // отсчётов на бит
    u8 pre[3] = {0, 0, 0};     // вступление + код кадра (младший бит первым)
    int nbytes = 0;            // байт данных
    int L = 3, R = 3;          // соседних бит в модели
    int ntop = 2;              // сколько лучших смещений пробовать
    int refits = 1;            // уточнений модели по всему пакету
    std::function<bool(const u8 *)> good;   // пакет годится для накопления модели
    // ограничение байта i по уже решённым байтам: 0 — любой, 1 — Хэмминг 8/4, 2 — нечётная чётность.
    // Байт перерешается среди допустимых значений по модели канала.
    std::function<int(const u8 *, int)> code;
    int code_kind = 0;         // 1 — code() это правило WST 625 (адрес/заголовок/строки): тогда перерешение идёт на видеокарте
    // производные
    int NT() const { return 24 + nbytes * 8; }
    int NTAP() const { return L + R + 1; }
    int NS() const { return 1 << NTAP(); }
};
constexpr int MLSE_NP = 3;                 // точки чтения внутри бита: 0.3 0.5 0.7

struct MlseResult { Bytes data; double off = 0, met = 0; bool good = false; };

class MlseDecoder {                        // построчный (копит априорную модель)
public:
    explicit MlseDecoder(const MlseModel &m);
    MlseResult decode(const float *line, int n, double lo, double hi, double step = 0.25);
    bool has_prior() const { return !prior.empty(); }
    std::vector<double> prior;             // MLSE_NP × (NTAP+1)
    int n = 0;
    const MlseModel M;
    struct Pre;
private:
    std::shared_ptr<Pre> pre_;
};

class MlseBatch {                          // тысячи строк сразу; Витерби на видеокарте, если есть
public:
    MlseBatch(const MlseModel &m, const std::vector<double> &prior, int n_prior, bool use_gpu, Progress &pr);
    ~MlseBatch();
    size_t batch() const { return batch_; }
    // rows: n строк по len отсчётов; learn — по каким строкам уточнять модель (nullptr — по всем годным)
    std::vector<MlseResult> decode(const std::vector<const float *> &rows, int len, double lo, double hi,
                                   const std::vector<char> *learn = nullptr, double step = 0.25);
    std::vector<double> prior; int n = 0;
    bool on_gpu() const;
private:
    MlseModel M; size_t batch_;
    struct Impl; Impl *p;
};
