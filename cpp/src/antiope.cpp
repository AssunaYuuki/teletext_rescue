#include "antiope.h"
#include "mlse.h"
#include "fastdata_core.h"
#include "png.h"
#include "saa5050.h"
#include "ui/util.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>

namespace {

constexpr int kData = 32;   // data bytes after the 5-byte prefix

struct Packet {
    int64_t frame = 0;
    int line = 0;           // TV line, for the order within a frame
    int addr = 0, ci = 0, fmt = 0;
    std::array<uint8_t, kData> d{};
};

mlse::Model didonModel(double spb)
{
    mlse::Model m;
    m.spb = spb;
    m.pre[0] = 0x55; m.pre[1] = 0x55; m.pre[2] = 0xE7;
    m.nbytes = 5 + kData;
    m.good = [](const uint8_t *p) { for (int i = 0; i < 5; i++) if (mlse::hamExact(p[i]) < 0) return false; return true; };
    m.code = [](const uint8_t *, int i) { return i < 5 ? 1 : 2; };
    return m;
}

bool lineSignal(const uint8_t *p, const VbiGeometry &g, std::vector<float> &y, float &edge)
{
    float info[4];
    fd_prepare(p, g.fmt, g.validSamples, g.lineSamples, info);
    if (info[3] <= 0) return false;
    edge = info[0];
    std::vector<float> raw(g.validSamples);
    for (int k = 0; k < g.validSamples; k++) raw[k] = fd_get(p, g.fmt, k);
    y.resize(g.validSamples);
    mlse::normalize(raw.data(), g.validSamples, y.data());
    return true;
}

template <class F> void parallelFor(int n, int threads, F fn)
{
    int nt = threads > 0 ? threads : (int)std::max(1u, std::thread::hardware_concurrency());
    nt = std::max(1, std::min(nt, n));
    std::atomic<int> next{0};
    std::vector<std::thread> th;
    for (int t = 0; t < nt; t++)
        th.emplace_back([&] { for (int i; (i = next.fetch_add(1)) < n;) fn(i); });
    for (auto &t : th) t.join();
}

bool oddParity(uint8_t v) { int n = 0; for (int i = 0; i < 8; i++) n += (v >> i) & 1; return n & 1; }

// ---------------------------------------------------------------- page drawing
struct Cell : AntiopeCell { bool bad = false; };
using Grid = std::array<std::array<Cell, 40>, 25>;
const char *kMosaicText = "\xE2\x96\x92";   // a mosaic cell in the text of a page

bool lightColour(int c) { return c == 2 || c == 3 || c == 6 || c == 7; }

const char *accentOf(int a)   // SS2 4/1.. : the non-spacing diacritics of the videotex G2 set
{
    switch (a) {
    case 0x41: return "\xCC\x80";   // grave
    case 0x42: return "\xCC\x81";   // acute
    case 0x43: return "\xCC\x82";   // circumflex
    case 0x44: return "\xCC\x83";   // tilde
    case 0x48: return "\xCC\x88";   // diaeresis
    case 0x4A: return "\xCC\x8A";   // ring
    case 0x4B: return "\xCC\xA7";   // cedilla
    default: return nullptr;
    }
}
// a letter with its accent as one character where Latin-1 has it (the pages keep to French)
std::string accented(char c, int a)
{
    static const std::map<std::pair<char, int>, const char *> t = {
        {{'a', 0x41}, "\xC3\xA0"}, {{'e', 0x41}, "\xC3\xA8"}, {{'u', 0x41}, "\xC3\xB9"}, {{'A', 0x41}, "\xC3\x80"}, {{'E', 0x41}, "\xC3\x88"},
        {{'e', 0x42}, "\xC3\xA9"}, {{'E', 0x42}, "\xC3\x89"}, {{'a', 0x42}, "\xC3\xA1"}, {{'o', 0x42}, "\xC3\xB3"}, {{'i', 0x42}, "\xC3\xAD"},
        {{'a', 0x43}, "\xC3\xA2"}, {{'e', 0x43}, "\xC3\xAA"}, {{'i', 0x43}, "\xC3\xAE"}, {{'o', 0x43}, "\xC3\xB4"}, {{'u', 0x43}, "\xC3\xBB"},
        {{'E', 0x43}, "\xC3\x8A"}, {{'e', 0x48}, "\xC3\xAB"}, {{'i', 0x48}, "\xC3\xAF"}, {{'u', 0x48}, "\xC3\xBC"}, {{'o', 0x48}, "\xC3\xB6"},
        {{'a', 0x48}, "\xC3\xA4"}, {{'c', 0x4B}, "\xC3\xA7"}, {{'C', 0x4B}, "\xC3\x87"}, {{'n', 0x44}, "\xC3\xB1"}, {{'A', 0x43}, "\xC3\x82"},
        {{'I', 0x43}, "\xC3\x8E"}, {{'O', 0x43}, "\xC3\x94"}, {{'U', 0x43}, "\xC3\x9B"}, {{'E', 0x48}, "\xC3\x8B"}, {{'I', 0x48}, "\xC3\x8F"},
        {{'U', 0x41}, "\xC3\x99"}, {{'a', 0x4A}, "\xC3\xA5"}, {{'A', 0x4A}, "\xC3\x85"}};
    auto it = t.find({c, a});
    if (it != t.end()) return it->second;
    std::string s(1, c);
    if (const char *d = accentOf(a)) s += d;
    return s;
}
// the spacing characters of the G2 set (SS2 x)
const char *g2Char(int x)
{
    switch (x) {
    case 0x23: return "\xC2\xA3";       // pound
    case 0x24: return "$";
    case 0x26: return "#";
    case 0x27: return "\xC2\xA7";       // section
    case 0x2C: return "\xE2\x86\x90";   // arrows
    case 0x2D: return "\xE2\x86\x91";
    case 0x2E: return "\xE2\x86\x92";
    case 0x2F: return "\xE2\x86\x93";
    case 0x30: return "\xC2\xB0";       // degree
    case 0x31: return "\xC2\xB1";
    case 0x38: return "\xC3\xB7";
    case 0x3C: return "\xC2\xBC";
    case 0x3D: return "\xC2\xBD";
    case 0x3E: return "\xC2\xBE";
    case 0x6A: return "\xC5\x92";       // OE
    case 0x7A: return "\xC5\x93";       // oe
    case 0x7B: return "\xC3\x9F";       // sharp s
    default: return nullptr;
    }
}

// the screen colour from the header nibbles after the page number (nibble 8 = 1ccc); black when not sent
int screenOf(const std::vector<int> &nib) { return nib.size() > 8 && (nib[8] & 8) ? nib[8] & 7 : 0; }

Grid blankGrid(int screen)
{
    Grid g;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            g[r][c].bg = r ? (uint8_t)screen : 0;
            g[r][c].fg = r && lightColour(screen) ? 0 : 7;
        }
    return g;
}

