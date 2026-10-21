#include "amol.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>

static float sampleAt(const uint8_t* p, int fmt, int i) { return fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i]; }

bool amolSlice(const uint8_t* line, int fmt, int n, float lineSamples, uint64_t& bits, float& quality)
{
    const float T = lineSamples / 64.0f; // samples per bit
    if (n < (int)(T * (kAmolBits + 4))) return false;
    std::vector<float> s(n);
    for (int i = 0; i < n; i++) s[i] = sampleAt(line, fmt, i);
    std::vector<float> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + n * 3 / 10, sorted.end());
    float lo = sorted[n * 3 / 10];
    std::nth_element(sorted.begin(), sorted.begin() + n * 98 / 100, sorted.end());
    float hi = sorted[n * 98 / 100];
    const float full = fmt == 1 ? 65535.0f : 255.0f;
    if (hi - lo < 0.12f * full) return false;
    float thr = (lo + hi) / 2;
    auto avg = [&](float a, float b) { // mean over [a, b) samples
        int ia = std::max(0, (int)std::lround(a)), ib = std::min(n, (int)std::lround(b));
        if (ib <= ia) return lo;
        float sum = 0;
        for (int i = ia; i < ib; i++) sum += s[i];
        return sum / (ib - ia);
    };
    // the first rise that stays up for most of a bit (colour burst does not)
    int st = -1;
    for (int i = 1; i + (int)T < n; i++)
        if (s[i] > thr && s[i - 1] <= thr && avg(i, i + 0.7f * T) > thr) { st = i; break; }
    if (st < 0 || st + T * kAmolBits > n) return false;
    // refine the clock origin on the preamble edges (bit boundaries 1, 2, 3, 4, 6)
    float x0 = (float)st, shift = 0;
    int edges = 0;
    for (int k : {1, 2, 3, 4, 6}) {
        float c = x0 + k * T;
        for (int i = std::max(1, (int)(c - T / 3)); i < std::min(n, (int)(c + T / 3)); i++)
            if ((s[i - 1] - thr) * (s[i] - thr) < 0) {
                float f = (thr - s[i - 1]) / (s[i] - s[i - 1]);
                shift += (i - 1 + f) - c;
                edges++;
                break;
            }
    }
    if (edges >= 3) x0 += shift / edges;
    // levels from the preamble: ones at bits 0 2 4 5, zeros at 1 3 6
    float h = 0, l = 0;
    for (int k : {0, 2, 4, 5}) h += avg(x0 + (k + 0.25f) * T, x0 + (k + 0.75f) * T);
    for (int k : {1, 3, 6}) l += avg(x0 + (k + 0.25f) * T, x0 + (k + 0.75f) * T);
    h /= 4;
    l /= 3;
    if (h - l < 0.08f * full) return false;
    thr = (h + l) / 2;
    bits = 0;
    quality = 1;
    for (int k = 0; k < kAmolBits; k++) {
        float v = avg(x0 + (k + 0.2f) * T, x0 + (k + 0.8f) * T);
        bits = bits << 1 | (v > thr);
        quality = std::min(quality, std::fabs(v - thr) / (h - l) * 2);
    }
    int pre = (int)(bits >> 41), miss = __builtin_popcount(pre ^ 0x56); // 1010110
    return miss <= 1;
}

bool amolParityOk(uint64_t w) { return __builtin_popcountll(w & ((1ULL << 41) - 1)) & 1; }

std::string AmolStamp::text() const
{
    char b[96];
    snprintf(b, sizeof b, "SID %d  %d/%d  %d:%02d:%02d %s", sid, month, day, hour, minute, second, pm ? "PM" : "AM");
    return b;
}

std::string amolSourceName(int sid)
{
    // seen in the recordings: 37 on NBC 1983 and TNBC 1999, 18 on WCAU (CBS) 1992
    switch (sid) {
    case 18: return "CBS";
    case 37: return "NBC";
    default: return "";
    }
}

static AmolStamp stampOf(uint64_t w, int64_t frame)
{
    AmolStamp t;
    t.frame = frame;
    t.sid = amolField(w, 12, 6);
    t.month = amolField(w, 18, 4);
    t.day = amolField(w, 22, 5);
    t.hour = amolField(w, 27, 4);
    t.minute = amolField(w, 31, 6);
    t.second = amolField(w, 37, 6);
    t.pm = amolField(w, 43, 1);
    t.parityOk = amolParityOk(w);
    t.bits = w;
    return t;
}

