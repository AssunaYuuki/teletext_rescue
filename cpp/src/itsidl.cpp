#include "itsidl.h"
#include "util.h"
#include <algorithm>
#include <cstring>

namespace {
struct Tables {
    int ham[256];        // Hamming 8/4 with one bit corrected
    uint8_t g[256];      // the driver's byte table: bit reversal ^ bit 0
    uint16_t t2[256];
    Tables()
    {
        static const uint8_t code[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
        for (int& v : ham) v = -1;
        for (int v = 0; v < 16; v++) ham[code[v]] = v;
        for (int v = 0; v < 16; v++)
            for (int k = 0; k < 8; k++)
                if (ham[code[v] ^ (1 << k)] < 0) ham[code[v] ^ (1 << k)] = v;
        for (int v = 0; v < 256; v++) {
            int r = 0;
            for (int k = 0; k < 8; k++) if (v >> k & 1) r |= 0x80 >> k;
            g[v] = (uint8_t)(r ^ (v & 1));
            uint16_t t = 0;
            for (int k = 0; k < 8; k++) if (v >> k & 1) t ^= (uint16_t)(0x8940 >> k);
            t2[v] = t;
        }
    }
};
const Tables T;

uint16_t crc(const uint8_t* d, int n)
{
    uint16_t c = 0;
    for (int i = 0; i < n; i++) c = (uint16_t)(T.t2[T.g[(d[i] ^ c) & 0xFF]] ^ (c >> 8));
    return c;
}

// Windows-1251 names and paths of InterTVNet as UTF-8
std::string fromCp1251(const char* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c >= 0xC0) s += from_cp(0x410 + (c - 0xC0));
        else if (c == 0xA8) s += from_cp(0x401);
        else if (c == 0xB8) s += from_cp(0x451);
        else if (c < 0x80) s += (char)c;
        else s += '_';
    }
    return s;
}

// InterTVNet: BCH(63,51) and the CRC of VbiDump.ax (itsidl.h)
struct ItvTables {
    uint64_t gen = 0;                  // generator polynomial, degree 12
    std::vector<uint64_t> err;         // remainder -> error pattern of up to 2 bits (kNone: more)
    uint16_t crc[256];                 // v * x^16 mod x^16+x^9+x^7+x^4+1
    uint8_t rev[256];
    std::vector<int16_t> ciOf;         // CRC difference -> CI
    static constexpr uint64_t kNone = ~0ull;
    uint64_t rem(uint64_t v) const
    {
        for (int i = 62; i >= 12; i--)
            if (v >> i & 1) v ^= gen << (i - 12);
        return v;
    }
    ItvTables() : err(4096, kNone), ciOf(65536, -1)
    {
        int ex[126], lg[64];
        for (int i = 0, x = 1; i < 63; i++) {
            ex[i] = ex[i + 63] = x;
            lg[x] = i;
            x <<= 1;
            if (x & 64) x ^= 0x43;
        }
        // minimal polynomials of a and a^3 (their conjugates are the roots), multiplied out in GF(64)
        auto minpoly = [&](int e) {
            std::vector<int> p{1};
            std::set<int> conj;
            for (int k = e; !conj.count(k); k = k * 2 % 63) conj.insert(k);
            for (int c : conj) {
                std::vector<int> r(p.size() + 1, 0);
                for (size_t i = 0; i < p.size(); i++) {
                    r[i + 1] ^= p[i];
                    if (p[i]) r[i] ^= ex[lg[p[i]] + c];
                }
                p = r;
            }
            uint64_t b = 0;
            for (size_t i = 0; i < p.size(); i++) if (p[i]) b |= 1ull << i;
            return b;
        };
        uint64_t m1 = minpoly(1), m3 = minpoly(3);
        for (int i = 0; i < 7; i++) if (m1 >> i & 1) gen ^= m3 << i;
        err[0] = 0;
        for (int i = 0; i < 63; i++) {
            uint64_t r = rem(1ull << i);
            if (err[r] == kNone) err[r] = 1ull << i;
        }
        for (int i = 0; i < 63; i++)
            for (int j = i + 1; j < 63; j++) {
                uint64_t r = rem(1ull << i | 1ull << j);
                if (err[r] == kNone) err[r] = 1ull << i | 1ull << j;
            }
        for (int v = 0; v < 256; v++) {
            uint32_t r = (uint32_t)v << 16;
            for (int i = 23; i >= 16; i--)
                if (r >> i & 1) r ^= 0x10291u << (i - 16);
            crc[v] = (uint16_t)r;
            int b = 0;
            for (int k = 0; k < 8; k++) if (v >> k & 1) b |= 0x80 >> k;
            rev[v] = (uint8_t)b;
        }
        static const uint16_t ciBase[8] = {0x4831, 0xA550, 0x52A8, 0x2954, 0x14AA, 0x0A55, 0x8462, 0x4231};
        for (int v = 0; v < 256; v++) {
            uint16_t t = 0;
            for (int k = 0; k < 8; k++) if (v >> k & 1) t ^= ciBase[k];
            ciOf[t] = (int16_t)v;
        }
    }
};
const ItvTables ITV;

