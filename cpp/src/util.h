// Общие помощники: строки UTF-8/UTF-16, файлы, отображение файла в память,
// форматирование, ход работы (STEP/PROGRESS) и отмена.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;
using u8 = uint8_t;
using Bytes = std::vector<u8>;

// ---------------------------------------------------------------- строки
std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);
std::u32string to_u32(const std::string &s);
std::string from_u32(const std::u32string &s);
std::string from_cp(char32_t c);
std::string fmt(const char *f, ...);
std::string lower(const std::string &s);           // ASCII и кириллица
std::string strip(const std::string &s);
std::string rstrip(const std::string &s);
std::vector<std::string> split(const std::string &s, char sep);
bool ends_with_i(const std::string &s, const std::string &suf);
std::string replace_all(std::string s, const std::string &a, const std::string &b);
std::string html_escape(const std::string &s);
std::string hms(double s);                         // ЧЧ:ММ:СС
std::string pct(double v, int digits = 0);         // 0.123 -> "12%"

// ---------------------------------------------------------------- пути (UTF-8 <-> path)
#ifdef _WIN32
inline fs::path P(const std::string &u) { return fs::path(widen(u)); }
inline std::string U(const fs::path &p) { return narrow(p.wstring()); }
#else                                                // Linux, macOS: пути и так в UTF-8
inline fs::path P(const std::string &u) { return fs::path(u); }
inline std::string U(const fs::path &p) { return p.string(); }
#endif
// fopen по пути в UTF-8 и сдвиг больше 2 ГБ — на всех системах
FILE *ufopen(const std::string &path, const char *mode);
int fseek64(FILE *f, int64_t off, int whence);
std::string path_join(const std::string &a, const std::string &b);
std::string dirname(const std::string &p);
std::string basename(const std::string &p);
std::string stem_path(const std::string &p);        // путь без расширения
std::string abspath(const std::string &p);
bool exists(const std::string &p);
bool is_dir(const std::string &p);
uint64_t file_size(const std::string &p);
void make_dirs(const std::string &p);

// ---------------------------------------------------------------- файлы
Bytes read_file(const std::string &p);
std::string read_text(const std::string &p);
void write_file(const std::string &p, const void *data, size_t n);
inline void write_file(const std::string &p, const Bytes &b) { write_file(p, b.data(), b.size()); }
void write_text(const std::string &p, const std::string &s);
void write_atomic(const std::string &p, const std::string &s);

// Файл, отображённый в память (только чтение).
class MappedFile {
public:
    explicit MappedFile(const std::string &path);
    ~MappedFile();
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    const u8 *data() const { return p_; }
    uint64_t size() const { return n_; }
private:
    void *file_ = nullptr, *map_ = nullptr;
    const u8 *p_ = nullptr;
    uint64_t n_ = 0;
};

// ---------------------------------------------------------------- ресурсы, встроенные в exe
std::string resource_text(const char *name);
Bytes resource_bytes(const char *name);

// ---------------------------------------------------------------- ход работы
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled") {} };

struct Progress {
    std::function<void(const std::string &)> on_step, on_log;
    std::function<void(long long, long long, const std::string &)> on_progress;
    std::atomic<bool> cancel{false};
    void step(const std::string &t) { check(); if (on_step) on_step(t); }
    void log(const std::string &t) { if (on_log) on_log(t); }
    void progress(long long a, long long b, const std::string &s = "") { check(); if (on_progress) on_progress(a, b, s); }
    void check() { if (cancel) throw Cancelled(); }
};
Progress &console_progress();                      // печатает в stdout (для trcli)

// ---------------------------------------------------------------- числа
double median(std::vector<double> v);
double percentile(std::vector<double> v, double q);  // как numpy (линейная интерполяция)
double mean(const std::vector<double> &v);
double stdev(const std::vector<double> &v);
inline int popcount8(unsigned x) { return __builtin_popcount(x & 0xFF); }
inline bool odd_parity(unsigned x) { return popcount8(x) & 1; }
double interp(double x, const float *y, int n);      // np.interp(x, arange(n), y, left=0, right=0)
// Решение A x = b (n×n, по строкам), несколько правых частей m: b (n×m) -> x (n×m). false — вырождена.
bool solve(std::vector<double> A, std::vector<double> &B, int n, int m);
// Наименьшие квадраты: X (r×c) C = Y (r×m) -> C (c×m), через нормальные уравнения с малой регуляризацией.
std::vector<double> lstsq(const std::vector<double> &X, const std::vector<double> &Y, int r, int c, int m);
// Действительное БПФ любой длины: |rfft|^2 (длина n/2+1).
std::vector<double> rfft_power(const std::vector<double> &x);
std::string now_str(const char *f = "%Y-%m-%d %H:%M");
