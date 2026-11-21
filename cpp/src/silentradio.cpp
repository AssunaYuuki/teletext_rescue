// Silent Radio LED sign: packets, records, sign scripts and templates.
// Started from the Teletext Rescue program (silent_radio.cpp).
//
// What is known from the recordings (WTTW 1989, WNET 1988):
//   packet   FF seq(2) len(2, BE) 00 03 ck(2) C1 zone(2) + len+1 bytes; zone 02 03 = A,
//            04 05 = B, 06 07 = C. Every packet is sent 1-4 times, minutes apart.
//   body     4 bytes, then records: 00 0C len(2, LE) script | 00 0A + 3 | 00 <template> fields 1F
//   script   bytes >= 80: a 15-bit LED column (bit 0 = top) with the next byte; 80 before
//            text: a blank column;
//            text in the 5 x 7 font; 1D x / 0A x cursor (11 = back to the 1D place, 01 = to the
//            start of the text line); x is a column of a memory wider than the sign (up to 255),
//            the sign shows a window of it and pans to what is drawn past its edge;
//            0C next text line; 1C t(2, LE) hold t/256 s; 1F xx show the picture with
//            transition xx (some take one more byte; 2C, 3F, 39 show nothing); text after a
//            shown picture starts a new page; 05 + 6, 06 + 1, 07 + 1,
//            1B + 1 set-ups whose meaning is not known.
//   template 1B / 17 crawl, 2C / 2D two-line headline, 2B table head + first row, 10 the
//            next row, 30 / 31 score (teams 5 6, runs 7 8, league 2, inning or final 9).
#include "silentradio.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>


static const char *LED_FONT =
    "0000000000" "00005f0000" "0007000700" "147f147f14" "242a7f2a12" "2313086462" "3649562050" "0008070300"
    "001c224100" "0041221c00" "2a1c7f1c2a" "08083e0808" "0080703000" "0808080808" "0000606000" "2010080402"
    "3e5149453e" "00427f4000" "7249494946" "2141494d33" "1814127f10" "2745454539" "3c4a494931" "4121110907"
    "3649494936" "464949291e" "0000140000" "0040340000" "0008142241" "1414141414" "0041221408" "0201590906"
    "3e415d594e" "7c1211127c" "7f49494936" "3e41414122" "7f4141413e" "7f49494941" "7f09090901" "3e41415173"
    "7f0808087f" "00417f4100" "2040413f01" "7f08142241" "7f40404040" "7f021c027f" "7f0408107f" "3e4141413e"
    "7f09090906" "3e4151215e" "7f09192946" "2649494932" "03017f0103" "3f4040403f" "1f2040201f" "3f4038403f"
    "6314081463" "0304780403" "61594d4543" "007f414141" "0204081020" "004141417f" "0402010204" "4040404040"
    "0003070800" "2054547840" "7f28444438" "3844444428" "384444287f" "3854545418" "00087e0902" "18a4a49c78"
    "7f08040478" "00447d4000" "2040403d00" "7f10284400" "00417f4000" "7c0478047c" "7c08040478" "3844444438"
    "fc18242418" "18242418fc" "7c08040408" "4854545424" "04043f4424" "3c4040207c" "1c2040201c" "3c4030403c"
    "4428102844" "4c9090907c" "4464544c44" "0008364100" "0000770000" "0041360800" "0201020402";

// 8 rows: the 8th holds the descenders of g j p q y
static std::vector<int> glyph8(char32_t ch)
{
    int c = (int)ch;
    if (c < 32 || c >= 127) c = 32;
    if (c == 32) return {0, 0};
    std::vector<int> g;
    for (int k = 0; k < 5; k++) {
        unsigned v;
        sscanf(LED_FONT + ((c - 32) * 5 + k) * 2, "%2x", &v);
        g.push_back((int)v);
    }
    while (!g.empty() && g.front() == 0) g.erase(g.begin());
    while (!g.empty() && g.back() == 0) g.pop_back();
    return g;
}

std::vector<int> srGlyph(char32_t ch)
{
    auto g = glyph8(ch);
    for (auto &x : g) x &= 0x7F;
    return g;
}

std::vector<uint16_t> srTextColumns(const std::string &s)
{
    std::vector<uint16_t> cols;
    for (unsigned char ch : s) {
        for (int g : glyph8(ch)) cols.push_back((uint16_t)g);
        cols.push_back(0);
    }
    if (!cols.empty()) cols.pop_back();
    return cols;
}

const char *srFxName(int fx)
{
    static const char *n[] = {"cut", "wipe right", "wipe left", "wipe out", "wipe in", "slide left", "slide right",
                              "scroll up", "scroll down", "dissolve", "split", "pan"};
    return fx >= 0 && fx < SR_FX_COUNT ? n[fx] : "?";
}

// ------------------------------------------------------------- frames ---

double SrFrame::duration() const
{
    if (!scroll) return std::max(hold, fxTime);
    size_t n = 0;
    for (auto &l : lines) n = std::max(n, l.first.size());
    return (SR_W + n) / speed;
}

