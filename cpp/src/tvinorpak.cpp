#include "tvinorpak.h"
#include "util.h"
#include "faxg4.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <cstring>
#include <ctime>

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

// product code: rows first (bad rows become erasures), then the 28 columns. When the bundle cannot be
// corrected, the rows whose own check passed are still returned (ok[] per packet).
bool decodeBundle(Bundle& b, TviNorpak& o, std::array<uint8_t, 364>& data, std::array<bool, 14>& ok)
{
    ok.fill(false);
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
    auto keepRows = [&]() {
        for (int ci = 0; ci < 14; ci++)
            if (std::find(bad.begin(), bad.end(), ci) == bad.end()) {
                std::copy(b.r[ci].begin(), b.r[ci].begin() + 26, data.begin() + ci * 26);
                ok[ci] = true;
                o.rowsKept++;
            } else if (b.have[ci])   // a packet that failed its check: its bytes as read, marked not intact
                std::copy(b.r[ci].begin(), b.r[ci].begin() + 26, data.begin() + ci * 26);
        return false;
    };
    if (bad.size() > 2) {
        // more bad packets than the 2 FEC packets can rebuild: iterative product decoding - a column with
        // one wrong byte is corrected wherever that byte is; then the rows are checked again (a row with
        // one wrong byte left is corrected), and again, while it helps. Taken only when every row checks
        const std::array<Row, 16> saved = b.r;
        auto posOf = [](int ci) { return ci >= 14 ? ci - 14 : ci + 2; };
        auto rowsCheck = [&](std::vector<int>& bd) {
            bd.clear();
            for (int i = 0; i < 16; i++) {
                int w[28];
                w[0] = b.r[i][26]; w[1] = b.r[i][27];
                for (int c = 0; c < 26; c++) w[2 + c] = b.r[i][c];
                bool fx;
                if (fix1(w, 28, fx)) {
                    b.r[i][26] = (uint8_t)w[0]; b.r[i][27] = (uint8_t)w[1];
                    for (int c = 0; c < 26; c++) b.r[i][c] = (uint8_t)w[2 + c];
                } else bd.push_back(i);
            }
        };
        std::vector<int> bd = bad;
        for (int it = 0; it < 6 && !bd.empty(); it++) {
            size_t before = bd.size();
            for (int c = 0; c < 28; c++) {
                int v[16];
                for (int ci = 0; ci < 16; ci++) v[posOf(ci)] = b.r[ci][c];
                bool good, fx;
                if (bd.size() == 2) good = erase2(v, 16, posOf(bd[0]), posOf(bd[1]));
                else if (bd.size() == 1) good = erase1(v, 16, posOf(bd[0]));
                else good = fix1(v, 16, fx);
                if (good) for (int ci = 0; ci < 16; ci++) b.r[ci][c] = (uint8_t)v[posOf(ci)];
            }
            rowsCheck(bd);
            if (bd.size() >= before && bd.size() > 2) break;
        }
        bool clean = bd.empty();
        if (clean)
            for (int c = 0; c < 28 && clean; c++) {   // and every column is a codeword
                int v[16], s0, s1;
                for (int ci = 0; ci < 16; ci++) v[posOf(ci)] = b.r[ci][c];
                synd(v, 16, s0, s1);
                clean = !s0 && !s1;
            }
        if (clean) {
            o.rowsErased += (int64_t)bad.size();
            for (int ci = 0; ci < 14; ci++) std::copy(b.r[ci].begin(), b.r[ci].begin() + 26, data.begin() + ci * 26);
            ok.fill(true);
            return true;
        }
        b.r = saved;
        return keepRows();
    }
    const std::array<Row, 16> saved = b.r;
    auto posOf = [](int ci) { return ci >= 14 ? ci - 14 : ci + 2; };   // code position of a packet
    for (int c = 0; c < 28; c++) {
        int v[16];
        for (int ci = 0; ci < 16; ci++) v[posOf(ci)] = b.r[ci][c];
        bool good = false, fx;
        if (bad.size() == 2) good = erase2(v, 16, posOf(bad[0]), posOf(bad[1]));
        else if (bad.size() == 1) good = erase1(v, 16, posOf(bad[0]));
        else good = fix1(v, 16, fx);
        if (!good) { b.r = saved; return keepRows(); }
        for (int ci = 0; ci < 16; ci++) b.r[ci][c] = (uint8_t)v[posOf(ci)];
    }
    o.rowsErased += (int64_t)bad.size();
    for (int ci = 0; ci < 14; ci++) std::copy(b.r[ci].begin(), b.r[ci].begin() + 26, data.begin() + ci * 26);
    ok.fill(true);
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

bool intact(const TviNorpak& o, size_t i, size_t n)
{
    if (i + n > o.stream.size()) return false;
    for (size_t k = 0; k < n; k++)
        if (!o.valid[i + k]) return false;
    return true;
}

// CRC-16/ARC (polynomial 0x8005 reflected, init 0): records carry one over their data (bytes 15-16) and
// one over the header from the type byte to the data CRC (bytes 3..16, stored in 17-18)
uint16_t crc16arc(const uint8_t* p, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    return c;
}

// files in the stream: descriptions (type 7) and 256-byte blocks (type 3) of each file id. A record counts
// when both its CRCs match (bytes of a bundle that could not be corrected included); of the copies of a
// block (the file goes round as a carousel) the most frequent one wins
// a file block no copy of which came intact: the carousel sent it several times, each copy damaged in
// other packets. The copies sit in the record slot next to intact records of the neighbouring blocks
// (275 bytes on, 550 past a beacon). A packet row that failed its check in one copy is put right from
// the bytes of the others (the row's own 2 check bytes have to agree); a copy whose data matches the
// CRC is taken as it is (only its marker was hit); else the bytes are put together from all copies
// (bytes of packets that passed their check first, else the most common byte) and a few bytes the
// copies disagree on are tried in turn. Taken only when the block's CRC-16 matches: the one in a
// copy's header, or the one the header CRC gives with the rest of the header known.
bool joinBlock(const TviNorpak& o, uint32_t id, int block, int len, std::vector<uint8_t>& out)
{
    const auto& S = o.stream;
    auto u16 = [&](size_t i) { return (uint32_t)(S[i] | S[i + 1] << 8); };
    // the records follow each other every 275 bytes (19 + 256; a beacon record may stand between): the
    // copies of the block are found next to intact records of blocks block-1 and block+1, their own header
    // has to agree with what it should say in at least 8 of its 15 known bytes
    uint8_t want[15] = {0xD5, 0xF3, 0xC9, 3, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24), 0, (uint8_t)block, (uint8_t)(block >> 8), 0, 0, (uint8_t)len, (uint8_t)(len >> 8)};
    auto score = [&](size_t c) {
        if (c + 19 + len > S.size()) return 0;
        int s = 0;
        for (int k : {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 13, 14}) s += S[c + k] == want[k];
        return s;
    };
    // a record whose header passed its CRC is what it says it is
    auto intact = [&](size_t c) {
        return c + 19 <= S.size() && S[c] == 0xD5 && S[c + 1] == 0xF3 && S[c + 2] == 0xC9 && crc16arc(&S[c + 3], 14) == u16(c + 17);
    };
    // the record slot next to an intact record: 275 bytes on, or 550 when a beacon record (type 6) takes that slot
    auto beaconLike = [&](size_t c) {
        if (c + 19 > S.size()) return false;
        if (intact(c)) return S[c + 3] == 6;
        int s = 0;
        for (int k = 0; k < 3; k++) s += S[c + k] == want[k];
        return S[c + 3] == 6 && s >= 2;
    };
    std::set<size_t> cand;
    for (size_t i = 0; i + 19 <= S.size(); i++) {
        if (!(S[i] == 0xD5 && S[i + 1] == 0xF3 && S[i + 2] == 0xC9 && S[i + 3] == 3) || !intact(i)) continue;
        if ((u16(i + 4) | u16(i + 6) << 16) != id) continue;
        int nb = (int)u16(i + 9);
        if (nb == block) cand.insert(i);
        if (nb != block - 1 && nb != block + 1) continue;
        size_t c = SIZE_MAX;
        if (nb == block - 1) c = beaconLike(i + 275) ? i + 550 : i + 275;
        else if (i >= 550) c = beaconLike(i - 275) ? i - 550 : i - 275;
        else if (i >= 275) c = i - 275;
        if (c == SIZE_MAX || c + 19 + len > S.size()) continue;
        // an intact header there names another record: not a copy of this block
        if (intact(c) && !(S[c + 3] == 3 && (u16(c + 4) | u16(c + 6) << 16) == id && (int)u16(c + 9) == block)) continue;
        if (score(c) >= 4) cand.insert(c);
    }
    std::vector<size_t> at(cand.begin(), cand.end());
    if (at.size() < 2) return false;
    std::map<uint32_t, int> crcs;
    for (size_t i : at) crcs[u16(i + 15)] += 1 + 2 * (o.valid[i + 15] && o.valid[i + 16]);
    // the data CRC from the header CRC (bytes 17-18), the rest of the header being known: for copies whose
    // data CRC bytes were hit but not the header CRC
    {
        uint8_t h[14] = {3, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24), 0, (uint8_t)block, (uint8_t)(block >> 8), 0, 0, (uint8_t)len, (uint8_t)(len >> 8), 0, 0};
        for (size_t i : at)
            for (uint32_t c = 0; c < 65536; c++) {
                h[12] = (uint8_t)c; h[13] = (uint8_t)(c >> 8);
                if (crc16arc(h, 14) == u16(i + 17)) { crcs[c] += 1; break; }
            }
    }
    // the copies' bytes; a packet row that failed its own check in one copy is put right from the others:
    // the bytes where the copies differ are taken from either copy until the row's 2 check bytes agree
    std::vector<std::vector<uint8_t>> cb(at.size()), cv(at.size());
    for (size_t q = 0; q < at.size(); q++) {
        cb[q].assign(S.begin() + at[q] + 19, S.begin() + at[q] + 19 + len);
        cv[q].assign(o.valid.begin() + at[q] + 19, o.valid.begin() + at[q] + 19 + len);
    }
    for (size_t q = 0; q < at.size(); q++) {
        size_t a0 = at[q] + 19;
        for (size_t r = a0 / 26; r * 26 < a0 + len; r++) {
            if (o.valid[r * 26] || r * 3 + 2 >= o.checks.size() || !o.checks[r * 3 + 2]) continue;
            int w[28];
            w[0] = o.checks[r * 3]; w[1] = o.checks[r * 3 + 1];
            std::vector<std::pair<int, std::vector<uint8_t>>> alt;   // row position, values the copies have
            for (int j = 0; j < 26; j++) {
                size_t p = r * 26 + j;
                w[2 + j] = S[p];
                if (p < a0 || p >= a0 + len) continue;
                std::set<uint8_t> v{S[p]};
                for (size_t q2 = 0; q2 < at.size(); q2++) if (q2 != q) v.insert(cb[q2][p - a0]);
                if (v.size() > 1) alt.push_back({2 + j, std::vector<uint8_t>(v.begin(), v.end())});
            }
            double combos = 1;
            for (auto& x : alt) combos *= (double)x.second.size();
            if (alt.empty() || combos > 4096) continue;
            std::vector<int> idx(alt.size(), 0);
            int found = 0, fw[28];
            for (;;) {
                for (size_t t = 0; t < alt.size(); t++) w[alt[t].first] = alt[t].second[idx[t]];
                int s0, s1;
                synd(w, 28, s0, s1);
                if (!s0 && !s1) { found++; std::copy(w, w + 28, fw); }
                size_t t = 0;
                while (t < idx.size() && ++idx[t] >= (int)alt[t].second.size()) idx[t++] = 0;
                if (t == idx.size()) break;
            }
            if (found != 1) continue;
            for (int j = 0; j < 26; j++) {
                size_t p = r * 26 + j;
                if (p >= a0 && p < a0 + len) { cb[q][p - a0] = (uint8_t)fw[2 + j]; cv[q][p - a0] = 1; }
            }
        }
    }
    // a copy that is whole by itself (only its marker or type byte was hit, ORT 2002 block 281)
    for (size_t q = 0; q < at.size(); q++)
        if (crcs.count(crc16arc(cb[q].data(), len))) { out = cb[q]; return true; }
    out.assign(len, 0);
    std::vector<std::pair<int, std::vector<uint8_t>>> amb;   // byte, its candidates (most common first)
    for (int k = 0; k < len; k++) {
        std::map<uint8_t, int> good, all;
        for (size_t q = 0; q < at.size(); q++) { all[cb[q][k]]++; if (cv[q][k]) good[cb[q][k]]++; }
        auto& use = good.empty() ? all : good;
        std::vector<std::pair<int, uint8_t>> v;
        for (auto& c : use) v.push_back({-c.second, c.first});
        std::sort(v.begin(), v.end());
        out[k] = v[0].second;
        if (good.empty() && all.size() > 1) {
            std::vector<uint8_t> c;
            for (auto& x : v) c.push_back(x.second);
            amb.push_back({k, c});
        }
    }
    for (auto& cr : crcs) {
        if (crc16arc(out.data(), len) == cr.first) return true;
        // up to 3 doubtful bytes, up to 4 values each
        if (amb.empty() || amb.size() > 3) continue;
        std::vector<uint8_t> t = out;
        std::vector<int> idx(amb.size(), 0);
        for (;;) {
            size_t j = 0;
            while (j < idx.size() && ++idx[j] >= (int)std::min<size_t>(4, amb[j].second.size())) idx[j++] = 0;
            if (j == idx.size()) break;
            for (size_t q = 0; q < amb.size(); q++) t[amb[q].first] = amb[q].second[idx[q]];
            if (crc16arc(t.data(), len) == cr.first) { out = t; return true; }
        }
    }
    return false;
}

