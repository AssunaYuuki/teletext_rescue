// Japanese teletext receiver for 27 MHz 8-bit captures, after JTES Decoder 0.9.50 (receiver.py, receiver_hauppauge.py,
// core.py). The tries go to the Slicer in batches (jtes_core.h on the processor or the GPU); here only what to try.
#include "jtesrx.h"
#include "fastdata.h"
#include "slicer.h"
#include "vbifile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

const int NB = 296;   // bits from the run-in on: 24 run-in + 272 codeword
const uint8_t PREFIX_BYTES[3] = {0x55, 0x55, 0xA7};

struct Channel { float v[16]; };   // per half bit: taps for bits t-3 .. t+3, then the level

// the GV-USB2 VHS profile of JTES Decoder (assets/channel.npy), at contrast 57
const Channel H0 = {{9.025328f, -1.514898f, 45.114968f, 56.682316f, -0.310481f, 1.306311f, 4.502061f, 59.036782f,
                     12.366063f, -1.197880f, 15.267705f, 64.466057f, 25.454458f, -6.108481f, 5.087201f, 58.889719f}};
const double CONTRAST = 57, EDGE = 39, START = 40;
const float SPB[3] = {33.0f / 7, 4.707f, 4.722f};
// Hauppauge USB-Live2 profiles (assets/hauppauge-channel.npy, -alternate.npy)
const Channel HP[2] = {
    {{7.955529f, 9.617691f, 36.959860f, 51.798310f, 15.607051f, -1.533104f, -1.540260f, 59.229450f,
      3.728114f, 7.742926f, 20.008630f, 51.734189f, 35.890313f, 2.344923f, -2.100612f, 59.022542f}},
    {{2.265790f, 7.053507f, 37.746359f, 52.214586f, 17.429258f, -4.245292f, -1.272491f, 61.406048f,
      0.912502f, 3.190100f, 19.880322f, 52.308733f, 38.331404f, 1.512404f, -3.005025f, 61.346215f}}};
const int HSERV[6] = {0x92, 0x46, 0x37, 0x2a, 0xfe, 0x8f};

uint8_t prefixBit(int i) { return (PREFIX_BYTES[i / 8] >> (i % 8)) & 1; }

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    double hi = v[m];
    if (v.size() % 2) return hi;
    double lo = *std::max_element(v.begin(), v.begin() + m);
    return (lo + hi) / 2;
}
double medianOf(const uint8_t* a, int from, int to) { return median(std::vector<double>(a + from, a + to)); }
double stdOf(const uint8_t* a, int from, int to)
{
    double s = 0, q = 0;
    int n = to - from;
    if (n <= 0) return 0;
    for (int i = from; i < to; i++) s += a[i];
    s /= n;
    for (int i = from; i < to; i++) q += (a[i] - s) * (a[i] - s);
    return std::sqrt(q / n);
}

struct Edge { double lo = 0, hi = 0; int e = 0; };
// levels of the line and where the run-in starts (receiver.py edge())
bool edge(const uint8_t* a, int n, Edge& out)
{
    if (n < 100) return false;
    double lo = medianOf(a, 0, 8), hi = medianOf(a, 45, 75);
    int first = -1;
    for (int i = 5; i < 100; i++) if (a[i] > (lo + hi) / 2) { first = i; break; }
    bool early = hi - lo >= 15 && first >= 0;
    if (early && first < 25) { out = {lo, hi, first}; return true; }
    double olo = medianOf(a, 0, 25), ohi = medianOf(a, 60, 95);
    int of = -1;
    for (int i = 20; i < 65; i++) if (a[i] > (olo + ohi) / 2) { of = i; break; }
    if (ohi - olo >= 15 && of >= 0) { out = {olo, ohi, of}; return true; }
    if (early) { out = {lo, hi, first}; return true; }
    return false;
}
Channel scaled(const Channel& base, const Edge& e, double contrast = CONTRAST)
{
    Channel h = base;
    for (int p = 0; p < 2; p++) {
        for (int k = 0; k < 7; k++) h.v[p * 8 + k] = (float)(base.v[p * 8 + k] * (e.hi - e.lo) / contrast);
        h.v[p * 8 + 7] = (float)e.lo;
    }
    return h;
}
double startOf(const Edge& e, double start = START, double edge = EDGE) { return start + e.e - edge; }
JtesChannel bundledChannel()
{
    JtesChannel c;
    memcpy(c.h, H0.v, sizeof c.h);
    return c;
}

