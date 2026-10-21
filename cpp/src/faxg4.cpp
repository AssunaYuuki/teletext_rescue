#include "faxg4.h"
#include "util.h"
#include "nabts/png.h"
#include <algorithm>
#include <cstring>
#include <map>

namespace {
// T.4 run-length codes: "bits" -> run
struct Code { const char* bits; int run; };
const Code WHITE[] = {
    {"00110101", 0}, {"000111", 1}, {"0111", 2}, {"1000", 3}, {"1011", 4}, {"1100", 5}, {"1110", 6}, {"1111", 7},
    {"10011", 8}, {"10100", 9}, {"00111", 10}, {"01000", 11}, {"001000", 12}, {"000011", 13}, {"110100", 14}, {"110101", 15},
    {"101010", 16}, {"101011", 17}, {"0100111", 18}, {"0001100", 19}, {"0001000", 20}, {"0010111", 21}, {"0000011", 22}, {"0000100", 23},
    {"0101000", 24}, {"0101011", 25}, {"0010011", 26}, {"0100100", 27}, {"0011000", 28}, {"00000010", 29}, {"00000011", 30}, {"00011010", 31},
    {"00011011", 32}, {"00010010", 33}, {"00010011", 34}, {"00010100", 35}, {"00010101", 36}, {"00010110", 37}, {"00010111", 38}, {"00101000", 39},
    {"00101001", 40}, {"00101010", 41}, {"00101011", 42}, {"00101100", 43}, {"00101101", 44}, {"00000100", 45}, {"00000101", 46}, {"00001010", 47},
    {"00001011", 48}, {"01010010", 49}, {"01010011", 50}, {"01010100", 51}, {"01010101", 52}, {"00100100", 53}, {"00100101", 54}, {"01011000", 55},
    {"01011001", 56}, {"01011010", 57}, {"01011011", 58}, {"01001010", 59}, {"01001011", 60}, {"00110010", 61}, {"00110011", 62}, {"00110100", 63},
    {"11011", 64}, {"10010", 128}, {"010111", 192}, {"0110111", 256}, {"00110110", 320}, {"00110111", 384}, {"01100100", 448}, {"01100101", 512},
    {"01101000", 576}, {"01100111", 640}, {"011001100", 704}, {"011001101", 768}, {"011010010", 832}, {"011010011", 896}, {"011010100", 960},
    {"011010101", 1024}, {"011010110", 1088}, {"011010111", 1152}, {"011011000", 1216}, {"011011001", 1280}, {"011011010", 1344},
    {"011011011", 1408}, {"010011000", 1472}, {"010011001", 1536}, {"010011010", 1600}, {"011000", 1664}, {"010011011", 1728}};
const Code BLACK[] = {
    {"0000110111", 0}, {"010", 1}, {"11", 2}, {"10", 3}, {"011", 4}, {"0011", 5}, {"0010", 6}, {"00011", 7},
    {"000101", 8}, {"000100", 9}, {"0000100", 10}, {"0000101", 11}, {"0000111", 12}, {"00000100", 13}, {"00000111", 14}, {"000011000", 15},
    {"0000010111", 16}, {"0000011000", 17}, {"0000001000", 18}, {"00001100111", 19}, {"00001101000", 20}, {"00001101100", 21}, {"00000110111", 22},
    {"00000101000", 23}, {"00000010111", 24}, {"00000011000", 25}, {"000011001010", 26}, {"000011001011", 27}, {"000011001100", 28},
    {"000011001101", 29}, {"000001101000", 30}, {"000001101001", 31}, {"000001101010", 32}, {"000001101011", 33}, {"000011010010", 34},
    {"000011010011", 35}, {"000011010100", 36}, {"000011010101", 37}, {"000011010110", 38}, {"000011010111", 39}, {"000001101100", 40},
    {"000001101101", 41}, {"000011011010", 42}, {"000011011011", 43}, {"000001010100", 44}, {"000001010101", 45}, {"000001010110", 46},
    {"000001010111", 47}, {"000001100100", 48}, {"000001100101", 49}, {"000001010010", 50}, {"000001010011", 51}, {"000000100100", 52},
    {"000000110111", 53}, {"000000111000", 54}, {"000000100111", 55}, {"000000101000", 56}, {"000001011000", 57}, {"000001011001", 58},
    {"000000101011", 59}, {"000000101100", 60}, {"000001011010", 61}, {"000001100110", 62}, {"000001100111", 63},
    {"0000001111", 64}, {"000011001000", 128}, {"000011001001", 192}, {"000001011011", 256}, {"000000110011", 320}, {"000000110100", 384},
    {"000000110101", 448}, {"0000001101100", 512}, {"0000001101101", 576}, {"0000001001010", 640}, {"0000001001011", 704},
    {"0000001001100", 768}, {"0000001001101", 832}, {"0000001110010", 896}, {"0000001110011", 960}, {"0000001110100", 1024},
    {"0000001110101", 1088}, {"0000001110110", 1152}, {"0000001110111", 1216}, {"0000001010010", 1280}, {"0000001010011", 1344},
    {"0000001010100", 1408}, {"0000001010101", 1472}, {"0000001011010", 1536}, {"0000001011011", 1600}, {"0000001100100", 1664},
    {"0000001100101", 1728}};
const Code EXTENDED[] = {   // makeup codes common to both colours
    {"00000001000", 1792}, {"00000001100", 1856}, {"00000001101", 1920}, {"000000010010", 1984}, {"000000010011", 2048},
    {"000000010100", 2112}, {"000000010101", 2176}, {"000000010110", 2240}, {"000000010111", 2304}, {"000000011100", 2368},
    {"000000011101", 2432}, {"000000011110", 2496}, {"000000011111", 2560}};

struct Table {
    std::map<uint32_t, int> m;   // (length << 16 | code) -> run
    Table(const Code* c, size_t n)
    {
        auto add = [&](const Code& x) {
            uint32_t v = 0, l = (uint32_t)strlen(x.bits);
            for (uint32_t i = 0; i < l; i++) v = v << 1 | (x.bits[i] == '1');
            m[l << 16 | v] = x.run;
        };
        for (size_t i = 0; i < n; i++) add(c[i]);
        for (auto& x : EXTENDED) add(x);
    }
};
const Table TW(WHITE, sizeof WHITE / sizeof WHITE[0]), TB(BLACK, sizeof BLACK / sizeof BLACK[0]);

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    bool lsb;
    int get()
    {
        if (pos >= n * 8) return -1;
        int b = p[pos / 8];
        int bit = lsb ? (b >> (pos % 8)) & 1 : (b >> (7 - pos % 8)) & 1;
        pos++;
        return bit;
    }
};

