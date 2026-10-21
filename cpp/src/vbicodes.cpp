#include "vbicodes.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
struct Cni { int code; const char* name; };
const Cni kCni[] = {
#include "vps_cni.inc"
};

float at(const float* y, int n, double t)
{
    int i = (int)t;
    if (i < 0) return y[0];
    if (i + 1 >= n) return y[n - 1];
    double f = t - i;
    return (float)(y[i] * (1 - f) + y[i + 1] * f);
}

// the spread between the low and the high tenth of the line
float swingOf(const float* y, int n)
{
    std::vector<float> s(y, y + n);
    std::sort(s.begin(), s.end());
    return s[n * 9 / 10] - s[n / 10];
}
}

// ---------------------------------------------------------------- VPS
// the threshold is the line's own run-in, not one level for the whole line (a tape's level drifts); the start code
// is taken with a few elements wrong and every place it matches is read - the best reading wins; a half-bit pair
// is "not biphase" when its halves hardly differ. viva.vbi line 16: 743 of 743 lines (one level, first match: 87)
bool vpsRead(const float* y, int n, double fs, uint8_t b[13])
{
    if (n < 100) return false;
    const float sw = swingOf(y, n);
    if (sw < 20) return false;
    const double H = fs / 5e6;
    const uint32_t CRI = 0xAAAA8A99;
    const double end = n - (32 + 13 * 16) * H;
    int bestBad = 1 << 30;
    uint8_t best[13];
    for (double s = 0; s < end; s += 0.25) {
        float h[32], mean = 0;
        for (int k = 0; k < 32; k++) { h[k] = at(y, n, s + (k + 0.5) * H); if (k < 16) mean += h[k]; }
        mean /= 16;   // the run-in 1010..: as many highs as lows
        uint32_t v = 0;
        for (int k = 0; k < 32; k++) v = (v << 1) | (h[k] > mean);
        if (__builtin_popcount((v >> 24) ^ 0xAA) > 1 || __builtin_popcount(v ^ CRI) > 2) continue;
        double p = s + 32 * H;
        int bad = 0;
        uint8_t x[13];
        for (int i = 0; i < 13; i++) {
            int c8 = 0;
            for (int j = 0; j < 8; j++) {
                float a = at(y, n, p + 0.5 * H), c = at(y, n, p + 1.5 * H);
                c8 = (c8 << 1) | (a > c);
                bad += std::fabs(a - c) < 0.15f * sw;
                p += 2 * H;
            }
            x[i] = (uint8_t)c8;
        }
        bad += __builtin_popcount(v ^ CRI);
        if (bad < bestBad) { bestBad = bad; std::copy(x, x + 13, best); }
        if (bad == 0) break;
    }
    if (bestBad > 6) return false;   // 8 lets a noisy line through with wrong bytes, 4 loses half of them (viva.vbi + noise)
    std::copy(best, best + 13, b);
    return true;
}

VpsLabel vpsLabel(const uint8_t b[13])
{
    // b[k] = byte k+3 of the line: 5 = sound, 11-14 = label, 15 = programme type
    VpsLabel l;
    l.cni = ((b[10] & 3) << 10) | ((b[11] & 0xC0) << 2) | (b[8] & 0xC0) | (b[11] & 0x3F);
    int pil = ((b[8] & 0x3F) << 14) | (b[9] << 6) | (b[10] >> 2);
    l.day = (pil >> 15) & 31; l.month = (pil >> 11) & 15; l.hour = (pil >> 6) & 31; l.minute = pil & 63;
    l.pty = b[12];
    l.sound = b[2] >> 6;
    return l;
}

std::string vpsCniName(int cni)
{
    for (auto& c : kCni) if (c.code == cni) return c.name;
    return "";
}

