#include "vitc.h"
#include "decoder.h"
#include "services.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

std::string VitcCode::text() const
{
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d%c%02d", hour, minute, second, drop ? ';' : ':', fr);
    return b;
}

bool vitcSlice(const uint8_t* line, int fmt, int n, float lineSamples, bool pal, VitcCode& out, float& quality)
{
    const float T = lineSamples / (pal ? 116.0f : 115.0f);   // samples per bit
    if (T < 2.5f || n < (int)(T * (kVitcBits + 4))) return false;
    std::vector<float> s(n);
    for (int i = 0; i < n; i++) s[i] = sampleAt(line, fmt, i);
    std::vector<float> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + n * 3 / 10, sorted.end());
    float lo = sorted[n * 3 / 10];
    std::nth_element(sorted.begin(), sorted.begin() + n * 97 / 100, sorted.end());
    float hi = sorted[n * 97 / 100];
    const float full = fmt == 1 ? 65535.0f : 255.0f;
    if (hi - lo < 0.15f * full) return false;
    float thr = (lo + hi) / 2;
    auto avg = [&](float a, float b) {
        int ia = std::max(0, (int)std::lround(a)), ib = std::min(n, (int)std::lround(b));
        if (ib <= ia) return lo;
        float sum = 0;
        for (int i = ia; i < ib; i++) sum += s[i];
        return sum / (ib - ia);
    };
    // the first sync bit: a rise that stays up for most of a bit (colour burst does not)
    int tries = 0;
    for (int st = 1; st + (int)(T * kVitcBits) < n && tries < 4; st++) {
        if (!(s[st] > thr && s[st - 1] <= thr && avg(st, st + 0.7f * T) > thr)) continue;
        tries++;
        uint8_t bits[kVitcBits];
        bool ok = true;
        float q = 1, origin = (float)st;
        for (int gI = 0; gI < 9 && ok; gI++) {
            // every group re-locks on its own 1 -> 0 sync edge: tape speed errors do not add up
            float x = origin + gI * 10 * T, edge = x + T;
            for (int i = std::max(1, (int)(edge - T / 2)); i < std::min(n, (int)(edge + T / 2)); i++)
                if (s[i - 1] > thr && s[i] <= thr) { edge = i - 1 + (s[i - 1] - thr) / (s[i - 1] - s[i]); break; }
            x = edge - T;
            if (gI == 0) origin = x;
            else origin += (x - (origin + gI * 10 * T)) / 2;   // follow the clock gently
            for (int b = 0; b < 10; b++) {
                if (gI == 8 && b >= 10) break;
                float v = avg(x + (b + 0.25f) * T, x + (b + 0.75f) * T);
                bits[gI * 10 + b] = v > thr;
                q = std::min(q, std::fabs(v - thr) / (hi - lo) * 2);
            }
            ok = bits[gI * 10] == 1 && bits[gI * 10 + 1] == 0;
        }
        if (!ok) continue;
        int crc[8] = {};
        for (int i = 0; i < kVitcBits; i++) crc[i & 7] ^= bits[i];
        bool crcOk = true;
        for (int k = 0; k < 8; k++) crcOk &= crc[k] == 0;
        if (!crcOk) continue;
        auto num = [&](int from, int len) { int v = 0; for (int k = 0; k < len; k++) v |= bits[from + k] << k; return v; };
        VitcCode c;
        c.fr = num(2, 4) + 10 * num(12, 2);
        c.second = num(22, 4) + 10 * num(32, 3);
        c.minute = num(42, 4) + 10 * num(52, 3);
        c.hour = num(62, 4) + 10 * num(72, 2);
        c.drop = bits[14];
        c.colourFrame = bits[15];
        c.fieldMark = pal ? bits[75] : bits[35];
        static const int ub[8] = {6, 16, 26, 36, 46, 56, 66, 76};
        for (int k = 0; k < 8; k++) c.user |= (uint32_t)num(ub[k], 4) << (4 * k);
        if (num(2, 4) > 9 || num(22, 4) > 9 || num(42, 4) > 9 || num(62, 4) > 9) continue;
        if (c.fr >= (pal ? 25 : 30) || c.second > 59 || c.minute > 59 || c.hour > 23) continue;
        out = c;
        quality = q;
        return true;
    }
    return false;
}

int64_t vitcToCount(const VitcCode& c, bool pal)
{
    const int fps = pal ? 25 : 30;
    int64_t n = ((int64_t)(c.hour * 60 + c.minute) * 60 + c.second) * fps + c.fr;
    if (!pal && c.drop) {   // 29.97: frame numbers 0 and 1 are skipped at every minute but each tenth
        int64_t mins = c.hour * 60 + c.minute;
        n -= 2 * (mins - mins / 10);
    }
    return n;
}

VitcCode vitcFromCount(int64_t n, bool pal, bool drop)
{
    const int fps = pal ? 25 : 30;
    const int64_t day = (int64_t)24 * 3600 * fps - (!pal && drop ? 2 * (24 * 60 - 24 * 6) : 0);
    n = ((n % day) + day) % day;
    if (!pal && drop) {
        int64_t d = n / 17982, m = n % 17982;
        n += 18 * d + (m > 1 ? 2 * ((m - 2) / 1798) : 0);
    }
    VitcCode c;
    c.drop = drop;
    c.fr = (int)(n % fps);
    c.second = (int)(n / fps % 60);
    c.minute = (int)(n / fps / 60 % 60);
    c.hour = (int)(n / fps / 3600 % 24);
    return c;
}

