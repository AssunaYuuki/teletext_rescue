#include "util.h"
#include <windows.h>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <numeric>

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::u32string to_u32(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp); i += n;
    }
    return out;
}

std::string from_cp(char32_t c) {
    std::string s;
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    return s;
}

std::string from_u32(const std::u32string &s) {
    std::string out;
    for (char32_t c : s) out += from_cp(c);
    return out;
}

std::string fmt(const char *f, ...) {
    va_list ap; va_start(ap, f);
    char buf[1024];
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < (int)sizeof buf) { va_end(ap2); return std::string(buf, n < 0 ? 0 : n); }
    std::string s(n + 1, 0);
    vsnprintf(s.data(), n + 1, f, ap2); va_end(ap2);
    s.resize(n);
    return s;
}

std::string lower(const std::string &s) {
    std::u32string u = to_u32(s);
    for (auto &c : u) {
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0x410 && c <= 0x42F) c += 32;
        else if (c >= 0x400 && c <= 0x40F) c += 80;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
    }
    return from_u32(u);
}

static bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string rstrip(const std::string &s) {
    size_t b = s.size();
    while (b > 0 && is_space(s[b - 1])) b--;
    return s.substr(0, b);
}
std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}
bool ends_with_i(const std::string &s, const std::string &suf) {
    if (s.size() < suf.size()) return false;
    return lower(s.substr(s.size() - suf.size())) == lower(suf);
}
std::string replace_all(std::string s, const std::string &a, const std::string &b) {
    if (a.empty()) return s;
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
    return s;
}
std::string html_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;"; else o += c;
    }
    return o;
}
std::string hms(double s) {
    long long v = llround(s);
    return fmt("%02lld:%02lld:%02lld", v / 3600, v / 60 % 60, v % 60);
}
std::string pct(double v, int digits) { return fmt("%.*f%%", digits, v * 100); }

std::string path_join(const std::string &a, const std::string &b) { return U(P(a) / P(b)); }
std::string dirname(const std::string &p) { return U(P(p).parent_path()); }
std::string basename(const std::string &p) { return U(P(p).filename()); }
std::string stem_path(const std::string &p) { fs::path x = P(p); return U(x.parent_path() / x.stem()); }
std::string abspath(const std::string &p) { std::error_code ec; return U(fs::absolute(P(p), ec).lexically_normal()); }
bool exists(const std::string &p) { std::error_code ec; return !p.empty() && fs::exists(P(p), ec); }
bool is_dir(const std::string &p) { std::error_code ec; return fs::is_directory(P(p), ec); }
uint64_t file_size(const std::string &p) { std::error_code ec; auto n = fs::file_size(P(p), ec); return ec ? 0 : n; }
void make_dirs(const std::string &p) { std::error_code ec; fs::create_directories(P(p), ec); }

Bytes read_file(const std::string &p) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string read_text(const std::string &p) {
    Bytes b = read_file(p);
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s.erase(0, 3);
    return s;
}
void write_file(const std::string &p, const void *data, size_t n) {
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + p);
    f.write((const char *)data, n);
}
void write_text(const std::string &p, const std::string &s) { write_file(p, s.data(), s.size()); }
void write_atomic(const std::string &p, const std::string &s) {
    std::string tmp = p + ".tmp";
    write_text(tmp, s);
    std::error_code ec;
    fs::rename(P(tmp), P(p), ec);
    if (ec) { fs::remove(P(p), ec); fs::rename(P(tmp), P(p), ec); }
}

MappedFile::MappedFile(const std::string &path) {
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path);
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    n_ = (uint64_t)sz.QuadPart; file_ = f;
    if (n_ == 0) return;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { CloseHandle(f); file_ = nullptr; throw std::runtime_error("cannot map " + path); }
    map_ = m;
    p_ = (const u8 *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!p_) throw std::runtime_error("cannot map view of " + path);
}
MappedFile::~MappedFile() {
    if (p_) UnmapViewOfFile(p_);
    if (map_) CloseHandle((HANDLE)map_);
    if (file_) CloseHandle((HANDLE)file_);
}

static Bytes res_raw(const char *name) {
    HMODULE h = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceA(h, name, MAKEINTRESOURCEA(10));   // RT_RCDATA
    if (!r) throw std::runtime_error(std::string("missing resource ") + name);
    HGLOBAL g = LoadResource(h, r);
    const u8 *p = (const u8 *)LockResource(g);
    return Bytes(p, p + SizeofResource(h, r));
}
std::string resource_text(const char *name) { Bytes b = res_raw(name); return std::string(b.begin(), b.end()); }
Bytes resource_bytes(const char *name) { return res_raw(name); }

