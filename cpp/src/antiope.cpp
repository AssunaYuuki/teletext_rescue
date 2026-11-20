#include "antiope.h"
#include "mlse.h"
#include "fastdata_core.h"
#include "ui/util.h"
#include <algorithm>
#include <array>
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
struct Cell { std::string ch = " "; bool bad = false; };
using Grid = std::array<std::array<Cell, 40>, 25>;

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
        {{'a', 0x48}, "\xC3\xA4"}, {{'c', 0x4B}, "\xC3\xA7"}, {{'C', 0x4B}, "\xC3\x87"}, {{'n', 0x44}, "\xC3\xB1"}};
    auto it = t.find({c, a});
    if (it != t.end()) return it->second;
    std::string s(1, c);
    if (const char *d = accentOf(a)) s += d;
    return s;
}

Grid render(const std::vector<int> &g, size_t from)
{
    Grid grid;
    int r = 1, c = 0;
    bool mosaic = false;
    auto put = [&](const std::string &ch, bool bad) {
        if (r >= 0 && r < 25 && c >= 0 && c < 40) grid[r][c] = {ch, bad};
        c++;
        if (c >= 40) { c = 0; r++; }
    };
    for (size_t i = from; i < g.size(); i++) {
        if (g[i] < 0) continue;
        uint8_t raw = (uint8_t)g[i], b = raw & 0x7F;
        bool bad = !oddParity(raw);
        auto at = [&](size_t k) { return k < g.size() && g[k] >= 0 ? (g[k] & 0x7F) : -1; };
        if (b == 0x1F) {   // US row column
            int rr = at(i + 1) - 0x40, cc = at(i + 2) - 0x40;
            if (rr >= 0 && rr < 25 && cc > 0 && cc <= 40) { r = rr; c = cc - 1; }
            mosaic = false;
            i += 2;
            continue;
        }
        if (b == 0x1B) { i += 1; continue; }   // ESC x: an attribute (colour, size, flash ...)
        if (b == 0x0E) { mosaic = true; continue; }
        if (b == 0x0F) { mosaic = false; continue; }
        if (b == 0x19) {   // SS2 accent letter
            int acc = at(i + 1), let = at(i + 2);
            if (let >= 0x20 && let < 0x7F) put(accented((char)let, acc), bad);
            else put("?", true);
            i += 2;
            continue;
        }
        if (b == 0x12) {   // REP n: the last character n more times
            int n = at(i + 1) - 0x40;
            Cell prev = (c > 0 && r >= 0 && r < 25 && c <= 40) ? grid[r][c - 1] : Cell{};
            for (int k = 0; k < std::min(n, 40); k++) put(prev.ch, prev.bad);
            i += 1;
            continue;
        }
        if (b == 0x0D) { c = 0; continue; }
        if (b == 0x0A) { r++; continue; }
        if (b == 0x0C) { grid = Grid(); r = 1; c = 0; continue; }
        if (b >= 0x20 && b < 0x7F) put(mosaic && b != 0x20 ? std::string("\xE2\x96\x92") : std::string(1, (char)b), bad);
    }
    return grid;
}

std::vector<std::string> rowsOf(const Grid &g)
{
    std::vector<std::string> out(25);
    for (int r = 0; r < 25; r++) {
        std::string s;
        for (int c = 0; c < 40; c++) s += g[r][c].ch;
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
            const std::string &x = a[r][c].ch, &y = b[r][c].ch;
            if (x == " " && y == " ") continue;
            tot++;
            same += x == y;
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

Grid vote(const std::vector<Grid> &gs)
{
    Grid out;
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            std::map<std::string, double> w;
            for (auto &g : gs) w[g[r][c].ch] += g[r][c].bad ? 0.3 : 1;
            double best = -1;
            for (auto &kv : w) if (kv.second > best) { best = kv.second; out[r][c].ch = kv.first; }
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
    std::map<std::string, std::vector<std::vector<Grid>>> versions;   // page -> versions -> copies
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
        while (k < grp.size() && grp[k] >= 0 && mlse::hamExact((uint8_t)grp[k]) >= 0) {
            if (num.size() < 3) num += fmt("%X", mlse::hamExact((uint8_t)grp[k]));
            k++;
        }
        if (num.size() < 3) continue;
        Grid gr = render(grp, k);
        auto &vs = versions[num];
        bool placed = false;
        for (auto &v : vs)
            if (cellSim(v.front(), gr) >= 0.85) { v.push_back(gr); placed = true; break; }
        if (!placed) vs.push_back({gr});
    }
    // ---- vote, merge versions whose rows are alike, keep texts sent twice at least (or the page's commonest)
    for (auto &kv : versions) {
        std::vector<std::vector<Grid>> vs = kv.second;
        std::sort(vs.begin(), vs.end(), [](auto &a, auto &b) { return a.size() > b.size(); });
        struct M { std::vector<Grid> copies; std::vector<std::string> rows; };
        std::vector<M> merged;
        for (auto &v : vs) {
            auto r = rowsOf(vote(v));
            bool into = false;
            for (auto &m : merged)
                if (rowSim(r, m.rows) >= 0.8) {
                    m.copies.insert(m.copies.end(), v.begin(), v.end());
                    m.rows = rowsOf(vote(m.copies));
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
            AntiopePage p;
            p.number = kv.first;
            p.copies = (int)m.copies.size();
            p.rows = m.rows;
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
            if (s.size() >= 4 && s.size() <= 30 && s.find("\xE2\x96\x92") == std::string::npos) out.service = s;
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
    o.push_back("  pages as text: antiope/<page>.txt and antiope/all.txt");
    return o;
}

std::string antiopeWrite(const AntiopeResult &r, const std::string &dir)
{
    if (r.pages.empty()) return "";
    make_dirs(dir);
    std::map<std::string, int> count, seen;
    for (auto &p : r.pages) count[p.number]++;
    std::string all;
    for (auto &p : r.pages) {
        int k = ++seen[p.number];
        std::string name = p.number + (count[p.number] > 1 ? fmt("_%d", k) : std::string());
        std::string t;
        for (auto &row : p.rows) t += row + "\n";
        while (t.size() > 1 && t[t.size() - 1] == '\n' && t[t.size() - 2] == '\n') t.pop_back();
        write_text(path_join(dir, name + ".txt"), t);
        all += fmt("===== page %s (%d copies) =====\n", name.c_str(), p.copies) + t + "\n";
    }
    std::string a = path_join(dir, "all.txt");
    write_text(a, all);
    return a;
}
