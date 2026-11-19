#include "teledata.h"
#include "nabts/teletext.h"
#include "ui/util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace {
struct DecodedTx {
    std::array<std::array<uint8_t, 35>, 23> data{};
    std::array<std::array<char, 35>, 23> known{};
    std::array<std::array<uint8_t, 40>, 23> raw{};
    int rows = 0, kind = 0;   // 2 = compressed/high-entropy, 1 = text, 0 = service or other
};

bool plausibleText(const std::array<uint8_t, kTeledataBlockBytes>& data,
                   const std::array<char, kTeledataBlockBytes>& known);

double entropyOf(const std::array<uint8_t, kTeledataBlockBytes>& data,
                 const std::array<char, kTeledataBlockBytes>& known)
{
    int n = 0, histogram[256] = {};
    for (size_t i = 0; i < data.size(); i++) if (known[i]) { histogram[data[i]]++; n++; }
    double entropy = 0;
    for (int c : histogram) if (c && n) {
        double q = (double)c / n;
        entropy -= q * log2(q);
    }
    return entropy;
}

template<class Tx>
DecodedTx decodeTx(const Tx& tx)
{
    DecodedTx d;
    for (const auto& rb : tx.rows) {
        int r = rb.first;
        if (r < 1 || r > 23) continue;
        d.rows++;
        d.raw[r - 1] = rb.second;
        for (int at = 0; at < 40; at += 8)
            for (int j = 0; j < 7; j++) {
                uint8_t v = 0;
                for (int i = 0; i < 8; i++) v |= ((rb.second[at + i] >> j) & 1) << i;
                d.data[r - 1][at / 8 * 7 + j] = v;
            }
        d.known[r - 1].fill(1);
    }
    std::array<uint8_t, kTeledataBlockBytes> data{};
    std::array<char, kTeledataBlockBytes> known{};
    for (int r = 0; r < 18; r++) {
        std::copy(d.data[r].begin(), d.data[r].end(), data.begin() + r * 35);
        std::copy(d.known[r].begin(), d.known[r].end(), known.begin() + r * 35);
    }
    double entropy = entropyOf(data, known);
    d.kind = entropy >= 7 ? 2 : plausibleText(data, known) ? 1 : 0;
    return d;
}

double distance(const DecodedTx& a, const DecodedTx& b)
{
    int samePlace = 0, different = 0;
    for (int r = 0; r < 18; r++)
        for (int k = 0; k < 35; k++)
            if (a.known[r][k] && b.known[r][k]) {
                samePlace++;
                different += a.data[r][k] != b.data[r][k];
                if (different * 100 > (int)kTeledataBlockBytes * 23) return 1;
            }
    return samePlace >= 35 ? (double)different / samePlace : 1;
}

bool plausibleText(const std::array<uint8_t, kTeledataBlockBytes>& data,
                   const std::array<char, kTeledataBlockBytes>& known)
{
    int n = 0, good = 0, zeros = 0, cr = 0, lf = 0;
    for (size_t i = 0; i < data.size(); i++) {
        if (!known[i]) continue;
        uint8_t c = data[i];
        n++;
        good += c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127) || (c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xEF);
        zeros += c == 0;
        cr += c == 13;
        lf += c == 10;
    }
    return n >= 35 && good * 20 >= n * 11 && zeros * 10 < n * 3 && cr >= 2 && lf >= 2;
}

int decimalBcd(int v)
{
    return v >= 0 && v <= 99 ? (v / 10) << 4 | v % 10 : -1;
}
}

int teledataNextBcd(int v)
{
    return (v & 15) == 9 ? v + 7 : v + 1;
}

void TeledataPages::packet(const uint8_t* p)
{
    int a = ham::dec[p[0]], b = ham::dec[p[1]];
    if (a < 0 || b < 0) {
        a = ham::fix[p[0]];
        b = ham::fix[p[1]];
        if (a < 0 || b < 0) return;
    }
    int m = a & 7, row = (a >> 3) | (b << 1);
    if (row == 0) {
        open_[m] = -1;
        if (m != 0 || tx_.size() >= 50000) return;
        int h[6];
        for (int k = 0; k < 6; k++) {
            h[k] = ham::dec[p[2 + k]];
            if (h[k] < 0) h[k] = ham::fix[p[2 + k]];
        }
        for (int k = 0; k < 6; k++) if (h[k] < 0) return;
        if (h[0] > 9 || h[1] > 9 || h[2] > 9 || (h[3] & 7) > 9 || h[4] != 0 || (h[5] & 3) != 1) return;
        Tx tx;
        tx.page = 0x800 | h[1] << 4 | h[0];
        tx.sub = h[2] | (h[3] & 7) << 4 | h[4] << 8 | (h[5] & 3) << 12;
        tx_.push_back(std::move(tx));
        open_[m] = (int)tx_.size() - 1;
        return;
    }
    if (m != 0 || row < 1 || row > 23 || open_[m] < 0) return;
    auto& rows = tx_[open_[m]].rows;
    if (!rows.count(row)) {
        std::array<uint8_t, 40> data;
        std::copy(p + 2, p + 42, data.begin());
        rows[row] = data;
    }
}

