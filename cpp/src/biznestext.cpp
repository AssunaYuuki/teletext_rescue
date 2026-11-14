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
#include <filesystem>

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
    if (frame > lastFrame_) lastFrame_ = frame;
    Cur& cur = cur_[mag];
    if (row == 0) {
        int pu = H.d[q[0]], pt = H.d[q[1]];
        looseRows(mag, frame);
        cur = Cur();
        cur.on = true;
        char pg[8];
        if (pu >= 0 && pt >= 0) snprintf(pg, sizeof pg, "%d%X%X", mag, pt, pu); else snprintf(pg, sizeof pg, "%d??", mag);
        cur.page = pg;
        return;
    }
    if (row < 1 || row > 23) return;
    // rows come in order and the catalogue rows 21-23 close a transmission: a lower row (or a data row after
    // them) is the next transmission whose header was lost - its rows are not this one's. They are kept apart
    // (headless): a block that lacks exactly them may take them, by its sum (looseRows)
    if (row <= 20) {
        if (!cur.on || row <= cur.last || cur.last >= 21) {
            looseRows(mag, frame);
            cur = Cur();
            cur.on = cur.headless = true;
        }
        cur.data[row] = q;
        cur.last = row;
        return;
    }
    if (!cur.on) return;
    if (cur.headless) { looseRows(mag, frame); cur = Cur(); return; }   // catalogue rows with no header: not taken
    cur.last = row;
    cur.rows[row] = q;
    if (cur.rows.size() < 3) return;
    auto r21 = cur.rows[21];
    auto r22 = cur.rows[22];
    auto r23 = cur.rows[23];
    std::string page = cur.page;
    cur.on = false;
    cur.closed = true;
    // 9.12.93 (the earliest, «Российские университеты» channel): the same fields, but every byte is sent as
    // its 7-bit character with odd parity + a check byte (also odd parity), not as two Hamming 8/4 nibbles;
    // row 21 begins "****" where 1994 has AA AA AA AA. A pair whose bytes fail their parity reads -1 (the
    // copies are voted as for the Hamming pairs)
    auto oddp = [](uint8_t x) { x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return (x & 1) != 0; };
    int stars = 0;
    for (int k = 0; k < 4; k++) stars += (r21[2 * k] & 0x7F) == '*' && oddp(r21[2 * k]) && oddp(r21[2 * k + 1]);
    const bool y93 = stars >= 3;
    auto pairAt = [&](const std::vector<uint8_t>& q, int i) {
        if (!y93) return ::pairAt(q, i);
        return oddp(q[2 * i]) && oddp(q[2 * i + 1]) ? (int)(q[2 * i] & 0x7F) : -1;
    };
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
    {
        // closed: AA AA AA AA in Hamming pairs; open (BTEST on 25.05.96): plain spaces there, no Hamming pairs
        int aa = 0;
        for (int k = 0; k < 4; k++) aa += pairAt(r21, k) == 0xAA;
        if (y93) aa = stars;
        if (aa >= 3) e.closed++;
        else e.open++;
    }
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
        if (blk > 0 && (count <= 0 || blk <= count) && weight_ > 0 && !done_.count(name)) {   // weight 0: the catalogue only
            Recent rc;
            rc.name = name; rc.blk = blk; rc.frame = frame;
            for (auto& rq : cur.data) {
                if (rq.second.size() != 40) continue;
                added = true;
                Row40 r;
                std::copy(rq.second.begin(), rq.second.end(), r.begin());
                addRow(name, blk, rq.first, r);
                rc.rows[rq.first] = r;
            }
            // 1994 (row 23 = 00 a b c): a block that lacks rows waits for them (they may come in a transmission of
            // their own, before or after it)
            int c0 = pairAt(r23, 0), c1 = pairAt(r23, 1), c2 = pairAt(r23, 2), c3 = pairAt(r23, 3);
            if (c0 == 0 && c1 >= 0 && c2 >= 0 && c2 < 100 && c3 >= 0 && c3 < 100) rc.sum = c1 * 10000L + c2 * 100 + c3;
            if (rc.sum >= 0 && !rc.rows.empty() && rc.rows.size() < 20) {
                bool whole = false;
                auto& L = loose_[mag];
                std::set<int> shared = sharedRows(mag, nullptr, frame);
                for (int r = 1; r <= 20; r++)
                    if (!rc.rows.count(r))
                        for (auto& o : recent_[mag]) if (frame - o.frame <= 50 && !o.rows.count(r)) shared.insert(r);
                // rows that came before the block (left over from an earlier split): only when the sum says so
                for (size_t i = 0; i < L.size() && !whole; i++)
                    if (frame - L[i].frame <= 50) whole = fill(rc, L[i], shared, true);
                if (!whole && rc.rows.size() < 20) {
                    auto& R = recent_[mag];
                    R.push_back(rc);
                    if (R.size() > 4) R.erase(R.begin());
                }
            }
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

void BtCatalog::addRow(const std::string& name, int blk, int row, const Row40& r)
{
    // a row already held in kEnoughCopies copies is not taken again: the file goes round and round, the
    // capture keeps only what is new (and writes the files again only then)
    auto& to = data_[name][blk][row];
    // copies that still differ (a row received badly) keep coming up to 3x as many
    bool agree = true;
    for (auto& c : to) agree &= c.b == to[0].b;
    if ((int)to.size() < (agree ? kEnoughCopies : 3 * kEnoughCopies)) { to.push_back({r, weight_}); return; }
    // a copy trusted more (a live 8-bit packet) takes the place of one trusted less
    auto low = std::min_element(to.begin(), to.end(), [](const Copy& a, const Copy& b) { return a.w < b.w; });
    if (low->w < weight_) *low = {r, weight_};
}

std::set<int> BtCatalog::sharedRows(int mag, const Recent* not1, int64_t frame) const
{
    std::map<int, int> n;
    for (auto& o : recent_[mag]) {
        if (&o == not1 || frame - o.frame > 50) continue;
        for (int r = 1; r <= 20; r++) if (!o.rows.count(r)) n[r]++;
    }
    std::set<int> s;
    for (auto& kv : n) if (kv.second > 1) s.insert(kv.first);
    return s;
}

bool BtCatalog::fill(Recent& b, Loose& l, const std::set<int>& shared, bool checkedOnly)
{
    if (b.sum < 0 || done_.count(b.name)) return false;
    long have = 0;
    std::vector<int> take;
    bool whole = true;
    for (int r = 1; r <= 20; r++) {
        auto in = b.rows.find(r);
        const Row40* x = nullptr;
        if (in != b.rows.end()) x = &in->second;
        else {
            auto lr = l.rows.find(r);
            if (lr == l.rows.end()) { whole = false; continue; }
            x = &lr->second;
            take.push_back(r);
        }
        for (uint8_t v : *x) have += v;
    }
    if (take.empty()) return false;
    const bool right = whole && have == b.sum;
    if (right) sure_++;
    else if (whole && !checkedOnly) unsure_++;
    if (!right) {
        // not checked: only once the split has proved itself, and only rows no other waiting block lacks
        if (checkedOnly || sure_ < 3) return false;
        std::vector<int> t;
        for (int r : take) if (!shared.count(r)) t.push_back(r);
        if (t.empty()) return false;
        take = t;
    }
    for (int r : take) {
        addRow(b.name, b.blk, r, l.rows[r]);
        b.rows[r] = l.rows[r];
        l.rows.erase(r);
    }
    mended_[b.name] += (int)take.size();
    return b.rows.size() >= 20;
}

void BtCatalog::looseRows(int mag, int64_t frame)
{
    Cur& cur = cur_[mag];
    if (weight_ <= 0 || cur.closed || cur.data.empty()) return;
    Loose l;
    l.frame = frame;
    for (auto& rq : cur.data) {
        if (rq.second.size() != 40) continue;
        Row40 r;
        std::copy(rq.second.begin(), rq.second.end(), r.begin());
        l.rows[rq.first] = r;
    }
    cur.data.clear();
    // the blocks of this magazine heard just before that lack these rows
    auto& R = recent_[mag];
    for (size_t i = 0; i < R.size();)
        if (frame - R[i].frame > 50) R.erase(R.begin() + (long)i);
        else i++;
    // first the blocks that come out right by their sum, then (once the split proved itself) the rest
    for (int pass = 0; pass < 2 && !l.rows.empty(); pass++)
        for (size_t i = 0; i < R.size() && !l.rows.empty();) {
            std::set<int> shared = sharedRows(mag, nullptr, frame);
            bool whole = fill(R[i], l, shared, pass == 0);
            if (whole || R[i].rows.size() >= 20) { R.erase(R.begin() + (long)i); continue; }
            i++;
        }
    if (l.rows.empty()) return;
    auto& L = loose_[mag];
    L.push_back(std::move(l));
    if (L.size() > 4) L.erase(L.begin());
}

long BtCatalog::mendedRows() const
{
    long n = 0;
    for (auto& kv : mended_) n += kv.second;
    return n;
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

// FAT directory entries in what the sender's disk left in a file (-1 = byte not received): names, sizes and dates of
// files that were on the disk
struct FatEntry {
    long at = 0;               // offset in the file
    std::string name, ext;     // UTF-8, trimmed; a deleted entry's first character is E5 (shown as "х")
    long size = 0;
    int attr = 0, cluster = 0, y = 0, m = 0, d = 0, hh = 0, mm = 0;
    bool deleted = false;
    bool isDir() const { return attr & 0x10; }
    std::string dosName() const   // for a file on Windows: a deleted entry's lost first letter as "_"
    {
        std::string n = deleted ? "_" + name.substr(name.size() >= 2 ? 2 : name.size()) : name;
        return ext.empty() ? n : n + "." + ext;
    }
};
std::vector<FatEntry> fatEntries(const std::vector<int>& b)
{
    std::vector<FatEntry> out;
    // (c != 0: strchr finds the terminating zero, and names of zeros made false entries in REB, SELX, STAN, TOVAR)
    auto nameChar = [](int c, bool first) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("_!$#&@-~", c)) || (first ? c == 0xE5 || c == '.' : c == ' ' || c == '.');
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
        FatEntry e;
        e.at = (long)i;
        for (int k = 0; k < 8; k++) e.name += utf8Of866((uint8_t)b[i + k]);
        for (int k = 8; k < 11; k++) e.ext += (char)b[i + k];
        while (!e.name.empty() && e.name.back() == ' ') e.name.pop_back();
        while (!e.ext.empty() && e.ext.back() == ' ') e.ext.pop_back();
        e.size = sz; e.attr = attr; e.cluster = cl; e.y = y; e.m = m; e.d = d; e.hh = t >> 11; e.mm = (t >> 5) & 63;
        e.deleted = b[i] == 0xE5;
        out.push_back(e);
    }
    return out;
}
std::string fatLine(const FatEntry& e)
{
    return fmt("%-8s %-3s %s  %02d.%02d.%d %02d:%02d  cluster %d%s", e.name.c_str(), e.ext.c_str(),
               e.isDir() ? "    <DIR>" : fmt("%9ld", e.size).c_str(), e.d, e.m, e.y, e.hh, e.mm, e.cluster, e.deleted ? "  (deleted)" : "");
}