float at(const uint8_t* a, int n, double t)
{
    t = std::max(0.0, std::min((double)(n - 1), t));
    int j = (int)t;
    double q = t - j;
    return (float)(a[j] * (1 - q) + a[std::min(j + 1, n - 1)] * q);
}

// least squares fit of the channel to a known bit sequence (core.py fitbits)
bool fitbits(const uint8_t* a, int n, const uint8_t* bits, double st, double spb, Channel& h)
{
    for (int p = 0; p < 2; p++) {
        double M[8][9] = {{0}};
        for (int t = 3; t < 293; t++) {
            double y = at(a, n, st + (2 * t + p + .5) * spb / 2);
            double x[8];
            for (int k = 0; k < 7; k++) x[k] = bits[t - 3 + k];
            x[7] = 1;
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 8; j++) M[i][j] += x[i] * x[j];
                M[i][8] += x[i] * y;
            }
        }
        for (int i = 0; i < 8; i++) M[i][i] += 1e-9;
        for (int c = 0; c < 8; c++) {
            int piv = c;
            for (int r = c + 1; r < 8; r++) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
            if (std::fabs(M[piv][c]) < 1e-12) return false;
            if (piv != c) for (int j = 0; j < 9; j++) std::swap(M[c][j], M[piv][j]);
            for (int r = 0; r < 8; r++) {
                if (r == c) continue;
                double f = M[r][c] / M[c][c];
                for (int j = c; j < 9; j++) M[r][j] -= f * M[c][j];
            }
        }
        for (int i = 0; i < 8; i++) {
            double v = M[i][8] / M[i][i];
            if (!std::isfinite(v)) return false;
            h.v[p * 8 + i] = (float)v;
        }
    }
    return true;
}

void unpack(const uint8_t* packed, int nbits, uint8_t* bits)
{
    for (int k = 0; k < nbits; k++) bits[k] = (packed[k / 8] >> (k % 8)) & 1;
}
// the run-in and a packet's 272 bits
void withPrefix(const uint8_t* word34, uint8_t* bits)
{
    for (int i = 0; i < 24; i++) bits[i] = prefixBit(i);
    unpack(word34, 272, bits + 24);
}

// the idle packet: service 8F, everything else zero, with its check bits (packed)
const uint8_t* idleWord()
{
    static uint8_t w[34];
    static bool done = false;
    if (!done) {
        static const int e[] = {82, 77, 76, 71, 67, 66, 56, 52, 48, 40, 36, 34, 24, 22, 18, 10, 4, 0};
        uint8_t g[83] = {0};
        for (int x : e) g[x] = 1;
        uint8_t bits[272] = {0};
        for (int k = 0; k < 8; k++) bits[k] = (0x8f >> k) & 1;
        uint8_t r[82] = {0};   // r[i] = coefficient of x^i
        for (int i = 0; i < 190; i++) {
            int f = r[81] ^ bits[i];
            for (int k = 81; k > 0; k--) r[k] = r[k - 1];
            r[0] = 0;
            if (f) for (int k = 0; k < 82; k++) r[k] ^= g[k];
        }
        for (int k = 0; k < 82; k++) bits[190 + k] = r[81 - k];
        for (int k = 0; k < 272; k++) w[k / 8] |= bits[k] << (k % 8);
        done = true;
    }
    return w;
}
bool isIdle(const JtesOut& o) { return memcmp(o.word, idleWord(), 34) == 0; }
bool knownService(int si) { return si == 0x92 || si == 0x46 || si == 0x37 || si == 0x2a || si == 0xfe || si == 0x8f; }
bool valid(const JtesOut& o) { return o.ec >= 0 && knownService(o.word[0]); }

// run-in + service byte predicted by a channel: 6 services x 26 bits x 2 halves
std::vector<float> prefixPred(const Channel& h)
{
    std::vector<float> pp(312);
    for (int s = 0; s < 6; s++) {
        uint8_t b[32];
        for (int i = 0; i < 24; i++) b[i] = prefixBit(i);
        for (int k = 0; k < 8; k++) b[24 + k] = (HSERV[s] >> k) & 1;
        for (int i = 0; i < 26; i++)
            for (int ph = 0; ph < 2; ph++) {
                float v = h.v[ph * 8 + 7];
                for (int k = 0; k < 7; k++) v += h.v[ph * 8 + k] * b[i + k];
                pp[s * 52 + i * 2 + ph] = v;
            }
    }
    return pp;
}