std::vector<TeledataBlock> TeledataPages::blocks() const
{
    std::map<std::pair<int, int>, std::vector<DecodedTx>> slots;
    for (const Tx& tx : tx_) slots[{tx.page, tx.sub}].push_back(decodeTx(tx));
    std::vector<TeledataBlock> out;
    for (const auto& kv : slots) {
        const auto& versions = kv.second;
        // A slot can be reused later. Binary copies are grouped by their data kind; text uses all similar copies.
        for (int mixed = 0; mixed < 2; mixed++) {
            std::vector<int> seeds;
            std::map<std::string, char> prefixes;
            for (int i = 0; i < (int)versions.size(); i++) {
                std::string prefix;
                if (!mixed) prefix += (char)versions[i].kind;
                for (int k = 0; k < 8; k++) {
                    prefix += versions[i].known[0][k] ? versions[i].data[0][k] : 0;
                    prefix += versions[i].known[0][k] ? 1 : 0;
                }
                if (prefixes.emplace(prefix, 0).second) seeds.push_back(i);
            }
            if (mixed) {
                int seed = 0;
                for (int i = 1; i < (int)versions.size(); i++) if (versions[i].rows > versions[seed].rows) seed = i;
                seeds = {seed};
            }
            std::map<std::vector<int>, char> made;
            for (int seed : seeds) {
                std::vector<int> copies;
                for (int i = 0; i < (int)versions.size(); i++)
                    if ((mixed || versions[i].kind == versions[seed].kind) && distance(versions[i], versions[seed]) < 0.23) copies.push_back(i);
                if (!made.emplace(copies, 0).second) continue;
            TeledataBlock b;
            b.page = kv.first.first;
            b.sub = kv.first.second;
            b.mixed = mixed;
            b.copies = (int)copies.size();
            b.seedRows = versions[seed].rows;
            if (mixed) {
                int counts[18][40][256] = {};
                uint8_t voted[18][40] = {};
                for (int r = 0; r < 18; r++)
                    for (int k = 0; k < 40; k++) {
                        int bestCount = 0;
                        for (int i : copies) {
                            if (!versions[i].known[r][0]) continue;
                            uint8_t v = versions[i].raw[r][k];
                            int n = ++counts[r][k][v];
                            if (n > bestCount) { bestCount = n; voted[r][k] = v; }
                        }
                    }
                for (int r = 0; r < 18; r++) {
                    bool any = false;
                    for (int i : copies) any |= versions[i].known[r][0];
                    for (int at = 0; at < 40; at += 8)
                        for (int j = 0; j < 7; j++) {
                            uint8_t v = 0;
                            for (int k = 0; k < 8; k++) v |= ((voted[r][at + k] >> j) & 1) << k;
                            b.data[r * 35 + at / 8 * 7 + j] = v;
                            b.known[r * 35 + at / 8 * 7 + j] = any;
                        }
                }
            } else {
                int counts[18][35][256] = {};
                for (int r = 0; r < 18; r++)
                    for (int k = 0; k < 35; k++) {
                        int bestCount = 0;
                        for (int i : copies) {
                            if (!versions[i].known[r][k]) continue;
                            uint8_t v = versions[i].data[r][k];
                            int n = ++counts[r][k][v];
                            b.known[r * 35 + k] = 1;
                            if (n > bestCount) {
                                bestCount = n;
                                b.data[r * 35 + k] = v;
                            }
                        }
                        for (int i : copies) {
                            if (!versions[i].known[r][k]) continue;
                            uint8_t v = versions[i].data[r][k];
                            auto& alt = b.alts[r * 35 + k];
                            if (v != b.data[r * 35 + k] && std::find(alt.begin(), alt.end(), v) == alt.end()) alt.push_back(v);
                        }
                    }
            }
            b.control = !memcmp(b.data.data(), "N(DD", 4);
            b.entropy = entropyOf(b.data, b.known);
            b.text = plausibleText(b.data, b.known);
            out.push_back(std::move(b));
            }
        }
    }
    return out;
}

TeledataCatalog teledataCatalogue(const std::vector<TeledataBlock>& blocks)
{
    TeledataCatalog best;
    for (const auto& b : blocks) {
        std::string s((const char*)b.data.data(), b.data.size());
        if (s.rfind("File Name", 0) != 0) continue;
        TeledataCatalog c;
        c.page = b.page;
        c.sub = b.sub;
        c.copies = b.copies;
        size_t pos = 0, end = 0;
        while (pos < s.size()) {
            size_t eol = s.find('\n', pos);
            if (eol == std::string::npos) break;
            std::string line = s.substr(pos, eol - pos + 1);
            std::istringstream is(line);
            TeledataEntry f;
            unsigned long long size = 0;
            int first = 0, count = 0;
            std::string extra;
            if (is >> f.name >> size >> first >> count && !(is >> extra) && size > 0 && size <= (1ull << 30) &&
                f.name.size() <= 64 && decimalBcd(first) >= 0 && count > 0 && count <= 99) {
                f.size = size;
                f.firstPage = decimalBcd(first);
                f.pageCount = count;
                c.files.push_back(std::move(f));
                end = eol + 1;
            }
            pos = eol + 1;
        }
        if (c.files.empty() || end == 0) continue;
        bool whole = true;
        for (size_t i = 0; i < end; i++) whole &= b.known[i] != 0;
        if (!whole) continue;
        c.bytes.assign(b.data.begin(), b.data.begin() + end);
        if (c.files.size() > best.files.size() || (c.files.size() == best.files.size() && c.copies > best.copies)) best = std::move(c);
    }
    return best;
}

TeledataPages teledataPagesOf(const std::string& t42)
{
    TeledataPages t;
    if (!exists(t42)) return t;
    Bytes d = read_file(t42);
    for (size_t i = 0; i + 42 <= d.size(); i += 42) t.packet(&d[i]);
    return t;
}