static unsigned pixHash(int x, int y)
{
    unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

// one transition, k = 0 .. 1 of the way from old to img
// (rolling up or down happens inside rows lo .. lo + rows - 1: one text line rolls in its own band)
static std::vector<uint16_t> transition(int fx, int fxArg, double k, const std::vector<uint16_t> &img,
                                        const std::vector<uint16_t> &old, int lo = 0, int rows = SR_H)
{
    const uint16_t M = 0x7FFF, B = (uint16_t)((1 << rows) - 1);
    std::vector<uint16_t> out(SR_W, 0);
    int s = (int)std::lround(k * SR_W), r = (int)std::lround(k * rows);
    for (int x = 0; x < SR_W; x++) {
        int d = std::abs(2 * x + 1 - SR_W);   // distance from the middle, in half columns
        switch (fx) {
        case SR_WIPE_RIGHT: out[x] = x < s ? img[x] : old[x]; break;
        case SR_WIPE_LEFT: out[x] = x >= SR_W - s ? img[x] : old[x]; break;
        case SR_WIPE_OUT: out[x] = d < s ? img[x] : old[x]; break;
        case SR_WIPE_IN: out[x] = d >= SR_W - s ? img[x] : old[x]; break;
        case SR_SLIDE_LEFT: out[x] = x < SR_W - s ? old[x + s] : img[x - (SR_W - s)]; break;
        case SR_SLIDE_RIGHT: out[x] = x >= s ? old[x - s] : img[x + SR_W - s]; break;
        case SR_SCROLL_UP: {
            uint16_t o = (old[x] >> lo) & B, n = (img[x] >> lo) & B;
            out[x] = (uint16_t)((((o >> r) | (n << (rows - r))) & B) << lo & M);
            break;
        }
        case SR_SCROLL_DOWN: {
            uint16_t o = (old[x] >> lo) & B, n = (img[x] >> lo) & B;
            out[x] = (uint16_t)((((o << r) | (n >> (rows - r))) & B) << lo & M);
            break;
        }
        case SR_DISSOLVE: {
            uint16_t v = 0;
            for (int y = 0; y < SR_H; y++) {
                bool nw = (pixHash(x, y) & 0xFFFF) < k * 65536.0;
                v |= (uint16_t)(((nw ? img[x] : old[x]) >> y & 1) << y);
            }
            out[x] = v;
            break;
        }
        case SR_SPLIT: {
            const uint16_t TOP = 0x00FF, BOT = M & ~TOP;
            uint16_t top = x < s ? img[x + SR_W - s] : old[x - s];
            uint16_t bot = x >= SR_W - s ? img[x - (SR_W - s)] : old[x + s];
            out[x] = (uint16_t)((top & TOP) | (bot & BOT));
            break;
        }
        case SR_PAN: {   // the window moves fxArg columns: the old picture leaves, the new comes in
            int d = fxArg, m = (int)std::lround(k * d);
            if (d > 0) out[x] = x + m < SR_W ? old[x + m] : img[x + m - d];
            else out[x] = x + m >= 0 ? old[x + m] : img[x + m - d];
            break;
        }
        default: out[x] = img[x];
        }
    }
    return out;
}

std::vector<uint16_t> SrFrame::at(double t, const std::vector<uint16_t> &prev) const
{
    const uint16_t M = 0x7FFF;
    std::vector<uint16_t> out(SR_W, 0);
    if (scroll) {
        int off = (int)(t * speed);
        for (auto &l : lines)
            for (int x = 0; x < SR_W; x++) {
                int k = x - SR_W + off;
                if (k >= 0 && k < (int)l.first.size()) out[x] |= (uint16_t)((l.first[k] << l.second) & M);
            }
        return out;
    }
    std::vector<uint16_t> img(SR_W, 0), old(SR_W, 0);
    for (int x = 0; x < SR_W && x < (int)cols.size(); x++) img[x] = cols[x] & M;
    for (int x = 0; x < SR_W && x < (int)prev.size(); x++) old[x] = prev[x] & M;
    if ((fx == SR_CUT && (fx2 < 0 || fx2 == SR_CUT)) || fxTime <= 0 || t >= fxTime) return img;
    double k = std::max(0.0, t / fxTime);
    bool lines2 = fx2 >= 0 && fx2 != fx;
    out = transition(fx, fxArg, k, img, old, 0, lines2 ? 8 : SR_H);
    if (lines2) {   // the bottom text line comes in its own way
        auto bot = transition(fx2, fxArg, k, img, old, 8, SR_H - 8);
        for (int x = 0; x < SR_W; x++) out[x] = (uint16_t)((out[x] & 0x00FF) | (bot[x] & 0x7F00));
    }
    return out;
}

namespace {

typedef std::vector<uint8_t> Bytes;

int effect(int code)
{
    switch (code) {
    case 0x00: case 0x20: return SR_CUT;
    case 0x21: return SR_WIPE_RIGHT;
    case 0x22: return SR_WIPE_LEFT;
    case 0x23: case 0x41: return SR_DISSOLVE;
    case 0x24: case 0x47: return SR_SLIDE_LEFT;
    case 0x25: case 0x46: return SR_SLIDE_RIGHT;
    case 0x26: return SR_SCROLL_UP;
    case 0x27: return SR_SCROLL_DOWN;
    case 0x28: return SR_SPLIT;
    case 0x29: case 0x2A: return SR_WIPE_OUT;
    case 0x2B: return SR_WIPE_IN;
    default: return 1 + (code * 7) % SR_SPLIT;
    }
}

double fxSeconds(int fx)
{
    switch (fx) {
    case SR_CUT: return 0;
    case SR_SCROLL_UP: case SR_SCROLL_DOWN: return 0.35;
    case SR_PAN: return 0.6;
    case SR_SLIDE_LEFT: case SR_SLIDE_RIGHT: case SR_SPLIT: return 0.7;
    case SR_DISSOLVE: return 0.6;
    default: return 0.45;
    }
}

SrFrame still(const std::vector<uint16_t> &cols, double hold, const std::string &what = "", int fx = SR_CUT)
{
    SrFrame f;
    f.cols = cols;
    f.hold = std::max(hold, 0.04);
    f.what = what;
    f.fx = fx;
    f.fxTime = fxSeconds(fx);
    return f;
}

int width(const std::string &s) { return (int)srTextColumns(s).size(); }

// The sign's memory while a script runs: LED columns written as bitmaps, and text
// runs, placed on one or two text lines when the picture is shown.
// The sign's memory is wider than the 112 LEDs (SPORTS puts its letters 32 columns apart up to
// column 173, SUMMER FILMS draws at 1D C0 = 192); the sign shows a window of it.
constexpr int SR_CANVAS = 256;

struct Screen {
    std::vector<uint16_t> cols = std::vector<uint16_t>(SR_CANVAS, 0);
    struct Run { int x, line; std::string s; };
    std::vector<Run> runs;
    int view = 0;                 // the first canvas column on the sign
    void put(int x, int col) { if (x >= 0 && x < SR_CANVAS) cols[x] = (uint16_t)(col & 0x7FFF); }
    // a picture drawn at [a, b) comes into the window
    void reveal(int a, int b)
    {
        if (b > view + SR_W) view = std::min(SR_CANVAS - SR_W, b - SR_W);
        if (a < view) view = std::max(0, a);
    }
    int text(int x, const std::string &s, int line)
    {
        int w = width(s);
        // text written over text on the same line replaces it
        runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const Run &r) {
            return r.line == line && r.x < x + w && x < r.x + width(r.s);
        }), runs.end());
        runs.push_back({x, line, s});
        return x + w + 1;
    }
    bool lineUsed(int line) const
    {
        for (auto &r : runs) if (r.line == line) return true;
        return false;
    }
    std::vector<uint16_t> compose() const
    {
        std::vector<uint16_t> out(cols.begin() + view, cols.begin() + view + SR_W);
        bool two = lineUsed(1);
        // the sign keeps text inside: text that runs off the right edge moves left, all
        // its lines together so they stay lined up
        int over = 0, left = SR_W;
        for (auto &r : runs) {
            int w = width(r.s);
            if (!w || r.s.find_first_not_of(' ') == std::string::npos) continue;
            over = std::max(over, r.x - view + w - SR_W);
            left = std::min(left, r.x - view);
        }
        int shift = std::min(std::max(over, 0), std::max(left, 0));
        for (int line = 0; line < 2; line++) {
            // one line: the middle of the sign; two: rows 0-7 and 8-14
            int row = two ? line * 8 : 4;
            for (auto &r : runs) {
                if (r.line != line) continue;
                auto c = srTextColumns(r.s);
                int w = (int)c.size(), x0 = r.x - shift - view;
                if (x0 + w > SR_W) x0 = std::max(0, SR_W - w);
                for (int k = 0; k < w; k++)
                    if (x0 + k >= 0 && x0 + k < SR_W) out[x0 + k] = (uint16_t)((out[x0 + k] | (c[k] << row)) & 0x7FFF);
            }
        }
        return out;
    }
    // the words on the sign; pieces of one word (a letter placed apart) are joined
    std::string words() const
    {
        std::string w;
        for (int line = 0; line < 2; line++) {
            std::vector<const Run *> rs;
            for (auto &r : runs) if (r.line == line) rs.push_back(&r);
            std::stable_sort(rs.begin(), rs.end(), [](const Run *a, const Run *b) { return a->x < b->x; });
            int end = -100;
            bool lineStart = true;
            for (auto *r : rs) {
                size_t a = r->s.find_first_not_of(' '), b = r->s.find_last_not_of(' ');
                if (a == std::string::npos) continue;
                bool glued = !lineStart && a == 0 && r->x <= end + 2;   // a space is 3 columns
                if (!w.empty() && !glued) w += ' ';
                w += r->s.substr(a, b - a + 1);
                end = r->x + width(r->s);
                lineStart = false;
            }
        }
        return w;
    }
};