void norpakFiles(TviNorpak& o, const std::string& outDir)
{
    const auto& S = o.stream;
    auto u16 = [&](size_t i) { return (uint32_t)(S[i] | S[i + 1] << 8); };
    auto u32 = [&](size_t i) { return u16(i) | u16(i + 2) << 16; };
    std::map<uint32_t, TviFile> desc;
    std::map<uint32_t, std::map<int, std::map<std::vector<uint8_t>, int>>> blocks;
    std::map<uint32_t, std::vector<uint8_t>> index;   // type 1 record of a file id (sent before its description)
    for (size_t i = 0; i + 19 <= S.size(); i++) {
        if (!(S[i] == 0xD5 && S[i + 1] == 0xF3 && S[i + 2] == 0xC9) || crc16arc(&S[i + 3], 14) != u16(i + 17)) continue;
        int type = S[i + 3];
        uint32_t id = u32(i + 4), len = u16(i + 13);
        if ((type != 1 && type != 3 && type != 7) || len > 512 || i + 19 + len > S.size() || crc16arc(&S[i + 19], len) != u16(i + 15)) continue;
        const size_t d = i + 19;
        if (type == 1) {
            index[id].assign(S.begin() + d, S.begin() + d + len);
        } else if (type == 3) {
            blocks[id][(int)u16(i + 9)][std::vector<uint8_t>(S.begin() + d, S.begin() + d + len)]++;
        } else if (len >= 60 && !desc.count(id)) {
            TviFile f;
            f.id = id;
            for (int k = 0; k < 12 && S[d + k] > ' '; k++) f.shortName += (char)S[d + k];
            uint64_t ft = (uint64_t)u32(d + 24) | (uint64_t)u32(d + 28) << 32;   // FILETIME, last written
            if (ft > 116444736000000000ULL) {
                time_t t = (time_t)((ft - 116444736000000000ULL) / 10000000ULL);
                struct tm g;
#ifdef _WIN32
                gmtime_s(&g, &t);
#else
                gmtime_r(&t, &g);
#endif
                f.time = fmt("%04d-%02d-%02d %02d:%02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
            }
            f.size = u32(d + 32);
            f.crc = u32(d + 36);
            f.blocks = (int)u16(d + 42);
            uint32_t nl = u16(d + 58);
            for (uint32_t k = 0; k < nl && 60 + k < len; k++) f.name += (char)S[d + 60 + k];
            bool clean = !f.name.empty();
            for (char c : f.name) clean &= c >= ' ' && c < 127 && !strchr("/\\:*?\"<>|", c);
            if (!clean) f.name = f.shortName;
            if (!f.name.empty() && f.size && f.blocks && (uint64_t)f.blocks * 256 >= f.size) desc[id] = f;
        }
    }
    for (auto& kv : desc) {
        TviFile f = kv.second;
        std::vector<uint8_t> data;
        auto& bl = blocks[kv.first];
        for (int b = 0; b < f.blocks; b++) {
            auto it = bl.find(b);
            const std::vector<uint8_t>* best = nullptr;
            int n = 0;
            // every block has 256 bytes, the last one the rest of the file
            size_t len = b + 1 < f.blocks ? 256 : f.size - (size_t)b * 256;
            if (it != bl.end())
                for (auto& c : it->second)
                    if (c.second > n && c.first.size() == len) { n = c.second; best = &c.first; }
            std::vector<uint8_t> joined;
            if (!best && joinBlock(o, kv.first, b, (int)len, joined)) { best = &joined; f.joined++; }
            if (best) { data.insert(data.end(), best->begin(), best->end()); f.have++; }
            else { data.insert(data.end(), len, 0); f.missing.push_back(b); }
        }
        data.resize(f.size);
        if (!outDir.empty() && f.have) {
            std::string dir = path_join(outDir, "tvinform_files");
            make_dirs(dir);
            f.path = path_join(dir, f.name);
            write_file(f.path, data);
            // a fax TIFF (Hydromet maps): a picture, and the TIFF again with a whole directory
            std::string note, stem = f.path.substr(0, f.path.rfind('.'));
            if (faxTiffToPng(data, stem + ".png", f.missing.empty() ? "" : stem + "_fixed.tif", note)) f.note = note + " -> " + stem + ".png";
        }
        o.files.push_back(f);
    }
    auto& R = o.report;
    if (o.files.empty()) return;
    R.push_back(fmt("  files sent (records D5 F3 C9 07 / 03): %zu", o.files.size()));
    for (auto& f : o.files) {
        R.push_back(fmt("    %s (%s)  %u bytes, written %s  - %d of %d blocks received", f.name.c_str(), f.shortName.c_str(), f.size, f.time.c_str(),
                        f.have, f.blocks) + (f.joined ? fmt(" (%d of them put together from damaged copies, CRC checked)", f.joined) : "") +
                    (f.path.empty() ? "" : "  -> " + f.path));
        if (!f.note.empty()) R.push_back("      " + f.note);
        // index record: 6-byte entries 00 00, 01 AE / 01 AF, u16 (ORT 2002: 27 of them); not sums or CRCs of
        // block groups, not times - shown as bytes
        auto ix = index.find(f.id);
        if (ix != index.end() && !ix->second.empty()) {
            std::string m;
            for (size_t k = 0; k < ix->second.size(); k++) m += fmt(k % 6 == 0 && k ? " %02X" : "%02X", ix->second[k]);
            R.push_back(fmt("      index record (D5 F3 C9 01, not decoded), %zu bytes: ", ix->second.size()) + m);
        }
        if (!f.missing.empty()) {
            std::string m;
            for (int b : f.missing) m += (m.empty() ? "" : " ") + fmt("%d", b);
            R.push_back("      blocks not received in this recording (zeros in the file): " + m);
        }
    }
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
    // every bundle goes into the stream (364 bytes) so the records keep their places; bytes of packets
    // that are missing or failed their own check are marked invalid
    auto flush = [&](int a) {
        Bundle& b = cur[a];
        int have = 0;
        for (bool h : b.have) have += h;
        if (have >= 4) {
            o.bundles++;
            std::array<uint8_t, 364> d{};
            std::array<bool, 14> ok{};
            if (decodeBundle(b, o, d, ok)) o.bundlesOk++;
            at.push_back({o.stream.size(), b.frame});
            o.stream.insert(o.stream.end(), d.begin(), d.end());
            for (int ci = 0; ci < 14; ci++) o.valid.insert(o.valid.end(), 26, ok[ci] ? 1 : 0);
            for (int ci = 0; ci < 14; ci++) {
                o.checks.push_back(b.r[ci][26]); o.checks.push_back(b.r[ci][27]); o.checks.push_back(b.have[ci]);
            }
        }
        b = Bundle();
    };
    // ТВ-информ sends a packet in every slot (frame x line 7, 8, 9, 320, 321, 322) and the CI counts the slots
    // round 0..15 (ORT 2002). So with the slot of every packet known (.pos), its CI and bundle come from the
    // slot: a packet whose header lost bits (9% on ORT 2002) is still used, its row check tells if its 28
    // bytes are good; a CI "corrected" to a wrong value no longer cuts a bundle in two.
    bool slotMode = n > 0 && pos.size() >= n * 8;
    if (slotMode) {
        auto exact = [](uint8_t b) { for (int v = 0; v < 16; v++) if (H84[v] == b) return v; return -1; };
        std::vector<int64_t> slot(n);
        // the lines of the service: those with a packet in (nearly) every frame. A stray packet read on
        // another line (sample.vbi: one on 21 and one on 334) is not a slot of the counter
        std::map<int, int64_t> perLine;
        for (size_t i = 0; i < n; i++) perLine[pos[i * 8 + 4] | pos[i * 8 + 5] << 8]++;
        int64_t most = 0;
        for (auto& kv : perLine) most = std::max(most, kv.second);
        std::map<int, int> lineIx;
        int L = 0;
        for (auto& kv : perLine) if (kv.second * 10 >= most) lineIx[kv.first] = L++;
        std::map<int, int64_t> addrN;
        std::vector<int> addr(n, -1), ph(n, -1);
        std::vector<char> test(n, 0);
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = &pos[i * 8];
            int64_t fr = q[0] | q[1] << 8 | q[2] << 16 | (int64_t)q[3] << 24;
            auto li = lineIx.find(q[4] | q[5] << 8);
            slot[i] = li == lineIx.end() ? -1 : fr * L + li->second;
            const uint8_t* p = &raw[i * 33];
            if (slot[i] < 0) continue;
            if (testPacket(p)) { test[i] = 1; continue; }
            int h0 = exact(p[0]), h1 = exact(p[1]), h2 = exact(p[2]), h3 = exact(p[3]);
            if (h0 < 0 || h1 < 0 || h2 < 0) continue;
            addr[i] = h0 * 256 + h1 * 16 + h2;
            addrN[addr[i]]++;
            if (h3 >= 0) ph[i] = (int)(((h3 - slot[i]) % 16 + 16) % 16);
        }
        int dom = -1;
        for (auto& kv : addrN) if (dom < 0 || kv.second > addrN[dom]) dom = kv.first;
        // the phase of the counter: the most common one of the 40 nearest packets with an exact header
        std::vector<size_t> rel;
        for (size_t i = 0; i < n; i++) if (ph[i] >= 0 && addr[i] == dom) rel.push_back(i);
        size_t r = 0;
        int64_t curB = INT64_MIN;
        Bundle bb;
        auto flushB = [&] { cur[dom] = bb; flush(dom); bb = Bundle(); };
        for (size_t i = 0; i < n && !rel.empty(); i++) {
            o.packets++;
            if (test[i]) { o.testPackets++; continue; }
            if (slot[i] < 0) continue;
            if (addr[i] >= 0 && addr[i] != dom) { perAddr[addr[i]]++; continue; }   // another channel, read exactly
            perAddr[dom]++;
            while (r + 1 < rel.size() && rel[r + 1] <= i) r++;
            int cnt[16] = {};
            size_t a0 = r >= 20 ? r - 20 : 0, a1 = std::min(rel.size(), r + 21);
            for (size_t k = a0; k < a1; k++) cnt[ph[rel[k]]]++;
            int phase = (int)(std::max_element(cnt, cnt + 16) - cnt);
            int64_t base = slot[i] + phase, bid = base >= 0 ? base / 16 : -1;
            int ci = (int)(base % 16);
            if (bid != curB) { if (curB != INT64_MIN) flushB(); curB = bid; }
            if (!bb.have[ci]) {
                std::copy(&raw[i * 33] + 5, &raw[i * 33] + 33, bb.r[ci].begin());
                bb.have[ci] = true;
                bb.frame = slot[i] / L;
            }
        }
        if (curB != INT64_MIN) flushB();
        cur.clear();
    }
    for (size_t i = 0; i < n && !slotMode; i++) {
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
    size_t nvalid = 0;
    for (size_t i = 0; i < S.size(); i++)
        if (o.valid[i]) { nvalid++; zeros += S[i] == 0; }
    for (size_t i = 0; i + 100 <= S.size(); i++) {
        if (!(S[i] == 0xD5 && S[i + 1] == 0xF3 && S[i + 2] == 0xC9 && S[i + 3] == 0x06) || !intact(o, i, 100)) continue;
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
        int most = 0;
        for (auto& b : bc) most = std::max(most, b.n);
        for (auto& b : bc) {
            // a version seen a few times among thousands is a damaged copy that passed the checks
            if (b.n < 3 || b.n * 50 < most) continue;
            std::string name;
            int best = 0;
            for (auto& kv : b.names) if (kv.second > best || (kv.second == best && kv.first.size() > name.size())) { best = kv.second; name = kv.first; }
            R.push_back(fmt("    \"%s\"  updated %s  version %08X  - %d times, frames %lld..%lld", name.c_str(), b.time.c_str(), b.ver, b.n,
                            (long long)b.first, (long long)b.lastF));
            o.beacons.push_back(fmt("\"%s\"  updated %s  version %08X  - %d times", name.c_str(), b.time.c_str(), b.ver, b.n));
        }
    }
    if (nvalid)
        R.push_back(fmt("  data stream: %zu bytes (%zu intact; %lld packets kept from bundles that could not be corrected), %.0f%% zero bytes",
                        S.size(), nvalid, (long long)o.rowsKept, 100.0 * zeros / nvalid) +
                    (zeros * 10 >= nvalid * 9 ? " - the channel is idle: only the beacon between zero fill" : ""));
    norpakFiles(o, outDir);
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
