#include "vbicodes.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
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

// A line already known to carry VPS (vpsRead took it in many frames of the recording): the frames vpsRead leaves
// out. The start is where the run-in and start code fit best (soft, no threshold), the sampling point within a
// sample of it is the one with the widest eye over the 104 bits; a line passes with up to 14 doubtful bits.
// On a line not known to be VPS this reads noise now and then (1+1: 167 false lines on the other 31 rows of 819
// frames), so it is not for looking for VPS. 1+1: 817 of 819 frames (vpsRead 713), Novy: 1160 of 1164 (1087)
bool vpsReadKnown(const float* y, int n, double fs, uint8_t b[13])
{
    if (n < 100) return false;
    const double H = fs / 5e6;
    const uint32_t CRI = 0xAAAA8A99;
    float pat[32], pm = 0;
    for (int k = 0; k < 32; k++) { pat[k] = (float)((CRI >> (31 - k)) & 1); pm += pat[k]; }
    for (int k = 0; k < 32; k++) pat[k] -= pm / 32;
    const double end = n - (32 + 13 * 16) * H;
    double bs = -1;
    float bc = 0;
    for (double s = 0; s < end; s += 0.25) {
        float h[32], m = 0;
        for (int k = 0; k < 32; k++) { h[k] = at(y, n, s + (k + 0.5) * H); m += h[k]; }
        m /= 32;
        float c = 0, a = 0;
        for (int k = 0; k < 32; k++) { c += pat[k] * (h[k] - m); a += std::fabs(h[k] - m); }
        if (a < 32 * 3.0f) continue;
        c /= a * 0.5f;   // 1 for a clean square signal
        if (c > bc) { bc = c; bs = s; }
    }
    if (bs < 0 || bc < 0.6f) return false;
    float q[104], best = -1;
    for (double dd = -1.0; dd <= 1.001; dd += 0.25) {
        float t[104], e = 0;
        double p = bs + dd + 32 * H;
        for (int i = 0; i < 104; i++, p += 2 * H) { t[i] = at(y, n, p + 0.5 * H) - at(y, n, p + 1.5 * H); e += std::fabs(t[i]); }
        if (e > best) { best = e; std::copy(t, t + 104, q); }
    }
    float srt[104];
    for (int i = 0; i < 104; i++) srt[i] = std::fabs(q[i]);
    std::nth_element(srt, srt + 52, srt + 104);
    int weak = 0;
    for (int i = 0; i < 104; i++) weak += std::fabs(q[i]) < 0.35f * srt[52];
    if (weak > 14) return false;
    for (int i = 0; i < 13; i++) {
        int c8 = 0;
        for (int j = 0; j < 8; j++) c8 = (c8 << 1) | (q[i * 8 + j] > 0);
        b[i] = (uint8_t)c8;
    }
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

// ---------------------------------------------------------------- VPS of a whole recording
namespace {
uint8_t rev8(uint8_t v) { uint8_t r = 0; for (int i = 0; i < 8; i++) r |= ((v >> i) & 1) << (7 - i); return r; }
std::string bin(int v, int n) { std::string s; for (int i = n - 1; i >= 0; i--) s += ((v >> i) & 1) ? '1' : '0'; return s; }
std::string clockOf(int64_t frame, double fps)
{
    double t = frame / fps;
    int m = (int)(t / 60);
    return fmt("%02d:%05.2f", m, t - m * 60.0);
}
std::string countryOf(int cni)
{
    switch (cni >> 8) {
    case 0xD: return "Germany";
    case 0xA: return "Austria";
    case 0x4: return "Switzerland";
    case 0x7: return "Ukraine";
    }
    return "";
}
// the label of a reading as people read it (PIL part only)
std::string pilText(const VpsLabel& l)
{
    if (l.day == 0 && l.month == 15 && l.minute == 63 && l.hour >= 28) {
        static const char* const sp[] = {"continuation code (the programme goes on)", "interruption code (the programme is interrupted: a VPS recorder pauses)",
                                         "record inhibit / terminate (nothing here is to be recorded)",
                                         "timer control code (no programme label is sent: a recorder runs by its own clock)"};
        return sp[l.hour - 28];
    }
    if (l.day >= 1 && l.day <= 31 && l.month >= 1 && l.month <= 12 && l.hour < 24 && l.minute < 60)
        return fmt("programme announced for %02d.%02d at %02d:%02d (day.month, the start time printed in the programme guide)", l.day, l.month, l.hour, l.minute);
    return fmt("day %d, month %d, hour %d, minute %d - not a date and not one of the four service codes of EN 300 231 (the broadcaster's own filling)",
               l.day, l.month, l.hour, l.minute);
}
}

namespace {
// One stretch of byte 4 readings (frame in the stretch, character with bit 7 = mark). The capture may have lost
// frames regularly (Behold / SAA713x under load: 2 of every 3 frames kept, VIVA, Eurosport, ARD 1995): frame f of
// the file is then frame t = (a f + p) / b of the station's turn. Tried: no loss first, then the common losses;
// a model counts when every place of the turn is read twice or more, 97% of the readings agree with their place
// and exactly one place carries the mark (a turn shown twice - what a loss looks like without the model - has two).
struct TextFit { int a = 1, b = 1, L = 0; std::string text; };
bool fitStretch(const std::vector<std::pair<int, int>>& pts, TextFit& out)
{
    static const int models[][2] = {{1, 1}, {3, 2}, {2, 1}, {4, 3}, {5, 4}, {5, 3}, {3, 1}};
    const int n = (int)pts.size();
    std::vector<std::vector<uint8_t>> at;
    int cnt[256];
    for (auto& m : models) {
        const int a = m[0], b = m[1];
        for (int L = 1; L <= 32 && 2 * L <= n; L++)
            for (int p = 0; p < b; p++) {
                at.assign(L, {});
                for (auto& x : pts) at[(size_t)(((int64_t)a * x.first + p) / b % L)].push_back((uint8_t)x.second);
                int agree = 0, marks = 0, markAt = -1;
                std::vector<int> maj(L);
                bool ok = true;
                for (int k = 0; k < L && ok; k++) {
                    if (at[k].size() < 2) { ok = false; break; }
                    int bi = at[k][0];
                    for (uint8_t c : at[k]) cnt[c] = 0;
                    for (uint8_t c : at[k]) if (++cnt[c] > cnt[bi]) bi = c;
                    agree += cnt[bi];
                    maj[k] = bi;
                    if (bi & 0x80) { marks++; markAt = k; }
                }
                if (!ok || marks != 1 || agree < 0.97 * n) continue;
                out.a = a; out.b = b; out.L = L; out.text.clear();
                for (int k = 0; k < L; k++) {
                    int c = maj[(markAt + k) % L] & 0x7F;
                    out.text += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                }
                return true;
            }
    }
    return false;
}
std::string lettersOf(const std::string& s)
{
    std::string t;
    for (char c : s) if (isalnum((unsigned char)c)) t += (char)toupper((unsigned char)c);
    return t;
}
}

std::string vpsCniGuess(int cni)
{
    if (!cni || cni == 0xFFF || !vpsCniName(cni).empty()) return "";
    std::string name;
    int found = 0, code = 0;
    for (int d = -2; d <= 2; d++) {
        int c = cni + d;
        if (d == 0 || (c >> 8) != (cni >> 8)) continue;
        std::string n = vpsCniName(c);
        if (!n.empty()) { name = n; code = c; found++; }
    }
    return found == 1 ? name + fmt(" (%03X)", code) : "";
}

std::string vpsDisplayName(const std::string& station0, int cni)
{
    // the text as people write it: no spaces at the ends, "V I V A" -> "VIVA", runs of spaces -> one
    std::string s = station0;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    bool spaced = s.size() >= 3 && s.size() % 2 == 1;
    for (size_t i = 0; i < s.size() && spaced; i++) spaced = (i % 2 == 1) == (s[i] == ' ');
    if (spaced) { std::string t; for (size_t i = 0; i < s.size(); i += 2) t += s[i]; s = t; }
    std::string t;
    for (char c : s) if (!(c == ' ' && !t.empty() && t.back() == ' ')) t += c;
    s = t;
    const std::string reg = vpsCniName(cni);
    const std::string a = lettersOf(s), b = lettersOf(reg);
    if (!reg.empty() && !a.empty() && (a == b || (a.size() >= 4 && b.rfind(a, 0) == 0) || (b.size() >= 3 && a.rfind(b, 0) == 0))) return reg;
    if (!a.empty()) return s;
    if (!reg.empty()) return reg;
    std::string g = vpsCniGuess(cni);
    return g.empty() ? "" : g.substr(0, g.rfind(" (")) + "?";
}

// byte 4: a text, one character a frame, bits in the order sent (the first bit sent is the lowest); bit 7 marks
// the first character. Read when nearly all its values are letters and it repeats with a fixed period
std::string vpsStationText(const std::vector<std::pair<int64_t, uint8_t>>& b4, int* periodOut, VpsTextStats* stats)
{
    if (periodOut) *periodOut = 0;
    if (stats) *stats = VpsTextStats{};
    if (b4.size() < 12) return "";
    // stretches of 96 frames (half overlapping), each fitted by itself (the loss comes and goes within a
    // recording: ARD 23.11.95 lost 1 frame in 3 from frame 201 to ~990 only); the text most stretches give
    {
        const int64_t f0 = b4.front().first;
        std::map<int64_t, int> byFrame;
        for (auto& x : b4) byFrame[x.first - f0] = rev8(x.second);
        const int64_t span = byFrame.rbegin()->first + 1;
        const int C = 96, STEP = 48;
        std::map<std::string, int> texts;
        std::map<std::string, int> periodOf;
        std::map<std::pair<int, int>, int> losses;
        int stretches = 0, lossy = 0;
        for (int64_t c0 = 0; c0 == 0 || c0 + C <= span; c0 += STEP) {
            std::vector<std::pair<int, int>> pts;
            for (auto it = byFrame.lower_bound(c0); it != byFrame.end() && it->first < c0 + C; ++it) pts.push_back({(int)(it->first - c0), it->second});
            if ((int)pts.size() < std::min<int64_t>(C, span) * 6 / 10 || pts.size() < 12) continue;
            TextFit fit;
            if (!fitStretch(pts, fit)) continue;
            stretches++;
            texts[fit.text]++;
            periodOf[fit.text] = fit.L;
            if (fit.a != fit.b) { lossy++; losses[{fit.b, fit.a}]++; }
        }
        if (!texts.empty()) {
            auto best = texts.begin();
            for (auto it = texts.begin(); it != texts.end(); ++it) if (it->second > best->second) best = it;
            if (stats) {
                stats->stretches = stretches; stats->lossy = lossy;
                int bc = 0;
                for (auto& l : losses) if (l.second > bc) { bc = l.second; stats->keepNum = l.first.first; stats->keepDen = l.first.second; }
            }
            if (periodOut) *periodOut = periodOf[best->first];
            std::string station = best->first;
            while (!station.empty() && station.back() == ' ') station.pop_back();
            if (!station.empty()) return station;
        }
    }
    const int64_t f0 = b4.front().first, span = b4.back().first - f0 + 1;
    if (span <= 0 || span > 200000) return "";
    std::vector<int> seq((size_t)span, -1);
    int printable = 0, seen[256] = {0}, distinct = 0;
    for (auto& x : b4) {
        if (x.first < f0 || x.first - f0 >= span) continue;
        int c = rev8(x.second);
        seq[(size_t)(x.first - f0)] = c;
        printable += (c & 0x7F) >= 0x20 && (c & 0x7F) < 0x7F;
        if (!seen[c]++) distinct++;
    }
    if (distinct < 2 || printable * 10 < (int)b4.size() * 9) return "";
    int period = 0;
    for (int p = 1; p <= 64 && !period; p++) {
        int same = 0, both = 0;
        for (size_t f = 0; f + p < seq.size(); f++)
            if (seq[f] >= 0 && seq[f + p] >= 0) { both++; same += seq[f] == seq[f + p]; }
        if (both >= 3 * p && same >= both * 0.9) period = p;
    }
    if (!period) return "";
    std::vector<int> ch(period, 0x20);
    for (int k = 0; k < period; k++) {
        int cnt[256] = {0}, bi = 0;
        for (size_t f = k; f < seq.size(); f += period) if (seq[f] >= 0) cnt[seq[f]]++;
        for (int i = 0; i < 256; i++) if (cnt[i] > cnt[bi]) bi = i;
        ch[k] = bi;
    }
    // from the marked character; the frame the window starts at does not matter (the phase is the mark's)
    int st = 0;
    for (int k = 0; k < period; k++) if (ch[k] & 0x80) { st = k; break; }
    std::string station;
    for (int k = 0; k < period; k++) { int c = ch[(st + k) % period] & 0x7F; station += (c >= 0x20 && c < 0x7F) ? (char)c : '?'; }
    while (!station.empty() && station.back() == ' ') station.pop_back();
    if (periodOut) *periodOut = period;
    return station;
}

VpsFile vpsAnalyse(const std::vector<VpsReading>& r, int64_t frames, double fps, int line, int field, const VpsClocks* clocks)
{
    VpsFile out;
    if (r.empty()) return out;
    out.readings = r; out.frames = frames; out.fps = fps; out.line = line; out.field = field;
    auto add = [&](const std::string& s) { out.report.push_back(s); };
    // every byte voted over the whole recording, and how often it had that value
    uint8_t v[13];
    int share[13], distinct[13];
    for (int k = 0; k < 13; k++) {
        int cnt[256] = {0}, bi = 0, d = 0;
        for (auto& x : r) cnt[x.b[k]]++;
        for (int i = 0; i < 256; i++) { if (cnt[i] > cnt[bi]) bi = i; d += cnt[i] > 0; }
        v[k] = (uint8_t)bi; share[k] = cnt[bi]; distinct[k] = d;
    }
    const VpsLabel l = vpsLabel(v);
    const std::string reg = vpsCniName(l.cni), ctry = countryOf(l.cni);

    // byte 4: a text, one character a frame, bits in the order sent (the first bit sent is the lowest); bit 7 marks
    // the first character. Read when nearly all its values are letters and it repeats with a fixed period
    std::string station;
    int period = 0;
    VpsTextStats ts;
    {
        std::vector<std::pair<int64_t, uint8_t>> b4;
        for (auto& x : r) b4.push_back({x.frame, x.b[1]});
        station = vpsStationText(b4, &period, &ts);
    }
    out.station = station;
    out.cni = l.cni;
    // one name: "VIVA", "Eurosport" (sent as "Eurospor"), "NOVY.TV.", "DBP-2"
    const std::string shown = vpsDisplayName(station, l.cni);
    out.name = !shown.empty() ? shown : fmt("CNI %03X", l.cni);

    static const char* const snd[] = {"not given", "mono", "stereo", "two sound channels (bilingual)"};
    if (out.name.rfind("CNI ", 0) == 0) out.head = "VPS: " + out.name + (ctry.empty() ? std::string() : " (" + ctry + ")");
    else out.head = "VPS: " + out.name + fmt(" (CNI %03X", l.cni) + (ctry.empty() ? std::string() : ", " + ctry) + ")";
    {   // the date and time it was on the air, when the label or a clock of the recording tells it
        std::string when;
        if (l.day >= 1 && l.day <= 31 && l.month >= 1 && l.month <= 12 && l.hour < 24 && l.minute < 60) when = fmt("programme of %02d.%02d %02d:%02d", l.day, l.month, l.hour, l.minute);
        if (clocks && !clocks->ttFrom.empty()) when += (when.empty() ? "" : ", ") + std::string("on the air ") + (clocks->ttDate.empty() ? "" : clocks->ttDate + " ") + clocks->ttFrom;
        else if (clocks && !clocks->vitcFrom.empty()) when += (when.empty() ? "" : ", ") + std::string("VITC ") + clocks->vitcFrom;
        if (!when.empty()) out.head += " \xC2\xB7 " + when;
    }
    out.head += fmt(" \xC2\xB7 line %d, read in %.0f%% of the frames", line, 100.0 * r.size() / std::max<int64_t>(1, frames));

    add(fmt("VPS (Video Programme System, EN 300 231): line %d of field %d, biphase at 2.5 Mbit/s, 15 bytes a frame", line, field));
    add(fmt("  read in %zu of %lld frames (%.1f%%), %s .. %s of the recording", r.size(), (long long)frames, 100.0 * r.size() / std::max<int64_t>(1, frames),
            clockOf(r.front().frame, fps).c_str(), clockOf(r.back().frame, fps).c_str()));
    {
        int sec = 0;
        for (auto& x : r) sec += x.second;
        if (sec) add(fmt("  %d of them by the second, more tolerant reading (frames the tape spoiled; their bytes may have single wrong bits)", sec));
    }
    add("");
    {
        std::string guess = reg.empty() ? vpsCniGuess(l.cni) : "";
        add("  Name              " + out.name);
        add("  Network (CNI)     " + fmt("%03X", l.cni) + "  =  country " + fmt("%X", l.cni >> 8) + (ctry.empty() ? "" : " (" + ctry + ")") + ", network " + fmt("%02X", l.cni & 0xFF) +
            (reg.empty() ? "; not in the ETSI TS 101 231 register" + (guess.empty() ? std::string() : "; next to it there: " + guess + " - a code one off, likely that network")
                         : "; ETSI TS 101 231 register: " + reg));
    }
    if (!station.empty()) {
        add("  Station text      \"" + station + "\"  (byte 4: one character a frame, " + fmt("%d", period) + " frames a turn" +
            (period ? fmt(" = %.2f s", period / fps) : std::string()) + "; bit 7 marks the first character)");
        if (!reg.empty() && out.name != reg)
            add("                    the station names itself differently from the register entry of this CNI (" + reg + ")");
    } else if (distinct[1] >= 2)
        add(fmt("  Byte 4            changes from frame to frame (%d values), not a readable text", distinct[1]));
    add("  Programme label   " + pilText(l));
    add(fmt("                    PIL = %05X: day %s, month %s, hour %s, minute %s", ((v[8] & 0x3F) << 14) | (v[9] << 6) | (v[10] >> 2), bin(l.day, 5).c_str(),
            bin(l.month, 4).c_str(), bin(l.hour, 5).c_str(), bin(l.minute, 6).c_str()));
    // the time of the broadcast: the teletext clock and VITC of the same recording
    if (clocks && !clocks->ttFrom.empty()) {
        add("  On the air        teletext clock " + (clocks->ttDate.empty() ? std::string() : clocks->ttDate + "  ") + clocks->ttFrom +
            (clocks->ttTo.empty() || clocks->ttTo == clocks->ttFrom ? std::string() : " .. " + clocks->ttTo));
        int hh = 0, mm = 0, ss = 0;
        if (l.day >= 1 && l.day <= 31 && l.month >= 1 && l.month <= 12 && l.hour < 24 && l.minute < 60 && sscanf(clocks->ttFrom.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) {
            int dd = 0, mo = 0;
            for (const char* d = clocks->ttDate.c_str(); *d; d++)
                if (isdigit((unsigned char)*d) && sscanf(d, "%d.%d", &dd, &mo) == 2) break;
            int diff = (hh * 60 + mm) - (l.hour * 60 + l.minute);
            if ((!dd || (dd == l.day && mo == l.month)) && diff >= 0 && diff < 24 * 60)
                add(fmt("                    the labelled programme began at %02d:%02d, %d h %02d min before the recording starts", l.hour, l.minute, diff / 60, diff % 60));
        }
    }
    if (clocks && !clocks->vitcFrom.empty())
        add("  VITC time code    " + clocks->vitcFrom + (clocks->vitcTo.empty() ? std::string() : " .. " + clocks->vitcTo) + "  (the studio's or the tape's own time code)");
    add(std::string("  Sound             ") + snd[l.sound] + fmt("  (byte 5 bits 7-6 = %s; its other bits %s %s)", bin(v[2] >> 6, 2).c_str(), bin((v[2] >> 4) & 3, 2).c_str(), bin(v[2] & 15, 4).c_str()));
    add("  Programme type    " + (l.pty == 0xFF ? std::string("FF (none sent)") : l.pty == 0 ? std::string("00 (not given)") : fmt("%02X", l.pty) + (l.pty >= 0x80 ? " (a series code)" : "")) + "  (byte 15)");
    // frames the capture lost: the station text shows it (and the clocks, when there are any)
    {
        const double rec = frames / fps;
        std::string by;
        if (clocks && clocks->ttSeconds > 0 && rec > 10 && clocks->ttSeconds > rec * 1.1)
            by += fmt("; the teletext clock ran %.0f s in %.1f s of the recording (x%.2f)", clocks->ttSeconds, rec, clocks->ttSeconds / rec);
        if (clocks && clocks->vitcSeconds > 0 && clocks->vitcFrames > 25 && clocks->vitcSeconds > clocks->vitcFrames / fps * 1.1)
            by += fmt("; VITC ran %.1f s in %.1f s of the recording (x%.2f)", clocks->vitcSeconds, clocks->vitcFrames / fps, clocks->vitcSeconds * fps / clocks->vitcFrames);
        if (ts.lossy || !by.empty()) {
            add("");
            if (ts.lossy)
                add(fmt("  FRAMES LOST BY THE CAPTURE: in %d of the %d stretches of %.0f s the station text came as %d frames of every %d", ts.lossy, ts.stretches, 96 / fps,
                        ts.keepNum, ts.keepDen) + by + ".");
            else
                add("  FRAMES LOST BY THE CAPTURE: " + by.substr(2) + ".");
            add("  The recording does not hold every frame of the broadcast (the card's driver or the computer did not keep up);");
            add("  the station text is read with that loss taken into account. Teletext and datacasts miss those frames too.");
        }
    }
    add("");
    add("  The bytes, voted over the recording (byte: value, bits as sent first to last, in how many readings, other values seen):");
    static const char* const what[13] = {"not used by recorders (the broadcaster's own)", "not used by recorders (the broadcaster's own; here: text)", "sound and reserved bits",
                                         "reserved", "reserved", "reserved", "reserved", "reserved", "network bits 7-6; label: day, month", "label: month, hour, minute",
                                         "label: minute; country (high bits)", "country (low bits), network bits 5-0", "programme type"};
    for (int k = 0; k < 13; k++) {
        std::string w = what[k];
        if (k == 1 && station.empty()) w = "not used by recorders (the broadcaster's own)";
        add(fmt("    byte %2d  %02X  %s  %5.1f%%  %s%s", k + 3, v[k], bin(v[k], 8).c_str(), 100.0 * share[k] / r.size(), w.c_str(),
                distinct[k] > 1 ? fmt("; %d values in all", distinct[k]).c_str() : ""));
    }
    // values of the bytes that really change (more than a tape's stray errors): each value and its share
    for (int k = 0; k < 13; k++) {
        if (share[k] >= r.size() * 0.97) continue;
        std::vector<std::pair<int, int>> vals;
        int cnt[256] = {0};
        for (auto& x : r) cnt[x.b[k]]++;
        for (int i = 0; i < 256; i++) if (cnt[i] * 100 >= (int)r.size()) vals.push_back({cnt[i], i});
        std::sort(vals.rbegin(), vals.rend());
        std::string s = fmt("    byte %d takes %zu values often:", k + 3, vals.size());
        for (size_t i = 0; i < vals.size() && i < 24; i++) {
            int c = rev8((uint8_t)vals[i].second) & 0x7F;
            s += fmt(" %02X", vals[i].second) + (k == 1 && c >= 0x21 && c < 0x7F ? fmt("='%c'", c) : std::string()) + fmt(" (%d)", vals[i].first);
        }
        add(s);
    }

    // the label as it changed: second by second, every byte of 5..15 voted in the second; a new label counts when
    // the next second says the same (a tape's bad second does not make a change)
    struct Sec { int64_t frame; std::string key; uint8_t b[13]; int n; };
    std::vector<Sec> secs;
    const int step = std::max(1, (int)std::lround(fps));
    for (size_t i = 0; i < r.size();) {
        int64_t s0 = r[i].frame / step;
        size_t j = i;
        while (j < r.size() && r[j].frame / step == s0) j++;
        Sec s; s.frame = r[i].frame; s.n = (int)(j - i);
        for (int k = 0; k < 13; k++) {
            int cnt[256] = {0}, bi = 0;
            for (size_t q = i; q < j; q++) cnt[r[q].b[k]]++;
            for (int q = 0; q < 256; q++) if (cnt[q] > cnt[bi]) bi = q;
            s.b[k] = (uint8_t)bi;
        }
        s.b[0] = v[0]; s.b[1] = v[1];   // the label is bytes 5..15
        s.key.assign((const char*)s.b + 2, 11);
        secs.push_back(s);
        i = j;
    }
    add("");
    add("  The label through the recording (time in the recording, frame):");
    std::string last;
    int changes = 0;
    for (size_t i = 0; i < secs.size(); i++) {
        if (secs[i].key == last) continue;
        bool held = i + 1 >= secs.size() || secs[i + 1].key == secs[i].key || secs[i].n >= step * 3 / 4;
        if (!held && !last.empty()) continue;
        last = secs[i].key;
        std::string t = vpsText(secs[i].b);
        t = t.substr(0, t.find(" \xC2\xB7 source data"));   // bytes 3-4 are not the label
        if (++changes <= 200) add("    [" + clockOf(secs[i].frame, fps) + "] frame " + fmt("%lld", (long long)secs[i].frame) + ": " + t);
    }
    if (changes > 200) add(fmt("    ... %d changes in all", changes));
    if (changes == 1) add("    the same label all through the recording");

    // frames without VPS: gaps of half a second and more
    {
        std::vector<std::string> gaps;
        int64_t lost = frames - (int64_t)r.size(), prev = -1;
        int ngaps = 0;
        auto gap = [&](int64_t a, int64_t b) {   // frames a..b-1 missing
            if (b - a < step / 2) return;
            if (++ngaps <= 20) gaps.push_back("    [" + clockOf(a, fps) + " .. " + clockOf(b, fps) + fmt("] %lld frames", (long long)(b - a)));
        };
        for (auto& x : r) { gap(prev + 1, x.frame); prev = x.frame; }
        gap(prev + 1, frames);
        add("");
        if (lost <= 0) add("  No frame without VPS.");
        else {
            add(fmt("  %lld frames without a readable VPS line (%.1f%%)%s", (long long)lost, 100.0 * lost / std::max<int64_t>(1, frames),
                    ngaps ? fmt("; gaps of half a second and more: %d", ngaps).c_str() : ", single frames only (tape noise)"));
            for (auto& g : gaps) add(g);
        }
        // readings that differ from the vote in the label bytes: tape errors
        int bad = 0;
        for (auto& x : r) bad += !std::equal(x.b + 2, x.b + 13, v + 2);
        if (changes == 1) add(fmt("  %d of the %zu readings differ from the voted label in some bit (tape errors).", bad, r.size()));
    }

    // every frame
    out.csv = "frame;time;b3;b4;b5;b6;b7;b8;b9;b10;b11;b12;b13;b14;b15;byte 4 as a character\r\n";
    for (auto& x : r) {
        out.csv += fmt("%lld;%s", (long long)x.frame, clockOf(x.frame, fps).c_str());
        for (int k = 0; k < 13; k++) out.csv += fmt(";%02X", x.b[k]);
        int c = rev8(x.b[1]);
        out.csv += std::string(";") + ((c & 0x7F) > 0x20 && (c & 0x7F) < 0x7F && (c & 0x7F) != ';' ? std::string(1, (char)(c & 0x7F)) : std::string()) + (c & 0x80 ? "*" : "") + "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------- other lines in the VPS format
bool vpsDataOf(const uint8_t b[13], VpsData& out, int clockAt)
{
    out = VpsData{};
    uint8_t r[13];
    for (int k = 0; k < 13; k++) r[k] = rev8(b[k]);
    auto bcd = [](uint8_t v, int lo, int hi, int& o) { int a = v >> 4, c = v & 15; if (a > 9 || c > 9) return false; o = a * 10 + c; return o >= lo && o <= hi; };
    for (int o = clockAt >= 0 ? clockAt : 0; o + 6 <= 13 && (clockAt < 0 || o == clockAt); o++) {
        VpsData d;
        if (bcd(r[o], 1, 31, d.day) && bcd(r[o + 1], 1, 12, d.month) && bcd(r[o + 2], 0, 99, d.year) && bcd(r[o + 3], 0, 23, d.hour) &&
            bcd(r[o + 4], 0, 59, d.minute) && bcd(r[o + 5], 0, 59, d.second)) {
            d.clock = true; d.at = o; out = d;
            return true;
        }
    }
    // a text: the longest run of letters / digits / spaces / . - + of 4 and more
    std::string best, cur;
    for (int k = 0; k <= 13; k++) {
        int c = k < 13 ? r[k] & 0x7F : 0;
        bool ok = k < 13 && !(r[k] & 0x80) && (isalnum(c) || c == ' ' || c == '.' || c == '-' || c == '+' || c == '/');
        if (ok) cur += (char)c;
        else { if (cur.size() > best.size()) best = cur; cur.clear(); }
    }
    while (!best.empty() && best.back() == ' ') best.pop_back();
    while (!best.empty() && best.front() == ' ') best.erase(0, 1);
    int letters = 0;
    for (char c : best) letters += isalpha((unsigned char)c) != 0;
    if (letters >= 3 && best.size() >= 4) { out.text = best; return true; }
    return false;
}

std::string vpsDataText(const VpsData& d)
{
    if (d.clock) return fmt("%02d.%02d.%02d %02d:%02d:%02d", d.day, d.month, d.year, d.hour, d.minute, d.second);
    if (!d.text.empty()) return "\"" + d.text + "\"";
    return "";
}

VpsDataFile vpsDataAnalyse(const std::vector<VpsReading>& r, int64_t frames, double fps, int line, int field)
{
    VpsDataFile out;
    if (r.empty()) return out;
    // where the date stands: the place most frames give a whole date and clock at
    int at[13] = {0};
    std::map<std::string, int> texts;
    for (auto& x : r) {
        VpsData d;
        if (!vpsDataOf(x.b, d)) continue;
        if (d.clock) at[d.at]++;
        else texts[d.text]++;
    }
    int ba = 0;
    for (int k = 1; k < 13; k++) if (at[k] > at[ba]) ba = k;
    std::string text;
    int tn = 0;
    for (auto& t : texts) if (t.second > tn) { tn = t.second; text = t.first; }
    out.report.push_back(fmt("Line %d of field %d: a line in the VPS format (biphase 2.5 Mbit/s, the VPS start code) that is no VPS label -", line, field));
    out.report.push_back(fmt("  the station's or the satellite uplink's own data; read in %zu of %lld frames", r.size(), (long long)frames));
    if (at[ba] * 2 >= (int)r.size()) {
        // the clock: readings in order; first / last that the next reading confirms (a second on, or the same)
        std::vector<std::pair<int64_t, VpsData>> c;
        for (auto& x : r) { VpsData d; if (vpsDataOf(x.b, d, ba) && d.clock) c.push_back({x.frame, d}); }
        auto secs = [](const VpsData& d) { return d.hour * 3600 + d.minute * 60 + d.second; };
        auto steady = [&](bool fromEnd) -> int {
            for (size_t k = 0; k + 2 < c.size() && k < 200; k++) {
                size_t i = fromEnd ? c.size() - 1 - k : k, j = fromEnd ? i - 1 : i + 1, j2 = fromEnd ? i - 2 : i + 2;
                int d1 = std::abs(secs(c[j].second) - secs(c[i].second)), d2 = std::abs(secs(c[j2].second) - secs(c[i].second));
                if (d1 <= 1 && d2 <= 1 && c[i].second.day == c[j].second.day && c[i].second.month == c[j].second.month && c[i].second.year == c[j].second.year) return (int)i;
            }
            return -1;
        };
        int a = steady(false), b = steady(true);
        if (a >= 0) {
            const VpsData& f = c[a].second;
            out.date = fmt("%02d.%02d.%s%02d", f.day, f.month, f.year >= 70 ? "19" : "20", f.year);
            out.clockFrom = fmt("%02d:%02d:%02d", f.hour, f.minute, f.second);
            out.kind = "VPS-format clock: " + out.date + " " + out.clockFrom;
            std::string to;
            if (b >= 0) {
                const VpsData& l = c[b].second;
                out.clockTo = fmt("%02d:%02d:%02d", l.hour, l.minute, l.second);
                int s = secs(l) - secs(f);
                if (s < 0) s += 86400;
                out.seconds = s;
                to = " .. " + out.clockTo + fmt(" (%d s of the broadcast in %.1f s of the recording, frames %lld .. %lld)", s, (c[b].first - c[a].first) / fps,
                                                 (long long)c[a].first, (long long)c[b].first);
            }
            out.report.push_back("  Date and clock    " + out.date + "  " + out.clockFrom + to);
            out.report.push_back(fmt("                    BCD in bytes %d-%d (day, month, year, hour, minute, second; bits reversed as VPS sends them), the clock in %zu frames",
                                     ba + 3, ba + 8, c.size()));
        }
    } else if (!text.empty() && tn * 2 >= (int)r.size()) {
        out.kind = "VPS-format text: \"" + text + "\"";
        out.report.push_back("  Text              \"" + text + "\"" + fmt("  (bits reversed as VPS sends them; the same in %d of the frames)", tn));
    }
    if (out.kind.empty()) out.kind = "VPS-format data";
    // the bytes as voted
    uint8_t v[13];
    for (int k = 0; k < 13; k++) {
        int cnt[256] = {0}, bi = 0;
        for (auto& x : r) cnt[x.b[k]]++;
        for (int i = 0; i < 256; i++) if (cnt[i] > cnt[bi]) bi = i;
        v[k] = (uint8_t)bi;
    }
    std::string hex, rv;
    for (int k = 0; k < 13; k++) { hex += fmt("%02X ", v[k]); rv += fmt("%02X ", rev8(v[k])); }
    out.report.push_back("  Bytes 3-15 voted  " + hex + " (as read)");
    out.report.push_back("                    " + rv + " (each byte's bits reversed)");
    return out;
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
