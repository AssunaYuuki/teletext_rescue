#include "fastdata.h"
#include "fastdata_core.h"
#include <algorithm>
#include <array>
#include <set>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const uint8_t kHam[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
                          0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};

// last run-in byte (10101010 on the wire) + framing code, LSB first
struct SyncPattern {
    char kind;
    uint8_t framing;
};
const SyncPattern kSync[] = {{'N', 0xE7}, {'W', 0x27}, {'X', 0x2D}};

void gaussH(float* H, float sigma, float spb)
{
    for (int o = 0; o < 2; o++) {
        float sum = 0;
        for (int j = 0; j < 7; j++) {
            float t = (j - 3) * spb - o * 0.5f * spb;
            H[j * 2 + o] = std::exp(-t * t / (2 * sigma * sigma));
            sum += H[j * 2 + o];
        }
        for (int j = 0; j < 7; j++) H[j * 2 + o] /= sum;
        H[14 + o] = 0;
    }
}

struct Line {
    const uint8_t* p;
    float info[4];
};

// Least squares refit of H from decided bits (decision directed).
bool refit(const std::vector<Line>& ls, const std::vector<std::vector<uint8_t>>& bits, const std::vector<int>& use,
           const VbiGeometry& g, float spb, float phi, float* H)
{
    double A[2][8][9] = {};
    int rows = 0;
    for (int idx : use) {
        const Line& l = ls[idx];
        const uint8_t* b = bits[idx].data();
        float st = l.info[0] - 24.0f * spb + phi;
        for (int k = 6; k < kFastBits; k++) {
            float c = st + (k - 3) * spb;
            float y[2] = {fd_at(l.p, g.fmt, g.validSamples, c, l.info[1], l.info[2]),
                          fd_at(l.p, g.fmt, g.validSamples, c + 0.5f * spb, l.info[1], l.info[2])};
            double x[8];
            for (int j = 0; j < 7; j++) x[j] = b[k - 6 + j] ? 1.0 : -1.0;
            x[7] = 1.0;
            for (int o = 0; o < 2; o++)
                for (int r = 0; r < 8; r++) {
                    for (int c2 = 0; c2 < 8; c2++) A[o][r][c2] += x[r] * x[c2];
                    A[o][r][8] += x[r] * y[o];
                }
            rows++;
        }
    }
    if (rows < 200) return false;
    for (int o = 0; o < 2; o++) {
        double (*M)[9] = A[o];
        for (int r = 0; r < 8; r++) M[r][r] += 1e-3 * rows;
        for (int c = 0; c < 8; c++) {
            int piv = c;
            for (int r = c + 1; r < 8; r++)
                if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
            for (int k = 0; k < 9; k++) std::swap(M[c][k], M[piv][k]);
            if (std::fabs(M[c][c]) < 1e-12) return false;
            for (int r = 0; r < 8; r++) {
                if (r == c) continue;
                double f = M[r][c] / M[c][c];
                for (int k = c; k < 9; k++) M[r][k] -= f * M[c][k];
            }
        }
        for (int j = 0; j < 7; j++) H[j * 2 + o] = (float)(M[j][8] / M[j][j]);
        H[14 + o] = (float)(M[7][8] / M[7][7]);
    }
    return true;
}

} // namespace

int hamming84(uint8_t b)
{
    for (int i = 0; i < 16; i++)
        if (kHam[i] == b) return i;
    for (int i = 0; i < 16; i++)
        if (__builtin_popcount(kHam[i] ^ b) == 1) return i;
    return -1;
}

bool fdFindSync(const uint8_t* bits, int nb, char& kind, int& pos, int& err)
{
    err = 99;
    for (int p = 0; p + 40 < nb && p < 72; p++)
        for (auto& sp : kSync) {
            int e = 0;
            for (int i = 0; i < 8; i++) e += bits[p + 8 + i] != (i % 2 == 0);
            for (int i = 0; i < 8 && e <= err; i++) e += bits[p + 16 + i] != ((sp.framing >> i) & 1);
            if (e < err) { err = e; pos = p; kind = sp.kind; }
        }
    return err <= 1;
}

