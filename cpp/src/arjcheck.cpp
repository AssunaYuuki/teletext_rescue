// ARJ check: headers and files by CRC-32, the decompression after UNARJ (Robert K. Jung, public domain).
#include "arjcheck.h"
#include "ui/util.h"
#include <algorithm>
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
    Unarj(const uint8_t* src, size_t n) : in(src), left(n) {}
    bool lz(uint32_t size, std::vector<uint8_t>& out);
    bool fast(uint32_t size, std::vector<uint8_t>& out);

private:
    enum { CODE_BIT = 16, THRESHOLD = 3, MAXMATCH = 256, NC = 255 + MAXMATCH + 2 - THRESHOLD, NP = 17, CBIT = 9, NT = 19, PBIT = 5,
           TBIT = 5, NPT = NT };
    const uint8_t* in;
    size_t left;
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

ArjCheck arjCheck(const std::vector<uint8_t>& a)
{
    ArjCheck r;
    size_t data, size, next;
    bool ok;
    if (!header(a, 0, data, size, next, ok) || size == 0) return r;
    r.archive = true;
    r.headerOk = ok;
    size_t p = next;
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
        for (size_t k = first; k < size && h[k]; k++) e.name += (char)h[k];
        if (!ok) e.why = "header CRC";
        size_t body = next;
        // a damaged header still says where its file is: when its packed size does not end at the next header (or
        // the end of the archive), the next 60 EA after the data decides
        auto headerAt = [&](size_t q) { return q + 4 <= a.size() && a[q] == 0x60 && a[q + 1] == 0xEA; };
        if (!ok && (body + e.packed > a.size() || !headerAt(body + e.packed))) {
            size_t q = body;
            while (q + 4 <= a.size() && !headerAt(q)) q++;
            e.packed = (uint32_t)(std::min(q, a.size()) - body);
        }
        if (body + e.packed > a.size()) { e.why = "cut off"; r.files.push_back(e); break; }
        if (!ok && (e.size > e.packed * 40u + 65536u || e.size > (32u << 20))) e.size = e.packed * 40u + 65536u;
        if (!ok && e.method > 4) e.method = 1;
        if (h[6] != 3 || !ok) {   // 3 = a directory
            std::vector<uint8_t> out;
            bool dec = false;
            if (flags & 1 && ok) e.why = "encrypted (garbled)";
            else if (e.method == 0) { out.assign(a.begin() + body, a.begin() + body + e.packed); dec = true; }
            else if (e.method >= 1 && e.method <= 3) dec = Unarj(&a[body], e.packed).lz(e.size, out);
            else if (e.method == 4) dec = Unarj(&a[body], e.packed).fast(e.size, out);
            else e.why = "unknown method";
            e.got = (uint32_t)out.size();
            bool right = dec && out.size() == e.size && crc32Arj(out.data(), out.size()) == crc;
            if (e.why.empty()) {
                if (!dec || out.size() != e.size) e.why = "data does not unpack whole";
                else if (!right) e.why = "data CRC";
                else e.ok = true;
            } else if (right) e.why += ", but the data's CRC is right";
            e.data = std::move(out);
        } else e.ok = true;
        r.files.push_back(e);
        p = body + e.packed;
    }
    return r;
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
        else if (e.got >= e.size) snprintf(b, sizeof b, "%u bytes, CRC wrong (bytes damaged inside)", e.got);
        else snprintf(b, sizeof b, "%u of %u bytes (unpacking stopped at the damage)", e.got, e.size);
        list += nm + "  " + b + (e.ok || e.why.empty() ? "" : "; " + e.why) + "\n";
    }
    return list;
}

std::string ArjCheck::summary() const
{
    if (!archive) return "not an ARJ archive (its start did not come through)";
    char b[200];
    snprintf(b, sizeof b, "ARJ: %d of %zu files right (CRC)%s%s", good(), files.size(), headerOk ? "" : ", archive header damaged",
             complete ? "" : ", end of the archive not found");
    std::string s = b;
    if (good() == (int)files.size() && headerOk && complete && !files.empty()) s = "ARJ: the whole archive checks (" + std::to_string(files.size()) + " files, CRC right)";
    return s;
}
