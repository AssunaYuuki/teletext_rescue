#include "telesoft.h"
#include "finds.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <cmath>
#include <map>

namespace {
int ham84(uint8_t b)
{
    static int t[256];
    static bool init = false;
    if (!init) {
        for (int& v : t) v = -1;
        for (int d = 0; d < 16; d++) {
            int d1 = d & 1, d2 = (d >> 1) & 1, d3 = (d >> 2) & 1, d4 = (d >> 3) & 1;
            int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3, p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
            int c = p1 | d1 << 1 | p2 << 2 | d2 << 3 | p3 << 4 | d3 << 5 | p4 << 6 | d4 << 7;
            t[c] = d;
            for (int k = 0; k < 8; k++) if (t[c ^ (1 << k)] < 0) t[c ^ (1 << k)] = d;
        }
        init = true;
    }
    return t[b];
}
bool oddParity(uint8_t b) { return __builtin_popcount(b) & 1; }

struct Copy { std::array<std::array<uint8_t, 40>, 24> rows{}; std::array<bool, 24> have{}; int page = 0; };

// a byte as sent (7 bits + odd parity) nearest to one of the characters: its index, or -1 if more than maxd bits off
int nearest(uint8_t b, const char* chars, int maxd)
{
    int best = -1, bd = 99;
    for (int i = 0; chars[i]; i++) {
        int c = chars[i];
        int code = c | (__builtin_popcount(c) & 1 ? 0 : 0x80);
        int d = __builtin_popcount((b ^ code) & 0xFF);
        if (d < bd) { bd = d; best = i; }
    }
    return bd <= maxd ? best : -1;
}

struct Header { int block = 0, total = 0; std::string name; };
bool parseHeader(const std::array<uint8_t, 40>& r, Header& h)
{
    auto c = [&](int i) { return r[i] & 0x7F; };
    // |B..|a1: one bit wrong in each of these is let through
    int fixedOff = 0;
    for (auto [i, ch] : {std::pair<int, char>{0, '|'}, {1, 'B'}, {9, '|'}, {10, 'a'}, {11, '1'}}) {
        char s[2] = {ch, 0};
        int k = nearest(r[i], s, 1);
        if (k < 0) return false;
        fixedOff += c(i) != ch;
    }
    if (fixedOff > 2) return false;
    static const char* HEX = "0123456789ABCDEF";
    int b1 = nearest(r[4], HEX, 2), b2 = nearest(r[5], HEX, 2), t1 = nearest(r[7], HEX, 2), t2 = nearest(r[8], HEX, 2);
    int n = nearest(r[12], "0123456789", 2);
    if (b1 < 0 || b2 < 0 || t1 < 0 || t2 < 0 || n < 1 || n > 9) return false;
    h.block = b1 * 16 + b2; h.total = t1 * 16 + t2;
    if (h.block < 1 || h.total < 1 || h.block > h.total) return false;
    h.name.clear();
    for (int i = 0; i < n && 13 + i < 40; i++) h.name += (char)c(13 + i);
    for (char ch : h.name) if (ch < 33 || ch > 126) return false;
    return true;
}
}  // namespace

std::string telesoftUnescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '|' && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d == '|' || d == '`' || d == '#' || d == '_') o += (char)d;
            else if (d >= 0x40 && d <= 0x5F) o += (char)(d - 0x40);
            else { o += (char)c; o += (char)d; }
            i++;
            continue;
        }
        o += (char)c;
    }
    return o;
}

struct TsCollector::Impl {
    Copy cur[9];                          // per magazine: the page being sent
    bool curOn[9] = {};
    std::vector<std::pair<Header, Copy>> copies;
    std::map<std::pair<std::string, int>, int> perBlock;   // copies kept of (name, block): at most 20
    void close(int mag)
    {
        if (!curOn[mag]) return;
        curOn[mag] = false;
        Copy& c = cur[mag];
        Header h;
        if (c.have[1] && parseHeader(c.rows[1], h)) {
            int& n = perBlock[{h.name, h.block}];
            if (n < 20) { n++; copies.push_back({h, c}); }
        }
    }
};

TsCollector::TsCollector() : d_(std::make_shared<Impl>()) {}

size_t TsCollector::blocksSeen() const { return d_->copies.size(); }

void TsCollector::packet(const uint8_t* p)
{
    int a = ham84(p[0]), b = ham84(p[1]);
    if (a < 0 || b < 0) return;
    int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
    Impl& d = *d_;
    if (row == 0) {
        d.close(mag);
        int pu = ham84(p[2]), pt = ham84(p[3]);
        d.cur[mag] = Copy();
        d.cur[mag].page = (pu >= 0 && pt >= 0) ? mag * 100 + pt * 10 + pu : 0;
        d.curOn[mag] = true;
        return;
    }
    if (row > 23 || !d.curOn[mag]) return;
    Copy& c = d.cur[mag];
    std::memcpy(c.rows[row].data(), p + 2, 40);
    c.have[row] = true;
    // the page is closed by its last row too (a page whose header comes long after is not lost)
    if (row == 23) d.close(mag);
}