FastPacket fdMakePacket(const uint8_t* bits, int nb, char kind, int pos, int err)
{
    FastPacket pk;
    pk.kind = kind;
    pk.syncErr = err;
    for (int b = pos; b + 8 <= nb && pk.bytes.size() < 45; b += 8) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= kind == 'X' ? bits[b + i] << (7 - i) : bits[b + i] << i;
        pk.bytes.push_back(v);
    }
    return pk;
}

FastCalib fdCalibrate(const std::vector<const uint8_t*>& raw, const VbiGeometry& g, double forceRate)
{
    FastCalib best;
    std::vector<Line> ls;
    for (auto p : raw) {
        Line l{p, {}};
        fd_prepare(p, g.fmt, g.validSamples, g.lineSamples, l.info);
        if (l.info[3] > 0) ls.push_back(l);
    }
    if (ls.size() < 20) return best;
    const float scale = g.lineSamples / 910.0f;
    std::vector<uint8_t> scratch(kFastBits * 64);
    std::vector<std::vector<uint8_t>> bits(ls.size(), std::vector<uint8_t>(kFastBits));

    auto evaluate = [&](const float* H, float spb, float phi, size_t limit, std::vector<int>& good, int kinds[3]) {
        good.clear();
        kinds[0] = kinds[1] = kinds[2] = 0;
        for (size_t i = 0; i < ls.size() && i < limit; i++) {
            fd_viterbi(ls[i].p, g.fmt, g.validSamples, spb, phi, H, ls[i].info, kFastBits, scratch.data(),
                       bits[i].data());
            char kind;
            int pos, err;
            if (fdFindSync(bits[i].data(), kFastBits, kind, pos, err)) {
                good.push_back((int)i);
                kinds[kind == 'N' ? 0 : kind == 'W' ? 1 : 2]++;
            }
        }
    };

    // coarse search over pulse width and sampling phase; the bit rate is
    // NABTS/WST standard 364 x fH, other rates are tried if that fails
    std::vector<int> good;
    int kinds[3];
    size_t coarseN = std::min<size_t>(ls.size(), 60);
    float bestH[16];
    float bestPhi = 0, bestSpb = 0;
    double bestRate = 364;
    size_t bestGood = 0;
    auto search = [&](double rate) {
        float spb = (float)(g.lineSamples / rate);
        for (float sigma : {1.25f, 1.75f, 2.25f, 2.75f})
            for (float ph : {0.0f, 0.25f, 0.5f, 0.75f}) {
                float H[16];
                gaussH(H, sigma * scale, spb);
                evaluate(H, spb, ph * spb, coarseN, good, kinds);
                if (good.size() > bestGood) {
                    bestGood = good.size();
                    memcpy(bestH, H, sizeof bestH);
                    bestPhi = ph * spb;
                    bestSpb = spb;
                    bestRate = rate;
                }
            }
    };
    const double nominal = g.fastRate;
    if (forceRate > 0) search(forceRate);
    else search(nominal);

    if (bestGood < 3) return best;

    // decision-directed refinement on every calibration line
    float H[16];
    memcpy(H, bestH, sizeof H);
    for (int it = 0; it < 4; it++) {
        evaluate(H, bestSpb, bestPhi, ls.size(), good, kinds);
        if (good.size() < 3 || !refit(ls, bits, good, g, bestSpb, bestPhi, H)) break;
    }
    evaluate(H, bestSpb, bestPhi, ls.size(), good, kinds);
    best.ok = !good.empty();
    memcpy(best.H, H, sizeof H);
    best.phi = bestPhi;
    best.spb = bestSpb;
    best.rate = bestRate;
    best.syncRate = (double)good.size() / ls.size();
    int k = std::max_element(kinds, kinds + 3) - kinds;
    best.kind = "NWX"[k];
    if (best.kind == 'N') {
        int ok = 0, tot = 0;
        for (int i : good) {
            char kind;
            int pos, err;
            fdFindSync(bits[i].data(), kFastBits, kind, pos, err);
            FastPacket pk = fdMakePacket(bits[i].data(), kFastBits, kind, pos, err);
            for (int b = 3; b < 8 && b < (int)pk.bytes.size(); b++, tot++) ok += hamming84(pk.bytes[b]) >= 0;
        }
        best.headerOk = tot ? (double)ok / tot : 0;
    }
    return best;
}

// --------------------------------------------------------------- report ---

static bool printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