// ------------------------------------------------------------ packets ---
struct Packet {
    int seq = 0;
    size_t pos = 0;
    std::string zone;
    int copies = 0, intact = 0;
    Bytes body;
};

struct Stream {
    const Bytes &data;
    const std::vector<uint8_t> *valid;
    const std::vector<float> *soft;
    bool ok(size_t i) const { return !valid || (i < valid->size() && (*valid)[i]); }
};

std::vector<Packet> packets(const Stream &S, std::vector<std::pair<size_t, size_t>> &ranges)
{
    const Bytes &data = S.data;
    struct Raw { size_t at, n; std::string key; int lost; };
    std::vector<Raw> raw;
    for (size_t p = 0; p + 13 <= data.size(); p++) {
        // the fixed bytes of the header; a lost line may hide one of them
        static const int fixedAt[4] = {0, 5, 6, 9};
        static const uint8_t fixedIs[4] = {0xFF, 0x00, 0x03, 0xC1};
        int seen = 0, bad = 0;
        for (int k = 0; k < 4; k++) {
            if (!S.ok(p + fixedAt[k])) continue;
            seen++;
            bad += data[p + fixedAt[k]] != fixedIs[k];
        }
        if (bad || seen < 3) continue;
        bool keyOk = true;
        for (int k : {1, 2, 3, 4, 10, 11}) keyOk = keyOk && S.ok(p + k);
        if (!keyOk) continue;
        size_t L = (data[p + 3] << 8) | data[p + 4];
        if (L < 8 || L > 4096 || p + 13 + L > data.size()) continue;
        if (S.ok(p + 12) && data[p + 12] < 0x80) continue;
        int lost = 0;
        for (size_t i = p; i < p + 13 + L; i++) lost += !S.ok(i);
        std::string key((const char *)&data[p + 1], 4);
        key += std::string((const char *)&data[p + 10], 2);
        raw.push_back({p, 13 + L, key, lost});
        ranges.push_back({p, p + 13 + L});
        p += 12; // matches do not overlap
    }
    // copies of one packet: same sequence number, length and zone
    std::map<std::string, std::vector<const Raw *>> groups;
    std::vector<std::string> order;
    for (auto &r : raw) {
        if (!groups.count(r.key)) order.push_back(r.key);
        groups[r.key].push_back(&r);
    }
    // a sequence number read with a wrong length forms a group of its own: the bigger group wins
    std::map<std::string, std::string> bestOf;   // seq + zone -> key
    for (auto &key : order) {
        std::string sz = key.substr(0, 2) + key.substr(4, 2);
        auto it = bestOf.find(sz);
        if (it == bestOf.end() || groups[key].size() > groups[it->second].size()) bestOf[sz] = key;
    }
    // The same item is sent again later under a new sequence number: those copies help too.
    auto similar = [&](const Raw *x, const Raw *y) {
        if (x->n != y->n) return false;
        size_t lim = std::min(x->n, (size_t)400);
        int same = 0, seen = 0;
        for (size_t i = 16; i < lim; i++)
            if (S.ok(x->at + i) && S.ok(y->at + i)) { seen++; same += data[x->at + i] == data[y->at + i]; }
        return seen >= 32 && same * 10 >= seen * 9;
    };
    std::vector<Packet> out;
    for (auto &key : order) {
        if (bestOf[key.substr(0, 2) + key.substr(4, 2)] != key) continue;
        const auto &own = groups[key];
        std::vector<const Raw *> all = own;
        for (auto &k2 : order) {
            if (k2 == key || k2.substr(2, 4) != key.substr(2, 4) || bestOf[k2.substr(0, 2) + k2.substr(4, 2)] != k2) continue;
            if (similar(own[0], groups[k2][0])) all.insert(all.end(), groups[k2].begin(), groups[k2].end());
        }
        size_t n = all[0]->n;
        // A clean copy ends in six zero bytes. One that does not has lost a piece of tape inside
        // (the recording paused or dropped out): what follows the gap sits in the wrong place.
        std::vector<const Raw *> cps;
        for (auto *c : all) {
            bool z = n >= 6;
            for (size_t i = n - 6; z && i < n; i++) z = !S.ok(c->at + i) || data[c->at + i] == 0;
            if (z) cps.push_back(c);
        }
        if (cps.empty()) cps = all;
        if (cps.size() == 2) {   // two that disagree: the one with a piece missing jumps ahead
            const Raw *a = cps[0], *b = cps[1];
            auto same = [&](size_t i) { return !S.ok(a->at + i) || !S.ok(b->at + i) || data[a->at + i] == data[b->at + i]; };
            size_t d = 12;
            while (d + 8 < n) {
                int diff = 0;
                for (size_t k = 0; k < 8; k++) diff += !same(d + k);
                if (diff >= 4) break;
                d++;
            }
            // where a's bytes after the split turn up later in b, a lost them (and the other way)
            auto skips = [&](const Raw *x, const Raw *y) {
                const size_t w = 12;
                // a window of x after the split that is not a run of one LED column over and over
                size_t from = d + 2;
                for (; from + w <= n && from < d + 200; from++) {
                    std::set<uint8_t> v;
                    bool ok = true;
                    for (size_t k = 0; k < w; k++) { v.insert(data[x->at + from + k]); ok = ok && S.ok(x->at + from + k); }
                    if (ok && v.size() >= 7) break;
                }
                if (from + w > n || from >= d + 200) return false;
                for (size_t q = from + 1; q + w <= n + 64 && y->at + q + w <= data.size(); q++) {
                    size_t k = 0;
                    while (k < w && data[y->at + q + k] == data[x->at + from + k]) k++;
                    if (k == w) return true;
                }
                return false;
            };
            if (d + 8 < n) {
                bool aLost = skips(a, b), bLost = skips(b, a);
                if (aLost != bLost) cps = {aLost ? b : a};
                else {
                    // no telling which: mixing the two bit by bit gives neither, so one is kept whole
                    size_t diff = 0, seen = 0;
                    for (size_t i = d; i < n; i++)
                        if (S.ok(a->at + i) && S.ok(b->at + i)) { seen++; diff += data[a->at + i] != data[b->at + i]; }
                    if (seen && diff * 4 > seen) cps = {b->at > a->at ? b : a};
                }
            }
        }
        if (cps.size() >= 3) {   // and one that disagrees with the others byte for byte
            std::vector<const Raw *> agree;
            for (auto *c : cps) {
                int same = 0, seen = 0;
                for (size_t i = 0; i < n; i++) {
                    if (!S.ok(c->at + i)) continue;
                    int m = 0, o = 0;
                    for (auto *d : cps)
                        if (d != c && S.ok(d->at + i)) { o++; m += data[d->at + i] == data[c->at + i]; }
                    if (o) { seen++; same += m * 2 >= o; }
                }
                if (!seen || same * 10 >= seen * 8) agree.push_back(c);
            }
            if (!agree.empty()) cps = agree;
        }
        Bytes voted(n);
        for (size_t i = 0; i < n; i++) {
            if (S.soft && S.soft->size() >= 8 * data.size()) {
                // bit by bit: the soft values of every copy add up (a lost line counts for little)
                int b = 0;
                for (int k = 0; k < 8; k++) {
                    float sum = 0;
                    for (auto *c : cps) sum += (*S.soft)[(c->at + i) * 8 + k];
                    b = (b << 1) | (sum > 0);
                }
                voted[i] = (uint8_t)b;
            } else {
                int cnt[256] = {0}, any = 0;
                for (auto *c : cps)
                    if (S.ok(c->at + i)) { cnt[data[c->at + i]]++; any++; }
                if (!any) for (auto *c : cps) cnt[data[c->at + i]]++;
                int b = 0;
                for (int v = 1; v < 256; v++) if (cnt[v] > cnt[b]) b = v;
                voted[i] = (uint8_t)b;
            }
        }
        Packet p;
        const uint8_t *k = (const uint8_t *)key.data();   // seq(2) len(2) zone(2), as read
        p.seq = (k[0] << 8) | k[1];
        p.pos = own[0]->at;
        if (k[4] == 2 && k[5] == 3) p.zone = "A";
        else if (k[4] == 4 && k[5] == 5) p.zone = "B";
        else if (k[4] == 6 && k[5] == 7) p.zone = "C";
        else { char b[8]; snprintf(b, sizeof b, "%02X%02X", k[4], k[5]); p.zone = b; }
        p.copies = (int)own.size();
        for (auto *c : own) p.intact += c->lost == 0;
        // where no copy was read (the recording ends or drops out), the packet ends
        size_t end = n - 2;
        for (size_t i = 16, run = 0; i < n - 2; i++) {
            bool any = false;
            for (auto *c : cps) any = any || S.ok(c->at + i);
            run = any ? 0 : run + 1;
            if (run >= 4) { end = i + 1 - run; break; }
        }
        if (end > 18) p.body = Bytes(voted.begin() + 16, voted.begin() + end);
        out.push_back(std::move(p));
    }
    return out;
}