static bool plausible(const AmolStamp& t)
{
    return t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour <= 12 && t.minute < 60 && t.second < 60;
}

static int secondsOfDay(const AmolStamp& t) { return ((t.hour % 12) + (t.pm ? 12 : 0)) * 3600 + t.minute * 60 + t.second; }

void amolDecode(const std::vector<AmolWord>& in, AmolStream& s)
{
    std::vector<AmolWord> w(in);
    std::vector<int> fa(w.size());
    s.words = (int64_t)w.size();
    for (size_t i = 0; i < w.size(); i++) fa[i] = amolFrameAddress(w[i].bits);
    // frame addresses count with the frames: a bad one is repaired from both neighbours
    for (size_t i = 1; i + 1 < w.size(); i++) {
        int fromPrev = (int)((fa[i - 1] + (w[i].frame - w[i - 1].frame)) % 30);
        int fromNext = (int)(((fa[i + 1] - (w[i + 1].frame - w[i].frame)) % 30 + 30) % 30);
        if (fromPrev == fromNext && fromPrev != fa[i] && w[i].frame - w[i - 1].frame < 30 && w[i + 1].frame - w[i].frame < 30) {
            fa[i] = fromPrev;
            s.faFixed++;
        }
    }
    // one cycle of 30 frame addresses per second
    std::map<int64_t, std::vector<size_t>> cycles;
    std::map<std::pair<int, uint64_t>, size_t> otherAt;
    for (size_t i = 0; i < w.size(); i++) {
        uint64_t pay = amolPayload(w[i].bits);
        if (!pay) { s.idle++; }
        else if (amolParityOk(w[i].bits)) s.parityOk++;
        else s.parityBad++;
        if (fa[i] >= 30) continue;
        cycles[w[i].frame - fa[i]].push_back(i);
        if (pay && fa[i] != 2 && fa[i] != 3 && amolParityOk(w[i].bits)) {
            auto key = std::make_pair(fa[i], pay);
            auto it = otherAt.find(key);
            if (it == otherAt.end()) {
                otherAt[key] = s.other.size();
                s.other.push_back({fa[i], pay, 1, w[i].frame});
            } else s.other[it->second].count++;
        }
    }
    const uint64_t msgMask = ((1ULL << 35) - 1) << 1;
    for (auto& kv : cycles) {
        // the stamp: any frame address outside the text ones, as sent or inverted;
        // parity-checked, plausible and with zero spare bits first
        AmolStamp best;
        int bestScore = -1;
        std::map<int, std::string> text;   // frame address -> 5 characters
        for (size_t i : kv.second) {
            uint64_t pay = amolPayload(w[i].bits);
            if (!pay) continue;
            if (fa[i] != 2 && fa[i] != 3 && fa[i] != 26 && fa[i] != 27) {
                // text: 5 characters of 7 bits (frame addresses 20-25 on most feeds, 4-15 on NBC 1999)
                std::string t;
                bool ok = true;
                for (int c = 0; c < 5; c++) {
                    int v = amolField(w[i].bits, 12 + 7 * c, 7);
                    if (!isalnum(v) && !(v && strchr(" -./:&'", v))) ok = false, v = '.';   // plain text only (strchr finds the NUL too)
                    t += (char)v;
                }
                int alnum = 0;
                for (char c : t) alnum += isalnum((unsigned char)c) != 0;
                if (ok && alnum >= 3 && !text.count(fa[i])) text[fa[i]] = t;
                continue;
            }
            if (fa[i] != 2 && fa[i] != 3 && fa[i] != 26 && fa[i] != 27) continue; // stamp frame addresses
            for (int inv = 0; inv < 2; inv++) {
                uint64_t word = inv ? w[i].bits ^ msgMask : w[i].bits;
                AmolStamp t = stampOf(word, w[i].frame);
                t.parityOk = amolParityOk(w[i].bits);
                t.bits = w[i].bits;
                t.fa = fa[i];
                t.inverted = inv;
                if (!plausible(t) || t.hour == 0) continue;
                int score = 4 * t.parityOk + 2 * (amolField(word, 44, 3) == 0) + (fa[i] == 2 || fa[i] == 3);
                if (score > bestScore) { bestScore = score; best = t; }
            }
        }
        if (bestScore >= 0) s.stamps.push_back(best);
        std::string line;
        for (auto& t : text)   // each chunk comes twice, in an even and the next odd frame address
            if (!(t.first & 1) || !text.count(t.first - 1) || text[t.first - 1] != t.second) line += (line.empty() ? "" : " ") + t.second;
        if (!line.empty() && (s.texts.empty() || s.texts.back().text != line)) s.texts.push_back({kv.first, line});
    }
    // runs of consecutive seconds
    for (auto& t : s.stamps) {
        if (!s.runs.empty()) {
            AmolRun& r = s.runs.back();
            int dt = secondsOfDay(t) - secondsOfDay(r.last);
            int expect = (int)std::lround((t.frame - r.last.frame) / 29.97);
            if (t.sid == r.last.sid && t.month == r.last.month && t.day == r.last.day && dt >= 1 && dt <= std::max(expect, 1) + 3) { // dropped frames shorten expect
                r.last = t;
                r.seconds++;
                r.bad += !t.parityOk;
                continue;
            }
        }
        AmolRun r;
        r.first = r.last = t;
        r.seconds = 1;
        r.bad = !t.parityOk;
        s.runs.push_back(r);
    }
    // a lone stamp between two runs that join up is a misread: drop runs of 1 inside longer ones
    std::vector<AmolRun> keep;
    for (size_t i = 0; i < s.runs.size(); i++)
        if (s.runs[i].seconds > 1 || s.runs.size() == 1) keep.push_back(s.runs[i]);
    if (!keep.empty()) s.runs = keep;
    std::sort(s.other.begin(), s.other.end(), [](const AmolStream::Other& a, const AmolStream::Other& b) {
        return a.fa != b.fa ? a.fa < b.fa : a.count != b.count ? a.count > b.count : a.firstFrame < b.firstFrame;
    });
}