static void textRuns(const std::vector<uint8_t>& d, size_t from, int minLen, std::vector<std::string>& out,
                     const std::string& tag)
{
    std::string cur;
    auto flush = [&]() {
        int letters = 0;
        for (char c : cur) letters += isalpha((unsigned char)c) != 0;
        if ((int)cur.size() >= minLen && letters * 2 >= (int)cur.size()) out.push_back(tag + cur);
        cur.clear();
    };
    for (size_t i = from; i < d.size(); i++) {
        if (printable(d[i])) cur += (char)d[i];
        else flush();
    }
    flush();
}

FastReport fdReport(const std::vector<FastPacket>& pk)
{
    FastReport r;
    char b[512];
    std::map<char, int> kinds;
    for (auto& p : pk) kinds[p.kind]++;
    snprintf(b, sizeof b, "5.7 Mbit/s packets: %zu (NABTS %d, WST %d, framing-2D %d, NABTS sync without NABTS data %d)", pk.size(),
             kinds['N'], kinds['W'], kinds['X'], kinds['E']);
    r.lines.push_back(b);

    // NABTS: header = 3 address nibbles, continuity index, packet structure
    struct Addr {
        int count = 0, ciBreaks = 0, lastCi = -1, printableBytes = 0, dataBytes = 0;
        std::map<int, int> ps;
    };
    std::map<int, Addr> addrs;
    int hdrOk = 0, hdrTot = 0;
    for (auto& p : pk) {
        if (p.kind != 'N' || p.bytes.size() < 36) continue;
        int n[5];
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            n[i] = hamming84(p.bytes[3 + i]);
            ok &= n[i] >= 0;
            hdrOk += n[i] >= 0;
            hdrTot++;
        }
        if (!ok) continue;
        int addr = n[0] << 8 | n[1] << 4 | n[2];
        Addr& a = addrs[addr];
        a.count++;
        if (a.lastCi >= 0 && n[3] != ((a.lastCi + 1) & 15)) a.ciBreaks++;
        a.lastCi = n[3];
        a.ps[n[4]]++;
        for (int i = 8; i < 36; i++) {
            a.dataBytes++;
            a.printableBytes += printable(p.bytes[i] & 0x7F);
        }
    }
    // Random 7-bit data is 74% "printable" (95 of 128 codes), so only
    // addresses well above that are treated as text.
    for (auto& p : pk) {
        if (p.kind != 'N' || p.bytes.size() < 36) continue;
        int n0 = hamming84(p.bytes[3]), n1 = hamming84(p.bytes[4]), n2 = hamming84(p.bytes[5]);
        if (n0 < 0 || n1 < 0 || n2 < 0) continue;
        int addr = n0 << 8 | n1 << 4 | n2;
        auto it = addrs.find(addr);
        if (it == addrs.end() || it->second.printableBytes * 10 < it->second.dataBytes * 9) continue;
        std::vector<uint8_t> d(p.bytes.begin() + 8, p.bytes.end());
        for (auto& c : d) c &= 0x7F;
        char tag[32];
        snprintf(tag, sizeof tag, "[NABTS %03X] ", addr);
        textRuns(d, 0, 12, r.text, tag);
    }
    if (hdrTot) {
        snprintf(b, sizeof b, "NABTS headers: %.1f%% of Hamming bytes valid; %zu packet addresses", 100.0 * hdrOk / hdrTot,
                 addrs.size());
        r.lines.push_back(b);
        std::vector<std::pair<int, int>> top;
        for (auto& kv : addrs) top.push_back({kv.second.count, kv.first});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 24; i++) {
            const Addr& a = addrs[top[i].second];
            std::string ps;
            for (auto& kv : a.ps) ps += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
            snprintf(b, sizeof b, "  address %03X: %5d packets, continuity breaks %d, PS%s, printable %.0f%%",
                     top[i].second, a.count, a.ciBreaks, ps.c_str(), 100.0 * a.printableBytes / std::max(1, a.dataBytes));
            r.lines.push_back(b);
        }
    }

    // WST teletext: magazine/row from MRAG, row 0 is the page header
    int wst = 0;
    for (auto& p : pk) {
        if (p.kind != 'W' || p.bytes.size() < 37) continue;
        size_t rowEnd = p.bytes.size() >= 45 ? 45 : 37;
        int a = hamming84(p.bytes[3]), c = hamming84(p.bytes[4]);
        if (a < 0 || c < 0) continue;
        int mag = a & 7, row = (a >> 3) | (c << 1);
        std::string t;
        for (size_t i = 5; i < rowEnd; i++) {
            uint8_t ch = p.bytes[i] & 0x7F;
            t += printable(ch) ? (char)ch : ' ';
        }
        snprintf(b, sizeof b, "[WST M%d R%02d] ", mag ? mag : 8, row);
        r.text.push_back(b + t);
        wst++;
    }

    // framing 2D service: AA AA B4 <service> <sequence> + 32 payload bytes,
    // MSB first. Service 74 carries plain ASCII text that runs on from one
    // packet to the next; the other services are binary.
    int xpk = 0, xprint = 0, xbytes = 0;
    std::map<int, std::vector<const FastPacket*>> svc; // service byte -> packets in time order
    // the service byte itself takes bit errors: snap it to the dominant
    // values of its line (within 2 bits)
    std::map<int, std::map<int, int>> perLine;
    std::map<int, int> lineTotal;
    for (auto& p : pk)
        if (p.kind == 'X' && p.bytes.size() >= 37) { perLine[p.lineNo][p.bytes[3]]++; lineTotal[p.lineNo]++; }
    for (auto& p : pk) {
        if (p.kind != 'X' || p.bytes.size() < 37) continue;
        xpk++;
        for (size_t i = 5; i < p.bytes.size(); i++, xbytes++) xprint += printable(p.bytes[i]);
        int best = -1, bd = 3;
        for (auto& kv : perLine[p.lineNo]) {
            if (kv.second * 100 < lineTotal[p.lineNo] * 3) continue;
            int d = __builtin_popcount(kv.first ^ p.bytes[3]);
            if (d < bd) { bd = d; best = kv.first; }
        }
        if (best >= 0 && p.crc != 3) svc[best].push_back(&p); // CRC failures stay out of the text
    }
    for (auto& kv : svc) {
        auto& v = kv.second;
        // mostly CRC-checked service: unchecked packets would only add noise
        size_t checked = 0;
        for (auto* q : v) checked += q->crc >= 0 && q->crc <= 2;
        if (checked * 2 >= v.size())
            v.erase(std::remove_if(v.begin(), v.end(), [](const FastPacket* q) { return q->crc < 0 || q->crc > 2; }), v.end());
        if (v.size() < 50) continue;
        size_t textual = 0;
        for (auto* q : v) {
            int pr = 0;
            for (int i = 5; i < 32; i++) pr += printable(q->bytes[i]);
            textual += pr > 17;
        }
        snprintf(b, sizeof b, "  service %02X: %zu packets on line %d, %zu mostly text", kv.first, v.size(), v[0]->lineNo,
                 textual);
        r.lines.push_back(b);
        if (textual * 20 < v.size()) continue;

        // Carousel repeats: vote every bit of a packet with its near copies.
        const size_t n = v.size();
        std::vector<std::array<uint64_t, 4>> bits(n);
        for (size_t i = 0; i < n; i++) {
            bits[i].fill(0);
            for (int k = 0; k < 27; k++) bits[i][k / 8] |= (uint64_t)v[i]->bytes[5 + k] << ((k % 8) * 8);
        }
        std::vector<std::vector<uint8_t>> voted(n);
        int repaired = 0;
        for (size_t i = 0; i < n; i++) {
            std::vector<size_t> near;
            for (size_t j = 0; j < n; j++) {
                if (j == i) continue;
                int d = 0;
                for (int w = 0; w < 4; w++) d += __builtin_popcountll(bits[i][w] ^ bits[j][w]);
                if (d <= 27) near.push_back(j);
            }
            voted[i].assign(v[i]->bytes.begin() + 5, v[i]->bytes.begin() + 32);
            if (near.size() < 2 || v[i]->crc == 0 || v[i]->crc == 1 || v[i]->crc == 2) continue; // CRC-checked: keep
            for (int k = 0; k < 27; k++) {
                uint8_t out = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int ones = (voted[i][k] >> bit) & 1;
                    for (size_t j : near) ones += (v[j]->bytes[5 + k] >> bit) & 1;
                    if (ones * 2 > (int)near.size() + 1) out |= 1 << bit;
                }
                if (out != voted[i][k]) repaired++;
                voted[i][k] = out;
            }
        }
        snprintf(b, sizeof b, "  service %02X: %d bytes repaired by voting carousel repeats", kv.first, repaired);
        r.lines.push_back(b);

        // Continuous text: concatenate payloads in order, cut on control bytes.
        std::string cur;
        char tag[48];
        snprintf(tag, sizeof tag, "[2D %02X line %d] ", kv.first, v[0]->lineNo);
        auto flush = [&]() {
            size_t a = cur.find_first_not_of(' '), e = cur.find_last_not_of(' ');
            if (a != std::string::npos) {
                std::string t = cur.substr(a, e - a + 1);
                int letters = 0, spaces = 0, maxword = 0, word = 0;
                for (char c : t) {
                    if (isalpha((unsigned char)c)) { letters++; word++; maxword = std::max(maxword, word); }
                    else { word = 0; if (c == ' ') spaces++; }
                }
                int freq[256] = {0}, top = 0;
                for (unsigned char c : t) top = std::max(top, ++freq[c]);
                // real text: mostly letters/spaces, a longer word present, and
                // not dominated by one repeated character (0x55 'U' filler etc.)
                if (t.size() >= 10 && (letters + spaces) * 5 >= (int)t.size() * 4 && maxword >= 4 &&
                    top * 5 < (int)t.size() * 2)
                    r.text.push_back(tag + t);
            }
            cur.clear();
        };
        // The text is Interactive Network pages with teletext-like control
        // codes (0x16 + attribute, 0x00 fillers) between words, and binary
        // records in between. Isolated control bytes should not break a line
        // ("160 British peace-" + "keepers from reaching"); only a run of
        // several non-text bytes marks the end of a text record.
        int bin = 0;
        for (size_t i = 0; i < n; i++)
            for (uint8_t c : voted[i]) {
                uint8_t d = c >= 0x80 && printable(c & 0x7F) ? c & 0x7F : c; // undo a one-bit MSB error
                if (printable(d)) {
                    if (bin && !cur.empty() && cur.back() != ' ') cur += ' '; // gap where control/binary was
                    cur += (char)d;
                    bin = 0;
                } else if (d == 0x0D || d == 0x0A) {
                    flush();
                    bin = 0;
                } else if (++bin >= 3) {
                    flush();   // a real binary block ends the text record
                }
            }
        flush();
    }
    if (xpk) {
        snprintf(b, sizeof b, "Framing-2D service: %d packets, header AA AA B4, %.0f%% printable payload (MSB-first ASCII)", xpk,
                 100.0 * xprint / std::max(1, xbytes));
        r.lines.insert(r.lines.end() - (r.lines.empty() ? 0 : 0), b);
    }
    if (wst) {
        snprintf(b, sizeof b, "WST teletext rows: %d", wst);
        r.lines.push_back(b);
    }
    return r;
}

