// ARJ check: headers and files by CRC-32, the decompression after UNARJ (Robert K. Jung, public domain).
#include "arjcheck.h"
#include "ui/util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

uint32_t crc32Arj(const uint8_t* p, size_t n, uint32_t crc)
{
    static uint32_t T[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            T[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = T[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// methods 1-3 and 4 of ARJ; false when the data does not decode (a damaged file)
class Unarj {
public:
    Unarj(const uint8_t* src, size_t n) : in(src), left(n), total(n) {}
    size_t used() const { return total - left; }   // packed bytes read so far
    bool lz(uint32_t size, std::vector<uint8_t>& out);
    bool fast(uint32_t size, std::vector<uint8_t>& out);

private:
    enum { CODE_BIT = 16, THRESHOLD = 3, MAXMATCH = 256, NC = 255 + MAXMATCH + 2 - THRESHOLD, NP = 17, CBIT = 9, NT = 19, PBIT = 5,
           TBIT = 5, NPT = NT };
    const uint8_t* in;
    size_t left, total;
    uint16_t bitbuf = 0;
    unsigned subbitbuf = 0;
    int bitcount = 0;
    bool bad = false;
    uint16_t left_[2 * NC - 1] = {}, right_[2 * NC - 1] = {};
    uint8_t c_len[NC] = {}, pt_len[NPT] = {};
    uint16_t c_table[4096] = {}, pt_table[256] = {};
    unsigned blocksize = 0;

    void fillbuf(int n)
    {
        bitbuf = (uint16_t)(bitbuf << n);
        while (n > bitcount) {
            bitbuf |= (uint16_t)(subbitbuf << (n -= bitcount));
            if (left) { left--; subbitbuf = *in++; } else subbitbuf = 0;
            bitcount = 8;
        }
        bitbuf |= (uint16_t)(subbitbuf >> (bitcount -= n));
    }
    unsigned getbits(int n)
    {
        unsigned x = bitbuf >> (CODE_BIT - n);
        fillbuf(n);
        return x;
    }
    void initGetbits() { bitbuf = 0; subbitbuf = 0; bitcount = 0; fillbuf(CODE_BIT); }

    bool makeTable(int nchar, const uint8_t* bitlen, int tablebits, uint16_t* table, int tablesize)
    {
        uint16_t count[17] = {}, weight[17] = {}, start[18] = {};
        for (int i = 0; i < nchar; i++) { if (bitlen[i] > 16) return false; count[bitlen[i]]++; }
        start[1] = 0;
        for (int i = 1; i <= 16; i++) start[i + 1] = (uint16_t)(start[i] + (count[i] << (16 - i)));
        if (start[17] != (uint16_t)(1U << 16)) return false;
        int jutbits = 16 - tablebits, i;
        for (i = 1; i <= tablebits; i++) { start[i] >>= jutbits; weight[i] = (uint16_t)(1U << (tablebits - i)); }
        while (i <= 16) { weight[i] = (uint16_t)(1U << (16 - i)); i++; }
        i = start[tablebits + 1] >> jutbits;
        if (i != (uint16_t)(1U << 16)) {
            int k = 1 << tablebits;
            while (i != k) { if (i >= tablesize) return false; table[i++] = 0; }
        }
        int avail = nchar;
        unsigned mask = 1U << (15 - tablebits);
        for (int ch = 0; ch < nchar; ch++) {
            int len = bitlen[ch];
            if (len == 0) continue;
            uint16_t k = start[len];
            uint16_t nextcode = (uint16_t)(k + weight[len]);
            if (len <= tablebits) {
                if (nextcode > tablesize && nextcode != 0) return false;
                for (int q = k; q < (nextcode ? nextcode : tablesize); q++) table[q] = (uint16_t)ch;
            } else {
                uint16_t* p = &table[k >> jutbits];
                int n = len - tablebits;
                while (n != 0) {
                    if (*p == 0) {
                        if (avail >= 2 * NC - 1) return false;
                        right_[avail] = left_[avail] = 0;
                        *p = (uint16_t)avail++;
                    }
                    if (*p >= 2 * NC - 1) return false;
                    p = (k & mask) ? &right_[*p] : &left_[*p];
                    k = (uint16_t)(k << 1);
                    n--;
                }
                *p = (uint16_t)ch;
            }
            start[len] = nextcode;
        }
        return true;
    }
    bool readPtLen(int nn, int nbit, int special)
    {
        int n = getbits(nbit);
        if (n == 0) {
            int c = getbits(nbit);
            memset(pt_len, 0, nn);
            for (int i = 0; i < 256; i++) pt_table[i] = (uint16_t)c;
            return true;
        }
        if (n > nn) return false;
        int i = 0;
        while (i < n) {
            int c = bitbuf >> 13;
            if (c == 7) {
                unsigned mask = 1U << 12;
                while (mask & bitbuf) { mask >>= 1; c++; if (c > 16) return false; }
            }
            fillbuf(c < 7 ? 3 : c - 3);
            pt_len[i++] = (uint8_t)c;
            if (i == special) {
                int z = getbits(2);
                while (--z >= 0 && i < nn) pt_len[i++] = 0;
            }
        }
        while (i < nn) pt_len[i++] = 0;
        return makeTable(nn, pt_len, 8, pt_table, 256);
    }
    bool readCLen()
    {
        int n = getbits(CBIT);
        if (n == 0) {
            int c = getbits(CBIT);
            memset(c_len, 0, NC);
            for (int i = 0; i < 4096; i++) c_table[i] = (uint16_t)c;
            return true;
        }
        if (n > NC) return false;
        int i = 0;
        while (i < n) {
            int c = pt_table[bitbuf >> 8];
            if (c >= NT) {
                unsigned mask = 1U << 7;
                int guard = 0;
                do {
                    c = (bitbuf & mask) ? right_[c] : left_[c];
                    mask >>= 1;
                    if (++guard > 32) return false;
                } while (c >= NT);
            }
            fillbuf(pt_len[c]);
            if (c <= 2) {
                if (c == 0) c = 1;
                else if (c == 1) c = getbits(4) + 3;
                else c = getbits(CBIT) + 20;
                while (--c >= 0) { if (i >= NC) return false; c_len[i++] = 0; }
            } else c_len[i++] = (uint8_t)(c - 2);
        }
        while (i < NC) c_len[i++] = 0;
        return makeTable(NC, c_len, 12, c_table, 4096);
    }
    int decodeC()
    {
        if (blocksize == 0) {
            blocksize = getbits(16);
            if (!readPtLen(NT, TBIT, 3) || !readCLen() || !readPtLen(NP, PBIT, -1)) { bad = true; return 0; }
        }
        blocksize--;
        int j = c_table[bitbuf >> 4];
        if (j >= NC) {
            unsigned mask = 1U << 3;
            int guard = 0;
            do {
                j = (bitbuf & mask) ? right_[j] : left_[j];
                mask >>= 1;
                if (++guard > 32) { bad = true; return 0; }
            } while (j >= NC);
        }
        fillbuf(c_len[j]);
        return j;
    }
    int decodeP()
    {
        int j = pt_table[bitbuf >> 8];
        if (j >= NP) {
            unsigned mask = 1U << 7;
            int guard = 0;
            do {
                j = (bitbuf & mask) ? right_[j] : left_[j];
                mask >>= 1;
                if (++guard > 32) { bad = true; return 0; }
            } while (j >= NP);
        }
        fillbuf(pt_len[j]);
        if (j != 0) { j--; j = (1 << j) + getbits(j); }
        return j;
    }
    // method 4
    uint16_t getbuf = 0;
    int getlen = 0;
    void bfil() { getbuf |= (uint16_t)(bitbuf >> getlen); fillbuf(CODE_BIT - getlen); getlen = CODE_BIT; }
    int getbit() { if (getlen <= 0) bfil(); int c = (getbuf & 0x8000) != 0; getbuf = (uint16_t)(getbuf << 1); getlen--; return c; }
    int getbitsF(int l) { if (getlen < l) bfil(); int c = getbuf >> (CODE_BIT - l); getbuf = (uint16_t)(getbuf << l); getlen -= l; return c; }
    int decodeRun(int strt, int stop)
    {
        int plus = 0, pwr = 1 << strt, width;
        for (width = strt; width < stop; width++) {
            if (!getbit()) break;
            plus += pwr;
            pwr <<= 1;
        }
        int c = width ? getbitsF(width) : 0;
        return c + plus;
    }
};

bool Unarj::lz(uint32_t size, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(size);
    initGetbits();
    blocksize = 0;
    while (out.size() < size) {
        int c = decodeC();
        if (bad) return false;
        if (c <= 255) { out.push_back((uint8_t)c); continue; }
        int j = c - (256 - THRESHOLD);
        int d = decodeP();
        if (bad) return false;
        if ((size_t)d + 1 > out.size()) return false;
        size_t from = out.size() - d - 1;
        for (int k = 0; k < j && out.size() < size; k++) out.push_back(out[from + k]);
    }
    return true;
}

bool Unarj::fast(uint32_t size, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(size);
    initGetbits();
    getlen = 0; getbuf = 0;
    while (out.size() < size) {
        int c = decodeRun(0, 7);
        if (c == 0) { out.push_back((uint8_t)getbitsF(8)); continue; }
        int j = c - 1 + THRESHOLD;
        int d = decodeRun(9, 13);
        if ((size_t)d + 1 > out.size()) return false;
        size_t from = out.size() - d - 1;
        for (int k = 0; k < j && out.size() < size; k++) out.push_back(out[from + k]);
    }
    return true;
}

// ---------------------------------------------------------------- methods 1-3 through damage
// A file whose packed data has holes (rows of a broadcast not received) or is cut off: decoding goes on after a hole.
// The bit position after a hole is known exactly (the hole keeps its length), and a Huffman stream falls back into
// step with its symbols after a few of them; what the hole held is unknown (its length guessed from the packing rate,
// marked as unknown bytes), and a new block's tables are recognised by their own checks (each table a complete code).
struct LzTables {
    enum { NC = 510, NP = 17, NT = 19, NPT = NT, CBIT = 9, PBIT = 5, TBIT = 5 };
    uint16_t left_[2 * NC - 1] = {}, right_[2 * NC - 1] = {};
    uint8_t c_len[NC] = {}, pt_len[NPT] = {};
    uint16_t c_table[4096] = {}, pt_table[256] = {};
};
class LzRec {
public:
    LzRec(const uint8_t* src, size_t n, const std::vector<char>& lost) : p(src), n(n), lost(lost) {}
    // out/unk: the file's bytes and which of them are not known; false = nothing could be decoded
    void run(uint32_t size, std::vector<uint8_t>& out, std::vector<char>& unk, int& holes, bool& cut);

private:
    const uint8_t* p;
    size_t n;
    const std::vector<char>& lost;
    size_t pos = 0;   // bit position
    unsigned peek() const   // the next 16 bits, MSB first (zeros past the end)
    {
        size_t b = pos >> 3;
        uint32_t w = 0;
        for (int k = 0; k < 3; k++) w = w << 8 | (b + k < n ? p[b + k] : 0);
        return (w >> (8 - (pos & 7))) & 0xFFFF;
    }
    void skip(int k) { pos += k; }
    unsigned get(int k) { if (!k) return 0; unsigned v = peek() >> (16 - k); pos += k; return v; }
    bool lostIn(size_t bitA, size_t bitB) const
    {
        for (size_t b = bitA >> 3; b <= (bitB >> 3) && b < n; b++) if (lost[b]) return true;
        return false;
    }
    static bool makeTable(LzTables& T, int nchar, const uint8_t* bitlen, int tablebits, uint16_t* table, int tablesize)
    {
        uint16_t count[17] = {}, weight[17] = {}, start[18] = {};
        for (int i = 0; i < nchar; i++) { if (bitlen[i] > 16) return false; count[bitlen[i]]++; }
        for (int i = 1; i <= 16; i++) start[i + 1] = (uint16_t)(start[i] + (count[i] << (16 - i)));
        if (start[17] != (uint16_t)(1U << 16)) return false;
        int jutbits = 16 - tablebits, i;
        for (i = 1; i <= tablebits; i++) { start[i] >>= jutbits; weight[i] = (uint16_t)(1U << (tablebits - i)); }
        while (i <= 16) { weight[i] = (uint16_t)(1U << (16 - i)); i++; }
        i = start[tablebits + 1] >> jutbits;
        if (i != (uint16_t)(1U << 16)) {
            int k = 1 << tablebits;
            while (i != k) { if (i >= tablesize) return false; table[i++] = 0; }
        }
        int avail = nchar;
        unsigned mask = 1U << (15 - tablebits);
        for (int ch = 0; ch < nchar; ch++) {
            int len = bitlen[ch];
            if (len == 0) continue;
            uint16_t k = start[len], nextcode = (uint16_t)(k + weight[len]);
            if (len <= tablebits) {
                if (nextcode > tablesize && nextcode != 0) return false;
                for (int q = k; q < (nextcode ? nextcode : tablesize); q++) table[q] = (uint16_t)ch;
            } else {
                uint16_t* t = &table[k >> jutbits];
                int m = len - tablebits;
                while (m != 0) {
                    if (*t == 0) {
                        if (avail >= 2 * LzTables::NC - 1) return false;
                        T.right_[avail] = T.left_[avail] = 0;
                        *t = (uint16_t)avail++;
                    }
                    if (*t >= 2 * LzTables::NC - 1) return false;
                    t = (k & mask) ? &T.right_[*t] : &T.left_[*t];
                    k = (uint16_t)(k << 1);
                    m--;
                }
                *t = (uint16_t)ch;
            }
            start[len] = nextcode;
        }
        return true;
    }
    bool readPtLen(LzTables& T, int nn, int nbit, int special)
    {
        int m = get(nbit);
        if (m == 0) {
            int c = get(nbit);
            if (c >= nn) return false;
            memset(T.pt_len, 0, nn);
            for (int i = 0; i < 256; i++) T.pt_table[i] = (uint16_t)c;
            return true;
        }
        if (m > nn) return false;
        int i = 0;
        while (i < m) {
            int c = peek() >> 13;
            if (c == 7) {
                unsigned mask = 1U << 12;
                while (mask & peek()) { mask >>= 1; c++; if (c > 16) return false; }
            }
            skip(c < 7 ? 3 : c - 3);
            T.pt_len[i++] = (uint8_t)c;
            if (i == special) {
                int z = get(2);
                while (--z >= 0 && i < nn) T.pt_len[i++] = 0;
            }
        }
        while (i < nn) T.pt_len[i++] = 0;
        return makeTable(T, nn, T.pt_len, 8, T.pt_table, 256);
    }
    bool readCLen(LzTables& T)
    {
        int m = get(LzTables::CBIT);
        if (m == 0) {
            int c = get(LzTables::CBIT);
            if (c >= LzTables::NC) return false;
            memset(T.c_len, 0, LzTables::NC);
            for (int i = 0; i < 4096; i++) T.c_table[i] = (uint16_t)c;
            return true;
        }
        if (m > LzTables::NC) return false;
        int i = 0;
        while (i < m) {
            int c = T.pt_table[peek() >> 8];
            if (c >= LzTables::NT) {
                unsigned mask = 1U << 7;
                int guard = 0;
                do {
                    c = (peek() & mask) ? T.right_[c] : T.left_[c];
                    mask >>= 1;
                    if (++guard > 32) return false;
                } while (c >= LzTables::NT);
            }
            skip(T.pt_len[c]);
            if (c <= 2) {
                if (c == 0) c = 1;
                else if (c == 1) c = get(4) + 3;
                else c = get(LzTables::CBIT) + 20;
                while (--c >= 0) { if (i >= LzTables::NC) return false; T.c_len[i++] = 0; }
            } else T.c_len[i++] = (uint8_t)(c - 2);
        }
        while (i < LzTables::NC) T.c_len[i++] = 0;
        return makeTable(T, LzTables::NC, T.c_len, 12, T.c_table, 4096);
    }
    // a block header at the current position: its symbol count and three tables, every one a complete code
    bool header(LzTables& T, unsigned& blocksize)
    {
        if (pos + 64 > n * 8) return false;
        blocksize = get(16);
        if (blocksize == 0) return false;
        return readPtLen(T, LzTables::NT, LzTables::TBIT, 3) && readCLen(T) && readPtLen(T, LzTables::NP, LzTables::PBIT, -1);
    }
    bool tryHeader(LzTables& T, unsigned& blocksize)
    {
        size_t keep = pos;
        if (header(T, blocksize)) return true;
        pos = keep;
        return false;
    }
    int decodeC(const LzTables& T, bool& bad)
    {
        int j = T.c_table[peek() >> 4];
        if (j >= LzTables::NC) {
            unsigned mask = 1U << 3;
            int guard = 0;
            do {
                j = (peek() & mask) ? T.right_[j] : T.left_[j];
                mask >>= 1;
                if (++guard > 32) { bad = true; return 0; }
            } while (j >= LzTables::NC);
        }
        skip(T.c_len[j]);
        return j;
    }
    int decodeP(const LzTables& T, bool& bad)
    {
        int j = T.pt_table[peek() >> 8];
        if (j >= LzTables::NP) {
            unsigned mask = 1U << 7;
            int guard = 0;
            do {
                j = (peek() & mask) ? T.right_[j] : T.left_[j];
                mask >>= 1;
                if (++guard > 32) { bad = true; return 0; }
            } while (j >= LzTables::NP);
        }
        skip(T.pt_len[j]);
        if (j != 0) { j--; j = (1 << j) + get(j); }
        return j;
    }
};

void LzRec::run(uint32_t size, std::vector<uint8_t>& out, std::vector<char>& unk, int& holes, bool& cut)
{
    out.clear();
    unk.clear();
    holes = 0;
    cut = false;
    LzTables T;
    bool haveT = false, blockKnown = true;
    long left = 0;            // symbols left in the block (blockKnown)
    long estLeft = -1;        // after a hole: the guess of it (-1 = no idea)
    long sinceSync = 0;
    size_t syncOut = 0, syncBits = 0;   // where the stretch now being decoded began (for the packing rate)
    size_t goodOut = 0, goodBits = 0;   // decoded bytes and bits of all the stretches before
    auto emit = [&](uint8_t v, char u) { out.push_back(v); unk.push_back(u); };
    // a hole (or a symbol that does not decode): skip to the next received byte, guess what the hole held, go on
    auto hole = [&](size_t badBit) {
        holes++;
        goodOut += out.size() - syncOut;
        goodBits += badBit > syncBits ? badBit - syncBits : 0;
        size_t g = badBit >> 3;
        // the lost bytes the symbol ran into, or one byte when the data itself did not decode
        size_t g0 = g;
        while (g < n && g < g0 + 16 && !lost[g]) g++;
        if (g >= n || g >= g0 + 16) g = g0 + 1;
        while (g < n && lost[g]) g++;
        double rate = goodBits ? (double)goodOut / goodBits : 0.3;   // bytes a bit
        double bitsPerSym = (goodBits && goodOut) ? (double)goodBits / std::max<size_t>(1, goodOut) * 1.6 : 6.0;
        size_t gapBits = g * 8 > badBit ? g * 8 - badBit : 8;
        size_t est = (size_t)(gapBits * rate + 0.5);
        if (out.size() + est > size) est = size - out.size();
        for (size_t k = 0; k < est; k++) emit('?', 1);
        long gapSyms = (long)(gapBits / std::max(1.0, bitsPerSym));
        if (blockKnown && haveT) estLeft = left - gapSyms;
        else if (estLeft >= 0) estLeft -= gapSyms;
        blockKnown = false;
        sinceSync = 0;
        pos = g * 8;
        syncOut = out.size();
        syncBits = pos;
    };
    while (out.size() < size) {
        if (pos >= n * 8) { cut = true; break; }
        if (!haveT) {
            // the first tables were lost: the first block header after this point that checks
            unsigned bs;
            bool found = false;
            for (size_t q = pos; q + 64 <= n * 8; q++) {
                pos = q;
                if (!lostIn(q, q + 8 * 48) && tryHeader(T, bs)) { found = true; left = bs; break; }
            }
            if (!found) { cut = true; break; }
            haveT = true;
            blockKnown = true;
            continue;
        }
        if (blockKnown && left == 0) {
            unsigned bs;
            size_t at = pos;
            if (lostIn(pos, pos + 8 * 48) || !header(T, bs)) { pos = at; haveT = false; hole(at); continue; }
            left = bs;
        } else if (!blockKnown && (estLeft < 0 || sinceSync >= estLeft - 400) && sinceSync <= (estLeft < 0 ? (1L << 30) : estLeft + 400)) {
            // after a hole the block's end is a guess: a header that checks, near where the block should end
            unsigned bs;
            LzTables T2;
            if (!lostIn(pos, pos + 8 * 48) && tryHeader(T2, bs)) { T = T2; left = bs; blockKnown = true; estLeft = -1; }
        }
        size_t at = pos;
        if (lostIn(pos, pos + 48)) { hole(at); continue; }
        bool bad = false;
        int c = decodeC(T, bad);
        if (bad) { hole(at); continue; }
        if (blockKnown) left--;
        sinceSync++;
        if (c <= 255) { emit((uint8_t)c, 0); continue; }
        int j = c - (256 - 3);
        int d = decodeP(T, bad);
        if (bad) { hole(at); continue; }
        if ((size_t)d + 1 > out.size()) {
            // after a hole its length is a guess: a copy from before the start is bytes not known, the stream goes on
            if (!holes) { hole(at); continue; }
            for (int k = 0; k < j && out.size() < size; k++) emit('?', 1);
            continue;
        }
        size_t from = out.size() - d - 1;
        for (int k = 0; k < j && out.size() < size; k++) emit(out[from + k], unk[from + k]);
    }
}

// a header at p: 60 EA, size, data, CRC-32, extended headers; false when it does not check
bool header(const std::vector<uint8_t>& a, size_t p, size_t& data, size_t& size, size_t& next, bool& crcOk)
{
    if (p + 4 > a.size() || a[p] != 0x60 || a[p + 1] != 0xEA) return false;
    size = u16(&a[p + 2]);
    data = p + 4;
    if (size == 0) { next = p + 4; crcOk = true; return true; }   // the end of the archive
    if (size > 2600 || data + size + 4 > a.size()) return false;
    crcOk = crc32Arj(&a[data], size) == u32(&a[data + size]);
    next = data + size + 4;
    for (int guard = 0; guard < 16; guard++) {   // extended headers
        if (next + 2 > a.size()) return false;
        size_t ext = u16(&a[next]);
        next += 2;
        if (ext == 0) break;
        next += ext + 4;
    }
    return next <= a.size();
}

} // namespace

ArjCheck arjCheck(const std::vector<uint8_t>& a, const std::vector<char>* lostMask)
{
    ArjCheck r;
    size_t data, size, next;
    bool ok;
    size_t p = 0;
    if (!header(a, 0, data, size, next, ok) || size == 0) {
        // the start of the archive did not come: its files from the first header that checks by its CRC on
        for (size_t q = 1; q + 4 < a.size(); q++) {
            size_t d2, s2, n2;
            bool o2;
            if (a[q] == 0x60 && a[q + 1] == 0xEA && header(a, q, d2, s2, n2, o2) && o2 && s2 >= 30) { p = q; break; }
        }
        if (!p) return r;
        r.archive = true;
        r.startLost = true;
        header(a, p, data, size, next, ok);
        if (a[data + 6] == 2) { r.headerOk = true; p = next; }   // the archive's own header after all, further on
    } else {
        r.archive = true;
        r.headerOk = ok;
        p = next;
    }
    while (p < a.size()) {
        if (!header(a, p, data, size, next, ok)) {
            // a damaged header: look for the next one (its CRC tells a real one from a chance 60 EA)
            size_t q = p + 1;
            bool found = false;
            for (; q + 4 < a.size(); q++) {
                size_t d2, s2, n2;
                bool o2;
                if (a[q] == 0x60 && a[q + 1] == 0xEA && header(a, q, d2, s2, n2, o2) && o2) { found = true; break; }
            }
            ArjEntry e;
            e.name = "?";
            e.why = "header damaged";
            r.files.push_back(e);
            if (!found) break;
            p = q;
            continue;
        }
        if (size == 0) { r.complete = true; break; }
        ArjEntry e;
        const uint8_t* h = &a[data];
        size_t first = h[0];
        int flags = h[4];
        e.method = h[5];
        e.packed = u32(h + 12);
        e.size = u32(h + 16);
        uint32_t crc = u32(h + 20);
        e.crc = crc;
        for (size_t k = first; k < size && h[k]; k++) e.name += (char)h[k];
        if (!ok) e.why = "header CRC";
        size_t body = next;
        e.at = body;
        e.hdr = p;
        // a damaged header still says where its file is: when its packed size does not end at the next header (or
        // the end of the archive), the next 60 EA after the data decides
        auto headerAt = [&](size_t q) { return q + 4 <= a.size() && a[q] == 0x60 && a[q + 1] == 0xEA; };
        if (!ok && (body + e.packed > a.size() || !headerAt(body + e.packed))) {
            size_t q = body;
            while (q + 4 <= a.size() && !headerAt(q)) q++;
            e.packed = (uint32_t)(std::min(q, a.size()) - body);
        }
        // the archive ends inside this file: what of it came is unpacked all the same
        bool cutOff = body + e.packed > a.size();
        uint32_t avail = cutOff ? (uint32_t)(a.size() - body) : e.packed;
        if (!ok && (e.size > e.packed * 40u + 65536u || e.size > (32u << 20))) e.size = e.packed * 40u + 65536u;
        if (!ok && e.method > 4) e.method = 1;
        if (h[6] != 3 || !ok) {   // 3 = a directory
            // the bytes of the packed data that did not come: as the caller knows them, or (packed data has no runs
            // of zeros) a run of 8 and more zero bytes, the way rows not received are filled
            std::vector<char> lost(avail, 0);
            size_t nLost = 0;
            if (lostMask) {
                for (uint32_t k = 0; k < avail; k++) if (body + k < lostMask->size() && (*lostMask)[body + k]) { lost[k] = 1; nLost++; }
            } else if (e.method >= 1)
                for (uint32_t k = 0; k < avail;) {
                    uint32_t j = k;
                    while (j < avail && a[body + j] == 0) j++;
                    if (j - k >= 8) for (uint32_t q = k; q < j; q++) { lost[q] = 1; nLost++; }
                    k = j > k ? j : k + 1;
                }
            std::vector<uint8_t> out;
            std::vector<char> unk;
            bool dec = false;
            if (flags & 1 && ok) e.why = "encrypted (garbled)";
            else if (e.method == 0) {
                out.assign(a.begin() + body, a.begin() + body + avail);
                unk.assign(lost.begin(), lost.end());
                dec = !cutOff;
            } else if (e.method >= 1 && e.method <= 3) {
                if (!cutOff) dec = Unarj(&a[body], e.packed).lz(e.size, out);
                bool right = dec && out.size() == e.size && crc32Arj(out.data(), out.size()) == crc;
                if (!right) {
                    // damaged, holed or cut off: decoded on past the damage
                    std::vector<uint8_t> o2;
                    std::vector<char> u2;
                    bool c2 = false;
                    LzRec(&a[body], avail, lost).run(e.size, o2, u2, e.holes, c2);
                    size_t known2 = 0, known1 = out.size();
                    for (char u : u2) known2 += !u;
                    // bytes of the packed data lost: what the plain decoding made after them is not to be trusted
                    if (nLost) known1 = 0;
                    if (known2 > known1 || crc32Arj(o2.data(), o2.size()) == crc) {
                        out.swap(o2);
                        unk.swap(u2);
                        dec = !c2 && e.holes == 0 && out.size() == e.size;
                    } else e.holes = 0;
                }
            } else if (e.method == 4) {
                dec = Unarj(&a[body], avail).fast(e.size, out);
                if (cutOff) dec = false;
            } else e.why = "unknown method";
            unk.resize(out.size(), 0);
            for (char u : unk) e.unknown += u != 0;
            e.got = (uint32_t)out.size();
            bool right = out.size() == e.size && crc32Arj(out.data(), out.size()) == crc;
            if (e.why.empty()) {
                if (right) e.ok = true;
                else if (cutOff) e.why = "cut off: the archive ends inside it";
                else if (e.unknown) e.why = fmt("%u bytes lost in %d holes", e.unknown, std::max(1, e.holes));
                else if (!dec || out.size() != e.size) e.why = "data does not unpack whole";
                else e.why = "data CRC";
            } else if (right) e.why += ", but the data's CRC is right";
            // bytes not known: "?" in a text, zero in anything else
            if (e.unknown) {
                size_t text = 0, known = 0;
                for (size_t k = 0; k < out.size(); k++)
                    if (!unk[k]) { known++; uint8_t c = out[k]; text += c == 9 || c == 10 || c == 13 || c >= 32; }
                bool isText = known && text >= 0.95 * known;
                for (size_t k = 0; k < out.size(); k++) if (unk[k]) out[k] = isText ? '?' : 0;
            }
            e.data = std::move(out);
        } else e.ok = true;
        r.files.push_back(e);
        if (cutOff) break;
        p = body + e.packed;
    }
    return r;
}

// one file of an archive unpacked from a[at, at+packed): does it give its CRC
static bool memberRight(const std::vector<uint8_t>& a, size_t at, uint32_t packed, int method, uint32_t size, uint32_t crc)
{
    if (at + packed > a.size()) return false;
    std::vector<uint8_t> out;
    bool dec = false;
    if (method == 0) { if (packed != size) return false; return crc32Arj(&a[at], packed) == crc; }
    if (method >= 1 && method <= 3) dec = Unarj(&a[at], packed).lz(size, out);
    else if (method == 4) dec = Unarj(&a[at], packed).fast(size, out);
    return dec && out.size() == size && crc32Arj(out.data(), out.size()) == crc;
}

int arjRepair(std::vector<uint8_t>& a, const std::vector<char>* lost, const std::vector<std::vector<uint8_t>>* alts,
              std::string& log, double seconds)
{
    auto t0 = std::chrono::steady_clock::now();
    auto late = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds; };
    int mended = 0;
    // headers first (a damaged header can hide where its file is): a header's own CRC-32 chooses among the copies'
    // readings and one-bit errors of its bytes
    {
        ArjCheck h = arjCheck(a, lost);
        std::vector<size_t> heads;
        for (auto& e : h.files) if (e.why.find("header CRC") != std::string::npos && e.hdr) heads.push_back(e.hdr);
        if (!h.headerOk && !h.startLost) heads.push_back(0);
        for (size_t p : heads) {
            if (late() || p + 4 > a.size()) continue;
            // the header's length may be the damaged byte: its one-bit variants and the copies' readings too
            std::vector<std::pair<uint8_t, uint8_t>> sz{{a[p + 2], a[p + 3]}};
            for (int b = 0; b < 16; b++) sz.push_back({(uint8_t)(a[p + 2] ^ (b < 8 ? 1 << b : 0)), (uint8_t)(a[p + 3] ^ (b >= 8 ? 1 << (b - 8) : 0))});
            if (alts && p + 3 < alts->size()) {
                for (uint8_t v : (*alts)[p + 2]) sz.push_back({v, a[p + 3]});
                for (uint8_t v : (*alts)[p + 3]) sz.push_back({a[p + 2], v});
            }
            const uint8_t k2 = a[p + 2], k3 = a[p + 3];
            bool found = false;
            size_t size = 0;
            for (auto& s : sz) {
                size_t z = s.first | s.second << 8;
                if (z < 20 || z > 2600 || p + 4 + z + 4 > a.size()) continue;
                if (crc32Arj(&a[p + 4], z) == u32(&a[p + 4 + z])) { a[p + 2] = s.first; a[p + 3] = s.second; size = z; found = true; break; }
            }
            if (!found) {
                size = k2 | k3 << 8;
                // the length as it is, then one bit wrong elsewhere in the header (below)
                for (auto& s : sz) {
                    size_t z = s.first | s.second << 8;
                    if (z >= 20 && z <= 2600 && p + 4 + z + 4 <= a.size()) { size = z; a[p + 2] = s.first; a[p + 3] = s.second; break; }
                }
            }
            if (size < 20 || size > 2600 || p + 4 + size + 4 > a.size()) { a[p + 2] = k2; a[p + 3] = k3; continue; }
            const size_t d = p + 4;
            auto ok = [&] { return crc32Arj(&a[d], size) == u32(&a[d + size]); };
            bool done = found;
            if (alts)
                for (size_t k = d; k < d + size + 4 && !done; k++)
                    if (k < alts->size())
                        for (uint8_t v : (*alts)[k]) { uint8_t keep = a[k]; a[k] = v; if ((done = ok())) break; a[k] = keep; }
            for (size_t k = d; k < d + size + 4 && !done; k++)
                for (int b = 0; b < 8; b++) {
                    a[k] ^= (uint8_t)(1 << b);
                    if ((done = ok())) break;
                    a[k] ^= (uint8_t)(1 << b);
                }
            if (!done) { a[p + 2] = k2; a[p + 3] = k3; }
            else log += fmt("header at byte %zu put right by its CRC\n", p);
        }
    }
    ArjCheck c = arjCheck(a, lost);
    for (auto& e : c.files) {
        if (e.ok || e.at == 0 || e.packed == 0 || e.method > 4 || e.at + e.packed > a.size() || late()) continue;
        if (e.why.find("header") != std::string::npos || e.why.find("encrypted") != std::string::npos) continue;
        const size_t at = e.at, end = at + e.packed;
        auto right = [&] { return memberRight(a, at, e.packed, e.method, e.size, e.crc); };
        bool done = false;
        std::string how;
        // (3) bytes not received: one or two, all their values
        std::vector<size_t> L;
        if (lost) for (size_t k = at; k < end && k < lost->size(); k++) if ((*lost)[k]) L.push_back(k);
        if (!L.empty() && L.size() <= 2) {
            if (L.size() == 1) {
                for (int v = 0; v < 256 && !done; v++) { a[L[0]] = (uint8_t)v; done = right(); }
            } else
                for (int v = 0; v < 65536 && !done && !(v % 256 == 0 && late()); v++) { a[L[0]] = (uint8_t)(v >> 8); a[L[1]] = (uint8_t)v; done = right(); }
            if (done) how = fmt("%zu byte(s) not received found by the CRC", L.size());
        }
        // (1) where copies differ: every combination (up to 4096), else one at a time
        if (!done && alts) {
            std::vector<size_t> P;
            for (size_t k = at; k < end && k < alts->size(); k++) if (!(*alts)[k].empty()) P.push_back(k);
            if (!P.empty()) {
                std::vector<uint8_t> keep;
                for (size_t k : P) keep.push_back(a[k]);
                double combos = 1;
                for (size_t k : P) combos *= 1 + (*alts)[k].size();
                if (combos <= 4096) {
                    std::vector<size_t> pick(P.size(), 0);
                    for (;;) {
                        size_t q = 0;
                        while (q < P.size() && ++pick[q] > (*alts)[P[q]].size()) pick[q++] = 0;
                        if (q == P.size()) break;
                        for (size_t i = 0; i < P.size(); i++) a[P[i]] = pick[i] ? (*alts)[P[i]][pick[i] - 1] : keep[i];
                        if ((done = right())) break;
                    }
                } else
                    for (size_t i = 0; i < P.size() && !done && !late(); i++)
                        for (uint8_t v : (*alts)[P[i]]) { a[P[i]] = v; if ((done = right())) break; a[P[i]] = keep[i]; }
                if (done) how = "bytes the copies read differently chosen by the CRC";
                else for (size_t i = 0; i < P.size(); i++) a[P[i]] = keep[i];
            }
        }
        // (2) one bit wrong in one byte
        if (!done && L.empty())
            for (size_t k = at; k < end && !done && !late(); k++)
                for (int b = 0; b < 8; b++) {
                    a[k] ^= (uint8_t)(1 << b);
                    if ((done = right())) { how = fmt("one wrong bit at byte %zu put right by the CRC", k); break; }
                    a[k] ^= (uint8_t)(1 << b);
                }
        // (4) several bytes wrong: a change is kept when the data unpacks further before it stops (a wrong byte makes
        // the decoding fail some way after it), until the CRC is right - the copies' readings first, then one-bit
        // errors in the stretch before where the decoding stopped
        if (!done && L.empty() && e.method >= 1 && e.method <= 4) {
            auto reach = [&](size_t& stop) -> long {
                std::vector<uint8_t> out;
                Unarj u(&a[at], e.packed);
                bool dec = e.method == 4 ? u.fast(e.size, out) : u.lz(e.size, out);
                stop = at + u.used();
                if (dec && out.size() == e.size) return crc32Arj(out.data(), out.size()) == e.crc ? (long)e.size + 1 : (long)e.size;
                return (long)out.size();
            };
            size_t stop = 0;
            long base = reach(stop);
            int fixedN = 0;
            std::vector<uint8_t> before(a.begin() + at, a.begin() + end);   // put back unless the CRC comes right
            for (int round = 0; round < 12 && base < (long)e.size && !late(); round++) {
                long best = base;
                size_t bk = 0;
                uint8_t bv = 0;
                auto tryAt = [&](size_t k, uint8_t v) {
                    uint8_t keep = a[k];
                    a[k] = v;
                    size_t s2;
                    long r = reach(s2);
                    a[k] = keep;
                    if (r > best) { best = r; bk = k; bv = v; }
                };
                if (alts)
                    for (size_t k = at; k < end && k < alts->size() && k <= stop + 2 && !late(); k++)
                        for (uint8_t v : (*alts)[k]) tryAt(k, v);
                size_t from = stop > at + 600 ? stop - 600 : at;
                size_t top = std::min(stop + 2, end - 1);
                for (size_t k = top; k >= from && !late(); k--) {
                    for (int b = 0; b < 8; b++) tryAt(k, (uint8_t)(a[k] ^ (1 << b)));
                    if (k == from) break;
                }
                if (best <= base) break;
                a[bk] = bv;
                fixedN++;
                base = reach(stop);
            }
            if (base == (long)e.size + 1) { done = true; how = fmt("%d wrong bytes put right, step by step, by how far the data unpacks and its CRC", fixedN); }
            else std::copy(before.begin(), before.end(), a.begin() + at);
        }
        if (done) { mended++; log += e.name + ": " + how + "\n"; }
    }
    return mended;
}

std::string arjExtract(const ArjCheck& c, const std::string& dir)
{
    std::string list;
    int n = 0;
    bool made = false;
    for (auto& e : c.files) {
        n++;
        if (e.data.empty()) {
            list += (e.name.empty() ? "?" : e.name) + "  nothing unpacked (" + e.why + ")\n";
            continue;
        }
        std::string nm;
        for (unsigned char ch : e.name) nm += ch < 32 || ch > 126 || strchr("\\/:*?\"<>|", ch) ? '_' : (char)ch;
        if (nm.empty() || nm == "?") nm = "file" + std::to_string(n);
        if (!made) { make_dirs(dir); made = true; }
        write_file(path_join(dir, nm), e.data);
        char b[160];
        if (e.ok) snprintf(b, sizeof b, "%u bytes, whole (CRC right)", e.size);
        else if (e.unknown) snprintf(b, sizeof b, "%u of %u bytes, %u of them lost (\"?\" in a text, zero otherwise)", e.got, e.size, e.unknown);
        else if (e.got >= e.size) snprintf(b, sizeof b, "%u bytes, CRC wrong (bytes damaged inside)", e.got);
        else snprintf(b, sizeof b, "%u of %u bytes (unpacking stopped at the damage)", e.got, e.size);
        list += nm + "  " + b + (e.ok || e.why.empty() ? "" : "; " + e.why) + "\n";
    }
    return list;
}

std::vector<uint8_t> combineCopies(const std::vector<std::vector<uint8_t>>& C, std::string& log)
{
    char b[200];
    if (C.empty()) return {};
    const size_t n = C[0].size();
    for (auto& c : C) if (c.size() != n) { log += "the copies are not of one size\n"; return {}; }
    std::vector<uint8_t> M(n, 0);
    std::vector<size_t> diff, single;
    std::vector<std::vector<uint8_t>> cand(n);
    size_t filled = 0;
    for (size_t i = 0; i < n; i++) {
        int cnt[256] = {0}, read = 0;
        for (auto& c : C) if (c[i]) { cnt[c[i]]++; read++; }
        if (!read) continue;
        int best = 0;
        for (int v = 1; v < 256; v++) {
            if (cnt[v]) cand[i].push_back((uint8_t)v);
            if (cnt[v] > cnt[best]) best = v;
        }
        M[i] = (uint8_t)best;
        if (C[0][i] == 0) filled++;
        if (cand[i].size() > 1) diff.push_back(i);
        else if (read == 1) single.push_back(i);
    }
    snprintf(b, sizeof b, "%zu copies of %zu bytes: %zu bytes the first did not read taken from the others, %zu read differently\n",
             C.size(), n, filled, diff.size());
    log += b;
    if (n < 4 || M[0] != 0x60 || M[1] != 0xEA) return M;   // not an ARJ archive: the vote is all there is

    auto score = [&](const std::vector<uint8_t>& d) {
        ArjCheck k = arjCheck(d);
        long s = k.headerOk ? 500000 : 0;
        for (auto& e : k.files) s += (e.ok ? 1000000 : (long)e.got - (long)e.unknown) + (e.why.find("header CRC") == std::string::npos ? 100000 : 0);
        return s;
    };
    log += "ARJ as voted: " + arjCheck(M).summary() + "\n";
    // inside each file of the archive (one header 60 EA to the next): every combination of the differing bytes
    std::vector<size_t> hdr;
    for (size_t i = 0; i + 1 < n; i++) if (M[i] == 0x60 && M[i + 1] == 0xEA) hdr.push_back(i);
    hdr.push_back(n);
    for (size_t h = 0; h + 1 < hdr.size(); h++) {
        std::vector<size_t> in;
        for (size_t i : diff) if (i >= hdr[h] && i < hdr[h + 1]) in.push_back(i);
        if (in.empty()) continue;
        double combos = 1;
        for (size_t i : in) combos *= cand[i].size();
        long best = score(M);
        std::vector<uint8_t> bestM = M;
        if (combos <= (1 << 16)) {
            std::vector<size_t> pick(in.size(), 0);
            for (;;) {
                for (size_t k = 0; k < in.size(); k++) M[in[k]] = cand[in[k]][pick[k]];
                long s = score(M);
                if (s > best) { best = s; bestM = M; }
                size_t k = 0;
                while (k < in.size() && ++pick[k] == cand[in[k]].size()) pick[k++] = 0;
                if (k == in.size()) break;
            }
            M = bestM;
        } else   // too many: one byte at a time
            for (size_t i : in)
                for (uint8_t v : cand[i]) {
                    uint8_t keep = M[i];
                    M[i] = v;
                    long s = score(M);
                    if (s > best) best = s; else M[i] = keep;
                }
        snprintf(b, sizeof b, "bytes %zu-%zu: %zu read differently, %.0f combinations tried\n", hdr[h], hdr[h + 1], in.size(), std::min(combos, 65536.0));
        log += b;
    }
    // a byte only one copy read may be wrong in it: all 256 values, while a file does not check yet
    long s0 = score(M);
    for (int pass = 0; pass < 3; pass++) {
        bool changed = false;
        for (size_t i : single) {
            ArjCheck k = arjCheck(M);
            if (k.good() == (int)k.files.size() && k.headerOk && !k.files.empty()) break;
            uint8_t keep = M[i], bv = keep;
            long bs = s0;
            for (int v = 0; v < 256; v++) {
                if (v == keep) continue;
                M[i] = (uint8_t)v;
                long s = score(M);
                if (s > bs) { bs = s; bv = (uint8_t)v; }
            }
            M[i] = bv;
            if (bs > s0) {
                s0 = bs;
                changed = true;
                snprintf(b, sizeof b, "byte %zu read by one copy only: %02X -> %02X\n", i, keep, bv);
                log += b;
            }
        }
        if (!changed) break;
    }
    log += "combined: " + arjCheck(M).summary() + "\n";
    return M;
}

std::string ArjCheck::summary() const
{
    if (!archive) return "not an ARJ archive (its start did not come through)";
    char b[200];
    snprintf(b, sizeof b, "ARJ: %d of %zu files right (CRC)%s%s", good(), files.size(), headerOk ? "" : ", archive header damaged",
             complete ? "" : ", end of the archive not found");
    std::string s = b;
    if (startLost) s += ", its start did not come (files from the first header that checks)";
    if (good() == (int)files.size() && headerOk && complete && !files.empty()) s = "ARJ: the whole archive checks (" + std::to_string(files.size()) + " files, CRC right)";
    return s;
}
