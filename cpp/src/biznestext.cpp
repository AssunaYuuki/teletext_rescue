#include "biznestext.h"
#include "util.h"
#include "finds.h"
#include "arjcheck.h"
#include "pcxfix.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
// Hamming 8/4 with one bit corrected
struct Ham {
    int d[256];
    Ham()
    {
        static const uint8_t code[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
        for (int& v : d) v = -1;
        for (int v = 0; v < 16; v++) d[code[v]] = v;
        for (int v = 0; v < 16; v++)
            for (int k = 0; k < 8; k++)
                if (d[code[v] ^ (1 << k)] < 0) d[code[v] ^ (1 << k)] = v;
    }
};
const Ham H;
int pairAt(const std::vector<uint8_t>& q, int i)   // byte i of a row sent as nibble pairs, low first
{
    int a = H.d[q[2 * i]], b = H.d[q[2 * i + 1]];
    return a < 0 || b < 0 ? -1 : a | b << 4;
}
template <class K> K best(const std::map<K, int>& m, K none)
{
    K k = none;
    int n = 0;
    for (auto& kv : m) if (kv.second > n) { n = kv.second; k = kv.first; }
    return k;
}
}

void BtCatalog::packet(const uint8_t* p, int64_t frame)
{
    int a = H.d[p[0]], b = H.d[p[1]];
    if (a < 0 || b < 0) return;
    int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
    std::vector<uint8_t> q(p + 2, p + 42);
    if (row == 31 && mag == 8) {   // the data channel
        if (idlFirst_ < 0) idlFirst_ = frame;
        idl_++;
        if (idlVote_.size() < 40) idlVote_.resize(40);
        for (int k = 0; k < 40; k++) idlVote_[k][(uint8_t)(q[k] & 0x7F)]++;
        return;
    }
    // magazines 5, 6 and 7 carry it in parallel (9.07.94: blocks 101+ of every file only on pages 7xx); other
    // magazines: the page of 5-7 stays open
    if (mag < 5 || mag > 7) return;
    Cur& cur = cur_[mag];
    if (row == 0) {
        int pu = H.d[q[0]], pt = H.d[q[1]];
        cur = Cur();
        cur.on = true;
        char pg[8];
        if (pu >= 0 && pt >= 0) snprintf(pg, sizeof pg, "%d%X%X", mag, pt, pu); else snprintf(pg, sizeof pg, "%d??", mag);
        cur.page = pg;
        return;
    }
    if (!cur.on || row < 1 || row > 23) return;
    // rows come in order and the catalogue rows 21-23 close a transmission: a lower row (or a data row after
    // them) is the next transmission whose header was lost - its rows are not this one's
    if (row <= 20) {
        if (row <= cur.last || cur.last >= 21) { cur.on = false; return; }
        cur.data[row] = q;
        cur.last = row;
        return;
    }
    cur.last = row;
    cur.rows[row] = q;
    if (cur.rows.size() < 3) return;
    auto r21 = cur.rows[21];
    auto r22 = cur.rows[22];
    auto r23 = cur.rows[23];
    std::string page = cur.page;
    cur.on = false;
    std::string nm;
    for (int k = 4; k < 15; k++) {
        int c = pairAt(r21, k);
        if (c < 32 || c > 126) return;
        nm += (char)c;
    }
    auto trim = [](std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); while (!s.empty() && s[0] == ' ') s.erase(0, 1); return s; };
    // 11 characters; a short name is not padded to 8 ("MESSAGECOD " = MESSAGE.COD): the last 3 are the extension
    std::string t = trim(nm);
    std::string name = t.size() > 3 ? t.substr(0, t.size() - 3) + "." + t.substr(t.size() - 3) : t;
    for (auto& c : name) c = (char)toupper((unsigned char)c);
    Entry& e = entries_[name];
    e.n++;
    if (page.find('?') == std::string::npos) e.page[page]++;
    if (e.first < 0) e.first = frame;
    e.last = frame;
    int bh = pairAt(r22, 1), bl = pairAt(r22, 2);
    if (bh >= 0 && bl >= 0) e.blocks[bh << 8 | bl]++;
    int s1 = pairAt(r21, 16), s2 = pairAt(r21, 17), s3 = pairAt(r21, 18);
    if (s1 >= 0 && s2 >= 0 && s3 >= 0 && s2 < 100 && s3 < 100) e.size[s1 * 10000L + s2 * 100 + s3]++;
    // the block being sent, decimal over bytes 3-5 (VINO.C01, 283 blocks: 00 00 63 -> 00 01 00 = 100)
    int n3 = pairAt(r22, 3), n4 = pairAt(r22, 4), n5 = pairAt(r22, 5);
    if (n3 >= 0 && n4 >= 0 && n5 >= 0 && n3 < 100 && n4 < 100 && n5 < 100) {
        int blk = n3 * 10000 + n4 * 100 + n5;
        e.sent.insert(blk);
        nowSent_ = name + fmt(" block %d", blk) + (page.find('?') == std::string::npos ? " (page " + page + ")" : "");
        nowFrame_ = frame;
        // a block number read wrong lands beyond the file (the count is in the same row)
        int count = bh >= 0 && bl >= 0 ? bh << 8 | bl : -1;
        bool added = false;
        if (blk > 0 && (count <= 0 || blk <= count) && weight_ > 0 && !done_.count(name))   // weight 0: the catalogue only
            for (auto& rq : cur.data) {
                added = true;
                // a row already held in kEnoughCopies copies is not taken again: the file goes round and round, the
                // capture keeps only what is new (and writes the files again only then)
                auto& to = data_[name][blk][rq.first];
                Row40 r;
                std::copy(rq.second.begin(), rq.second.end(), r.begin());
                // copies that still differ (a row received badly) keep coming up to 3x as many
                bool agree = true;
                for (auto& c : to) agree &= c.b == to[0].b;
                if ((int)to.size() < (agree ? kEnoughCopies : 3 * kEnoughCopies)) { to.push_back({r, weight_}); continue; }
                // a copy trusted more (a live 8-bit packet) takes the place of one trusted less
                auto low = std::min_element(to.begin(), to.end(), [](const Copy& a, const Copy& b) { return a.w < b.w; });
                if (low->w < weight_) *low = {r, weight_};
            }
        if (added && whole(name)) done_.insert(name);
    }
    std::string dg;
    for (int k = 6; k < 18; k++) {
        int c = pairAt(r22, k);
        if (c < '0' || c > '9') { dg.clear(); break; }
        dg += (char)c;
    }
    if (dg.size() == 12) e.date[dg.substr(0, 2) + "." + dg.substr(2, 2) + "." + dg.substr(4, 2) + " " + dg.substr(6, 2) + ":" + dg.substr(8, 2) + ":" + dg.substr(10, 2)]++;
    int ck[4];
    bool ok = true;
    for (int k = 0; k < 4; k++) ok &= (ck[k] = pairAt(r23, k)) >= 0;
    if (ok) e.check[fmt("%02X%02X%02X%02X", ck[0], ck[1], ck[2], ck[3])]++;
    // 1994: row 23 = 00 a b c, the sum of the block's 800 bytes as sent (a*10000 + b*100 + c)
    if (ok && ck[0] == 0 && ck[2] < 100 && ck[3] < 100 && n3 >= 0 && n4 >= 0 && n5 >= 0 && n3 < 100 && n4 < 100 && n5 < 100)
        e.blockSum[n3 * 10000 + n4 * 100 + n5][ck[1] * 10000L + ck[2] * 100 + ck[3]]++;
}