// one run (makeup codes + a terminating code); -1 on a bad code
int readRun(Bits& b, const Table& t)
{
    int total = 0;
    for (;;) {
        uint32_t v = 0;
        int run = -1;
        for (uint32_t l = 1; l <= 13; l++) {
            int x = b.get();
            if (x < 0) return -1;
            v = v << 1 | (uint32_t)x;
            auto it = t.m.find(l << 16 | v);
            if (it != t.m.end()) { run = it->second; break; }
        }
        if (run < 0) return -1;
        total += run;
        if (run < 64) return total;
    }
}

enum Mode { MP, MH, V0, VR1, VR2, VR3, VL1, VL2, VL3, EOL, BAD };
Mode readMode(Bits& b)
{
    // 1 V0, 011 VR1, 010 VL1, 001 H, 0001 P, 000011 VR2, 000010 VL2, 0000011 VR3, 0000010 VL3, 000000000001 EOL
    int zeros = 0;
    for (;;) {
        int x = b.get();
        if (x < 0) return BAD;
        if (x == 1) break;
        if (++zeros > 11) return BAD;
    }
    switch (zeros) {
    case 0: return V0;
    case 1: { int x = b.get(); return x < 0 ? BAD : x ? VR1 : VL1; }
    case 2: return MH;
    case 3: return MP;
    case 4: { int x = b.get(); return x < 0 ? BAD : x ? VR2 : VL2; }
    case 5: { int x = b.get(); return x < 0 ? BAD : x ? VR3 : VL3; }
    case 11: return EOL;
    default: return BAD;
    }
}
}  // namespace

