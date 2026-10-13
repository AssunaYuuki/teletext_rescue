// "VBI lines": the raw signal of the recording.
//   Left: every stored line of one frame as a raster (grey or false colour), labelled with what the
//   decoder found there.
//   Right, top: the selected line as
//     - a waveform (levels, microseconds; drag to zoom, right click to zoom out, the cursor reads
//       time / sample / level; the bit grid of the line's service is drawn when it is known),
//     - a spectrum (0..7 MHz, markers for the PAL / SECAM subcarriers and the data clocks), or
//     - an eye diagram over the next frames (the samples folded on two bits of the line's clock).
//   "Colour (as on TV)" decodes the rasters in colour (SECAM FM, PAL / NTSC from the burst), so test
//   lines (colour bars, ITS / VITS) and the SECAM identification show as a TV would.
//   Right, bottom: the same line over the following frames (carousels and repeats show up as
//   vertical patterns). Frame slider, Play, mouse wheel and arrow keys move through the recording.
#include "gui_app.h"
#include "slicer.h"
#include "linecolour.h"
#include <windowsx.h>
#include <cmath>
#include <complex>
#include <map>

namespace ui {
namespace {
void paint_buffered(HWND h, const std::function<void(HDC, RECT)> &draw)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ ob = SelectObject(mem, bm);
    fill(mem, rc, BG);
    draw(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps);
}

enum View { WAVE, SPECTRUM, EYE, FM };

struct LinesWin : Window {
    ResultPtr R;
    VbiFile vf;
    VbiGeometry g;
    Header hd;
    HWND slider = nullptr, info = nullptr, b_play = nullptr, b_view[4] = {}, b_colour = nullptr, b_secam = nullptr;
    RECT rast{}, scope{}, hist{}, plot{};
    int64_t frame = 0;
    int sel = 0;                       // selected line index
    static constexpr int kHist = 192;  // frames shown in "over time" and folded into the eye
    std::vector<uint8_t> cur, run;     // this frame; kHist frames from it
    int64_t runFrom = -1;
    View view = WAVE;
    bool falseColour = false, secamColour = false, playing = false;
    double z0 = 0, z1 = 1;             // waveform zoom (share of the line)
    int dragFrom = -1, dragTo = -1;    // waveform drag, pixels
    POINT mouse{-1, -1};