// the page from its data group (from = the first byte after the header nibbles)
Grid render(const std::vector<int> &g, size_t from, int screen)
{
    Grid grid = blankGrid(screen);
    int r = 1, c = 0;
    struct Attr {
        uint8_t fg = 7, bg = 0, pbg = 0;
        bool dh = false, dw = false, mos = false, inv = false, flash = false;
        bool ul = false, pul = false, conc = false, pconc = false;
    } a;
    auto reset = [&] {
        a = Attr();
        a.bg = a.pbg = r > 0 ? (uint8_t)screen : 0;
        a.fg = r > 0 && lightColour(screen) ? 0 : 7;
    };
    reset();
    auto moveRow = [&](int nr) { if (nr != r) { r = nr; reset(); } };
    struct Last { std::string ch; uint8_t mos = 0; bool bad = false; bool ok = false; } last;
    auto put = [&](const std::string &ch, uint8_t mos, bool bad) {
        if (mos || ch == " ") { a.bg = a.pbg; a.ul = a.pul; a.conc = a.pconc; }   // a delimiter
        const bool dh = a.dh && !mos && r > 1, dw = a.dw && !mos && c < 39;
        if (r >= 0 && r < 25 && c >= 0 && c < 40) {
            Cell x;
            x.ch = mos ? std::string(" ") : ch;
            x.mosaic = mos;
            x.fg = a.fg; x.bg = a.bg;
            if (a.inv && !mos) std::swap(x.fg, x.bg);
            x.flags = (dh ? AntiopeCell::DoubleHeight : 0) | (dw ? AntiopeCell::DoubleWidth : 0) | (a.flash ? AntiopeCell::Flash : 0) |
                      (a.conc ? AntiopeCell::Conceal : 0) | (mos && a.pul ? AntiopeCell::Separated : 0) | (!mos && a.ul ? AntiopeCell::Underline : 0);
            x.bad = bad;
            grid[r][c] = x;
            auto cover = [&](int rr, int cc, uint8_t f) {
                if (rr < 0 || rr >= 25 || cc < 0 || cc >= 40) return;
                Cell k;
                k.ch = "";
                k.fg = x.fg; k.bg = x.bg;
                k.flags = f;
                grid[rr][cc] = k;
            };
            if (dw) cover(r, c + 1, AntiopeCell::CoveredLeft);
            if (dh) { cover(r - 1, c, AntiopeCell::CoveredBelow); if (dw) cover(r - 1, c + 1, AntiopeCell::CoveredBelow | AntiopeCell::CoveredLeft); }
        }
        last = {ch, mos, bad, true};
        c += dw ? 2 : 1;
        if (c >= 40) { c = 0; moveRow(r + 1); }
    };
    auto at = [&](size_t k) { return k < g.size() && g[k] >= 0 ? (g[k] & 0x7F) : -1; };
    for (size_t i = from; i < g.size(); i++) {
        if (g[i] < 0) continue;
        uint8_t raw = (uint8_t)g[i], b = raw & 0x7F;
        bool bad = !oddParity(raw);
        switch (b) {
        case 0x1F: {   // US row column
            int rr = at(i + 1) - 0x40, cc = at(i + 2) - 0x40;
            if (rr >= 0 && rr < 25 && cc > 0 && cc <= 40) { r = rr; c = cc - 1; }
            reset();
            i += 2;
            continue;
        }
        case 0x1B: {   // ESC x: an attribute
            int x = at(i + 1);
            i += 1;
            if (x >= 0x40 && x <= 0x47) a.fg = (uint8_t)(x - 0x40);
            else if (x >= 0x50 && x <= 0x57) { a.pbg = (uint8_t)(x - 0x50); if (a.mos) a.bg = a.pbg; }
            else if (x == 0x48) a.flash = true;
            else if (x == 0x49) a.flash = false;
            else if (x >= 0x4C && x <= 0x4F) { a.dh = x == 0x4D || x == 0x4F; a.dw = x == 0x4E || x == 0x4F; }
            else if (x == 0x58) a.pconc = true;
            else if (x == 0x5F) a.pconc = false;
            else if (x == 0x59) a.pul = false;
            else if (x == 0x5A) a.pul = true;
            else if (x == 0x5C) a.inv = false;
            else if (x == 0x5D) a.inv = true;
            continue;
        }
        case 0x0E: a.mos = true; continue;
        case 0x0F: a.mos = false; continue;
        case 0x19: {   // SS2: an accent and its letter, or a character of the G2 set
            int x = at(i + 1);
            if (accentOf(x)) {
                int let = at(i + 2);
                put(let >= 0x20 && let < 0x7F ? accented((char)let, x) : std::string("?"), 0, bad || let < 0x20);
                i += 2;
            } else {
                const char *s = g2Char(x);
                put(s ? std::string(s) : std::string("?"), 0, bad || !s);
                i += 1;
            }
            continue;
        }
        case 0x12: {   // REP n: the last character n more times
            int n = at(i + 1) - 0x40;
            if (last.ok)
                for (int k = 0; k < std::min(n, 40); k++) put(last.ch, last.mos, last.bad);
            i += 1;
            continue;
        }
        case 0x08: if (--c < 0) { c = 39; moveRow(std::max(0, r - 1)); } continue;   // APB
        case 0x09: if (++c >= 40) { c = 0; moveRow(r + 1); } continue;                 // APF
        case 0x0A: moveRow(r + 1); continue;                                             // APD
        case 0x0B: moveRow(std::max(0, r - 1)); continue;                                // APU
        case 0x0D: c = 0; continue;                                                      // CR
        case 0x0C: grid = blankGrid(screen); r = 1; c = 0; reset(); continue;            // FF
        case 0x1E: r = 1; c = 0; reset(); continue;                                      // RS
        case 0x18:   // CAN: the rest of the row cleared
            for (int k = c; k < 40 && r >= 0 && r < 25; k++) { grid[r][k] = Cell(); grid[r][k].fg = a.fg; grid[r][k].bg = a.bg; }
            continue;
        default: break;
        }
        if (b < 0x20 || b == 0x7F) continue;
        if (a.mos && (b < 0x40 || b >= 0x60 || b == 0x5F)) put(" ", b == 0x5F ? 0x7F : b, bad);
        else put(std::string(1, (char)b), 0, bad);
    }
    return grid;
}

