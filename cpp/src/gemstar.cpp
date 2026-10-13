#include "gemstar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

namespace {
struct Line {
    std::vector<float> s;
    float lo = 0, hi = 0, thr = 0;
    bool load(const uint8_t* line, int fmt, int n)
    {
        s.resize(n);
        for (int i = 0; i < n; i++) s[i] = sampleAt(line, fmt, i);
        std::vector<float> sorted(s);
        std::nth_element(sorted.begin(), sorted.begin() + n * 3 / 10, sorted.end());
        lo = sorted[n * 3 / 10];
        std::nth_element(sorted.begin(), sorted.begin() + n * 98 / 100, sorted.end());
        hi = sorted[n * 98 / 100];
        thr = (lo + hi) / 2;
        return hi - lo >= 0.12f * (fmt == 1 ? 65535.0f : 255.0f);
    }
    float avg(float a, float b) const // mean over [a, b) samples
    {
        int n = (int)s.size();
        int ia = std::max(0, (int)std::lround(a)), ib = std::min(n, (int)std::lround(b));
        if (ib <= ia) return lo;
        float sum = 0;
        for (int i = ia; i < ib; i++) sum += s[i];
        return sum / (ib - ia);
    }
};
}

bool gemstarSlice2x(const uint8_t* line, int fmt, int n, float lineSamples, uint8_t out[4], float& quality)
{
    static const int FC[11] = {1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 1};
    const float T = lineSamples / 64.0f; // samples per bit
    if (n < (int)(T * 50)) return false;
    Line L;
    if (!L.load(line, fmt, n)) return false;
    // the frame code at the clock origin with the widest margin
    float best = 0, x0 = -1;
    for (float s = 0; s + 43 * T <= n; s += T / 8) {
        float m = 1e9f;
        for (int k = 0; k < 11 && m > best; k++) {
            float v = L.avg(s + (k + 0.25f) * T, s + (k + 0.75f) * T) - L.thr;
            m = std::min(m, FC[k] ? v : -v);
        }
        if (m > best) best = m, x0 = s;
    }
    if (x0 < 0 || best < 0.15f * (L.hi - L.lo)) return false;
    // levels from the frame code
    float h = 0, l = 0;
    for (int k = 0; k < 11; k++) (FC[k] ? h : l) += L.avg(x0 + (k + 0.25f) * T, x0 + (k + 0.75f) * T);
    h /= 7;
    l /= 4;
    float thr = (h + l) / 2;
    quality = 1;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (int k = 0; k < 32; k++) {
        float v = L.avg(x0 + (k + 11.2f) * T, x0 + (k + 11.8f) * T);
        if (v > thr) out[k / 8] |= 1 << (k % 8); // LSB first
        quality = std::min(quality, std::fabs(v - thr) / (h - l) * 2);
    }
    return true;
}

bool gemstarSlice1x(const uint8_t* line, int fmt, int n, float lineSamples, uint8_t out[2], bool& parityOk)
{
    const float T = lineSamples / 32.0f;
    if (n < (int)(T * 26)) return false;
    Line L;
    if (!L.load(line, fmt, n)) return false;
    std::vector<float> rise; // rising edges, sub-sample
    for (int i = 1; i < n; i++)
        if (L.s[i - 1] <= L.thr && L.s[i] > L.thr) rise.push_back(i - 1 + (L.thr - L.s[i - 1]) / (L.s[i] - L.s[i - 1]));
    // >= 6 run-in edges one bit apart, then the start bit edge 2.5 bits after the last
    int run = 0;
    for (size_t k = 1; k < rise.size(); k++) {
        float d = rise[k] - rise[k - 1];
        if (std::fabs(d - T) < 0.2f * T) { run++; continue; }
        if (run >= 5 && std::fabs(d - 2.5f * T) < 0.35f * T) {
            float x0 = rise[k]; // leading edge of the start bit
            if (x0 + 17 * T > n) return false;
            uint8_t b[2] = {0, 0};
            for (int j = 0; j < 16; j++) {
                float c = x0 + (j + 1.5f) * T;
                if (L.avg(c - 0.25f * T, c + 0.25f * T) > L.thr) b[j / 8] |= 1 << (j % 8);
            }
            out[0] = b[0], out[1] = b[1];
            parityOk = (__builtin_popcount(b[0]) & 1) && (__builtin_popcount(b[1]) & 1);
            return true;
        }
        run = 0;
    }
    return false;
}

