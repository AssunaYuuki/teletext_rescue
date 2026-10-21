// VBI repair after VHSTTX 2.7 (teletext/vbi/line.py), see vbirepair.h
#include "vbirepair.h"
#include "ui/util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// scipy.ndimage.gaussian_filter1d, mode "reflect", truncate 4
void gauss(const float* x, int n, double sigma, std::vector<float>& out)
{
    out.assign(x, x + n);
    if (sigma <= 0 || n < 2) return;
    int r = (int)(4.0 * sigma + 0.5);
    std::vector<double> k(2 * r + 1);
    double s = 0;
    for (int i = -r; i <= r; i++) s += k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (auto& v : k) v /= s;
    auto at = [&](int i) {
        // reflect: d c b a | a b c d | d c b a
        while (i < 0 || i >= n) i = i < 0 ? -i - 1 : 2 * n - i - 1;
        return x[i];
    };
    for (int i = 0; i < n; i++) {
        double v = 0;
        for (int j = -r; j <= r; j++) v += k[j + r] * at(i + j);
        out[i] = (float)v;
    }
}
double mean(const float* x, int n) { double s = 0; for (int i = 0; i < n; i++) s += x[i]; return n ? s / n : 0; }
double percentile(const float* x, int n, double p)
{
    std::vector<float> v(x, x + n);
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (n - 1);
    int i = (int)pos;
    double f = pos - i;
    return i + 1 < n ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}
double factor(int value, double coeff) { return 1.0 + ((value - 50.0) / 50.0) * coeff; }

// where the data starts: the first crossing of the middle level in the first 40 % of the line (smoothed)
double dataStart(const float* y, int n)
{
    std::vector<float> s;
    gauss(y, n, 1.0, s);
    double lo = percentile(s.data(), n, 10), hi = percentile(s.data(), n, 90);
    if (hi - lo < 20) return -1;
    double mid = (lo + hi) / 2;
    for (int i = 1; i < n * 4 / 10; i++)
        if (s[i - 1] < mid && s[i] >= mid) return i - 1 + (mid - s[i - 1]) / std::max(1e-6f, s[i] - s[i - 1]);
    return -1;
}
void shiftLine(float* y, int n, double d)   // content moved by d samples (positive: later)
{
    if (std::fabs(d) < 1e-3) return;
    std::vector<float> o(y, y + n);
    for (int i = 0; i < n; i++) {
        double t = std::clamp(i - d, 0.0, (double)(n - 1));
        int j = (int)t;
        double f = t - j;
        y[i] = (float)(o[j] * (1 - f) + o[std::min(j + 1, n - 1)] * f);
    }
}

} // namespace

const std::vector<VbiRepair::Control>& VbiRepair::controls()
{
    static const std::vector<Control> c = {
        {"brightness", "Brightness", &VbiRepair::brightness, 50}, {"sharpness", "Sharpness", &VbiRepair::sharpness, 50},
        {"gain", "Gain", &VbiRepair::gain, 50}, {"contrast", "Contrast", &VbiRepair::contrast, 50},
        {"impulse", "Impulse filter", &VbiRepair::impulse, 0}, {"temporal", "Temporal denoise", &VbiRepair::temporal, 0},
        {"noise", "Noise reduction", &VbiRepair::noise, 0}, {"hum", "Hum removal", &VbiRepair::hum, 0},
        {"black", "Auto black level", &VbiRepair::blackLevel, 0}, {"headswitch", "Head switching mask", &VbiRepair::headSwitch, 0},
        {"stabilize", "Line-to-line stabilization", &VbiRepair::stabilize, 0}, {"autogain", "Auto gain / contrast", &VbiRepair::autoGain, 0},
        {"dropout", "Dropout repair", &VbiRepair::dropout, 0}, {"adaptive", "Adaptive threshold", &VbiRepair::adaptive, 0},
        {"wow", "Wow / flutter compensation", &VbiRepair::wow, 0}, {"align", "Auto line align", &VbiRepair::align, 0}};
    return c;
}

bool VbiRepair::active() const
{
    for (auto& c : controls()) if (this->*c.v != c.neutral) return true;
    for (auto& kv : lineShift) if (kv.second) return true;
    return false;
}

std::string VbiRepair::text() const
{
    std::string s;
    for (auto& c : controls())
        if (this->*c.v != c.neutral) s += (s.empty() ? "" : ", ") + std::string(c.key) + " " + std::to_string(this->*c.v);
    for (auto& kv : lineShift)
        if (kv.second) s += (s.empty() ? "" : ", ") + fmt("line %d %+d", kv.first, kv.second);
    return s;
}