int g4Decode(const uint8_t* p, size_t n, int width, int maxRows, bool lsbFirst, std::vector<uint8_t>& out)
{
    Bits b{p, n, 0, lsbFirst};
    std::vector<int> ref{width, width}, cur;   // changing elements of the reference / coding line
    int rows = 0;
    while (rows < maxRows) {
        cur.clear();
        int a0 = -1, colour = 0;   // 0 = white
        bool bad = false, end = false;
        auto b1b2 = [&](int& b1, int& b2) {
            size_t i = 0;
            while (i < ref.size() && (ref[i] <= a0 || (int)(i & 1) != colour)) i++;
            b1 = i < ref.size() ? ref[i] : width;
            b2 = i + 1 < ref.size() ? ref[i + 1] : width;
        };
        while (a0 < width) {
            Mode m = readMode(b);
            if (m == BAD) { bad = true; break; }
            if (m == EOL) { end = true; break; }
            int b1, b2;
            b1b2(b1, b2);
            if (m == MP) { a0 = b2; continue; }
            if (m == MH) {
                int r1 = readRun(b, colour ? TB : TW), r2 = readRun(b, colour ? TW : TB);
                if (r1 < 0 || r2 < 0) { bad = true; break; }
                int s = std::max(a0, 0), a1 = std::min(width, s + r1), a2 = std::min(width, a1 + r2);
                cur.push_back(a1);
                cur.push_back(a2);
                a0 = a2;
                continue;
            }
            static const int D[] = {0, 0, 0, 1, 2, 3, -1, -2, -3};
            int a1 = std::clamp(b1 + D[m], 0, width);
            cur.push_back(a1);
            a0 = a1;
            colour ^= 1;
        }
        if (end && cur.empty()) break;
        // the row: colours change at cur[]
        size_t at = out.size();
        out.resize(at + width, 0);
        int c = 0, x = 0;
        for (int ch : cur) {
            ch = std::min(ch, width);
            if (c) std::fill(out.begin() + at + x, out.begin() + at + std::max(x, ch), 1);
            x = std::max(x, ch);
            c ^= 1;
        }
        rows++;
        if (bad || end) break;
        ref = cur;
        ref.push_back(width);
        ref.push_back(width);
    }
    return rows;
}

namespace {
struct Fax { int width = 1728, height = 0, strips = 0, rps = 0, bad = 0; bool lsb = true; size_t dataEnd = 0; std::vector<size_t> ends; };

bool faxRead(const std::vector<uint8_t>& t, Fax& F, std::vector<uint8_t>& px)
{
    if (t.size() < 16 || t[0] != 'I' || t[1] != 'I' || t[2] != 42) return false;
    auto u16 = [&](size_t i) { return i + 2 <= t.size() ? (uint32_t)(t[i] | t[i + 1] << 8) : 0u; };
    auto u32 = [&](size_t i) { return u16(i) | u16(i + 2) << 16; };
    uint32_t ifd = u32(4);
    int width = 1728, height = 0, comp = 4;
    if (ifd + 2 <= t.size()) {
        uint32_t n = u16(ifd);
        for (uint32_t k = 0; k < n && k < 64 && ifd + 2 + 12 * k + 12 <= t.size(); k++) {
            size_t e = ifd + 2 + 12 * k;
            uint32_t tag = u16(e), typ = u16(e + 2), v = typ == 3 ? u16(e + 8) : u32(e + 8);
            if (tag == 256 && v > 0 && v < 10000) width = (int)v;
            if (tag == 257 && v > 0 && v < 20000) height = (int)v;
            if (tag == 259) comp = (int)v;
        }
    }
    if (comp != 4) return false;
    size_t dataEnd = ifd > 8 && ifd <= t.size() ? ifd : t.size();
    // strips: each ends in EOFB (two EOL codes); find the fill order in which they are seen
    auto eofbs = [&](bool lsb) {
        std::vector<size_t> ends;
        uint32_t w = 0;
        for (size_t bit = 0; bit < (dataEnd - 8) * 8; bit++) {
            size_t i = 8 + bit / 8;
            int x = lsb ? (t[i] >> (bit % 8)) & 1 : (t[i] >> (7 - bit % 8)) & 1;
            w = (w << 1 | (uint32_t)x) & 0xFFFFFF;
            if (w == 0x001001) ends.push_back(8 + (bit + 8) / 8);
        }
        return ends;
    };
    bool lsb = true;
    std::vector<size_t> ends = eofbs(true);
    if (ends.empty()) { ends = eofbs(false); lsb = false; }
    if (ends.empty()) ends.push_back(dataEnd);
    int strips = (int)ends.size();
    int rps = height ? (height + strips - 1) / strips : 52;
    if (!height) height = rps * strips;
    px.clear();
    int bad = 0;
    size_t start = 8;
    for (int s = 0; s < strips; s++) {
        int rows = std::min(rps, height - s * rps);
        size_t before = px.size();
        int got = g4Decode(&t[start], ends[s] - start, width, rows, lsb, px);
        if (got < rows) bad++;
        px.resize(before + (size_t)rows * width, 0);
        start = ends[s];
    }
    F.width = width; F.height = height; F.strips = strips; F.rps = rps; F.bad = bad; F.lsb = lsb; F.dataEnd = dataEnd; F.ends = ends;
    return true;
}
}  // namespace