void amolBuildReport(AmolStream& s)
{
    s.report.clear();
    char b[256];
    snprintf(b, sizeof b, "AMOL I, line %d field %d: %lld words, %lld idle, %lld with data (%lld parity OK, %lld parity errors), %lld frame addresses repaired",
             s.lineNo, s.field, (long long)s.words, (long long)s.idle, (long long)(s.parityOk + s.parityBad),
             (long long)s.parityOk, (long long)s.parityBad, (long long)s.faFixed);
    s.report.push_back(b);
    snprintf(b, sizeof b, "  timestamps: %zu seconds read, %zu continuous runs", s.stamps.size(), s.runs.size());
    s.report.push_back(b);
    for (auto& r : s.runs) {
        std::string src = amolSourceName(r.first.sid);
        snprintf(b, sizeof b, "  SID %d%s  %d/%d  %d:%02d:%02d %s .. %d:%02d:%02d %s  (%d s, %d without parity)", r.first.sid,
                 src.empty() ? "" : (" (" + src + ")").c_str(), r.first.month, r.first.day, r.first.hour, r.first.minute,
                 r.first.second, r.first.pm ? "PM" : "AM", r.last.hour, r.last.minute, r.last.second, r.last.pm ? "PM" : "AM",
                 r.seconds, r.bad);
        s.report.push_back(b);
    }
    if (!s.texts.empty()) {
        snprintf(b, sizeof b, "  text: %zu different lines", s.texts.size());
        s.report.push_back(b);
        for (size_t i = 0; i < s.texts.size() && i < 60; i++) s.report.push_back("    " + s.texts[i].text);
    }
    if (s.other.empty()) return;
    snprintf(b, sizeof b, "  other frame addresses: %zu distinct payloads", s.other.size());
    s.report.push_back(b);
    // a few values per frame address when they keep changing
    const bool many = s.other.size() > 40;
    int shown = 0, fa = -1;
    for (auto& o : s.other) {
        if (o.fa != fa) {
            fa = o.fa;
            shown = 0;
            if (many) {
                int n = 0, words = 0;
                for (auto& x : s.other)
                    if (x.fa == fa) { n++; words += x.count; }
                snprintf(b, sizeof b, "    FA %2d  %d words, %d distinct values, most frequent:", fa, words, n);
                s.report.push_back(b);
            }
        }
        if (many && shown >= 3) continue;
        shown++;
        snprintf(b, sizeof b, "    FA %2d  %09llx  x%d", o.fa, (unsigned long long)o.payload, o.count);
        s.report.push_back(b);
    }
}