void VbiRepairer::line(float* y, int n, int key)
{
    const VbiRepair& r = r_;
    std::vector<float> a(y, y + n), tmp;
    if (r.gain != 50) { double f = factor(r.gain, 0.5); for (auto& v : a) v = (float)(v * f); }
    if (r.contrast != 50) {
        double f = factor(r.contrast, 0.5), c = mean(a.data(), n);
        for (auto& v : a) v = (float)(c + (v - c) * f);
    }
    if (r.brightness != 50) { double o = (r.brightness - 50.0) / 50.0 * 48.0; for (auto& v : a) v = (float)(v + o); }
    if (r.sharpness != 50) {
        if (r.sharpness < 50) {
            gauss(a.data(), n, 1.0, tmp);
            double blend = (50.0 - r.sharpness) / 50.0;
            for (int i = 0; i < n; i++) a[i] = (float)(a[i] * (1 - blend) + tmp[i] * blend);
        } else {
            gauss(a.data(), n, 1.0, tmp);
            double amount = (r.sharpness - 50.0) / 50.0 * 3.0;
            for (int i = 0; i < n; i++) a[i] = (float)(a[i] + (a[i] - tmp[i]) * amount);
        }
    }
    if (r.impulse > 0 && n >= 3) {
        double s = r.impulse / 100.0, blend = std::min(s, 1.0);
        double thr = std::clamp(20.0 - s * 14.0, 2.0, 20.0);
        std::vector<float> o = a;
        for (int i = 0; i < n; i++) {
            float p = o[std::max(i - 1, 0)], c = o[i], q = o[std::min(i + 1, n - 1)];
            float med = std::max(std::min(p, c), std::min(std::max(p, c), q));
            double mask = std::clamp((std::fabs(c - med) - thr) / std::max(thr, 1.0), 0.0, 1.0);
            a[i] = (float)(c + (med - c) * mask * blend);
        }
    }
    if (r.temporal > 0) {
        double s = r.temporal / 100.0, blend = std::clamp(s * 0.65, 0.0, 0.9);
        auto& h = hist_[key];
        if ((int)h.size() != n) h = a;
        else {
            for (int i = 0; i < n; i++) a[i] = (float)(a[i] * (1 - blend) + h[i] * blend);
            double store = std::min(blend + 0.1, 0.95);
            for (int i = 0; i < n; i++) h[i] = (float)(h[i] * store + a[i] * (1 - store));
        }
    }
    if (r.noise > 0) {
        double s = r.noise / 100.0;
        gauss(a.data(), n, 0.2 + s * 2.2, tmp);
        double blend = std::min(s, 1.0);
        for (int i = 0; i < n; i++) a[i] = (float)(a[i] * (1 - blend) + tmp[i] * blend);
    }
    if (r.hum > 0) {
        double s = r.hum / 100.0;
        gauss(a.data(), n, 12.0 + s * 64.0, tmp);
        double m = mean(tmp.data(), n), blend = std::min(s, 1.0);
        for (int i = 0; i < n; i++) a[i] = (float)(a[i] - (tmp[i] - m) * blend);
    }
    if (r.blackLevel > 0) {
        int m = std::max(n / 8, 1);
        double ref = percentile(a.data(), m, 20), f = std::min(r.blackLevel / 100.0, 1.0);
        for (auto& v : a) v = (float)(v - ref * f);
    }
    if (r.headSwitch > 0 && n >= 16) {
        double s = r.headSwitch / 100.0, blend = std::clamp(s * 0.85, 0.0, 1.0);
        int w = std::clamp((int)std::lround(n * (0.015 + s * 0.12)), 6, std::max(n / 4, 6));
        int tail = std::max(n - w, 0), anchor = std::max(tail - w, 0);
        double target = tail > anchor ? mean(&a[anchor], tail - anchor) : a[std::max(tail - 1, 0)];
        int len = n - tail;
        for (int i = tail; i < n; i++) {
            double ramp = len > 1 ? (double)(i - tail) / (len - 1) : 1.0;
            a[i] = (float)(a[i] + (target - a[i]) * ramp * blend);
        }
    }
    if (r.stabilize > 0) {   // held to the line before (same frame) when it is the line just above
        double s = r.stabilize / 100.0, blend = std::clamp(s * 0.7, 0.0, 0.9);
        auto& prev = stab_[-1];
        auto& last = stab_[-2];
        bool near = !last.empty() && key - (int)last[0] >= 0 && key - (int)last[0] <= 2;
        if ((int)prev.size() == n && near) {
            double cm = mean(a.data(), n), pm = mean(prev.data(), n), cs = 0, ps = 0;
            for (int i = 0; i < n; i++) { cs += (a[i] - cm) * (a[i] - cm); ps += (prev[i] - pm) * (prev[i] - pm); }
            cs = std::sqrt(cs / n); ps = std::sqrt(ps / n);
            for (int i = 0; i < n; i++) {
                double st = cs > 1e-3 && ps > 1e-3 ? (a[i] - cm) * (ps / cs) + pm : a[i] + (pm - cm);
                a[i] = (float)(a[i] * (1 - blend) + st * blend);
            }
            for (int i = 0; i < n; i++) prev[i] = (float)(prev[i] * 0.35 + a[i] * 0.65);
        } else prev = a;
        last = {(float)key};
    }
    if (r.autoGain > 0 && n >= 8) {
        double blend = std::clamp(r.autoGain / 100.0, 0.0, 1.0);
        double lo = percentile(a.data(), n, 10), hi = percentile(a.data(), n, 90);
        if (hi - lo > 1.0) {
            double sc = (236.0 - 18.0) / (hi - lo);
            for (auto& v : a) v = (float)(v * (1 - blend) + std::clamp((v - lo) * sc + 18.0, 0.0, 255.0) * blend);
        }
    }
    for (auto& v : a) v = std::clamp(v, 0.0f, 255.0f);
    // decoder tuning, done on the samples here
    if (r.dropout > 0 && n >= 5) {
        double s = r.dropout / 100.0, thr = std::clamp(48.0 - s * 32.0, 8.0, 48.0), blend = 0.35 + s * 0.55;
        std::vector<float> o = a;
        for (int i = 0; i < n; i++) {
            float w[5];
            for (int k = 0; k < 5; k++) w[k] = o[std::clamp(i + k - 2, 0, n - 1)];
            std::nth_element(w, w + 2, w + 5);
            double mask = std::clamp((std::fabs(o[i] - w[2]) - thr) / std::max(thr, 1.0), 0.0, 1.0);
            a[i] = (float)(o[i] + (w[2] - o[i]) * mask * blend);
        }
    }
    if (r.adaptive > 0) {
        double s = std::clamp(r.adaptive / 100.0, 0.0, 1.0);
        gauss(a.data(), n, 4.0 + (1 - s) * 8.0, tmp);
        double m = mean(tmp.data(), n);
        for (int i = 0; i < n; i++) a[i] = (float)(a[i] * (1 - s) + (a[i] - tmp[i] + m) * s);
    }
    if (r.wow > 0 || r.align > 0) {
        double st = dataStart(a.data(), n);
        if (st >= 0) {
            double target = st;
            if (r.wow > 0) {   // the start of this line held near its own start a frame before
                auto& h = align_[key];
                double s = r.wow / 100.0;
                if (!h.empty()) {
                    double lim = std::clamp(10.0 - s * 6.0, 2.0, 10.0), prev = h[0];
                    double corr = prev + std::clamp(target - prev, -lim, lim), blend = std::clamp(0.18 + s * 0.62, 0.0, 0.85);
                    target = target * (1 - blend) + corr * blend;
                    h[0] = prev * 0.4 + target * 0.6;
                } else h = {target};
            }
            if (r.align > 0) {   // ... and near the median start of the recent lines
                auto& h = align_[-1];
                double s = r.align / 100.0;
                if (h.size() >= 2) {
                    std::vector<double> v = h;
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    double med = v[v.size() / 2], mx = std::clamp(2.0 + s * 18.0, 2.0, 20.0);
                    target += std::clamp(med - target, -mx, mx) * std::clamp(0.12 + s * 0.78, 0.0, 0.9);
                }
                h.push_back(target);
                if (h.size() > 32) h.erase(h.begin());
            }
            shiftLine(a.data(), n, target - st);
        }
    }
    // the line moved by hand (Stabilize VBI)
    if (auto it = r.lineShift.find(key); it != r.lineShift.end() && it->second) shiftLine(a.data(), n, it->second);
    for (int i = 0; i < n; i++) y[i] = std::clamp(a[i], 0.0f, 255.0f);
}

