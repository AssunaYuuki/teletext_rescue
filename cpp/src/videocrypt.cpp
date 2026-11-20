#include "videocrypt.h"
#include "ui/util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

const uint8_t kHamming[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
const uint8_t kSeq1[7] = {0x87, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1};
const uint8_t kSeq2[8] = {0x80, 0x91, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7};

struct Tables {
    int fix[256];     // Hamming 8/4 with one bit put right, -1 = not readable
    bool exact[256];  // a codeword as it is (random bytes: 16 of 256; with one bit put right 144 of 256 would pass)
    Tables()
    {
        for (int x = 0; x < 256; x++) {
            int found = -1, n = 0;
            exact[x] = false;
            for (int d = 0; d < 16; d++) {
                int diff = __builtin_popcount(kHamming[d] ^ x);
                if (diff == 0) { found = d; n = 1; exact[x] = true; break; }
                if (diff == 1) { found = d; n++; }
            }
            fix[x] = n == 1 ? found : -1;
        }
    }
};
const Tables &tables() { static const Tables t; return t; }

uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}
uint8_t swapNibbles(uint8_t b) { return (uint8_t)((b & 15) << 4 | b >> 4); }

// the interleaving undone: its 8-byte bit transposes at 0, 6, 12, 20, 26, 32 taken back in the reverse order
void deinterleave(uint8_t *v)
{
    const int offset[6] = {0, 6, 12, 20, 26, 32};
    for (int b = 5; b >= 0; b--) {
        uint8_t *r = v + offset[b], s[8] = {};
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++) s[j] |= (uint8_t)(((r[i] >> j) & 1) << (7 - i));
        s[0] = reverse8(s[0]);
        s[7] = reverse8(s[7]);
        std::copy(s, s + 8, r);
    }
}

// 40 bytes of a frame -> the 20 bytes, when every codeword reads and both sums hold
bool decodeHalf(const uint8_t *raw, uint8_t out[20])
{
    uint8_t v[40];
    std::copy(raw, raw + 40, v);
    deinterleave(v);
    const Tables &t = tables();
    for (int i = 0; i < 20; i++) {
        int hi = t.fix[v[2 * i]], lo = t.fix[v[2 * i + 1]];
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    uint8_t s1 = 0, s2 = 0;
    for (int i = 0; i < 9; i++) { s1 += out[i]; s2 += out[10 + i]; }
    return s1 == out[9] && s2 == out[19];
}

struct Slicer {
    const VbiGeometry &g;
    double bit = 0;   // samples a bit
    std::vector<float> y;
    explicit Slicer(const VbiGeometry &geom) : g(geom) { bit = g.lineSamples * g.lineRate / 14e6 * 18; y.resize(g.validSamples); }
    void load(const uint8_t *p)
    {
        for (int i = 0; i < g.validSamples; i++) y[i] = g.fmt == 1 ? (p[2 * i] | (p[2 * i + 1] << 8)) / 256.0f : (float)p[i];
    }
    // the 5 bytes of a line from bit centres at start + bit / 2 + k * bit, the threshold halfway between the
    // 5th and the 95th percentile of the line; three samples around each centre averaged
    void bytes(double start, uint8_t out[5])
    {
        std::vector<float> s(y);
        size_t k5 = s.size() / 20, k95 = s.size() - 1 - s.size() / 20;
        std::nth_element(s.begin(), s.begin() + k5, s.end());
        float lo = s[k5];
        std::nth_element(s.begin(), s.begin() + k95, s.end());
        float thr = (lo + s[k95]) / 2;
        int d = std::max(1, (int)std::lround(bit / 12));
        for (int q = 0; q < 5; q++) {
            uint8_t b = 0;
            for (int i = 0; i < 8; i++) {
                int c = (int)std::lround(start + bit / 2 + (8 * q + i) * bit);
                float v = 0;
                int n = 0;
                for (int e = -d; e <= d; e += d) { int x = std::clamp(c + e, 0, (int)y.size() - 1); v += y[x]; n++; }
                if (v / n > thr) b |= (uint8_t)(1 << i);
            }
            out[q] = b;
        }
    }
};

}  // namespace

