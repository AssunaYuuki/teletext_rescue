#include "wavetop.h"
#include "teletext.h"
#include <array>
#include <ctime>

namespace {
// ---------------------------------------------------------------- GF(256): многочлен 0x11D, образующий 0x1D
struct GF {
    int EXP[512], LOG[256];
    GF() {
        auto mul = [](int a, int b) { int r = 0; while (b) { if (b & 1) r ^= a; a <<= 1; if (a & 0x100) a ^= 0x11D; b >>= 1; } return r; };
        int x = 1;
        for (int i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x = mul(x, 0x1D); }
        for (int i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
        LOG[0] = 0;
    }
    int pw(int i) const { return EXP[((i % 255) + 255) % 255]; }
    int mul(int a, int b) const { return (a && b) ? EXP[(LOG[a] + LOG[b]) % 255] : 0; }
    int div(int a, int b) const { return a ? EXP[((LOG[a] - LOG[b]) % 255 + 255) % 255] : 0; }
};
const GF &gf() { static const GF g; return g; }

using Mat = std::array<std::array<int, 28>, 18>;
void syndromes(const Mat &M, int s0[28], int s1[28]) {
    const GF &G = gf();
    for (int c = 0; c < 28; c++) {
        int a = 0, b = 0;
        for (int i = 0; i < 18; i++) { a ^= G.mul(M[i][c], G.pw(i)); b ^= G.mul(M[i][c], G.pw(3 * i)); }
        s0[c] = a; s1[c] = b;
    }
}
bool fec_plain(Mat &M) {                         // без стираний: по одной ошибке на столбец
    const GF &G = gf(); int s0[28], s1[28]; syndromes(M, s0, s1);
    for (int c = 0; c < 28; c++) {
        if (!s0[c] && !s1[c]) continue;
        if (!s0[c] || !s1[c]) return false;
        int j = (G.LOG[G.div(s1[c], s0[c])] * 128) % 255;
        if (j >= 18) return false;
        M[j][c] ^= G.div(s0[c], G.pw(j));
    }
    return true;
}
bool fec_erase1(Mat &M, int j) {
    const GF &G = gf();
    for (int c = 0; c < 28; c++) M[j][c] = 0;
    int s0[28], s1[28]; syndromes(M, s0, s1);
    for (int c = 0; c < 28; c++) {
        int e = G.div(s0[c], G.pw(j));
        if (G.mul(e, G.pw(3 * j)) != s1[c]) return false;
        M[j][c] = e;
    }
    return true;
}
bool fec_erase2(Mat &M, int j, int k) {
    const GF &G = gf();
    for (int c = 0; c < 28; c++) M[j][c] = M[k][c] = 0;
    int s0[28], s1[28]; syndromes(M, s0, s1);
    int a = G.pw(j), b = G.pw(k), cc = G.pw(3 * j), d = G.pw(3 * k);
    int det = G.mul(a, d) ^ G.mul(b, cc);
    if (!det) return false;
    for (int c = 0; c < 28; c++) {
        M[j][c] = G.div(G.mul(s0[c], d) ^ G.mul(b, s1[c]), det);
        M[k][c] = G.div(G.mul(a, s1[c]) ^ G.mul(cc, s0[c]), det);
    }
    return true;
}

// ---------------------------------------------------------------- запись B: 16×26 байт
const std::vector<u8> &key() {
    static const std::vector<u8> K = [] {
        const uint32_t taps[4] = {0x40000000, 0x400000, 0x200000, 0x100000};
        std::vector<u8> out; uint32_t s = 0xffffffff;
        for (int i = 12; i <= 4095; i++) {
            int fb = 0; for (uint32_t t : taps) if (s & t) fb ^= 1;
            s >>= 1; if (fb) s |= 0x80000000;
            if (i % 32 == 0) for (int b = 3; b >= 0; b--) out.push_back((u8)(s >> (8 * b)));
        }
        s >>= 1; for (int b = 3; b >= 0; b--) out.push_back((u8)(s >> (8 * b)));
        out.resize(416);
        return out;
    }();
    return K;
}
uint16_t crc_arc(const u8 *d, size_t n) { uint16_t c = 0; while (n--) { c ^= *d++; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1); } return c; }
uint32_t crc32(const u8 *d, size_t n) { uint32_t c = 0xffffffff; while (n--) { c ^= *d++; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; } return ~c; }
uint32_t le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Record { int type = 0, ci = 0; uint32_t svc = 0; int fid = -1; uint32_t off = 0; Bytes payload; int field = 0, line = 0, addr = 0; };
bool record_from_bundle(const Mat &M, Record &r) {
    u8 rec[416];
    const auto &K = key();
    for (int i = 0; i < 16; i++) for (int c = 0; c < 26; c++) rec[i * 26 + c] = (u8)(M[i + 2][c] ^ K[i * 26 + c]);
    if (rec[0] != 0xAA || rec[1] != 0x55) return false;
    int t = rec[4], n = rec[7] | (rec[8] << 8);
    if ((t != 1 && t != 2) || n > 403) return false;
    std::vector<u8> z(rec, rec + 13 + n); z[2] = z[3] = 0;
    if (crc_arc(z.data(), z.size()) != (uint16_t)(rec[2] | (rec[3] << 8))) return false;
    r.type = t; r.ci = rec[5] | (rec[6] << 8); r.svc = le32(rec + 9);
    Bytes data(rec + 13, rec + 13 + n);
    if (t == 2 && n >= 6) { r.fid = data[0] | (data[1] << 8); r.off = le32(&data[2]); r.payload.assign(data.begin() + 6, data.end()); }
    return true;
}

// ---------------------------------------------------------------- распаковка PKWARE DCL («explode»)
struct Huff { int count[14] = {0}; std::vector<int> sym; };
Huff construct(std::initializer_list<int> rep) {
    std::vector<int> len;
    for (int b : rep) for (int k = 0; k <= (b >> 4); k++) len.push_back(b & 15);
    Huff h; for (int l : len) h.count[l]++;
    int offs[15] = {0}; for (int l = 1; l < 13; l++) offs[l + 1] = offs[l] + h.count[l];
    h.sym.assign(len.size(), 0);
    for (size_t s = 0; s < len.size(); s++) if (len[s]) h.sym[offs[len[s]]++] = (int)s;
    return h;
}
int explode(const Bytes &in, Bytes &out) {       // 0 — до конца, 2 — данные кончились, <0 — ошибка
    static const Huff LIT = construct({11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
                                       7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
                                       44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45, 44, 173});
    static const Huff LEN = construct({2, 35, 36, 53, 38, 23});
    static const Huff DIST = construct({2, 20, 53, 230, 247, 151, 248});
    static const int BASE[16] = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
    static const int EXTRA[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    size_t pos = 0; uint32_t buf = 0; int cnt = 0;
    struct Eof {};
    auto bits = [&](int k) -> int {
        uint32_t v = buf;
        while (cnt < k) { if (pos >= in.size()) throw Eof{}; v |= (uint32_t)in[pos++] << cnt; cnt += 8; }
        buf = v >> k; cnt -= k;
        return (int)(v & ((1u << k) - 1));
    };
    auto decode = [&](const Huff &h) -> int {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 14; l++) {
            code |= bits(1) ^ 1;
            int c = h.count[l];
            if (code < first + c) return h.sym[index + code - first];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        return -1;
    };
    try {
        int lit = bits(8); if (lit > 1) return -1;
        int dict = bits(8); if (dict < 4 || dict > 6) return -2;
        for (;;) {
            if (bits(1)) {
                int s = decode(LEN); if (s < 0) return -4;
                int len = BASE[s] + bits(EXTRA[s]);
                if (len == 519) return 0;
                int sh = len == 2 ? 2 : dict;
                int ds = decode(DIST); if (ds < 0) return -4;
                size_t dist = ((size_t)ds << sh) + bits(sh) + 1;
                if (dist > out.size()) return -3;
                for (int k = 0; k < len; k++) out.push_back(out[out.size() - dist]);
            } else {
                int c = lit ? decode(LIT) : bits(8);
                if (c < 0) return -4;
                out.push_back((u8)c);
            }
            if (out.size() > 64u << 20) return -5;
        }
    } catch (Eof &) { return 2; }
}

std::string safe_part(std::string s) {
    for (auto &c : s) if (strchr("<>:\"|?*", c) || (unsigned char)c < 0x20) c = '_';
    return s;
}
}

Json wavetop_decode(const std::string &t33, const std::string &out_dir, Progress &pr) {
    Bytes raw = read_file(t33);
    size_t n = raw.size() / 33;
    std::vector<int> field(n), line(n, 0);
    Bytes pos = exists(t33 + ".pos") ? read_file(t33 + ".pos") : Bytes();
    for (size_t i = 0; i < n; i++) {
        if (pos.size() >= (i + 1) * 8) { field[i] = (int)le32(&pos[i * 8]); line[i] = (int)le32(&pos[i * 8 + 4]); }
        else field[i] = (int)i;
    }
    return wavetop_decode_packets(raw, field, line, basename(t33), out_dir, pr);
}

Json wavetop_decode_packets(const Bytes &raw, const std::vector<int> &field, const std::vector<int> &line, const std::string &t33,
                            const std::string &out_dir, Progress &pr) {
    Json rep = Json::object();
    size_t n = std::min(raw.size() / 33, field.size());
    if (!n) return rep;
    auto pk = [&](size_t i) { return &raw[i * 33]; };
    auto nib = [&](size_t i, int k) { return ham::dec[pk(i)[k]]; };
    static const std::set<int> CONTROL = {0xFFF, 0xFC0, 0xEF0, 0x111, 0xFF1, 0x300};
    // пачки по строкам
    struct Bundle { int line, addr, f0; int slots[18]; };
    std::vector<Bundle> B;
    std::map<int, std::vector<size_t>> by_line;
    for (size_t i = 0; i < n; i++) by_line[line[i]].push_back(i);
    for (auto &kv : by_line) {
        auto &idx = kv.second;
        size_t m = idx.size();
        for (size_t s = 0; s < m; s++) {
            size_t is = idx[s];
            int d[5]; for (int k = 0; k < 5; k++) d[k] = nib(is, k);
            if (d[4] != 15 || d[0] < 0 || d[1] < 0 || d[2] < 0 || d[3] < 0) continue;
            int A = d[0] * 256 + d[1] * 16 + d[2];
            if (CONTROL.count(A)) continue;
            Bundle b{kv.first, A, field[is], {}}; for (int &q : b.slots) q = -1;
            b.slots[0] = (int)is;
            int c0 = d[3], lastci = c0, posn = 0;
            for (size_t k = s + 1; k < std::min(m, s + 40); k++) {
                size_t ik = idx[k];
                if (field[ik] - field[is] > 32) break;
                int dk[5]; for (int q = 0; q < 5; q++) dk[q] = nib(ik, q);
                int mt = (dk[0] == d[0]) + (dk[1] == d[1]) + (dk[2] == d[2]);
                bool full = dk[0] >= 0 && dk[1] >= 0 && dk[2] >= 0;
                if (full && mt < 3) {
                    int ak = dk[0] * 256 + dk[1] * 16 + dk[2];
                    if (CONTROL.count(ak)) continue;
                    if (dk[4] == 15) break;
                    if (mt < 2) continue;
                } else if (mt < 2) continue;
                int ci = dk[3] >= 0 ? dk[3] : (lastci + 1) % 16;
                int p = ((ci - c0) % 16 + 16) % 16, q;
                if (p == 1 && posn < 1) q = 1; else if (p >= 2) q = p; else q = p + 16;
                if (q <= posn) { if (q + 16 < 18 && q + 16 > posn) q += 16; else continue; }
                if (q < 18 && b.slots[q] < 0) { b.slots[q] = (int)ik; posn = q; lastci = ci; }
                if (posn >= 17) break;
            }
            B.push_back(b);
        }
    }
    // защита и записи
    std::vector<Record> recs; std::map<std::string, int> st;
    for (auto &b : B) {
        std::vector<int> er; for (int i = 0; i < 18; i++) if (b.slots[i] < 0) er.push_back(i);
        if (er.size() > 2) { st["too many missing packets"]++; continue; }
        Mat M0;
        for (int i = 0; i < 18; i++) for (int c = 0; c < 28; c++) M0[i][c] = b.slots[i] >= 0 ? pk(b.slots[i])[5 + c] : 0;
        Record r; bool ok = false;
        { Mat X = M0; bool f = er.empty() ? fec_plain(X) : er.size() == 1 ? fec_erase1(X, er[0]) : fec_erase2(X, er[0], er[1]); ok = f && record_from_bundle(X, r); }
        if (!ok && er.size() <= 1) {                // поиск по парам стираний
            for (int j = 0; j < 18 && !ok; j++) for (int k = j + 1; k < 18 && !ok; k++) {
                if (er.size() == 1 && j != er[0] && k != er[0]) continue;
                Mat X = M0; if (fec_erase2(X, j, k) && record_from_bundle(X, r)) { ok = true; st["recovered by erasure search"]++; }
            }
        }
        if (!ok) { st["uncorrectable"]++; continue; }
        st["ok"]++; r.field = b.f0; r.line = b.line; r.addr = b.addr; recs.push_back(r);
    }
    // время из служебного канала (адреса FC0/EF0: полезная нагрузка кодом Хэмминга)
    std::vector<std::pair<int, uint32_t>> ts;
    for (size_t i = 0; i < n; i++) {
        bool good = true; u8 b[14];
        for (int k = 0; k < 5; k++) if (nib(i, k) < 0) good = false;
        for (int k = 0; k < 14 && good; k++) { int lo = nib(i, 5 + 2 * k), hi = nib(i, 6 + 2 * k); if (lo < 0 || hi < 0) good = false; else b[k] = (u8)((hi << 4) | lo); }
        if (!good || b[0] != 2 || b[1] != 2) continue;
        uint32_t u = le32(b + 5);
        if (u > 600000000u && u < 1100000000u) ts.push_back({field[i], u});
    }
    if (recs.empty() && ts.empty()) { rep["bundles"] = (double)B.size(); return rep; }
    make_dirs(out_dir);
    std::string txt = "WaveTop datacast in " + t33 + "\n";
    txt += fmt("bundles %zu:", B.size()); for (auto &kv : st) txt += fmt("  %s %d", kv.first.c_str(), kv.second); txt += "\n";
    if (!ts.empty()) {
        auto tstr = [](uint32_t u) { time_t t = (time_t)u; char s[64]; struct tm *g = gmtime(&t); if (!g) return std::string(); strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S UTC", g); return std::string(s); };
        txt += "broadcast time (control channel): " + tstr(ts.front().second) + " \xE2\x80\x93 " + tstr(ts.back().second) + "\n";
    }
    // файлы
    std::map<std::pair<uint32_t, int>, std::map<uint32_t, Bytes>> files;
    for (auto &r : recs) if (r.type == 2 && r.fid >= 0) files[{r.svc, r.fid}].emplace(r.off, r.payload);
    int nok = 0; Json list = Json::array();
    for (auto &kv : files) {
        auto parts = kv.second;
        Json e = Json::object(); e["service"] = (double)kv.first.first; e["file_id"] = (double)kv.first.second;
        if (!parts.count(0) || parts[0].size() < 24) { e["status"] = "no first record"; list.push(e); continue; }
        Bytes pre(parts[0].begin(), parts[0].begin() + 24); parts[0].erase(parts[0].begin(), parts[0].begin() + 24);
        uint32_t total = le32(&pre[8]);
        size_t ext = 0; for (auto &p : parts) ext = std::max(ext, (size_t)p.first + p.second.size());
        size_t len = total < 50000000 ? std::max(ext, (size_t)total) : ext;
        Bytes w(len, 0); std::vector<char> have(len, 0);
        for (auto &p : parts) { memcpy(&w[p.first], p.second.data(), p.second.size()); memset(&have[p.first], 1, p.second.size()); }
        if (total > 0 && total < 50000000) { w.resize(total); have.resize(total); }
        std::string tname; for (int k = 14; k < 24 && pre[k]; k++) tname += (char)pre[k];
        e["sent_as"] = tname;
        // контейнер WTZY: путь, сжатый размер, CRC-32, имя
        bool wtz = w.size() > 16 && !memcmp(w.data(), "WTZY", 4);
        if (wtz) {
            size_t j = 4; while (j < w.size() && w[j]) j++;
            int plen = atoi(std::string(w.begin() + 4, w.begin() + j).c_str());
            size_t p = j + 1 + plen;
            if (p + 10 + 214 > w.size()) wtz = false;
            else {
                std::string path(w.begin() + j + 1, w.begin() + j + 1 + plen);
                uint32_t csize = le32(&w[p]), crc = le32(&w[p + 4]); p += 10;
                std::string name, run;
                for (size_t k = p; k < p + 190; k++) { if (w[k] >= 0x20 && w[k] < 0x7f) run += (char)w[k]; else { if (!run.empty()) name = run; run.clear(); } }
                if (!run.empty()) name = run;
                if (name.empty()) name = "unnamed";
                size_t d0 = p + 214, end = std::min(w.size(), d0 + csize);
                bool complete = end == d0 + csize; size_t upto = end;
                for (size_t k = d0; k < end; k++) if (!have[k]) { complete = false; upto = k; break; }
                Bytes out; int rc = upto > d0 ? explode(Bytes(w.begin() + d0, w.begin() + upto), out) : 2;
                bool ok = complete && rc == 0 && crc32(out.data(), out.size()) == crc;
                std::string rel = replace_all(path, "\\", "/");
                if (lower(rel).rfind("cache/", 0) == 0) rel = rel.substr(6);
                std::string dir = out_dir;
                for (auto &seg : split(rel, '/')) if (!seg.empty() && seg != "." && seg != "..") dir = path_join(dir, safe_part(seg));
                make_dirs(dir);
                std::string fn = safe_part(name) + (ok ? "" : ".partial");
                if (!out.empty()) write_file(path_join(dir, fn), out);
                e["path"] = path; e["name"] = fn; e["status"] = ok ? "ok" : complete ? "complete but bad" : "partial"; e["bytes"] = (double)out.size();
                nok += ok;
                txt += fmt("  %-8s ", ok ? "OK" : complete ? "BAD" : "PARTIAL") + path + "\\" + name + fmt("  (%zu bytes)\n", out.size());
            }
        }
        if (!wtz) {
            std::string dir = path_join(out_dir, "other"); make_dirs(dir);
            std::string fn = fmt("%u_%d_", kv.first.first, kv.first.second) + safe_part(tname);
            write_file(path_join(dir, fn), w);
            e["status"] = "raw"; e["name"] = fn;
        }
        list.push(e);
    }
    txt = txt + fmt("files %zu, recovered intact %d\n", files.size(), nok);
    write_text(path_join(out_dir, "wavetop.txt"), txt);
    rep["files"] = list; rep["bundles"] = (double)B.size(); rep["records"] = (double)recs.size(); rep["ok"] = (double)nok;
    rep["report"] = path_join(out_dir, "wavetop.txt");
    pr.log(fmt("WaveTop: %zu bundles, %zu records, %zu files (%d intact)", B.size(), recs.size(), files.size(), nok));
    return rep;
}