bool BtCatalog::whole(const std::string& name) const
{
    auto e = entries_.find(name);
    auto d = data_.find(name);
    if (e == entries_.end() || d == data_.end()) return false;
    int count = best<int>(e->second.blocks, -1);
    if (count <= 0 || count > 2000) return false;
    // a row is had when two of its copies are the same
    auto rowHad = [](const std::vector<Copy>& cs) {
        for (size_t i = 0; i < cs.size(); i++)
            for (size_t j = i + 1; j < cs.size(); j++) if (cs[i].b == cs[j].b) return true;
        return false;
    };
    for (int b = 1; b <= count; b++) {
        auto bl = d->second.find(b);
        if (bl == d->second.end() || bl->second.empty()) return false;
        // every block 20 rows; the last one may stop early, but from row 1 on without a hole
        int rows = 0;
        for (int r = 1; r <= 20; r++) {
            auto it = bl->second.find(r);
            if (it == bl->second.end()) { if (b < count) return false; break; }
            if (!rowHad(it->second)) return false;
            rows++;
        }
        if (b == count && rows < (int)bl->second.size()) return false;
    }
    return true;
}

std::vector<BtFile> BtCatalog::files(int minCopies) const
{
    std::vector<BtFile> out;
    for (auto& kv : entries_) {
        const Entry& e = kv.second;
        if (e.n < minCopies) continue;
        BtFile f;
        f.name = kv.first;
        f.copies = e.n;
        f.page = best<std::string>(e.page, "");
        f.date = best<std::string>(e.date, "");
        f.check = best<std::string>(e.check, "");
        f.blocks = best<int>(e.blocks, -1);
        f.size = best<long>(e.size, -1);
        for (int s : e.sent) if (s > 0 && (f.blocks < 0 || s <= f.blocks)) f.sent.insert(s);
        f.first = e.first; f.last = e.last;
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const BtFile& a, const BtFile& b) { return a.copies > b.copies; });
    // a name read with one wrong character (EX@MPCH1.ARJ) is the same file as a far more frequent one
    std::vector<BtFile> keep;
    for (auto& f : out) {
        bool dup = false;
        for (auto& k : keep) {
            if (k.name.size() != f.name.size() || k.copies < 3 * f.copies) continue;
            int diff = 0;
            for (size_t i = 0; i < f.name.size(); i++) diff += k.name[i] != f.name[i];
            if (diff == 1) { dup = true; k.sent.insert(f.sent.begin(), f.sent.end()); break; }
        }
        if (!dup) keep.push_back(f);
    }
    return keep;
}

