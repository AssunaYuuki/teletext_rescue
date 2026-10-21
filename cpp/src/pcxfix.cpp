// PCX pictures with bytes lost or wrong. The RLE stream is cut into plane lines (bytes-per-line bytes each): PC
// Paintbrush and the programs after it never let a run cross a line, so a line that does not decode to exactly its
// length is damaged. After the damage the next true line start is the earliest offset from which a long chain of
// lines decodes exactly (a false start - wrong line phase - gives a few lines only). How many lines a gap took is
// set by its bytes against the bytes a line around it, the plane (line number mod planes) by how well the lines
// after it fit the lines above them, and all gaps together take exactly the lines missing (DP over the gaps).
// Worked out on «Бизнес-Текст» TOWER350.PCX / PAGE350.PCX (tools/pcx/pcxfix2.py).
#include "pcxfix.h"
#include <algorithm>
#include <cstring>

namespace {
const int kChain = 8;     // lines that must follow a line for it to count as whole
const int kResync = 12;   // a new line start: at least this many lines from it
const int kLong = 60;     // ... and the earliest with this many is surely the true one
const int kFitLines = 16;
const double kFitWeight = 400;

struct Stream {
    const Bytes& d;
    long end;
    int L;
    std::vector<uint8_t> bad;   // byte lost
    bool guess;                 // the lost bytes are not known: 00 00 marks them

    // one plane line from p: where the next one starts, -1 = damaged
    long line(long p, uint8_t* out) const
    {
        int n = 0, prev = -1, pk = 0;
        while (n < L) {
            if (p >= end || bad[p]) return -1;
            int c = d[p++], k = 1, v = c;
            if (c >= 0xC0) {
                k = c & 0x3F;
                if (p >= end || bad[p]) return -1;
                v = d[p++];
            }
            if (k == 0 || n + k > L) return -1;
            // equal bytes are always one run (up to 63): a literal 00 after 00 is two lost bytes
            if (guess && v == 0 && prev == 0 && k == 1 && pk < 63) return -1;
            if (out) memset(out + n, v, k);
            n += k; prev = v; pk = k;
        }
        return p;
    }
    int chain(long p, int cap) const
    {
        int n = 0;
        while (n < cap) {
            long q = line(p, nullptr);
            if (q < 0) break;
            n++; p = q;
        }
        return n;
    }
    // a damaged line: the bytes that decoded before the damage
    int partial(long p, uint8_t* out) const
    {
        int n = 0, prev = -1, pk = 0;
        while (n < L && p < end && !bad[p]) {
            int c = d[p], k = 1, v = c;
            if (c >= 0xC0) {
                k = c & 0x3F;
                if (k == 0 || p + 1 >= end || bad[p + 1] || n + k > L) break;
                v = d[p + 1];
            }
            if (guess && v == 0 && prev == 0 && k == 1 && pk < 63) break;
            memset(out + n, v, k);
            n += k; p += c >= 0xC0 ? 2 : 1; prev = v; pk = k;
        }
        return n;
    }
};

struct Seg {
    long st = 0, en = 0;
    std::vector<std::vector<uint8_t>> ls;
    std::vector<int> sz;   // bytes each line took
};

double jaccard(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int i = 0, u = 0;
    for (size_t k = 0; k < a.size(); k++) { i += __builtin_popcount(a[k] & b[k]); u += __builtin_popcount(a[k] | b[k]); }
    return u ? (double)i / u : 0.5;
}

std::vector<Seg> segments(const Stream& s, int N)
{
    std::vector<Seg> segs;
    Seg cur;
    cur.st = 128;
    long p = 128;
    int total = 0;
    std::vector<uint8_t> buf(s.L);
    while (p < s.end && total + (int)cur.ls.size() < N) {
        int left = N - total - (int)cur.ls.size();
        long q = s.line(p, buf.data());
        if (q >= 0 && s.chain(p, std::min(kChain, left)) >= std::min(kChain, left)) {
            cur.ls.push_back(buf); cur.sz.push_back((int)(q - p)); p = q;
            continue;
        }
        cur.en = p;
        total += (int)cur.ls.size();
        segs.push_back(std::move(cur));
        cur = Seg();
        int cap = std::min(kLong, N - total), need = std::min(kResync, N - total), bn = 0;
        long first = -1, best = -1;
        for (long t = p + 1; t < std::min(s.end, p + 4000); t++) {
            if (s.bad[t] || (s.guess && s.d[t] == 0 && t + 1 < s.end && s.d[t + 1] == 0)) continue;
            if (first >= 0 && t > first + 300) break;
            int n = s.chain(t, cap);
            if (n >= cap) { best = t; break; }
            if (n >= need && n > 0) {
                if (first < 0) first = t;
                if (n > bn) { bn = n; best = t; }
            }
        }
        if (best < 0) { p = s.end; break; }
        cur.st = p = best;
    }
    cur.en = p;
    segs.push_back(std::move(cur));
    return segs;
}
}  // namespace

