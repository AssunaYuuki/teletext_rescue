#include "rar.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

namespace {

uint32_t crc32Of(const uint8_t* p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

void putUtf8(std::string& s, uint32_t c)
{
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | (c >> 6 & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

uint32_t cp866(uint8_t c)
{
    static const uint16_t hi[128] = {
        0x410, 0x411, 0x412, 0x413, 0x414, 0x415, 0x416, 0x417, 0x418, 0x419, 0x41A, 0x41B, 0x41C, 0x41D, 0x41E, 0x41F,
        0x420, 0x421, 0x422, 0x423, 0x424, 0x425, 0x426, 0x427, 0x428, 0x429, 0x42A, 0x42B, 0x42C, 0x42D, 0x42E, 0x42F,
        0x430, 0x431, 0x432, 0x433, 0x434, 0x435, 0x436, 0x437, 0x438, 0x439, 0x43A, 0x43B, 0x43C, 0x43D, 0x43E, 0x43F,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x440, 0x441, 0x442, 0x443, 0x444, 0x445, 0x446, 0x447, 0x448, 0x449, 0x44A, 0x44B, 0x44C, 0x44D, 0x44E, 0x44F,
        0x401, 0x451, 0x404, 0x454, 0x407, 0x457, 0x40E, 0x45E, 0xB0, 0x2219, 0xB7, 0x221A, 0x2116, 0xA4, 0x25A0, 0xA0};
    return c < 0x80 ? c : hi[c - 0x80];
}

// ---------------------------------------------------------------- bits, MSB first
struct Bits {
    const uint8_t* d = nullptr;
    size_t n = 0, pos = 0;   // pos in bits
    uint32_t get16() const   // the next 16 bits
    {
        size_t b = pos >> 3;
        uint32_t v = (uint32_t)(b < n ? d[b] : 0) << 16 | (uint32_t)(b + 1 < n ? d[b + 1] : 0) << 8 | (b + 2 < n ? d[b + 2] : 0);
        return (v >> (8 - (pos & 7))) & 0xFFFF;
    }
    void add(int k) { pos += k; }
    bool over() const { return (pos >> 3) > n + 4; }
    void align() { pos = (pos + 7) & ~(size_t)7; }
    int byte()   // a whole byte (aligned reading of the PPM coder)
    {
        size_t b = pos >> 3;
        pos += 8;
        return b < n ? d[b] : 0;
    }
};

// ---------------------------------------------------------------- Huffman tables (canonical, lengths 1-15)
struct Huff {
    uint32_t len[16] = {}, pos[16] = {};
    std::vector<uint16_t> num;
    void make(const uint8_t* lengths, int size)
    {
        int count[16] = {}, tmp[16];
        num.assign(size, 0);
        for (int i = 0; i < size; i++) count[lengths[i] & 15]++;
        count[0] = 0;
        len[0] = pos[0] = 0;
        tmp[0] = 0;
        long n = 0;
        for (int i = 1; i < 16; i++) {
            n = 2 * (n + count[i]);
            long m = n << (15 - i);
            if (m > 0xFFFF) m = 0xFFFF;
            len[i] = (uint32_t)m;
            pos[i] = pos[i - 1] + count[i - 1];
            tmp[i] = pos[i];
        }
        for (int i = 0; i < size; i++)
            if (lengths[i] & 15) num[tmp[lengths[i] & 15]++] = (uint16_t)i;
    }
    int decode(Bits& b) const
    {
        uint32_t f = b.get16() & 0xFFFE;
        int k = 1;
        while (k < 15 && f >= len[k]) k++;
        b.add(k);
        uint32_t i = pos[k] + ((f - len[k - 1]) >> (16 - k));
        if (i >= num.size()) i = 0;
        return num[i];
    }
};

// ---------------------------------------------------------------- PPMd var. H with the RAR range coder
class Ppm {
public:
    bool init(Bits& in, int& escChar);
    int decodeChar();   // -1 at an error

private:
    struct State { uint8_t sym, freq; uint32_t succ; };   // successor: offset in the heap
    struct See { uint16_t summ; uint8_t shift, count; };
    // heap layout of a context (12 bytes as in PPMd: numStats, summFreq, stats / oneState, suffix)
    static constexpr int kUnit = 12;
    std::vector<uint8_t> heap_;
    uint32_t size_ = 0, lo_ = 0, hi_ = 0, text_ = 0, unitsStart_ = 0, heapEnd_ = 0;
    int glue_ = 0;
    uint32_t freeList_[38] = {};
    uint8_t indx2Units_[38], units2Indx_[128];
    // model
    uint32_t minCtx_ = 0, maxCtx_ = 0, foundState_ = 0;
    int numMasked_ = 0, initEsc_ = 0, orderFall_ = 0, maxOrder_ = 0, runLength_ = 0, initRl_ = 0;
    uint8_t charMask_[256], ns2Indx_[256], ns2BsIndx_[256], hb2Flag_[256];
    uint8_t escCount_ = 0, prevSuccess_ = 0, hiBitsFlag_ = 0;
    uint16_t binSumm_[128][64];
    See see2_[25][16], dummySee_;
    // range coder
    Bits* in_ = nullptr;
    uint32_t low_ = 0, code_ = 0, range_ = 0;
    uint32_t cLow_ = 0, cHigh_ = 0, cScale_ = 0;

    // memory helpers
    uint8_t* P(uint32_t off) { return heap_.data() + off; }
    uint16_t& numStats(uint32_t c) { return *(uint16_t*)P(c); }
    uint16_t& summFreq(uint32_t c) { return *(uint16_t*)P(c + 2); }
    uint32_t& stats(uint32_t c) { return *(uint32_t*)P(c + 4); }
    uint32_t& suffix(uint32_t c) { return *(uint32_t*)P(c + 8); }
    uint32_t oneState(uint32_t c) { return c + 2; }   // a State in place of summFreq + stats
    uint8_t& sym(uint32_t s) { return *P(s); }
    uint8_t& freq(uint32_t s) { return *P(s + 1); }
    uint32_t succ(uint32_t s) { uint32_t v; memcpy(&v, P(s + 2), 4); return v; }
    void setSucc(uint32_t s, uint32_t v) { memcpy(P(s + 2), &v, 4); }
    static constexpr int kState = 6;

    // sub-allocator (units of 12 bytes)
    void initAlloc();
    uint32_t allocUnits(int nu);
    uint32_t allocContext();
    uint32_t expandUnits(uint32_t p, int oldNu);
    uint32_t shrinkUnits(uint32_t p, int oldNu, int newNu);
    void freeUnits(uint32_t p, int nu);
    uint32_t allocUnitsRare(int indx);
    void glueFree();
    void insertNode(uint32_t p, int indx) { *(uint32_t*)P(p + 4) = freeList_[indx]; *(uint32_t*)P(p) = 0xFFFFFFFF; freeList_[indx] = p; }
    uint32_t removeNode(int indx) { uint32_t p = freeList_[indx]; freeList_[indx] = *(uint32_t*)P(p + 4); return p; }
    void splitBlock(uint32_t p, int oldIndx, int newIndx);

    void restartModel();
    void startModel(int maxOrder);
    uint32_t createSuccessors(bool skip, uint32_t p1);
    void updateModel();
    void rescale();
    void update1(uint32_t p);
    void update2(uint32_t p);
    void decodeBin();
    bool decodeSymbol1();
    bool decodeSymbol2();
    See* makeEscFreq2(int diff, uint32_t& escFreq);
    void clearMask() { escCount_ = 1; memset(charMask_, 0, sizeof charMask_); }

    // range decoder of RAR (carry-less, Subbotin)
    void rcInit() { low_ = code_ = 0; range_ = 0xFFFFFFFFu; for (int i = 0; i < 4; i++) code_ = code_ << 8 | in_->byte(); }
    uint32_t getFreq(uint32_t total) { range_ /= total; return (code_ - low_) / range_; }
    uint32_t getBits(int k) { range_ >>= k; return (code_ - low_) / range_; }
    void decode() { low_ += range_ * cLow_; range_ *= cHigh_ - cLow_; }
    void normalize()
    {
        const uint32_t top = 1u << 24, bot = 1u << 15;
        for (;;) {
            if ((low_ ^ (low_ + range_)) >= top) {
                if (range_ >= bot) break;
                range_ = (0u - low_) & (bot - 1);
            }
            code_ = code_ << 8 | in_->byte();
            range_ <<= 8;
            low_ <<= 8;
        }
    }
};

const uint16_t kInitBinEsc[8] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
const uint8_t kExpEscape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};

void Ppm::initAlloc()
{
    int i = 0, k = 1;
    for (; i < 4; i++, k += 1) indx2Units_[i] = (uint8_t)k;
    for (k++; i < 8; i++, k += 2) indx2Units_[i] = (uint8_t)k;
    for (k++; i < 12; i++, k += 3) indx2Units_[i] = (uint8_t)k;
    for (k++; i < 38; i++, k += 4) indx2Units_[i] = (uint8_t)k;
    for (k = 0, i = 0; k < 128; k++) {
        i += indx2Units_[i] < k + 1;
        units2Indx_[k] = (uint8_t)i;
    }
}

void Ppm::restartModel()
{
    memset(charMask_, 0, sizeof charMask_);
    memset(freeList_, 0, sizeof freeList_);
    // the heap (InitSubAllocator): offset 0 unused (a null pointer); text from the start, units after it
    text_ = kUnit;
    uint32_t size2 = kUnit * (size_ / 8 / kUnit * 7), size1 = size_ - size2;
    lo_ = unitsStart_ = kUnit + size1;
    hi_ = lo_ + size2;
    glue_ = 0;
    orderFall_ = maxOrder_;
    runLength_ = initRl_ = -(maxOrder_ < 12 ? maxOrder_ : 12) - 1;
    prevSuccess_ = 0;
    minCtx_ = maxCtx_ = allocContext();
    suffix(minCtx_) = 0;
    numStats(minCtx_) = 256;
    summFreq(minCtx_) = 256 + 1;
    uint32_t s = allocUnits(256 / 2);
    stats(minCtx_) = s;
    foundState_ = s;
    for (int i = 0; i < 256; i++) {
        uint32_t p = s + i * kState;
        sym(p) = (uint8_t)i;
        freq(p) = 1;
        setSucc(p, 0);
    }
    for (int i = 0; i < 128; i++)
        for (int k = 0; k < 8; k++)
            for (int m = 0; m < 64; m += 8) binSumm_[i][k + m] = (uint16_t)(0x4000 - kInitBinEsc[k] / (i + 2));
    for (int i = 0; i < 25; i++)
        for (int k = 0; k < 16; k++) { see2_[i][k].summ = (uint16_t)((5 * i + 10) << 3); see2_[i][k].shift = 3; see2_[i][k].count = 4; }
}

void Ppm::startModel(int maxOrder)
{
    escCount_ = 1;
    maxOrder_ = maxOrder;
    restartModel();
    ns2BsIndx_[0] = 0;
    ns2BsIndx_[1] = 2;
    memset(ns2BsIndx_ + 2, 4, 9);
    memset(ns2BsIndx_ + 11, 6, 256 - 11);
    for (int i = 0; i < 3; i++) ns2Indx_[i] = (uint8_t)i;
    for (int m = 3, i = 3, k = 1, step = 1; i < 256; i++) {
        ns2Indx_[i] = (uint8_t)m;
        if (!--k) { k = ++step; m++; }
    }
    memset(hb2Flag_, 0, 0x40);
    memset(hb2Flag_ + 0x40, 8, 0x100 - 0x40);
    dummySee_.shift = 7;
    dummySee_.summ = 0;
    dummySee_.count = 64;
}

void Ppm::splitBlock(uint32_t p, int oldIndx, int newIndx)
{
    int diff = indx2Units_[oldIndx] - indx2Units_[newIndx];
    p += indx2Units_[newIndx] * kUnit;
    int i = units2Indx_[diff - 1];
    if (indx2Units_[i] != diff) {
        int k = indx2Units_[--i];
        insertNode(p, i);
        p += k * kUnit;
        diff -= k;
    }
    insertNode(p, units2Indx_[diff - 1]);
}

void Ppm::glueFree()
{
    // GlueFreeBlocks of PPMd: the free blocks into one list (each put at its head), joined with free blocks right
    // after them, then given back to the free lists in that order - the order decides later allocations
    struct Blk { uint32_t p, nu; };
    std::vector<Blk> L;   // the list, head first
    std::map<uint32_t, uint32_t> freeAt;   // block -> units, for the blocks in the list
    for (int i = 0; i < 38; i++)
        while (freeList_[i]) {
            uint32_t p = removeNode(i);
            L.insert(L.begin(), {p, indx2Units_[i]});
            freeAt[p] = indx2Units_[i];
        }
    if (lo_ != hi_) freeAt.erase(lo_);
    for (size_t k = 0; k < L.size(); k++) {
        if (!freeAt.count(L[k].p)) continue;
        for (;;) {
            uint32_t q = L[k].p + L[k].nu * kUnit;
            auto it = freeAt.find(q);
            if (it == freeAt.end() || L[k].nu + it->second >= 0x10000 || q == L[k].p) break;
            L[k].nu += it->second;
            freeAt.erase(it);
            for (size_t j = 0; j < L.size(); j++) if (L[j].p == q && j != k) { L.erase(L.begin() + j); if (j < k) k--; break; }
        }
        freeAt[L[k].p] = L[k].nu;
    }
    for (auto& b : L) {
        uint32_t p = b.p, nu = b.nu;
        for (; nu > 128; nu -= 128, p += 128 * kUnit) insertNode(p, 37);
        int i = units2Indx_[nu - 1];
        if (indx2Units_[i] != (int)nu) {
            int k = nu - indx2Units_[--i];
            insertNode(p + (nu - k) * kUnit, k - 1);
        }
        insertNode(p, i);
    }
}

uint32_t Ppm::allocUnitsRare(int indx)
{
    if (!glue_) {
        glue_ = 255;
        glueFree();
        if (freeList_[indx]) return removeNode(indx);
    }
    int i = indx;
    do {
        if (++i == 38) {
            int nu = indx2Units_[indx] * kUnit;
            glue_--;
            if ((int)(unitsStart_ - text_) > nu) { unitsStart_ -= nu; return unitsStart_; }
            return 0;
        }
    } while (!freeList_[i]);
    uint32_t p = removeNode(i);
    splitBlock(p, i, indx);
    return p;
}

uint32_t Ppm::allocUnits(int nu)
{
    int indx = units2Indx_[nu - 1];
    if (freeList_[indx]) return removeNode(indx);
    uint32_t p = lo_;
    lo_ += indx2Units_[indx] * kUnit;
    if (lo_ <= hi_) return p;
    lo_ -= indx2Units_[indx] * kUnit;
    return allocUnitsRare(indx);
}

uint32_t Ppm::allocContext()
{
    if (hi_ != lo_) return hi_ -= kUnit;
    if (freeList_[0]) return removeNode(0);
    return allocUnitsRare(0);
}

uint32_t Ppm::expandUnits(uint32_t p, int oldNu)
{
    int i0 = units2Indx_[oldNu - 1], i1 = units2Indx_[oldNu];
    if (i0 == i1) return p;
    uint32_t q = allocUnits(oldNu + 1);
    if (q) {
        memmove(P(q), P(p), oldNu * kUnit);
        insertNode(p, i0);
    }
    return q;
}

uint32_t Ppm::shrinkUnits(uint32_t p, int oldNu, int newNu)
{
    int i0 = units2Indx_[oldNu - 1], i1 = units2Indx_[newNu - 1];
    if (i0 == i1) return p;
    if (freeList_[i1]) {
        uint32_t q = removeNode(i1);
        memmove(P(q), P(p), newNu * kUnit);
        insertNode(p, i0);
        return q;
    }
    splitBlock(p, i0, i1);
    return p;
}

void Ppm::freeUnits(uint32_t p, int nu) { insertNode(p, units2Indx_[nu - 1]); }

bool Ppm::init(Bits& in, int& escChar)
{
    in_ = &in;
    int maxOrder = in.byte();
    bool reset = maxOrder & 0x20;
    int maxMb = 0;
    if (reset) maxMb = in.byte();
    else if (heap_.empty()) return false;
    if (maxOrder & 0x40) escChar = in.byte();
    rcInit();
    if (reset) {
        maxOrder = (maxOrder & 0x1F) + 1;
        if (maxOrder > 16) maxOrder = 16 + (maxOrder - 16) * 3;
        if (maxOrder == 1) { heap_.clear(); return false; }
        size_ = (uint32_t)(maxMb + 1) << 20;
        heap_.assign(size_ + 2 * kUnit, 0);
        heapEnd_ = kUnit + size_;
        initAlloc();
        startModel(maxOrder);
    }
    return minCtx_ != 0;
}

void Ppm::rescale()
{
    int oldNs = numStats(minCtx_), i = oldNs - 1, adder, escFreq;
    uint32_t st = stats(minCtx_);
    uint32_t p = foundState_;
    for (; p != st; p -= kState) std::swap_ranges(P(p), P(p) + kState, P(p - kState));
    freq(st) += 4;
    summFreq(minCtx_) += 4;
    escFreq = summFreq(minCtx_) - freq(st);
    adder = orderFall_ != 0;
    freq(st) = (uint8_t)((freq(st) + adder) >> 1);
    summFreq(minCtx_) = freq(st);
    p = st;
    do {
        p += kState;
        escFreq -= freq(p);
        freq(p) = (uint8_t)((freq(p) + adder) >> 1);
        summFreq(minCtx_) += freq(p);
        if (freq(p) > freq(p - kState)) {
            uint8_t tmp[kState];
            memcpy(tmp, P(p), kState);
            uint32_t q = p;
            do { memcpy(P(q), P(q - kState), kState); q -= kState; } while (q != st && tmp[1] > freq(q - kState));
            memcpy(P(q), tmp, kState);
        }
    } while (--i);
    if (freq(p) == 0) {
        int nz = 0;
        do { nz++; p -= kState; } while (freq(p) == 0);
        escFreq += nz;
        numStats(minCtx_) -= nz;
        if (numStats(minCtx_) == 1) {
            uint8_t tmp[kState];
            memcpy(tmp, P(st), kState);
            do { tmp[1] -= tmp[1] >> 1; escFreq >>= 1; } while (escFreq > 1);
            freeUnits(st, (oldNs + 1) >> 1);
            memcpy(P(oneState(minCtx_)), tmp, kState);
            foundState_ = oneState(minCtx_);
            return;
        }
    }
    summFreq(minCtx_) += escFreq - (escFreq >> 1);
    int n0 = (oldNs + 1) >> 1, n1 = (numStats(minCtx_) + 1) >> 1;
    if (n0 != n1) stats(minCtx_) = shrinkUnits(st, n0, n1);
    foundState_ = stats(minCtx_);
}

uint32_t Ppm::createSuccessors(bool skip, uint32_t p1)
{
    uint32_t pc = minCtx_, upBranch = succ(foundState_);
    uint32_t ps[64];
    int n = 0;
    if (!skip) {
        ps[n++] = foundState_;
        if (!suffix(pc)) goto noLoop;
    }
    if (p1) {
        pc = suffix(pc);
        goto loopEntry;
    }
    do {
        pc = suffix(pc);
        if (numStats(pc) != 1) {
            p1 = stats(pc);
            while (sym(p1) != sym(foundState_)) p1 += kState;
        } else
            p1 = oneState(pc);
    loopEntry:
        if (succ(p1) != upBranch) { pc = succ(p1); break; }
        if (n >= 64) return 0;
        ps[n++] = p1;
    } while (suffix(pc));
noLoop:
    if (n == 0) return pc;
    uint8_t upSym = *P(upBranch), upFreq;
    uint32_t upSucc = upBranch + 1;
    if (numStats(pc) != 1) {
        if (pc <= text_) return 0;
        uint32_t q = stats(pc);
        while (sym(q) != upSym) q += kState;
        uint32_t cf = freq(q) - 1, s0 = summFreq(pc) - numStats(pc) - cf;
        upFreq = (uint8_t)(1 + (2 * cf <= s0 ? (5 * cf > s0) : ((2 * cf + 3 * s0 - 1) / (2 * s0))));
    } else
        upFreq = freq(oneState(pc));
    do {
        uint32_t c = allocContext();
        if (!c) return 0;
        numStats(c) = 1;
        uint32_t os = oneState(c);
        sym(os) = upSym;
        freq(os) = upFreq;
        setSucc(os, upSucc);
        suffix(c) = pc;
        setSucc(ps[--n], c);
        pc = c;
    } while (n);
    return pc;
}

void Ppm::updateModel()
{
    uint8_t fSym = sym(foundState_), fFreq = freq(foundState_);
    uint32_t fSucc = succ(foundState_);
    uint32_t p = 0;
    if (fFreq < 124 / 4 && suffix(minCtx_)) {
        uint32_t pc = suffix(minCtx_);
        if (numStats(pc) != 1) {
            p = stats(pc);
            if (sym(p) != fSym) {
                do p += kState; while (sym(p) != fSym);
                if (freq(p) >= freq(p - kState)) {
                    std::swap_ranges(P(p), P(p) + kState, P(p - kState));
                    p -= kState;
                }
            }
            if (freq(p) < 124 - 9) { freq(p) += 2; summFreq(pc) += 2; }
        } else {
            p = oneState(pc);
            if (freq(p) < 32) freq(p)++;
        }
    }
    if (!orderFall_) {
        minCtx_ = maxCtx_ = createSuccessors(true, p);
        setSucc(foundState_, minCtx_);
        if (!minCtx_) { restartModel(); escCount_ = 0; return; }
        return;
    }
    *P(text_++) = fSym;
    uint32_t successor = text_;
    if (text_ >= unitsStart_) { restartModel(); escCount_ = 0; return; }
    if (fSucc) {
        if (fSucc <= text_) {   // points into the text: make the contexts
            uint32_t cs = createSuccessors(false, p);
            if (!cs) { restartModel(); escCount_ = 0; return; }
            fSucc = cs;
        }
        if (!--orderFall_) {
            successor = fSucc;
            text_ -= (maxCtx_ != minCtx_);
        }
    } else {
        setSucc(foundState_, successor);
        fSucc = minCtx_;
    }
    int ns = numStats(minCtx_);
    uint32_t s0 = summFreq(minCtx_) - ns - (fFreq - 1);
    for (uint32_t pc = maxCtx_; pc != minCtx_; pc = suffix(pc)) {
        int ns1 = numStats(pc);
        if (ns1 != 1) {
            if ((ns1 & 1) == 0) {
                uint32_t st = expandUnits(stats(pc), ns1 >> 1);
                if (!st) { restartModel(); escCount_ = 0; return; }
                stats(pc) = st;
            }
            summFreq(pc) += (2 * ns1 < ns) + 2 * ((4 * ns1 <= ns) & (summFreq(pc) <= 8 * ns1));
        } else {
            uint32_t st = allocUnits(1);
            if (!st) { restartModel(); escCount_ = 0; return; }
            memcpy(P(st), P(oneState(pc)), kState);
            stats(pc) = st;
            if (freq(st) < 124 / 4 - 1) freq(st) <<= 1;
            else freq(st) = 124 - 4;
            summFreq(pc) = (uint16_t)(freq(st) + initEsc_ + (ns > 3));
        }
        uint32_t cf = 2 * fFreq * (summFreq(pc) + 6), sf = s0 + summFreq(pc);
        if (cf < 6 * sf) {
            cf = 1 + (cf > sf) + (cf >= 4 * sf);
            summFreq(pc) += 3;
        } else {
            cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
            summFreq(pc) += cf;
        }
        uint32_t q = stats(pc) + ns1 * kState;
        setSucc(q, successor);
        sym(q) = fSym;
        freq(q) = (uint8_t)cf;
        numStats(pc) = (uint16_t)(ns1 + 1);
    }
    maxCtx_ = minCtx_ = fSucc;
}

void Ppm::update1(uint32_t p)
{
    foundState_ = p;
    freq(p) += 4;
    summFreq(minCtx_) += 4;
    if (freq(p) > freq(p - kState)) {
        std::swap_ranges(P(p), P(p) + kState, P(p - kState));
        foundState_ = p - kState;
        if (freq(p - kState) > 124) rescale();
    }
}

void Ppm::update2(uint32_t p)
{
    foundState_ = p;
    freq(p) += 4;
    summFreq(minCtx_) += 4;
    if (freq(p) > 124) rescale();
    escCount_++;
    runLength_ = initRl_;
}

Ppm::See* Ppm::makeEscFreq2(int diff, uint32_t& escFreq)
{
    int ns = numStats(minCtx_);
    if (ns != 256) {
        See* s = &see2_[ns2Indx_[diff - 1]][(diff < numStats(suffix(minCtx_)) - ns) + 2 * (summFreq(minCtx_) < 11 * ns) + 4 * (numMasked_ > diff) +
                                          hiBitsFlag_];
        uint32_t r = s->summ >> s->shift;
        s->summ -= (uint16_t)r;
        escFreq = r + (r == 0);
        return s;
    }
    escFreq = 1;
    return &dummySee_;
}

bool Ppm::decodeSymbol1()
{
    uint32_t p = stats(minCtx_);
    cScale_ = summFreq(minCtx_);
    uint32_t count = getFreq(cScale_);
    if (count >= cScale_) return false;
    uint32_t hiCnt = freq(p);
    if (count < hiCnt) {
        cHigh_ = hiCnt;
        cLow_ = 0;
        prevSuccess_ = 2 * hiCnt > cScale_;
        runLength_ += prevSuccess_;
        foundState_ = p;
        freq(p) = (uint8_t)(hiCnt + 4);
        summFreq(minCtx_) += 4;
        if (hiCnt + 4 > 124) rescale();
        return true;
    }
    prevSuccess_ = 0;
    int i = numStats(minCtx_) - 1;
    while ((hiCnt += freq(p += kState)) <= count)
        if (--i == 0) {
            hiBitsFlag_ = hb2Flag_[sym(foundState_)];
            cLow_ = hiCnt;
            charMask_[sym(p)] = escCount_;
            i = (numMasked_ = numStats(minCtx_)) - 1;
            foundState_ = 0;
            do charMask_[sym(p -= kState)] = escCount_; while (--i);
            cHigh_ = cScale_;
            return true;
        }
    cLow_ = hiCnt - freq(p);
    cHigh_ = hiCnt;
    update1(p);
    return true;
}

void Ppm::decodeBin()
{
    uint32_t rs = oneState(minCtx_);
    hiBitsFlag_ = hb2Flag_[sym(foundState_)];
    uint16_t& bs = binSumm_[freq(rs) - 1][prevSuccess_ + ns2BsIndx_[numStats(suffix(minCtx_)) - 1] + hiBitsFlag_ + 2 * hb2Flag_[sym(rs)] +
                                          ((runLength_ >> 26) & 0x20)];
    if (getBits(14) < bs) {
        foundState_ = rs;
        freq(rs) += (freq(rs) < 128);
        cLow_ = 0;
        cHigh_ = bs;
        bs = (uint16_t)(bs + 128 - ((bs + 32) >> 7));   // GET_MEAN(bs, 7, 2): INTERVAL 128, PERIOD_BITS 7
        prevSuccess_ = 1;
        runLength_++;
    } else {
        cLow_ = bs;
        bs = (uint16_t)(bs - ((bs + 32) >> 7));
        cHigh_ = 1 << 14;
        initEsc_ = kExpEscape[bs >> 10];
        numMasked_ = 1;
        charMask_[sym(rs)] = escCount_;
        prevSuccess_ = 0;
        foundState_ = 0;
    }
}

bool Ppm::decodeSymbol2()
{
    int diff = numStats(minCtx_) - numMasked_;
    uint32_t escFreq;
    See* s = makeEscFreq2(diff, escFreq);
    uint32_t ps[256], hiCnt = 0;
    uint32_t p = stats(minCtx_) - kState;
    int i = diff, n = 0;
    do {
        do p += kState; while (charMask_[sym(p)] == escCount_);
        hiCnt += freq(p);
        ps[n++] = p;
    } while (--i);
    cScale_ = escFreq + hiCnt;
    uint32_t count = getFreq(cScale_);
    if (count >= cScale_) return false;
    if (count < hiCnt) {
        int k = 0;
        hiCnt = 0;
        while ((hiCnt += freq(ps[k])) <= count) k++;
        p = ps[k];
        cLow_ = hiCnt - freq(p);
        cHigh_ = hiCnt;
        // SEE update
        if (s->shift < 7 && --s->count == 0) { s->summ += s->summ; s->count = (uint8_t)(3 << s->shift++); }
        update2(p);
    } else {
        cLow_ = hiCnt;
        cHigh_ = cScale_;
        i = diff;
        for (int k = 0; k < n; k++) charMask_[sym(ps[k])] = escCount_;
        s->summ += (uint16_t)cScale_;
        numMasked_ = numStats(minCtx_);
    }
    return true;
}

int Ppm::decodeChar()
{
    if (minCtx_ <= text_ || minCtx_ > heapEnd_) return -1;
    if (numStats(minCtx_) != 1) {
        if (stats(minCtx_) <= text_ || stats(minCtx_) > heapEnd_) return -1;
        if (!decodeSymbol1()) return -1;
    } else
        decodeBin();
    decode();
    while (!foundState_) {
        normalize();
        do {
            orderFall_++;
            minCtx_ = suffix(minCtx_);
            if (minCtx_ <= text_ || minCtx_ > heapEnd_) return -1;
        } while (numStats(minCtx_) == numMasked_);
        if (!decodeSymbol2()) return -1;
        decode();
    }
    int c = sym(foundState_);
    if (!orderFall_ && succ(foundState_) > text_) minCtx_ = maxCtx_ = succ(foundState_);
    else {
        updateModel();
        if (escCount_ == 0) clearMask();
    }
    normalize();
    return c;
}

// ---------------------------------------------------------------- the LZ unpacker of RAR 2.9
const uint8_t kLBits[28] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
const uint8_t kLDecode[28] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224};
const uint8_t kSDDecode[8] = {0, 4, 8, 16, 32, 64, 128, 192};
const uint8_t kSDBits[8] = {2, 2, 3, 4, 5, 6, 6, 6};

enum FilterType { kNone, kE8, kE8E9, kItanium, kDelta, kRgb, kAudio, kUpcase };

struct Filter {
    FilterType type = kNone;
    uint64_t start = 0;   // in the window (absolute)
    uint32_t length = 0;
    uint32_t r[7] = {};
};

class Unpack29 {
public:
    std::vector<uint8_t> win;   // all that was unpacked (one solid stream)
    bool unpack(const uint8_t* data, size_t n, uint64_t size, bool solid, std::string& err);
    std::vector<Filter> pending;   // filters to apply to the output of this file

private:
    Bits in_;
    Huff ld_, dd_, ldd_, rd_, bd_;
    uint8_t oldTable_[404] = {};
    bool tablesRead_ = false, ppmBlock_ = false;
    uint32_t oldDist_[4] = {}, lastLength_ = 0, prevLowDist_ = 0;
    int lowDistRep_ = 0;
    uint32_t dDecode_[60];
    uint8_t dBits_[60];
    Ppm ppm_;
    int ppmEsc_ = 2;
    std::vector<FilterType> filterTypes_;
    std::vector<uint32_t> oldLengths_;
    int lastFilter_ = 0;

    bool readTables();
    bool readEndOfBlock();
    bool addVmCode(int first, const std::vector<uint8_t>& code);
    void copy(uint32_t length, uint32_t dist)
    {
        size_t from = win.size() - std::min<size_t>(dist, win.size());
        if (dist > win.size()) { win.insert(win.end(), length, 0); return; }
        for (uint32_t i = 0; i < length; i++) win.push_back(win[from + i]);
    }
    void insertOldDist(uint32_t d) { oldDist_[3] = oldDist_[2]; oldDist_[2] = oldDist_[1]; oldDist_[1] = oldDist_[0]; oldDist_[0] = d; }

public:
    Unpack29()
    {
        static const int counts[19] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
        for (int i = 0, slot = 0, dist = 0; i < 19; i++)
            for (int k = 0; k < counts[i]; k++, slot++, dist += 1 << i) { dDecode_[slot] = dist; dBits_[slot] = (uint8_t)i; }
    }
};

bool Unpack29::readTables()
{
    in_.align();
    uint32_t f = in_.get16();
    if (f & 0x8000) {
        ppmBlock_ = true;
        return ppm_.init(in_, ppmEsc_);
    }
    ppmBlock_ = false;
    prevLowDist_ = 0;
    lowDistRep_ = 0;
    if (!(f & 0x4000)) memset(oldTable_, 0, sizeof oldTable_);
    in_.add(2);
    uint8_t bl[20];
    for (int i = 0; i < 20; i++) {
        int l = in_.get16() >> 12;
        in_.add(4);
        if (l == 15) {
            int z = in_.get16() >> 12;
            in_.add(4);
            if (z == 0) bl[i] = 15;
            else {
                z += 2;
                while (z-- > 0 && i < 20) bl[i++] = 0;
                i--;
            }
        } else
            bl[i] = (uint8_t)l;
    }
    bd_.make(bl, 20);
    uint8_t t[404];
    for (int i = 0; i < 404;) {
        if (in_.over()) return false;
        int n = bd_.decode(in_);
        if (n < 16) { t[i] = (uint8_t)((n + oldTable_[i]) & 15); i++; }
        else if (n < 18) {
            int c;
            if (n == 16) { c = (in_.get16() >> 13) + 3; in_.add(3); }
            else { c = (in_.get16() >> 9) + 11; in_.add(7); }
            if (i == 0) return false;
            while (c-- > 0 && i < 404) { t[i] = t[i - 1]; i++; }
        } else {
            int c;
            if (n == 18) { c = (in_.get16() >> 13) + 3; in_.add(3); }
            else { c = (in_.get16() >> 9) + 11; in_.add(7); }
            while (c-- > 0 && i < 404) t[i++] = 0;
        }
    }
    tablesRead_ = true;
    ld_.make(t, 299);
    dd_.make(t + 299, 60);
    ldd_.make(t + 359, 17);
    rd_.make(t + 376, 28);
    memcpy(oldTable_, t, sizeof t);
    return true;
}

bool Unpack29::readEndOfBlock()
{
    uint32_t f = in_.get16();
    bool newTable, newFile = false;
    if (f & 0x8000) { newTable = true; in_.add(1); }
    else { newFile = true; newTable = (f & 0x4000) != 0; in_.add(2); }
    tablesRead_ = !newTable;
    if (newFile) return false;
    return readTables();
}

uint32_t vmData(Bits& b)
{
    uint32_t d = b.get16();
    switch (d & 0xC000) {
    case 0: b.add(6); return (d >> 10) & 15;
    case 0x4000:
        if ((d & 0x3C00) == 0) { b.add(14); return 0xFFFFFF00u | ((d >> 2) & 0xFF); }
        b.add(10);
        return (d >> 6) & 0xFF;
    case 0x8000: b.add(2); d = b.get16(); b.add(16); return d;
    default: {
        b.add(2);
        uint32_t v = b.get16() << 16;
        b.add(16);
        v |= b.get16();
        b.add(16);
        return v;
    }
    }
}

bool Unpack29::addVmCode(int first, const std::vector<uint8_t>& code)
{
    Bits b;
    b.d = code.data();
    b.n = code.size();
    uint32_t pos;
    if (first & 0x80) {
        pos = vmData(b);
        if (pos == 0) { filterTypes_.clear(); oldLengths_.clear(); }
        else pos--;
    } else
        pos = lastFilter_;
    if (pos > filterTypes_.size() || pos > oldLengths_.size()) return false;
    lastFilter_ = pos;
    bool isNew = pos == filterTypes_.size();
    if (isNew) {
        if (pos > 1024) return false;
        filterTypes_.push_back(kNone);
        oldLengths_.push_back(0);
    }
    Filter f;
    uint32_t start = vmData(b);
    if (first & 0x40) start += 258;
    f.start = win.size() + start;
    f.length = (first & 0x20) ? vmData(b) : (pos < oldLengths_.size() ? oldLengths_[pos] : 0);
    oldLengths_[pos] = f.length;
    f.r[4] = f.length;
    if (first & 0x10) {
        uint32_t mask = b.get16() >> 9;
        b.add(7);
        for (int i = 0; i < 7; i++)
            if (mask & (1 << i)) f.r[i] = vmData(b);
    }
    if (isNew) {
        uint32_t sz = vmData(b);
        if (sz == 0 || sz >= 0x10000) return false;
        std::vector<uint8_t> prg(sz);
        for (uint32_t i = 0; i < sz; i++) { prg[i] = (uint8_t)(b.get16() >> 8); b.add(8); }
        // the standard programs, by their length and CRC
        static const struct { uint32_t len, crc; FilterType t; } known[] = {
            {53, 0xAD576887, kE8}, {57, 0x3CD7E57E, kE8E9}, {120, 0x3769893F, kItanium}, {29, 0x0E06077D, kDelta},
            {149, 0x1C2C5DC8, kRgb}, {216, 0xBC85E701, kAudio}, {40, 0x46B9C560, kUpcase}};
        uint32_t c = crc32Of(prg.data(), prg.size());
        FilterType t = kNone;
        for (auto& k : known) if (k.len == sz && k.crc == c) t = k.t;
        filterTypes_[pos] = t;
    }
    f.type = filterTypes_[pos];
    pending.push_back(f);
    return true;
}

bool Unpack29::unpack(const uint8_t* data, size_t n, uint64_t size, bool solid, std::string& err)
{
    in_.d = data;
    in_.n = n;
    in_.pos = 0;
    pending.clear();
    if (!solid) {
        win.clear();
        tablesRead_ = false;
        memset(oldDist_, 0, sizeof oldDist_);
        lastLength_ = 0;
        memset(oldTable_, 0, sizeof oldTable_);
        ppmEsc_ = 2;
        filterTypes_.clear();
        oldLengths_.clear();
        lastFilter_ = 0;
    }
    uint64_t end = win.size() + size;
    if (!solid || !tablesRead_)
        if (!readTables()) { err = "bad tables"; return false; }
    while (win.size() < end) {
        if (in_.over()) { err = "packed data ended early"; return false; }
        if (ppmBlock_) {
            int c = ppm_.decodeChar();
            if (c < 0) { err = "PPM data error"; return false; }
            if (c == ppmEsc_) {
                int nc = ppm_.decodeChar();
                if (nc == 0) { if (!readTables()) { err = "bad tables"; return false; } continue; }
                if (nc == 2 || nc == -1) break;
                if (nc == 3) {
                    int fb = ppm_.decodeChar();
                    if (fb < 0) break;
                    int len = (fb & 7) + 1;
                    if (len == 7) len = ppm_.decodeChar() + 7;
                    else if (len == 8) { len = ppm_.decodeChar() << 8; len += ppm_.decodeChar(); }
                    std::vector<uint8_t> code(std::max(len, 0));
                    for (auto& x : code) x = (uint8_t)ppm_.decodeChar();
                    if (!addVmCode(fb, code)) { err = "bad filter"; return false; }
                    continue;
                }
                if (nc == 4) {
                    uint32_t d = 0;
                    for (int i = 0; i < 3; i++) d = (d << 8) + (uint8_t)ppm_.decodeChar();
                    uint32_t l = (uint8_t)ppm_.decodeChar();
                    copy(l + 32, d + 2);
                    continue;
                }
                if (nc == 5) {
                    uint32_t l = (uint8_t)ppm_.decodeChar();
                    copy(l + 4, 1);
                    continue;
                }
            }
            win.push_back((uint8_t)c);
            continue;
        }
        int num = ld_.decode(in_);
        if (num < 256) { win.push_back((uint8_t)num); continue; }
        if (num >= 271) {
            num -= 271;
            uint32_t len = kLDecode[num] + 3;
            if (int b = kLBits[num]) { len += in_.get16() >> (16 - b); in_.add(b); }
            int dn = dd_.decode(in_);
            uint32_t dist = dDecode_[dn] + 1;
            if (int b = dBits_[dn]) {
                if (dn > 9) {
                    if (b > 4) { dist += (in_.get16() >> (20 - b)) << 4; in_.add(b - 4); }
                    if (lowDistRep_ > 0) { lowDistRep_--; dist += prevLowDist_; }
                    else {
                        int low = ldd_.decode(in_);
                        if (low == 16) { lowDistRep_ = 16 - 1; dist += prevLowDist_; }
                        else { dist += low; prevLowDist_ = low; }
                    }
                } else { dist += in_.get16() >> (16 - b); in_.add(b); }
            }
            if (dist >= 0x2000) { len++; if (dist >= 0x40000) len++; }
            insertOldDist(dist);
            lastLength_ = len;
            copy(len, dist);
            continue;
        }
        if (num == 256) { if (!readEndOfBlock()) break; continue; }
        if (num == 257) {
            int fb = in_.get16() >> 8;
            in_.add(8);
            int len = (fb & 7) + 1;
            if (len == 7) { len = (in_.get16() >> 8) + 7; in_.add(8); }
            else if (len == 8) { len = in_.get16(); in_.add(16); }
            std::vector<uint8_t> code(len);
            for (auto& x : code) { x = (uint8_t)(in_.get16() >> 8); in_.add(8); }
            if (!addVmCode(fb, code)) { err = "bad filter"; return false; }
            continue;
        }
        if (num == 258) { if (lastLength_) copy(lastLength_, oldDist_[0]); continue; }
        if (num < 263) {
            int k = num - 259;
            uint32_t dist = oldDist_[k];
            for (int i = k; i > 0; i--) oldDist_[i] = oldDist_[i - 1];
            oldDist_[0] = dist;
            int ln = rd_.decode(in_);
            uint32_t len = kLDecode[ln] + 2;
            if (int b = kLBits[ln]) { len += in_.get16() >> (16 - b); in_.add(b); }
            lastLength_ = len;
            copy(len, dist);
            continue;
        }
        num -= 263;   // 263..270: a short distance, length 2
        uint32_t dist = kSDDecode[num] + 1;
        if (int b = kSDBits[num]) { dist += in_.get16() >> (16 - b); in_.add(b); }
        insertOldDist(dist);
        lastLength_ = 2;
        copy(2, dist);
    }
    if (win.size() < end) { err = "packed data ended early"; return false; }
    return true;
}

// the standard filters on a block of the output (in place); fileOffset = position of the block in the file
bool runFilter(const Filter& f, uint8_t* d, uint32_t n, uint32_t fileOffset)
{
    std::vector<uint8_t> o(n);
    switch (f.type) {
    case kE8:
    case kE8E9: {
        const uint32_t fileSize = 0x1000000;
        uint8_t c2 = f.type == kE8E9 ? 0xE9 : 0xE8;
        for (uint32_t pos = 0; n >= 4 && pos < n - 4;) {
            uint8_t c = d[pos++];
            if (c == 0xE8 || c == c2) {
                uint32_t off = pos + fileOffset, addr;
                memcpy(&addr, d + pos, 4);
                if (addr & 0x80000000u) {
                    if (((addr + off) & 0x80000000u) == 0) { addr += fileSize; memcpy(d + pos, &addr, 4); }
                } else if ((addr - fileSize) & 0x80000000u) {
                    addr -= off;
                    memcpy(d + pos, &addr, 4);
                }
                pos += 4;
            }
        }
        return true;
    }
    case kDelta: {
        uint32_t ch = f.r[0], src = 0;
        if (!ch) return false;
        for (uint32_t c = 0; c < ch; c++) {
            uint8_t prev = 0;
            for (uint32_t i = c; i < n; i += ch) o[i] = prev -= d[src++];
        }
        memcpy(d, o.data(), n);
        return true;
    }
    case kRgb: {
        int width = (int)f.r[0] - 3, posR = (int)f.r[1];
        if (width < 3 || posR < 0 || posR > 2) return false;
        const uint8_t* src = d;
        for (int c = 0; c < 3; c++) {
            uint32_t prev = 0;
            for (int i = c; i < (int)n; i += 3) {
                uint32_t pred;
                int up = i - width;
                if (up >= 3) {
                    uint32_t u = o[up], ul = o[up - 3];
                    pred = prev + u - ul;
                    int pa = std::abs((int)(pred - prev)), pb = std::abs((int)(pred - u)), pc = std::abs((int)(pred - ul));
                    pred = pa <= pb && pa <= pc ? prev : pb <= pc ? u : ul;
                } else
                    pred = prev;
                o[i] = (uint8_t)(prev = (uint8_t)(pred - *src++));
            }
        }
        for (int i = posR; i < (int)n - 2; i += 3) { uint8_t g = o[i + 1]; o[i] += g; o[i + 2] += g; }
        memcpy(d, o.data(), n);
        return true;
    }
    case kAudio: {
        uint32_t ch = f.r[0];
        if (!ch) return false;
        const uint8_t* src = d;
        for (uint32_t c = 0; c < ch; c++) {
            uint32_t prev = 0, prevDelta = 0, dif[7] = {};
            int d1 = 0, d2 = 0, d3, k1 = 0, k2 = 0, k3 = 0;
            for (uint32_t i = c, cnt = 0; i < n; i += ch, cnt++) {
                d3 = d2;
                d2 = prevDelta - d1;
                d1 = prevDelta;
                uint32_t pred = 8 * prev + k1 * d1 + k2 * d2 + k3 * d3;
                pred = (pred >> 3) & 0xFF;
                uint32_t cur = *src++;
                pred -= cur;
                o[i] = (uint8_t)pred;
                prevDelta = (uint32_t)(int8_t)(pred - prev);
                prev = pred & 0xFF;
                int dd = ((int8_t)cur) << 3;
                dif[0] += std::abs(dd); dif[1] += std::abs(dd - d1); dif[2] += std::abs(dd + d1); dif[3] += std::abs(dd - d2);
                dif[4] += std::abs(dd + d2); dif[5] += std::abs(dd - d3); dif[6] += std::abs(dd + d3);
                if ((cnt & 0x1F) == 0) {
                    uint32_t mn = dif[0], k = 0;
                    dif[0] = 0;
                    for (int j = 1; j < 7; j++) { if (dif[j] < mn) { mn = dif[j]; k = j; } dif[j] = 0; }
                    switch (k) {
                    case 1: if (k1 >= -16) k1--; break;
                    case 2: if (k1 < 16) k1++; break;
                    case 3: if (k2 >= -16) k2--; break;
                    case 4: if (k2 < 16) k2++; break;
                    case 5: if (k3 >= -16) k3--; break;
                    case 6: if (k3 < 16) k3++; break;
                    }
                }
            }
        }
        memcpy(d, o.data(), n);
        return true;
    }
    default: return false;
    }
}

} // namespace

bool rarExtract(const std::vector<uint8_t>& a, std::vector<RarEntry>& out, std::string& err)
{
    out.clear();
    static const uint8_t sig[7] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
    if (a.size() < 7 + 13 || memcmp(a.data(), sig, 7)) { err = "not a RAR archive (RAR 1.5-4.x)"; return false; }
    auto u16 = [&](size_t i) { return (uint32_t)a[i] | a[i + 1] << 8; };
    auto u32 = [&](size_t i) { return u16(i) | u16(i + 2) << 16; };
    size_t p = 7;
    bool solidArc = false, havePrev = false;
    std::unique_ptr<Unpack29> u(new Unpack29);
    while (p + 7 <= a.size()) {
        int type = a[p + 2];
        uint32_t flags = u16(p + 3), hsize = u16(p + 5);
        if (hsize < 7 || p + hsize > a.size()) break;
        uint64_t add = 0;
        if (type == 0x73) solidArc = flags & 0x08;
        if (type == 0x74 && hsize >= 32) {
            RarEntry e;
            uint64_t pack = u32(p + 7), unp = u32(p + 11);
            uint32_t crc = u32(p + 16);
            e.version = a[p + 24];
            e.method = a[p + 25];
            uint32_t nl = u16(p + 26), attr = u32(p + 28);
            size_t np = p + 32;
            if (flags & 0x100) { pack |= (uint64_t)u32(p + 32) << 32; unp |= (uint64_t)u32(p + 36) << 32; np += 8; }
            add = pack;
            e.size = unp;
            e.crc = crc;
            e.encrypted = flags & 0x04;
            e.solid = (flags & 0x10) && havePrev;
            e.dir = (flags & 0xE0) == 0xE0 || (attr & 0x10);
            if (np + nl > p + hsize) break;
            const uint8_t* nm = &a[np];
            size_t z = 0;
            while (z < nl && nm[z]) z++;
            if ((flags & 0x200) && z < nl) {   // Unicode name after the OEM one
                std::vector<uint32_t> w;
                size_t ep = z + 1;
                uint32_t high = nm[ep++], fl = 0;
                int fb = 0;
                while (ep < nl && w.size() < 1024) {
                    if (fb == 0) { fl = nm[ep++]; fb = 8; }
                    switch (fl >> 6) {
                    case 0: if (ep < nl) w.push_back(nm[ep++]); break;
                    case 1: if (ep < nl) w.push_back(nm[ep++] + (high << 8)); break;
                    case 2: if (ep + 1 < nl) w.push_back(nm[ep] | nm[ep + 1] << 8); ep += 2; break;
                    case 3: {
                        if (ep >= nl) break;
                        int len = nm[ep++];
                        if (len & 0x80) {
                            uint8_t corr = ep < nl ? nm[ep++] : 0;
                            for (len = (len & 0x7F) + 2; len > 0; len--) {
                                size_t i = w.size();
                                w.push_back((((i < z ? nm[i] : 0) + corr) & 0xFF) + (high << 8));
                            }
                        } else
                            for (len += 2; len > 0; len--) { size_t i = w.size(); w.push_back(i < z ? nm[i] : 0); }
                    } break;
                    }
                    fl = (fl << 2) & 0xFF;
                    fb -= 2;
                }
                for (uint32_t c : w) putUtf8(e.name, c == '\\' ? '/' : c);
            } else
                for (size_t i = 0; i < z; i++) putUtf8(e.name, nm[i] == '\\' ? '/' : cp866(nm[i]));
            size_t dp = p + hsize;
            if (dp + pack > a.size()) { e.error = "the archive is cut short"; out.push_back(std::move(e)); break; }
            const uint8_t* d = &a[dp];
            if (e.dir) e.ok = true;
            else if (e.encrypted) e.error = "encrypted";
            else if (e.method == 0x30) {
                e.data.assign(d, d + std::min<uint64_t>(pack, unp));
                e.ok = crc32Of(e.data.data(), e.data.size()) == crc;
                if (!e.ok) e.error = "CRC does not match";
            } else if (e.version < 29 || e.version > 36) e.error = "packed by an old RAR (version " + std::to_string(e.version / 10) + "." + std::to_string(e.version % 10) + "), not read";
            else if (unp > ((uint64_t)1 << 31)) e.error = "too big";
            else {
                std::string ue;
                size_t before = e.solid ? u->win.size() : 0;
                if (!u->unpack(d, pack, unp, e.solid, ue)) e.error = ue;
                else {
                    e.data.assign(u->win.begin() + before, u->win.begin() + before + unp);
                    for (auto& f : u->pending) {
                        if (f.type == kNone) { e.error = "a filter program it does not know"; break; }
                        if (f.start < before || f.start - before + f.length > unp || f.length < 4) continue;
                        uint32_t at = (uint32_t)(f.start - before);
                        if (!runFilter(f, e.data.data() + at, f.length, at)) { e.error = "filter not read"; break; }
                    }
                    if (e.error.empty()) {
                        e.ok = crc32Of(e.data.data(), e.data.size()) == crc;
                        if (!e.ok) e.error = "CRC does not match";
                    }
                }
                havePrev = true;
            }
            if (!e.dir && !(e.method == 0x30)) havePrev = true;
            out.push_back(std::move(e));
        } else if (flags & 0x8000)
            add = u32(p + 7);
        if (type == 0x7B) break;   // end of the archive
        p += hsize + add;
    }
    (void)solidArc;
    return true;
}