std::vector<std::string> rowsOf(const Grid &g)
{
    std::vector<std::string> out(25);
    for (int r = 0; r < 25; r++) {
        std::string s;
        for (int c = 0; c < 40; c++) {
            const Cell &x = g[r][c];
            if (x.flags & AntiopeCell::CoveredLeft) continue;     // the second half of a wide letter
            if (x.flags & AntiopeCell::CoveredBelow) { s += ' '; continue; }
            s += x.mosaic ? (x.mosaic == 0x20 ? " " : kMosaicText) : x.ch;
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        out[r] = s;
    }
    return out;
}

// share of the cells that carry something in either grid and are the same
double cellSim(const Grid &a, const Grid &b)
{
    int same = 0, tot = 0;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const Cell &x = a[r][c], &y = b[r][c];
            bool ex = x.ch == " " && !x.mosaic, ey = y.ch == " " && !y.mosaic;
            if (ex && ey) continue;
            tot++;
            same += x.ch == y.ch && x.mosaic == y.mosaic;
        }
    return tot ? (double)same / tot : 1;
}

// mean row similarity 2 * LCS / (|a| + |b|) over the rows with text: one character a tape put in moves the rest of a
// row, which the cell comparison takes for another text
double rowSim(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    double sum = 0;
    int n = 0;
    for (int r = 0; r < 25; r++) {
        const std::string &x = a[r], &y = b[r];
        bool ex = x.find_first_not_of(' ') == std::string::npos, ey = y.find_first_not_of(' ') == std::string::npos;
        if (ex && ey) continue;
        std::vector<std::vector<int>> L(x.size() + 1, std::vector<int>(y.size() + 1, 0));
        for (size_t i = 1; i <= x.size(); i++)
            for (size_t j = 1; j <= y.size(); j++) L[i][j] = x[i - 1] == y[j - 1] ? L[i - 1][j - 1] + 1 : std::max(L[i - 1][j], L[i][j - 1]);
        sum += x.size() + y.size() ? 2.0 * L[x.size()][y.size()] / (x.size() + y.size()) : 1;
        n++;
    }
    return n ? sum / n : 1;
}

