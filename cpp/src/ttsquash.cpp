// "squash" of VHSTTX 2.7 (teletext/pipeline.py, teletext/subpage.py), see ttsquash.h
#include "ttsquash.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdio>
#include <cstdlib>

namespace {

const int SLOTS = 26 + 3 * 16;   // rows 0-25, then rows 26-28 by designation code
using Pkt = std::array<uint8_t, 42>;

// VHSTTX coding.py hamming8_dec: single errors corrected, an uncorrectable byte reads 15
const uint8_t H8[256] = {
    1, 15, 1, 1, 15, 0, 1, 15, 15, 2, 1, 15, 10, 15, 15, 7, 15, 0, 1, 15, 0, 0, 15, 0, 6, 15, 15, 11, 15, 0, 3, 15,
    15, 12, 1, 15, 4, 15, 15, 7, 6, 15, 15, 7, 15, 7, 7, 7, 6, 15, 15, 5, 15, 0, 13, 15, 6, 6, 6, 15, 6, 15, 15, 7,
    15, 2, 1, 15, 4, 15, 15, 9, 2, 2, 15, 2, 15, 2, 3, 15, 8, 15, 15, 5, 15, 0, 3, 15, 15, 2, 3, 15, 3, 15, 3, 3,
    4, 15, 15, 5, 4, 4, 4, 15, 15, 2, 15, 15, 4, 15, 15, 7, 15, 5, 5, 5, 4, 15, 15, 5, 6, 15, 15, 5, 15, 14, 3, 15,
    15, 12, 1, 15, 10, 15, 15, 9, 10, 15, 15, 11, 10, 10, 10, 15, 8, 15, 15, 11, 15, 0, 13, 15, 15, 11, 11, 11, 10, 15, 15, 11,
    12, 12, 15, 12, 15, 12, 13, 15, 15, 12, 15, 15, 10, 15, 15, 7, 15, 12, 13, 15, 13, 15, 13, 13, 6, 15, 15, 11, 15, 14, 13, 15,
    8, 15, 15, 9, 15, 9, 9, 9, 15, 2, 15, 15, 10, 15, 15, 9, 8, 8, 8, 15, 8, 15, 15, 9, 8, 15, 15, 11, 15, 14, 3, 15,
    15, 12, 15, 15, 4, 15, 15, 9, 15, 15, 15, 15, 15, 14, 15, 15, 8, 15, 15, 5, 15, 14, 13, 15, 15, 14, 15, 15, 14, 14, 15, 14,
};
int h8(uint8_t b) { return H8[b]; }
int slotOf(int row, int dc) { return row < 26 ? row : (row - 26) * 16 + 26 + dc; }

struct Subpage {
    std::array<Pkt, SLOTS> a{};
    std::array<bool, SLOTS> has{};
    int mag = 1, page = 0, sub = 0;
    // shown characters (rows 1-24, 7 bits) and the V1 thresholds (subpage.py / pipeline.py _v1_similarity_cache)
    mutable bool cached = false;
    mutable std::array<std::array<uint8_t, 40>, 24> disp;
    mutable std::array<double, 24> thr;
    mutable double thrSum = 0;
    const std::array<std::array<uint8_t, 40>, 24>& display() const
    {
        if (!cached) {
            for (int r = 0; r < 24; r++)
                for (int c = 0; c < 40; c++) disp[r][c] = a[r + 1][2 + c] & 0x7f;
            double sum = 0;
            for (int r = 0; r < 24; r++) {
                bool noDoubleOnPrev = true;
                if (r > 0) for (int c = 0; c < 40; c++) if (disp[r - 1][c] == 0x0d) { noDoubleOnPrev = false; break; }
                double t = 0;
                for (int c = 0; c < 40; c++) t += disp[r][c] != 0x20;
                t *= (t > 5 && noDoubleOnPrev) ? 1 : 0;
                t *= 0.5;
                thr[r] = t;
                sum += t;
            }
            thrSum = sum * 1.5;
            cached = true;
        }
        return disp;
    }
    void dirty() { cached = false; }
};

// the most common value of each column; a tie goes to the smallest (numpy unique + argmax)
template <typename T>
T modeOf(std::vector<T>& v)
{
    std::sort(v.begin(), v.end());
    T best = v[0];
    size_t bestN = 0;
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        while (j < v.size() && v[j] == v[i]) j++;
        if (j - i > bestN) { bestN = j - i; best = v[i]; }
        i = j;
    }
    return best;
}