JtesJob job(int line, const Channel& h, double st, double spb)
{
    JtesJob j;
    j.line = line; j.st = (float)st; j.spb = (float)spb;
    memcpy(j.h, h.v, sizeof j.h);
    return j;
}

struct Attempt { float cost; float st, spb; uint8_t bits[NB]; };
Attempt attemptOf(const JtesJob& j, const JtesOut& o)
{
    Attempt a;
    a.cost = o.cost; a.st = j.st; a.spb = j.spb;
    unpack(o.bits, NB, a.bits);
    return a;
}

// the lines' state while they are read
struct Row {
    int line = 0;         // index in the lines given
    bool found = false, idle = false;
    JtesRxPacket p;
    std::vector<Attempt> att;
};

void take(Row& r, const JtesJob& j, const JtesOut& o, bool refined)
{
    r.found = true;
    r.p.row = r.line;
    r.p.cost = o.cost; r.p.start = j.st; r.p.spb = j.spb; r.p.errors = o.ec;
    memcpy(r.p.word, o.word, 34);
    r.p.refined = refined;
}

// decision-directed retries: the channel fitted to each of the best attempts (up to `top` a line), iterated twice
bool retryByFit(const uint8_t* lines, int n, std::vector<Row>& R, const std::vector<int>& todo, int top, Slicer& sl,
                std::string& err, bool refined)
{
    struct Cur { int row; Attempt a; bool live; };
    std::vector<Cur> cur;
    for (int ri : todo) {
        Row& r = R[ri];
        if (r.found || r.idle || r.att.empty()) continue;
        std::sort(r.att.begin(), r.att.end(), [](const Attempt& x, const Attempt& y) { return x.cost < y.cost; });
        for (int k = 0; k < top && k < (int)r.att.size(); k++) cur.push_back({ri, r.att[k], true});
    }
    for (int it = 0; it < 2 && !cur.empty(); it++) {
        std::vector<JtesJob> jobs;
        std::vector<int> of;
        for (size_t c = 0; c < cur.size(); c++) {
            if (!cur[c].live || R[cur[c].row].found) continue;
            const uint8_t* a = lines + (size_t)R[cur[c].row].line * n;
            Channel hh;
            if (!fitbits(a, n, cur[c].a.bits, cur[c].a.st, cur[c].a.spb, hh)) { cur[c].live = false; continue; }
            jobs.push_back(job(R[cur[c].row].line, hh, cur[c].a.st, cur[c].a.spb));
            of.push_back((int)c);
        }
        if (jobs.empty()) break;
        std::vector<JtesOut> outs;
        if (!sl.jtesDecode(jobs, outs, err)) return false;
        for (size_t k = 0; k < jobs.size(); k++) {
            Cur& c = cur[of[k]];
            Row& r = R[c.row];
            if (r.found) continue;
            if (valid(outs[k])) {
                if (isIdle(outs[k])) r.idle = true;
                else take(r, jobs[k], outs[k], refined);
                continue;
            }
            Attempt na = attemptOf(jobs[k], outs[k]);
            if (!memcmp(na.bits, c.a.bits, NB)) c.live = false;   // a fixed point: the next fit is the same
            c.a = na;
        }
    }
    return true;
}

} // namespace

