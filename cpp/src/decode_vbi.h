// Декодирование записи VBI bt8x8 PAL в поток .t42 и проект страниц.
#pragma once
#include "mlse.h"
#include "vbidecode.h"
#include <random>

bool wst_confident(const u8 *p);              // адрес точный, ошибок чётности не больше одной
int wst_par_bad(const u8 *p);
MlseModel wst_mlse_model();                   // MLSE для WST PAL

struct LineRes { int f, l; std::array<u8, 42> b; double off; bool has_off; };

class VbiReader {
public:
    VbiReader(const std::string &path, int lpf, bool use_gpu, Progress &pr);
    ~VbiReader();
    int frames() const { return N; }
    const u8 *line(int f, int l) const { return mf.data() + ((uint64_t)f * lpf + l) * 2048; }
    std::vector<std::pair<int, int>> sample(int n_frames = 1500, int n_lines = 2500);
    double prepare(const std::vector<std::pair<int, int>> &cand, bool train);   // < 0 — не читается
    std::map<std::pair<int, int>, std::array<u8, 42>> decode_all(const std::vector<int> *frames = nullptr);
    bool gpu_on() const { return (bool)gpu; }
    double win_lo = 113, win_hi = 121;
    bool fallback = false, do_repair = true;
    std::vector<std::pair<int, int>> warm;   // строки, уверенно прочитанные MLSE при калибровке: на них копится модель для видеокарты
private:
    std::vector<LineRes> tpl_lines(const std::vector<std::pair<int, int>> &sel);
    std::vector<LineRes> mlse_lines(const std::vector<std::pair<int, int>> &sel, double lo, double hi);
    int train(const std::vector<LineRes> &res);
    double calibrate(const std::vector<std::pair<int, int>> &cand);
    void set_templates(const tpl::Templates &T);
    MappedFile mf; int lpf, N;
    Progress &pr;
    tpl::Decoder dec;
    std::unique_ptr<tpl::GpuDecoder> gpu;
};

// запись -> out/stream.t42, line_pkt.npy, проект страниц. Бросает runtime_error.
void decode_vbi_project(const std::string &vbi, const std::string &out, int lpf, bool use_gpu, bool repair, Progress &pr);
float line_std(const u8 *y, int n);