// ----------------------------------------------------------- 2D CRC-32 ---

namespace {

struct Crc2D {
    uint32_t C[217];                          // column of data bit j (1 = last bit sent)
    std::unordered_map<uint32_t, int> one;    // syndrome -> bit (0..215 data, 216.. check)
    std::unordered_map<uint32_t, std::pair<int, int>> two;
    Crc2D()
    {
        const uint32_t G = 0xBE340351u;
        C[1] = G;
        for (int j = 2; j <= 216; j++) C[j] = (C[j - 1] << 1) ^ ((C[j - 1] >> 31) ? G : 0);
        std::vector<uint32_t> sy;
        for (int j = 1; j <= 216; j++) sy.push_back(C[j]);
        for (int t = 0; t < 32; t++) sy.push_back(1u << t);
        for (size_t i = 0; i < sy.size(); i++) one[sy[i]] = (int)i;
        std::unordered_map<uint32_t, int> cnt;
        for (size_t a = 0; a < sy.size(); a++)
            for (size_t b = a + 1; b < sy.size(); b++) cnt[sy[a] ^ sy[b]]++;
        for (size_t a = 0; a < sy.size(); a++)
            for (size_t b = a + 1; b < sy.size(); b++) {
                uint32_t s = sy[a] ^ sy[b];
                if (cnt[s] == 1 && !one.count(s)) two[s] = {(int)a, (int)b};
            }
    }
    // data: the 27 bytes as read (MSB first)
    uint32_t data(const uint8_t* d) const
    {
        uint32_t s = 0;
        for (int k = 0; k < 27; k++)
            for (int m = 0; m < 8; m++)
                if (d[k] & (1 << m)) s ^= C[1 + (26 - k) * 8 + (7 - m)];
        return s;
    }
    static uint32_t check(const uint8_t* c)
    {
        uint32_t y = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t v = c[k], r = 0;
            for (int i = 0; i < 8; i++) r |= ((v >> i) & 1) << (7 - i);
            y = y << 8 | r;
        }
        return y;
    }
    // flips bit i of the syndrome numbering (data bit j = i + 1, check bit i - 216)
    static void flip(uint8_t* p, int i)
    {
        if (i < 216) {
            int j = i + 1, k = 26 - (j - 1) / 8, m = 7 - (j - 1) % 8;
            p[5 + k] ^= (uint8_t)(1 << m);
        } else {
            int t = i - 216, k = 3 - t / 8, b = t % 8;   // bit b of the reversed byte = bit 7-b as read
            p[32 + k] ^= (uint8_t)(1 << (7 - b));
        }
    }
};

} // namespace