bool jtesReceive(const uint8_t* lines, int64_t rows, int n, int lpf, const std::vector<int>& slots, bool hauppauge, bool quick,
                 Slicer& sl, std::vector<JtesRxPacket>& out, std::string& err, JtesRxStats* stats, const JtesChannel* chan)
{
    out.clear();
    JtesRxStats st;
    const JtesChannel C = chan ? *chan : bundledChannel();
    Channel HC;
    memcpy(HC.v, C.h, sizeof HC.v);
    std::vector<char> useSlot(lpf, 0);
    for (int s : slots) if (s >= 0 && s < lpf) useSlot[s] = 1;
    if (!sl.jtesLines(lines, rows, n, err)) return false;
    std::vector<Row> R;
    std::vector<int> rowOf((size_t)rows, -1);
    for (int64_t l = 0; l < rows; l++)
        if (useSlot[l % lpf]) { rowOf[l] = (int)R.size(); R.push_back(Row()); R.back().line = (int)l; }
    st.scanned = (int64_t)R.size();
    auto line = [&](const Row& r) { return lines + (size_t)r.line * n; };

    // ---- first reading
    if (hauppauge) {
        std::vector<int> act;
        for (int i = 0; i < (int)R.size(); i++)
            if (n >= 1350 && stdOf(line(R[i]), 100, 1350) >= 8) act.push_back(i);
        for (int p = 0; p < 2; p++) {
            std::vector<int> todo, ln;
            for (int i : act) if (!R[i].found && !R[i].idle) { todo.push_back(i); ln.push_back(R[i].line); }
            if (todo.empty()) break;
            std::vector<JtesCand> cand;
            auto pred = prefixPred(HP[p]);
            if (!sl.jtesCand(ln, pred.data(), cand, err)) return false;
            std::vector<JtesJob> jobs;
            std::vector<int> of;
            for (size_t k = 0; k < todo.size(); k++) {
                R[todo[k]].att.clear();
                if (cand[k].fit > 0.8f) continue;   // no run-in like this anywhere on the line (packets fit under 0.4)
                for (int c = 0; c < cand[k].n; c++) { jobs.push_back(job(R[todo[k]].line, HP[p], cand[k].st[c], cand[k].spb[c])); of.push_back(todo[k]); }
            }
            std::vector<JtesOut> outs;
            if (!sl.jtesDecode(jobs, outs, err)) return false;
            for (size_t k = 0; k < jobs.size(); k++) {   // the first codeword in candidate order
                Row& r = R[of[k]];
                if (r.found || r.idle) continue;
                if (valid(outs[k])) { if (isIdle(outs[k])) r.idle = true; else take(r, jobs[k], outs[k], false); }
                else r.att.push_back(attemptOf(jobs[k], outs[k]));
            }
            if (!retryByFit(lines, n, R, todo, 2, sl, err, false)) return false;
        }
    } else {
        std::vector<JtesJob> jobs;
        std::vector<int> of;
        static const double DS[5] = {0, -.75, .75, -1.5, 1.5};
        for (int i = 0; i < (int)R.size(); i++) {
            const uint8_t* a = line(R[i]);
            Edge e;
            if (!edge(a, n, e)) continue;
            bool flat = stdOf(a, 250, std::min(n, 1000)) < 7;
            if (flat && stdOf(a, 1000, std::min(n, 1400)) < 7) continue;
            Channel h = scaled(HC, e, C.contrast);
            for (float spb : SPB)
                for (double ds : DS) { jobs.push_back(job(R[i].line, h, startOf(e, C.start, C.edge) + ds, spb)); of.push_back(i); }
        }
        std::vector<JtesOut> outs;
        if (!sl.jtesDecode(jobs, outs, err)) return false;
        std::vector<int> best(R.size(), -1);
        for (size_t k = 0; k < jobs.size(); k++) {
            int i = of[k];
            if (!valid(outs[k])) continue;
            if (isIdle(outs[k])) { R[i].idle = true; continue; }
            if (best[i] < 0 || outs[k].cost < outs[best[i]].cost) best[i] = (int)k;
        }
        for (int i = 0; i < (int)R.size(); i++)
            if (best[i] >= 0 && !R[i].idle) take(R[i], jobs[best[i]], outs[best[i]], false);
    }
    for (auto& r : R) { st.found += r.found; st.idle += r.idle; }

    if (!quick) {
        // ---- a second channel learned from confident packets
        std::vector<int> anchors;
        for (int i = 0; i < (int)R.size(); i++)
            if (R[i].found && R[i].p.errors <= 4 && (R[i].p.word[0] == 0x92 || R[i].p.word[0] == 0x46)) anchors.push_back(i);
        std::vector<Channel> learned;
        int want = (int)std::min<size_t>(anchors.size(), 256);
        for (int k = 0; k < want; k++) {
            const Row& r = R[anchors[want == 1 ? 0 : (size_t)std::llround((double)k * (anchors.size() - 1) / (want - 1))]];
            Edge e;
            if (!edge(line(r), n, e)) continue;
            uint8_t bits[NB];
            withPrefix(r.p.word, bits);
            Channel hh;
            if (!fitbits(line(r), n, bits, r.p.start, r.p.spb, hh)) continue;
            for (int p = 0; p < 2; p++) {
                for (int q = 0; q < 7; q++) hh.v[p * 8 + q] = (float)(hh.v[p * 8 + q] * CONTRAST / (e.hi - e.lo));
                hh.v[p * 8 + 7] = 0;
            }
            learned.push_back(hh);
        }
        st.anchors = (int)learned.size();
        bool haveLearned = learned.size() >= 8;
        Channel learnedH{};
        if (haveLearned)
            for (int k = 0; k < 16; k++) {
                std::vector<double> v;
                for (auto& c : learned) v.push_back(c.v[k]);
                learnedH.v[k] = (float)median(v);
            }

        // ---- refine the lines the first reading missed (receiver.py refine)
        static const double DS[5] = {0, -1, 1, -2, 2};
        std::vector<int> todo;
        std::vector<Edge> edges(R.size());
        std::vector<char> hasEdge(R.size(), 0);
        for (int i = 0; i < (int)R.size(); i++) {
            if (R[i].found || R[i].idle) continue;
            if (!edge(line(R[i]), n, edges[i])) continue;
            hasEdge[i] = 1;
            R[i].att.clear();
            todo.push_back(i);
        }
        int64_t before = 0;
        for (auto& r : R) before += r.found;
        for (int pass = 0; pass < 2; pass++) {   // the bundled channel, then the learned one
            if (pass == 1 && !haveLearned) break;
            std::vector<JtesJob> jobs;
            std::vector<int> of;
            for (int i : todo) {
                if (R[i].found || R[i].idle) continue;
                Channel h = pass ? scaled(learnedH, edges[i]) : scaled(HC, edges[i], C.contrast);
                for (float spb : SPB)
                    for (double ds : DS) { jobs.push_back(job(R[i].line, h, startOf(edges[i], C.start, C.edge) + ds, spb)); of.push_back(i); }
            }
            std::vector<JtesOut> outs;
            if (!sl.jtesDecode(jobs, outs, err)) return false;
            std::vector<int> best(R.size(), -1);
            for (size_t k = 0; k < jobs.size(); k++) {
                int i = of[k];
                R[i].att.push_back(attemptOf(jobs[k], outs[k]));
                if (!valid(outs[k])) continue;
                if (isIdle(outs[k])) { if (!pass) R[i].idle = true; continue; }
                if (best[i] < 0 || outs[k].cost < outs[best[i]].cost) best[i] = (int)k;
            }
            for (int i : todo)
                if (best[i] >= 0 && !R[i].idle && !R[i].found) take(R[i], jobs[best[i]], outs[best[i]], true);
        }
        if (!retryByFit(lines, n, R, todo, 3, sl, err, true)) return false;
        int64_t after = 0;
        for (auto& r : R) after += r.found;
        st.refined = after - before;

        // ---- a single missing packet between two good ones of the same stream: wide timing search
        std::vector<int> gaps;
        for (int i = 0; i < (int)R.size(); i++) {
            if (R[i].found) continue;
            int64_t l = R[i].line;
            if (l - lpf < 0 || l + lpf >= rows) continue;
            int a = rowOf[l - lpf], b = rowOf[l + lpf];
            if (a < 0 || b < 0 || !R[a].found || !R[b].found) continue;
            int sa = R[a].p.word[0], sb = R[b].p.word[0];
            if (sa == sb && sa != 0x8f && ((((R[b].p.word[1] & 15) - (R[a].p.word[1] & 15)) & 15) == 2)) gaps.push_back(i);
        }
        std::vector<JtesJob> jobs;
        std::vector<int> of;
        for (int i : gaps) {
            int64_t l = R[i].line;
            const Row& A = R[rowOf[l - lpf]];
            const Row& B = R[rowOf[l + lpf]];
            std::vector<Channel> prof;
            double st0 = 0, spb0 = 0;
            if (hauppauge) {
                prof.push_back(HP[0]);
                st0 = (A.p.start + B.p.start) / 2;
                spb0 = (A.p.spb + B.p.spb) / 2;
            } else {
                if (!hasEdge[i]) continue;
                prof.push_back(scaled(HC, edges[i], C.contrast));
                st0 = startOf(edges[i], C.start, C.edge);
            }
            for (const Row* nb : {&A, &B}) {
                uint8_t bits[NB];
                withPrefix(nb->p.word, bits);
                Channel hh;
                if (fitbits(line(*nb), n, bits, nb->p.start, nb->p.spb, hh)) prof.push_back(hh);
            }
            std::vector<std::pair<double, double>> grid;
            if (hauppauge) {
                for (double spb = spb0 - .01; spb < spb0 + .01 + 1e-9; spb += .005)
                    for (double s = st0 - 2; s < st0 + 2 + 1e-9; s += .5) grid.push_back({s, spb});
                std::stable_sort(grid.begin(), grid.end(), [&](const std::pair<double, double>& x, const std::pair<double, double>& y) {
                    return std::fabs(x.first - st0) + 100 * std::fabs(x.second - spb0) < std::fabs(y.first - st0) + 100 * std::fabs(y.second - spb0);
                });
            } else {
                for (double spb = 4.67; spb < 4.76 - 1e-9; spb += .005)
                    for (double s = st0 - 4; s < st0 + 4 - 1e-9; s += .5) grid.push_back({s, spb});
            }
            for (auto& h : prof)
                for (auto& g : grid) { jobs.push_back(job(R[i].line, h, g.first, g.second)); of.push_back(i); }
        }
        std::vector<JtesOut> outs;
        if (!sl.jtesDecode(jobs, outs, err)) return false;
        std::vector<int> best(R.size(), -1);
        for (size_t k = 0; k < jobs.size(); k++) {
            int i = of[k];
            int64_t l = R[i].line;
            if (!valid(outs[k]) || outs[k].word[0] != R[rowOf[l - lpf]].p.word[0]) continue;
            // Hauppauge: the first in order (nearest the neighbours' timing); GV-USB2: the lowest cost
            if (best[i] < 0 || (!hauppauge && outs[k].cost < outs[best[i]].cost)) best[i] = (int)k;
        }
        for (int i : gaps)
            if (best[i] >= 0) { take(R[i], jobs[best[i]], outs[best[i]], true); st.wide++; }
    }
    for (auto& r : R) if (r.found) out.push_back(r.p);
    if (stats) *stats = st;
    return true;
}