struct Copy { Grid g; int screen = 0; };

// every cell (character, mosaic and attributes together) and the screen colour by the majority of the copies
Copy vote(const std::vector<Copy> &cs)
{
    Copy out;
    std::map<int, int> sc;
    for (auto &c : cs) sc[c.screen]++;
    int best = -1;
    for (auto &kv : sc) if (kv.second > best) { best = kv.second; out.screen = kv.first; }
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            std::map<std::string, std::pair<double, const Cell *>> w;
            for (auto &cp : cs) {
                const Cell &x = cp.g[r][c];
                std::string key = x.ch;
                key += {'\x01', (char)x.mosaic, (char)x.fg, (char)x.bg, (char)x.flags};
                auto &e = w[key];
                e.first += x.bad ? 0.3 : 1;
                e.second = &x;
            }
            double bw = -1;
            for (auto &kv : w) if (kv.second.first > bw) { bw = kv.second.first; out.g[r][c] = *kv.second.second; }
            out.g[r][c].bad = false;
        }
    return out;
}

}  // namespace

bool antiopeDecode(VbiFile &vf, const VbiGeometry &g, int64_t frames, int threads, const std::function<void(double)> &progress,
                   const std::atomic<bool> *cancel, AntiopeResult &out)
{
    out = AntiopeResult();
    if (!g.pal || frames < 50) return false;
    const double spb = g.lineSamples / 397.0;
    if (spb < 2.2) return false;   // a 13.5 MHz capture has too few samples a bit
    // ---- which lines: a channel model learnt on up to 400 frames of each VBI line, kept when a third of the lines read
    std::vector<uint8_t> probe;
    const int64_t pn = std::min<int64_t>(frames, 400), first = frames / 3;
    vf.read(first, pn, probe);
    const int64_t got = (int64_t)(probe.size() / std::max(1, g.frameBytes()));
    std::vector<int> cand;
    for (int l = 0; l < g.linesPerFrame; l++) {
        int tv = tvLineOf(g, l);
        if ((tv >= 6 && tv <= 23) || (tv >= 318 && tv <= 336)) cand.push_back(l);
    }
    std::vector<int> goodN(cand.size(), 0), tried(cand.size(), 0);
    parallelFor((int)cand.size(), threads, [&](int k) {
        mlse::Engine eng(didonModel(spb));
        std::vector<float> y;
        float edge = 0;
        for (int64_t f = 0; f < got && tried[k] < 160; f++) {
            if (!lineSignal(&probe[(size_t)f * g.frameBytes() + (size_t)cand[k] * g.lineBytes], g, y, edge)) continue;
            tried[k]++;
            auto r = eng.decodeLearn(y.data(), g.validSamples, edge - 4 * spb, edge + 4 * spb, spb / 5);
            goodN[k] += r.good;
        }
    });
    std::vector<int> rows;
    for (size_t k = 0; k < cand.size(); k++)
        if (tried[k] >= 20 && goodN[k] * 3 >= tried[k]) rows.push_back(cand[k]);
    if (rows.empty()) return false;
    // ---- every frame of those lines
    std::vector<Packet> pk;
    std::vector<uint8_t> buf;
    for (size_t li = 0; li < rows.size(); li++) {
        const int row = rows[li];
        mlse::Engine eng(didonModel(spb));
        std::vector<float> y;
        float edge = 0;
        for (int64_t f = 0; f < got && eng.learned() < 150; f++)
            if (lineSignal(&probe[(size_t)f * g.frameBytes() + (size_t)row * g.lineBytes], g, y, edge))
                eng.decodeLearn(y.data(), g.validSamples, edge - 4 * spb, edge + 4 * spb, spb / 5);
        if (!eng.hasPrior()) continue;
        long lines = 0, good = 0;
        const int chunk = 256;
        for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
            if (cancel && *cancel) return false;
            int n = (int)vf.read(f0, std::min<int64_t>(chunk, frames - f0), buf);
            if (n <= 0) break;
            std::vector<mlse::Result> rs(n);
            std::vector<char> ok(n, 0);
            parallelFor(n, threads, [&](int f) {
                std::vector<float> yy;
                float e = 0;
                if (!lineSignal(&buf[(size_t)f * g.frameBytes() + (size_t)row * g.lineBytes], g, yy, e)) return;
                rs[f] = eng.decode(yy.data(), g.validSamples, e - 3 * spb, e + 3 * spb, spb / 8);
                ok[f] = 1;
            });
            for (int f = 0; f < n; f++) {
                if (!ok[f] || rs[f].data.size() < (size_t)(5 + kData)) continue;
                lines++;
                if (!rs[f].good) continue;
                good++;
                eng.learn(rs[f]);
                Packet p;
                p.frame = f0 + f;
                p.line = tvLineOf(g, row);
                int h[5];
                for (int k = 0; k < 5; k++) h[k] = mlse::hamExact(rs[f].data[k]);
                p.addr = h[0] << 8 | h[1] << 4 | h[2];
                p.ci = h[3];
                p.fmt = h[4];
                std::copy(rs[f].data.begin() + 5, rs[f].data.begin() + 5 + kData, p.d.begin());
                pk.push_back(p);
            }
            if (progress) progress(((double)li + (double)(f0 + n) / frames) / rows.size());
        }
        out.rows.push_back(row);
        out.tvLines.push_back(tvLineOf(g, row));
        out.rates.push_back(lines ? (double)good / lines : 0);
    }
    out.packets = (long long)pk.size();
    // ---- the channel of the pages: the one with the most packets whose continuity index moves (a test line read as
    // packets keeps one value)
    std::map<int, long> per, moving;
    {
        std::map<int, int> lastCi;
        for (auto &p : pk) {
            per[p.addr]++;
            auto it = lastCi.find(p.addr);
            if (it != lastCi.end() && it->second != p.ci) moving[p.addr]++;
            lastCi[p.addr] = p.ci;
        }
    }
    int chan = -1;
    long best = 0;
    for (auto &kv : moving) if (kv.second > best && kv.second * 2 > per[kv.first]) { best = kv.second; chan = kv.first; }
    if (chan < 0) return !out.rows.empty();
    out.channel = fmt("%03X", chan);
    std::vector<Packet> cp;
    for (auto &p : pk) if (p.addr == chan) cp.push_back(p);
    std::stable_sort(cp.begin(), cp.end(), [](const Packet &a, const Packet &b) { return a.frame != b.frame ? a.frame < b.frame : a.line < b.line; });
    // ---- the stream: -1 where a packet was lost
    std::vector<int> st;
    int last = -1;
    for (auto &p : cp) {
        if (last >= 0 && (last + 1) % 16 != p.ci) st.push_back(-1);
        last = p.ci;
        for (uint8_t v : p.d) st.push_back(v);
    }
    auto b7 = [&](size_t i) { return i < st.size() && st[i] >= 0 ? st[i] & 0x7F : -1; };
    // ---- data groups SOH RS ... ETX EOT that lost nothing; the page number from the header nibbles
    std::map<std::string, std::vector<std::vector<Copy>>> versions;   // page -> versions -> copies
    for (size_t i = 0; i + 1 < st.size(); i++) {
        if (b7(i) != 0x01 || b7(i + 1) != 0x1E) continue;
        size_t j = i + 2;
        bool lost = false;
        while (j + 1 < st.size()) {
            if (st[j] < 0) { lost = true; break; }
            if (b7(j) == 0x03 && b7(j + 1) == 0x04) break;
            if (b7(j) == 0x01 && b7(j + 1) == 0x1E) { lost = true; break; }
            j++;
        }
        if (lost || j + 1 >= st.size()) { i = j > i ? j - 1 : i; continue; }
        std::vector<int> grp(st.begin() + (long)i, st.begin() + (long)j + 2);
        i = j + 1;
        out.groups++;
        size_t k = 2;
        std::string num;
        std::vector<int> nib;
        while (k < grp.size() && grp[k] >= 0 && mlse::hamExact((uint8_t)grp[k]) >= 0) {
            nib.push_back(mlse::hamExact((uint8_t)grp[k]));
            if (num.size() < 3) num += fmt("%X", nib.back());
            k++;
        }
        if (num.size() < 3) continue;
        Copy cpy;
        cpy.screen = screenOf(nib);
        cpy.g = render(grp, k, cpy.screen);
        auto &vs = versions[num];
        bool placed = false;
        for (auto &v : vs)
            if (cellSim(v.front().g, cpy.g) >= 0.85) { v.push_back(cpy); placed = true; break; }
        if (!placed) vs.push_back({cpy});
    }
    // ---- vote, merge versions whose rows are alike, keep texts sent twice at least (or the page's commonest)
    for (auto &kv : versions) {
        std::vector<std::vector<Copy>> vs = kv.second;
        std::sort(vs.begin(), vs.end(), [](auto &a, auto &b) { return a.size() > b.size(); });
        struct M { std::vector<Copy> copies; std::vector<std::string> rows; };
        std::vector<M> merged;
        for (auto &v : vs) {
            auto r = rowsOf(vote(v).g);
            bool into = false;
            for (auto &m : merged)
                if (rowSim(r, m.rows) >= 0.8) {
                    m.copies.insert(m.copies.end(), v.begin(), v.end());
                    m.rows = rowsOf(vote(m.copies).g);
                    into = true;
                    break;
                }
            if (!into) merged.push_back({v, r});
        }
        std::vector<M> keep;
        for (auto &m : merged) if (m.copies.size() >= 2) keep.push_back(m);
        if (keep.empty() && !merged.empty()) keep.push_back(merged.front());
        std::sort(keep.begin(), keep.end(), [](auto &a, auto &b) { return a.copies.size() > b.copies.size(); });
        for (auto &m : keep) {
            Copy v = vote(m.copies);
            AntiopePage p;
            p.number = kv.first;
            p.copies = (int)m.copies.size();
            p.rows = rowsOf(v.g);
            p.screen = v.screen;
            p.cells.reserve(25 * 40);
            for (int r = 0; r < 25; r++)
                for (int c = 0; c < 40; c++) p.cells.push_back(v.g[r][c]);
            out.pages.push_back(p);
        }
    }
    // the service's name: the most common text of rows 1-3 of the pages "000" / "001"
    for (auto &p : out.pages) {
        if (p.number != "000" && p.number != "001") continue;
        for (int r = 0; r < 4 && out.service.empty(); r++) {
            std::string s = p.rows[r];
            size_t a = s.find_first_not_of(' ');
            if (a == std::string::npos) continue;
            s = s.substr(a);
            if (s.size() >= 4 && s.size() <= 30 && s.find(kMosaicText) == std::string::npos) out.service = s;
        }
    }
    return true;
}