Subpage squashList(const std::vector<const Subpage*>& g)
{
    Subpage s;
    s.mag = g[0]->mag; s.page = g[0]->page; s.sub = g[0]->sub;
    for (int k = 0; k < SLOTS; k++) {
        int n = 0;
        for (auto* p : g) n += p->has[k];
        // the row is kept when most copies have it (mode of the numbers; a tie leaves it out)
        if (n * 2 <= (int)g.size()) continue;
        std::vector<const Pkt*> ps;
        for (auto* p : g) if (p->has[k]) ps.push_back(&p->a[k]);
        Pkt out = *ps[0];
        int row = k < 26 ? k : 26 + (k - 26) / 16;
        if (row == 26 || row == 28) {   // triplets voted whole
            for (int t = 3; t + 2 < 42; t += 3) {
                std::vector<uint32_t> v;
                for (auto* p : ps) v.push_back((*p)[t] | (*p)[t + 1] << 8 | (uint32_t)(*p)[t + 2] << 16);
                uint32_t m = modeOf(v);
                out[t] = m & 0xff; out[t + 1] = (m >> 8) & 0xff; out[t + 2] = (m >> 16) & 0xff;
            }
        } else {
            for (int c = row == 27 ? 3 : 2; c < 42; c++) {
                std::vector<uint8_t> v;
                for (auto* p : ps) v.push_back((*p)[c]);
                out[c] = modeOf(v);
            }
        }
        s.a[k] = out;
        s.has[k] = true;
    }
    return s;
}
Subpage squashGroup(const std::vector<const Subpage*>& g) { return squashList(g); }

int diffV3(const Subpage& x, const Subpage& y)
{
    int d = 0;
    for (int k = 0; k < SLOTS; k++)
        if (x.has[k] && y.has[k])
            for (int c = 0; c < 42; c++) d += x.a[k][c] != y.a[k][c];
    return d;
}

bool matchesV1(const Subpage& s, const Subpage& o)
{
    auto& d = s.display();
    auto& od = o.display();
    double total = 0;
    for (int r = 0; r < 24; r++) {
        int m = 0;
        for (int c = 0; c < 40; c++) m += d[r][c] != 0x20 && d[r][c] == od[r][c];
        if (m < s.thr[r]) return false;
        total += m;
    }
    return total >= s.thrSum;
}

using Group = std::vector<const Subpage*>;
void bySize(std::vector<Group>& gs) { std::stable_sort(gs.begin(), gs.end(), [](const Group& a, const Group& b) { return a.size() > b.size(); }); }

std::vector<Group> groupV3(const std::vector<const Subpage*>& sps, int threshold)
{
    std::map<int, std::vector<Group>> bySub;   // insertion order of subcodes kept below
    std::vector<int> order;
    for (auto* sp : sps) {
        auto it = bySub.find(sp->sub);
        if (it == bySub.end()) { order.push_back(sp->sub); it = bySub.emplace(sp->sub, std::vector<Group>()).first; }
        bool put = false;
        for (auto& g : it->second)
            if (threshold == -1 || diffV3(*sp, *g[0]) < threshold) { g.push_back(sp); put = true; break; }
        if (!put) it->second.push_back({sp});
    }
    std::vector<Group> out;
    for (int s : order) for (auto& g : bySub[s]) out.push_back(g);
    bySize(out);
    return out;
}

std::vector<Group> groupV1(const std::vector<const Subpage*>& sps, int iterations, std::vector<Subpage>& store)
{
    std::vector<Group> groups;
    for (auto* sp : sps) {
        bool put = false;
        for (auto& g : groups) if (matchesV1(*sp, *g[0])) { g.push_back(sp); put = true; break; }
        if (!put) groups.push_back({sp});
    }
    bySize(groups);
    for (int it = 0; it < iterations; it++) {
        std::vector<Subpage> cent;
        for (auto& g : groups) cent.push_back(squashList(g));
        std::vector<Group> re(cent.size()), extras;
        for (auto* sp : sps) {
            bool put = false;
            for (size_t i = 0; i < cent.size(); i++) if (matchesV1(*sp, cent[i])) { re[i].push_back(sp); put = true; break; }
            if (!put) extras.push_back({sp});
        }
        groups.clear();
        for (auto& g : re) if (!g.empty()) groups.push_back(g);
        for (auto& g : extras) groups.push_back(g);
        bySize(groups);
    }
    (void)store;
    return groups;
}

