#include "tvinorpak.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <map>

namespace {
// ---------------------------------------------------------------- GF(256), polynomial 0x11D; b = 0x1D = a^8
struct GF {
    int EXP[512], LOG[256];
    GF()
    {
        int x = 1;
        for (int i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
        LOG[0] = 0;
    }
    int pw(int e) const { return EXP[((e % 255) + 255) % 255]; }
    int mul(int a, int b) const { return a && b ? EXP[LOG[a] + LOG[b]] : 0; }
    int div(int a, int b) const { return a ? EXP[(LOG[a] - LOG[b] + 255) % 255] : 0; }
};
const GF G;
constexpr int B = 8;

void synd(const int* v, int n, int& s0, int& s1)
{
    s0 = s1 = 0;
    for (int k = 0; k < n; k++)
        if (v[k]) { s0 ^= G.EXP[(G.LOG[v[k]] + B * k) % 255]; s1 ^= G.EXP[(G.LOG[v[k]] + 3 * B * k) % 255]; }
}
// one error corrected in place; false if the word cannot be a codeword with at most one error
bool fix1(int* v, int n, bool& fixed)
{
    fixed = false;
    int s0, s1;
    synd(v, n, s0, s1);
    if (!s0 && !s1) return true;
    if (!s0 || !s1) return false;
    int l2 = (G.LOG[s1] - G.LOG[s0] + 255) % 255;   // = 2Bk mod 255
    for (int k = 0; k < n; k++)
        if ((2 * B * k) % 255 == l2) {
            v[k] ^= G.div(s0, G.pw(B * k));
            synd(v, n, s0, s1);
            fixed = true;
            return !s0 && !s1;
        }
    return false;
}
bool erase1(int* v, int n, int j)
{
    v[j] = 0;
    int s0, s1;
    synd(v, n, s0, s1);
    int e = G.div(s0, G.pw(B * j));
    if (G.mul(e, G.pw(3 * B * j)) != s1) return false;
    v[j] = e;
    return true;
}
bool erase2(int* v, int n, int j, int k)
{
    v[j] = v[k] = 0;
    int s0, s1;
    synd(v, n, s0, s1);
    int a = G.pw(B * j), b = G.pw(B * k), c = G.pw(3 * B * j), d = G.pw(3 * B * k);
    int det = G.mul(a, d) ^ G.mul(b, c);
    if (!det) return false;
    v[j] = G.div(G.mul(s0, d) ^ G.mul(b, s1), det);
    v[k] = G.div(G.mul(a, s1) ^ G.mul(c, s0), det);
    return true;
}

const uint8_t H84[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
int ham(uint8_t b)
{
    for (int v = 0; v < 16; v++) if (popcount8(H84[v] ^ b) <= 1) return v;
    return -1;
}

using Row = std::array<uint8_t, 28>;
struct Bundle { std::array<Row, 16> r{}; std::array<bool, 16> have{}; int64_t frame = 0; };

// product code: rows first (bad rows become erasures), then the 28 columns
bool decodeBundle(Bundle& b, TviNorpak& o, std::array<uint8_t, 364>& data)
{
    std::vector<int> bad;
    for (int i = 0; i < 16; i++) {
        if (!b.have[i]) { bad.push_back(i); continue; }
        int w[28];
        w[0] = b.r[i][26]; w[1] = b.r[i][27];
        for (int c = 0; c < 26; c++) w[2 + c] = b.r[i][c];
        bool fx;
        if (fix1(w, 28, fx)) {
            if (fx) o.rowsFixed++;
            b.r[i][26] = (uint8_t)w[0]; b.r[i][27] = (uint8_t)w[1];
            for (int c = 0; c < 26; c++) b.r[i][c] = (uint8_t)w[2 + c];
        } else bad.push_back(i);
    }
    if (bad.size() > 2) return false;
    o.rowsErased += (int64_t)bad.size();
    auto posOf = [](int ci) { return ci >= 14 ? ci - 14 : ci + 2; };   // code position of a packet
    for (int c = 0; c < 28; c++) {
        int v[16];
        for (int ci = 0; ci < 16; ci++) v[posOf(ci)] = b.r[ci][c];
        bool ok, fx;
        if (bad.size() == 2) ok = erase2(v, 16, posOf(bad[0]), posOf(bad[1]));
        else if (bad.size() == 1) ok = erase1(v, 16, posOf(bad[0]));
        else ok = fix1(v, 16, fx);
        if (!ok) return false;
        for (int ci = 0; ci < 16; ci++) b.r[ci][c] = (uint8_t)v[posOf(ci)];
    }
    for (int ci = 0; ci < 14; ci++) std::copy(b.r[ci].begin(), b.r[ci].begin() + 26, data.begin() + ci * 26);
    return true;
}

std::string cp1251(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= 0xC0) s += from_cp(0x410 + (c - 0xC0));
        else if (c == 0xA8) s += from_cp(0x401);
        else if (c == 0xB8) s += from_cp(0x451);
        else if (c >= 0x20 && c < 0x7F) s += (char)c;
        else s += '?';
    }
    return s;
}

// test transmission packet: bytes are 7 bits + odd parity, each the previous shifted by one bit
// (a sliding window over the PRBS s[k+8] = s[k] ^ s[k+3])
bool testPacket(const uint8_t* p)
{
    int par = 0, ov = 0;
    for (int i = 6; i < 32; i++) par += odd_parity(p[i]);
    for (int i = 7; i < 32; i++) ov += ((p[i] & 0x3F) == ((p[i - 1] & 0x7F) >> 1));
    if (par < 22 || ov < 20) return false;
    std::vector<int> s;
    for (int k = 0; k < 7; k++) s.push_back((p[6] >> k) & 1);
    for (int i = 7; i < 32; i++) s.push_back((p[i] >> 6) & 1);
    int good = 0, n = 0;
    for (size_t k = 0; k + 8 < s.size(); k++, n++) good += s[k + 8] == (s[k] ^ s[k + 3]);
    return good * 10 >= n * 9;
}
}