std::vector<std::string> antiopeReport(const AntiopeResult &r)
{
    std::vector<std::string> o;
    if (r.rows.empty()) return o;
    std::set<std::string> numbers;
    for (auto &p : r.pages) numbers.insert(p.number);
    o.push_back(fmt("Antiope (French teletext, Didon packets): %zu pages", numbers.size()) +
                (r.service.empty() ? std::string() : " \xC2\xAB" + r.service + "\xC2\xBB"));
    std::string ls;
    for (size_t i = 0; i < r.tvLines.size(); i++)
        ls += fmt("%s%d (%.0f%%)", i ? ", " : "", r.tvLines[i] > 313 ? r.tvLines[i] - 313 : r.tvLines[i], 100 * (i < r.rates.size() ? r.rates[i] : 0)) +
              (r.tvLines[i] > 313 ? "/f2" : "");
    o.push_back(fmt("  Didon packets (teletext system A, 6.203 Mbit/s = 397 x fH, run-in 55 55, framing E7) on lines %s: %lld read "
                    "with their prefix whole",
                    ls.c_str(), r.packets));
    if (!r.channel.empty())
        o.push_back(fmt("  channel %s: %lld whole pages received; %zu page numbers, %zu texts (a page turning through several texts "
                        "gives one each), voted over their copies",
                        r.channel.c_str(), r.groups, numbers.size(), r.pages.size()));
    o.push_back("  pages: antiope/index.html (the pages as the receiver showed them), antiope/<page>.png and .txt, antiope/all.txt");
    return o;
}