// ---- custom (weighted) matching
double weightedSim(const uint8_t* l, const uint8_t* r, const double* w, int n)
{
    double tw = 0, mw = 0;
    for (int i = 0; i < n; i++) {
        bool act = l[i] != 0x20 || r[i] != 0x20;
        if (!act) continue;
        tw += w[i];
        if (l[i] == r[i]) mw += w[i];
    }
    return tw > 0 ? mw / tw : 0.0;
}
double rowWeight(int r, int rows, const SquashProfile& p)
{
    if (r == 0) return p.headerWeight;
    if (r >= rows - 2) return p.footerWeight;
    return p.bodyWeight;
}
double customScore(const Subpage& L, const Subpage& R, const SquashProfile& p)
{
    double hw[32];
    for (double& x : hw) x = p.headerWeight;
    double hs = weightedSim(&L.a[0][10], &R.a[0][10], hw, 32);
    auto& ld = L.display();
    auto& rd = R.display();
    double tw = 0, mw = 0;
    for (int r = 0; r < 24; r++) {
        double w = rowWeight(r, 24, p);
        for (int c = 0; c < 40; c++) {
            bool act = ld[r][c] != 0x20 || rd[r][c] != 0x20;
            if (!act) continue;
            tw += w;
            if (ld[r][c] == rd[r][c]) mw += w;
        }
    }
    double bs = tw > 0 ? mw / tw : 0.0;
    double tot = p.headerWeight + p.bodyWeight + p.footerWeight;
    if (tot <= 0) tot = 1;
    double score = (hs * p.headerWeight + bs * (p.bodyWeight + p.footerWeight)) / tot;
    score += L.sub == R.sub ? p.subcodeMatchBonus : -p.subcodeMismatchPenalty;
    return score;
}
std::vector<Group> groupCustom(const std::vector<const Subpage*>& sps, const SquashProfile& p)
{
    std::vector<Group> groups;
    std::vector<Subpage> cent;
    auto best = [&](const Subpage& s, double& sc) {
        int bi = -1;
        sc = -1e300;
        for (size_t i = 0; i < cent.size(); i++) {
            double v = customScore(s, cent[i], p);
            if (v > sc) { sc = v; bi = (int)i; }
        }
        return bi;
    };
    for (auto* sp : sps) {
        if (cent.empty()) { groups.push_back({sp}); cent.push_back(*sp); continue; }
        double sc;
        int bi = best(*sp, sc);
        if (bi >= 0 && sc >= p.matchThreshold) { groups[bi].push_back(sp); cent[bi] = squashList(groups[bi]); }
        else { groups.push_back({sp}); cent.push_back(*sp); }
    }
    for (int it = 0; it < p.iterations; it++) {
        cent.clear();
        for (auto& g : groups) cent.push_back(squashList(g));
        std::vector<Group> re(cent.size()), extras;
        for (auto* sp : sps) {
            double sc;
            int bi = best(*sp, sc);
            if (bi >= 0 && sc >= p.matchThreshold) re[bi].push_back(sp);
            else extras.push_back({sp});
        }
        groups.clear();
        for (auto& g : re) if (!g.empty()) groups.push_back(g);
        for (auto& g : extras) groups.push_back(g);
        bySize(groups);
    }
    return groups;
}