// a packet X/31 of magazine 8 as InterTVNet: the 29 data bytes, the CI and the bits corrected
bool itvDecode(const uint8_t* p, uint8_t* data, int& ci, int& errors)
{
    uint8_t out[32] = {};
    errors = 0;
    auto bit = [&](int k) { return (p[2 + (k >> 3)] >> (k & 7)) & 1; };
    for (int j = 0; j < 5; j++) {
        uint64_t v = 0;
        for (int i = 0; i < 63; i++) if (bit(j + 5 * i)) v |= 1ull << i;
        uint64_t e = ITV.err[ITV.rem(v)];
        if (e == ItvTables::kNone) return false;
        errors += __builtin_popcountll(e);
        v ^= e;
        for (int d = 0; d < 51; d++)
            if (v >> (12 + d) & 1) out[(j + 5 * d) >> 3] |= (uint8_t)(1 << ((j + 5 * d) & 7));
    }
    uint16_t c = 0;
    for (int i = 0; i < 29; i++) c = (uint16_t)((c << 8) ^ ITV.crc[c >> 8] ^ ITV.crc[ITV.rev[out[i]]]);
    ci = ITV.ciOf[(uint16_t)((ITV.rev[out[29]] << 8 | ITV.rev[out[30]]) ^ c)];
    if (ci < 0) return false;
    memcpy(data, out, 29);
    return true;
}

// CI of the packet's data (from = the byte after RI) if its CRC is right, else -1
int checkedCi(const uint8_t* q, int from, bool ciSent)
{
    uint16_t c = crc(q + from, 42 - from);
    if (ciSent) return c == 0 ? q[from] : -1;
    return (c & 0xFF) == (c >> 8) ? (c & 0xFF) : -1;
}
}