bool vitcAt(const std::vector<VitcCode>& codes, int64_t frame, bool pal, VitcCode& out)
{
    if (codes.empty()) return false;
    auto it = std::lower_bound(codes.begin(), codes.end(), frame, [](const VitcCode& c, int64_t f) { return c.frame < f; });
    const VitcCode* best = nullptr;
    if (it != codes.end()) best = &*it;
    if (it != codes.begin() && (!best || frame - std::prev(it)->frame <= best->frame - frame)) best = &*std::prev(it);
    // further than 10 s from any code read: the tape has none there
    if (!best || std::llabs(best->frame - frame) > (pal ? 250 : 300)) return false;
    out = vitcFromCount(vitcToCount(*best, pal) + (frame - best->frame), pal, best->drop);
    out.frame = frame;
    out.user = best->user;
    out.lineNo = best->lineNo;
    out.field = best->field;
    return true;
}

std::vector<std::string> vitcReport(const std::vector<VitcCode>& codes, bool pal, double fps)
{
    std::vector<std::string> r;
    if (codes.empty()) return r;
    char b[256];
    auto at = [&](int64_t f) {
        double s = f / fps;
        snprintf(b, sizeof b, "%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, std::fmod(s, 60));
        return std::string(b);
    };
    snprintf(b, sizeof b, "VITC time code on %zu frames: %s (recording %s) .. %s (recording %s)%s", codes.size(),
             codes.front().text().c_str(), at(codes.front().frame).c_str(), codes.back().text().c_str(), at(codes.back().frame).c_str(),
             codes.front().drop ? ", drop frame (29.97)" : "");
    r.push_back(b);
    // breaks: the code does not count on with the frames (an edit, a pause, a new tape)
    int breaks = 0;
    for (size_t i = 1; i < codes.size(); i++) {
        int64_t want = vitcToCount(codes[i - 1], pal) + (codes[i].frame - codes[i - 1].frame);
        if (vitcToCount(codes[i], pal) == want) continue;
        if (++breaks <= 30) {
            std::string a = codes[i - 1].text(), c = codes[i].text();
            snprintf(b, sizeof b, "  break at recording %s: %s -> %s", at(codes[i].frame).c_str(), a.c_str(), c.c_str());
            r.push_back(b);
        }
    }
    if (breaks > 30) r.push_back("  ... " + std::to_string(breaks - 30) + " more breaks");
    if (!breaks) r.push_back("  the code counts on without a break");
    std::map<uint32_t, int> ub;
    for (auto& c : codes) if (c.user) ub[c.user]++;
    if (!ub.empty()) {
        std::vector<std::pair<int, uint32_t>> v;
        for (auto& kv : ub) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        std::string s = "  user bits (UB8..UB1):";
        for (size_t i = 0; i < v.size() && i < 6; i++) {
            snprintf(b, sizeof b, " %08X (%d frames)", v[i].second, v[i].first);
            s += b;
        }
        r.push_back(s);
    }
    return r;
}

bool captionsUseVitc(const DecodeResult& r) { return !r.vitc.empty(); }

std::string captionTime(const DecodeResult& r, int64_t frame)
{
    VitcCode c;
    if (vitcAt(r.vitc, frame, r.geom.pal, c)) return c.text();
    return ui::rec_time(frame / ui::frame_rate(r));
}

std::vector<std::string> captionsTimed(const DecodeResult& r)
{
    if (r.captionCues.empty()) return r.captions;
    std::vector<std::string> v;
    v.push_back(captionsUseVitc(r) ? "# times: VITC time code of the tape (hh:mm:ss:ff)" : "# times: place in the recording (hh:mm:ss.s)");
    for (auto& c : r.captionCues) v.push_back("[" + captionTime(r, c.frame) + "] " + c.svc + ": " + c.text);
    return v;
}

std::string captionsSrt(const DecodeResult& r, const std::string& svc)
{
    const double fps = ui::frame_rate(r);
    const int nominal = r.geom.pal ? 25 : 30;
    auto ms = [&](int64_t frame) -> int64_t {
        VitcCode c;
        if (vitcAt(r.vitc, frame, r.geom.pal, c))
            return ((int64_t)(c.hour * 60 + c.minute) * 60 + c.second) * 1000 + c.fr * 1000 / nominal;
        return (int64_t)std::llround(frame / fps * 1000);
    };
    auto stamp = [](int64_t t) {
        char b[32];
        snprintf(b, sizeof b, "%02lld:%02lld:%02lld,%03lld", (long long)(t / 3600000), (long long)(t / 60000 % 60),
                 (long long)(t / 1000 % 60), (long long)(t % 1000));
        return std::string(b);
    };
    std::string s;
    int k = 0;
    const int64_t longest = (int64_t)(7 * fps);   // a caption nobody took off: at most 7 s
    for (auto& c : r.captionCues) {
        if (c.svc != svc) continue;
        int64_t end = c.end > c.frame ? std::min(c.end, c.frame + longest) : c.frame + longest;
        int64_t a = ms(c.frame), e = ms(end);
        if (e <= a) e = a + (int64_t)std::llround((end - c.frame) / fps * 1000);   // the time code broke in between
        s += std::to_string(++k) + "\n" + stamp(a) + " --> " + stamp(e) + "\n" + c.text + "\n\n";
    }
    return s;
}