std::string PcxRepair::note() const
{
    std::string s = fmt("PCX %dx%d", width, height);
    if (repaired) s += fmt("; %d of %d lines whole, put together again after %d damaged place%s", linesGood, lines, gaps, gaps == 1 ? "" : "s");
    else if (linesGood < lines) s += fmt("; %d of %d lines whole", linesGood, lines);
    return s;
}

bool pcxDecode(const Bytes& d, const std::vector<long>& unread, Image& im, bool fill, PcxRepair* info)
{
    if (!isPcx(d)) return false;
    int bpp = d[3], planes = d[65], bpl = d[66] | d[67] << 8;
    int x0 = d[4] | d[5] << 8, y0 = d[6] | d[7] << 8, x1 = d[8] | d[9] << 8, y1 = d[10] | d[11] << 8;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || bpl <= 0 || planes < 1 || planes > 4) return false;
    if (!(bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8) || (bpp == 8 && planes != 1 && planes != 3)) return false;
    uint32_t pal[256];
    for (int i = 0; i < 16; i++) pal[i] = (uint32_t)d[16 + 3 * i] << 16 | d[17 + 3 * i] << 8 | d[18 + 3 * i];
    if (bpp == 1 && planes == 1) { pal[0] = 0; pal[1] = 0xFFFFFF; }
    bool vga = bpp == 8 && planes == 1 && d.size() >= 129 + 769 && d[d.size() - 769] == 0x0C;
    if (bpp == 8 && planes == 1) {
        if (!vga) for (int i = 0; i < 256; i++) pal[i] = i * 0x010101u;
        else for (int i = 0; i < 256; i++) { size_t o = d.size() - 768 + 3 * i; pal[i] = (uint32_t)d[o] << 16 | d[o + 1] << 8 | d[o + 2]; }
    }
    Stream s{d, (long)(vga ? d.size() - 769 : d.size()), bpl, std::vector<uint8_t>(d.size(), 0), unread.empty()};
    for (long u : unread) if (u >= 0 && u < (long)d.size()) s.bad[u] = 1;
    const int N = h * planes;

    // the plane lines and how much of each is known (bpl = whole)
    std::vector<std::vector<uint8_t>> L(N, std::vector<uint8_t>(bpl, 0));
    std::vector<int> known(N, 0);
    std::vector<Seg> segs = segments(s, N);
    int have = 0;
    for (auto& g : segs) have += (int)g.ls.size();
    int G = (int)segs.size() - 1, lost = N - have;
    bool repaired = G > 0 || lost > 0;
    if (repaired && have < N / 4) {
        // read less than a quarter: perhaps an encoder whose runs do cross lines - the plain decoding
        long at = 128;
        std::vector<uint8_t> line((size_t)bpl * planes);
        for (int y = 0; y < h; y++) {
            size_t n = 0;
            while (n < line.size()) {
                uint8_t c = at < s.end ? d[at++] : 0, v = c;
                int run = 1;
                if ((c & 0xC0) == 0xC0) { run = c & 0x3F; v = at < s.end ? d[at++] : 0; }
                while (run-- > 0 && n < line.size()) line[n++] = v;
            }
            for (int p = 0; p < planes; p++) {
                memcpy(L[y * planes + p].data(), line.data() + (size_t)p * bpl, bpl);
                known[y * planes + p] = bpl;
            }
        }
        repaired = false;
        have = N;
        segs.clear();
    } else {
        // lines each gap took: k >= 1, near its bytes / bytes a line around it; all together = lost
        std::vector<int> ks(std::max(G, 0), 1);
        if (G > 0) {
            const double NEG = -1e300;
            std::vector<std::vector<double>> sc(G + 1, std::vector<double>(lost + 1, NEG));
            std::vector<std::vector<int>> from(G + 1, std::vector<int>(lost + 1, -1));
            sc[0][0] = 0;
            int before = 0;   // lines of the segments up to a
            for (int g = 0; g < G; g++) {
                const Seg& a = segs[g];
                const Seg& b = segs[g + 1];
                before += (int)a.ls.size();
                long gb = b.st - a.en;
                double sum = 0;
                int cnt = 0;
                for (size_t i = a.sz.size() > 16 ? a.sz.size() - 16 : 0; i < a.sz.size(); i++) { sum += a.sz[i]; cnt++; }
                for (size_t i = 0; i < b.sz.size() && i < 16; i++) { sum += b.sz[i]; cnt++; }
                double avg = std::max(2.0, cnt ? sum / cnt : 20.0), ek = gb / avg;
                for (int used = 0; used <= lost; used++) {
                    if (sc[g][used] == NEG) continue;
                    int aend = before + used;
                    for (int k = 1; used + k <= lost && k <= ek * 2.5 + 8; k++) {
                        double f = 0;
                        int n = 0;
                        for (int t = 0; t < (int)b.ls.size() && t < kFitLines; t++) {
                            int j = aend + k + t;
                            int u = aend - 1 - (((aend - 1 - j) % planes) + planes) % planes;   // a's last line of that plane
                            int ui = (int)a.ls.size() - (aend - u);
                            if (ui >= 0) { f += jaccard(b.ls[t], a.ls[ui]); n++; }
                        }
                        double fit = n ? f / n : 0.5, sd = 1.0 + 0.3 * ek;
                        double v = sc[g][used] + kFitWeight * (fit - 0.5) * std::min(n, kFitLines) / kFitLines - (k - ek) * (k - ek) / (2 * sd * sd);
                        if (v > sc[g + 1][used + k]) { sc[g + 1][used + k] = v; from[g + 1][used + k] = k; }
                    }
                }
            }
            int bu = -1;
            for (int u = 0; u <= lost; u++) if (sc[G][u] != NEG && (bu < 0 || sc[G][u] > sc[G][bu])) bu = u;
            if (bu < 0) bu = 0;
            for (int g = G; g > 0 && bu >= 0; g--) {
                int k = from[g][bu];
                if (k < 1) { k = 1; }
                ks[g - 1] = k;
                bu -= k;
            }
        }
        int i = 0;
        for (int g = 0; g <= G; g++) {
            for (auto& l : segs[g].ls) {
                if (i < N) { L[i] = l; known[i] = bpl; }
                i++;
            }
            if (g < G) {
                if (i < N) known[i] = s.partial(segs[g].en, L[i].data());
                i += ks[g];
            }
        }
    }
    if (info) {
        info->width = w; info->height = h; info->lines = N; info->linesGood = std::min(have, N);
        info->gaps = std::max(0, (int)segs.size() - 1); info->repaired = repaired;
    }

    im = Image(w, h, 0);
    std::vector<uint8_t> line((size_t)bpl * planes), prev((size_t)bpl * planes, 0);
    for (int y = 0; y < h; y++) {
        int kmin = bpl;
        for (int p = 0; p < planes; p++) {
            int idx = y * planes + p, k = known[idx];
            uint8_t* o = line.data() + (size_t)p * bpl;
            memcpy(o, L[idx].data(), k);
            if (fill) memcpy(o + k, prev.data() + (size_t)p * bpl + k, bpl - k);
            else memset(o + k, 0, bpl - k);
            kmin = std::min(kmin, k);
        }
        prev = line;
        for (int x = 0; x < w; x++) {
            if (!fill && (long)x * bpp / 8 >= kmin) {   // not received: a dark checker
                im.set(x, y, ((x / 4 + y / 4) & 1) ? 0x800080 : 0x400040);
                continue;
            }
            uint32_t rgb;
            if (bpp == 8 && planes == 3) rgb = (uint32_t)line[x] << 16 | line[bpl + x] << 8 | line[2 * bpl + x];
            else if (bpp == 8) rgb = pal[line[x]];
            else {
                int idx = 0;
                for (int p = 0; p < planes; p++) {
                    int bit = x * bpp;
                    int v = (line[(size_t)p * bpl + bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1);
                    idx |= v << (p * bpp);
                }
                rgb = pal[idx & 255];
            }
            im.set(x, y, rgb);
        }
    }
    return true;
}