void ItsIdl::packet(const uint8_t* p, int64_t frame)
{
    int a = T.ham[p[0]], b = T.ham[p[1]];
    if (a < 0 || b < 0 || ((a >> 3) | (b << 1)) != 31) return;
    if ((a & 7) == 0 && itvPacket(p, frame)) return;
    int ft = T.ham[p[2]], ial = T.ham[p[3]];
    if (ft < 0 || ial < 0 || (ft & 1)) return;   // format B not used
    int na = ial & 7;
    if (na > 6) return;
    uint8_t q[42];
    memcpy(q, p, 42);
    std::string spa;
    for (int k = 0; k < na; k++) {
        int s = T.ham[p[4 + k]];
        if (s < 0) return;
        q[4 + k] = (uint8_t)s;
        spa += fmt("%X", s);
    }
    int mag = (a & 7) ? (a & 7) : 8;
    int from = 4 + na + ((ft & 2) ? 1 : 0);
    bool ciSent = ft & 4;
    std::string key = fmt("%d/%s/%X", mag, spa.c_str(), ft);
    ItsChannel& c = ch_[key];
    if (!c.packets) { c.mag = mag; c.ft = ft; c.spa = spa; }
    c.packets++;
    int ci = checkedCi(q, from, ciSent);
    // one wrong bit, mended only to the CI expected (this one again or the next), as the driver does: without CI
    // sent any remainder with equal bytes passes (1 in 256), so a free search would pass almost every packet
    if (ci < 0 && c.prevCi >= 0) {
        auto expected = [&](int v) { return v == c.prevCi || v == ((c.prevCi + 1) & 0xFF); };
        for (int bit = from * 8; bit < 42 * 8 && ci < 0; bit++) {
            q[bit >> 3] ^= (uint8_t)(1 << (bit & 7));
            int v = checkedCi(q, from, ciSent);
            if (v >= 0 && expected(v)) ci = v;
            else q[bit >> 3] ^= (uint8_t)(1 << (bit & 7));
        }
        if (ci >= 0) c.mended++;
    }
    if (ci < 0) return;
    // a CI that jumps (or the first one) is taken only when the next packet goes on from it: a noisy packet
    // passes the implicit check 1 time in 256
    int d = c.prevCi >= 0 ? (ci - c.prevCi) & 0xFF : 255;
    if (d > 4) {
        int dc = c.candCi >= 0 ? (ci - c.candCi) & 0xFF : 255;
        if (dc > 4) { c.candCi = ci; return; }
        if (c.prevCi >= 0) c.lost += (c.candCi - c.prevCi) & 0xFF;   // the candidate's packet is lost too
        if (c.inMsg) message(c, true);
        c.esc = false;
        c.prevCi = c.candCi;
        d = dc;
    }
    c.candCi = -1;
    c.good++;
    if (c.first < 0) c.first = frame;
    c.last = frame;
    if (d == 0) { c.repeats++; return; }   // another copy (RI)
    if (d > 1) {
        c.lost += d - 1;
        if (c.inMsg) message(c, true);
        c.esc = false;
    }
    c.prevCi = ci;
    int dl = 40 - from - (ciSent ? 1 : 0);
    const uint8_t* data = q + from + (ciSent ? 1 : 0);
    if (ft & 8) { dl = std::min<int>(data[0], 39 - from - (ciSent ? 1 : 0)); data++; }
    for (int i = 0; i < dl; i++) {
        uint8_t x = data[i];
        if (c.esc) {
            c.esc = false;
            if (x == 0x10) { if (c.inMsg) c.msg.push_back(0x10); continue; }
            if (c.inMsg) message(c, false);
            c.inMsg = true;
            c.msgType = x;
            c.msg.clear();
            continue;
        }
        if (x == 0x10) { c.esc = true; continue; }
        if (c.inMsg && c.msg.size() < 4096) c.msg.push_back(x);
    }
}

void ItsIdl::message(ItsChannel& c, bool broken)
{
    c.inMsg = false;
    int t = c.msgType;
    std::string kind = broken ? "broken" : t == 0xFF ? "break" : (t & 0xC0) == 0 ? "open" : (t & 0xC0) == 0x40 ? "encrypted" : (t & 0xC0) == 0x80 ? "key" : "unknown";
    c.types[kind]++;
    if (kind == "key" && c.msg.size() == 12) {
        c.receivers.insert((uint32_t)c.msg[0] << 16 | c.msg[1] << 8 | c.msg[2]);
        c.keyNumbers[c.msg[3]]++;
    }
    if (kind == "open" && c.open.size() < 50) c.open.push_back(c.msg);
    c.msg.clear();
}

