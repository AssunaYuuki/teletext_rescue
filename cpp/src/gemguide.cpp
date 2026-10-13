// Gemstar guide data, decoded the way Gemstar's own monitor program does it: FREQCCD_.EXE,
// "Gemstar's VBI Data Monitor Program" 1.5 (Hilite Inc. 1995-96), from the user's boot floppy.
// Everything here was reverse-engineered from that program (addresses are its segment:offset)
// and checked against its code run in an emulator (tools/gemstar).
//
// Type A packets (07 7C/7A .. checksum 0F lrc, 7-bit + parity):
//   1a3b:0000  sum 07..checksum = 0 mod 128; lrc = XOR(07..checksum, with parity) ^ 8F
//   bytes 2..12: country (b2 bit 0 = 0 USA, bit 1 = 0 Canada), block = nibbles b3 b4, subtype = b5 & 7,
//   blocks in all = nibbles b6 b7, HostID from b8..b10, b11 = payload checksum, version = b12 | (b5 & 18) << 3
//   payload from b13, scrambled with a monthly key file <month*100 + year%100>.INP (1bac:0168)
// Type B blocks (2x lines, 4 bytes a line): 05 FC b2 b3 idx tag flags chk + 64 (b3 bit 0) or 32 bytes
//   decrypted with a key made from year-1980, month, idx and tag (23cd:0561), chained checksum (22fd:07ab);
//   blocks of one tag make a message: byte 0 = type (0 Show Information, 9 Theme, ...)
// Show Information: records of 4 bytes + 4 strings (title, two description lines, programme number),
//   strings compressed with digrams, trigrams and two dictionaries (2a4f), the message PlusCode gives
//   channel, date and the time the records count from (28ec:0fcf, the VCR Plus+ algorithm).
#include "gemstar.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "gemstar_tables.inc"