std::string vpsText(const uint8_t b[13])
{
    VpsLabel l = vpsLabel(b);
    std::string name = vpsCniName(l.cni);
    std::string s = (name.empty() ? std::string("network") : name) + fmt(" (CNI %03X)", l.cni);
    // special labels: day 0, month 15 (EN 300 231 table 4)
    if (l.day == 0 && l.month == 15 && l.minute == 63 && l.hour >= 28) {
        static const char* const sp[] = {"continuation code", "interruption code (pause)", "record inhibit (blank code)", "timer control code (no programme label)"};
        s += std::string(" \xC2\xB7 ") + sp[l.hour - 28];
    } else if (l.day >= 1 && l.day <= 31 && l.month >= 1 && l.month <= 12 && l.hour < 24 && l.minute < 60)
        s += fmt(" \xC2\xB7 programme announced for %02d.%02d %02d:%02d", l.day, l.month, l.hour, l.minute);
    else
        s += fmt(" \xC2\xB7 label %02d.%02d %02d:%02d", l.day, l.month, l.hour, l.minute);
    static const char* const snd[] = {"", "mono", "stereo", "two sound channels"};
    if (l.sound) s += std::string(" \xC2\xB7 ") + snd[l.sound];
    if (l.pty != 0xFF && l.pty != 0) s += fmt(" \xC2\xB7 programme type %02X", l.pty);
    s += fmt(" \xC2\xB7 source data %02X %02X", b[0], b[1]);
    return s;
}

// ---------------------------------------------------------------- WSS
// as vpsRead: the run-in's own middle as threshold, every matching start tried (synthetic WSS + noise 35: 559 of
// 600 lines against 385)
bool wssRead(const float* y, int n, double fs, int& bits)
{
    if (n < 100) return false;
    const float sw = swingOf(y, n);
    if (sw < 20) return false;
    const double E = fs / 5e6;
    static const char* const head = "11111000111000111000111000111" "000111100011110000011111";
    const int HL = 53;
    const double end = n - (HL + 14 * 6) * E;
    int bestBad = 1 << 30, bestBits = 0;
    for (double s = 0; s < end; s += 0.25) {
        float h[HL], lo = 1e9f, hi = -1e9f;
        for (int k = 0; k < HL; k++) { h[k] = at(y, n, s + (k + 0.5) * E); if (k < 29) { lo = std::min(lo, h[k]); hi = std::max(hi, h[k]); } }
        if (hi - lo < 0.4f * sw) continue;
        float th = (lo + hi) / 2;   // the run-in's own middle
        int err = 0;
        for (int k = 0; k < HL && err <= 4; k++) err += (h[k] > th) != (head[k] == '1');
        if (err > 4) continue;
        int bv = 0, bad = 0;
        double p = s + HL * E;
        for (int i = 0; i < 14; i++) {
            float a = 0, c = 0;
            for (int j = 0; j < 3; j++) { a += at(y, n, p + (j + 0.5) * E); c += at(y, n, p + (j + 3.5) * E); }
            if (a > c) bv |= 1 << i;
            bad += std::fabs(a - c) / 3 < 0.15f * (hi - lo);
            p += 6 * E;
        }
        if (!(__builtin_popcount(bv & 15) & 1)) continue;   // group 1 has odd parity
        bad = bad * 4 + err;
        if (bad < bestBad) { bestBad = bad; bestBits = bv; }
        if (bad == 0) break;
    }
    if (bestBad > 2 * 4 + 4) return false;
    bits = bestBits;
    return true;
}

std::string wssText(int bits)
{
    // group 1 as sent, b0 b1 b2 b3
    std::string g;
    for (int i = 0; i < 4; i++) g += ((bits >> i) & 1) ? '1' : '0';
    static const std::pair<const char*, const char*> ar[] = {
        {"0001", "4:3 full format"}, {"1000", "14:9 letterbox centre"}, {"0100", "14:9 letterbox top"},
        {"1101", "16:9 letterbox centre"}, {"0010", "16:9 letterbox top"}, {"1011", "wider than 16:9, letterbox centre"},
        {"0111", "14:9 full format"}, {"1110", "16:9 full format (anamorphic)"}};
    std::string s = "aspect ratio code " + g;
    for (auto& a : ar) if (g == a.first) s = a.second;
    if (bits & (1 << 4)) s += " \xC2\xB7 film mode";
    if (bits & (1 << 5)) s += " \xC2\xB7 Motion Adaptive Colour Plus";
    if (bits & (1 << 6)) s += " \xC2\xB7 PALplus helper";
    if (bits & (1 << 8)) s += " \xC2\xB7 subtitles in teletext";
    int sm = (bits >> 9) & 3;
    if (sm == 1) s += " \xC2\xB7 open subtitles inside the picture";
    if (sm == 2) s += " \xC2\xB7 open subtitles outside the picture";
    if (bits & (1 << 11)) s += " \xC2\xB7 surround sound";
    if (bits & (1 << 12)) s += " \xC2\xB7 copyright asserted";
    if (bits & (1 << 13)) s += " \xC2\xB7 copying restricted";
    return s;
}