std::string GemstarWord::hex() const
{
    char t[16];
    if (x2) snprintf(t, sizeof t, "%02X%02X%02X%02X", b[0], b[1], b[2], b[3]);
    else snprintf(t, sizeof t, "%02X%02X", b[0], b[1]);
    return t;
}

std::string GemstarMessage::hex() const
{
    std::string s;
    for (auto& w : words) s += (s.empty() ? "" : " ") + w;
    return s;
}

GemstarClock gemstarClock(const GemstarPacket& p)
{
    GemstarClock c;
    if (p.type != 0x7A || p.data.size() < 7) return c;
    c.minute = p.data[0] & 0x3F;
    c.hour = p.data[1] & 0x1F;
    c.day = p.data[2] & 0x1F;
    c.month = p.data[3] & 0x0F;
    c.weekday = p.data[4] & 0x07;
    c.year = 1990 + (p.data[5] & 0x3F);   // FREQCCD_ 1bdb:02fc: (b & 3F) + 1990
    c.zone = p.data.size() > 6 ? p.data[6] & 0x1F : 0;
    c.frame = p.frame;
    c.valid = c.minute < 60 && c.hour < 24 && c.day >= 1 && c.day <= 31 && c.month >= 1 && c.month <= 12;
    return c;
}

std::string GemstarPacket::text() const
{
    char b[160];
    GemstarClock c = gemstarClock(*this);
    if (c.valid) {
        static const char* WD[] = {"?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(b, sizeof b, "clock: %s %d %s %d %d:%02d (zone %d)", WD[c.weekday], c.day, MO[c.month - 1], c.year, c.hour, c.minute, c.zone);
        return b;
    }
    snprintf(b, sizeof b, "type %02X: ", type);
    std::string s = b;
    for (uint8_t v : data) s += v >= 0x20 && v < 0x7F ? (char)v : '.';
    s += "  [";
    for (size_t i = 0; i < data.size(); i++) snprintf(b, sizeof b, "%s%02X", i ? " " : "", data[i]), s += b;
    return s + "]";
}

static bool oddParity(uint8_t v) { return __builtin_popcount(v) & 1; }

void gemstarDecode(GemstarStream& s)
{
    // 2x fill words: the ones sent far more often than anything else
    std::map<std::string, int> count;
    for (auto& w : s.words) {
        if (w.x2) s.words2x++, count[w.hex()]++;
        else s.words1x++, s.parityBad1x += !w.ok, s.null1x += (w.b[0] == 0x80 && w.b[1] == 0x80);
    }
    for (auto& kv : count)
        if (kv.second >= 20 && kv.second * 10 >= s.words2x) s.idleWords.push_back(kv.first), s.idle2x += kv.second;
    auto idle = [&](const GemstarWord& w) {
        if (!w.x2) return w.b[0] == 0x80 && w.b[1] == 0x80;
        return std::find(s.idleWords.begin(), s.idleWords.end(), w.hex()) != s.idleWords.end();
    };
    std::map<std::string, size_t> seenPk;
    std::vector<bool> blockSeen(256, false);
    // packets and download blocks inside one run of words (bytes; -1 = missed word)
    auto parse = [&](const std::vector<int>& b, int64_t frame, bool x2) {
        size_t n = b.size();
        for (size_t i = 0; i + 3 < n; i++) {
            if (x2 && i % 4 == 0 && b[i] == 0x05 && b[i + 1] == 0xFC && b[i + 2] == 0x4D && b[i + 3] == 0x61 && i + 4 < n && b[i + 4] >= 0) {
                s.blocks++;
                blockSeen[b[i + 4]] = true;
                continue;
            }
            if (b[i] != 0x07) continue;
            int sum = 7;
            for (size_t j = i + 1; j < n && j < i + 64; j++) {
                if (b[j] < 0 || !oddParity((uint8_t)b[j])) break;
                sum = (sum + (b[j] & 0x7F)) & 0x7F;
                if (sum || j + 1 >= n || b[j + 1] != 0x8F) continue;
                GemstarPacket p;
                p.frame = frame;
                p.x2 = x2;
                p.type = b[i + 1] & 0x7F;
                for (size_t k = i + 2; k < j; k++)
                    if (b[k] != 0x80 || x2) p.data.push_back(b[k] & 0x7F);
                int x = 0;
                for (size_t k = i; k <= j; k++) p.raw.push_back(b[k] & 0x7F), x ^= b[k];
                if (j + 2 < n && b[j + 2] >= 0) p.lrc = ((x ^ 0x8F) == b[j + 2]);
                if (!p.data.empty() && j > i + 1) {
                    std::string key = std::string(x2 ? "2" : "1") + (char)p.type + std::string(p.data.begin(), p.data.end());
                    auto it = seenPk.find(key);
                    if (it != seenPk.end()) s.packets[it->second].copies++;
                    else seenPk[key] = s.packets.size(), s.packets.push_back(p);
                }
                i = j + 1;
                break;
            }
        }
    };
    // messages: other words in consecutive frames; one missed frame is kept as a gap
    std::map<std::string, size_t> seen;
    GemstarMessage cur;
    std::vector<int> bytes;
    int64_t last = -10;
    auto flush = [&]() {
        if (cur.words.empty()) return;
        parse(bytes, cur.frame, cur.x2);
        std::string k = (cur.x2 ? "2:" : "1:") + cur.hex();
        auto it = seen.find(k);
        if (it != seen.end()) s.messages[it->second].copies++;
        else seen[k] = s.messages.size(), s.messages.push_back(cur);
        cur = GemstarMessage();
        bytes.clear();
    };
    for (auto& w : s.words) {
        if (idle(w)) { flush(); last = -10; continue; }
        if (!cur.words.empty() && (cur.x2 != w.x2 || w.frame - last > 2)) flush();
        int nb = w.x2 ? 4 : 2;
        if (cur.words.empty()) cur.frame = w.frame, cur.x2 = w.x2;
        else if (w.frame - last == 2) {
            cur.words.push_back(w.x2 ? "????????" : "????");
            bytes.insert(bytes.end(), nb, -1);
        }
        cur.words.push_back(w.hex() + (w.ok ? "" : "!"));
        for (int i = 0; i < nb; i++) bytes.push_back(w.b[i]);
        last = w.frame;
    }
    flush();
    for (int i = 0; i < 256; i++)
        if (blockSeen[i]) s.blockNumbers.push_back(i);
}

void gemstarBuildReport(GemstarStream& s)
{
    char b[256];
    s.report.clear();
    snprintf(b, sizeof b, "Gemstar (VCR Plus+ / Guide Plus+ data), line %d field %d:", s.lineNo, s.field);
    s.report.push_back(b);
    if (s.words2x) {
        std::string idle;
        for (auto& w : s.idleWords) idle += (idle.empty() ? "" : ", ") + w;
        snprintf(b, sizeof b, "  2x (1.007 Mbit/s, frame code 10011101101, 4 bytes): %lld words, %lld idle (%s)", (long long)s.words2x,
                 (long long)s.idle2x, idle.empty() ? "none" : idle.c_str());
        s.report.push_back(b);
    }
    if (s.words1x) {
        snprintf(b, sizeof b, "  1x (caption framing, 2 bytes): %lld pairs, %lld null, %lld parity errors", (long long)s.words1x,
                 (long long)s.null1x, (long long)s.parityBad1x);
        s.report.push_back(b);
    }
    int n2 = 0, n1 = 0;
    for (auto& m : s.messages) (m.x2 ? n2 : n1)++;
    snprintf(b, sizeof b, "  runs of data: %d in 2x, %d in 1x; packets with a good checksum: %zu", n2, n1, s.packets.size());
    s.report.push_back(b);
    if (s.blocks) {
        std::string nums;
        for (int v : s.blockNumbers) nums += (nums.empty() ? "" : " ") + std::to_string(v);
        snprintf(b, sizeof b, "  Type B blocks (05 FC, 80 bytes): %lld, numbers %s", (long long)s.blocks, nums.c_str());
        s.report.push_back(b);
    }
    for (auto& l : s.guide) s.report.push_back(l);
    if (!s.guide.empty()) return;
    for (auto& p : s.packets) {
        snprintf(b, sizeof b, "  packet %s%s", p.x2 ? "2x " : "1x ", p.text().c_str());
        s.report.push_back(b + (p.copies > 1 ? "  (x" + std::to_string(p.copies) + ")" : std::string()));
    }
}