std::string BtCatalog::dataText() const
{
    std::string s;
    for (auto& v : idlVote_) {
        uint8_t c = best<uint8_t>(v, ' ');
        s += c >= 32 && c < 127 ? (char)c : ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

BtCatalog btCatalogOf(const std::string& t42, int weight)
{
    BtCatalog c;
    c.setWeight(weight);
    if (!exists(t42)) return c;
    Bytes d = read_file(t42);
    for (size_t i = 0; i + 42 <= d.size(); i += 42) c.packet(&d[i], (int64_t)(i / 42 / 20));
    return c;
}

long BtCatalog::dataRows() const
{
    long n = 0;
    for (auto& f : data_) for (auto& b : f.second) for (auto& r : b.second) n += (long)r.second.size();
    return n;
}

bool BtCatalog::sameFiles(const BtCatalog& o) const
{
    for (auto& kv : entries_) {
        auto it = o.entries_.find(kv.first);
        if (it == o.entries_.end() || kv.second.n < 3 || it->second.n < 3) continue;
        if (best<std::string>(kv.second.date, "") == best<std::string>(it->second.date, "")) return true;
    }
    return false;
}

void BtCatalog::merge(const BtCatalog& o)
{
    for (auto& kv : o.entries_) {
        Entry& e = entries_[kv.first];
        const Entry& x = kv.second;
        e.n += x.n;
        for (auto& v : x.blocks) e.blocks[v.first] += v.second;
        for (auto& v : x.date) e.date[v.first] += v.second;
        for (auto& v : x.check) e.check[v.first] += v.second;
        for (auto& v : x.page) e.page[v.first] += v.second;
        for (auto& v : x.size) e.size[v.first] += v.second;
        for (auto& b : x.blockSum) for (auto& v : b.second) e.blockSum[b.first][v.first] += v.second;
        e.sent.insert(x.sent.begin(), x.sent.end());
    }
    for (auto& f : o.data_)
        for (auto& b : f.second)
            for (auto& r : b.second) {
                auto& to = data_[f.first][b.first][r.first];
                to.insert(to.end(), r.second.begin(), r.second.end());
            }
}

// ---------------------------------------------------------------- the files
std::string utf8Of866(uint8_t x)
{
    static const char16_t hi[] = u"АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмноп"
                                 u"░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀"
                                 u"рстуфхцчшщъыьэюяЁёЄєЇїЎў°∙·√№¤■ ";
    char16_t u = x < 0x80 ? x : hi[x - 0x80];
    std::string s;
    if (u < 0x80) s += (char)u;
    else if (u < 0x800) { s += (char)(0xC0 | (u >> 6)); s += (char)(0x80 | (u & 0x3F)); }
    else { s += (char)(0xE0 | (u >> 12)); s += (char)(0x80 | ((u >> 6) & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
    return s;
}
namespace {
// a byte of DOS text: printable ASCII, CP866 letters, line ends
bool textByte(int c) { return (c >= 0x20 && c < 0x7F) || (c >= 0x80 && c < 0xB0) || (c >= 0xE0 && c < 0xF2) || c == 13 || c == 10 || c == 9; }

// what the sender's disk left in a file (-1 = byte not received): FAT directory entries (names, sizes, dates of
// files that were on the disk) and every run of DOS text of 40+ characters, with offsets, as UTF-8
std::string leftoversReport(const std::string& name, const std::vector<int>& b)
{
    std::string dirs, texts;
    auto nameChar = [](int c, bool first) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("_!$#&@-~", c) || (first ? c == 0xE5 || c == '.' : c == ' ' || c == '.');
    };
    for (size_t i = 0; i + 32 <= b.size(); i++) {
        bool ok = true;
        for (int k = 0; k < 11 && ok; k++) ok = b[i + k] >= 0 && nameChar(b[i + k], k == 0);
        if (!ok) continue;
        int attr = b[i + 11];
        if (!strchr("\x01\x02\x03\x10\x20\x21\x22\x23\x27", attr) && attr != 0) continue;
        bool whole = true;
        for (int k = 22; k < 32; k++) whole &= b[i + k] >= 0;
        if (!whole) continue;
        int t = b[i + 22] | b[i + 23] << 8, dt = b[i + 24] | b[i + 25] << 8, cl = b[i + 26] | b[i + 27] << 8;
        long sz = (long)b[i + 28] | (long)b[i + 29] << 8 | (long)b[i + 30] << 16 | (long)b[i + 31] << 24;
        int y = 1980 + (dt >> 9), m = (dt >> 5) & 15, d = dt & 31;
        if (y < 1985 || y > 1996 || m < 1 || m > 12 || d < 1 || sz < 0 || sz > 50000000L) continue;
        std::string n, x;
        for (int k = 0; k < 8; k++) n += utf8Of866((uint8_t)b[i + k]);
        for (int k = 8; k < 11; k++) x += (char)b[i + k];
        while (!n.empty() && n.back() == ' ') n.pop_back();
        while (!x.empty() && x.back() == ' ') x.pop_back();
        dirs += fmt("@%-7lu %-8s %-3s %s  %02d.%02d.%d %02d:%02d  cluster %d%s\n", (unsigned long)i, n.c_str(), x.c_str(),
                    attr & 0x10 ? "    <DIR>" : fmt("%9ld", sz).c_str(), d, m, y, t >> 11, (t >> 5) & 63, cl,
                    b[i] == 0xE5 ? "  (deleted)" : "");
    }
    for (size_t i = 0; i < b.size();) {
        if (b[i] < 0 || !textByte(b[i])) { i++; continue; }
        size_t j = i;
        int letters = 0;
        while (j < b.size() && b[j] >= 0 && textByte(b[j])) {
            int c = b[j] | 0x20;
            letters += (c >= 'a' && c <= 'z') || (b[j] >= 0x80 && b[j] < 0xB0) || (b[j] >= 0xE0 && b[j] < 0xF2);
            j++;
        }
        if (j - i >= 40 && letters * 2 >= (int)(j - i)) {
            texts += fmt("\n--- @%lu (%lu chars)\n", (unsigned long)i, (unsigned long)(j - i));
            for (size_t k = i; k < j; k++)
                if (b[k] != 13) texts += b[k] == 10 || b[k] == 9 ? std::string(1, (char)b[k]) : utf8Of866((uint8_t)b[k]);
            texts += "\n";
        }
        i = j;
    }
    if (dirs.empty() && texts.empty()) return "";
    return name + ": what the sender's disk left in it (the file was made at its size, not written over)\n\n" +
           (dirs.empty() ? std::string() : "== FAT directory entries (files that were on the sender's disk)\n" + dirs + "\n") +
           "== text runs (40+ readable characters)\n" + texts;
}

struct KBlock { int base; std::array<int16_t, 800> c; };   // key offset of the block, bytes (-1 = not received)

// one key byte per position (key index = base + offset in the block), decided by the text of the files:
// started from "the commonest byte is a space", the case bit by a prior (lower-case letters and spaces win),
// then the letter pairs of the text read so far
std::vector<int> findKey(const std::vector<KBlock>& B, int len)
{
    std::vector<std::vector<std::pair<int, int>>> at(len);   // key index -> (block, offset)
    for (int b = 0; b < (int)B.size(); b++)
        for (int o = 0; o < 800; o++) if (B[b].c[o] >= 0) at[B[b].base + o].push_back({b, o});
    std::vector<int> K(len, 0);
    for (int k = 0; k < len; k++) {
        int n[256] = {0}, bi = 0;
        for (auto& s : at[k]) n[B[s.first].c[s.second]]++;
        for (int i = 1; i < 256; i++) if (n[i] > n[bi]) bi = i;
        K[k] = bi ^ 0x20;
    }
    auto pick = [&](int k, auto score, const std::vector<int>& cands) {
        int bestK = K[k];
        double bs = -1e300;
        for (int c : cands) {
            double s = 0;
            for (auto& sm : at[k]) s += score(sm.first, sm.second, B[sm.first].c[sm.second] ^ c);
            if (s > bs) { bs = s; bestK = c; }
        }
        return bestK;
    };
    std::vector<int> all(256);
    for (int i = 0; i < 256; i++) all[i] = i;
    std::vector<double> lu(256);
    auto unigram = [&]() {   // the characters of the current reading
        std::vector<double> u(256, 0.5);
        double t = 128;
        for (auto& b : B) for (int o = 0; o < 800; o++) if (b.c[o] >= 0) { u[b.c[o] ^ K[b.base + o]]++; t++; }
        for (int i = 0; i < 256; i++) lu[i] = std::log(u[i] / t);
    };
    for (int it = 0; it < 2; it++) {
        unigram();
        std::vector<int> nk(len);
        for (int k = 0; k < len; k++) nk[k] = pick(k, [&](int, int, int p) { return lu[p]; }, all);
        K = nk;
    }
    // the case bit (0x20 tells "e" from "E", "р" from "└", a space from 00)
    std::vector<double> pr(256, std::log(0.05));
    pr[0x20] = std::log(15.0);
    for (int c = 'a'; c <= 'z'; c++) pr[c] = std::log(4.0);
    for (int c = 'A'; c <= 'Z'; c++) pr[c] = std::log(1.5);
    for (int c = 0xA0; c < 0xB0; c++) pr[c] = std::log(4.0);
    for (int c = 0xE0; c < 0xF0; c++) pr[c] = std::log(4.0);
    for (int c = 0x80; c < 0xA0; c++) pr[c] = std::log(1.5);
    for (const char* q = "0123456789.,;:-()\"'!?/*"; *q; q++) pr[(uint8_t)*q] = 0;
    pr[13] = pr[10] = 0;
    for (int k = 0; k < len; k++) K[k] = pick(k, [&](int, int, int p) { return pr[p]; }, {K[k], K[k] ^ 0x20});
    for (int it = 0; it < 3; it++) {
        unigram();
        std::vector<double> pair(65536, 0.0), one(256, 0.0);
        double tot = 0;
        for (auto& b : B)
            for (int o = 0; o < 800; o++) {
                if (b.c[o] < 0) continue;
                int p = b.c[o] ^ K[b.base + o];
                one[p]++; tot++;
                if (o && b.c[o - 1] >= 0) pair[(b.c[o - 1] ^ K[b.base + o - 1]) << 8 | p]++;
            }
        auto lb = [&](int a, int c) { return std::log((pair[a << 8 | c] + 0.1 * (one[c] + 0.1) / tot) / (one[a] + 0.1)); };
        for (int k = 0; k < len; k++)
            K[k] = pick(k, [&](int b, int o, int p) {
                const KBlock& x = B[b];
                double s = o && x.c[o - 1] >= 0 ? lb(x.c[o - 1] ^ K[x.base + o - 1], p) : lu[p];
                if (o < 799 && x.c[o + 1] >= 0) { int nx = x.c[o + 1] ^ K[x.base + o + 1]; s += lb(p, nx) - lu[nx]; }
                return s;
            }, all);
    }
    return K;
}

// ---- 1996 (НТВ 25.05.96 tape): every byte of a file is sent as two Hamming 8/4 codewords (low nibble first), 400
// bytes a block, so a file takes twice its size. A column of the block (rows 1-20, 8 bits) holds 10 bytes: each half of
// the sent byte (lane = bits 0-3 or bits 4-7) is one run of 80 bits down the column - bit 0 of the lane in rows 1..20,
// then bit 1, ... - and codeword j of the lane is bits 8j..8j+7 of the run (the first one = the codeword's top bit as
// in the table). The lane of bits 0-3 carries bytes 0-4 of the column, the other bytes 5-9. Each codeword is XORed
// with a key byte of its own (800 a block, the same for every block and file); the nibble a codeword stands for
// then goes through kT96. The end of a file is filled with codewords of 0.
const uint8_t kHam[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
const int kT96[16] = {15, 1, 8, 6, 4, 10, 3, 13, 2, 12, 5, 11, 9, 7, 14, 0};
// a received byte of the block: bits 0-7 voted over the copies, bits 8-15 = bits the copies are split on (-1 = not received)
using Blk = std::array<int32_t, 800>;

// codeword j of a lane in column c; mask = its bits received (rows not received are erasures)
int cw96(const Blk& b, int c, int lane, int j, int& mask, int* split = nullptr)
{
    int x = 0;
    mask = 0;
    if (split) *split = 0;
    for (int t = 0; t < 8; t++) {
        int s = 8 * j + t, v = b[(s % 20) * 40 + c], bit = lane + s / 20;
        if (v < 0) continue;
        x |= ((v >> bit) & 1) << (7 - t);
        if (split && (v >> 8 >> bit & 1)) *split |= 1 << (7 - t);
        mask |= 1 << (7 - t);
    }
    return x;
}
// the nibble of a codeword: one error corrected, up to 3 erasures filled (distance 4); -1 = unreadable
int ham96(int x, int mask)
{
    int bd = 9, bv = -1, amb = 0, unk = 8 - __builtin_popcount(mask);
    for (int v = 0; v < 16; v++) {
        int d = __builtin_popcount((x ^ kHam[v]) & mask);
        if (d < bd) { bd = d; bv = v; amb = 1; } else if (d == bd) amb++;
    }
    if (amb > 1 || unk >= 4 || (bd > 0 && bd + unk >= 4)) return -1;
    return kT96[bv];
}

// one row over its copies, bit by bit with their weights: a bit the copies are split on evenly is kept as
// bit 8+ (an erasure for the 1996 codewords; 1994 takes the bit of the commonest copy)
template <class C> std::array<int32_t, 40> voteRow(const std::vector<const C*>& cs)
{
    std::array<int32_t, 40> out;
    for (int k = 0; k < 40; k++) {
        int n[256] = {0}, bi = 0, tot = 0, one[8] = {0};
        for (auto* x : cs) {
            n[x->b[k]] += x->w;
            tot += x->w;
            for (int i = 0; i < 8; i++) if (x->b[k] >> i & 1) one[i] += x->w;
        }
        for (int i = 1; i < 256; i++) if (n[i] > n[bi]) bi = i;
        int v = 0, split = 0;
        for (int i = 0; i < 8; i++) {
            if (2 * one[i] == tot) { split |= 1 << i; v |= bi & (1 << i); }
            else if (2 * one[i] > tot) v |= 1 << i;
        }
        out[k] = v | split << 8;
    }
    return out;
}

struct Key96 { std::array<uint8_t, 800> k; double text = 0, score = 0; std::string from; };   // k[(c*2 + lane/4)*10 + j]

// the blocks of the files read as 1996 codewords? For every codeword place the key byte that makes the most
// received codewords valid; base = that key (each of 16 equal choices, one taken); returns the share that are valid
double base96(const std::vector<const Blk*>& B, std::array<uint8_t, 800>& base)
{
    long ok = 0, n = 0;
    for (int c = 0; c < 40; c++)
        for (int l = 0; l < 2; l++)
            for (int j = 0; j < 10; j++) {
                int cnt[256] = {0}, m;
                for (auto* b : B) { int x = cw96(*b, c, 4 * l, j, m); if (m == 0xFF) cnt[x]++; }
                int bo = 0, bs = -1, tot = 0;
                for (int x = 0; x < 256; x++) tot += cnt[x];
                for (int o = 0; o < 256; o++) {
                    int s = 0;
                    for (auto h : kHam) s += cnt[h ^ o];
                    if (s > bs) { bs = s; bo = o; }
                }
                base[(c * 2 + l) * 10 + j] = (uint8_t)bo;
                ok += bs; n += tot;
            }
    return n ? (double)ok / n : 0;
}

// which of the 16 equal key bytes of each codeword: the one whose bytes read best as text (lp = log weight of a byte)
std::array<uint8_t, 800> solve96(const std::vector<const Blk*>& B, const std::array<uint8_t, 800>& base, const std::vector<double>& lp)
{
    std::array<uint8_t, 800> key = base;
    for (int c = 0; c < 40; c++)
        for (int l = 0; l < 2; l++)
            for (int p = 0; p < 5; p++) {
                int iL = (c * 2 + l) * 10 + 2 * p, iH = iL + 1;
                std::vector<int> lo[16], hi[16];
                for (int a = 0; a < 16; a++) {
                    int dL = kHam[a] ^ 0x15;   // the codewords of the code itself (a valid key stays valid)
                    for (auto* b : B) {
                        int m, x = cw96(*b, c, 4 * l, 2 * p, m);
                        lo[a].push_back(ham96(x ^ ((base[iL] ^ dL) & m), m));
                        x = cw96(*b, c, 4 * l, 2 * p + 1, m);
                        hi[a].push_back(ham96(x ^ ((base[iH] ^ dL) & m), m));
                    }
                }
                double bs = -1e300;
                for (int a = 0; a < 16; a++)
                    for (int h = 0; h < 16; h++) {
                        double s = 0;
                        for (size_t i = 0; i < B.size(); i++)
                            if (lo[a][i] >= 0 && hi[h][i] >= 0) s += lp[hi[h][i] << 4 | lo[a][i]];
                        if (s > bs) { bs = s; key[iL] = (uint8_t)(base[iL] ^ kHam[a] ^ 0x15); key[iH] = (uint8_t)(base[iH] ^ kHam[h] ^ 0x15); }
                    }
            }
    return key;
}

// the 400 bytes of a block (-1 = not readable)
std::array<int16_t, 400> bytes96(const Blk& b, const std::array<uint8_t, 800>& key)
{
    std::array<int16_t, 400> out;
    for (int c = 0; c < 40; c++)
        for (int l = 0; l < 2; l++)
            for (int p = 0; p < 5; p++) {
                int i = (c * 2 + l) * 10 + 2 * p, m1, m2, s1, s2;
                int x1 = cw96(b, c, 4 * l, 2 * p, m1, &s1), x2 = cw96(b, c, 4 * l, 2 * p + 1, m2, &s2);
                // a codeword that does not read: the bits its copies were split on taken as not received
                auto read = [&](int x, int m, int s, int k) {
                    int v = ham96(x ^ (k & m), m);
                    return v < 0 && s ? ham96(x ^ (k & m & ~s), m & ~s) : v;
                };
                int lo = read(x1, m1, s1, key[i]), hi = read(x2, m2, s2, key[i + 1]);
                out[c * 10 + l * 5 + p] = (int16_t)(lo < 0 || hi < 0 ? -1 : hi << 4 | lo);
            }
    return out;
}

// the key from the files themselves: per file, started from a prior of DOS text and refined twice by the bytes it
// reads; the file that reads most as text gives the key for all (binary files - ARJ, PCX - cannot tell it)
Key96 findKey96(const std::map<std::string, std::map<int, Blk>>& F, const std::array<uint8_t, 800>& base)
{
    std::vector<double> pr(256, std::log(0.05));
    pr[0x20] = std::log(15.0);
    for (int c = 'a'; c <= 'z'; c++) pr[c] = std::log(4.0);
    for (int c = 'A'; c <= 'Z'; c++) pr[c] = std::log(1.5);
    for (int c = 0xA0; c < 0xB0; c++) pr[c] = std::log(4.0);
    for (int c = 0xE0; c < 0xF0; c++) pr[c] = std::log(4.0);
    for (int c = 0x80; c < 0xA0; c++) pr[c] = std::log(1.5);
    for (const char* q = "0123456789.,;:-()\"'!?/*"; *q; q++) pr[(uint8_t)*q] = 0;
    pr[13] = pr[10] = 0;
    Key96 best;
    best.k = base;
    for (auto& f : F) {
        std::vector<const Blk*> B;
        for (auto& b : f.second) B.push_back(&b.second);
        if (B.size() < 8) continue;   // too few blocks to tell 256 choices apart
        auto key = solve96(B, base, pr);
        double share = 0;
        for (int it = 0; it < 2; it++) {
            std::vector<double> u(256, 0.5);
            double t = 128;
            long tx = 0, n = 0;
            for (auto* b : B)
                for (auto v : bytes96(*b, key)) if (v >= 0) { u[v]++; t++; n++; tx += textByte(v); }
            share = n ? (double)tx / n : 0;
            if (share < 0.5) break;   // not text: this file cannot give the key
            for (auto& x : u) x = std::log(x / t);
            key = solve96(B, base, u);
        }
        // 256 choices per byte place bend even a picture towards "text" (TOWER350.PCX: 92%); line ends next to each
        // other (0D 0A) do not come that way, a DOS text has one every line
        long tx = 0, n = 0, crlf = 0;
        for (auto* b : B) {
            auto d = bytes96(*b, key);
            for (int i = 0; i < 400; i++) {
                if (d[i] < 0) continue;
                n++; tx += textByte(d[i]);
                crlf += d[i] == 13 && i + 1 < 400 && d[i + 1] == 10;
            }
        }
        share = n ? (double)tx / n : 0;
        double lines = n ? (double)crlf / n : 0, score = share + (lines > 0.004 ? 1 : 0);
        if (score > best.score) { best.score = score; best.text = share; best.k = key; best.from = f.first; }
    }
    if (best.score < 1) best.from.clear();   // no file reads as DOS text with lines
    return best;
}
}

std::vector<BtWritten> BtCatalog::writeFiles(const std::string& dir, std::string& note,
                                             const std::function<void(const std::string&)>& status) const
{
    const std::string bt = "\xD0\x91\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x81-\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82";   // Бизнес-Текст
    auto say = [&](const std::string& s) { if (status) status(bt + ": " + s); };
    say(fmt("%zu files in the catalogue, putting their blocks together", data_.size()));
    std::vector<BtWritten> out;
    note.clear();
    // a name read with one wrong character joins the frequent one (as in files())
    auto fl = files(1);
    std::map<std::string, std::string> to;
    auto alnum = [](const std::string& n) { std::string r; for (char c : n) if (isalnum((unsigned char)c)) r += c; return r; };
    for (auto& kv : data_) {
        to[kv.first] = kv.first;
        auto me = entries_.find(kv.first);
        for (auto& f : fl) {
            if (f.name == kv.first || f.name.size() != kv.first.size()) continue;
            int d = 0;
            for (size_t i = 0; i < f.name.size(); i++) d += f.name[i] != kv.first[i];
            if (d == 1 && me != entries_.end() && f.copies >= 3 * me->second.n) { to[kv.first] = f.name; break; }
        }
        // the same letters and digits as a far more frequent name, a sign read wrong ("STROIT1C.01!" = STROIT1.C01
        // with the 11th character, a space, read as "!")
        if (to[kv.first] == kv.first && me != entries_.end())
            for (auto& f : fl)
                if (f.name != kv.first && alnum(f.name) == alnum(kv.first) && f.copies >= 3 * me->second.n) { to[kv.first] = f.name; break; }
        // a name that is no DOS name at all ("CHEKC01.  '": the catalogue row read badly) goes to the nearest
        // proper one, or is dropped
        auto dosName = [](const std::string& n) {
            int dots = 0;
            for (char c : n) {
                if (c == '.') { dots++; continue; }
                if (!isalnum((unsigned char)c) && !strchr("_-$~!#%&@^()", c)) return false;
            }
            return dots <= 1 && !n.empty();
        };
        if (!dosName(to[kv.first])) {
            std::string bestName;
            int bd = 4;
            for (auto& f : fl) {
                if (!dosName(f.name) || f.name.size() != kv.first.size()) continue;
                int d = 0;
                for (size_t i = 0; i < f.name.size(); i++) d += f.name[i] != kv.first[i];
                if (d < bd) { bd = d; bestName = f.name; }
            }
            to[kv.first] = bestName;
        }
    }
    // a name read wrong where the rules above miss it (PACE357.PCX = PAGE350.PCX, two characters; PAGE350P.CX! while
    // the true name is not yet heard 3 times as often): the same block count and size in the catalogue and, where
    // both have a row, the same bytes as sent (one key for all files: the same file is the same bytes)
    {
        auto rowsOf = [&](const std::string& n) { long k = 0; for (auto& b : data_.at(n)) k += (long)b.second.size(); return k; };
        auto catOf = [&](const std::string& n, int& blocks, long& size, int& copies) {
            auto e = entries_.find(n);
            blocks = e != entries_.end() ? best<int>(e->second.blocks, -1) : -1;
            size = e != entries_.end() ? best<long>(e->second.size, -1L) : -1;
            copies = e != entries_.end() ? e->second.n : 0;
        };
        auto agree = [&](const std::string& a, const std::string& b, long& n) {
            long eq = 0;
            n = 0;
            auto& A = data_.at(a);
            auto& B = data_.at(b);
            for (auto& blk : A) {
                auto ib = B.find(blk.first);
                if (ib == B.end()) continue;
                for (auto& r : blk.second) {
                    auto ir = ib->second.find(r.first);
                    if (ir == ib->second.end()) continue;
                    std::vector<const Copy*> ca, cb;
                    for (auto& x : r.second) ca.push_back(&x);
                    for (auto& x : ir->second) cb.push_back(&x);
                    auto va = voteRow(ca), vb = voteRow(cb);
                    for (int k = 0; k < 40; k++) { n++; eq += (va[k] & 0xFF) == (vb[k] & 0xFF); }
                }
            }
            return n ? (double)eq / n : 0.0;
        };
        for (auto& kv : data_) {
            const std::string& me = kv.first;
            if (to[me] != me) continue;
            int mb, mc; long ms;
            catOf(me, mb, ms, mc);
            long mr = rowsOf(me);
            std::string bestName;
            double bestAgree = 0;
            for (auto& o : data_) {
                const std::string& on = o.first;
                if (on == me || to[on] != on) continue;
                int ob, oc; long os;
                catOf(on, ob, os, oc);
                long orr = rowsOf(on);
                if (orr < mr || (orr == mr && oc <= mc)) continue;   // joins the copy received more
                if (mb > 0 && ob > 0 && mb != ob) continue;
                if (ms > 0 && os > 0 && ms != os) continue;
                if (ob <= 0 && os <= 0) continue;   // nothing in the catalogue to tell
                int diff = 0;
                if (on.size() == me.size()) for (size_t i = 0; i < on.size(); i++) diff += on[i] != me[i];
                else diff = 99;
                long n;
                double a = agree(me, on, n);
                // ten rows and more the same bytes: the same file whatever the name ("AVTOZAWM.01!" = AVTOZAP.C01,
                // the catalogue row slipped a character)
                if (diff > 2 && alnum(on) != alnum(me) && (n < 400 || a < 0.95)) continue;
                if (n >= 40 ? a < 0.9 : (diff > 2 || oc < 2 * mc)) continue;   // no common row: the name and counts alone
                if (n < 40) a = 0.5;
                if (a > bestAgree) { bestAgree = a; bestName = on; }
            }
            if (!bestName.empty()) to[me] = bestName;
        }
        for (auto& kv : to)   // a chain (A -> B -> C) ends at its last name
            for (int i = 0; i < 8 && !kv.second.empty() && to.count(kv.second) && to[kv.second] != kv.second; i++) kv.second = to[kv.second];
    }
    // file -> block -> 800 bytes voted over the copies (-1 = row not received)
    std::map<std::string, std::map<int, std::map<int, std::vector<const Copy*>>>> cp;
    for (auto& kv : data_)
        for (auto& b : kv.second)
            for (auto& r : b.second)
                for (auto& x : r.second) {
                    const std::string& nm = to[kv.first];
                    if (nm.empty()) continue;
                    // a block number read wrong beyond the file (AVTOZAP.C01 "block 10055" of 332): the count the
                    // catalogue gives most often decides, not the one of the same transmission
                    auto e = entries_.find(nm);
                    int count = e != entries_.end() ? best<int>(e->second.blocks, -1) : -1;
                    if (count > 0 && b.first > count) continue;
                    cp[nm][b.first][r.first].push_back(&x);
                }
    std::map<std::string, std::map<int, Blk>> F;
    std::map<std::string, std::map<int, std::array<int16_t, 800>>> alt;   // the byte of the next commonest copy (-1 = none)
    for (auto& f : cp)
        for (auto& b : f.second) {
            auto& blk = F[f.first][b.first];
            auto& al = alt[f.first][b.first];
            blk.fill(-1);
            al.fill(-1);
            for (auto& r : b.second) {
                auto v = voteRow(r.second);
                for (int k = 0; k < 40; k++) {
                    blk[(r.first - 1) * 40 + k] = v[k];
                    int n[256] = {0}, a = -1;
                    for (auto* x : r.second) n[x->b[k]] += x->w;
                    for (int i = 0; i < 256; i++)
                        if (n[i] && i != (v[k] & 0xFF) && (a < 0 || n[i] > n[a])) a = i;
                    al[(r.first - 1) * 40 + k] = (int16_t)a;
                }
            }
        }
    if (F.empty()) return out;
    // 1996: Hamming codewords spread over the columns (most received codewords valid under one key)
    std::vector<const Blk*> all;
    for (auto& f : F) for (auto& b : f.second) all.push_back(&b.second);
    std::array<uint8_t, 800> base{};
    say(fmt("%zu blocks of %zu files: which form of the service (1994 / 1996)", all.size(), F.size()));
    // a clean recording gives > 80% valid codewords, a tape received live much less (bit errors, split copies);
    // from 30% on the 1996 key is looked for, and a key under which a file reads as DOS text with lines decides
    double s96 = all.size() >= 8 ? base96(all, base) : 0;
    bool v96 = s96 > 0.8;
    Key96 k96;
    if (s96 > 0.3) {
        say(fmt("%.0f%% of the codewords can be valid: trying the 1996 form (two Hamming 8/4 codewords a byte)", 100 * s96));
        k96 = findKey96(F, base);
        if (!k96.from.empty()) v96 = true;
    }
    say(v96 ? "1996 form (two Hamming 8/4 codewords a byte)" : "1994 form (800-byte blocks): finding the key from the text of the files");
    // the key: the same for every block, or one per block number mod 8 - the one that reads as text
    std::vector<int> K;
    int P = 0;
    double bestText = 0;
    for (int per : {1, 8}) {
        if (v96) break;
        std::vector<KBlock> B;
        for (auto& f : F)
            for (auto& b : f.second) {
                KBlock k{((b.first - 1) % per) * 800, {}};
                for (int o = 0; o < 800; o++) k.c[o] = (int16_t)(b.second[o] < 0 ? -1 : b.second[o] & 0xFF);
                B.push_back(k);
            }
        long n = 0;
        for (auto& b : B) for (auto v : b.c) n += v >= 0;
        if (n < 800L * per * 40) continue;   // too few copies of each key byte to tell
        auto k = findKey(B, 800 * per);
        long t = 0;
        for (auto& b : B) for (int o = 0; o < 800; o++) if (b.c[o] >= 0) t += textByte(b.c[o] ^ k[b.base + o]);
        double share = (double)t / n;
        if (share > 0.6 && share > bestText + 0.03) { bestText = share; K = k; P = per; }
    }
    note = v96 ? fmt("each byte sent as two Hamming 8/4 codewords spread down the columns (400 bytes a block); key of 800 codewords "
                     "found from the text of %s (%.0f%% of its bytes read as DOS text)", k96.from.c_str(), 100 * k96.text)
         : P ? fmt("key of %d bytes found from the text of the files (%.0f%% of the bytes read as DOS text)", 800 * P, 100 * bestText)
             : std::string("encrypted; too little of the files received to find the key");
    if (v96 && k96.from.empty()) note = "each byte sent as two Hamming 8/4 codewords (1996); no file reads as text to find the key";
    // 1994: a block whose bytes as sent do not add up to the catalogue's sum (row 23) is mended when exactly one byte
    // where the copies differ makes it right with the other copy's value
    std::map<std::string, std::array<int, 3>> sumStat;   // file -> right, mended, wrong
    if (!v96)
        for (auto& f : F) {
            auto e = entries_.find(f.first);
            if (e == entries_.end()) continue;
            for (auto& b : f.second) {
                auto s = e->second.blockSum.find(b.first);
                if (s == e->second.blockSum.end()) continue;
                long want = best<long>(s->second, -1L), have = 0;
                bool whole = true;
                for (auto v : b.second) { if (v < 0) { whole = false; break; } have += v & 0xFF; }
                if (!whole || want < 0) continue;
                auto& st = sumStat[f.first];
                if (have == want) { st[0]++; continue; }
                auto& al = alt[f.first][b.first];
                int at = -1, n = 0;
                for (int k = 0; k < 800; k++)
                    if (al[k] >= 0 && al[k] - (b.second[k] & 0xFF) == want - have) { at = k; n++; }
                if (n == 1) { b.second[at] = al[at]; st[1]++; }
                else st[2]++;
            }
        }
    const int bb = v96 ? 400 : 800;   // file bytes a block
    // the file's bytes of a block, -1 = not received / not readable
    auto plain = [&](int blk, const Blk& raw) {
        std::vector<int16_t> d(bb, -1);
        if (v96) {
            if (k96.from.empty()) return d;
            auto x = bytes96(raw, k96.k);
            std::copy(x.begin(), x.end(), d.begin());
        } else
            for (int o = 0; o < 800; o++) if (raw[o] >= 0) d[o] = (int16_t)(P ? (raw[o] & 0xFF) ^ K[((blk - 1) % P) * 800 + o] : raw[o] & 0xFF);
        return d;
    };
    make_dirs(dir);
    std::string rep;
    size_t fileNo = 0;
    for (auto& f : F) {
        say(fmt("decrypting and writing %s (%zu of %zu, %zu blocks)", f.first.c_str(), ++fileNo, F.size(), f.second.size()));
        BtWritten w;
        w.name = f.first;
        w.blockBytes = bb;
        if (sumStat.count(f.first)) { auto& st = sumStat[f.first]; w.sumRight = st[0]; w.sumMended = st[1]; w.sumWrong = st[2]; }
        auto e = entries_.find(f.first);
        if (e != entries_.end()) w.blocks = best<int>(e->second.blocks, -1);
        long size = e != entries_.end() ? best<long>(e->second.size, -1L) : -1;
        // the blocks the file really has: its size in blocks (9.07.94: the catalogue's count is 156 more for every
        // file - VINO.C01 101192 bytes = 127 blocks, "283"; blocks 101+ come on pages 7xx)
        if (size > 0) w.blocks = (int)((size + bb - 1) / bb);
        int last = f.second.rbegin()->first;
        int nb = std::max(last, w.blocks);
        // one key for all files: every file is decrypted (PASSWORD.COD then shows its repeating structure);
        // a text of its own (.txt) when the most of it reads as DOS text
        w.decrypted = v96 ? !k96.from.empty() : P != 0;
        std::map<int, std::vector<int16_t>> D;
        for (auto& b : f.second) D[b.first] = plain(b.first, b.second);
        // a block carrying another block's data (25.05.96: "block 12" of PAGE350.PCX = block 28, "113" of TOWER350.PCX
        // = 129, each sent whole on page 601 - the generator's page changed before its catalogue rows) is not the
        // block it says: always the data of the block 16 further on (the round of 16 pages), so block b is taken as
        // not received when it reads as block b+16 (neighbouring blocks of disk leftovers may well be alike)
        std::vector<int> dropped;
        for (auto& x : D) {
            auto y = D.find(x.first + 16);
            if (y == D.end()) continue;
            int n = 0, eq = 0;
            std::set<int> vals;
            for (int o = 0; o < bb; o++)
                if (x.second[o] > 0 && y->second[o] > 0) { n++; eq += x.second[o] == y->second[o]; vals.insert(x.second[o]); }
            if (n >= 40 && eq >= 0.8 * n && vals.size() >= 16) {
                std::fill(x.second.begin(), x.second.end(), (int16_t)-1);
                dropped.push_back(x.first);
            }
        }
        bool isText = false;
        if (w.decrypted) {
            long t = 0, n = 0;
            for (auto& b : D) for (auto v : b.second) if (v >= 0) { n++; t += textByte(v); }
            isText = n && t >= 0.6 * n;
            // byte entropy of every block received (decrypted): packed data ~7.8 bits, text and code below 7
            int packed = 0, other = 0;
            for (auto& b : D) {
                std::map<int, int> cnt;
                int n = 0;
                for (auto v : b.second) if (v >= 0) { cnt[v]++; n++; }
                if (n < bb / 2) continue;
                double h = 0;
                for (auto& kv : cnt) { double q = (double)kv.second / n; h -= q * std::log2(q); }
                (h > (v96 ? 7.0 : 7.3) ? packed : other)++;   // 400 bytes cannot show more than ~7.4 bits
            }
            // random bytes are 64% "DOS text" codes: packed data is no text
            if (packed + other && packed >= 0.9 * (packed + other)) isText = false;
            if (packed + other)
                w.content = v96 ? (packed >= 0.9 * (packed + other) ? "compressed (archive / packed)" : isText ? "text" : "data")
                          : packed >= 0.9 * (packed + other) ? "compressed data of the service"
                          : other >= 0.9 * (packed + other) ? (isText ? "disk leftovers (text)" : "disk leftovers")
                          : "disk leftovers with packed parts";
        }
        Bytes bin;
        std::string txt;
        std::vector<int> txtIn;   // the file's bytes, -1 = row not received
        long& bad = w.bytesBad;   // bytes of the blocks received that did not come or did not read
        for (int b = 1; b <= nb; b++) {
            auto it = f.second.find(b);
            auto dt = D.find(b);
            if (it != f.second.end()) w.blocksHave++;
            for (int r = 0; r < 20; r++) {
                bool have = it != f.second.end() && it->second[r * 40] >= 0;
                if (b <= last) { w.rows++; w.rowsHave += have; }
            }
            for (int o = 0; o < bb; o++) {
                int v = dt != D.end() ? dt->second[o] : -1;
                if (dt != D.end() && v < 0 && (size <= 0 || (long)bin.size() < size)) bad++;
                bin.push_back(v >= 0 ? (uint8_t)v : 0);
                txtIn.push_back(v);
            }
        }
        // the catalogue's size is the file's length (the end of the last block is filler)
        if (size > 0 && (long)bin.size() > size) { bin.resize(size); txtIn.resize(size); }
        // the text: runs of DOS text of 6+ characters, each from a new line (a dictionary keeps ~60 bytes of
        // binary offsets after every article - left out); rows not received as "[...]", a few bytes that did not
        // read (bit errors of the tape) as "·" in their place, so the line stays whole
        if (isText) {
            const int kGap = -2;
            for (size_t i = 0; i < txtIn.size();) {
                if (txtIn[i] >= 0) { i++; continue; }
                size_t j = i;
                while (j < txtIn.size() && txtIn[j] < 0) j++;
                if (j - i < 40) for (size_t k = i; k < j; k++) txtIn[k] = kGap;
                i = j;
            }
            auto nl = [&]() { if (!txt.empty() && txt.back() != '\n') txt += '\n'; };
            auto isTxt = [&](int v) { return v == kGap || (v >= 0 && textByte(v)); };
            size_t i = 0;
            while (i < txtIn.size()) {
                if (txtIn[i] == -1) {
                    while (i < txtIn.size() && txtIn[i] == -1) i++;
                    nl();
                    txt += "[...]\n";
                    continue;
                }
                size_t j = i;
                while (j < txtIn.size() && isTxt(txtIn[j])) j++;
                if (j - i >= 6) {
                    nl();
                    for (size_t k = i; k < j; k++)
                        if (txtIn[k] == kGap) txt += "\xC2\xB7";
                        else if (txtIn[k] != 13) txt += txtIn[k] == 10 || txtIn[k] == 9 ? std::string(1, (char)txtIn[k]) : utf8Of866((uint8_t)txtIn[k]);
                }
                i = j > i ? j : i + 1;
            }
        }
        std::string safe = w.name;
        for (auto& c : safe) if (strchr("\\/:*?\"<>|", c)) c = '_';
        w.path = path_join(dir, safe);
        write_file(w.path, bin);
        if (isText) { w.text = w.path + ".txt"; write_file(w.text, txt.data(), txt.size()); }
        else {   // a picture, archive or document: a copy Windows opens next to it
            if (exists(w.path + ".txt")) _wremove(widen(w.path + ".txt").c_str());   // read as text by an earlier run
            for (size_t k = 0; k < txtIn.size(); k++) if (txtIn[k] < 0) w.unread.push_back((long)k);
            openableCopy(w.path, true, &w.unread);
        }
        // how much is there, and does it check
        w.readPct = bin.empty() ? 0 : 100.0 * (double)((long)bin.size() - std::min<long>(bad, (long)bin.size())) / bin.size();
        if (bin.size() >= 4 && bin[0] == 0x60 && bin[1] == 0xEA) {
            ArjCheck a = arjCheck(bin);
            w.check = a.summary();
            for (auto& e : a.files) if (!e.ok) w.check += "; " + e.name + ": " + e.why;
            w.checkOk = a.archive && a.headerOk && a.complete && !a.files.empty() && a.good() == (int)a.files.size();
        } else if (isPcx(bin)) {
            std::vector<long> un;
            for (size_t k = 0; k < txtIn.size(); k++) if (txtIn[k] < 0) un.push_back((long)k);
            Image im;
            PcxRepair info;
            if (pcxDecode(bin, un, im, false, &info)) {
                w.check = info.note() + (info.lines ? fmt(" (%.0f%%)", 100.0 * info.linesGood / info.lines) : std::string());
                w.checkOk = info.lines > 0 && info.linesGood == info.lines && !info.repaired && un.empty();
            } else w.check = "PCX: does not decode";
        } else if (isText) {
            w.check = bad ? fmt("text: %ld bytes did not read", bad) : "text: every byte read";
            w.checkOk = bad == 0;
        } else {
            w.check = bad ? "no check for this kind of file; bytes missing" : "no check for this kind of file; every byte read";
            w.checkOk = false;
        }
        // old contents of the sender's disk: its directories and texts in leftovers/<name>.txt
        if (w.content.rfind("disk leftovers", 0) == 0) {
            std::string lo = leftoversReport(w.name, txtIn);
            if (!lo.empty()) {
                make_dirs(path_join(dir, "leftovers"));
                write_file(path_join(path_join(dir, "leftovers"), safe + ".txt"), lo.data(), lo.size());
            }
        }
        rep += fmt("%-14s %-32s %s  blocks %d of %d, rows %d of %d", w.name.c_str(), w.content.c_str(), w.decrypted ? "decrypted" : "as sent  ", w.blocksHave,
                   std::max(nb, 0), w.rowsHave, w.rows) + (w.rowsBad ? fmt(" (%d spoiled rows left out)", w.rowsBad) : std::string()) +
               (bad ? fmt(", %ld bytes missing or not readable", bad) : std::string()) +
               (w.sumRight + w.sumMended + w.sumWrong ? fmt(", block sums %d right, %d mended, %d wrong", w.sumRight, w.sumMended, w.sumWrong) : std::string()) + (size > 0 ? fmt(", %ld bytes by the catalogue", size) : std::string()) +
               fmt("\n               %.1f%% read; check: %s%s", w.readPct, w.check.c_str(), w.checkOk ? "  [OK]" : "");
        for (size_t k = 0; k < dropped.size(); k++)
            rep += (k ? fmt(", %d", dropped[k]) : fmt(", block %d", dropped[k])) + (k + 1 == dropped.size() ? " left out (it carried another block's data)" : "");
        rep += "\n";
        out.push_back(w);
    }
    rep = "\xD0\x91\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x81-\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82: " + note + "\n" +
          "rows not received are zero bytes in the file and [...] in the .txt, single bytes that did not read \xC2\xB7 (the text without the binary records between dictionary articles)\n"
          "\"disk leftovers\": the file was made at its size and holds what was on the sender's disk before (dictionaries, BBS, Windows);\n"
          "\"compressed data of the service\": its own data, packed by its own program (the format is not known yet)\n\n" + rep;
    // the files of an earlier run under names that are joined now (PACE357.PCX -> PAGE350.PCX): its list, its files
    // and the copies made to open them
    try {
        if (exists(path_join(dir, "files.txt"))) {
            std::string old = read_text(path_join(dir, "files.txt"));
            size_t a = old.find("\n\n");   // the files' lines follow the blank line
            while (a != std::string::npos && a < old.size()) {
                size_t b = old.find('\n', a + 1);
                std::string line = old.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
                a = b;
                std::string nm = line.substr(0, line.find(' '));
                if (nm.empty() || F.count(nm)) continue;
                for (auto& c : nm) if (strchr("\\/:*?\"<>|", c)) c = '_';
                for (const char* ext : {"", ".txt", ".png", ".arj", ".zip", ".rar", ".lzh", ".arc", ".zoo", ".gz", ".gif", ".jpg", ".bmp",
                                        ".tif", ".doc", ".xls", ".rtf", ".pdf", ".ps"})
                    if (exists(path_join(dir, nm + ext))) _wremove(widen(path_join(dir, nm + ext)).c_str());
            }
        }
    } catch (...) {}
    write_file(path_join(dir, "files.txt"), rep.data(), rep.size());
    std::string ks;
    if (v96 && !k96.from.empty())   // one line a column: lane of bits 0-3 codewords 0-9, then lane 4-7
        for (int i = 0; i < 800; i++) ks += fmt("%02X%s", k96.k[i], i % 20 == 19 ? "\n" : " ");
    else if (P)
        for (size_t i = 0; i < K.size(); i++) ks += fmt("%02X%s", K[i], i % 40 == 39 ? "\n" : " ");
    if (!ks.empty()) write_file(path_join(dir, "key.txt"), ks.data(), ks.size());
    return out;
}