// records in a packet body: 00 0C len(2, LE) script | 00 0A .. control |
// 00 <template> fields (02..09 = field number, text) 1F
struct Record { int kind = 0; Bytes script; int t = 0; std::map<int, std::string> fields; };

std::vector<Record> records(const Bytes &body)
{
    std::vector<Record> out;
    size_t i = 0, n = body.size();
    while (i < n) {
        if (body[i] != 0) { i++; continue; }
        if (i + 1 >= n) break;
        int t = body[i + 1];
        if (t == 0x0C && i + 3 < n) {
            size_t L = body[i + 2] | (body[i + 3] << 8);
            size_t e = std::min(n, i + 4 + L);
            Record r; r.kind = 0; r.script.assign(body.begin() + i + 4, body.begin() + e);
            out.push_back(std::move(r));
            i += 4 + L;
            continue;
        }
        if (t == 0x0A) { Record r; r.kind = 2; out.push_back(r); i += 5; continue; }
        if (t == 0) { i++; continue; }
        size_t j = i + 2;
        Record r; r.kind = 1; r.t = t;
        int cur = -1; std::string buf;
        while (j < n && body[j] != 0x1F) {
            int c = body[j];
            if (c >= 2 && c <= 9) { if (cur >= 0) r.fields[cur] = buf; cur = c; buf.clear(); }
            else if (c >= 32 && c < 127) buf += (char)c;
            j++;
        }
        if (cur >= 0) r.fields[cur] = buf;
        out.push_back(std::move(r));
        i = j + 1;
    }
    return out;
}