ItvAcl itvAclOf(const std::vector<uint8_t>& d)
{
    ItvAcl a;
    if (d.size() < 3 || d[0] != 0xAB || d[1] != 0xBD || d[2] < 1 || d[2] > 2) return a;
    a.type = d[2];
    size_t p = 3;
    auto u16 = [&](size_t i) { return d[i] | d[i + 1] << 8; };
    while (p < d.size()) {
        if (p + 4 > d.size()) return a;
        int kind = d[p + 1] & 0xF, n = u16(p + 2);
        bool flag = d[p + 1] >> 4 & 1;
        p += 4;
        for (int k = 0; k < n; k++) {
            if (kind == 1) {
                if (p + 4 > d.size() || p + 4 + d[p + 2] + d[p + 3] > d.size()) return a;
                ItvAcl::Resource r;
                r.a = fromCp1251((const char*)&d[p + 4], d[p + 2]);
                r.b = fromCp1251((const char*)&d[p + 4 + d[p + 2]], d[p + 3]);
                r.flag = flag;
                a.resources[u16(p)] = r;
                p += 4 + d[p + 2] + d[p + 3];
            } else if (kind == 2) {
                if (p + 5 > d.size()) return a;
                int g = u16(p), m = u16(p + 2), sz = d[p + 4];
                p += 5;
                if (sz < 1 || p + (size_t)m * sz > d.size()) return a;
                auto& v = a.groups[g];
                for (int i = 0; i < m; i++, p += sz) v.push_back(sz >= 2 ? u16(p) : d[p]);
            } else if (kind == 3 || kind == 4) {
                if (p + 8 > d.size()) return a;
                a.cards++;
                p += 8;
            } else
                return a;
        }
    }
    a.ok = true;
    return a;
}

std::string ItvAcl::folderOf(int resource) const
{
    auto it = resources.find(resource);
    if (it == resources.end()) return "";
    // the first string (acl_dll gives it as the folder, checked by running the dll: tools/acl/aclemu.py), else the
    // second; each part kept to letters, digits and . - _ space
    std::string s = !it->second.a.empty() ? it->second.a : it->second.b, out, part;
    auto flush = [&] {
        while (!part.empty() && (part.back() == ' ' || part.back() == '.')) part.pop_back();
        if (!part.empty() && part != "..") out += (out.empty() ? "" : "/") + part;
        part.clear();
    };
    for (char c : s) {
        if (c == '\\' || c == '/') { flush(); continue; }
        if (c == ':') continue;
        part += (unsigned char)c < 0x20 || strchr("*?\"<>|", c) ? '_' : c;
    }
    flush();
    return out;
}

bool ItsIdl::itvPacket(const uint8_t* p, int64_t frame)
{
    itvPackets_++;
    uint8_t d[29];
    int ci, errors;
    if (!itvDecode(p, d, ci, errors)) return false;
    itvGood_++;
    if (errors) itvCorrected_++;
    if (ci == itvPrevCi_) return true;   // the same packet again
    if (itvPrevCi_ >= 0 && ((ci - itvPrevCi_) & 0xFF) != 1) {
        itvLost_ += ((ci - itvPrevCi_) & 0xFF) - 1;
        itvEnd();   // a file with a hole: kept as far as it came
    }
    itvPrevCi_ = ci;
    if (d[0] == 0x10 && d[1] == 0 && d[2] >= 1 && d[2] <= 3 && d[3] == 0x10) {
        itvEnd();
        itvIn_ = true;
        itvFrame_ = frame;
        itvMsg_.assign(d, d + 29);
    } else if (itvIn_ && itvMsg_.size() < ((size_t)16 << 20))
        itvMsg_.insert(itvMsg_.end(), d, d + 29);
    return true;
}