namespace {

std::string fmt(const char* f, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string printable(const std::vector<uint8_t>& v, size_t from = 0, size_t to = (size_t)-1)
{
    std::string s;
    for (size_t i = from; i < v.size() && i < to; i++) s += v[i] >= 0x20 && v[i] < 0x7F ? (char)v[i] : '.';
    return s;
}

std::string hexs(const std::vector<uint8_t>& v, size_t from = 0, size_t to = (size_t)-1)
{
    std::string s;
    for (size_t i = from; i < v.size() && i < to; i++) s += fmt("%s%02X", s.empty() ? "" : " ", v[i]);
    return s;
}

// ---------------------------------------------------------------- PlusCode (28ec:07b0, 28ec:000c)
long cdiv(long a, long b) { long q = std::labs(a) / std::labs(b); return (a >= 0) == (b >= 0) ? q : -q; }
long cmod(long a, long b) { return a - cdiv(a, b) * b; }

long pcMix(long year, long month, long code)
{
    static const long P[10] = {0, 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    long N = 0;
    for (int i = 7; i >= 0; i--)
        if (code >= P[i + 1]) { N = i + 1; break; }
    long d[24] = {};
    long rest = code;
    for (long i = N; i >= 1; i--) rest -= d[i + 1] * P[i + 1], d[i] = cdiv(rest, P[i]);
    if (N != 1)
        for (int guard = 0; guard < 100; guard++) {
            for (int r = 0; r < 3; r++)
                for (long i = 2; i <= N; i++) d[i] = cmod(d[i] + d[i - 1], 10);
            if (d[N]) break;
        }
    long X = d[3] * 100 + d[2] * 10 + d[1] - 1;
    long A = cmod(X, 32);
    long B = X >= 0 ? cdiv(X, 32) + 1 : cdiv(-X, 32);
    N -= 3;
    bool skipA = false;
    if (N >= 1) {
        for (long i = 1; i <= N; i++) d[i] = d[i + 3];
        for (long i = 1; i <= N; i++) A += d[i];
        A = cmod(A, 32);
        d[N + 1] = 0;
        if (N >= 4) {
            long S = 5 - N, T = year * 12 + month, R = cmod(T, 31);
            for (int guard = 0; guard < 100; guard++) {
                for (long i = 1; i <= 31; i++) {
                    long k = R - i;
                    if (k < 0) k += 31;
                    long v = gsPcW[k], a = cmod(v, 10) - S, b = cdiv(v, 10) - S;
                    if (a <= 0) a = b >= 3 ? 2 : 4;
                    else if (b <= 0) b = a >= 3 ? 2 : 4;
                    long y = d[b] * 10 + d[a], z;
                    for (int g2 = 0; g2 < 1000; g2++) {
                        z = cmod((long)gsPcU[y] - T, 192);
                        if (z < 0) z += 192;
                        if (z <= 99) break;
                        y = z;
                    }
                    d[b] = cdiv(z, 10), d[a] = cmod(z, 10);
                }
                if (d[N]) break;
            }
            skipA = true;
        } else {
            long C = cmod(B, 10);
            for (int guard = 0; guard < 100; guard++) {
                for (long r = 0; r <= cmod(year, 16); r++) {
                    d[N] = cmod(d[N] + C, 10);
                    for (long j = N - 1; j >= 1; j--) d[j] = cmod(d[j] + d[j + 1], 10);
                    A += d[1];
                }
                if (d[N]) break;
            }
        }
    }
    if (!skipA) A = cmod(A + B * (month + 1), 32);
    long Z = 0;
    for (long i = 1; i <= N; i++) Z += d[i] * P[i];
    return Z * 1000 + ((B - 1) << 5) + A + 1;
}

struct PlusCode { int channel = 0, day = 0, hour = 0, minute = 0, duration = 0; };

PlusCode pcUnpack(long Z)
{
    PlusCode r;
    Z -= 1;
    long H = cdiv(Z, 1000), L = Z - H * 1000;
    r.day = (int)cdiv(L, 32) + 1;
    long e[30] = {}, m = cmod(L, 32);
    for (int i = 1; i <= 5; i++) e[i] = (m >> (i - 1)) & 1;
    for (int i = 27; i >= 11; i--) H -= e[i + 1] * (1L << (i - 10)), e[i] = cdiv(H, 1L << (i - 11));
    long h[15] = {0, e[1], e[3], e[5], e[11], e[14], e[15], e[16], e[18], e[20], e[21], e[22], e[24], e[25], e[26]};
    long g[9] = {0, e[2], e[4], e[12], e[13], e[17], e[19], e[23], e[27]};
    if (e[27]) h[12] = e[23], h[13] = e[24], h[14] = e[25], g[7] = e[26];
    long ch = 0, T = 0;
    for (int i = 1; i <= 8; i++) ch += g[i] << (i - 1);
    for (int i = 1; i <= 14; i++) T += h[i] << (i - 1);
    r.channel = (int)ch + 1;
    long Lx = 0, Sx = 0;
    if (T >= 0x300) {
        if (T <= 0xE3F) {
            long q = cmod(T - 768, 10) + 1;
            if (q < 6) Lx = q * 5; else Sx = (q - 5) * 5;
            T = cdiv(T - 768, 10);
        } else if (T <= 0x197F) {
            long q = cmod(T - 3648, 6) + 1;
            if (q == 1) Lx = 15; else Sx = (q - 1) * 5;
            T = cdiv(T - 3648, 6) + 288;
        } else if (T <= 0x359F) {
            long q = cmod(T - 6528, 25);
            Lx = (cmod(q, 5) + 1) * 5, Sx = (cdiv(q, 5) + 1) * 5;
            T = cdiv(T - 6528, 25);
        } else {
            Sx = (cmod(T - 13728, 5) + 1) * 5, Lx = 15;
            T = cdiv(T - 13728, 5) + 288;
        }
    }
    long tA, tB;
    if (T >= 0 && T < 192) tA = cmod(gsPcU[T], 48), tB = cdiv(gsPcU[T], 48);
    else tA = 47 - cmod(T - 192, 48), tB = cdiv(T, 48);
    r.hour = (int)cdiv(tA, 2);
    r.minute = (int)(cmod(tA, 2) * 30 + Sx);
    r.duration = (int)((tB + 1) * 30 - Lx);
    return r;
}

// 28ec:0fcf with year % 100 and month of the decoding date
PlusCode plusCode(int year, int month, long code) { return pcUnpack(pcMix(year % 100, month, code)); }

// ---------------------------------------------------------------- compressed text (2a4f)
struct TextDecoder {
    std::vector<int> in;
    size_t pos = 0;
    std::string out;
    int F = 4;
    bool eof = false;
    int gbase = 0, glen = 0;
    const uint8_t* gcnt = nullptr;
    static int T(int off) { off -= gsTxtBase; return off >= 0 && off < (int)sizeof gsTxt ? gsTxt[off] : 0; }

    int getc()
    {
        if (pos >= in.size()) { eof = true; return 0; }
        return in[pos++];
    }
    void push(const std::vector<int>& v) { in.insert(in.begin() + pos, v.begin(), v.end()); }
    void put(int c) { if (out.size() < 4096) out += (char)(c & 0xFF); }
    void space2() { F &= 0xF3; put(' '); }
    void digram(int c, int& a, int& b) { int i = (c & 0x7F) * 2; a = T(0x46F3 + i), b = T(0x46F4 + i); }
    void setgroup(int n)
    {
        gbase = (gsDict[n * 8] | gsDict[n * 8 + 1] << 8) - 0x7A00;
        glen = gsDict[n * 8 + 2];
        gcnt = gsDict + n * 8 + 3;
    }
    void pushdyn(int c)
    {
        int ln = glen, off = gbase;
        for (int k = 0; gcnt[k] <= c; k++) {
            if (k >= 12) return;
            off += gcnt[k] * ln, c -= gcnt[k], ln++;
        }
        int p = off + c * ln;
        std::vector<int> v;
        for (int i = 0; i < ln && p + i >= 0 && p + i < (int)sizeof gsDict; i++) v.push_back(gsDict[p + i]);
        push(v);
    }
    void pushstatic(int c)
    {
        bool up = c & 0x80;
        c &= 0x7F;
        int ln = T(0x4590), off = 0;
        for (int k = 0; T(0x4591 + k) <= c; k++) {
            if (k >= 12) return;
            off += ln * T(0x4591 + k), c -= T(0x4591 + k), ln++;
        }
        off += c * ln;
        std::vector<int> v;
        for (int i = 0; i < ln; i++) {
            int b = T(0x4595 + off + i);
            if (!up) v.push_back(b);
            else if (b < 0x80) v.push_back((b + 0x1A) & 0xFF);
            else {
                int x, y;
                digram(b, x, y);
                v.push_back(x == 0x27 ? 0x7F : (x - 7) & 0xFF);
                v.push_back((y - 7) & 0xFF);
            }
        }
        push(v);
    }
    // labels 28b / 293 / 2c4 of 2a4f:01cb; mode 1 = digram (second letter as is), 3 = trigram
    void emit(int c, int mode, int c3 = 0, int* tri = nullptr, int ntri = 0)
    {
        bool raw = false;
        for (int guard = 0; guard < 8; guard++) {
            if (!raw) {
                if (F & 1) { F ^= 1; if (!(F & 4)) put(' '); }
                if (F & 2) { F ^= 2; if (c & 0x40) c = (c - 0x20) & 0xFF; }
            }
            put(c);
            F &= 0xFB;
            if (mode == 1) { mode = 0, raw = true, c = c3; continue; }
            if (mode == 3 && ntri) { c = *tri++, ntri--, F &= 0xF7, raw = false; continue; }
            return;
        }
    }
    void number()
    {
        for (int guard = 0; guard < 4096; guard++) {
            int c = getc();
            if (eof) return;
            int c3 = c;
            F |= 0x20;
            int n = (c >> 4) & 0xF;
            for (;;) {
                if (n == 0xF) {
                    if (F & 0x20)
                        for (int k = c3 & 0xF; k > 0; k--) space2();
                    F = 0;
                    return;
                }
                put(n < 10 ? n + 0x30 : T(0x46C4 + n));
                if (F & 0x20) { F ^= 0x20, n = c3 & 0xF; continue; }
                break;
            }
        }
    }
    void token()
    {
        if (F & 0x10) return number();
        int c = getc();
        if (eof) return;
        if (c >= 0x80) {
            int a, b;
            digram(c, a, b);
            F &= 0xF7;
            return emit(a, 1, b);
        }
        int orig = c;
        if (c < 0x20) {
            F |= 1;
            if (c == 0) return emit(0x26, 0);
            c = T(0x408A + orig);
            if (c & 0x80) F |= 2, c &= 0x7F;
            return pushstatic(c);
        }
        if (c & 0x40) {
            if (c <= 0x73) {
                if (c <= 0x59) { F &= 0xF7; return emit(orig + 0x21, 0); }
                if (!(F & 8)) F |= 9;
                return emit(orig - 0x19, 0);
            }
            c = T(0x464E + orig);
            if (c & 0x80) { F |= 8; return emit(c & 0x7F, 0); }
            if (c & 0x40) return emit(c & 0x3F, 0);
            F &= 0xF7;
            return emit(c, 0);
        }
        if (c & 0x10) {
            if (orig <= 0x39) {
                int base = (orig * 3 + 0x70) & 0xFF;
                int tri[3] = {T(0x46D4 + base), T(0x46D5 + base), T(0x46D6 + base)};
                F &= 0xF7;
                return emit(tri[0], 3, 0, tri + 1, 2);
            }
            if (orig == 0x3A || orig == 0x3B) {
                if (orig == 0x3B) space2();
                c = getc();
                if (eof) return;
                if ((c & 0x80) && (c & 0x40)) c &= 0x7F, put(0x1B);
                if (c == 0x5B) F = 8;
                return emit(c, 0);
            }
            if (orig == 0x3C) { F |= 3, F &= 0xF7; return; }
            if (orig == 0x3D) { F = 0x10 | (F & 4); return number(); }
            F &= 0xFD;
            if (orig == 0x3E) { F = 4, put('\n'); return; }   // the program writes 0x0C here: a line break
            if (F & 4) F ^= 4; else F ^= 8;
            return;
        }
        if (c & 8) F = 1;
        if (c & 4) F |= 2;
        int n = c & 3;
        if (n != 3) {
            setgroup(n);
            c = getc();
            if (eof) return;
            return pushdyn(c);
        }
        c = getc();
        if (eof) return;
        if (c == 0) return emit(0x26, 0);
        if (c & 0x80) F &= 0xFD;
        pushstatic(c);
    }
    std::string decode(const uint8_t* p, size_t n)
    {
        in.assign(p, p + n);
        pos = 0, out.clear(), F = 4, eof = false;
        for (int guard = 0; pos < in.size() && !eof && guard < 20000 && out.size() < 4096; guard++) token();
        return out;
    }
};

// ---------------------------------------------------------------- Type A (1a3b, 1af1, 1bdb, 1bac)
const char* typeAName(const std::vector<uint8_t>& b)
{
    if (b[1] == 0x7A) return "G-Clock";
    switch (b[5] & 7) {
    case 0: return "IndexPlus+ Source Schedule";
    case 1: return "Source Mapping";
    case 2: return "ZIP Code";
    case 3: return "IndexPlus+ Host Schedule";
    case 4: return "Channel Mapping";
    case 5: return "Channel Map Selection";
    case 6: return b.size() > 8 && (b[8] & 0x20) ? "Time Zone and DST Use" : "GuidePlus+ Host Schedule";
    }
    return "Filler";
}

// 1bac:0168 on a copy of the payload; true if the payload checksum (b11) is right afterwards
bool typeADescramble(const std::vector<uint8_t>& buf, const GsKey& key, std::vector<uint8_t>& p)
{
    size_t len = buf.size() + 2; // the program counts 0F and the lrc too
    if (len < 17) return false;
    size_t n = len - 16;
    if (buf[len - 4] == 0) n--;
    p.assign(buf.begin() + 13, buf.begin() + 13 + n);
    if (n > 9) {
        int r = buf[6] ^ buf[7] ^ buf[8], prev = 0, b = (p[0] - 0x10) & 0xFF;
        for (int i = 0; i < key.n; i++) {
            size_t s = (size_t)std::abs((int)key.v[i]);
            if (s >= n) continue;
            size_t pos = (s + prev) % n;
            int a = (p[pos] + b) & 0xFF;
            if (a & 0x80) a = (a + 0x90) & 0xFF;
            if (r & 1) a = (a + 1) & 0xFF;
            r = ((r >> 1) | (r << 7)) & 0xFF;
            p[pos] = gsSub[a];
            if (key.v[i] < 0) std::swap(p[prev], p[pos]);
            b = (p[pos] - 0x10) & 0xFF;
            prev = (int)pos;
        }
    }
    int sum = buf[11];
    for (uint8_t v : p) sum += v;
    return (sum & 0x7F) == 0;
}

void typeA(GemstarStream& s, int year, int month)
{
    std::map<std::string, int> kinds;
    std::vector<std::string> lines, plain;
    std::set<std::string> callSeen;
    for (auto& pk : s.packets) {
        const auto& b = pk.raw;
        if (b.size() < 14 || (b[1] != 0x7C)) continue;
        int block = (b[3] & 15) << 4 | (b[4] & 15), total = (b[6] & 15) << 4 | (b[7] & 15);
        int version = (b[12] & 0x3F) | (b[5] & 0x18) << 3;
        int host = (b[9] & 0x3F) << 8 | (b[8] & 0x18) << 11 | (b[10] & 0x3F) | (b[8] & 6) << 5;
        const char* country = !(b[2] & 1) ? "USA" : !(b[2] & 2) ? "Canada" : "?";
        std::string name = typeAName(b);
        kinds[fmt("%s (%s, version %02X, HostID %04X, %d blocks)", name.c_str(), country, version, host, total)]++;
        // keys: the recording month or the next one, by the month parity in b6
        std::vector<uint8_t> p;
        const GsKey* used = nullptr;
        if (year) {
            for (int step = 0; step < 2 && !used; step++) {
                int m = (month - 1 + step) % 12 + 1, y = year + (month + step > 12);
                if ((b[6] ^ m) & 1) continue;
                for (auto& k : gsKeys)
                    if (k.year == y % 100 && k.month == m && typeADescramble(b, k, p)) used = &k;
            }
        }
        if (!used) continue;
        std::string t = fmt("  Type A %s block %d/%d, key %02d/%02d:", name.c_str(), block, total, used->month, used->year);
        if ((b[5] & 7) == 1)
            for (size_t i = 0; i + 7 <= p.size(); i += 7) {
                int sid = (p[i] & 0xC) << 12 | (p[i] & 3) << 6 | (p[i + 1] & 0x3F) | (p[i + 2] & 0x3F) << 8;
                std::string call(p.begin() + i + 3, p.begin() + i + 7);
                if (callSeen.insert(fmt("%04X", sid) + call).second) t += fmt(" %04X=%s", sid, call.c_str());
            }
        else t += " " + printable(p) + "  [" + hexs(p) + "]";
        plain.push_back(t);
    }
    if (kinds.empty()) return;
    lines.push_back("  Type A packets (07 7C, FREQCCD_ names):");
    for (auto& kv : kinds) lines.push_back(fmt("    %s  x%d", kv.first.c_str(), kv.second));
    if (plain.empty())
        lines.push_back(year ? fmt("    payload scrambled with the key of %02d/%04d; the floppy has keys for 01/2000 .. 03/2001 only", month, year)
                             : "    payload scrambled with a monthly key (no clock in the recording to pick it)");
    for (auto& l : plain) lines.push_back(l);
    s.guide.insert(s.guide.end(), lines.begin(), lines.end());
}

// ---------------------------------------------------------------- Type B (22fd, 23cd)
struct TypeBCipher {
    static void keysched(int a, int b, int idx, int tag, uint8_t out[16])
    {
        a &= 0xFF, b &= 0xFF;
        if (!(tag & 0x80)) {
            b++;
            if (b == 13) b = 1, a = (a + 1) & 0xFF;
        }
        int l7 = ((tag << 3) & 0xF8) | (idx & 7);
        int y = ((idx >> 4) & 0xF) | ((idx << 4) & 0xF0);
        y ^= b;
        y = ((y >> 1) | (y << 7)) & 0xFF;
        int l8 = ((y - a) & 0xFF) ^ 0x39;
        int cur = a ^ ((b + l7) & 0xFF), l1 = a;
        for (int i = 0; i < 16; i++) {
            l1 = (l1 + b) & 0xFF;
            out[i] = gsSub[l1 & 0x7F];
            cur = (cur + out[i]) & 0xFF;
        }
        const uint8_t* P = tag & 0x80 ? gsP1 : gsP0;
        l1 = cur;
        for (int i = 0; i < 16; i++) {
            int c = out[i];
            out[i] = c ^ l1;
            l1 = P[(c + l7) & 0xFF] ^ l8;
        }
    }
    static void mix(const uint8_t* d, int sel, uint8_t* k)
    {
        const uint8_t *A = gsP0, *B = gsP1;
        auto sw = [&]() { std::swap(A, B); };
        int cl;
        if (sel & 1) sw();
        cl = A[((d[8] ^ d[0x18]) + d[0xA]) & 0xFF]; k[0] ^= cl; cl = k[0];
        cl ^= (d[0x17] + d[6]) & 0xFF; cl = A[cl] ^ k[1]; k[1] = cl;
        cl = (d[0x11] + d[7] + cl) & 0xFF;
        if (sel & 2) sw();
        cl = A[cl] ^ k[2]; k[2] = cl;
        cl = (cl - d[0x16] + d[0xD]) & 0xFF; cl = A[cl] ^ k[3]; k[3] = cl;
        cl = (cl + d[0x13]) & 0xFF;
        if (sel & 4) sw();
        cl = (cl + d[9]) & 0xFF; cl = A[cl] ^ k[0]; k[0] = cl;
        cl = ((cl ^ d[0x1F]) + d[0xF]) & 0xFF; cl = A[cl] ^ k[1]; k[1] = cl;
        cl = (cl - d[0x1B]) & 0xFF;
        if (sel & 8) sw();
        cl = (cl + d[1]) & 0xFF; cl = A[cl] ^ k[2]; k[2] = cl;
        cl = (d[0x15] + d[0xD] + cl) & 0xFF; cl = A[cl] ^ k[3]; k[3] = cl;
        cl &= d[0x1C];
        if (sel & 0x10) sw();
        cl = (cl + d[5]) & 0xFF; cl = A[cl] ^ k[0]; k[0] = cl;
        cl = ((cl ^ d[0x1E]) + d[0]) & 0xFF; cl = A[cl] ^ k[1]; k[1] = cl;
        cl = (cl - d[0x10]) & 0xFF;
        if (sel & 0x20) sw();
        cl = (cl + d[4]) & 0xFF; cl = A[cl] ^ k[2]; k[2] = cl;
        cl = ((cl | d[0x12]) + d[0xC]) & 0xFF; cl = A[cl] ^ k[3]; k[3] = cl;
        cl = ((cl | d[0x19]) + d[3]) & 0xFF;
        if (sel & 0x40) A = B;
        cl = A[cl] ^ k[0]; k[0] = cl;
        cl = ((cl ^ d[0x1D]) + d[0xE]) & 0xFF; cl = A[cl] ^ k[1]; k[1] = cl;
        cl = (cl + d[0x14] + d[2]) & 0xFF; cl = A[cl] ^ k[2]; k[2] = cl;
        cl = (cl - d[0x1A] + d[0xB]) & 0xFF; cl = A[cl] ^ k[3]; k[3] = cl;
        cl = (cl + d[0x18]) & 0xFF; k[0] ^= A[cl];
    }
    static void feistel(uint8_t* d, uint8_t* k)
    {
        for (int i = 0; i < 32; i++) {
            k[0] = (k[0] + k[2]) & 0xFF;
            int dl = gsP0[(d[32 + i] + k[0]) & 0xFF];
            int tmp = d[i];
            dl = gsP0[dl ^ k[1]];
            d[32 + i] = tmp;
            d[i] = dl ^ tmp;
            k[1] = (k[1] + k[3]) & 0xFF;
        }
    }
    // 22fd:07ab + 23cd:0561: year/month = the date of the recording
    static void decrypt(uint8_t d[64], int year, int month, int idx, int tag, int flags)
    {
        if (((flags >> 7) ^ month) & 1) {
            month = month % 12 + 1;
            if (month == 1) year++;
        }
        uint8_t key[16];
        keysched(year - 1980, month, idx, tag, key);
        static const int part[4] = {0, 4, 8, 12}, sel[4] = {13, 11, 6, 4};
        for (int r = 0; r < 4; r++) {
            uint8_t k[4] = {key[part[r]], key[part[r] + 1], key[part[r] + 2], key[part[r] + 3]};
            mix(d, key[sel[r]], k);
            feistel(d, k);
        }
    }
};

struct Block { int64_t frame; std::vector<uint8_t> b; };

std::vector<Block> typeBBlocks(const GemstarStream& s)
{
    std::vector<Block> out;
    Block cur;
    int64_t last = -10;
    for (auto& w : s.words) {
        if (!w.x2) continue;
        if (!cur.b.empty() && w.frame != last + 1) cur.b.clear();
        last = w.frame;
        if (cur.b.empty()) {
            if (w.b[0] != 0x05 || w.b[1] != 0xFC) continue;
            cur.frame = w.frame;
        }
        cur.b.insert(cur.b.end(), w.b, w.b + 4);
        size_t need = cur.b[3] & 1 ? 80 : 48;
        if (cur.b.size() >= need) {
            cur.b.resize(need);
            out.push_back(cur);
            cur.b.clear();
        }
    }
    return out;
}

struct Message { int64_t frame; int tag; std::vector<uint8_t> m; bool complete; };

std::vector<Message> typeBMessages(GemstarStream& s, const std::vector<Block>& blocks, int year, int month)
{
    std::vector<Message> out;
    Message cur{0, -1, {}, false};
    int chain = 0;
    auto flush = [&]() {
        if (cur.tag >= 0 && !cur.m.empty()) {
            size_t need = cur.m[0] == 0 && cur.m.size() >= 3 ? (size_t)(cur.m[1] | cur.m[2] << 8) + 4 : (size_t)cur.m[1] * 64;
            cur.complete = need && cur.m.size() >= need;
            if (cur.complete) cur.m.resize(need);
            out.push_back(cur);
        }
        cur = Message{0, -1, {}, false};
    };
    for (auto& bl : blocks) {
        int idx = bl.b[4], tag = bl.b[5], flags = bl.b[6], chk = bl.b[7];
        size_t n = bl.b.size() == 80 ? 64 : 32;
        uint8_t d[64] = {};
        std::copy(bl.b.begin() + 8, bl.b.begin() + 8 + n, d);
        TypeBCipher::decrypt(d, year, month, idx, tag, flags);
        int sum = 0;
        for (size_t i = 0; i < 64; i++) sum += d[i];
        sum &= 0xFF;
        if (idx == 0) flush(), cur.frame = bl.frame, cur.tag = tag, chain = 0;
        bool ok = cur.tag == tag && cur.m.size() == (size_t)idx * 64 && (sum ^ chain) == chk;
        if (!ok) { s.blocksBad++; flush(); continue; }
        s.blocksGood++;
        chain ^= chk;
        cur.m.insert(cur.m.end(), d, d + n);
    }
    flush();
    return out;
}

// the year and month the Type B data decrypts with: tried around the clock date, or everywhere
bool typeBDate(const std::vector<Block>& blocks, int& year, int& month)
{
    auto score = [&](int y, int m) {
        int good = 0;
        for (auto& bl : blocks) {
            if (bl.b[4] != 0) continue;
            uint8_t d[64] = {};
            size_t n = bl.b.size() == 80 ? 64 : 32;
            std::copy(bl.b.begin() + 8, bl.b.begin() + 8 + n, d);
            TypeBCipher::decrypt(d, y, m, 0, bl.b[5], bl.b[6]);
            int sum = 0;
            for (int i = 0; i < 64; i++) sum += d[i];
            good += (sum & 0xFF) == bl.b[7];
        }
        return good;
    };
    if (year && score(year, month) > 0) return true;
    int best = 0, by = 0, bm = 0;
    for (int y = 1990; y <= 2012; y++)
        for (int m = 1; m <= 12; m++) {
            int sc = score(y, m);
            if (sc > best) best = sc, by = y, bm = m;
        }
    if (best < 2) return false;
    year = by, month = bm;
    return true;
}

int64_t bcdNumber(const uint8_t* p, size_t n)
{
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if ((p[i] >> 4) > 9 || (p[i] & 15) > 9) return -1;
        v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
    }
    return v;
}

std::string clean(std::string t)
{
    for (auto& c : t) if ((unsigned char)c < 0x20 && c != '\n') c = ' ';
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

// 252f:0367 (header, PlusCode -> channel/date/time) + 252f:0d54 (records) + 252f:0568 (strings)
void showInfo(GemstarStream& s, const Message& msg, int year, int month, int today, std::vector<std::string>& lines)
{
    const auto& m = msg.m;
    if (m.size() < 14) return;
    size_t total = std::min(m.size(), (size_t)(m[1] | m[2] << 8) + 4);
    int64_t code = bcdNumber(&m[4], 4);
    if (code <= 0) return;
    // decoded with the recording month; a date before the recording day means next month's code
    int y = year, mo = month;
    PlusCode pc = plusCode(y, mo, (long)code);
    if (today && pc.day < today) mo = mo % 12 + 1, y += mo == 1, pc = plusCode(y, mo, (long)code);
    int base = pc.hour * 60 + pc.minute;
    TextDecoder td;
    size_t p = 10;
    for (int rec = 0; rec < 64 && p + 4 < total; rec++) {
        size_t h = p;
        p += 4;
        std::string str[4];
        const uint8_t* raw4 = nullptr;
        size_t n4 = 0;
        bool bad = false;
        for (int k = 0; k < 4; k++) {
            size_t n = 0;
            for (;;) {
                if (p >= total) { bad = true; break; }
                int v = m[p++];
                n += v;
                if (v != 0xFF) break;
            }
            if (bad || p + n > total) { bad = true; break; }
            if (k < 3) str[k] = clean(td.decode(&m[p], n));
            else raw4 = &m[p], n4 = n;
            p += n;
        }
        if (bad) break;
        GemstarShow sh;
        sh.channel = pc.channel;
        sh.year = y, sh.month = mo, sh.day = pc.day;
        sh.start = (m[h] & 0x3F) * 5 + base;
        sh.duration = (m[h + 1] & 0x7F) * 5;
        sh.misc = m[h + 2], sh.theme = m[h + 3];
        sh.code = raw4 ? bcdNumber(raw4, n4) : -1;
        sh.frame = msg.frame;
        sh.title = str[0], sh.line1 = str[1], sh.line2 = str[2];
        int st = sh.start;
        int d = sh.day + st / 1440;
        lines.push_back(fmt("    ch %3d  %02d/%02d/%04d %02d:%02d UTC  %3d min  %s", sh.channel, sh.month, d, sh.year, st / 60 % 24, st % 60,
                            sh.duration, sh.title.c_str()));
        auto add = [&](const std::string& t) {
            size_t a = 0;
            while (a <= t.size()) {
                size_t e = t.find('\n', a);
                if (e == std::string::npos) e = t.size();
                if (e > a) lines.push_back("              " + t.substr(a, e - a));
                a = e + 1;
            }
        };
        if (!sh.line1.empty() && sh.line1 != "&") add(sh.line1);
        if (!sh.line2.empty() && sh.line2 != "&") add(sh.line2);
        s.shows.push_back(sh);
        if (p >= total || m[p] != 0) break;
        if (!(m[h] & 0x80)) break;
        p++;
    }
}

std::string themes(const std::vector<uint8_t>& m)
{
    std::string t;
    for (size_t p = 6; p + 2 <= m.size();) {
        int code = m[p], n = m[p + 1];
        if (!n || p + 2 + n > m.size()) break;
        bool text = true;
        for (int i = 0; i < n; i++) text &= m[p + 2 + i] >= 0x20 && m[p + 2 + i] < 0x7F;
        if (!text) break;
        t += fmt("%s%02X %s", t.empty() ? "" : ", ", code, std::string(m.begin() + p + 2, m.begin() + p + 2 + n).c_str());
        p += 2 + n;
    }
    return t;
}

const char* typeBName(int t)
{
    switch (t) {
    case 0: return "Show Information";
    case 1: return "SIP Update";
    case 2: return "Program Related Info (PRI)";
    case 3: return "Instant Info (II)";
    case 4: return "Theme";
    case 5: return "Show Description";
    case 6: return "Subscription";
    case 7: return "Display Priority - Gemstar";
    case 8: return "Display Priority - Cable";
    case 9: return "Sort Control / Themes";
    case 10: return "TVGuide Gold SIP";
    case 11: return "TVGuide Gold SIP Update";
    case 12: return "Dynamic Dictionary";
    }
    return "newer type (not in FREQCCD_ 1.5)";
}

} // namespace

GemstarTypeA gemstarTypeA(const GemstarPacket& pk)
{
    GemstarTypeA h;
    const auto& b = pk.raw;
    if (b.size() < 14 || (b[1] != 0x7C && b[1] != 0x7A)) return h;
    h.ok = true;
    h.name = typeAName(b);
    h.country = !(b[2] & 1) ? "USA" : !(b[2] & 2) ? "Canada" : "?";
    h.block = (b[3] & 15) << 4 | (b[4] & 15);
    h.total = (b[6] & 15) << 4 | (b[7] & 15);
    h.version = (b[12] & 0x3F) | (b[5] & 0x18) << 3;
    h.host = (b[9] & 0x3F) << 8 | (b[8] & 0x18) << 11 | (b[10] & 0x3F) | (b[8] & 6) << 5;
    h.payload.assign(b.begin() + 13, b.end() - 1);
    return h;
}

void gemstarGuide(std::vector<GemstarStream>& all)
{
    // the date: the first good clock packet in any stream
    int year = 0, month = 0, today = 0;
    for (auto& s : all)
        for (auto& p : s.packets) {
            GemstarClock c = gemstarClock(p);
            if (c.valid && !year) year = c.year, month = c.month, today = c.day;
        }
    for (auto& s : all) {
        s.guide.clear(), s.shows.clear(), s.blocksGood = s.blocksBad = 0;
        typeA(s, year, month);
        auto blocks = typeBBlocks(s);
        if (blocks.empty()) continue;
        int y = year, m = month;
        if (!typeBDate(blocks, y, m)) {
            s.guide.push_back(fmt("  Type B blocks: %zu, none decrypts with a date 1990..2012", blocks.size()));
            continue;
        }
        s.keyYear = y, s.keyMonth = m;
        auto msgs = typeBMessages(s, blocks, y, m);
        std::map<int, int> types;
        std::set<std::vector<uint8_t>> seen;
        std::vector<std::string> lines, other;
        for (auto& g : msgs) {
            if (!g.complete || !seen.insert(g.m).second) continue;
            types[g.m[0]]++;
            if (g.m[0] == 0) showInfo(s, g, y, m, y == year && m == month ? today : 0, lines);
            else if (g.m[0] == 9) {
                std::string t = themes(g.m);
                if (!t.empty()) other.push_back("    themes: " + t);
            } else {
                std::string txt;
                size_t run = 0;
                std::string cur;
                for (uint8_t v : g.m) {
                    if (v >= 0x20 && v < 0x7F) { cur += (char)v; continue; }
                    if (cur.size() >= 6) txt += (txt.empty() ? "" : " | ") + cur;
                    cur.clear();
                }
                if (cur.size() >= 6) txt += (txt.empty() ? "" : " | ") + cur;
                (void)run;
                other.push_back(fmt("    type %02X (%s), %zu bytes%s%s", g.m[0], typeBName(g.m[0]), g.m.size(), txt.empty() ? "" : ": ", txt.c_str()));
            }
        }
        std::string tl;
        for (auto& kv : types) tl += fmt("%s%02X x%d", tl.empty() ? "" : ", ", kv.first, kv.second);
        s.guide.push_back(fmt("  Type B: %zu blocks, %lld with a right checksum after decrypting with %02d/%04d; messages (type x count): %s",
                              blocks.size(), (long long)s.blocksGood, m, y, tl.empty() ? "none complete" : tl.c_str()));
        if (!lines.empty()) {
            s.guide.push_back(fmt("  Programmes (Show Information, %zu):", s.shows.size()));
            s.guide.insert(s.guide.end(), lines.begin(), lines.end());
        }
        if (!other.empty()) {
            s.guide.push_back("  Other messages:");
            s.guide.insert(s.guide.end(), other.begin(), other.end());
        }
    }
}