std::vector<std::string> fd2dCrc(std::vector<FastPacket>& pk)
{
    static const Crc2D crc;
    std::vector<std::string> out;
    // header term: per (service, counter) the residual the most packets agree on
    std::map<int, std::map<uint32_t, int>> res;
    std::map<std::pair<int, uint32_t>, std::set<uint32_t>> kinds; // distinct payloads behind a residual
    for (auto& p : pk) {
        if (p.kind != 'X' || p.bytes.size() < 37) continue;
        const uint8_t* d = &p.bytes[5];
        bool any = false;
        for (int k = 0; k < 27; k++) any |= d[k] != 0;
        if (!any) continue;
        uint32_t r = crc.data(d) ^ Crc2D::check(&p.bytes[32]);
        res[p.bytes[3] << 8 | p.bytes[4]][r]++;
        auto& ks = kinds[{p.bytes[3] << 8 | p.bytes[4], r}];
        if (ks.size() < 4) ks.insert(crc.data(d));
    }
    std::map<int, uint32_t> K;
    for (auto& kv : res) {
        int tot = 0, b1 = 0, b2 = 0;
        uint32_t best = 0;
        for (auto& r : kv.second) {
            tot += r.second;
            if (r.second > b1) { b2 = b1; b1 = r.second; best = r.first; }
            else b2 = std::max(b2, r.second);
        }
        // the same packet repeated proves nothing: the residual must hold for 3 different payloads
        if (b1 >= 3 && b1 >= 2 * b2 && kinds[{kv.first, best}].size() >= 3) K[kv.first] = best;
    }
    int n[4] = {0, 0, 0, 0}, unk = 0;
    for (auto& p : pk) {
        if (p.kind != 'X' || p.bytes.size() < 37) continue;
        auto it = K.find(p.bytes[3] << 8 | p.bytes[4]);
        if (it == K.end()) { unk++; continue; }
        uint32_t s = crc.data(&p.bytes[5]) ^ Crc2D::check(&p.bytes[32]) ^ it->second;
        if (s == 0) p.crc = 0;
        else if (auto a = crc.one.find(s); a != crc.one.end()) { Crc2D::flip(p.bytes.data(), a->second); p.crc = 1; }
        else if (auto b = crc.two.find(s); b != crc.two.end()) {
            Crc2D::flip(p.bytes.data(), b->second.first);
            Crc2D::flip(p.bytes.data(), b->second.second);
            p.crc = 2;
        } else p.crc = 3;
        n[p.crc]++;
    }
    if (n[0] + n[1] + n[2] + n[3]) {
        char b[260];
        snprintf(b, sizeof b,
                 "Framing 2D CRC-32 (x^32+0xBE340351): %d packets correct, %d repaired 1 bit, %d repaired 2 bits, %d failed; "
                 "%d with a header not seen often enough",
                 n[0], n[1], n[2], n[3], unk);
        out.push_back(b);
    }
    return out;
}