std::vector<TsProgram> telesoftOf(const std::string& t42)
{
    TsCollector c;
    Bytes data;
    try { data = read_file(t42); } catch (...) { return {}; }
    for (size_t o = 0; o + 42 <= data.size(); o += 42) c.packet(&data[o]);
    return c.programs();
}

std::vector<TsProgram> TsCollector::programs() const
{
    std::vector<TsProgram> out;
    // the pages still open count too
    Impl d = *d_;
    for (int m = 0; m < 9; m++) d.close(m);
    const auto& copies = d.copies;
    if (copies.empty()) return out;
    // the name of a program: a name 1 character from a three times more frequent one is that one
    std::map<std::string, int> freq;
    for (auto& c : copies) freq[c.first.name]++;
    auto near = [](const std::string& x, const std::string& y) {
        if (x.size() != y.size()) return false;
        int d = 0;
        for (size_t i = 0; i < x.size(); i++) d += x[i] != y[i];
        return d <= 1;
    };
    // ... and of the same number of blocks (MUSIC7 and MUSIC8 stay two)
    std::map<std::string, std::map<int, int>> ownTotals;
    for (auto& c : copies) ownTotals[c.first.name][c.first.total]++;
    auto mainTotal = [&](const std::string& n) {
        auto& t = ownTotals[n];
        return std::max_element(t.begin(), t.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
    };
    std::map<std::string, std::string> canon;
    for (auto& kv : freq) {
        std::string best = kv.first;
        for (auto& o : freq)
            if (near(o.first, kv.first) && o.second >= 3 * kv.second && o.second > freq[best] && mainTotal(o.first) == mainTotal(kv.first)) best = o.first;
        canon[kv.first] = best;
    }
    std::map<std::string, std::map<int, int>> totals;
    for (auto& c : copies) totals[canon[c.first.name]][c.first.total]++;
    // every block of every program voted (bytes whose parity no copy got right are marked), then those bytes are
    // chosen among their one-bit changes by a character bigram model of the confident bytes
    struct Blk { std::string text; std::vector<char> bad; };
    std::vector<std::map<int, Blk>> blocks;
    for (auto& tv : totals) {
        TsProgram pr;
        pr.name = tv.first;
        pr.blocks = std::max_element(tv.second.begin(), tv.second.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
        pr.copies.assign(pr.blocks, 0);
        pr.page.assign(pr.blocks, 0);
        std::map<int, Blk> bl;
        for (int blk = 1; blk <= pr.blocks; blk++) {
            std::vector<const Copy*> cs;
            for (auto& c : copies)
                if (canon[c.first.name] == pr.name && c.first.block == blk) cs.push_back(&c.second);
            pr.copies[blk - 1] = (int)cs.size();
            if (cs.empty()) continue;
            pr.page[blk - 1] = cs[0]->page;
            Blk& b = bl[blk];
            for (int r = 1; r <= 23; r++)
                for (int i = 0; i < 40; i++) {
                    int cntGood[128] = {}, cntAll[128] = {};
                    bool any = false;
                    for (auto* c : cs) {
                        if (!c->have[r]) continue;
                        any = true;
                        uint8_t v = c->rows[r][i];
                        cntAll[v & 0x7F]++;
                        if (oddParity(v)) cntGood[v & 0x7F]++;
                    }
                    if (!any) { b.text += '\x01'; b.bad.push_back(0); continue; }   // row not received
                    int bv = 0, bn = -1;
                    for (int v = 0; v < 128; v++) {
                        int n = cntGood[v] * 4 + cntAll[v];
                        if (n > bn) { bn = n; bv = v; }
                    }
                    b.text += (char)bv;
                    b.bad.push_back(cntGood[bv] == 0);
                }
        }
        out.push_back(pr);
        blocks.push_back(std::move(bl));
    }
    // a 0 read as its letter twin (P @ p: bits 4-6) right after a digit, before a separator; then confident
    for (auto& bl : blocks)
        for (auto& kv : bl) {
            Blk& b = kv.second;
            for (size_t i = 1; i + 1 < b.text.size(); i++) {
                char c = b.text[i], pc = b.text[i - 1], nc = b.text[i + 1];
                if ((c == 'P' || c == '@' || c == 'p') && pc >= '0' && pc <= '9' && !isalnum((unsigned char)nc) && nc != '%' && nc != '$' && nc != '(' && nc != '.')
                    { b.text[i] = '0'; b.bad[i] = 0; }
            }
        }
    static double lp[128][128];
    {
        static double cnt[128][128];
        for (auto& r : cnt) for (auto& v : r) v = 1;
        for (auto& bl : blocks)
            for (auto& kv : bl) {
                const Blk& b = kv.second;
                for (size_t i = 0; i + 1 < b.text.size(); i++)
                    if (!b.bad[i] && !b.bad[i + 1] && b.text[i] != 1 && b.text[i + 1] != 1)
                        cnt[(uint8_t)b.text[i] & 0x7F][(uint8_t)b.text[i + 1] & 0x7F] += 1;
            }
        for (int a = 0; a < 128; a++) {
            double s = 0;
            for (int b = 0; b < 128; b++) s += cnt[a][b];
            for (int b = 0; b < 128; b++) lp[a][b] = std::log(cnt[a][b] / s);
        }
    }
    for (auto& bl : blocks)
        for (auto& kv : bl) {
            Blk& b = kv.second;
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < b.text.size(); i++) {
                    if (!b.bad[i]) continue;
                    int v = (uint8_t)b.text[i] & 0x7F;
                    int best = v; double bs = -1e9;
                    for (int k = -1; k < 7; k++) {
                        int c = k < 0 ? v : (v ^ (1 << k));
                        double sc = 0;
                        if (i > 0) sc += lp[(uint8_t)b.text[i - 1] & 0x7F][c];
                        if (i + 1 < b.text.size()) sc += lp[c][(uint8_t)b.text[i + 1] & 0x7F];
                        if (sc > bs) { bs = sc; best = c; }
                    }
                    b.text[i] = (char)best;
                }
        }
    for (size_t pi = 0; pi < out.size(); pi++) {
        TsProgram& pr = out[pi];
        std::string body, listing;
        for (int blk = 1; blk <= pr.blocks; blk++) {
            auto it = blocks[pi].find(blk);
            if (it == blocks[pi].end()) {
                listing += fmt("\x07[*** block %d of %d not received ***]\x07", blk, pr.blocks);
                continue;
            }
            const std::string& text = it->second.text;
            size_t s = text.find("|s~Z");
            std::string b = s != std::string::npos ? text.substr(s + 4) : text;
            if (!b.empty() && b[0] == '0') b.erase(0, 1);
            size_t e = b.find("|c");
            if (e != std::string::npos) b.resize(e);
            for (char& ch : b) if (ch == '\x01') ch = '?';
            // a 0 read as its letter twin (P @ p: bits 4-6) right after a digit, before a separator
            for (size_t i = 1; i < b.size(); i++) {
                char c = b[i], pc = b[i - 1], nc = i + 1 < b.size() ? b[i + 1] : '\x07';
                if ((c == 'P' || c == '@' || c == 'p') && pc >= '0' && pc <= '9' && !isalnum((unsigned char)nc) && nc != '%' && nc != '$' && nc != '(' && nc != '.')
                    b[i] = '0';
            }
            body += b;
            listing += b;
        }
        std::string exec;
        for (char ch : body) exec += ch == '\x07' ? '\r' : ch;
        exec = telesoftUnescape(exec);
        while (!exec.empty() && exec.back() == '\r') exec.pop_back();
        pr.exec = exec + "\r";
        std::string lst;
        for (char ch : listing) {
            if (ch == '\x07') lst += "\r\n";
            else lst += ch;
        }
        pr.listing = lst;
    }
    return out;
}

void addTelesoftFinds(std::vector<Find>& out, const std::vector<TsProgram>& progs, const std::string& where, const std::string& dir, bool write)
{
    for (auto& p : progs) {
        Find x;
        x.kind = Find::File;
        x.name = p.name;
        x.type = "BBC BASIC program (telesoftware): runs in the BBC Micro window";
        x.service = "BBC Ceefax telesoftware (teletext)";
        x.where = where;
        x.blocks = p.blocks;
        x.have = p.have();
        x.blockBytes = 920;
        x.size = p.exec.size();
        int copies = 0;
        for (int c : p.copies) copies += c;
        x.status = p.complete() ? fmt("complete, %d blocks (%d copies)", p.blocks, copies) : fmt("%d of %d blocks", p.have(), p.blocks);
        std::string pages;
        for (int g : p.page) if (g && pages.find(fmt("%d", g)) == std::string::npos) pages += (pages.empty() ? "page " : ", ") + fmt("%d", g);
        x.note = pages + (p.complete() ? "" : "; the missing blocks are left out: the program may not run");
        if (!dir.empty()) {
            std::string fn = path_join(dir, p.name + ".bbc");
            if (write) {
                try {
                    make_dirs(dir);
                    write_file(fn, p.exec.data(), p.exec.size());
                    write_text(path_join(dir, p.name + ".txt"), p.listing);
                    x.path = fn;
                } catch (...) {}
            } else if (exists(fn)) x.path = fn;
        }
        for (auto& l : split(replace_all(p.listing, "\r\n", "\n"), '\n')) x.text.push_back(l);
        out.push_back(x);
    }
}
