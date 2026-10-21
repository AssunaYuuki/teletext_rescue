#include "ttdata.h"
#include "nabts/teletext.h"
#include "util.h"
#include <algorithm>

namespace {
int codewords(const uint8_t* p, int n, int* nonPad)
{
    int c = 0, np = 0;
    for (int k = 0; k < n; k++) {
        c += ham::dec[p[k]] >= 0;
        np += p[k] != 0x20;
    }
    if (nonPad) *nonPad = np;
    return c;
}

// CP866 Russian letters (the DOS text of the data receivers), other bytes above 0x7F as '?'
std::string cp866(uint8_t x)
{
    static const char16_t a[] = u"АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдежзийклмноп", b[] = u"рстуфхцчшщъыьэюя";
    char16_t u = x >= 0x80 && x < 0xB0 ? a[x - 0x80] : x >= 0xE0 && x < 0xF0 ? b[x - 0xE0] : u'?';
    if (u < 0x80) return std::string(1, (char)u);
    std::string s;
    s += (char)(0xC0 | (u >> 6));
    s += (char)(0x80 | (u & 0x3F));
    return s;
}
}

void TtDataPages::packet(const uint8_t* p)
{
    int a = ham::dec[p[0]], b = ham::dec[p[1]];
    if (a < 0 || b < 0) {   // a header with one wrong bit in its address (as in pagebuild.cpp)
        int fa = ham::fix[p[0]], fb = ham::fix[p[1]];
        bool hdr = fa >= 0 && fb >= 0 && ((fa >> 3) | (fb << 1)) == 0;
        for (int k = 2; k < 8 && hdr; k++) hdr = ham::dec[p[k]] >= 0;
        if (!hdr) return;
        a = fa; b = fb;
    }
    int m = a & 7, row = (a >> 3) | (b << 1);
    if (row == 0) {
        // a transmission that brought no data rows is not kept
        if (open_[m] >= 0 && open_[m] == (int)tx_.size() - 1 && tx_.back().rows.empty()) tx_.pop_back();
        open_[m] = -1;
        int u = ham::dec[p[2]], t = ham::dec[p[3]];
        if (u < 0 || t < 0 || u > 9 || t > 9) return;
        int s[4];
        for (int k = 0; k < 4; k++) s[k] = ham::dec[p[4 + k]];
        int c = ham::dec[p[8]];
        Tx x;
        x.page = (m ? m : 8) * 0x100 + t * 16 + u;
        x.sub = s[0] < 0 || s[1] < 0 || s[2] < 0 || s[3] < 0 ? -1 : s[0] | (s[1] & 7) << 4 | s[2] << 8 | (s[3] & 3) << 12;
        x.inhibit = c >= 0 && (c & 8);
        for (int k = 10; k < 42; k++) x.header += (char)((p[k] & 0x7F) >= 0x20 && (p[k] & 0x7F) < 0x7F ? p[k] & 0x7F : ' ');
        if (tx_.size() >= 50000) return;   // a day of live capture is far below this
        tx_.push_back(std::move(x));
        open_[m] = (int)tx_.size() - 1;
        return;
    }
    if (row > 24 || open_[m] < 0) return;
    tx_[open_[m]].sent++;
    // a data row: (nearly) every byte that is not padding is a codeword. Text pages have far fewer: of the
    // 7-bit characters with odd parity only 16 are codewords
    int np = 0, c = codewords(p + 2, 40, &np);
    if (c < 8 || c * 10 < np * 8) return;
    auto& r = tx_[open_[m]].rows;
    if (!r.count(row)) r[row].assign(p + 2, p + 42);
}

std::vector<TtDataPage> TtDataPages::pages() const
{
    std::map<std::pair<int, int>, std::vector<const Tx*>> by;
    for (auto& t : tx_)
        // most rows of the page are data, three of them in rows 1-20: the «Бизнес-Текст» catalogue (600-602)
        // has only rows 21-23 in Hamming 8/4 among 8-bit rows, and it is read by biznestext.cpp
        if (t.sub >= 0 && (int)t.rows.size() * 2 >= t.sent && std::count_if(t.rows.begin(), t.rows.end(), [](auto& r) { return r.first <= 20; }) >= 3)
            by[{t.page, t.sub}].push_back(&t);
    std::vector<TtDataPage> out;
    for (auto& kv : by) {
        TtDataPage d;
        d.page = kv.first.first; d.sub = kv.first.second;
        d.copies = (int)kv.second.size();
        int maxRow = 0;
        std::map<std::string, int> hdr;
        for (auto* t : kv.second) {
            d.inhibit |= t->inhibit;
            hdr[t->header]++;
            maxRow = std::max(maxRow, t->rows.rbegin()->first);
        }
        d.headerText = std::max_element(hdr.begin(), hdr.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
        std::vector<int> known;   // per byte of d.data: read (1) or not (0)
        for (int row = 1; row <= maxRow; row++) {
            // every byte voted over the copies: the most common codeword
            std::vector<int> v(40, -1);
            bool any = false;
            for (int k = 0; k < 40; k++) {
                std::map<int, int> cnt;
                for (auto* t : kv.second) {
                    auto it = t->rows.find(row);
                    if (it != t->rows.end()) cnt[it->second[k]]++;
                }
                if (cnt.empty()) continue;
                any = true;
                int best = -1, bc = 0, raw = -1, rc = 0;
                for (auto& c : cnt) {
                    if (ham::dec[c.first] >= 0 && c.second > bc) { best = c.first; bc = c.second; }
                    if (c.second > rc) { raw = c.first; rc = c.second; }
                }
                // no codeword in any copy: not read (a bit slip turns 0x15 into 0x2B, which "corrects" to 7)
                v[k] = best >= 0 ? ham::dec[best] : raw == 0x20 ? -2 : -1;
            }
            int len = 40;
            if (any)
                while (len > 0 && v[len - 1] == -2) len--;   // padding after the data
            else
                len = 40;   // a row no copy brought: 20 bytes not read
            for (int k = 0; k < len; k += 2) {
                int lo = v[k], hi = k + 1 < 40 ? v[k + 1] : -1;
                bool ok = lo >= 0 && hi >= 0;
                uint8_t byte = ok ? (uint8_t)(lo | hi << 4) : 0;
                if (row == 1) d.head.push_back(byte);
                else { d.data.push_back(byte); known.push_back(ok); d.unread += !ok; }
            }
        }
        // text: printable characters and line ends
        int good = 0, nk = 0;
        bool allZero = true;
        for (size_t i = 0; i < d.data.size(); i++) {
            if (!known[i]) continue;
            nk++;
            uint8_t c = d.data[i];
            good += (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
            allZero &= c == 0;
        }
        d.zeros = nk > 0 && allZero;
        if (!d.zeros && nk && good * 10 >= nk * 8) {
            std::string line;
            for (size_t i = 0; i < d.data.size(); i++) {
                uint8_t c = d.data[i];
                if (!known[i]) line += '?';
                else if (c == '\n') { d.text.push_back(line); line.clear(); }
                else if (c == '\r') {}
                else if (c >= 0x20 && c < 0x7F) line += (char)c;
                else if (c == '\t') line += ' ';
                else line += c >= 0x80 ? cp866(c) : "?";
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty()) d.text.push_back(line);
        }
        out.push_back(std::move(d));
    }
    return out;
}

TtDataPages ttDataPagesOf(const std::string& t42)
{
    TtDataPages t;
    if (!exists(t42)) return t;
    Bytes d = read_file(t42);
    for (size_t i = 0; i + 42 <= d.size(); i += 42) t.packet(&d[i]);
    return t;
}
