// "VBI lines" (as src/gui_lines.cpp): the raw signal of the recording. Left: every stored line of one
// frame as a raster (grey, false colour or the colour as on a TV); right: the selected line as a
// waveform (drag to zoom, right click for the whole line), spectrum, eye diagram or SECAM FM, and the
// same line over the following frames. Frame slider, Play, mouse wheel and arrow keys.
#include "draw.h"
#include "linecolour.h"
#include "png.h"
#include "slicer.h"
#include "vbifile.h"
#include <cmath>
#include <complex>
#include <map>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace xg {
namespace {
enum View { WAVE, SPECTRUM, EYE, FM };

struct LinesWin : Win {
    ResultPtr R;
    VbiFile vf;
    VbiGeometry g;
    bool ok = false;
    int64_t frame = 0;
    int sel = 0;
    static constexpr int kHist = 192;
    std::vector<uint8_t> cur, run;
    int64_t runFrom = -1;
    View view = WAVE;
    bool falseColour = false, colour = false, playing = false, dirty = true;
    double z0 = 0, z1 = 1, nextFrameAt = 0;
    float dragFrom = -1, dragTo = -1;
    Tex rastTex, histTex, eyeTex;
    std::string infoText;

    float sample(const uint8_t *line, int i) const { return g.fmt == 1 ? (line[2 * i] | (line[2 * i + 1] << 8)) / 256.0f : line[i]; }
    const uint8_t *line_of(const std::vector<uint8_t> &buf, int f, int l) const { return &buf[(size_t)f * g.frameBytes() + (size_t)l * g.lineBytes]; }
    int nsamp() const { return std::max(1, g.validSamples); }
    double fs() const { return g.lineSamples * g.lineRate; }
    std::string line_label(int l) const { int fld = l < g.linesPerFrame / 2 ? 1 : 2; return fmt("%d f%d", g.firstLine + l % std::max(1, g.linesPerFrame / 2), fld); }
    const LineInfo *info_of(int l) const { for (auto &li : R->lines) if (li.index == l) return &li; return nullptr; }
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
    std::string tv_system() const { return R->colour == "SECAM" ? "SECAM" : !g.pal ? "NTSC" : "PAL"; }
    void init()
    {
        std::string err;
        ok = vf.open(R->path, err, R->geom.format);
        if (!ok) { infoText = "Cannot open the recording: " + err; return; }
        g = vf.geom();
        bool tests = false;
        for (auto &li : R->lines) tests |= li.mode == MODE_TEST;
        colour = R->colour == "SECAM" || tests;
        for (auto &li : R->lines) if (li.mode != MODE_TEST) { sel = li.index; break; }
        load();
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
        const LineInfo *li = info_of(sel);
        double fr = ui::frame_rate(*R);
        infoText = fmt("frame %lld of %lld  \xC2\xB7  %s  \xC2\xB7  line %s", (long long)frame + 1, (long long)g.frames, ui::rec_time(frame / fr).c_str(), line_label(sel).c_str());
        if (li) infoText += "  \xC2\xB7  " + li->kind + fmt(" (signal %.0f%%)", li->detectRate * 100);
        double ck = clock_of(sel);
        if (ck > 0) infoText += fmt("  \xC2\xB7  clock %.4f MHz = %.2f samples/bit", ck / 1e6, fs() / ck);
        infoText += fmt("  \xC2\xB7  %.2f MHz sampling", fs() / 1e6);
        if (!R->colour.empty()) infoText += "  \xC2\xB7  " + R->colour;
        if (colour) infoText += "  \xC2\xB7  colour decoded as " + tv_system();
        dirty = true;
    }
    Image raster(int rows, std::function<const uint8_t *(int)> row) const
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
    Image qam_raster(const std::vector<uint8_t> &buf, int rows, std::function<std::pair<int, int>(int)> at) const
    {
        int n = nsamp();
        bool pal = g.pal;
        double fsc = pal ? 4.43361875e6 : 3579545.4545;
        Image im(n, rows);
        std::vector<float> all;
        for (int r = 0; r < rows; r++) { auto fl = at(r); const uint8_t *p = line_of(buf, fl.first, fl.second); for (int i = 0; i < n; i += 7) all.push_back(sample(p, i)); }
        int hist[256] = {0};
        for (float v : all) hist[std::max(0, std::min(255, (int)v))]++;
        int mode = 0;
        for (int v = 1; v < 256; v++) if (hist[v] > hist[mode]) mode = v;
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
        std::map<std::pair<int, int>, int> parity;
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
    Image frame_image() const
    {
        int L = g.linesPerFrame;
        return colour ? colour_raster(cur, L, [](int y) { return std::make_pair(0, y); }) : raster(L, [&](int y) { return line_of(cur, 0, y); });
    }
    void rebuild()
    {
        if (!dirty) return;
        dirty = false;
        Image im = frame_image();
        rastTex.set(im.px.data(), im.w, im.h);
        Image hm = colour ? colour_raster(run, kHist, [this](int y) { return std::make_pair(y, sel); }) : raster(kHist, [&](int y) { return line_of(run, y, sel); });
        histTex.set(hm.px.data(), hm.w, hm.h);
        eyeTex.clear();
    }
    void go(int64_t f) { frame = f; load(); }
    void dline(float x0, float y0, float x1, float y1, ImU32 c, float th = 1) { DL()->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), c, th); }
    void dotted(float x0, float y0, float x1, float y1, ImU32 c)
    {
        float len = std::hypot(x1 - x0, y1 - y0);
        int n = (int)(len / 4);
        for (int i = 0; i < n; i += 2) dline(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, x0 + (x1 - x0) * (i + 1) / n, y0 + (y1 - y0) * (i + 1) / n, c);
    }
    // ---------------------------------------------------------------- waveform
    void wave(Rc scope, Rc plot)
    {
        const uint8_t *p = line_of(cur, 0, sel);
        int n = nsamp();
        std::vector<float> v(n);
        for (int i = 0; i < n; i++) v[i] = sample(p, i);
        std::vector<float> srt(v);
        std::sort(srt.begin(), srt.end());
        float lo = srt[n * 8 / 100], hi = srt[n * 92 / 100], mid = (lo + hi) / 2;
        double a = z0 * (n - 1), b = z1 * (n - 1);
        auto X = [&](double i) { return plot.l + (float)((i - a) * plot.w() / std::max(1e-9, b - a)); };
        auto Y = [&](float s) { return plot.b - s * plot.h() / 255.0f; };
        for (int q = 0; q <= 255; q += 64) { dotted(plot.l, Y((float)q), plot.r, Y((float)q), LINE); text(plot.l - px(4), Y((float)q) - px(7), fmt("%d", q), DIM, NORM, 8, RIGHT); }
        double usA = a / fs() * 1e6, usB = b / fs() * 1e6, span = usB - usA;
        double step = span > 40 ? 10 : span > 16 ? 5 : span > 6 ? 2 : span > 2 ? 0.5 : 0.1;
        for (double t = std::ceil(usA / step) * step; t <= usB; t += step) {
            float x = X(t * 1e-6 * fs());
            dotted(x, plot.t, x, plot.b, LINE);
            text(x, plot.b + px(3), step < 1 ? fmt("%.1f \xC2\xB5s", t) : fmt("%.0f \xC2\xB5s", t), DIM, NORM, 8, CENTER);
        }
        for (float q : {lo, mid, hi}) dotted(plot.l, Y(q), plot.r, Y(q), rgb(0x6a, 0x55, 0x30));
        double ck = clock_of(sel);
        if (ck > 0) {
            double T = fs() / ck;
            if (plot.w() / ((b - a) / T) >= 6) {
                std::complex<double> acc = 0;
                for (int i = 1; i < n; i++)
                    if ((v[i - 1] - mid) * (v[i] - mid) < 0) { double e = i - 1 + (mid - v[i - 1]) / (v[i] - v[i - 1]); acc += std::polar(1.0, 2 * M_PI * e / T); }
                double ph = std::arg(acc) / (2 * M_PI) * T;
                for (double e = ph + std::floor((a - ph) / T) * T; e <= b; e += T) {
                    float x = X(e);
                    if (x >= plot.l) dline(x, plot.t, x, plot.b, rgb(0x2a, 0x45, 0x60));
                    double c = e + T / 2;
                    if (c >= 0 && c < n && c >= a && c <= b) text(X(c), plot.t + px(2), v[(int)c] > mid ? "1" : "0", v[(int)c] > mid ? ACCENT2 : DIM, NORM, 8, CENTER);
                }
            }
        }
        int cols = std::max(1, (int)plot.w());
        float lastY = 0;
        for (int c = 0; c < cols; c++) {
            double fa = a + (b - a) * c / cols, fb = a + (b - a) * (c + 1) / cols;
            int ia = std::max(0, (int)fa), ib = std::min(n - 1, std::max(ia, (int)fb));
            float mn = 255, mx = 0;
            for (int i = ia; i <= ib; i++) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
            if (b - a < cols) { double t = fa - ia; float y = v[ia] + (float)t * (v[std::min(n - 1, ia + 1)] - v[ia]); mn = mx = y; }
            float x = plot.l + c;
            if (c) dline(x - 1, lastY, x, Y(mx), ACCENT2);
            dline(x, Y(mx), x, Y(mn) + 1, ACCENT2);
            lastY = Y(mn);
        }
        if (b - a < cols / 3.0)
            for (int i = (int)std::ceil(a); i <= (int)b && i < n; i++) fillr({X(i) - 1, Y(v[i]) - 1, X(i) + 2, Y(v[i]) + 2}, ACCENT);
        ImVec2 m = ImGui::GetMousePos();
        bool hov = ImGui::IsWindowHovered();
        if (hov && plot.in(m)) {
            if (ImGui::IsMouseClicked(0)) dragFrom = dragTo = m.x;
            if (ImGui::IsMouseClicked(1)) { z0 = 0; z1 = 1; }
            double i = a + (b - a) * (m.x - plot.l) / std::max(1.f, plot.w());
            int ii = std::max(0, std::min(n - 1, (int)std::lround(i)));
            dotted(m.x, plot.t, m.x, plot.b, MUTED);
            text(std::min(m.x + px(6), plot.r - px(220)), plot.t + px(16), fmt("%.3f \xC2\xB5s  \xC2\xB7  sample %d  \xC2\xB7  level %.0f", ii / fs() * 1e6, ii, v[ii]), FG, BOLD, 9);
        }
        if (dragFrom >= 0) {
            dragTo = std::max(plot.l, std::min(plot.r, m.x));
            DL()->AddRect(ImVec2(std::min(dragFrom, dragTo), plot.t), ImVec2(std::max(dragFrom, dragTo), plot.b), rgb(0x90, 0x70, 0x20));
            if (ImGui::IsMouseReleased(0)) {
                float x0 = std::min(dragFrom, dragTo), x1 = std::max(dragFrom, dragTo);
                if (x1 - x0 > px(6)) {
                    double nz0 = z0 + (z1 - z0) * (x0 - plot.l) / plot.w(), nz1 = z0 + (z1 - z0) * (x1 - plot.l) / plot.w();
                    if (nz1 - nz0 > 8.0 / nsamp()) { z0 = nz0; z1 = nz1; }
                }
                dragFrom = dragTo = -1;
            }
        }
        text(scope.l + px(8), scope.t + px(6), fmt("line %s \xE2\x80\x94 waveform  (8%% / middle / 92%%: %.0f / %.0f / %.0f)%s", line_label(sel).c_str(), lo, mid, hi,
                                                 z1 - z0 < 0.999 ? "  \xC2\xB7  right click: whole line" : "  \xC2\xB7  drag to zoom"), FG, BOLD, 9);
    }
    // ---------------------------------------------------------------- spectrum
    void spectrum(Rc scope, Rc plot)
    {
        int n = nsamp(), N = 1;
        while (N < n) N <<= 1;
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
                    for (int k = 0; k < len / 2; k++, u *= w) { auto t = x[i + k + len / 2] * u; x[i + k + len / 2] = x[i + k] - t; x[i + k] += t; }
                }
            }
            for (int k = 0; k <= N / 2; k++) P[k] += std::norm(x[k]);
        }
        double fmax = std::min(7e6, fs() / 2);
        int kmax = (int)(fmax / fs() * N);
        double top = 1e-9;
        for (int k = 2; k <= kmax; k++) top = std::max(top, P[k]);
        auto X = [&](double f) { return plot.l + (float)(f / fmax * plot.w()); };
        auto Y = [&](double p) { double db = 10 * std::log10(std::max(p / top, 1e-6)); return plot.t + (float)(-db / 60 * plot.h()); };
        for (int db = 0; db >= -60; db -= 20) { float y = plot.t + (float)(-db / 60.0 * plot.h()); dotted(plot.l, y, plot.r, y, LINE); text(plot.l - px(4), y - px(7), fmt("%d dB", db), DIM, NORM, 8, RIGHT); }
        for (double f = 0; f <= fmax; f += 1e6) { dotted(X(f), plot.t, X(f), plot.b, LINE); text(X(f), plot.b + px(3), fmt("%.0f MHz", f / 1e6), DIM, NORM, 8, CENTER); }
        struct Mk { double f; const char *name; ImU32 c; };
        std::vector<Mk> mk;
        if (g.pal) {
            mk.push_back({4.43361875e6, "PAL 4.434", rgb(0x3f, 0xa7, 0xff)});
            mk.push_back({4.25e6, "SECAM D'B 4.250", rgb(0xd0, 0x3c, 0x3c)});
            mk.push_back({4.40625e6, "D'R 4.406", rgb(0xd0, 0x3c, 0x3c)});
        } else
            mk.push_back({3.579545e6, "NTSC 3.58", rgb(0x3f, 0xa7, 0xff)});
        double ck = clock_of(sel);
        if (ck > 0) mk.push_back({ck / 2, "clock/2", ACCENT});
        float ty = plot.t + px(2);
        for (auto &m : mk) {
            if (m.f > fmax) continue;
            dotted(X(m.f), plot.t, X(m.f), plot.b, m.c);
            text(X(m.f) + px(3), ty, m.name, m.c, NORM, 8);
            ty += px(13);
        }
        for (int k = 2; k <= kmax; k++) dline(X((double)(k - 1) * fs() / N), Y(P[k - 1]), X((double)k * fs() / N), Y(P[k]), ACCENT2);
        ImVec2 m = ImGui::GetMousePos();
        if (ImGui::IsWindowHovered() && plot.in(m)) {
            dotted(m.x, plot.t, m.x, plot.b, MUTED);
            text(std::min(m.x + px(6), plot.r - px(90)), plot.b - px(18), fmt("%.3f MHz", (m.x - plot.l) / plot.w() * fmax / 1e6), FG, BOLD, 9);
        }
        text(scope.l + px(8), scope.t + px(6), fmt("line %s \xE2\x80\x94 spectrum over %d frames (PAL / SECAM subcarriers, the data clock)", line_label(sel).c_str(), frames), FG, BOLD, 9);
    }
    // ---------------------------------------------------------------- eye diagram
    void eye(Rc scope, Rc plot)
    {
        double ck = clock_of(sel);
        if (ck <= 0) { text(scope.l + px(8), scope.t + px(6), "line " + line_label(sel) + " \xE2\x80\x94 eye diagram: no data clock known for this line", MUTED, BOLD, 9); return; }
        double T = fs() / ck;
        int W = std::max(64, (int)plot.w()), H = std::max(32, (int)plot.h());
        if (!eyeTex.t || eyeTex.w != W || eyeTex.h != H) {
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
                    if ((v[i - 1] - mid) * (v[i] - mid) < 0) { double e = i - 1 + (mid - v[i - 1]) / (v[i] - v[i - 1]); acc += std::polar(1.0, 2 * M_PI * e / T); }
                double ph = std::arg(acc) / (2 * M_PI) * T;
                for (double t = 0; t < n - 1; t += 0.25) {
                    double u = std::fmod(t - ph + T * 0.5 + 1000 * T, 2 * T) / (2 * T);
                    int i = (int)t;
                    float y = v[i] + (float)(t - i) * (v[i + 1] - v[i]);
                    int xx = (int)(u * W), yy = H - 1 - (int)(y / 255.0f * (H - 1));
                    if (xx >= 0 && xx < W && yy >= 0 && yy < H) hits[(size_t)yy * W + xx]++;
                }
            }
            int mx = 1;
            for (int h : hits) mx = std::max(mx, h);
            std::vector<uint32_t> im(hits.size());
            for (size_t i = 0; i < hits.size(); i++) im[i] = hits[i] ? heat(40 + (int)(215 * std::log(1 + hits[i]) / std::log(1 + mx))) : 0x101113;
            eyeTex.set(im.data(), W, H);
        }
        DL()->AddImage(eyeTex.id(), ImVec2(plot.l, plot.t), ImVec2(plot.r, plot.b));
        text(scope.l + px(8), scope.t + px(6), fmt("line %s \xE2\x80\x94 eye diagram over %d frames, two bits of %.4f MHz (%.2f samples/bit); an open eye = readable data", line_label(sel).c_str(), kHist, ck / 1e6, T), FG, BOLD, 9);
    }
    // ---------------------------------------------------------------- SECAM FM
    void fm(Rc scope, Rc plot)
    {
        int n = nsamp();
        std::vector<float> x(n);
        const uint8_t *p = line_of(cur, 0, sel);
        for (int i = 0; i < n; i++) x[i] = sample(p, i);
        SecamLine L = secam_line(x, fs());
        const double f0 = 3.7e6, f1 = 4.9e6;
        auto X = [&](double i) { return plot.l + (float)(i * plot.w() / std::max(1, n - 1)); };
        auto Y = [&](double f) { return plot.b - (float)((f - f0) / (f1 - f0) * plot.h()); };
        struct Mk { double f; const char *name; ImU32 c; };
        const Mk mk[] = {{3.9e6, "3.900 D'B bottle", rgb(0x3c, 0xcf, 0x7a)}, {4.25e6, "4.250 D'B rest", rgb(0x5a, 0x8a, 0xff)}, {4.40625e6, "4.406 D'R rest", rgb(0xd0, 0x3c, 0x3c)},
                         {4.43361875e6, "4.434 PAL", MUTED}, {4.756e6, "4.756 D'R bottle", rgb(0x3c, 0xcf, 0x7a)}};
        for (auto &m : mk) { dotted(plot.l, Y(m.f), plot.r, Y(m.f), m.c); text(plot.l - px(4), Y(m.f) - px(7), m.name, m.c, NORM, 8, RIGHT); }
        for (double t = 0; t <= n / fs() * 1e6; t += 10) { float xx = X(t * 1e-6 * fs()); dotted(xx, plot.t, xx, plot.b, LINE); text(xx, plot.b + px(3), fmt("%.0f \xC2\xB5s", t), DIM, NORM, 8, CENTER); }
        if (!L.f.empty())
            for (int i = 1; i < n; i++) {
                if (L.a[i] < 6 || L.a[i - 1] < 6) continue;
                double fa = std::max(f0, std::min(f1, (double)L.f[i - 1])), fb = std::max(f0, std::min(f1, (double)L.f[i]));
                dline(X(i - 1), Y(fa), X(i), Y(fb), (fb < 4.0e6 || fb > 4.65e6) ? rgb(0x3c, 0xe0, 0x7a) : ACCENT2);
            }
        const char *what = L.type == 1 ? "D'B line (blue colour difference)" : L.type == 2 ? "D'R line (red colour difference)" : "no SECAM subcarrier";
        text(scope.l + px(8), scope.t + px(6), "line " + line_label(sel) + " \xE2\x80\x94 SECAM subcarrier frequency (FM demodulated): " + what, FG, BOLD, 9);
    }
    void draw() override
    {
        if (!ok) { wrapped(ACCENT, infoText); return; }
        double fr = ui::frame_rate(*R);
        if (playing && SDL_GetTicks() / 1000.0 >= nextFrameAt) {
            if (frame + 1 >= g.frames) playing = false;
            else { go(frame + 1); nextFrameAt = SDL_GetTicks() / 1000.0 + 1.0 / std::max(1.0, fr); }
        }
        header("VBI lines", ui::file_name(*R) + " \xC2\xB7 " + formatName(R->geom.format));
        int64_t sec = (int64_t)std::lround(fr);
        int b = header_buttons({playing ? "Pause" : "Play", "\xE2\x97\x80\xE2\x97\x80 1 s", "\xE2\x97\x80 frame", "frame \xE2\x96\xB6", "1 s \xE2\x96\xB6\xE2\x96\xB6", "Save PNG\xE2\x80\xA6"}, {80, 70, 76, 76, 70, 96});
        if (b == 0) { playing = !playing; nextFrameAt = 0; }
        if (b == 1) go(frame - sec);
        if (b == 2) go(frame - 1);
        if (b == 3) go(frame + 1);
        if (b == 4) go(frame + sec);
        if (b == 5) {
            Image im = frame_image();
            std::string name = stem_path(ui::file_name(*R)) + fmt("_frame%lld.png", (long long)frame);
            open_folder_dialog("Folder for the PNG", dirname(R->path), [im, name](std::string d) { write_file(path_join(d, name), png_encode(im)); });
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
            auto k = [](ImGuiKey x) { return ImGui::IsKeyPressed(x, true); };
            if (k(ImGuiKey_LeftArrow)) go(frame - 1);
            if (k(ImGuiKey_RightArrow)) go(frame + 1);
            if (k(ImGuiKey_PageUp)) go(frame - 25);
            if (k(ImGuiKey_PageDown)) go(frame + 25);
            if (k(ImGuiKey_Home)) go(0);
            if (k(ImGuiKey_End)) go(g.frames - 1);
            if (k(ImGuiKey_UpArrow)) { sel = (sel + g.linesPerFrame - 1) % g.linesPerFrame; load(); }
            if (k(ImGuiKey_DownArrow)) { sel = (sel + 1) % g.linesPerFrame; load(); }
            if (k(ImGuiKey_Space)) { playing = !playing; nextFrameAt = 0; }
        }
        wrapped(MUTED, infoText);
        rebuild();
        ImVec2 av = ImGui::GetContentRegionAvail();
        float lw = av.x * 0.40f, sliderH = px(34);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        Rc rast{p0.x, p0.y, p0.x + lw, p0.y + av.y - sliderH};
        float rx = rast.r + px(10);
        // view buttons
        ImGui::SetCursorScreenPos(ImVec2(rx, p0.y));
        static const char *vn[4] = {"Waveform", "Spectrum", "Eye diagram", "SECAM FM"};
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (view == i ? accent_button(vn[i], ImVec2(px(104), 0)) : ImGui::Button(vn[i], ImVec2(px(104), 0))) { view = (View)i; eyeTex.clear(); }
        }
        ImGui::SameLine(0, px(16));
        if (ImGui::Checkbox("False colour", &falseColour)) dirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Colour (as on TV)", &colour)) load();
        float y = p0.y + px(34);
        float sh = (rast.b - y) * 0.55f;
        Rc scope{rx, y, p0.x + av.x, y + sh}, hist{rx, scope.b + px(10), p0.x + av.x, rast.b};
        Rc plot{scope.l + px(view == FM ? 110 : 44), scope.t + px(26), scope.r - px(10), scope.b - px(22)};
        // raster with labels and service marks
        fillr(rast, PANEL);
        float lab = px(64), mark = px(6);
        Rc img{rast.l + lab + mark + px(4), rast.t + px(6), rast.r - px(6), rast.b - px(6)};
        DL()->AddImage(rastTex.id(), ImVec2(img.l, img.t), ImVec2(img.r, img.b));
        int L = g.linesPerFrame;
        float rh = img.h() / L;
        for (int l = 0; l < L; l++) {
            float y0 = img.t + l * rh, y1 = img.t + (l + 1) * rh;
            const LineInfo *li = info_of(l);
            fillr({rast.l + lab, y0 + 1, rast.l + lab + mark, std::max(y0 + 2, y1 - 1)}, li ? service_colour(li->kind) : service_colour("empty"));
            if (rh >= px(11)) text(rast.l + px(6), y0 + (rh - px(13)) / 2, line_label(l), l == sel ? ACCENT : MUTED, NORM, 8);
            if (l == sel) DL()->AddRect(ImVec2(img.l - 1, y0), ImVec2(img.r + 1, std::max(y0 + 3, y1)), ACCENT, 0, 0, px(2));
        }
        ImVec2 m = ImGui::GetMousePos();
        bool hov = ImGui::IsWindowHovered();
        if (hov && ImGui::IsMouseClicked(0) && rast.in(m) && img.h() > 0) { sel = std::max(0, std::min(L - 1, (int)((m.y - img.t) * L / img.h()))); z0 = 0; z1 = 1; load(); }
        // the selected line
        fillr(scope, rgb(0x10, 0x11, 0x13));
        if (view == WAVE) wave(scope, plot);
        else if (view == SPECTRUM) spectrum(scope, plot);
        else if (view == FM) fm(scope, plot);
        else eye(scope, plot);
        // over time
        fillr(hist, PANEL);
        Rc hi2{hist.l + px(6), hist.t + px(22), hist.r - px(6), hist.b - px(6)};
        DL()->AddImage(histTex.id(), ImVec2(hi2.l, hi2.t), ImVec2(hi2.r, hi2.b));
        text(hist.l + px(8), hist.t + px(4), fmt("line %s over %d frames from this one (click a row to go to that frame)", line_label(sel).c_str(), kHist), FG, BOLD, 9);
        if (hov && ImGui::IsMouseClicked(0) && hi2.in(m)) go(frame + (int64_t)((m.y - hi2.t) * kHist / hi2.h()));
        if (hov && (rast.in(m) || hist.in(m))) {
            float wh = ImGui::GetIO().MouseWheel;
            if (wh != 0) go(frame + (wh > 0 ? -1 : 1) * (ImGui::GetIO().KeyShift ? 25 : 1));
        }
        // the frame slider
        ImGui::SetCursorScreenPos(ImVec2(p0.x, rast.b + px(6)));
        ImGui::SetNextItemWidth(av.x);
        int f = (int)frame;
        if (ImGui::SliderInt("##frame", &f, 0, (int)std::max<int64_t>(0, g.frames - 1), "frame %d") && f != frame) go(f);
    }
};
}  // namespace

void lines_window(const ResultPtr &r)
{
    auto w = std::make_unique<LinesWin>();
    w->R = r;
    w->title = "VBI lines \xE2\x80\x94 " + ui::file_name(*r) + "##lines" + std::to_string((intptr_t)r.get());
    w->size = ImVec2(px(1280), px(800));
    w->init();
    add_window(std::move(w));
}
}
