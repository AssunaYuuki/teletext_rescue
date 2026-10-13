// Colour of the raw VBI lines as a TV shows them (see linecolour.h).
#include "linecolour.h"
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// false colour: black - blue - green - yellow - white
uint32_t heat(int v)
{
    static uint32_t lut[256];
    static bool init = false;
    if (!init) {
        const int k[5][3] = {{0, 0, 0}, {20, 40, 200}, {30, 200, 90}, {250, 220, 40}, {255, 255, 255}};
        for (int i = 0; i < 256; i++) {
            double t = i / 255.0 * 4;
            int a = std::min(3, (int)t);
            double f = t - a;
            int r = (int)(k[a][0] + (k[a + 1][0] - k[a][0]) * f), g = (int)(k[a][1] + (k[a + 1][1] - k[a][1]) * f),
                b = (int)(k[a][2] + (k[a + 1][2] - k[a][2]) * f);
            lut[i] = (uint32_t)(r << 16 | g << 8 | b);
        }
        init = true;
    }
    return lut[std::max(0, std::min(255, v))];
}


// SECAM colour of one stored line: luma by a comb on the subcarrier, chroma FM-demodulated
// (quadrature mix at 4.33 MHz, low-pass, phase step = instantaneous frequency). A line carries
// D'B (rest 4.250 MHz, 230 kHz per unit, D'B = 1.505 (B-Y)) or D'R (rest 4.40625 MHz, 280 kHz
// per unit, D'R = -1.902 (R-Y)); which one is told by its mean frequency. The other colour
// difference comes from the neighbouring line, as a SECAM receiver's delay line does.
// The line identification signals ("bottles": D'B down to 3.90, D'R up to 4.75 MHz) come out green.
SecamLine secam_line(const std::vector<float> &x, double fs)
{
    SecamLine L;
    int n = (int)x.size();
    L.y.assign(n, 0); L.d.assign(n, 0); L.a.assign(n, 0);
    if (n < 64) return L;
    auto box = [&](const std::vector<float> &v, int w) {
        std::vector<float> o(n);
        double acc = 0;
        int h = w / 2;
        std::vector<double> c(n + 1, 0);
        for (int i = 0; i < n; i++) c[i + 1] = c[i] + v[i];
        for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n, i - h + w); o[i] = (float)((c[b] - c[a]) / std::max(1, b - a)); }
        (void)acc;
        return o;
    };
    int wc = std::max(2, (int)std::lround(fs / 4.33e6));
    L.y = box(box(x, wc), wc);                       // nulls at the subcarrier
    std::vector<float> c(n), re(n), im(n);
    const double fc = 4.33e6;
    for (int i = 0; i < n; i++) {
        c[i] = x[i] - L.y[i];
        double ph = 2 * M_PI * fc * i / fs;
        re[i] = (float)(c[i] * std::cos(ph));
        im[i] = (float)(-c[i] * std::sin(ph));
    }
    int wl = std::max(4, (int)std::lround(fs / 1.3e6));
    re = box(box(re, wl), wl);
    im = box(box(im, wl), wl);
    std::vector<float> f(n, (float)fc);
    double sumF = 0, sumW = 0;
    for (int i = 1; i < n; i++) {
        double dp = std::atan2(im[i] * re[i - 1] - re[i] * im[i - 1], re[i] * re[i - 1] + im[i] * im[i - 1]);
        f[i] = (float)(fc + dp * fs / (2 * M_PI));
        L.a[i] = 2 * std::sqrt(re[i] * re[i] + im[i] * im[i]);
        if (L.a[i] > 6) { sumF += f[i] * L.a[i]; sumW += L.a[i]; }
    }
    L.f = f;
    if (sumW < n * 2.0) return L;                    // no subcarrier on this line
    double mean = sumF / sumW;
    L.type = mean < 4.328e6 ? 1 : 2;
    for (int i = 0; i < n; i++) {
        double D = L.type == 1 ? (f[i] - 4.25e6) / 230e3 : (f[i] - 4.40625e6) / 280e3;
        D = std::max(-2.5, std::min(2.5, D));
        double w = std::min(1.0, L.a[i] / 20.0);     // fade out where the subcarrier is weak
        L.d[i] = (float)(w * (L.type == 1 ? D / 1.505 : -D / 1.902));
    }
    return L;
}

// RGB of a stored line from its own and a neighbour's colour difference; scale = white - black
void secam_rgb(const SecamLine &a, const SecamLine *b, float black, float scale, uint32_t *out)
{
    int n = (int)a.y.size();
    for (int i = 0; i < n; i++) {
        double Y = (a.y[i] - black) / scale, BY = 0, RY = 0;
        if (a.type == 1) BY = a.d[i]; else if (a.type == 2) RY = a.d[i];
        if (b && b->type && b->type != a.type && i < (int)b->d.size()) { if (b->type == 1) BY = b->d[i]; else RY = b->d[i]; }
        double R = Y + RY, B = Y + BY, G = Y - 0.509 * RY - 0.194 * BY;
        auto q = [](double v) { return (uint32_t)std::max(0, std::min(255, (int)std::lround(v * 255))); };
        out[i] = q(R) << 16 | q(G) << 8 | q(B);
    }
}