std::vector<std::string> antiopeNames(const AntiopeResult &r)
{
    std::map<std::string, int> count, seen;
    for (auto &p : r.pages) count[p.number]++;
    std::vector<std::string> o;
    for (auto &p : r.pages) {
        int k = ++seen[p.number];
        o.push_back(p.number + (count[p.number] > 1 ? fmt("_%d", k) : std::string()));
    }
    return o;
}

std::string antiopeTitle(const AntiopePage &p)
{
    // the first row below the header with a few words of text (not the page number, not a mosaic bar)
    for (int k = 1; k <= (int)p.rows.size(); k++) {
        const int r = k % (int)p.rows.size();   // rows 1-24, then row 0 (the service line of page 000)
        std::string s = p.rows[r];
        if (!p.cells.empty()) {   // concealed text (the editor's stamp) is no title
            bool hidden = false;
            for (int c = 0; c < 40; c++) hidden |= (p.cells[r * 40 + c].flags & AntiopeCell::Conceal) && p.cells[r * 40 + c].ch != " ";
            if (hidden) continue;
        }
        int letters = 0;
        for (unsigned char ch : s) letters += (ch < 0x80 && std::isalpha(ch)) || ch == 0xC3;   // ASCII and Latin-1 letters
        if (letters < 5) continue;
        size_t a = s.find_first_not_of(' ');
        s = s.substr(a);
        // the text, without mosaic cells and runs of spaces
        std::string o;
        for (size_t i = 0; i < s.size();) {
            if (s.compare(i, 3, kMosaicText) == 0) { i += 3; continue; }
            if (s[i] == ' ' && !o.empty() && o.back() == ' ') { i++; continue; }
            o += s[i++];
        }
        // without the signs around it ("} LA FRANCE EN BREF")
        auto sign = [](unsigned char ch) { return ch < 0x80 && !std::isalnum(ch) && ch != '"' && ch != '(' && ch != ')'; };
        while (!o.empty() && sign((unsigned char)o.back())) o.pop_back();
        while (!o.empty() && sign((unsigned char)o.front())) o.erase(0, 1);
        return o;
    }
    return "";
}