// ---- auto (pipeline.py _choose_auto_groups_for_page)
const double AUTO_W[24] = {0.5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.6, 0.6};
double groupQuality(const Group& g)
{
    if (g.empty()) return 0;
    double cons = 0, cov = 0;
    std::map<uint8_t, int> cnt;
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 40; c++) {
            bool act = false;
            cnt.clear();
            for (auto* s : g) { uint8_t v = s->display()[r][c]; act |= v != 0x20; cnt[v]++; }
            if (!act) continue;
            int mx = 0;
            for (auto& kv : cnt) mx = std::max(mx, kv.second);
            cons += AUTO_W[r] * mx / (double)g.size();
            cov += AUTO_W[r];
        }
    double support = 1.0 + 0.35 * std::min(std::log2((double)std::max<size_t>(g.size(), 1)), 3.0);
    return cons * support - cov * 0.75;
}
double groupingQuality(const std::vector<Group>& gs) { double s = 0; for (auto& g : gs) s += groupQuality(g); return s; }
std::vector<Group> eligible(const std::vector<Group>& gs, int minDup)
{
    std::vector<Group> o;
    for (auto& g : gs) if ((int)g.size() >= minDup) o.push_back(g);
    return o;
}
double simScore(const Subpage& L, const Subpage& R)
{
    auto& ld = L.display();
    auto& rd = R.display();
    double tw = 0, mw = 0;
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 40; c++) {
            bool act = ld[r][c] != 0x20 || rd[r][c] != 0x20;
            if (!act) continue;
            tw += AUTO_W[r];
            if (ld[r][c] == rd[r][c]) mw += AUTO_W[r];
        }
    return tw > 0 ? mw / tw : 0.0;
}
// row 0 = header (32 shown bytes), 1-24 = rows
bool rowDisplay(const Subpage& s, int row, std::vector<uint8_t>& out)
{
    if (row == 0) { out.assign(&s.a[0][10], &s.a[0][10] + 32); return true; }
    if (!s.has[row]) return false;
    out.assign(&s.a[row][2], &s.a[row][2] + 40);
    return true;
}
double rowQuality(const Group& g, int row)
{
    std::vector<std::vector<uint8_t>> arrs;
    std::vector<uint8_t> d;
    for (auto* s : g) if (rowDisplay(*s, row, d)) arrs.push_back(d);
    if (arrs.empty()) return 0;
    size_t w = arrs[0].size();
    double cons = 0;
    int active = 0;
    std::map<uint8_t, int> cnt;
    for (size_t c = 0; c < w; c++) {
        bool act = false;
        cnt.clear();
        for (auto& a : arrs) { act |= a[c] != 0x20; cnt[a[c]]++; }
        if (!act) continue;
        active++;
        int mx = 0;
        for (auto& kv : cnt) mx = std::max(mx, kv.second);
        cons += mx / (double)arrs.size();
    }
    if (!active) return 0;
    double coverage = active / (double)w;
    double support = 1.0 + 0.35 * std::min(std::log2((double)std::max<size_t>(g.size(), 1)), 3.0);
    return cons * (0.5 + coverage) * support;
}
double rowSim(const Subpage& a, const Subpage& b, int row)
{
    std::vector<uint8_t> x, y;
    if (!rowDisplay(a, row, x) || !rowDisplay(b, row, y)) return 0;
    int act = 0, m = 0;
    for (size_t c = 0; c < x.size(); c++) {
        if (x[c] == 0x20 && y[c] == 0x20) continue;
        act++;
        m += x[c] == y[c];
    }
    return act ? m / (double)act : 1.0;
}

struct AutoGroup { Group g, alt; double score = 0; };