// PAL / NTSC colour of one stored line: luma by a comb on the subcarrier, chroma by quadrature mixing
// at fsc (c = amplitude and phase of the subcarrier along the line), referenced to the colour burst
// that follows the line sync (or, when the sync is not in the stored samples, the most steady burst of
// subcarrier in the first 12 microseconds).
std::vector<float> box_filter(const std::vector<float> &v, int w)
{
    int n = (int)v.size(), h = w / 2;
    std::vector<double> c(n + 1, 0);
    for (int i = 0; i < n; i++) c[i + 1] = c[i] + v[i];
    std::vector<float> o(n);
    for (int i = 0; i < n; i++) { int a = std::max(0, i - h), b = std::min(n, i - h + w); o[i] = (float)((c[b] - c[a]) / std::max(1, b - a)); }
    return o;
}
QamLine qam_line(const std::vector<float> &x, double fs, double fsc)
{
    QamLine L;
    int n = (int)x.size();
    if (n < 64) return L;
    int wc = std::max(2, (int)std::lround(fs / fsc));
    L.y = box_filter(box_filter(x, wc), wc);
    std::vector<float> re(n), im(n);
    for (int i = 0; i < n; i++) {
        double ph = 2 * M_PI * fsc * i / fs, v = x[i] - L.y[i];
        re[i] = (float)(v * std::cos(ph));
        im[i] = (float)(-v * std::sin(ph));
    }
    int wl = std::max(4, (int)std::lround(fs / 1.3e6));
    re = box_filter(box_filter(re, wl), wl);
    im = box_filter(box_filter(im, wl), wl);
    L.c.resize(n);
    for (int i = 0; i < n; i++) L.c[i] = {2 * re[i], 2 * im[i]};
    auto us = [&](double t) { return (int)(t * 1e-6 * fs); };
    int head = std::min(n, us(16));
    // the line sync: the lowest stretch at the start, at least 3 us long
    std::vector<float> h(L.y.begin(), L.y.begin() + head);
    std::sort(h.begin(), h.end());
    float lo = h.empty() ? 0 : h[h.size() / 50], blank = h.empty() ? 0 : h[h.size() / 2], cut = (lo + blank) / 2;
    int b0 = -1;
    if (blank - lo > 15) {
        int run = 0;
        for (int i = 0; i < head; i++) {
            if (L.y[i] < cut) run++;
            else { if (run >= us(3)) { b0 = i + us(1.0); break; } run = 0; }
        }
    }
    int bw = us(1.6);
    auto steady = [&](int a) {
        std::complex<double> m = 0; double am = 0;
        for (int i = a; i < a + bw && i < n; i++) { m += std::complex<double>(L.c[i]); am += std::abs(L.c[i]); }
        return std::make_pair(m / (double)bw, am / bw);
    };
    std::complex<double> best = 0;
    if (b0 >= 0 && b0 + bw < n) best = steady(b0).first;
    else
        for (int a = 0; a + bw < std::min(n, us(12)); a += std::max(1, bw / 8)) {
            auto q = steady(a);
            if (q.second > 4 && std::abs(q.first) > 0.9 * q.second && std::abs(q.first) > std::abs(best)) best = q.first;
        }
    if (std::abs(best) > 4) { L.burst = std::arg(best); L.bamp = (float)std::abs(best); }
    return L;
}
// RGB of a PAL / NTSC line. s = +1 / -1: PAL V switch of this line (burst at 135 / 225 degrees from +U);
// NTSC: burst at 180 degrees, s = +1
void qam_rgb(const QamLine &a, bool pal, int s, float black, float scale, uint32_t *out)
{
    int n = (int)a.y.size();
    double alpha = a.burst - M_PI + (pal ? s * M_PI / 4 : 0);   // direction of +U
    std::complex<double> rot = std::polar(1.0, -alpha);
    for (int i = 0; i < n; i++) {
        double Y = (a.y[i] - black) / scale, U = 0, V = 0;
        if (a.bamp > 0) {
            std::complex<double> q = std::complex<double>(a.c[i]) * rot;
            U = q.real() / scale; V = s * q.imag() / scale;
        }
        double BY = U / 0.493, RY = V / 0.877;
        double R = Y + RY, B = Y + BY, G = Y - 0.509 * RY - 0.194 * BY;
        auto qn = [](double v) { return (uint32_t)std::max(0, std::min(255, (int)std::lround(v * 255))); };
        out[i] = qn(R) << 16 | qn(G) << 8 | qn(B);
    }
}