// ---------------------------------------------------------------- recordings
namespace {
bool jtesGeometry(const VbiGeometry& g) { return !g.pal && g.fmt == 0 && g.samplesPerLine == 1440 && g.linesPerFrame == 24; }

// GV-USB2 channel for the recording (receiver.py calibrate, 'choose'): a channel fitted to the clearest idle packet
// among flat lines spread over the recording, used only when a first reading of a sample of lines (3 starts, the
// main rate) gets at least 1.1 times and 10 more packets with it than with the bundled one
bool chooseChannel(VbiFile& vf, const std::vector<int>& slots, Slicer& sl, std::atomic<bool>* cancel, JtesChannel& out)
{
    out = bundledChannel();
    const VbiGeometry& g = vf.geom();
    const int lpf = 12, n = g.lineBytes;
    std::vector<char> use(lpf, 0);
    for (int s : slots) if (s >= 0 && s < lpf) use[s] = 1;
    std::vector<uint8_t> buf, piece;
    const int64_t K = std::min<int64_t>(g.frames, 500);
    for (int64_t k = 0; k < K; k++) {
        if (cancel && *cancel) return false;
        int64_t f = K == 1 ? 0 : k * (g.frames - 1) / (K - 1);
        if (vf.read(f, 1, piece) <= 0) continue;
        for (int r = 0; r < 2 * lpf; r++)
            if (use[r % lpf]) buf.insert(buf.end(), piece.begin() + (size_t)r * n, piece.begin() + (size_t)(r + 1) * n);
    }
    int64_t rows = (int64_t)buf.size() / n;
    if (rows < 50) return true;
    std::string err;
    if (!sl.jtesLines(buf.data(), rows, n, err)) return false;
    auto line = [&](int64_t r) { return buf.data() + (size_t)r * n; };
    std::vector<Edge> E(rows);
    std::vector<char> hasE(rows, 0);
    for (int64_t r = 0; r < rows; r++) hasE[r] = edge(line(r), n, E[r]);

    // the idle packet fit
    uint8_t idle[NB];
    withPrefix(idleWord(), idle);
    std::vector<JtesJob> jobs;
    std::vector<int64_t> of;
    for (int64_t r = 0; r < rows && jobs.size() < 96; r++) {
        if (!hasE[r] || stdOf(line(r), 250, std::min(n, 1000)) >= 7) continue;
        Channel hh;
        if (!fitbits(line(r), n, idle, E[r].e + 1, 33.0 / 7, hh)) continue;
        jobs.push_back(job((int)r, hh, E[r].e + 1, 33.0 / 7));
        of.push_back(r);
    }
    std::vector<JtesOut> outs;
    if (!jobs.empty() && !sl.jtesDecode(jobs, outs, err)) return false;
    int bi = -1;
    double bs = 0;
    for (size_t k = 0; k < outs.size(); k++) {
        if (outs[k].ec < 0 || !isIdle(outs[k])) continue;
        const Edge& e = E[of[k]];
        double sc = outs[k].cost / ((e.hi - e.lo) * (e.hi - e.lo));
        if (bi < 0 || sc < bs) { bi = (int)k; bs = sc; }
    }
    if (bi < 0) return true;
    JtesChannel fit;
    memcpy(fit.h, jobs[bi].h, sizeof fit.h);
    const Edge& fe = E[of[bi]];
    fit.contrast = fe.hi - fe.lo; fit.edge = fe.e; fit.start = fe.e + 1; fit.fitted = true;

    // both on the same sample
    auto yield = [&](const JtesChannel& c, int64_t& got) {
        Channel h0;
        memcpy(h0.v, c.h, sizeof h0.v);
        std::vector<JtesJob> js;
        std::vector<int64_t> o;
        int64_t seen = 0;
        for (int64_t r = 0; r < rows && seen < 1500; r++) {
            if (!hasE[r]) continue;
            seen++;
            Channel h = scaled(h0, E[r], c.contrast);
            for (double ds : {0.0, -1.0, 1.0}) { js.push_back(job((int)r, h, startOf(E[r], c.start, c.edge) + ds, 33.0 / 7)); o.push_back(r); }
        }
        std::vector<JtesOut> os;
        if (!js.empty() && !sl.jtesDecode(js, os, err)) return false;
        std::vector<char> ok(rows, 0);
        for (size_t k = 0; k < os.size(); k++) if (valid(os[k])) ok[o[k]] = 1;
        got = std::count(ok.begin(), ok.end(), 1);
        return true;
    };
    int64_t ours = 0, theirs = 0;
    if (!yield(out, ours) || !yield(fit, theirs)) return false;
    JtesChannel& pick = theirs >= ours * 1.1 && theirs - ours >= 10 ? fit : out;
    pick.sampleBundled = ours; pick.sampleFitted = theirs;
    out = pick;
    return true;
}
}