std::vector<VcStream> videocryptPass(VbiFile &vf, const VbiGeometry &g, int64_t frames, const std::function<void(double)> &progress,
                                     const std::atomic<bool> *cancel)
{
    std::vector<VcStream> out;
    if (!g.pal || frames < 20) return out;
    // the stored lines of each system, field 1 then field 2
    auto rowOf = [&](int tv) { for (int l = 0; l < g.linesPerFrame; l++) if (tvLineOf(g, l) == tv) return l; return -1; };
    const int first[2][2] = {{12, 325}, {8, 321}};
    std::vector<VcStream> cand;
    for (int s = 0; s < 2; s++) {
        VcStream v;
        v.system = s + 1;
        bool all = true;
        for (int f = 0; f < 2 && all; f++)
            for (int k = 0; k < 4; k++) { int r = rowOf(first[s][f] + k); if (r < 0) { all = false; break; } v.rows.push_back(r); }
        if (all) cand.push_back(v);
    }
    if (cand.empty()) return out;
    Slicer sl(g);
    const double fs = g.lineSamples * g.lineRate;
    std::vector<uint8_t> buf;
    auto frameBytes = [&](const uint8_t *fr, const VcStream &v, double start, uint8_t raw[40]) {
        for (int k = 0; k < 8; k++) {
            sl.load(fr + (size_t)v.rows[k] * g.lineBytes);
            sl.bytes(start, raw + 5 * k);
        }
    };
    // where the first bit starts: the place that makes the most codewords after de-interleaving, over 24 frames
    // spread through the recording (a few microseconds into the stored line on every card met so far)
    for (auto &v : cand) {
        double bestStart = -1;
        int bestScore = 0;
        const int probes = 24;
        std::vector<std::vector<uint8_t>> pf;
        for (int i = 0; i < probes; i++) {
            if (vf.read(frames * (i + 1) / (probes + 1), 1, buf) < 1) continue;
            pf.push_back(buf);
        }
        const Tables &t = tables();
        // up to 6 us into the stored line: a 1440-sample line (SAA713x, 53 us) holds the 40 bits (51.4 us) only from its start, and
        // the last bit may reach its end (its samples are clamped there). The bit width too, within 2%: a card's clock is not
        // the nominal one, and over 40 bits 0.2% puts the last bits a sample or two off (111.vbi: 34.65 samples, not 34.71)
        const double nominal = sl.bit;
        double bestBit = nominal;
        for (double bw = nominal * 0.98; bw <= nominal * 1.0201; bw += nominal * 0.002) {
            sl.bit = bw;
            for (double st = 0; st < std::min<double>(g.validSamples - 39.5 * bw, 6e-6 * fs); st += std::max(1.0, bw / 24)) {
                int score = 0;
                for (auto &fr : pf) {
                    uint8_t raw[40];
                    frameBytes(fr.data(), v, st, raw);
                    deinterleave(raw);
                    for (int i = 0; i < 40; i++) score += t.exact[raw[i]];
                }
                if (score > bestScore) { bestScore = score; bestStart = st; bestBit = bw; }
            }
        }
        sl.bit = bestBit;
        // random bytes are an exact codeword one time in 16 (6%); the lines of 111.vbi give 70-75%, a poor tape less:
        // a quarter is no chance
        if (getenv("TR_DEBUG"))
            fprintf(stderr, "videocrypt %d: rows %d..%d, best start %.1f, bit %.2f samples, exact codewords %d of %d\n", v.system, v.rows.front(),
                    v.rows.back(), bestStart, bestBit, bestScore, (int)pf.size() * 40);
        if (bestStart < 0 || bestScore * 4 < (int)pf.size() * 40) { sl.bit = nominal; continue; }
        // every frame, at that place and a sample or two either side
        std::map<int, std::pair<int64_t, std::array<uint8_t, 20>>> firstHalf;   // header -> frame, bytes
        std::set<std::vector<uint8_t>> seen;
        const double steps[9] = {0, -1, 1, -2, 2, -3, 3, -4, 4};
        for (int64_t f = 0; f < frames; f++) {
            if (cancel && *cancel) return out;
            if (progress && f % 500 == 0) progress((double)f / frames);
            if (vf.read(f, 1, buf) < 1) break;
            v.frames++;
            uint8_t half[20];
            bool ok = false;
            for (double d : steps) {
                uint8_t raw[40];
                frameBytes(buf.data(), v, bestStart + d * std::max(1.0, sl.bit / 34), raw);
                if (decodeHalf(raw, half)) { ok = true; break; }
            }
            if (!ok) continue;
            v.halves++;
            int a = half[0];
            const uint8_t *seq = v.system == 1 ? kSeq1 : kSeq2;
            int nseq = v.system == 1 ? 7 : 8;
            bool isFirst = std::find(seq, seq + nseq, a) != seq + nseq;
            bool isSecond = std::find(seq, seq + nseq, swapNibbles((uint8_t)a)) != seq + nseq;
            std::array<uint8_t, 20> h;
            std::copy(half, half + 20, h.begin());
            if (isFirst) { firstHalf[a] = {f, h}; continue; }
            if (!isSecond) continue;
            auto it = firstHalf.find(swapNibbles((uint8_t)a));
            if (it == firstHalf.end() || f - it->second.first > 8) continue;
            VcMessage m;
            m.frame = it->second.first;
            m.sequence = it->first;
            m.counter = it->second.second[10];
            m.mode = half[10];
            for (int i = 0; i < 8; i++) {
                m.bytes[i] = it->second.second[1 + i];
                m.bytes[8 + i] = it->second.second[11 + i];
                m.bytes[16 + i] = half[1 + i];
                m.bytes[24 + i] = half[11 + i];
            }
            uint8_t sum = 0;
            for (uint8_t b : m.bytes) sum += b;
            if (sum) { v.badMessages++; continue; }
            v.messages++;
            v.kinds[m.bytes[0]]++;
            if (seen.insert(std::vector<uint8_t>(m.bytes, m.bytes + 32)).second) v.distinct++;
            v.list.push_back(m);
            firstHalf.erase(it);
        }
        if (getenv("TR_DEBUG"))
            fprintf(stderr, "videocrypt %d: %lld frames, %lld halves, %lld messages, %lld with a wrong sum\n", v.system, (long long)v.frames,
                    (long long)v.halves, (long long)v.messages, (long long)v.badMessages);
        // a few messages by chance cannot pass both their half sums and their message sum: 3 are taken as the service
        sl.bit = nominal;   // the next system finds its own
        if (v.messages >= 3) out.push_back(std::move(v));
    }
    return out;
}