bool faxTiffDecode(const std::vector<uint8_t>& tiff, int& width, int& height, std::vector<uint8_t>& px, std::string& note)
{
    Fax F;
    if (!faxRead(tiff, F, px)) return false;
    width = F.width; height = F.height;
    note = fmt("fax page %d x %d, G4, %d strips", F.width, F.height, F.strips) + (F.bad ? fmt(" (%d damaged)", F.bad) : "");
    return true;
}

bool faxTiffToPng(const std::vector<uint8_t>& t, const std::string& png, const std::string& fixedTiff, std::string& note)
{
    Fax F;
    std::vector<uint8_t> px;
    if (!faxRead(t, F, px)) return false;
    int width = F.width, height = F.height, strips = F.strips, rps = F.rps;
    bool lsb = F.lsb;
    size_t dataEnd = F.dataEnd;
    const std::vector<size_t>& ends = F.ends;
    Image im(width, height, 0xFFFFFF);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (px[(size_t)y * width + x]) im.px[(size_t)y * width + x] = 0;
    png_save(png, im);
    note = fmt("fax page %d x %d, G4, %d strips", width, height, strips) + (F.bad ? fmt(" (%d damaged)", F.bad) : "");
    if (!fixedTiff.empty()) {
        // the same data with a complete directory
        std::vector<uint8_t> o(t.begin(), t.begin() + dataEnd);
        while (o.size() & 1) o.push_back(0);
        auto put16 = [&](uint32_t v) { o.push_back(v & 255); o.push_back(v >> 8 & 255); };
        auto put32 = [&](uint32_t v) { put16(v & 0xFFFF); put16(v >> 16); };
        size_t offs = o.size();
        put32(8);
        for (int s = 0; s + 1 < strips; s++) put32((uint32_t)ends[s]);
        size_t cnts = o.size();
        size_t prev = 8;
        for (int s = 0; s < strips; s++) { put32((uint32_t)(ends[s] - prev)); prev = ends[s]; }
        size_t newIfd = o.size();
        o[4] = newIfd & 255; o[5] = newIfd >> 8 & 255; o[6] = newIfd >> 16 & 255; o[7] = newIfd >> 24 & 255;
        struct E { uint16_t tag, typ; uint32_t cnt, val; };
        std::vector<E> es = {{256, 3, 1, (uint32_t)width}, {257, 3, 1, (uint32_t)height}, {258, 3, 1, 1}, {259, 3, 1, 4}, {262, 3, 1, 0},
                             {266, 3, 1, lsb ? 2u : 1u}, {273, 4, (uint32_t)strips, strips > 1 ? (uint32_t)offs : 8u}, {277, 3, 1, 1},
                             {278, 3, 1, (uint32_t)rps}, {279, 4, (uint32_t)strips, strips > 1 ? (uint32_t)cnts : (uint32_t)(ends[0] - 8)}};
        put16((uint32_t)es.size());
        for (auto& e : es) { put16(e.tag); put16(e.typ); put32(e.cnt); put32(e.val); }
        put32(0);
        write_file(fixedTiff, o);
    }
    return true;
}