Progress &console_progress() {
    static Progress p;
    static bool init = false;
    if (!init) {
        init = true;
        // TR_TIME=1 — секунды от запуска перед каждой строкой (где уходит время)
        static const bool tm = getenv("TR_TIME") != nullptr;
        static const DWORD start = GetTickCount();
        static auto stamp = []() {
            if (!tm) return std::string();
            FILETIME a, b, k, u; GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
            double cpu = ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
            return fmt("[%6.1f cpu %6.1f] ", (GetTickCount() - start) / 1000.0, cpu);
        };
        p.on_step = [](const std::string &t) { std::cout << stamp() << "STEP " << t << std::endl; };
        p.on_log = [](const std::string &t) { std::cout << stamp() << t << std::endl; };
        p.on_progress = [](long long a, long long b, const std::string &s) {
            static long long last = -1; static DWORD t0 = 0;
            DWORD t = GetTickCount();
            if (a == b || t - t0 > 1000 || last > a) { std::cout << stamp() << "PROGRESS " << a << " " << b << " " << s << std::endl; t0 = t; }
            last = a;
        };
    }
    return p;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    double m = v[n / 2];
    if (n % 2 == 0) { double a = *std::max_element(v.begin(), v.begin() + n / 2); m = (m + a) / 2; }
    return m;
}
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double pos = q / 100.0 * (v.size() - 1);
    size_t i = (size_t)floor(pos); double fr = pos - i;
    if (i + 1 >= v.size()) return v.back();
    return v[i] * (1 - fr) + v[i + 1] * fr;
}
double mean(const std::vector<double> &v) {
    if (v.empty()) return 0;
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}
double stdev(const std::vector<double> &v) {
    if (v.empty()) return 0;
    double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return sqrt(s / v.size());
}
double interp(double x, const float *y, int n) {
    if (x < 0 || x > n - 1) return 0;
    int i = (int)x; if (i >= n - 1) return y[n - 1];
    double f = x - i;
    return y[i] * (1 - f) + y[i + 1] * f;
}

bool solve(std::vector<double> A, std::vector<double> &B, int n, int m) {
    for (int c = 0; c < n; c++) {
        int p = c; double best = fabs(A[c * n + c]);
        for (int r = c + 1; r < n; r++) if (fabs(A[r * n + c]) > best) { best = fabs(A[r * n + c]); p = r; }
        if (best < 1e-300) return false;
        if (p != c) {
            for (int k = 0; k < n; k++) std::swap(A[c * n + k], A[p * n + k]);
            for (int k = 0; k < m; k++) std::swap(B[c * m + k], B[p * m + k]);
        }
        for (int r = 0; r < n; r++) {
            if (r == c) continue;
            double f = A[r * n + c] / A[c * n + c];
            if (f == 0) continue;
            for (int k = c; k < n; k++) A[r * n + k] -= f * A[c * n + k];
            for (int k = 0; k < m; k++) B[r * m + k] -= f * B[c * m + k];
        }
    }
    for (int r = 0; r < n; r++) for (int k = 0; k < m; k++) B[r * m + k] /= A[r * n + r];
    return true;
}

std::vector<double> lstsq(const std::vector<double> &X, const std::vector<double> &Y, int r, int c, int m) {
    std::vector<double> A(c * c, 0.0), B(c * m, 0.0);
    for (int i = 0; i < r; i++) {
        const double *x = &X[i * c];
        for (int a = 0; a < c; a++) {
            if (x[a] == 0) continue;
            for (int b = 0; b < c; b++) A[a * c + b] += x[a] * x[b];
            for (int k = 0; k < m; k++) B[a * m + k] += x[a] * Y[i * m + k];
        }
    }
    double tr = 0; for (int a = 0; a < c; a++) tr += A[a * c + a];
    for (int a = 0; a < c; a++) A[a * c + a] += 1e-10 * (tr / c + 1e-30);
    if (!solve(A, B, c, m)) std::fill(B.begin(), B.end(), 0.0);
    return B;
}

using cd = std::complex<double>;
static void fft2(std::vector<cd> &a, bool inv) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / len * (inv ? 1 : -1);
        cd wl(cos(ang), sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t j = 0; j < len / 2; j++) {
                cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
    if (inv) for (auto &x : a) x /= (double)n;
}
std::vector<double> rfft_power(const std::vector<double> &x) {
    size_t n = x.size();
    if (n == 0) return {};
    std::vector<cd> X(n);
    if ((n & (n - 1)) == 0) {
        for (size_t i = 0; i < n; i++) X[i] = x[i];
        fft2(X, false);
    } else {                                           // Bluestein
        size_t m = 1; while (m < 2 * n - 1) m <<= 1;
        std::vector<cd> w(n), a(m), b(m);
        for (size_t k = 0; k < n; k++) { double ang = M_PI * (double)((k * k) % (2 * n)) / n; w[k] = cd(cos(ang), -sin(ang)); }
        for (size_t k = 0; k < n; k++) a[k] = x[k] * w[k];
        b[0] = std::conj(w[0]);
        for (size_t k = 1; k < n; k++) b[k] = b[m - k] = std::conj(w[k]);
        fft2(a, false); fft2(b, false);
        for (size_t i = 0; i < m; i++) a[i] *= b[i];
        fft2(a, true);
        for (size_t k = 0; k < n; k++) X[k] = a[k] * w[k];
    }
    std::vector<double> out(n / 2 + 1);
    for (size_t k = 0; k < out.size(); k++) out[k] = std::norm(X[k]);
    return out;
}

std::string now_str(const char *f) {
    time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
    char buf[64]; strftime(buf, sizeof buf, f, &lt);
    return buf;
}