// A sign script: text, raw columns (bytes >= 80: 15-bit column), cursor
// moves, text lines, holds (1C, 1/256 s), clear / show commands (1F xx).
std::vector<SrFrame> runScript(const Bytes &s, std::vector<std::string> &texts)
{
    const double HOLD = 2.0;
    std::vector<SrFrame> out;
    std::vector<bool> timed;
    Screen sc;
    int x = 0, saved = 0, last = -1, line = 0, lineX = 0;
    bool dirty = false, lineHasText = false, shown = false;
    // a sprite drawn over a picture already shown is a step of an animation: it goes where the
    // picture has the same sprite (MOVIE REVIEW: the reel at 17 turns, the script says 1D 2A)
    bool animKnown = false, animStep = false;
    int animOff = 0;
    size_t i = 0, n = s.size();
    int shownView = 0;
    bool lastTwoLines = false, bottomSet = false;
    auto show = [&](int code) {
        lastTwoLines = sc.lineUsed(1);
        bottomSet = false;
        int fx = animStep ? SR_CUT : effect(code);   // a step of an animation just changes
        animStep = false;
        int pan = sc.view - shownView;
        out.push_back(still(sc.compose(), 0, sc.words(), fx));
        if (shown && pan) {   // the window moves on to what was drawn past its edge
            out.back().fx = SR_PAN;
            out.back().fxArg = pan;
            out.back().fxTime = std::min(1.5, std::abs(pan) / 48.0);
        }
        shownView = sc.view;
        timed.push_back(false);
        last = (int)out.size() - 1;
        dirty = false;
        shown = true;
        std::string w = sc.words();
        // text that builds up letter by letter is one text
        if (!w.empty() && !texts.empty() && w.compare(0, texts.back().size(), texts.back()) == 0) texts.back() = w;
        else if (!w.empty()) texts.push_back(w);
    };
    while (i < n) {
        int c = s[i];
        if (c == 0x80) {   // between words: one blank column ("THE 80 80 VERDICT", "80 MEN'S")
            size_t j = i;
            while (j < n && s[j] == 0x80) j++;
            auto text = [&](size_t k) { return k < n && s[k] >= 0x20 && s[k] < 0x7F; };
            if (text(j) && (j + 1 >= n || text(j + 1) || s[j + 1] < 0x20)) { x += (int)(j - i); i = j; continue; }
        }
        if (c >= 0x80 && i + 1 < n) {
            std::vector<uint16_t> run;
            size_t j = i;
            while (j + 1 < n && s[j] >= 0x80) { run.push_back((uint16_t)(((s[j] & 0x7F) << 8) | s[j + 1])); j += 2; }
            int at = x;
            if (shown && run.size() < 64) {
                if (!animKnown) {
                    animKnown = true;
                    animOff = 0;
                    auto bits = [](uint16_t v) { int b = 0; for (; v; v &= v - 1) b++; return b; };
                    int lit = 0, w = (int)run.size();
                    for (auto v : run) lit += bits(v);
                    // where the script puts it, it would cover another picture or text: it belongs
                    // over the one it is a step of (the same sprite, or one much like it)
                    bool clash = false;
                    for (int k = 0; k < w && !clash; k++) {
                        int p = x + k;
                        clash = p >= 0 && p < SR_CANVAS && sc.cols[p] && sc.cols[p] != run[k];
                    }
                    for (auto &r : sc.runs)
                        if (r.s.find_first_not_of(' ') != std::string::npos && r.x < x + w && x < r.x + width(r.s)) clash = true;
                    if (lit >= 6 && clash) {
                        int best = -1;
                        double bestScore = 0;
                        for (int p = 0; p + w <= SR_CANVAS; p++) {
                            int both = 0, there = 0;
                            for (int k = 0; k < w; k++) { both += bits(sc.cols[p + k] & run[k]); there += bits(sc.cols[p + k]); }
                            double score = 2.0 * both / (lit + there);   // Dice: 1 = the same picture
                            if (score > bestScore + 1e-9 || (score > bestScore - 1e-9 && best >= 0 && std::abs(p - x) < std::abs(best - x))) {
                                bestScore = score;
                                best = p;
                            }
                        }
                        if (best >= 0 && bestScore >= 0.6) animOff = best - x;
                    }
                }
                at = x + animOff;
                animStep = true;
            }
            for (size_t k = 0; k < run.size(); k++) sc.put(at + (int)k, run[k]);
            int lo = at, hi = at + (int)run.size();   // the lit part of it has to be on the sign
            while (lo < hi && !run[lo - at]) lo++;
            while (hi > lo && !run[hi - 1 - at]) hi--;
            if (lo < hi) sc.reveal(lo, hi);
            x += (int)run.size();
            i = j;
            dirty = true;
            continue;
        }
        if (c >= 32 && c < 127) {
            std::string t;
            while (i < n && s[i] >= 32 && s[i] < 127) t += (char)s[i++];
            // text after a picture was shown starts a new page; pictures drawn after one
            // are steps of an animation over it
            if (shown && t.find_first_not_of(' ') != std::string::npos) {
                sc = Screen(); line = 0; lineHasText = false; shown = false; animKnown = false; animOff = 0; shownView = 0;
            }
            if (!lineHasText) lineX = x;
            x = sc.text(x, t, line);
            lineHasText = true;
            dirty = true;
            continue;
        }
        if (c == 0x1C && i + 2 < n) {
            double hold = (s[i + 1] | (s[i + 2] << 8)) / 256.0;
            if (dirty || last < 0) show(0x20);
            out[last].hold = (timed[last] ? out[last].hold : 0) + hold;
            timed[last] = true;
            i += 3;
            continue;
        }
        if ((c == 0x1D || c == 0x0A) && i + 1 < n) {
            x = s[i + 1];   // a canvas column, can be past the sign's edge
            if (c == 0x1D) saved = x;
            i += 2;
            continue;
        }
        if (c == 0x11) { x = saved; i++; continue; }
        if (c == 0x01) {   // back to the start of the text line (SUMMER FILMS is written twice in one place)
            if (lineHasText) x = lineX;
            i++;
            continue;
        }
        if (c == 0x0C) {   // next text line, under the start of this one
            if (lineHasText && line == 0) { line = 1; x = lineX; lineHasText = false; }
            i++;
            continue;
        }
        if (c == 0x1F && i + 1 < n) {
            int a = s[i + 1];
            i += 2;
            // 1F 4x carries the effect in the next byte (1F 47 28, 1F 46 27 as 1F 28, 1F 27);
            // 1F 24 one more byte too
            if ((a == 0x24 || (a >= 0x40 && a <= 0x4F)) && i < n && s[i] >= 0x20 && s[i] < 0x40) {
                if (a >= 0x40) a = s[i];
                i++;
            }
            // a second effect right after a two-line picture: how its bottom line comes in
            // (1F 2B 01 1F 2A, 1F 47 28 01 1F 46 27)
            if (!dirty && !bottomSet && lastTwoLines && last >= 0 && a >= 0x20 && a < 0x80 && a != 0x2C && a != 0x3F && a != 0x39) {
                int f2 = effect(a);
                SrFrame &L = out[last];
                if (f2 != L.fx && L.fx != SR_PAN) {
                    L.fx2 = f2;
                    L.fxTime = std::max(L.fxTime, fxSeconds(f2));
                }
                bottomSet = true;
                continue;
            }
            if (a == 0x2C) {}   // seen before text and before each step of an animation; not a clear
            else if (a == 0x32 || a == 0x38) { x = sc.text(x, a == 0x32 ? "12" : "00", line); dirty = true; }
            else if (a == 0x33) { x = sc.text(x + 2, "PM", line); dirty = true; }
            else if (a != 0x3F && a != 0x39 && a < 0x80 && dirty) show(a);   // 1F 82: a set-up, as 06 is
            continue;
        }
        if (c == 0x05) { i += 7; continue; }
        if (c == 0x06 || c == 0x07 || c == 0x1B) { i += 2; continue; }
        i++;
    }
    if (dirty) show(0x20);
    for (size_t k = 0; k < out.size(); k++) {
        if (!timed[k]) out[k].hold = HOLD;
        out[k].hold = std::max(out[k].hold, 0.04);
    }
    return out;
}