std::vector<AutoGroup> groupAuto(const std::vector<const Subpage*>& sps, const SquashOptions& o, std::vector<Subpage>& store)
{
    auto v3 = groupV3(sps, o.threshold);
    auto v1 = groupV1(sps, o.v1Iterations, store);
    double s3 = groupingQuality(v3), s1 = groupingQuality(v1);
    bool e3 = !eligible(v3, o.minDuplicates).empty(), e1 = !eligible(v1, o.minDuplicates).empty();
    // the subcodes: one for all copies, or 0000/0001 for most of them, say little
    std::map<int, int> codes;
    for (auto* s : sps) codes[s->sub]++;
    int dom = 0, domCode = 0;
    for (auto& kv : codes) if (kv.second > dom) { dom = kv.second; domCode = kv.first; }
    double ratio = dom / (double)sps.size();
    bool suspicious = codes.size() == 1 || ((domCode == 0 || domCode == 1) && ratio >= 0.8) || (ratio >= 0.8 && codes.size() <= 2);
    bool healthy = !suspicious;
    bool preferV1;
    if (e3 && !e1) preferV1 = false;
    else if (e1 && !e3) preferV1 = true;
    else if (suspicious) preferV1 = s1 >= s3 * 0.98;
    else if (healthy) preferV1 = false;
    else preferV1 = s1 > s3;
    const auto& chosen = preferV1 ? v1 : v3;
    const auto& other = preferV1 ? v3 : v1;
    // refine: a group split the other way when that reads clearly better
    std::vector<Group> refined;
    for (auto& g : chosen) {
        if (g.size() < 2) { refined.push_back(g); continue; }
        double cur = groupQuality(g);
        std::vector<Group> alt = preferV1 ? groupV3(g, o.threshold) : groupV1(g, o.v1Iterations, store);
        if (alt.size() <= 1) { refined.push_back(g); continue; }
        if ((int)g.size() >= o.minDuplicates && eligible(alt, o.minDuplicates).empty()) { refined.push_back(g); continue; }
        if (groupingQuality(alt) > cur * 1.15) for (auto& a : alt) refined.push_back(a);
        else refined.push_back(g);
    }
    bySize(refined);
    // each group with the most similar group of the other grouping
    std::vector<AutoGroup> out;
    std::vector<Subpage> altSq;
    for (auto& a : other) altSq.push_back(squashList(a));
    for (auto& g : refined) {
        AutoGroup ag;
        ag.g = g;
        if (!other.empty()) {
            Subpage sq = squashList(g);
            int bi = -1;
            double bs = 0;
            for (size_t i = 0; i < other.size(); i++) {
                double s = simScore(sq, altSq[i]);
                if (s > bs) { bs = s; bi = (int)i; }
            }
            if (bi >= 0 && bs >= 0.20) { ag.alt = other[bi]; ag.score = bs; }
        }
        out.push_back(ag);
    }
    return out;
}

Subpage mergeAuto(const AutoGroup& ag)
{
    Subpage prim = squashList(ag.g);
    if (ag.alt.empty() || ag.score < 0.20) return prim;
    Subpage alt = squashList(ag.alt);
    if (simScore(prim, alt) < 0.18) return prim;
    for (int row = 0; row < 25; row++) {
        double ps = rowQuality(ag.g, row), as = rowQuality(ag.alt, row);
        if (as <= 0) continue;
        if (ps > 0 && rowSim(prim, alt, row) < (row == 0 ? 0.25 : 0.10)) continue;
        if (as > ps * 1.08 || (ps == 0 && as > 0)) {
            if (row == 0) memcpy(&prim.a[0][10], &alt.a[0][10], 32);
            else if (alt.has[row]) {
                if (!prim.has[row]) { prim.a[row][0] = alt.a[row][0]; prim.a[row][1] = alt.a[row][1]; prim.has[row] = true; }
                memcpy(&prim.a[row][2], &alt.a[row][2], 40);
            }
            prim.dirty();
        }
    }
    return prim;
}

} // namespace

bool squashProfileByName(const std::string& name, SquashProfile& p)
{
    p = SquashProfile();
    if (name == "balanced" || name == "default") return true;
    if (name == "aggressive") { p = {0.66, 0.40, 1.0, 0.25, 0.06, 0.01, 4}; return true; }
    if (name == "conservative") { p = {0.84, 0.72, 1.0, 0.60, 0.18, 0.09, 2}; return true; }
    if (name == "broken-subcodes") { p = {0.70, 0.50, 1.0, 0.35, 0.00, 0.00, 4}; return true; }
    return false;
}

bool squashOptionsByName(const std::string& name, SquashOptions& o)
{
    o = SquashOptions();
    if (name == "v1") { o.mode = SquashMode::V1; return true; }
    if (name == "v3") { o.mode = SquashMode::V3; return true; }
    if (name == "auto") { o.mode = SquashMode::Auto; return true; }
    if (name == "custom") { o.mode = SquashMode::Custom; return true; }
    o.mode = SquashMode::Custom;
    return squashProfileByName(name, o.profile);
}