void VbiRepairer::frame(uint8_t* frame, const VbiGeometry& g)
{
    if (!r_.active()) return;
    const int n = g.samplesPerLine;
    std::vector<float> y(n);
    for (int l = 0; l < g.linesPerFrame; l++) {
        uint8_t* p = frame + (size_t)l * g.lineBytes;
        for (int i = 0; i < n; i++) y[i] = g.fmt == 1 ? (p[2 * i] | p[2 * i + 1] << 8) / 256.0f : p[i];
        line(y.data(), n, l);
        for (int i = 0; i < n; i++) {
            if (g.fmt == 1) {
                int v = std::clamp((int)std::lround(y[i] * 256.0f), 0, 65535);
                p[2 * i] = (uint8_t)(v & 0xff); p[2 * i + 1] = (uint8_t)(v >> 8);
            } else p[i] = (uint8_t)std::lround(y[i]);
        }
    }
}

bool vbiRepairFile(const std::string& in, const std::string& out, VbiFormat format, const VbiRepair& r,
                   const std::function<void(double)>& progress, std::atomic<bool>* cancel, std::string& err)
{
    return vbiRepairFile(in, out, format, r, {}, 0, -1, progress, cancel, err);
}

bool vbiRepairFile(const std::string& in, const std::string& out, VbiFormat format, const VbiRepair& r,
                   const std::vector<VbiRepairRange>& ranges, int64_t from, int64_t to,
                   const std::function<void(double)>& progress, std::atomic<bool>* cancel, std::string& err)
{
    VbiFile vf;
    if (!vf.open(in, err, format)) return false;
    const VbiGeometry& g = vf.geom();
    if (g.storedPerField) { err = "full-field .tbc files are not repaired (only VBI-only layouts)"; return false; }
    from = std::clamp<int64_t>(from, 0, std::max<int64_t>(g.frames - 1, 0));
    to = to < 0 ? g.frames - 1 : std::min(to, g.frames - 1);
    if (to < from) { err = "no frames to write"; return false; }
    FILE* f = _wfopen(widen(out).c_str(), L"wb");
    if (!f) { err = "cannot write " + out; return false; }
    // one repairer per setting (its temporal state stays with the frames it repairs)
    VbiRepairer main(r);
    std::vector<VbiRepairer> per;
    for (auto& rg : ranges) per.emplace_back(rg.r);
    std::vector<uint8_t> buf;
    const int64_t chunk = 500, total = to - from + 1;
    for (int64_t f0 = from; f0 <= to; f0 += chunk) {
        if (cancel && *cancel) { fclose(f); err = "cancelled"; return false; }
        int64_t got = vf.read(f0, std::min(chunk, to + 1 - f0), buf);
        if (got <= 0) break;
        for (int64_t k = 0; k < got; k++) {
            int64_t fr = f0 + k;
            VbiRepairer* rp = &main;
            for (size_t i = 0; i < ranges.size(); i++)
                if (fr >= ranges[i].from && fr <= ranges[i].to) { rp = &per[i]; break; }
            rp->frame(&buf[(size_t)k * g.frameBytes()], g);
        }
        if (fwrite(buf.data(), 1, (size_t)got * g.frameBytes(), f) != (size_t)got * g.frameBytes()) { fclose(f); err = "write failed"; return false; }
        if (progress) progress((double)(f0 + got - from) / total);
    }
    fclose(f);
    return true;
}

bool vbiRepairParse(const std::string& spec, VbiRepair& r, std::string& err)
{
    r = VbiRepair();
    size_t a = 0;
    while (a < spec.size()) {
        size_t b = spec.find(',', a);
        if (b == std::string::npos) b = spec.size();
        std::string item = spec.substr(a, b - a);
        a = b + 1;
        if (item.empty()) continue;
        size_t e = item.find('=');
        std::string k = item.substr(0, e);
        int v = e == std::string::npos ? 50 : atoi(item.c_str() + e + 1);
        if (k.size() > 5 && k.compare(0, 5, "shift") == 0 && isdigit((unsigned char)k[5])) {   // shift7=-3: line 7 moved
            r.lineShift[atoi(k.c_str() + 5)] = std::clamp(v, -64, 64);
            continue;
        }
        bool found = false;
        for (auto& c : VbiRepair::controls())
            if (k == c.key) { r.*c.v = std::clamp(v, 0, 100); found = true; }
        if (!found) { err = "unknown repair control '" + k + "'"; return false; }
    }
    return true;
}