    float sample(const uint8_t *line, int i) const { return g.fmt == 1 ? (line[2 * i] | (line[2 * i + 1] << 8)) / 256.0f : line[i]; }
    const uint8_t *line_of(const std::vector<uint8_t> &buf, int f, int l) const { return &buf[(size_t)f * g.frameBytes() + (size_t)l * g.lineBytes]; }
    int nsamp() const { return std::max(1, g.validSamples); }
    double fs() const { return g.lineSamples * g.lineRate; }
    std::string line_label(int l) const
    {
        int fld = l < g.linesPerFrame / 2 ? 1 : 2;
        return fmt("%d f%d", g.firstLine + l % std::max(1, g.linesPerFrame / 2), fld);
    }
    const LineInfo *info_of(int l) const
    {
        for (auto &li : R->lines) if (li.index == l) return &li;
        return nullptr;
    }
    // bit clock of the service on a line (Hz), 0 if none is known
    double clock_of(int l) const
    {
        const LineInfo *li = info_of(l);
        if (!li) return 0;
        const std::string &k = li->kind;
        if (li->mode == MODE_TVINFORM) return kTviRateHz[li->tviRate == 0 ? 0 : 1];
        if (k.find("WST PAL") != std::string::npos || (g.pal && k.find("teletext") != std::string::npos)) return 6.9375e6;
        if (k.find("VPS") != std::string::npos) return 2.5e6;
        if (k.find("WSS") != std::string::npos) return 5e6 / 6 * 3;
        if (li->mode == MODE_FAST || k.find("NABTS") != std::string::npos || k.find("Didon") != std::string::npos) return g.pal ? 6.9375e6 : 364 * g.lineRate;
        if (li->mode == MODE_CC || k.find("Captions") != std::string::npos || k.find("XDS") != std::string::npos || k == "StarSight") return 32 * g.lineRate;
        if (li->mode == MODE_AMOL || li->mode == MODE_GEMSTAR) return 64 * g.lineRate;
        if (li->mode == MODE_SILENT_RADIO) return 80 * g.lineRate;
        return 0;
    }
    void load()
    {
        frame = std::max<int64_t>(0, std::min<int64_t>(frame, g.frames - 1));
        if (vf.read(frame, 1, cur) < 1) cur.assign(g.frameBytes(), 0);
        if (runFrom != frame) {
            run.clear();
            vf.read(frame, kHist, run);
            run.resize((size_t)kHist * g.frameBytes(), 0);
            runFrom = frame;
        }
        SendMessageW(slider, TBM_SETPOS, TRUE, (LPARAM)(frame));
        const LineInfo *li = info_of(sel);
        double fr = frame_rate(*R);
        std::string s = fmt("frame %lld of %lld  \xC2\xB7  %s  \xC2\xB7  line %s", (long long)frame + 1, (long long)g.frames,
                            rec_time(frame / fr).c_str(), line_label(sel).c_str());
        if (li) s += "  \xC2\xB7  " + li->kind + fmt(" (signal %.0f%%)", li->detectRate * 100);
        double ck = clock_of(sel);
        if (ck > 0) s += fmt("  \xC2\xB7  clock %.4f MHz = %.2f samples/bit", ck / 1e6, fs() / ck);
        s += fmt("  \xC2\xB7  %.2f MHz sampling", fs() / 1e6);
        if (!R->colour.empty()) s += "  \xC2\xB7  " + R->colour;
        if (secamColour) s += "  \xC2\xB7  colour decoded as " + tv_system();
        set_text(info, s);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    Image raster(const std::vector<uint8_t> &buf, int rows, std::function<const uint8_t *(int)> row) const
    {
        int n = nsamp();
        Image im(n, rows);
        for (int y = 0; y < rows; y++) {
            const uint8_t *p = row(y);
            for (int x = 0; x < n; x++) {
                int v = (int)std::min(255.0f, std::max(0.0f, sample(p, x)));
                im.px[(size_t)y * n + x] = falseColour ? heat(v) : (uint32_t)(v << 16 | v << 8 | v);
            }
        }
        return im;
    }
    // SECAM colour raster: rows = (frame, line) pairs from buf; neighbours come from the same frame
    Image secam_raster(const std::vector<uint8_t> &buf, int rows, std::function<std::pair<int, int>(int)> at) const
    {
        int n = nsamp();
        Image im(n, rows);
        std::vector<float> all;
        for (int r = 0; r < rows; r++) { auto fl = at(r); const uint8_t *p = line_of(buf, fl.first, fl.second); for (int i = 0; i < n; i += 7) all.push_back(sample(p, i)); }
        std::sort(all.begin(), all.end());
        float black = all.empty() ? 0 : all[all.size() * 5 / 100], white = all.empty() ? 255 : all[all.size() * 995 / 1000];
        float scale = std::max(20.0f, white - black);
        auto get = [&](int f, int l) {
            std::vector<float> x(n);
            const uint8_t *p = line_of(buf, f, l);
            for (int i = 0; i < n; i++) x[i] = sample(p, i);
            return secam_line(x, fs());
        };
        int per = g.linesPerFrame / 2;
        for (int r = 0; r < rows; r++) {
            auto fl = at(r);
            SecamLine a = get(fl.first, fl.second);
            int fld0 = fl.second < per ? 0 : per, k = fl.second - fld0;
            SecamLine b;
            const SecamLine *nb = nullptr;
            for (int dk : {-1, 1}) {
                int k2 = k + dk;
                if (k2 < 0 || k2 >= per) continue;
                b = get(fl.first, fld0 + k2);
                if (b.type && b.type != a.type) { nb = &b; break; }
            }
            secam_rgb(a, nb, black, scale, &im.px[(size_t)r * n]);
        }
        return im;
    }
    // colour system the raster is decoded with
    std::string tv_system() const { return R->colour == "SECAM" ? "SECAM" : !g.pal ? "NTSC" : "PAL"; }
    // PAL / NTSC colour raster: rows = (frame, line) pairs from buf. PAL: the V switch of each line comes
    // from the bursts of neighbouring lines of the same field (line to line the subcarrier advances
    // 283.75 cycles = 270.6 degrees, the switched burst adds +-90), voted over the whole field
    Image qam_raster(const std::vector<uint8_t> &buf, int rows, std::function<std::pair<int, int>(int)> at) const
    {
        int n = nsamp();
        bool pal = g.pal;
        double fsc = pal ? 4.43361875e6 : 3579545.4545;
        Image im(n, rows);
        std::vector<float> all;
        for (int r = 0; r < rows; r++) { auto fl = at(r); const uint8_t *p = line_of(buf, fl.first, fl.second); for (int i = 0; i < n; i += 7) all.push_back(sample(p, i)); }
        // black = the most common level (blanking), white = the top
        int hist[256] = {0};
        for (float v : all) hist[std::max(0, std::min(255, (int)v))]++;
        int mode = 0; for (int v = 1; v < 256; v++) if (hist[v] > hist[mode]) mode = v;
        std::sort(all.begin(), all.end());
        float black = (float)mode, white = all.empty() ? 255 : all[all.size() * 995 / 1000];
        float scale = std::max(20.0f, white - black);
        auto get = [&](int f, int l) {
            std::vector<float> x(n);
            const uint8_t *p = line_of(buf, f, l);
            for (int i = 0; i < n; i++) x[i] = sample(p, i);
            return qam_line(x, fs(), fsc);
        };
        int per = g.linesPerFrame / 2;
        std::map<std::pair<int, int>, int> parity;   // (frame, field) -> V switch of the field's even lines
        auto field_switch = [&](int f, int fld) {
            auto key = std::make_pair(f, fld);
            auto it = parity.find(key);
            if (it != parity.end()) return it->second;
            double vote = 0;
            std::vector<QamLine> ls;
            for (int k = 0; k < per; k++) ls.push_back(get(f, fld * per + k));
            double adv = std::fmod(2 * M_PI * fsc / g.lineRate, 2 * M_PI);
            for (int k = 0; k + 1 < per; k++) {
                if (ls[k].bamp <= 0 || ls[k + 1].bamp <= 0) continue;
                double r = std::remainder(ls[k + 1].burst - ls[k].burst - adv, 2 * M_PI);
                // r = +90: line k is V+ ; -90: line k is V-
                double sk = r > 0 ? 1 : -1, w = std::abs(std::sin(r));
                vote += (k % 2 == 0 ? sk : -sk) * w;
            }
            return parity[key] = vote >= 0 ? 1 : -1;
        };
        for (int r = 0; r < rows; r++) {
            auto fl = at(r);
            QamLine a = get(fl.first, fl.second);
            int fld = fl.second < per ? 0 : 1, k = fl.second - fld * per;
            int s = pal ? field_switch(fl.first, fld) * (k % 2 == 0 ? 1 : -1) : 1;
            qam_rgb(a, pal, s, black, scale, &im.px[(size_t)r * n]);
        }
        return im;
    }
    Image colour_raster(const std::vector<uint8_t> &buf, int rows, std::function<std::pair<int, int>(int)> at) const
    {
        return tv_system() == "SECAM" ? secam_raster(buf, rows, at) : qam_raster(buf, rows, at);
    }
    void pen_line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int style = PS_SOLID)
    {
        HPEN p = CreatePen(style, 1, c); HGDIOBJ o = SelectObject(dc, p);
        MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
        SelectObject(dc, o); DeleteObject(p);
    }
    // ---------------------------------------------------------------- waveform
    void paint_wave(HDC dc, HFONT small, HFONT smallb)
    {
        const uint8_t *p = line_of(cur, 0, sel);
        int n = nsamp();
        std::vector<float> v(n);
        for (int i = 0; i < n; i++) v[i] = sample(p, i);
        std::vector<float> srt(v);
        std::sort(srt.begin(), srt.end());
        float lo = srt[n * 8 / 100], hi = srt[n * 92 / 100], mid = (lo + hi) / 2;
        int sx0 = plot.left, sx1 = plot.right, sy0 = plot.top, sy1 = plot.bottom;
        double a = z0 * (n - 1), b = z1 * (n - 1);
        auto X = [&](double i) { return sx0 + (int)((i - a) * (sx1 - sx0) / std::max(1e-9, b - a)); };
        auto Y = [&](float s) { return sy1 - (int)(s * (sy1 - sy0) / 255.0f); };
        for (int q = 0; q <= 255; q += 64) {
            pen_line(dc, sx0, Y((float)q), sx1, Y((float)q), LINE, PS_DOT);
            text_out(dc, sx0 - S(hwnd, 4), Y((float)q) - S(hwnd, 7), fmt("%d", q), DIM, small, TA_RIGHT);
        }
        double usA = a / fs() * 1e6, usB = b / fs() * 1e6, span = usB - usA;
        double step = span > 40 ? 10 : span > 16 ? 5 : span > 6 ? 2 : span > 2 ? 0.5 : 0.1;
        for (double t = std::ceil(usA / step) * step; t <= usB; t += step) {
            int x = X(t * 1e-6 * fs());
            pen_line(dc, x, sy0, x, sy1, LINE, PS_DOT);
            text_out(dc, x, sy1 + S(hwnd, 3), step < 1 ? fmt("%.1f \xC2\xB5s", t) : fmt("%.0f \xC2\xB5s", t), DIM, small, TA_CENTER);
        }
        for (float q : {lo, mid, hi}) pen_line(dc, sx0, Y(q), sx1, Y(q), RGB(0x6a, 0x55, 0x30), PS_DOT);
        // bit grid of the line's clock, phased on the data edges, when zoomed in enough to see bits
        double ck = clock_of(sel);
        if (ck > 0) {
            double T = fs() / ck;
            if ((sx1 - sx0) / ((b - a) / T) >= 6) {
                std::complex<double> acc = 0;
                for (int i = 1; i < n; i++)
                    if ((v[i - 1] - mid) * (v[i] - mid) < 0) {
                        double e = i - 1 + (mid - v[i - 1]) / (v[i] - v[i - 1]);
                        acc += std::polar(1.0, 2 * M_PI * e / T);
                    }
                double ph = std::arg(acc) / (2 * M_PI) * T;
                for (double e = ph + std::floor((a - ph) / T) * T; e <= b; e += T) {
                    int x = X(e);
                    if (x >= sx0) pen_line(dc, x, sy0, x, sy1, RGB(0x2a, 0x45, 0x60));
                    // the bit read at the middle of the cell
                    double c = e + T / 2;
                    if (c >= 0 && c < n && c >= a && c <= b)
                        text_out(dc, X(c), sy0 + S(hwnd, 2), v[(int)c] > mid ? "1" : "0", v[(int)c] > mid ? ACCENT2 : DIM, small, TA_CENTER);
                }
            }
        }
        // the signal: min..max of the samples under each pixel column
        HPEN wpen = CreatePen(PS_SOLID, 1, ACCENT2);
        HGDIOBJ op = SelectObject(dc, wpen);
        int cols = sx1 - sx0;
        for (int c = 0; c < cols; c++) {
            double fa = a + (b - a) * c / cols, fb = a + (b - a) * (c + 1) / cols;
            int ia = std::max(0, (int)fa), ib = std::min(n - 1, std::max(ia, (int)fb));
            float mn = 255, mx = 0;
            for (int i = ia; i <= ib; i++) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
            if (b - a < cols) {   // zoomed in: interpolate between samples
                double t = fa - ia;
                float y = v[ia] + (float)t * (v[std::min(n - 1, ia + 1)] - v[ia]);
                mn = mx = y;
            }
            if (c == 0) MoveToEx(dc, sx0, Y(mx), nullptr);
            LineTo(dc, sx0 + c, Y(mx)); LineTo(dc, sx0 + c, Y(mn));
        }
        SelectObject(dc, op); DeleteObject(wpen);
        if (b - a < cols / 3)   // sample dots
            for (int i = (int)std::ceil(a); i <= (int)b && i < n; i++) {
                RECT d{X(i) - 1, Y(v[i]) - 1, X(i) + 2, Y(v[i]) + 2};
                fill(dc, d, ACCENT);
            }
        // drag selection and cursor readout
        if (dragFrom >= 0 && dragTo >= 0) {
            RECT r{std::min(dragFrom, dragTo), sy0, std::max(dragFrom, dragTo), sy1};
            HBRUSH br = CreateHatchBrush(HS_BDIAGONAL, RGB(0x60, 0x50, 0x20)); FrameRect(dc, &r, br); DeleteObject(br);
        }
        if (mouse.x >= sx0 && mouse.x < sx1 && mouse.y >= sy0 && mouse.y <= sy1) {
            double i = a + (b - a) * (mouse.x - sx0) / std::max(1, sx1 - sx0);
            int ii = std::max(0, std::min(n - 1, (int)std::lround(i)));
            pen_line(dc, mouse.x, sy0, mouse.x, sy1, MUTED, PS_DOT);
            text_out(dc, std::min((int)mouse.x + S(hwnd, 6), sx1 - S(hwnd, 170)), sy0 + S(hwnd, 16),
                     fmt("%.3f \xC2\xB5s  \xC2\xB7  sample %d  \xC2\xB7  level %.0f", ii / fs() * 1e6, ii, v[ii]), FG, smallb);
        }
        text_out(dc, scope.left + S(hwnd, 8), scope.top + S(hwnd, 6),
                 fmt("line %s \xE2\x80\x94 waveform  (8%% / middle / 92%%: %.0f / %.0f / %.0f)%s", line_label(sel).c_str(), lo, mid, hi,
                     z1 - z0 < 0.999 ? "  \xC2\xB7  right click: whole line" : "  \xC2\xB7  drag to zoom"),
                 FG, smallb);
    }
    // ---------------------------------------------------------------- spectrum
    void paint_spectrum(HDC dc, HFONT small, HFONT smallb)
    {
        int n = nsamp(), N = 1;
        while (N < n) N <<= 1;
        // average power over the next frames (Hann window, radix-2 FFT)
        std::vector<double> P(N / 2 + 1, 0);
        std::vector<std::complex<double>> x(N);
        int frames = std::min(kHist, 64);
        for (int f = 0; f < frames; f++) {
            const uint8_t *p = line_of(run, f, sel);
            double m = 0;
            for (int i = 0; i < n; i++) m += sample(p, i);
            m /= n;
            for (int i = 0; i < N; i++) x[i] = i < n ? (sample(p, i) - m) * (0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1))) : 0.0;
            for (int i = 1, j = 0; i < N; i++) {
                int bit = N >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap(x[i], x[j]);
            }
            for (int len = 2; len <= N; len <<= 1) {
                std::complex<double> w = std::polar(1.0, -2 * M_PI / len);
                for (int i = 0; i < N; i += len) {
                    std::complex<double> u = 1;
                    for (int k = 0; k < len / 2; k++, u *= w) {
                        auto t = x[i + k + len / 2] * u;
                        x[i + k + len / 2] = x[i + k] - t;
                        x[i + k] += t;
                    }
                }
            }
            for (int k = 0; k <= N / 2; k++) P[k] += std::norm(x[k]);
        }
        double fmax = std::min(7e6, fs() / 2);
        int kmax = (int)(fmax / fs() * N);
        double top = 1e-9;
        for (int k = 2; k <= kmax; k++) top = std::max(top, P[k]);
        int sx0 = plot.left, sx1 = plot.right, sy0 = plot.top, sy1 = plot.bottom;
        auto X = [&](double f) { return sx0 + (int)(f / fmax * (sx1 - sx0)); };
        auto Y = [&](double p) { double db = 10 * std::log10(std::max(p / top, 1e-6)); return sy0 + (int)(-db / 60 * (sy1 - sy0)); };
        for (int db = 0; db >= -60; db -= 20) {
            int y = sy0 + (int)(-db / 60.0 * (sy1 - sy0));
            pen_line(dc, sx0, y, sx1, y, LINE, PS_DOT);
            text_out(dc, sx0 - S(hwnd, 4), y - S(hwnd, 7), fmt("%d dB", db), DIM, small, TA_RIGHT);
        }
        for (double f = 0; f <= fmax; f += 1e6) {
            pen_line(dc, X(f), sy0, X(f), sy1, LINE, PS_DOT);
            text_out(dc, X(f), sy1 + S(hwnd, 3), fmt("%.0f MHz", f / 1e6), DIM, small, TA_CENTER);
        }
        struct Mk { double f; const char *name; COLORREF c; };
        std::vector<Mk> mk;
        if (g.pal) {
            mk.push_back({4.43361875e6, "PAL 4.434", RGB(0x3f, 0xa7, 0xff)});
            mk.push_back({4.25e6, "SECAM D'B 4.250", RGB(0xd0, 0x3c, 0x3c)});
            mk.push_back({4.40625e6, "D'R 4.406", RGB(0xd0, 0x3c, 0x3c)});
        } else mk.push_back({3.579545e6, "NTSC 3.58", RGB(0x3f, 0xa7, 0xff)});
        double ck = clock_of(sel);
        if (ck > 0) mk.push_back({ck / 2, "clock/2", ACCENT});
        int ty = sy0 + S(hwnd, 2);
        for (auto &m : mk) {
            if (m.f > fmax) continue;
            pen_line(dc, X(m.f), sy0, X(m.f), sy1, m.c, PS_DASH);
            text_out(dc, X(m.f) + S(hwnd, 3), ty, m.name, m.c, small);
            ty += S(hwnd, 13);
        }
        HPEN wpen = CreatePen(PS_SOLID, 1, ACCENT2);
        HGDIOBJ op = SelectObject(dc, wpen);
        for (int k = 1; k <= kmax; k++) {
            int x = X((double)k * fs() / N), y = Y(P[k]);
            if (k == 1) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
        }
        SelectObject(dc, op); DeleteObject(wpen);
        if (mouse.x >= sx0 && mouse.x < sx1 && mouse.y >= sy0 && mouse.y <= sy1) {
            double f = (double)(mouse.x - sx0) / (sx1 - sx0) * fmax;
            pen_line(dc, mouse.x, sy0, mouse.x, sy1, MUTED, PS_DOT);
            text_out(dc, std::min((int)mouse.x + S(hwnd, 6), sx1 - S(hwnd, 90)), sy1 - S(hwnd, 18), fmt("%.3f MHz", f / 1e6), FG, smallb);
        }
        text_out(dc, scope.left + S(hwnd, 8), scope.top + S(hwnd, 6),
                 fmt("line %s \xE2\x80\x94 spectrum over %d frames (PAL / SECAM subcarriers, the data clock)", line_label(sel).c_str(), frames), FG, smallb);
    }
    // ---------------------------------------------------------------- eye diagram
    void paint_eye(HDC dc, HFONT small, HFONT smallb)
    {
        double ck = clock_of(sel);
        if (ck <= 0) {
            text_out(dc, scope.left + S(hwnd, 8), scope.top + S(hwnd, 6), "line " + line_label(sel) + " \xE2\x80\x94 eye diagram: no data clock known for this line", MUTED, smallb);
            return;
        }
        double T = fs() / ck;
        int W = std::max(64, (int)(plot.right - plot.left)), H = std::max(32, (int)(plot.bottom - plot.top));
        std::vector<int> hits((size_t)W * H, 0);
        int n = nsamp();
        std::vector<float> v(n);
        for (int f = 0; f < kHist; f++) {
            const uint8_t *p = line_of(run, f, sel);
            for (int i = 0; i < n; i++) v[i] = sample(p, i);
            std::vector<float> s(v);
            std::sort(s.begin(), s.end());
            float lo = s[n * 8 / 100], hi = s[n * 92 / 100], mid = (lo + hi) / 2;
            if (hi - lo < 20) continue;
            std::complex<double> acc = 0;
            for (int i = 1; i < n; i++)
                if ((v[i - 1] - mid) * (v[i] - mid) < 0) {
                    double e = i - 1 + (mid - v[i - 1]) / (v[i] - v[i - 1]);
                    acc += std::polar(1.0, 2 * M_PI * e / T);
                }
            double ph = std::arg(acc) / (2 * M_PI) * T;   // an edge position
            // fold on two bits, one bit before and after the cell: edges at 0.5 and 1.5 of the width
            for (double t = 0; t < n - 1; t += 0.25) {
                double u = std::fmod(t - ph + T * 0.5 + 1000 * T, 2 * T) / (2 * T);
                int i = (int)t;
                float y = v[i] + (float)(t - i) * (v[i + 1] - v[i]);
                int px = (int)(u * W), py = H - 1 - (int)(y / 255.0f * (H - 1));
                if (px >= 0 && px < W && py >= 0 && py < H) hits[(size_t)py * W + px]++;
            }
        }
        int mx = 1;
        for (int h : hits) mx = std::max(mx, h);
        Image im(W, H);
        for (size_t i = 0; i < hits.size(); i++) im.px[i] = hits[i] ? heat(40 + (int)(215 * std::log(1 + hits[i]) / std::log(1 + mx))) : 0x101113;
        blit(dc, im, plot.left, plot.top, W, H);
        text_out(dc, scope.left + S(hwnd, 8), scope.top + S(hwnd, 6),
                 fmt("line %s \xE2\x80\x94 eye diagram over %d frames, two bits of %.4f MHz (%.2f samples/bit); an open eye = readable data",
                     line_label(sel).c_str(), kHist, ck / 1e6, T),
                 FG, smallb);
    }
    // ---------------------------------------------------------------- SECAM FM: subcarrier frequency along the line
    void paint_fm(HDC dc, HFONT small, HFONT smallb)
    {
        int n = nsamp();
        std::vector<float> x(n);
        const uint8_t *p = line_of(cur, 0, sel);
        for (int i = 0; i < n; i++) x[i] = sample(p, i);
        SecamLine L = secam_line(x, fs());
        int sx0 = plot.left, sx1 = plot.right, sy0 = plot.top, sy1 = plot.bottom;
        const double f0 = 3.7e6, f1 = 4.9e6;
        auto X = [&](double i) { return sx0 + (int)(i * (sx1 - sx0) / std::max(1, n - 1)); };
        auto Y = [&](double f) { return sy1 - (int)((f - f0) / (f1 - f0) * (sy1 - sy0)); };
        struct Mk { double f; const char *name; COLORREF c; };
        const Mk mk[] = {{3.9e6, "3.900 D'B bottle", RGB(0x3c, 0xcf, 0x7a)}, {4.25e6, "4.250 D'B rest", RGB(0x5a, 0x8a, 0xff)},
                         {4.40625e6, "4.406 D'R rest", RGB(0xd0, 0x3c, 0x3c)}, {4.43361875e6, "4.434 PAL", MUTED},
                         {4.756e6, "4.756 D'R bottle", RGB(0x3c, 0xcf, 0x7a)}};
        for (auto &m : mk) {
            pen_line(dc, sx0, Y(m.f), sx1, Y(m.f), m.c, PS_DOT);
            text_out(dc, sx0 - S(hwnd, 4), Y(m.f) - S(hwnd, 7), m.name, m.c, small, TA_RIGHT);
        }
        for (double t = 0; t <= n / fs() * 1e6; t += 10) {
            int xx = X(t * 1e-6 * fs());
            pen_line(dc, xx, sy0, xx, sy1, LINE, PS_DOT);
            text_out(dc, xx, sy1 + S(hwnd, 3), fmt("%.0f \xC2\xB5s", t), DIM, small, TA_CENTER);
        }
        if (!L.f.empty()) {
            // frequency where the subcarrier is present, coloured by what it means
            for (int i = 1; i < n; i++) {
                if (L.a[i] < 6 || L.a[i - 1] < 6) continue;
                double fa = std::max(f0, std::min(f1, (double)L.f[i - 1])), fb = std::max(f0, std::min(f1, (double)L.f[i]));
                COLORREF c = (fb < 4.0e6 || fb > 4.65e6) ? RGB(0x3c, 0xe0, 0x7a) : ACCENT2;
                pen_line(dc, X(i - 1), Y(fa), X(i), Y(fb), c);
            }
        }
        const char *what = L.type == 1 ? "D'B line (blue colour difference)" : L.type == 2 ? "D'R line (red colour difference)" : "no SECAM subcarrier";
        text_out(dc, scope.left + S(hwnd, 8), scope.top + S(hwnd, 6),
                 "line " + line_label(sel) + " \xE2\x80\x94 SECAM subcarrier frequency (FM demodulated): " + what +
                     "; the identification \xE2\x80\x9C" "bottles\xE2\x80\x9D rise to 4.756 / fall to 3.900 MHz and show green",
                 FG, smallb);
    }
    void paint(HDC dc, RECT)
    {
        HFONT small = font(hwnd, 8), smallb = font(hwnd, 9, true);
        if (cur.empty()) return;
        // raster of the frame, a label and a colour mark per line
        fill(dc, rast, PANEL);
        int lab = S(hwnd, 64), mark = S(hwnd, 6);
        RECT img{rast.left + lab + mark + S(hwnd, 4), rast.top + S(hwnd, 6), rast.right - S(hwnd, 6), rast.bottom - S(hwnd, 6)};
        int L = g.linesPerFrame;
        Image im = secamColour ? colour_raster(cur, L, [&](int y) { return std::make_pair(0, y); })
                               : raster(cur, L, [&](int y) { return line_of(cur, 0, y); });
        blit(dc, im, img.left, img.top, img.right - img.left, img.bottom - img.top, false);
        double rh = (double)(img.bottom - img.top) / L;
        for (int l = 0; l < L; l++) {
            int y0 = img.top + (int)(l * rh), y1 = img.top + (int)((l + 1) * rh);
            const LineInfo *li = info_of(l);
            RECT m{rast.left + lab, y0 + 1, rast.left + lab + mark, std::max(y0 + 2, y1 - 1)};
            fill(dc, m, li ? service_colour(li->kind) : service_colour("empty"));
            if (rh >= S(hwnd, 11)) text_out(dc, rast.left + S(hwnd, 6), y0 + (int)(rh - S(hwnd, 13)) / 2, line_label(l), l == sel ? ACCENT : MUTED, small);
            if (l == sel) {
                HPEN pen = CreatePen(PS_SOLID, std::max(1, S(hwnd, 2)), ACCENT); HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, img.left - 1, y0, img.right + 1, std::max(y0 + 3, y1));
                SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
            }
        }
        // the selected line
        fill(dc, scope, RGB(0x10, 0x11, 0x13));
        SetBkMode(dc, TRANSPARENT);
        if (view == WAVE) paint_wave(dc, small, smallb);
        else if (view == SPECTRUM) paint_spectrum(dc, small, smallb);
        else if (view == FM) paint_fm(dc, small, smallb);
        else paint_eye(dc, small, smallb);
        // the selected line over the next frames
        fill(dc, hist, PANEL);
        RECT hi2{hist.left + S(hwnd, 6), hist.top + S(hwnd, 22), hist.right - S(hwnd, 6), hist.bottom - S(hwnd, 6)};
        Image hm = secamColour ? colour_raster(run, kHist, [&](int y) { return std::make_pair(y, sel); })
                               : raster(run, kHist, [&](int y) { return line_of(run, y, sel); });
        blit(dc, hm, hi2.left, hi2.top, hi2.right - hi2.left, hi2.bottom - hi2.top, false);
        text_out(dc, hist.left + S(hwnd, 8), hist.top + S(hwnd, 4),
                 fmt("line %s over %d frames from this one (click a row to go to that frame)", line_label(sel).c_str(), kHist), FG, smallb);
    }
    void go(int64_t f) { frame = f; load(); }
    void set_view(View v)
    {
        view = v;
        layout_plot();
        for (int i = 0; i < 4; i++) set_button_style(b_view[i], i == (int)v ? BTN_ACCENT : BTN_NORMAL);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void layout_plot()
    {
        plot = {scope.left + S(hwnd, view == FM ? 110 : 44), scope.top + S(hwnd, 26), scope.right - S(hwnd, 10), scope.bottom - S(hwnd, 22)};
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            hd.create(hwnd, "VBI lines", file_name(*R) + " \xC2\xB7 " + formatName(R->geom.format));
            b_play = hd.add(hwnd, 506, "\xE2\x96\xB6 Play", 80);
            hd.add(hwnd, 501, "\xE2\x97\x80\xE2\x97\x80 1 s", 70);
            hd.add(hwnd, 502, "\xE2\x97\x80 frame", 76);
            hd.add(hwnd, 503, "frame \xE2\x96\xB6", 76);
            hd.add(hwnd, 504, "1 s \xE2\x96\xB6\xE2\x96\xB6", 70);
            hd.add(hwnd, 505, "Save PNG\xE2\x80\xA6", 96);
            std::string err;
            if (!vf.open(R->path, err, R->geom.format)) { ask(hwnd, "VBI lines", "Cannot open the recording:\n" + err, MB_ICONERROR); return -1; }
            g = vf.geom();
            slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10, hwnd, (HMENU)600, inst(), nullptr);
            SendMessageW(slider, TBM_SETRANGEMIN, FALSE, 0);
            SendMessageW(slider, TBM_SETRANGEMAX, TRUE, (LPARAM)std::max<int64_t>(0, g.frames - 1));
            SendMessageW(slider, TBM_SETPAGESIZE, 0, 25);
            info = label(hwnd, ""); set_label_colour(info, MUTED);
            b_view[0] = button(hwnd, 510, "Waveform");
            b_view[1] = button(hwnd, 511, "Spectrum");
            b_view[2] = button(hwnd, 512, "Eye diagram");
            b_view[3] = button(hwnd, 515, "SECAM FM");
            b_colour = button(hwnd, 513, "False colour", BTN_CHECK);
            b_secam = button(hwnd, 514, "Colour (as on TV)", BTN_CHECK);
            set_view(WAVE);
            // colour on at once for SECAM and when there are test lines (colour bars, ITS / VITS)
            bool tests = false;
            for (auto &li : R->lines) tests |= li.mode == MODE_TEST;
            if (R->colour == "SECAM" || tests) { secamColour = true; set_checked(b_secam, true); }
            // start on the first line the decoder found something on
            for (auto &li : R->lines) if (li.mode != MODE_TEST) { sel = li.index; break; }
            load();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            MoveWindow(info, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 24);
            int bottom = rc.bottom - p - S(hwnd, 34);
            MoveWindow(slider, p, bottom + S(hwnd, 6), rc.right - 2 * p, S(hwnd, 28), TRUE);
            int lw = (rc.right - 2 * p) * 40 / 100;
            rast = {p, y, p + lw, bottom};
            int rx = rast.right + S(hwnd, 10);
            int bx = rx;
            for (int i = 0; i < 4; i++) { MoveWindow(b_view[i], bx, y, S(hwnd, 104), S(hwnd, 28), TRUE); bx += S(hwnd, 110); }
            MoveWindow(b_colour, bx + S(hwnd, 10), y, S(hwnd, 130), S(hwnd, 28), TRUE);
            MoveWindow(b_secam, bx + S(hwnd, 150), y, S(hwnd, 140), S(hwnd, 28), TRUE);
            y += S(hwnd, 34);
            int sh = (bottom - y) * 55 / 100;
            scope = {rx, y, rc.right - p, y + sh};
            hist = {rx, scope.bottom + S(hwnd, 10), rc.right - p, bottom};
            layout_plot();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (w == 1) { if (frame + 1 >= g.frames) { playing = false; KillTimer(hwnd, 1); set_text(b_play, "\xE2\x96\xB6 Play"); } else go(frame + 1); }
            return 0;
        case WM_HSCROLL:
            if ((HWND)l == slider) { int64_t f = SendMessageW(slider, TBM_GETPOS, 0, 0); if (f != frame) go(f); if (LOWORD(w) == TB_ENDTRACK) SetFocus(hwnd); }
            return 0;
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
            go(frame + d * ((GET_KEYSTATE_WPARAM(w) & MK_SHIFT) ? 25 : 1));
            return 0;
        }
        case WM_MOUSEMOVE: {
            mouse = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (dragFrom >= 0) dragTo = std::max((int)plot.left, std::min((int)plot.right, (int)mouse.x));
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t);
            InvalidateRect(hwnd, &scope, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE: mouse = {-1, -1}; InvalidateRect(hwnd, &scope, FALSE); return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            SetFocus(hwnd);
            int top = rast.top + S(hwnd, 6), h = rast.bottom - S(hwnd, 6) - top;
            if (PtInRect(&rast, pt) && h > 0) { sel = std::max(0, std::min(g.linesPerFrame - 1, (int)((pt.y - top) * (int64_t)g.linesPerFrame / h))); z0 = 0; z1 = 1; load(); }
            int ht = hist.top + S(hwnd, 22), hh = hist.bottom - S(hwnd, 6) - ht;
            if (PtInRect(&hist, pt) && hh > 0 && pt.y >= ht) go(frame + (pt.y - ht) * (int64_t)kHist / hh);
            if (view == WAVE && PtInRect(&plot, pt)) { dragFrom = dragTo = pt.x; SetCapture(hwnd); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragFrom >= 0) {
                ReleaseCapture();
                int a = std::min(dragFrom, dragTo), b = std::max(dragFrom, dragTo);
                if (b - a > S(hwnd, 6)) {
                    double w0 = plot.right - plot.left, nz0 = z0 + (z1 - z0) * (a - plot.left) / w0, nz1 = z0 + (z1 - z0) * (b - plot.left) / w0;
                    if (nz1 - nz0 > 8.0 / nsamp()) { z0 = nz0; z1 = nz1; }
                }
                dragFrom = dragTo = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONDOWN: z0 = 0; z1 = 1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_KEYDOWN:
            switch (w) {
            case VK_LEFT: go(frame - 1); break;
            case VK_RIGHT: go(frame + 1); break;
            case VK_PRIOR: go(frame - 25); break;
            case VK_NEXT: go(frame + 25); break;
            case VK_HOME: go(0); break;
            case VK_END: go(g.frames - 1); break;
            case VK_UP: sel = (sel + g.linesPerFrame - 1) % g.linesPerFrame; load(); break;
            case VK_DOWN: sel = (sel + 1) % g.linesPerFrame; load(); break;
            case VK_SPACE: SendMessageW(hwnd, WM_COMMAND, 506, 0); break;
            }
            return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_COMMAND: {
            int id = LOWORD(w);
            int64_t sec = (int64_t)std::lround(frame_rate(*R));
            if (id == 501) go(frame - sec);
            else if (id == 502) go(frame - 1);
            else if (id == 503) go(frame + 1);
            else if (id == 504) go(frame + sec);
            else if (id == 506) {
                playing = !playing;
                if (playing) SetTimer(hwnd, 1, (UINT)(1000 / std::max(1.0, frame_rate(*R))), nullptr); else KillTimer(hwnd, 1);
                set_text(b_play, playing ? "\xE2\x8F\xB8 Pause" : "\xE2\x96\xB6 Play");
            } else if (id >= 510 && id <= 512) set_view((View)(id - 510));
            else if (id == 515) set_view(FM);
            else if (id == 513) { falseColour = !falseColour; set_checked(b_colour, falseColour); InvalidateRect(hwnd, nullptr, FALSE); }
            else if (id == 514) { secamColour = !secamColour; set_checked(b_secam, secamColour); load(); }
            else if (id == 505) {
                std::string out = save_file(hwnd, "Save the frame raster", stem_path(file_name(*R)) + fmt("_frame%lld.png", (long long)frame), "png");
                if (!out.empty()) {
                    Image im = secamColour ? colour_raster(cur, g.linesPerFrame, [&](int y) { return std::make_pair(0, y); })
                                           : raster(cur, g.linesPerFrame, [&](int y) { return line_of(cur, 0, y); });
                    write_file(out, png_encode(im));
                }
            }
            SetFocus(hwnd);
            return 0;
        }
        case WM_DESTROY: KillTimer(hwnd, 1); break;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
} // namespace

void lines_window(HWND owner, const ResultPtr &r)
{
    auto *w = new LinesWin; w->R = r;
    w->create(L"TRLines", "VBI lines \xE2\x80\x94 " + file_name(*r), WS_OVERLAPPEDWINDOW, S(owner, 1280), S(owner, 800), owner);
    if (w->hwnd) ShowWindow(w->hwnd, SW_SHOW);
}
}