bool tviNorpakDecode(const std::string& t33, const std::string& outDir, TviNorpak& o)
{
    if (!exists(t33)) return false;
    Bytes raw = read_file(t33), pos = exists(t33 + ".pos") ? read_file(t33 + ".pos") : Bytes();
    size_t n = raw.size() / 33;
    std::map<int, Bundle> cur;
    std::map<int, int> last;
    std::map<int, int64_t> perAddr;
    std::vector<std::pair<size_t, int64_t>> at;        // stream offset -> frame of its bundle
    auto flush = [&](int a) {
        Bundle& b = cur[a];
        int have = 0;
        for (bool h : b.have) have += h;
        if (have >= 14) {
            o.bundles++;
            std::array<uint8_t, 364> d;
            if (decodeBundle(b, o, d)) {
                o.bundlesOk++;
                at.push_back({o.stream.size(), b.frame});
                o.stream.insert(o.stream.end(), d.begin(), d.end());
            }
        }
        b = Bundle();
    };
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = &raw[i * 33];
        int h[5];
        bool ok = true;
        for (int k = 0; k < 5; k++) { h[k] = ham(p[k]); ok &= h[k] >= 0 || k == 4; }
        if (!ok) continue;
        o.packets++;
        if (testPacket(p)) { o.testPackets++; continue; }
        int a = h[0] * 256 + h[1] * 16 + h[2], ci = h[3];
        perAddr[a]++;
        auto it = last.find(a);
        if (it != last.end() && ci <= it->second) flush(a);
        Bundle& b = cur[a];
        if (!b.have[ci]) {
            std::copy(p + 5, p + 33, b.r[ci].begin());
            b.have[ci] = true;
            if (pos.size() >= (i + 1) * 8) b.frame = pos[i * 8] | pos[i * 8 + 1] << 8 | pos[i * 8 + 2] << 16 | (int64_t)pos[i * 8 + 3] << 24;
        }
        last[a] = ci;
    }
    for (auto& kv : cur) flush(kv.first);

    auto& R = o.report;
    R.push_back("\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC in packets (Norpak equipment: 55 55 E7 + 33 bytes, 5.73 Mbit/s; not a teletext service): " + basename(t33));
    std::string addrs;
    for (auto& kv : perAddr)
        if (kv.second * 50 >= o.packets) addrs += (addrs.empty() ? "" : ", ") + fmt("%03X (%lld packets)", kv.first, (long long)kv.second);
    R.push_back(fmt("  %lld packets", (long long)o.packets) + (addrs.empty() ? "" : "; channel address " + addrs));
    if (o.testPackets)
        R.push_back(fmt("  test transmission: %lld packets of the receiver test pattern (7-bit + odd parity bytes, a sliding window over the PRBS s[k+8] = s[k] xor s[k+3])",
                        (long long)o.testPackets));
    if (o.bundles)
        R.push_back(fmt("  error protection: product code over GF(256) - 2 check bytes per packet, 2 FEC packets (CI 14, 15) per bundle of 16; "
                        "%lld bundles, %lld corrected (%lld packets with one wrong byte fixed, %lld packets rebuilt from the FEC)",
                        (long long)o.bundles, (long long)o.bundlesOk, (long long)o.rowsFixed, (long long)o.rowsErased));
    // the source beacon: d5 f3 c9 06, type, version word, time of the last update, source name
    // one beacon per (update time, version); the name is voted - a record cut by a lost bundle reads short
    struct Beacon { std::string time; uint32_t ver; int64_t first, lastF; int n; std::map<std::string, int> names; };
    std::vector<Beacon> bc;
    const auto& S = o.stream;
    size_t zeros = 0;
    for (uint8_t c : S) zeros += c == 0;
    for (size_t i = 0; i + 100 <= S.size(); i++) {
        if (!(S[i] == 0xD5 && S[i + 1] == 0xF3 && S[i + 2] == 0xC9 && S[i + 3] == 0x06)) continue;
        const uint8_t* r = &S[i];
        auto u16 = [&](int k) { return r[k] | r[k + 1] << 8; };
        int Y = u16(55), Mo = u16(57), D = u16(59), hh = u16(61), mm = u16(63), ss = u16(65);
        if (Y < 1990 || Y > 2030 || Mo < 1 || Mo > 12 || D < 1 || D > 31 || hh > 23 || mm > 59 || ss > 59) continue;
        size_t L = 0;
        while (L < 29 && r[71 + L]) L++;
        if (L < 3) continue;
        std::string name = cp1251(r + 71, L);
        if (name.find('?') != std::string::npos) continue;
        std::string tm = fmt("%04d-%02d-%02d %02d:%02d:%02d", Y, Mo, D, hh, mm, ss);
        uint32_t ver = r[15] | r[16] << 8 | r[17] << 16 | (uint32_t)r[18] << 24;
        auto f = std::upper_bound(at.begin(), at.end(), std::make_pair(i, INT64_MAX));
        int64_t fr = f == at.begin() ? 0 : std::prev(f)->second;
        bool found = false;
        for (auto& b : bc)
            if (b.time == tm && b.ver == ver) { b.n++; b.lastF = fr; b.names[name]++; found = true; }
        if (!found) { bc.push_back({tm, ver, fr, fr, 1, {}}); bc.back().names[name] = 1; }
    }
    std::sort(bc.begin(), bc.end(), [](const Beacon& a, const Beacon& b) { return a.first < b.first; });
    if (!bc.empty()) {
        R.push_back("  source beacon (record D5 F3 C9 06, every 275 bytes of the stream): source name, time of the last update, version word:");
        for (auto& b : bc) {
            if (b.n < 3) continue;
            std::string name;
            int best = 0;
            for (auto& kv : b.names) if (kv.second > best || (kv.second == best && kv.first.size() > name.size())) { best = kv.second; name = kv.first; }
            R.push_back(fmt("    \"%s\"  updated %s  version %08X  - %d times, frames %lld..%lld", name.c_str(), b.time.c_str(), b.ver, b.n,
                            (long long)b.first, (long long)b.lastF));
        }
    }
    if (!S.empty())
        R.push_back(fmt("  data stream: %zu bytes, %.0f%% zero bytes", S.size(), 100.0 * zeros / S.size()) +
                    (zeros * 10 >= S.size() * 9 ? " - the channel is idle (no files sent): only the beacon between zero fill" : ""));
    if (!outDir.empty() && (o.bundlesOk || o.testPackets)) {
        make_dirs(outDir);
        if (!S.empty()) {
            write_file(path_join(outDir, "tvinform_stream.bin"), S);
            R.push_back("  stream written to " + path_join(outDir, "tvinform_stream.bin"));
        }
        std::string txt;
        for (auto& l : R) txt += l + "\n";
        write_text(path_join(outDir, "tvinform_norpak.txt"), txt);
    }
    return o.packets > 0;
}