void ItsIdl::itvEnd()
{
    if (!itvIn_) return;
    itvIn_ = false;
    std::vector<uint8_t> u;
    bool esc = false;
    for (uint8_t x : itvMsg_) {
        if (x == 0x10 && !esc) { esc = true; continue; }
        u.push_back(x);
        esc = false;
    }
    itvMsg_.clear();
    if (u.size() < 8) return;
    ItvFile f;
    f.kind = u[1];
    f.flags = u[2];
    f.resource = u[3] | u[4] << 8;
    f.first = itvFrame_;
    size_t nl = u[5], at = 6;
    while (at < u.size() && u[at] == 0x10 && nl) { at++; nl--; }   // as VbiDump.ax ("Error char")
    size_t p = 6 + u[5];
    if (at + nl > u.size() || nl == 0) return;
    for (size_t i = at; i < at + nl; i++) {
        uint8_t c = u[i];
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return;   // not a file name: not a file start
    }
    f.name = fromCp1251((const char*)&u[at], nl);
    size_t from;
    if (f.flags & 0x10) {
        if (p + 2 > u.size()) return;
        f.size = u[p] | u[p + 1] << 8;
        from = p + 2;
    } else {
        if (p + 6 > u.size()) return;
        f.size = (uint32_t)u[p + 2] | (uint32_t)u[p + 3] << 8 | (uint32_t)u[p + 4] << 16 | (uint32_t)u[p + 5] << 24;
        from = p + 4;
    }
    size_t got = u.size() - from;
    f.complete = got >= f.size;
    f.data.assign(u.begin() + from, u.begin() + from + std::min<size_t>(got, f.size));
    f.copies = f.complete ? 1 : 0;
    for (auto& g : itvFiles_)
        if (g.resource == f.resource && g.name == f.name && g.size == f.size) {
            if (f.complete) {
                if (!g.complete) { int n = g.copies; g = f; g.copies = n; }
                g.copies++;
            } else if (!g.complete && f.data.size() > g.data.size()) g.data = f.data;
            return;
        }
    if (itvFiles_.size() < 5000) itvFiles_.push_back(std::move(f));
}

void ItsIdl::finish()
{
    for (auto& kv : ch_)
        if (kv.second.inMsg) message(kv.second, true);
    itvEnd();
}

int64_t ItsIdl::good() const
{
    int64_t n = 0;
    for (auto& kv : ch_) n += kv.second.good;
    return n;
}

std::vector<std::string> ItsIdl::report() const
{
    std::vector<std::string> out;
    for (auto& kv : ch_) {
        const ItsChannel& c = kv.second;
        if (c.good < 3) continue;
        std::string s = fmt("magazine %d (data channel %X), service address %s, FT %X: %lld packets, %lld with right CRC (%lld mended), %lld repeats, %lld lost",
                            c.mag, (c.mag & 7) + 8, c.spa.c_str(), c.ft, (long long)c.packets, (long long)c.good, (long long)c.mended,
                            (long long)c.repeats, (long long)c.lost);
        std::string ty;
        for (auto& t : c.types) ty += (ty.empty() ? "" : ", ") + fmt("%d ", t.second) + t.first;
        if (!ty.empty()) s += "; messages: " + ty;
        out.push_back(s);
        if (!c.receivers.empty()) {
            std::string kn;
            for (auto& k : c.keyNumbers) kn += (kn.empty() ? "" : ", ") + fmt("%02X", k.first);
            out.push_back(fmt("  keys sent to subscribers: key %s to %zu receivers (numbers %06X..%06X)", kn.c_str(), c.receivers.size(),
                              *c.receivers.begin(), *c.receivers.rbegin()));
        }
        for (auto& m : c.open) {
            std::string h, t;
            for (size_t i = 0; i < m.size() && i < 48; i++) {
                h += fmt("%02X ", m[i]);
                t += m[i] >= 0x20 && m[i] < 0x7F ? (char)m[i] : '.';
            }
            out.push_back("  open message: " + h + " " + t);
        }
    }
    if (itvFound()) {
        out.push_back(fmt("InterTVNet (packets 8/31 with BCH): %lld packets, %lld right (%lld corrected), %lld lost; %zu files",
                          (long long)itvPackets_, (long long)itvGood_, (long long)itvCorrected_, (long long)itvLost_, itvFiles_.size()));
        for (auto& f : itvFiles_)
            out.push_back(fmt("  resource %d, kind %d, flags %02X: %s, %u bytes, %s", f.resource, f.kind, f.flags, f.name.c_str(), f.size,
                              f.complete ? fmt("complete (%d copies)", f.copies).c_str() : fmt("%zu bytes came", f.data.size()).c_str()));
    }
    return out;
}

ItsIdl itsIdlOf(const std::string& t42)
{
    ItsIdl c;
    if (!exists(t42)) return c;
    Bytes d = read_file(t42);
    for (size_t i = 0; i + 42 <= d.size(); i += 42) c.packet(&d[i], (int64_t)(i / 42 / 20));
    c.finish();
    return c;
}