// what the sender's disk left in a file (-1 = byte not received): FAT directory entries (names, sizes, dates of
// files that were on the disk) and every run of DOS text of 40+ characters, with offsets, as UTF-8
std::string leftoversReport(const std::string& name, const std::vector<int>& b)
{
    std::string dirs, texts;
    for (auto& e : fatEntries(b)) dirs += fmt("@%-7lu ", (unsigned long)e.at) + fatLine(e) + "\n";
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

// whole files of the sender's disk inside the leftovers (9.07.94: JINN.EXE in TOVAR.C01 @49152, its READ.ME @79872): a
// DOS program (MZ header, its length from the header) or a text ending in the DOS end of file 1A, each from a sector
// boundary; named by a FAT entry of that size found in any of the leftovers ("_" for a deleted entry's lost letter)
struct Carved {
    std::string name, from, what, entry;   // entry: the FAT entry that named it ("" = none)
    long at = 0, size = 0, missing = 0;
    std::vector<uint8_t> bytes;
    std::vector<long> unread;              // its bytes not received (0 in bytes)
};
std::vector<Carved> carveLeftovers(const std::vector<std::pair<std::string, std::vector<int>>>& L, const std::vector<FatEntry>& E)
{
    std::vector<Carved> out;
    auto named = [&](Carved& c, bool exe) {
        // a program's header counts its image only (an overlay or padding may follow): the nearest entry not below it
        const FatEntry* best = nullptr;
        for (auto& e : E) {
            if (e.isDir()) continue;
            bool fit = exe ? e.size >= c.size && e.size < c.size + 512 : e.size == c.size || e.size == c.size - 1;
            if (fit && (!best || std::labs(e.size - c.size) < std::labs(best->size - c.size))) best = &e;
        }
        if (best) { c.name = best->dosName(); c.entry = fatLine(*best); if (exe) c.size = best->size; }
    };
    for (auto& f : L) {
        const std::vector<int>& b = f.second;
        long lastEnd = -1;
        for (size_t at = 0; at + 64 <= b.size(); at += 512) {
            Carved c;
            c.from = f.first; c.at = (long)at;
            if (b[at] == 'M' && b[at + 1] == 'Z' && b[at + 2] >= 0 && b[at + 3] >= 0 && b[at + 4] >= 0 && b[at + 5] >= 0 &&
                b[at + 8] >= 0 && b[at + 9] >= 0 && b[at + 24] >= 0 && b[at + 25] >= 0) {
                int cblp = b[at + 2] | b[at + 3] << 8, cp = b[at + 4] | b[at + 5] << 8, hdr = b[at + 8] | b[at + 9] << 8,
                    rel = b[at + 24] | b[at + 25] << 8;
                long img = cp > 0 ? (long)(cp - 1) * 512 + (cblp ? cblp : 512) : 0;
                if (cblp >= 512 || cp < 2 || cp > 2048 || hdr < 2 || hdr * 16L >= img || rel < 0x1C || rel > hdr * 16) continue;
                c.size = img;
                c.what = "DOS program";
                named(c, true);
            } else if (b[at] >= 0 && textByte(b[at]) && (long)at > lastEnd) {
                size_t j = at;
                while (j < b.size() && j - at < 65536 && b[j] >= 0 && textByte(b[j])) j++;
                if (j >= b.size() || b[j] != 0x1A || j - at < 128) continue;
                lastEnd = (long)j;   // the later sectors of the same text are not files of their own
                c.size = (long)(j - at) + 1;
                c.what = "text";
                named(c, false);
            } else continue;
            if ((long)at + c.size > (long)b.size()) c.size = (long)b.size() - (long)at;
            for (long k = 0; k < c.size; k++) {
                int v = b[at + k];
                if (v < 0) { c.missing++; c.unread.push_back(k); }
                c.bytes.push_back(v >= 0 ? (uint8_t)v : 0);
            }
            if (c.missing * 10 > c.size) continue;   // too little of it came
            if (c.name.empty()) c.name = fmt("%s@%ld.%s", f.first.c_str(), c.at, c.what == "text" ? "TXT" : "EXE");
            out.push_back(std::move(c));
        }
    }
    // a name found twice (two copies on the disk): the one with fewer bytes missing
    std::sort(out.begin(), out.end(), [](const Carved& a, const Carved& b) { return a.name != b.name ? a.name < b.name : a.missing < b.missing; });
    out.erase(std::unique(out.begin(), out.end(), [](const Carved& a, const Carved& b) { return a.name == b.name; }), out.end());
    return out;
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
    // until the reading settles (two rounds were not enough where a letter is about as common as the space: the
    // start "commonest = space" is then wrong at half the places, every one by the same XOR, and the bigram rounds below
    // kept it)
    for (int it = 0; it < 8; it++) {
        unigram();
        std::vector<int> nk(len);
        for (int k = 0; k < len; k++) nk[k] = pick(k, [&](int, int, int p) { return lu[p]; }, all);
        bool same = nk == K;
        K = nk;
        if (same && it >= 1) break;
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
    // zeros are common in files from a DOS disk (binary records of dictionaries, code, padding). Taken as rare, a key
    // moved by 45 won (9.07.94): 45 keeps the lower-case Cyrillic of CP866 Cyrillic (A0-AF <-> E0-EF) and makes every
    // 00 an "E" - the dictionaries then read as other Russian letters, the English as punctuation
    pr[0] = std::log(4.0);
    // the whole key moved by one XOR can read as consistently as the true one (a Russian text: 88 swaps the space
    // with "и" and "а о е" with "( & -"); the prior of a DOS text decides between such readings
    {
        int bestD = 0;
        double bestS = -1e300;
        for (int d = 0; d < 256; d++) {
            double s = 0;
            for (auto& b : B) for (int o = 0; o < 800; o++) if (b.c[o] >= 0) s += pr[b.c[o] ^ K[b.base + o] ^ d];
            if (s > bestS) { bestS = s; bestD = d; }
        }
        if (bestD) for (auto& k : K) k ^= bestD;
    }
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

// ---- the 1995-96 form (29.12.95 MOSCOW / PCXPIC / .COD, 25.05.96 EXAMPCH1, FIRM_OUT, HOTEL ...): 2 bytes sent a byte
// of the file, blocks in groups of 8 = 3200 bytes of the file. Logical row q (0..159) of group g is block g*8 + q%8 + 1,
// row (q/8 + 2*(q%8)) % 20 as sent. A codeword is 8 logical rows in a row (8k..8k+7) of one column c and one bit b
// (the first row the highest bit), 20 a (c, b); XOR the key of its place (c*8 + b)*20 + k it is a teletext Hamming
// 8/4 codeword; its value through kTi95 is a nibble of the file byte g*3200 + k*160 + c*4 + b/2 (b even: the low one).
// One key for every group, file and tape (found from EXAMPCH1.ARJ = COMI0304.ARJ against its 25.05.96 broadcast).
#include "biznestext_key95.inc"
const int kTi95[16] = {0, 14, 7, 9, 11, 5, 12, 2, 13, 3, 10, 4, 6, 8, 1, 15};   // the table is its own inverse
// the Hamming value of a codeword with erasures, as ham96 but without the 1996 table; -1 = unreadable
int hamv95(int x, int mask)
{
    int bd = 9, bv = -1, amb = 0, unk = 8 - __builtin_popcount(mask);
    for (int v = 0; v < 16; v++) {
        int d = __builtin_popcount((x ^ kHam[v]) & mask);
        if (d < bd) { bd = d; bv = v; amb = 1; } else if (d == bd) amb++;
    }
    if (amb > 1 || unk >= 4 || (bd > 0 && bd + unk >= 4)) return -1;
    return bv;
}
// the codeword (c, b, k) of group g: bits received in mask
int cw95(const std::map<int, Blk>& B, int g, int c, int b, int k, int& mask)
{
    int x = 0;
    mask = 0;
    for (int t = 0; t < 8; t++) {
        int q = 8 * k + t, blk = g * 8 + q % 8 + 1, row = (q / 8 + 2 * (q % 8)) % 20;
        auto it = B.find(blk);
        if (it == B.end()) continue;
        int v = it->second[row * 40 + c];
        if (v < 0 || (v >> 8 >> b & 1)) continue;   // not received, or the copies split on this bit
        x |= ((v >> b) & 1) << (7 - t);
        mask |= 1 << (7 - t);
    }
    return x;
}
// the share of whole codewords that are valid under the key (random data ~6%, this form ~95%)
double share95(const std::map<int, Blk>& B)
{
    if (B.empty()) return 0;
    int groups = (B.rbegin()->first + 7) / 8;
    long ok = 0, n = 0;
    for (int g = 0; g < groups; g++)
        for (int c = 0; c < 40; c += 3)
            for (int b = 0; b < 8; b++)
                for (int k = 0; k < 20; k++) {
                    // whole, or one bit missing (a block of the group not received: every codeword of the group
                    // lacks its row; random bytes then fit 1 in 8)
                    int m, x = cw95(B, g, c, b, k, m);
                    if (__builtin_popcount(m) < 7) continue;
                    n++;
                    int y = x ^ kKey95[(c * 8 + b) * 20 + k];
                    for (int v = 0; v < 16; v++) if (((kHam[v] ^ y) & m) == 0) { ok++; break; }
                }
    return n >= 200 ? (double)ok / n : 0;
}
// the file's bytes as 400-byte pieces (piece i = bytes (i-1)*400 ..), -1 = not received / not readable
std::map<int, std::vector<int16_t>> bytes95(const std::map<int, Blk>& B, int groups)
{
    std::map<int, std::vector<int16_t>> out;
    for (int g = 0; g < groups; g++) {
        std::vector<int16_t> d(3200, -1);
        std::vector<int> lo(3200, -1), hi(3200, -1);
        bool any = false;
        for (int c = 0; c < 40; c++)
            for (int b = 0; b < 8; b++)
                for (int k = 0; k < 20; k++) {
                    int m, x = cw95(B, g, c, b, k, m);
                    if (!m) continue;
                    int v = hamv95(x ^ (kKey95[(c * 8 + b) * 20 + k] & m), m);
                    if (v < 0) continue;
                    int i = k * 160 + c * 4 + b / 2;
                    (b % 2 ? hi : lo)[(size_t)i] = kTi95[v];
                    any = true;
                }
        if (!any) continue;
        for (int i = 0; i < 3200; i++) if (lo[(size_t)i] >= 0 && hi[(size_t)i] >= 0) d[(size_t)i] = (int16_t)(lo[(size_t)i] | hi[(size_t)i] << 4);
        for (int m = 0; m < 8; m++) out[g * 8 + m + 1].assign(d.begin() + m * 400, d.begin() + (m + 1) * 400);
    }
    return out;
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
        std::vector<long> hist(256, 0);
        for (auto* b : B) {
            auto d = bytes96(*b, key);
            for (int i = 0; i < 400; i++) {
                if (d[i] < 0) continue;
                n++; tx += textByte(d[i]);
                hist[(size_t)(d[i] & 0xFF)]++;
                crlf += d[i] == 13 && i + 1 < 400 && d[i + 1] == 10;
            }
        }
        share = n ? (double)tx / n : 0;
        double lines = n ? (double)crlf / n : 0, score = share + (lines > 0.004 ? 1 : 0);
        // a file of filler (29.12.95 FULL.COD: zeros and a running bit) bends into "spaces with line ends" under the
        // free choices; a text has no byte in more than half of its places
        if (n && *std::max_element(hist.begin(), hist.end()) * 2 > n) score = 0;
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
                int diff = 0;
                if (on.size() == me.size()) for (size_t i = 0; i < on.size(); i++) diff += on[i] != me[i];
                else diff = 99;
                // the block count of a catalogue row read with its name damaged may be damaged too (live 25.05.96:
                // "EX@MQCH1.ARJ 38 blocks" = EXAMPCH1.ARJ, 40): the same size and a name a character or two off decide
                if (mb > 0 && ob > 0 && mb != ob && !(diff <= 2 && ms > 0 && ms == os)) continue;
                if (ms > 0 && os > 0 && ms != os) continue;
                if (ob <= 0 && os <= 0) continue;   // nothing in the catalogue to tell
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
    // one file sent under several names (29.12.95: MOSCOW1-4.ARJ, PCXPIC1-4.ARJ - the same size and date, and as sent
    // 66-80% the same bytes, the rest tape errors; two different files share 0.4%): all their copies voted together into
    // <name>_combined, next to the files themselves
    std::map<std::string, std::string> catOfName;   // combined name -> the member whose catalogue entry it takes
    std::map<std::string, std::vector<std::string>> groups;
    {
        auto sizeOf = [&](const std::string& n) { auto e = entries_.find(n); return e != entries_.end() ? best<long>(e->second.size, -1L) : -1L; };
        auto dateOf = [&](const std::string& n) { auto e = entries_.find(n); return e != entries_.end() ? best<std::string>(e->second.date, std::string()) : std::string(); };
        std::vector<std::string> names;
        for (auto& kv : data_) if (to[kv.first] == kv.first && !kv.first.empty()) names.push_back(kv.first);
        std::map<std::string, std::string> root;
        for (auto& nm : names) root[nm] = nm;
        std::function<std::string(const std::string&)> find = [&](const std::string& x) { return root[x] == x ? x : root[x] = find(root[x]); };
        for (size_t i = 0; i < names.size(); i++)
            for (size_t j = i + 1; j < names.size(); j++) {
                const std::string &a = names[i], &b = names[j];
                long sa = sizeOf(a);
                if (sa <= 0 || sa != sizeOf(b) || dateOf(a) != dateOf(b)) continue;
                long n = 0;
                double ag = 0;
                {
                    long eq = 0;
                    for (auto& blk : data_.at(a)) {
                        auto ib = data_.at(b).find(blk.first);
                        if (ib == data_.at(b).end()) continue;
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
                    ag = n ? (double)eq / n : 0;
                }
                if (n >= 400 && ag >= 0.3) root[find(a)] = find(b);
            }
        std::map<std::string, std::vector<std::string>> by;
        for (auto& nm : names) by[find(nm)].push_back(nm);
        for (auto& g : by) {
            if (g.second.size() < 2) continue;
            std::sort(g.second.begin(), g.second.end());
            // MOSCOW1.ARJ + MOSCOW2.ARJ -> MOSCOW_combined.ARJ
            const std::string& f0 = g.second.front();
            size_t dot = f0.rfind('.');
            std::string stem = dot == std::string::npos ? f0 : f0.substr(0, dot), ext = dot == std::string::npos ? "" : f0.substr(dot);
            size_t common = stem.size();
            for (auto& m : g.second) {
                size_t k = 0;
                while (k < common && k < m.size() && m[k] == stem[k]) k++;
                common = k;
            }
            std::string base = stem.substr(0, common);
            while (!base.empty() && (base.back() == '_' || base.back() == '-')) base.pop_back();
            if (base.empty()) base = stem;
            std::string cn = base + "_combined" + ext;
            catOfName[cn] = f0;
            groups[cn] = g.second;
        }
    }
    auto entryOf = [&](const std::string& n) {
        auto c = catOfName.find(n);
        return entries_.find(c != catOfName.end() ? c->second : n);
    };
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
                    for (auto& g : groups)
                        if (std::find(g.second.begin(), g.second.end(), nm) != g.second.end()) cp[g.first][b.first][r.first].push_back(&x);
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
    // a file sent open (row 21 FF FF FF FF: BTEST1-4.TXT on 25.05.96, "Блок 0005,Строка  2 ABC..") is plain text:
    // it is written as it came and kept out of the key search (its text made a false key fit, which then spoiled
    // it and every closed file)
    auto isOpen = [&](const std::string& n) {
        auto c = catOfName.find(n);
        auto e = entries_.find(c != catOfName.end() ? c->second : n);
        return e != entries_.end() && e->second.open > e->second.closed;
    };
    for (auto& f : F)
        if (!isOpen(f.first)) for (auto& b : f.second) all.push_back(&b.second);
    std::array<uint8_t, 800> base{};
    say(fmt("%zu blocks of %zu files: which form of the service (1994 / 1995 / 1996)", all.size(), F.size()));
    // 1995-96: the built-in key makes the codewords of the closed files valid (most of them; a live copy fewer)
    bool v95 = false;
    {
        long n = 0;
        double sum = 0;
        for (auto& f : F) {
            if (isOpen(f.first) || f.second.size() < 8) continue;
            double sh = share95(f.second);
            if (sh > 0) { sum += sh * f.second.size(); n += (long)f.second.size(); }
        }
        v95 = n && sum / n > 0.35;
        if (getenv("TR_BT96DEBUG")) fprintf(stderr, "1995 form: %.0f%% valid codewords under the built-in key\n", n ? 100 * sum / n : 0.0);
    }
    // a clean recording gives > 80% valid codewords, a tape received live much less (bit errors, split copies);
    // from 30% on the 1996 key is looked for, and a key under which a file reads as DOS text with lines decides
    double s96 = all.size() >= 8 && !v95 ? base96(all, base) : 0;
    if (getenv("TR_BT96DEBUG"))
        for (auto& f : F) {
            std::vector<const Blk*> one;
            for (auto& b : f.second) one.push_back(&b.second);
            std::array<uint8_t, 800> tmp{};
            say(fmt("  %s: %zu blocks, %.0f%% codewords valid", f.first.c_str(), one.size(), 100 * base96(one, tmp)));
        }
    bool v96 = s96 > 0.8 || v95;   // 1995 goes the 1996 way from here on (400 bytes a piece), its bytes from bytes95
    Key96 k96;
    if (s96 > 0.3) {
        say(fmt("%.0f%% of the codewords can be valid: trying the 1996 form (two Hamming 8/4 codewords a byte)", 100 * s96));
        std::map<std::string, std::map<int, Blk>> closedF;
        for (auto& f : F) if (!isOpen(f.first)) closedF[f.first] = f.second;
        k96 = findKey96(closedF, base);
        if (!k96.from.empty()) v96 = true;
    }
    say(v95 ? "1995-96 form (two bytes sent a byte, groups of 8 blocks): the built-in key"
            : v96 ? "1996 form (two Hamming 8/4 codewords a byte)" : "1994 form (800-byte blocks): finding the key from the text of the files");
    // the key: the same for every block, or one per block number mod 8 - the one that reads as text
    std::vector<int> K;
    int P = 0;
    double bestText = 0;
    for (int per : {1, 8}) {
        if (v96) break;
        std::vector<KBlock> B;
        // each file once: copies of one file under several names give the same bytes at the same places, and that
        // false "most common value" would fit the key to them (the combined file stands for its copies)
        std::set<std::string> member;
        for (auto& g : groups) for (auto& m : g.second) member.insert(m);
        for (auto& f : F)
            if (!member.count(f.first) && !isOpen(f.first))
            for (auto& b : f.second) {
                KBlock k{((b.first - 1) % per) * 800, {}};
                for (int o = 0; o < 800; o++) k.c[o] = (int16_t)(b.second[o] < 0 ? -1 : b.second[o] & 0xFF);
                // rows of the transmitter's filler (zeros, a running bit 01 02 04 .. 80) are not encrypted: in the key
                // search they make the key "20 20 20 .." (29.12.95: a batch of packed files and empty .COD records)
                for (int r = 0; r < 20; r++) {
                    int fill = 0, got = 0;
                    for (int c = 0; c < 40; c++) {
                        int v = k.c[r * 40 + c];
                        if (v < 0) continue;
                        got++;
                        fill += v == 0 || (v & (v - 1)) == 0;
                    }
                    if (got && fill >= got * 9 / 10) for (int c = 0; c < 40; c++) k.c[r * 40 + c] = -1;
                }
                B.push_back(k);
            }
        long n = 0;
        for (auto& b : B) for (auto v : b.c) n += v >= 0;
        if (n < 800L * per * 40) continue;   // too few copies of each key byte to tell
        auto k = findKey(B, 800 * per);
        long t = 0;
        for (auto& b : B) for (int o = 0; o < 800; o++) if (b.c[o] >= 0) t += textByte(b.c[o] ^ k[b.base + o]);
        double share = (double)t / n;
        // random bytes are ~64% "DOS text" codes, so the share over everything passes for packed files too (29.12.95:
        // a batch of archives got a key "20 20 20 .."): the key must make at least one file of 4+ blocks read as text
        double fileBest = 0;
        for (auto& f : F) {
            if (f.second.size() < 4 || isOpen(f.first)) continue;
            long ft = 0, fn = 0;
            for (auto& b : f.second)
                for (int r = 0; r < 20; r++) {
                    int fill = 0, got = 0;
                    for (int c = 0; c < 40; c++) { int v = b.second[r * 40 + c]; if (v < 0) continue; got++; v &= 0xFF; fill += v == 0 || (v & (v - 1)) == 0; }
                    if (!got || fill >= got * 9 / 10) continue;
                    for (int c = 0; c < 40; c++) {
                        int v = b.second[r * 40 + c];
                        if (v < 0) continue;
                        fn++;
                        ft += textByte((v & 0xFF) ^ k[((b.first - 1) % per) * 800 + r * 40 + c]);
                    }
                }
            if (fn >= 800) fileBest = std::max(fileBest, (double)ft / fn);
        }
        if (share > 0.6 && fileBest >= 0.85 && share > bestText + 0.03) { bestText = share; K = k; P = per; }
    }
    note = v96 ? fmt("each byte sent as two Hamming 8/4 codewords spread down the columns (400 bytes a block); key of 800 codewords "
                     "found from the text of %s (%.0f%% of its bytes read as DOS text)", k96.from.c_str(), 100 * k96.text)
         : P ? fmt("key of %d bytes found from the text of the files (%.0f%% of the bytes read as DOS text)", 800 * P, 100 * bestText)
             : std::string("not decrypted: no file of this batch reads as text under any key (the key is found from a text "
                           "file; packed files and empty records cannot give it), or too little received");
    if (v96 && k96.from.empty()) note = "each byte sent as two Hamming 8/4 codewords (1996); no file reads as text to find the key";
    if (v95) note = "1995-96 form: each byte two Hamming 8/4 codewords, rows interleaved over groups of 8 blocks; the built-in key "
                    "(found from EXAMPCH1.ARJ = COMI0304.ARJ, one for every tape of 1995-96)";
    // 1994: a block whose bytes as sent do not add up to the catalogue's sum (row 23) is mended when exactly one byte
    // where the copies differ makes it right with the other copy's value
    std::map<std::string, std::array<int, 3>> sumStat;   // file -> right, mended, wrong
    if (!v96)
        for (auto& f : F) {
            auto e = entryOf(f.first);
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
    const int bbAll = v96 ? 400 : 800;   // file bytes a block
    const int bb = bbAll;
    // the file's bytes of a block, -1 = not received / not readable
    auto plain = [&](int blk, const Blk& raw, bool open) {
        if (open) {   // as it came, 800 bytes a block
            std::vector<int16_t> d(800, -1);
            for (int o = 0; o < 800; o++) if (raw[o] >= 0) d[o] = (int16_t)(raw[o] & 0xFF);
            return d;
        }
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
    std::vector<std::pair<std::string, std::vector<int>>> lefts;   // the files that hold old disk contents, their bytes
    for (auto& f : F) {
        const bool open = isOpen(f.first);
        // 1995-96 batch: a file its key does not fit (29.12.95 .COD: one byte sent a byte, the records in the odd
        // blocks) is written as sent
        const bool f95 = v95 && !open && share95(f.second) > 0.35, asSent = v95 && !open && !f95;
        const int bb = open || asSent ? 800 : bbAll;   // a file sent open: 800 file bytes a block in any form
        say(fmt(open ? "writing %s as sent - it was sent open (%zu of %zu, %zu blocks)" : "decrypting and writing %s (%zu of %zu, %zu blocks)",
                f.first.c_str(), ++fileNo, F.size(), f.second.size()));
        BtWritten w;
        w.name = f.first;
        w.blockBytes = bb;
        if (groups.count(f.first)) w.combinedOf = groups.at(f.first);
        if (sumStat.count(f.first)) { auto& st = sumStat[f.first]; w.sumRight = st[0]; w.sumMended = st[1]; w.sumWrong = st[2]; }
        auto e = entryOf(f.first);
        if (e != entries_.end()) w.blocks = best<int>(e->second.blocks, -1);
        long size = e != entries_.end() ? best<long>(e->second.size, -1L) : -1;
        // the blocks the file really has: its size in blocks (9.07.94: the catalogue's count is 156 more for every
        // file - VINO.C01 101192 bytes = 127 blocks, "283"; blocks 101+ come on pages 7xx)
        if (size > 0) w.blocks = (int)((size + bb - 1) / bb);
        int last = f.second.rbegin()->first;
        int nb = std::max(last, w.blocks);
        // one key for all files: every file is decrypted (PASSWORD.COD then shows its repeating structure);
        // a text of its own (.txt) when the most of it reads as DOS text
        w.decrypted = !open && (v95 ? f95 : v96 ? !k96.from.empty() : P != 0);
        w.open = open;
        std::map<int, std::vector<int16_t>> D;
        if (asSent)
            for (auto& b : f.second) D[b.first] = plain(b.first, b.second, true);
        else if (f95) {   // the group's 8 blocks give its 3200 bytes together: in pieces of 400 as the 1996 form
            int groups = size > 0 ? (int)((2 * size + 6399) / 6400) : (f.second.rbegin()->first + 7) / 8;
            D = bytes95(f.second, groups);
            w.blocks = groups * 8;
            nb = std::max(nb, w.blocks);
        } else
            for (auto& b : f.second) D[b.first] = plain(b.first, b.second, open);
        // a block carrying another block's data (25.05.96: "block 12" of PAGE350.PCX = block 28, "113" of TOWER350.PCX
        // = 129, each sent whole on page 601 - the generator's page changed before its catalogue rows) is not the
        // block it says: always the data of the block 16 further on (the round of 16 pages), so block b is taken as
        // not received when it reads as block b+16 (neighbouring blocks of disk leftovers may well be alike)
        // (not for a file sent open: the test table BTEST*.TXT has blocks that differ only in their number, "Блок
        // 0006" - 59 of its 75 blocks were left out, 21% read of a file received whole)
        std::vector<int> dropped;
        for (auto& x : D) {
            if (open) break;
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
        if (w.decrypted || open) {
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
            if (open)
                w.content = isText ? "sent open (text)" : "sent open";
            else if (packed + other)
                w.content = v96 ? (packed >= 0.9 * (packed + other) ? "compressed (archive / packed)" : isText ? "text" : "data")
                          : packed >= 0.9 * (packed + other) ? "compressed data of the service"
                          : other >= 0.9 * (packed + other) ? (isText ? "disk leftovers (text)" : "disk leftovers")
                          : "disk leftovers with packed parts";
        }
        Bytes bin;
        std::string txt;
        std::vector<int> txtIn;   // the file's bytes, -1 = row not received
        long& bad = w.bytesBad;   // bytes of the blocks received that did not come or did not read
        long lost = 0;            // bytes of the file not there at all: also the blocks that never came
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
                if (v < 0 && (size <= 0 || (long)bin.size() < size)) lost++;
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
        // (of the whole file: EX@MQCH1.ARJ, 1 block of 38 received, said "100.0% read; every byte read")
        w.readPct = bin.empty() ? 0 : 100.0 * (double)((long)bin.size() - std::min<long>(lost, (long)bin.size())) / bin.size();
        // an ARJ archive also when its start did not come (by its name, its files from the first header that checks);
        // the rows not received are told to the unpacking, which goes on past them
        std::vector<char> lostMask(txtIn.size(), 0);
        for (size_t k = 0; k < txtIn.size(); k++) lostMask[k] = txtIn[k] < 0;
        bool arjName = w.name.size() > 4 && (w.name.compare(w.name.size() - 4, 4, ".ARJ") == 0 || w.name.compare(w.name.size() - 4, 4, ".arj") == 0);
        ArjCheck arjTry;
        bool startArj = bin.size() >= 4 && bin[0] == 0x60 && bin[1] == 0xEA;
        if (bin.size() >= 4 && !startArj && arjName && w.decrypted) {
            arjTry = arjCheck(bin, &lostMask);
            // an .ARJ begins with 60 EA: two damaged bytes there must not hide the archive (its header's CRC then
            // tells whether it is one)
            if (!arjTry.archive) {
                std::vector<uint8_t> t = bin;
                t[0] = 0x60; t[1] = 0xEA;
                std::string lg;
                arjRepair(t, &lostMask, nullptr, lg, 5);
                ArjCheck c2 = arjCheck(t, &lostMask);
                if (c2.archive && c2.headerOk) { bin.swap(t); startArj = true; }
            }
        }
        if (startArj || arjTry.archive) {
            // files inside that do not check: bytes the copies read differently, a tape's one-bit errors and one or
            // two bytes not received, chosen by each file's CRC-32
            std::vector<std::vector<uint8_t>> alts;
            if (!v96 && P && alt.count(f.first)) {
                alts.assign(bin.size(), {});
                for (auto& ab : alt.at(f.first))
                    for (int o = 0; o < 800; o++) {
                        int v = ab.second[o];
                        long idx = (long)(ab.first - 1) * 800 + o;
                        if (v >= 0 && idx >= 0 && idx < (long)bin.size()) alts[idx].push_back((uint8_t)(v ^ K[((ab.first - 1) % P) * 800 + o]));
                    }
            }
            std::string mendLog;
            int mended = w.decrypted ? arjRepair(bin, &lostMask, alts.empty() ? nullptr : &alts, mendLog) : 0;
            if (mended) write_file(w.path, bin);
            ArjCheck a = (arjTry.archive && !mended) ? arjTry : arjCheck(bin, &lostMask);
            w.check = a.summary();
            if (mended) w.check += fmt("; %d file(s) inside mended by their CRC", mended);
            for (auto& e : a.files) if (!e.ok) w.check += "; " + e.name + ": " + e.why;
            w.checkOk = a.archive && a.headerOk && a.complete && !a.files.empty() && a.good() == (int)a.files.size();
            // unpacked even when damaged (7-Zip refuses it): whole files, and the rest as far as it unpacks
            std::string xd = path_join(dir, safe + "_files"), list = arjExtract(a, xd);
            if (!list.empty() && exists(xd)) {
                write_file(path_join(xd, "_unpacked.txt"), list.data(), list.size());
                long whole = 0, got = 0;
                for (auto& e : a.files) { whole += e.ok; got += !e.data.empty(); }
                w.check += fmt("; unpacked into %s_files: %ld whole, %ld in part", safe.c_str(), whole, got - whole);
            }
        } else if (isPcx(bin)) {
            std::vector<long> un;
            for (size_t k = 0; k < txtIn.size(); k++) if (txtIn[k] < 0) un.push_back((long)k);
            Image im;
            PcxRepair info;
            // the picture's size is in its 128-byte header: a byte lost there makes the size itself unsure; lines
            // with a byte lost are not whole, whatever the decoding made of them
            bool headBad = !un.empty() && un.front() < 128;
            if (pcxDecode(bin, un, im, false, &info)) {
                w.check = fmt("PCX %dx%d%s", info.width, info.height, headBad ? " (header damaged: the size may be wrong)" : "");
                if (!un.empty()) w.check += fmt("; %zu bytes lost, the picture is not whole", un.size());
                else if (info.linesGood < info.lines) w.check += fmt("; %d of %d lines whole", info.linesGood, info.lines);
                else w.check += "; every line whole";
                w.checkOk = info.lines > 0 && info.linesGood == info.lines && !info.repaired && un.empty();
            } else w.check = headBad ? "PCX: header damaged, does not decode" : "PCX: does not decode";
        } else if (isText) {
            w.check = lost ? fmt("text: %ld bytes did not read", lost) : "text: every byte read";
            w.checkOk = lost == 0;
        } else {
            w.check = lost ? "no check for this kind of file; bytes missing" : "no check for this kind of file; every byte read";
            w.checkOk = false;
        }
        // old contents of the sender's disk: its directories and texts in leftovers/<name>.txt
        if (w.content.rfind("disk leftovers", 0) == 0) {
            lefts.emplace_back(w.name, txtIn);
            std::string lo = leftoversReport(w.name, txtIn);
            if (!lo.empty()) {
                make_dirs(path_join(dir, "leftovers"));
                write_file(path_join(path_join(dir, "leftovers"), safe + ".txt"), lo.data(), lo.size());
            }
        }
        rep += fmt("%-14s %-32s %s  blocks %d of %d, rows %d of %d", w.name.c_str(), w.content.c_str(), w.decrypted ? "decrypted" : w.open ? "open     " : "as sent  ", w.blocksHave,
                   std::max(nb, 0), w.rowsHave, w.rows) + (w.rowsBad ? fmt(" (%d spoiled rows left out)", w.rowsBad) : std::string()) +
               (lost ? fmt(", %ld bytes missing or not readable", lost) : std::string()) +
               (w.sumRight + w.sumMended + w.sumWrong ? fmt(", block sums %d right, %d mended, %d wrong", w.sumRight, w.sumMended, w.sumWrong) : std::string()) + (size > 0 ? fmt(", %ld bytes by the catalogue", size) : std::string()) +
               fmt("\n               %.1f%% read; check: %s%s", w.readPct, w.check.c_str(), w.checkOk ? "  [OK]" : "");
        for (size_t k = 0; k < dropped.size(); k++)
            rep += (k ? fmt(", %d", dropped[k]) : fmt(", block %d", dropped[k])) + (k + 1 == dropped.size() ? " left out (it carried another block's data)" : "");
        rep += "\n";
        out.push_back(w);
    }
    // the sender's disk: every FAT entry of all the leftovers in leftovers/catalog.txt, and the whole files found in them
    // in leftovers/files (shown as files of their own)
    if (!lefts.empty()) {
        std::vector<FatEntry> E;
        std::string cat;
        for (auto& l : lefts) {
            auto es = fatEntries(l.second);
            if (es.empty()) continue;
            cat += "== in " + l.first + "\n";
            for (auto& e : es) cat += fmt("@%-7ld ", e.at) + fatLine(e) + "\n";
            cat += "\n";
            E.insert(E.end(), es.begin(), es.end());
        }
        auto C = carveLeftovers(lefts, E);
        std::string fdir = path_join(path_join(dir, "leftovers"), "files");
        if (!C.empty()) {
            make_dirs(fdir);
            cat += "== whole files found in the leftovers (in files/)\n";
            rep += "\n  files of the sender's disk found whole in the leftovers (leftovers\\files):\n";
        }
        for (auto& c : C) {
            std::string safe = c.name;
            for (auto& ch : safe) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
            BtWritten w;
            w.name = c.name;
            w.path = path_join(fdir, safe);
            write_file(w.path, c.bytes);
            w.carvedFrom = fmt("%s @%ld", c.from.c_str(), c.at);
            w.carvedEntry = c.entry;
            w.content = c.what == "text" ? "text from the sender's disk" : "DOS program from the sender's disk";
            w.rowsHave = 1;
            w.blocksHave = 1;
            w.readPct = c.size ? 100.0 * (c.size - c.missing) / c.size : 0;
            w.unread = c.unread;
            if (c.what == "text") {
                std::string t;
                for (uint8_t ch : c.bytes) if (ch != 13 && ch != 0x1A) t += ch == 10 || ch == 9 ? std::string(1, (char)ch) : utf8Of866(ch);
                w.text = w.path + ".txt";
                write_file(w.text, t.data(), t.size());
            }
            std::string line = fmt("%-12s %7ld bytes  from %s%s%s", c.name.c_str(), c.size, w.carvedFrom.c_str(),
                                   c.missing ? fmt(", %ld bytes not received", c.missing).c_str() : "",
                                   c.entry.empty() ? "; no FAT entry of this size" : ("; FAT entry: " + c.entry).c_str());
            cat += line + "\n";
            rep += "    " + line + "\n";
            out.push_back(w);
        }
        if (!cat.empty()) {
            make_dirs(path_join(dir, "leftovers"));
            write_file(path_join(path_join(dir, "leftovers"), "catalog.txt"), cat.data(), cat.size());
        }
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
    else {   // no key this time: a key.txt of an earlier run would say the files were decrypted with it
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(path_join(dir, "key.txt")), ec);
    }
    return out;
}

std::vector<BtProgress> BtCatalog::progress(int minCopies, int64_t nowFrame) const
{
    std::vector<BtProgress> out;
    // the block on the air now ("AKC.C01 block 57 (page 602)"), heard in the last 10 s
    std::string nowName;
    int nowBlk = -1;
    if (nowFrame >= 0 && nowFrame_ >= 0 && nowFrame - nowFrame_ < 25 * 10) {
        size_t k = nowSent_.find(" block ");
        if (k != std::string::npos) { nowName = nowSent_.substr(0, k); nowBlk = atoi(nowSent_.c_str() + k + 7); }
    }
    for (auto& f : files(minCopies)) {
        BtProgress p;
        p.name = f.name; p.page = f.page; p.date = f.date; p.size = f.size;
        p.announced = f.copies; p.first = f.first; p.last = f.last;
        if (f.name == nowName) p.nowBlock = nowBlk;
        auto e = entries_.find(f.name);
        auto d = data_.find(f.name);
        const int count = f.blocks;
        // 1996: 400 file bytes a block, the catalogue's count = the size in blocks; 9.07.94: 800, the count 156 more
        if (f.size > 0 && count > 0 && count == (int)((f.size + 399) / 400)) p.blockBytes = 400;
        const int bb = p.blockBytes;
        p.blocks = f.size > 0 ? (int)((f.size + bb - 1) / bb) : std::max(count, 0);
        if (f.size <= 0 && d != data_.end() && !d->second.empty()) {
            int top = d->second.rbegin()->first;
            if (count <= 0 || top <= count) p.blocks = std::max(p.blocks, top);
        }
        p.blocks = std::min(p.blocks, 5000);
        p.map.assign((size_t)p.blocks, '.');
        auto mi = mended_.find(f.name);
        if (mi != mended_.end()) p.mended = mi->second;
        std::string byCopies = p.map;   // the same map judged by the copies only (when the sums do not hold)
        int sumRight = 0;
        for (int b = 1; b <= p.blocks; b++) {
            // rows of the block: 20; the last block of a 1994 file ends with the file
            int rowsIn = 20;
            if (bb == 800 && f.size > 0 && b == p.blocks) rowsIn = std::clamp((int)((f.size - (long)(b - 1) * 800 + 39) / 40), 1, 20);
            const std::map<int, std::vector<Copy>>* rows = nullptr;
            if (d != data_.end()) {
                auto bl = d->second.find(b);
                if (bl != d->second.end()) rows = &bl->second;
            }
            if (rows && !rows->empty()) rowsIn = std::max(rowsIn, std::min(20, rows->rbegin()->first));
            p.rows += rowsIn;
            if (!rows) continue;
            int have = 0;
            bool twice = true;
            for (auto& r : *rows) {
                if (r.first > rowsIn || r.second.empty()) continue;
                have++;
                bool two = false;
                for (size_t i = 0; i < r.second.size() && !two; i++)
                    for (size_t j = i + 1; j < r.second.size() && !two; j++) two = r.second[i].b == r.second[j].b;
                twice &= two;
            }
            p.rowsHave += have;
            if (!have) continue;
            p.blocksHave++;
            char& m = p.map[(size_t)b - 1];
            if (have < rowsIn) { m = 'p'; continue; }
            p.blocksWhole++;
            m = twice ? 'c' : 'r';
            byCopies[(size_t)b - 1] = m;
            // 1994: the bytes as sent against the sum of row 23
            if (bb == 800 && have == 20 && e != entries_.end()) {
                auto s = e->second.blockSum.find(b);
                if (s != e->second.blockSum.end()) {
                    long want = best<long>(s->second, -1L), sum = 0;
                    for (auto& r : *rows) {
                        std::vector<const Copy*> cs;
                        for (auto& x : r.second) cs.push_back(&x);
                        auto v = voteRow(cs);
                        for (int k = 0; k < 40; k++) sum += v[k] & 0xFF;
                    }
                    if (want >= 0) {
                        m = sum == want ? 'c' : 'x';
                        if (sum != want) p.sumWrong++;
                        else sumRight++;
                    }
                }
            }
        }
        // row 23 is not the sum of the bytes as sent in every form (29.12.95: it fails on every block with data):
        // then the copies alone say how sure a block is
        if (p.sumWrong > sumRight) {
            for (size_t k = 0; k < p.map.size(); k++) if (p.map[k] == 'c' || p.map[k] == 'x') p.map[k] = byCopies[k];
            p.sumWrong = 0;
        }
        p.blocksSure = (int)std::count(p.map.begin(), p.map.end(), 'c');
        out.push_back(std::move(p));
    }
    return out;
}