bool jtesProbeRecording(VbiFile& vf, Slicer& sl, std::atomic<bool>* cancel, JtesRecRead& out, const std::vector<int>& only)
{
    out = JtesRecRead();
    const VbiGeometry& g = vf.geom();
    if (!jtesGeometry(g) || g.frames < 50) return false;
    const int lpf = 12, n = g.lineBytes;
    std::vector<int> all;
    for (int s = 0; s < lpf; s++)
        if (only.empty() || std::any_of(only.begin(), only.end(), [&](int o) { return std::abs(o - s) <= 1; })) all.push_back(s);
    // the three pieces side by side
    std::vector<uint8_t> buf, piece;
    for (double at : {0.25, 0.5, 0.75}) {
        int64_t f0 = std::max<int64_t>(0, (int64_t)(g.frames * at) - 50);
        int64_t got = vf.read(f0, std::min<int64_t>(40, g.frames - f0), piece);
        if (got > 0) buf.insert(buf.end(), piece.begin(), piece.begin() + (size_t)got * g.frameBytes());
    }
    int64_t rows = (int64_t)buf.size() / n;
    if (rows < lpf * 4) return false;
    int64_t best = 0;
    std::vector<int64_t> bestSlots(lpf, 0);
    for (int mode = 0; mode < 2; mode++) {
        if (cancel && *cancel) return false;
        std::vector<JtesRxPacket> pk;
        std::string err;
        // the first 10 frames of the first piece say whether the rest is worth reading (a datacast line costs as
        // much as a failed packet on every try)
        int64_t head = std::min<int64_t>(rows, (int64_t)20 * lpf);
        if (!jtesReceive(buf.data(), head, n, lpf, all, mode == 1, true, sl, pk, err)) return false;
        if (pk.empty()) continue;
        if (!jtesReceive(buf.data(), rows, n, lpf, all, mode == 1, true, sl, pk, err)) return false;
        std::vector<int64_t> perSlot(lpf, 0);
        for (auto& p : pk) perSlot[p.row % lpf]++;
        if ((int64_t)pk.size() > best) { best = (int64_t)pk.size(); bestSlots = perSlot; out.hauppauge = mode == 1; }
    }
    out.probePackets = best;
    if (best < 20) return false;
    int64_t top = *std::max_element(bestSlots.begin(), bestSlots.end());
    std::vector<char> use(lpf, 0);
    for (int s = 0; s < lpf; s++)
        if (bestSlots[s] >= std::max<int64_t>(3, top / 50))   // a line the teletext moves on keeps its neighbours
            for (int d = -1; d <= 1; d++) if (s + d >= 0 && s + d < lpf) use[s + d] = 1;
    for (int s = 0; s < lpf; s++) if (use[s]) out.slots.push_back(s);
    if (!out.hauppauge && !chooseChannel(vf, out.slots, sl, cancel, out.chan)) return false;
    char b[400];
    int k = snprintf(b, sizeof b, "Japanese teletext: %lld packets in 120 frames, read like %s, lines %d-%d of each field",
                     (long long)best, out.hauppauge ? "a Hauppauge capture (packet start searched by the run-in)" : "a GV-USB2 capture",
                     lineNoOf(g, out.slots.front()), lineNoOf(g, out.slots.back()));
    if (!out.hauppauge && out.chan.sampleFitted >= 0)
        snprintf(b + k, sizeof b - k, "; channel %s (sample of lines: bundled %lld packets, fitted to this tape %lld)",
                 out.chan.fitted ? "fitted to this tape's idle packets" : "bundled GV-USB2 VHS", (long long)out.chan.sampleBundled,
                 (long long)out.chan.sampleFitted);
    out.why = b;
    return true;
}