std::vector<std::string> videocryptReport(const std::vector<VcStream> &s)
{
    std::vector<std::string> r;
    if (s.empty()) return r;
    std::string head = "VideoCrypt (scrambling system data):";
    for (auto &v : s) head += fmt(" %s %lld messages;", v.system == 1 ? "VideoCrypt I" : "VideoCrypt II", (long long)v.messages);
    head.pop_back();
    r.push_back(head);
    for (auto &v : s) {
        std::string lines = v.system == 1 ? "12-15 / 325-328" : "8-11 / 321-324";
        r.push_back(fmt("  %s, lines %s: 40 bits a line (1.286 us a bit, LSB first), Hamming 8/4, interleaved",
                        v.system == 1 ? "VideoCrypt I" : "VideoCrypt II", lines.c_str()));
        r.push_back(fmt("    %lld of %lld frames read with both sums right (%.0f%%); %lld whole messages of 32 bytes with their sum right, %d different"
                        "%s",
                        (long long)v.halves, (long long)v.frames, v.frames ? 100.0 * v.halves / v.frames : 0.0, (long long)v.messages, v.distinct,
                        v.badMessages ? fmt(", %lld put together with a wrong sum", (long long)v.badMessages).c_str() : ""));
        std::string k = "    first byte of the messages:";
        std::vector<std::pair<int, int>> kv(v.kinds.begin(), v.kinds.end());
        std::sort(kv.begin(), kv.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < kv.size() && i < 10; i++) k += fmt(" %02X x%d", kv[i].first, kv[i].second);
        r.push_back(k);
    }
    r.push_back("  The messages are the scrambling system's own (control and entitlement messages for the decoder and its card);"
                " they are listed in videocrypt.txt. Nothing of the picture is recovered from them.");
    return r;
}

std::string videocryptMessagesText(const std::vector<VcStream> &s)
{
    std::string t;
    for (auto &l : videocryptReport(s)) t += l + "\r\n";
    for (auto &v : s) {
        t += fmt("\r\n%s: frame, header, counter, mode, the 32 bytes of the message\r\n", v.system == 1 ? "VideoCrypt I" : "VideoCrypt II");
        for (auto &m : v.list) {
            t += fmt("%6lld  %02X  %02X  %02X  ", (long long)m.frame, m.sequence, m.counter, m.mode);
            for (uint8_t b : m.bytes) t += fmt("%02X ", b);
            t += "\r\n";
        }
    }
    return t;
}