Image antiopeImage(const AntiopePage &p, bool reveal)
{
    static const uint32_t COL[8] = {0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
    const int CW = 12, CH = 20;
    Image im(40 * CW, 25 * CH, 0);
    im.fill(0, CH, 40 * CW, 24 * CH, COL[p.screen & 7]);
    if (p.cells.size() < 25 * 40) return im;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const AntiopeCell &x = p.cells[r * 40 + c];
            if (x.flags & (AntiopeCell::CoveredLeft | AntiopeCell::CoveredBelow)) continue;   // drawn with its character
            const bool dh = x.flags & AntiopeCell::DoubleHeight, dw = x.flags & AntiopeCell::DoubleWidth;
            int px = c * CW, py = (dh ? r - 1 : r) * CH, w = (dw ? 2 : 1) * CW, h = (dh ? 2 : 1) * CH;
            uint32_t fg = COL[x.fg & 7], bg = COL[x.bg & 7];
            im.fill(px, py, w, h, bg);
            if ((x.flags & AntiopeCell::Conceal) && !reveal) continue;
            if (x.mosaic) {
                int m = x.mosaic, bits[6] = {m & 1, m & 2, m & 4, m & 8, m & 16, m & 64};
                const int gap = (x.flags & AntiopeCell::Separated) ? 2 : 0;
                for (int i = 0; i < 6; i++) {
                    if (!bits[i]) continue;
                    int x0 = px + (i % 2) * w / 2, x1 = px + (i % 2 + 1) * w / 2;
                    int y0 = py + (i / 2) * h / 3, y1 = py + (i / 2 + 1) * h / 3;
                    im.fill(x0 + gap, y0 + gap, x1 - x0 - gap, y1 - y0 - gap, fg);
                }
                continue;
            }
            if (x.ch != " " && !x.ch.empty()) {
                std::u32string u = to_u32(x.ch);
                const Glyph20 *gl = u.empty() ? nullptr : saa_rounded(u[0]);
                if (gl) {
                    for (int yy = 0; yy < h; yy++) {
                        int sy = yy * 20 / h;
                        for (int xx = 0; xx < w; xx++) if ((*gl)[sy][xx * 12 / w]) im.set(px + xx, py + yy, fg);
                    }
                } else {   // no such character in the font: a box
                    im.fill(px + 3, py + 4, w - 6, 1, fg); im.fill(px + 3, py + h - 5, w - 6, 1, fg);
                    im.fill(px + 3, py + 4, 1, h - 8, fg); im.fill(px + w - 4, py + 4, 1, h - 8, fg);
                }
            }
            if (x.flags & AntiopeCell::Underline) im.fill(px, py + h - 2, w, 2, fg);
        }
    return im;
}