bool jtesReadRecording(VbiFile& vf, const JtesRecRead& how, int64_t maxFrames, Slicer& sl, std::atomic<bool>* cancel,
                       const std::function<void(double)>& progress, std::vector<FastPacket>& out, std::string& err,
                       JtesRxStats* stats)
{
    out.clear();
    const VbiGeometry& g = vf.geom();
    if (!jtesGeometry(g)) return true;
    const int lpf = 12, n = g.lineBytes;
    int64_t frames = maxFrames > 0 ? std::min(maxFrames, g.frames) : g.frames;
    const int64_t chunk = 2000;   // frames read and received at a time
    JtesRxStats tot;
    std::vector<uint8_t> buf;
    for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
        if (cancel && *cancel) return true;
        int64_t got = vf.read(f0, std::min(chunk, frames - f0), buf);
        if (got <= 0) break;
        JtesRxStats st;
        std::vector<JtesRxPacket> pk;
        if (!jtesReceive(buf.data(), got * 2 * lpf, n, lpf, how.slots, how.hauppauge, false, sl, pk, err, &st, &how.chan)) return false;
        tot.scanned += st.scanned; tot.found += st.found; tot.refined += st.refined; tot.wide += st.wide; tot.idle += st.idle;
        tot.anchors += st.anchors;
        for (auto& p : pk) {
            FastPacket q;
            int l = (int)(p.row % (2 * lpf));
            q.frame = f0 + p.row / (2 * lpf);
            lineOf(g, l, q.lineNo, q.field);
            q.kind = 'E';
            q.bytes = {0x55, 0x55, 0xA7};
            q.bytes.insert(q.bytes.end(), p.word, p.word + 34);
            out.push_back(std::move(q));
        }
        if (progress) progress((double)(f0 + got) / frames);
    }
    if (stats) *stats = tot;
    return true;
}