SrFrame crawl(const std::string &s)
{
    SrFrame f;
    f.scroll = true;
    f.lines.push_back({srTextColumns(s), 4});
    f.what = s;
    return f;
}

// one line in the middle of the sign or two lines, centred; too wide: they scroll
SrFrame twoLines(const std::string &l1, const std::string *l2, double hold, int fx)
{
    std::vector<std::pair<std::string, int>> rows;
    if (l2) { rows.push_back({l1, 0}); rows.push_back({*l2, 8}); }
    else rows.push_back({l1, 4});
    size_t wide = 0;
    for (auto &r : rows) wide = std::max(wide, srTextColumns(r.first).size());
    std::string w;
    for (auto &r : rows) w += (w.empty() ? "" : " / ") + r.first;
    if (wide > SR_W) {
        SrFrame f;
        f.scroll = true;
        for (auto &r : rows) f.lines.push_back({srTextColumns(r.first), r.second});
        f.what = w;
        return f;
    }
    std::vector<uint16_t> cols(SR_W, 0);
    for (auto &r : rows) {
        auto c = srTextColumns(r.first);
        int x0 = (SR_W - (int)c.size()) / 2;
        for (size_t k = 0; k < c.size(); k++)
            if (x0 + (int)k >= 0 && x0 + (int)k < SR_W) cols[x0 + k] = (uint16_t)((cols[x0 + k] | (c[k] << r.second)) & 0x7FFF);
    }
    return still(cols, hold, w, fx);
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// templates: 1B / 17 = crawl, 30/31 = sports score (teams, scores, league,
// status), 2B = table head and its first row, 10 after it = the next row,
// anything else = one or two centred lines
struct TemplateState { std::string league, tableHead; };

SrFrame templateFrame(int t, std::map<int, std::string> &f, TemplateState &st)
{
    auto get = [&](int k) { auto it = f.find(k); return it == f.end() ? std::string() : trim(it->second); };
    if (t == 0x1B || t == 0x17) return crawl("   " + get(2));
    if (t == 0x30 || t == 0x31) {
        std::string lg = !get(2).empty() ? get(2) : !get(3).empty() ? get(3) : !get(4).empty() ? get(4) : st.league;
        std::string l1 = get(5) + " " + get(7) + "  " + get(6) + " " + get(8), l2 = lg + "  " + get(9);
        st.league = lg;
        return twoLines(trim(l1), &l2, 3.0, SR_SCROLL_UP);
    }
    if (t == 0x2B) {
        st.tableHead = get(2);
        std::string l2 = get(3);
        return twoLines(st.tableHead, &l2, 3.0, SR_WIPE_RIGHT);
    }
    if (t == 0x10 && !st.tableHead.empty()) {   // table rows go by under the head
        std::string l2 = get(2);
        return twoLines(st.tableHead, &l2, 2.5, SR_CUT);
    }
    std::string l1 = get(2);
    if (f.count(3) && !get(3).empty()) {
        std::string l2 = get(3);
        return twoLines(l1, &l2, 3.5, SR_SPLIT);
    }
    if (width(l1) > SR_W) return crawl("   " + l1);
    return twoLines(l1, nullptr, 3.0, SR_WIPE_OUT);
}

// Without packets: printable runs between the binary parts.
void looseText(const Bytes &bytes, const std::vector<int64_t> &frames, std::vector<SilentRadioMessage> &out)
{
    std::string cur;
    int64_t start = 0;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
        if (a != std::string::npos) {
            std::string s = cur.substr(a, b - a + 1);
            int letters = 0;
            for (char c : s) letters += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (s.size() >= 6 && letters * 2 >= (int)s.size() && (out.empty() || out.back().text != s)) {
                SilentRadioMessage m;
                m.frame = start;
                m.text = s;
                m.texts.push_back(s);
                m.frames.push_back(width(s) > SR_W ? crawl("   " + s) : twoLines(s, nullptr, 3.0, SR_WIPE_RIGHT));
                out.push_back(std::move(m));
            }
        }
        cur.clear();
    };
    for (size_t i = 0; i < bytes.size(); i++) {
        uint8_t c = bytes[i];
        if (c >= 0x20 && c < 0x7F) {
            if (cur.empty()) start = frames.empty() ? 0 : frames[std::min(i / 2, frames.size() - 1)];
            cur += (char)c;
        } else if ((c == 0x0D || c == 0x0A) && !cur.empty()) {
            cur += ' ';
        } else {
            flush();
        }
    }
    flush();
}

} // namespace