std::vector<uint8_t> ttSquash(const std::vector<uint8_t>& t42, const SquashOptions& o, std::string& note)
{
    // paginate: per magazine, a header closes the page before it; a page needs a header and one more row
    std::vector<Subpage> subs;
    struct Buf { std::vector<Pkt> pk; };
    Buf mb[8];
    auto close = [&](Buf& b) {
        if (b.pk.size() > 1) {
            Subpage s;
            const Pkt& h = b.pk[0];
            s.mag = h8(h[0]) & 7;
            s.page = h8(h[2]) | h8(h[3]) << 4;
            int v0 = h8(h[4]) | h8(h[5]) << 4, v1 = h8(h[6]) | h8(h[7]) << 4;
            // VHSTTX reads (v0 & 0x7f) | ((v1 & 0x3f) << 8) in numpy uint8, where the shift loses the high part:
            // its pages are grouped by S1/S2 only, and so are they here
            (void)v1;
            s.sub = v0 & 0x7f;
            for (auto& p : b.pk) {
                int row = (h8(p[0]) >> 3) | (h8(p[1]) << 1);
                if (row >= 29) continue;
                int dc = row < 26 ? 0 : h8(p[2]);
                int k = slotOf(row, dc);
                s.a[k] = p;
                s.has[k] = true;
            }
            subs.push_back(s);
        }
        b.pk.clear();
    };
    for (size_t i = 0; i + 42 <= t42.size(); i += 42) {
        Pkt p;
        memcpy(p.data(), &t42[i], 42);
        bool pad = true;
        for (uint8_t v : p) if (v) { pad = false; break; }
        if (pad) continue;
        int a0 = h8(p[0]), mag = a0 & 7, row = (a0 >> 3) | (h8(p[1]) << 1);
        if (row == 0) {   // the page before in this magazine is complete
            if (!mb[mag].pk.empty()) {
                const Pkt& h = mb[mag].pk[0];
                if (((h8(h[0]) >> 3) | (h8(h[1]) << 1)) == 0) close(mb[mag]);
            }
            mb[mag].pk.clear();
        }
        mb[mag].pk.push_back(p);
    }
    for (auto& b : mb) {
        if (!b.pk.empty()) {
            int hr = (h8(b.pk[0][0]) >> 3) | (h8(b.pk[0][1]) << 1);
            if (hr == 0) close(b);
        }
    }
    // pages in the order they first came
    std::vector<std::pair<int, int>> order;
    std::map<std::pair<int, int>, std::vector<const Subpage*>> byPage;
    for (auto& s : subs) {
        auto k = std::make_pair(s.mag, s.page);
        if (!byPage.count(k)) order.push_back(k);
        byPage[k].push_back(&s);
    }
    std::vector<Subpage> store, outSubs;
    size_t groups = 0, few = 0;
    // a page none of whose groups has minDuplicates copies (a short recording, a page sent seldom) is not lost:
    // its largest group is taken (VHSTTX drops it)
    for (auto& k : order) {
        auto& sps = byPage[k];
        size_t before = outSubs.size();
        if (o.mode == SquashMode::Auto) {
            auto ags = groupAuto(sps, o, store);
            for (auto& ag : ags) {
                groups++;
                if ((int)ag.g.size() >= o.minDuplicates) outSubs.push_back(mergeAuto(ag));
            }
            if (outSubs.size() == before && !ags.empty()) {
                auto big = std::max_element(ags.begin(), ags.end(), [](auto& a, auto& b) { return a.g.size() < b.g.size(); });
                outSubs.push_back(mergeAuto(*big));
                few++;
            }
            continue;
        }
        std::vector<Group> gs = o.mode == SquashMode::V1 ? groupV1(sps, o.v1Iterations, store)
                              : o.mode == SquashMode::Custom ? groupCustom(sps, o.profile) : groupV3(sps, o.threshold);
        for (auto& g : gs) {
            groups++;
            if ((int)g.size() >= o.minDuplicates) outSubs.push_back(squashList(g));
        }
        if (outSubs.size() == before && !gs.empty()) {
            auto big = std::max_element(gs.begin(), gs.end(), [](auto& a, auto& b) { return a.size() < b.size(); });
            outSubs.push_back(squashList(*big));
            few++;
        }
    }
    std::vector<uint8_t> out;
    for (auto& s : outSubs)
        for (int k = 0; k < SLOTS; k++)
            if (s.has[k]) out.insert(out.end(), s.a[k].begin(), s.a[k].end());
    static const char* names[] = {"V1", "V3", "Auto", "Custom"};
    char b[400];
    snprintf(b, sizeof b, "squash %s: %zu copies of %zu pages in %zu groups -> %zu subpages (groups of %d copies or more; "
             "%zu pages heard fewer times taken from their largest group)",
             names[(int)o.mode], subs.size(), order.size(), groups, outSubs.size(), o.minDuplicates, few);
    note = b;
    return out;
}