namespace {

const char *kViewer = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Antiope /*TITLE*/</title>
<style>
:root{--bg:#16181c;--panel:#1f2228;--fg:#e6e6e6;--muted:#8a8d93;--acc:#3a7bd5}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,Segoe UI,sans-serif;display:flex;height:100vh}
#list{width:340px;overflow:auto;background:var(--panel);border-right:1px solid #2c3038}
#list h1{font-size:15px;margin:12px 14px 4px}#list .sub{color:var(--muted);margin:0 14px 10px;font-size:12px}
#list a{display:flex;gap:10px;padding:5px 14px;color:var(--fg);text-decoration:none;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#list a:hover{background:#2a2e36}#list a.on{background:var(--acc);color:#fff}#list b{font-family:Consolas,monospace;min-width:52px}
#main{flex:1;display:flex;flex-direction:column;align-items:center;overflow:auto;padding:16px}
#bar{display:flex;gap:8px;align-items:center;margin-bottom:12px}
#bar input{width:70px;background:#000;color:#ff0;border:1px solid #444;font:18px Consolas,monospace;padding:3px 6px;text-align:center}
#bar button{background:#2a2e36;color:var(--fg);border:1px solid #3a3f48;padding:5px 12px;cursor:pointer}
#scr{background:#000;padding:18px 26px;border-radius:10px}#scr img{display:block;width:min(720px,80vw);aspect-ratio:4/3;image-rendering:pixelated}
#txt{white-space:pre;font:13px Consolas,monospace;color:#cfcfcf;margin-top:14px;background:#0d0e10;padding:10px 14px;border-radius:6px}
#info{color:var(--muted)}
</style></head><body>
<div id="list"><h1>/*TITLE*/</h1><div class="sub">/*SUB*/</div></div>
<div id="main"><div id="bar"><button id="prev">&#9664;</button><input id="num" maxlength="3" placeholder="page"><button id="next">&#9654;</button>
<span id="info"></span></div><div id="scr"><img id="img" alt=""></div><div id="txt"></div></div>
<script>
const P=/*PAGES*/;
let cur=0;const L=document.getElementById('list');
P.forEach((p,i)=>{const a=document.createElement('a');a.href='#'+p.name;a.innerHTML='<b>'+p.name+'</b>';a.appendChild(document.createTextNode(p.title));a.onclick=e=>{e.preventDefault();show(i)};L.appendChild(a)});
function show(i){if(i<0||i>=P.length)return;cur=i;const p=P[i];document.getElementById('img').src=p.name+'.png';
document.getElementById('txt').textContent=p.text;document.getElementById('info').textContent='page '+p.number+' · '+p.copies+' copies';
document.getElementById('num').value=p.number;[...L.querySelectorAll('a')].forEach((a,k)=>a.classList.toggle('on',k==i));
L.querySelectorAll('a')[i].scrollIntoView({block:'nearest'});history.replaceState(null,'','#'+p.name)}
document.getElementById('prev').onclick=()=>show(cur-1);document.getElementById('next').onclick=()=>show(cur+1);
document.getElementById('num').oninput=e=>{const v=e.target.value.padStart(3,'0');if(e.target.value.length==3){const k=P.findIndex(p=>p.number==v);if(k>=0)show(k)}};
document.addEventListener('keydown',e=>{if(e.target.tagName=='INPUT')return;if(e.key=='ArrowRight'||e.key=='ArrowDown'||e.key=='PageDown'){show(cur+1);e.preventDefault()}
if(e.key=='ArrowLeft'||e.key=='ArrowUp'||e.key=='PageUp'){show(cur-1);e.preventDefault()}});
const h=location.hash.slice(1),k=P.findIndex(p=>p.name==h);show(k>=0?k:0);
</script></body></html>
)HTML";

std::string jsonStr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch == '\n') o += "\\n";
        else if (ch == '<') o += "\\u003c";
        else if (ch < 0x20) o += fmt("\\u%04x", ch);
        else o += (char)ch;
    }
    return o + "\"";
}

}  // namespace

std::string antiopeWrite(const AntiopeResult &r, const std::string &dir)
{
    if (r.pages.empty()) return "";
    make_dirs(dir);
    {   // the files of an earlier pass do not stay beside the new ones
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string f = it->path().filename().u8string();
            if (f.size() > 4 && (f.compare(f.size() - 4, 4, ".txt") == 0 || f.compare(f.size() - 4, 4, ".png") == 0))
                std::filesystem::remove(it->path(), ec);
        }
    }
    auto names = antiopeNames(r);
    std::string all, js = "[";
    for (size_t i = 0; i < r.pages.size(); i++) {
        const AntiopePage &p = r.pages[i];
        const std::string &name = names[i];
        std::string t;
        for (auto &row : p.rows) t += row + "\n";
        while (t.size() > 1 && t[t.size() - 1] == '\n' && t[t.size() - 2] == '\n') t.pop_back();
        write_text(path_join(dir, name + ".txt"), t);
        Image im = antiopeImage(p);
        png_save(path_join(dir, name + ".png"), im.scaled(im.w * 2, im.h * 2));
        all += fmt("===== page %s (%d copies) =====\n", name.c_str(), p.copies) + t + "\n";
        js += std::string(i ? "," : "") + "{\"name\":" + jsonStr(name) + ",\"number\":" + jsonStr(p.number) + fmt(",\"copies\":%d", p.copies) +
              ",\"title\":" + jsonStr(antiopeTitle(p)) + ",\"text\":" + jsonStr(t) + "}";
    }
    js += "]";
    std::string a = path_join(dir, "all.txt");
    write_text(a, all);
    std::string title = r.service.empty() ? std::string("pages") : r.service;
    std::string html = replace_all(kViewer, "/*TITLE*/", html_escape(title));
    std::set<std::string> numbers;
    for (auto &p : r.pages) numbers.insert(p.number);
    html = replace_all(html, "/*SUB*/", html_escape(fmt("Antiope, channel %s \xC2\xB7 %zu pages, %zu texts", r.channel.c_str(), numbers.size(), r.pages.size())));
    html = replace_all(html, "/*PAGES*/", js);
    write_text(path_join(dir, "index.html"), html);
    return a;
}