void srDecode(const std::vector<uint8_t> &bytes, const std::vector<int64_t> &frames,
              std::vector<SilentRadioMessage> &out, const std::vector<uint8_t> *valid, const std::vector<float> *soft)
{
    size_t before = out.size();
    std::vector<std::pair<size_t, size_t>> ranges;
    Stream S{bytes, valid && valid->size() == bytes.size() ? valid : nullptr, soft};
    for (auto &p : packets(S, ranges)) {
        if (p.body.size() < 4) continue;
        SilentRadioMessage m;
        TemplateState st;
        for (auto &r : records(p.body)) {
            if (r.kind == 0) {
                auto f = runScript(r.script, m.texts);
                m.frames.insert(m.frames.end(), f.begin(), f.end());
            } else if (r.kind == 1) {
                m.frames.push_back(templateFrame(r.t, r.fields, st));
                std::string t;
                for (auto &kv : r.fields) {
                    std::string v = trim(kv.second);
                    if (!v.empty()) t += (t.empty() ? "" : " / ") + v;
                }
                if (!t.empty()) m.texts.push_back(t);
            }
        }
        if (m.frames.empty()) continue;
        // drop crumbs: stray bytes that happen to be printable
        m.texts.erase(std::remove_if(m.texts.begin(), m.texts.end(), [](const std::string &t) {
            int an = 0;
            for (char c : t) an += isalnum((unsigned char)c) != 0;
            return an < 2;
        }), m.texts.end());
        for (auto &t : m.texts) m.text += (m.text.empty() ? "" : " \xC2\xB7 ") + t;
        if (m.text.empty()) m.text = "(picture)";
        m.zone = p.zone;
        m.seq = p.seq;
        m.copies = p.copies;
        m.intact = p.intact;
        m.frame = frames.empty() ? 0 : frames[std::min(p.pos / 2, frames.size() - 1)];
        out.push_back(std::move(m));
    }
    // text outside the packets that were found (packets with a damaged header)
    Bytes rest = bytes;
    for (auto &r : ranges)
        for (size_t i = r.first; i < r.second && i < rest.size(); i++) rest[i] = 0;
    if (S.valid)
        for (size_t i = 0; i < rest.size(); i++) if (!(*S.valid)[i]) rest[i] = 0;
    size_t packed = out.size();
    looseText(rest, frames, out);
    // a damaged copy of a packet that was also received whole adds nothing; its text may
    // have a wrong letter or two, so it goes if most of its words are in the packets
    std::set<std::string> known;
    auto words = [](const std::string &t) {
        std::vector<std::string> w;
        std::string cur;
        for (char c : t + " ") {
            if (isalnum((unsigned char)c)) cur += c;
            else { if (cur.size() >= 3) w.push_back(cur); cur.clear(); }
        }
        return w;
    };
    std::string all;   // the letters and digits of every packet text, to find a piece of one in
    auto alnum = [](const std::string &t) {
        std::string r;
        for (char c : t) if (isalnum((unsigned char)c)) r += (char)toupper((unsigned char)c);
        return r;
    };
    for (size_t k = before; k < packed; k++) {
        for (auto &w : words(out[k].text)) known.insert(w);
        all += alnum(out[k].text) + "|";
    }
    out.erase(std::remove_if(out.begin() + packed, out.end(), [&](const SilentRadioMessage &m) {
        for (size_t k = before; k < packed; k++)
            if (out[k].text.find(m.text) != std::string::npos) return true;
        // a piece of a script ("1ABASEBALL" = set-up bytes 31 41 + BASEBALL, ",Men's Gol")
        std::string a = alnum(m.text);
        for (size_t k = 0; k <= 2 && a.size() >= k + 6; k++)
            if (all.find(a.substr(k)) != std::string::npos) return true;
        auto w = words(m.text);
        int in = 0;
        for (auto &x : w) in += known.count(x) != 0;
        return !w.empty() && in * 10 >= (int)w.size() * 8;
    }), out.end());
    std::stable_sort(out.begin() + before, out.end(),
                     [](const SilentRadioMessage &a, const SilentRadioMessage &b) { return a.frame < b.frame; });
}
