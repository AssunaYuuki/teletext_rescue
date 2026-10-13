#include "colour.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

ColourInfo detectColour(VbiFile& vf)
{
    ColourInfo ci;
    const VbiGeometry& g = vf.geom();
    if (!g.pal) { ci.system = "NTSC"; ci.why = "525 lines"; return ci; }
    const double fs = g.lineSamples * g.lineRate;
    const int n = std::min(g.validSamples, (int)(fs * 52e-6));   // at most one active line
    if (n < 256) return ci;
    // power at 3.80 .. 4.90 MHz in 5 kHz steps (Hann window, direct DFT at those frequencies)
    std::vector<double> freqs;
    for (double f = 3.80e6; f <= 4.90e6; f += 5e3) freqs.push_back(f);
    std::vector<float> co(freqs.size() * n), si(freqs.size() * n), win(n);
    for (int i = 0; i < n; i++) win[i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / (n - 1));
    for (size_t k = 0; k < freqs.size(); k++)
        for (int i = 0; i < n; i++) {
            double ph = 2 * M_PI * freqs[k] * i / fs;
            co[k * n + i] = (float)std::cos(ph) * win[i];
            si[k * n + i] = (float)std::sin(ph) * win[i];
        }
    const int NF = (int)std::min<int64_t>(g.frames, 48);
    std::vector<std::vector<double>> peaks(g.linesPerFrame);   // per line index: peak frequency per frame (kHz)
    std::vector<uint8_t> buf;
    std::vector<float> x(n);
    for (int fi = 0; fi < NF; fi++) {
        int64_t f = g.frames * fi / NF;
        if (vf.read(f, 1, buf) < 1) continue;
        for (int l = 0; l < g.linesPerFrame; l++) {
            const uint8_t* p = &buf[(size_t)l * g.lineBytes];
            double mean = 0, var = 0;
            for (int i = 0; i < n; i++) { x[i] = g.fmt ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) / 256.0f : p[i]; mean += x[i]; }
            mean /= n;
            for (int i = 0; i < n; i++) { x[i] -= (float)mean; var += x[i] * x[i]; }
            if (var / n < 4) continue;   // flat line
            double best = 0;
            int bk = -1;
            for (size_t k = 0; k < freqs.size(); k++) {
                double re = 0, im = 0;
                const float* c = &co[k * n];
                const float* s = &si[k * n];
                for (int i = 0; i < n; i++) { re += x[i] * c[i]; im += x[i] * s[i]; }
                double pw = re * re + im * im;
                if (pw > best) { best = pw; bk = (int)k; }
            }
            // tone amplitude against the line's spread: only lines that really carry a subcarrier
            double amp = 2 * std::sqrt(best) / (n * 0.5);
            if (bk >= 0 && amp > 0.25 * std::sqrt(var / n)) peaks[l].push_back(freqs[bk] / 1e3);
        }
    }
    int idLines = 0, altLines = 0;
    for (int l = 0; l < g.linesPerFrame; l++) {
        auto& v = peaks[l];
        if ((int)v.size() < NF / 4) continue;
        int pal = 0, id = 0;
        std::map<int, int> modes;   // 10 kHz bins
        for (double f : v) {
            pal += std::fabs(f - 4433.6) < 8;
            id += std::fabs(f - 3900) < 40 || std::fabs(f - 4756) < 40;
            modes[(int)std::lround(f / 10)]++;
        }
        if (pal >= 0.7 * v.size()) { ci.palLines++; continue; }
        if (id >= 0.3 * v.size()) { ci.secamLines++; idLines++; continue; }
        // two subcarriers that take turns: D'B and D'R on alternate lines
        std::vector<std::pair<int, int>> m(modes.begin(), modes.end());
        std::sort(m.begin(), m.end(), [](auto& a, auto& b) { return a.second > b.second; });
        // merge neighbouring bins into the strongest two clusters
        int c1 = m[0].first, n1 = 0, n2 = 0, c2 = -1;
        for (auto& kv : m) if (std::abs(kv.first - c1) <= 1) n1 += kv.second;
        for (auto& kv : m) if (std::abs(kv.first - c1) > 5) { c2 = kv.first; break; }
        if (c2 >= 0) for (auto& kv : m) if (std::abs(kv.first - c2) <= 1) n2 += kv.second;
        if (c2 >= 0 && n1 >= 0.1 * v.size() && n2 >= 0.1 * v.size()) { ci.secamLines++; altLines++; }
    }
    char b[200];
    if (ci.secamLines > 0 && ci.secamLines >= ci.palLines) {
        ci.system = "SECAM";
        snprintf(b, sizeof b, "%d lines with SECAM line identification (3.90 / 4.75 MHz), %d with alternating D'B / D'R subcarriers",
                 idLines, altLines);
        ci.why = b;
    } else if (ci.palLines > 0) {
        ci.system = "PAL";
        snprintf(b, sizeof b, "%d lines with a fixed 4.4336 MHz subcarrier", ci.palLines);
        ci.why = b;
    } else
        ci.why = "no colour subcarrier in the VBI lines";
    return ci;
}
