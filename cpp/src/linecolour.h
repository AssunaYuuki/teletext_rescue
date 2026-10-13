// Colour of the raw VBI lines as a TV shows them (SECAM FM, PAL / NTSC from the colour burst) and the
// false-colour scale of the line viewer; shared by both programs' line viewers.
#pragma once
#include <complex>
#include <cstdint>
#include <vector>

uint32_t heat(int v);   // false colour: black - blue - green - yellow - white
struct SecamLine {
    std::vector<float> y, d, a, f; // luma, colour difference (B-Y or R-Y), subcarrier amplitude, frequency (Hz)
    int type = 0;                 // 0 none, 1 D'B line, 2 D'R line
};
SecamLine secam_line(const std::vector<float> &x, double fs);
void secam_rgb(const SecamLine &a, const SecamLine *b, float black, float scale, uint32_t *out);
struct QamLine {
    std::vector<float> y;
    std::vector<std::complex<float>> c;
    double burst = 0;   // burst phase (radians)
    float bamp = 0;     // burst amplitude (samples); 0 = no burst
};
std::vector<float> box_filter(const std::vector<float> &v, int w);
QamLine qam_line(const std::vector<float> &x, double fs, double fsc);
void qam_rgb(const QamLine &a, bool pal, int s, float black, float scale, uint32_t *out);
