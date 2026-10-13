// Second reading of the 5.7 Mbit/s lines once their kind is known: the MLSE
// of the original Teletext Rescue (mlse.cpp) with a channel model fitted to
// every line and code-constrained bytes. The first pass (fastdata_core.h)
// uses one channel per line position, which is too coarse for worn tape.
#include "fastdata.h"
#include "fastdata_core.h"
#include "mlse.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

uint8_t rev8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
    return (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

const uint8_t kCounter2D[8] = {0x8B, 0x9C, 0x4D, 0xC6, 0x17, 0xE8, 0x39, 0x5A};

mlse::Model modelFor(const FastLineJob& j, double spb)
{
    mlse::Model m;
    m.spb = spb;
    m.pre[0] = 0x55;
    m.pre[1] = 0x55;
    switch (j.kind) {
    case 'N':
        m.pre[2] = 0xE7;
        m.nbytes = 33;
        m.good = [](const uint8_t* p) {
            for (int i = 0; i < 5; i++)
                if (mlse::hamExact(p[i]) < 0) return false;
            return true;
        };
        m.code = [](const uint8_t*, int i) { return i < 5 ? 1 : 0; };
        break;
    case 'W':
        m.pre[2] = 0x27;
        m.nbytes = 34;
        m.good = [](const uint8_t* p) { return mlse::hamExact(p[0]) >= 0 && mlse::hamExact(p[1]) >= 0; };
        m.code = mlse::wstCode;
        break;
    case 'X': {
        m.pre[2] = 0x2D;
        m.nbytes = 34;
        std::vector<int> svc = j.services;
        bool cyc = j.cycle8;
        m.good = [svc, cyc](const uint8_t* p) {
            uint8_t s = rev8(p[0]), c = rev8(p[1]);
            bool okS = std::find(svc.begin(), svc.end(), s) != svc.end();
            bool okC = !cyc || std::find(kCounter2D, kCounter2D + 8, c) != kCounter2D + 8;
            return okS && okC;
        };
        break;
    }
    default: // NABTS sync, header not NABTS: nothing to check the packet with; 34 bytes so that a
             // Japanese teletext line (BT.653 system D: 272-bit (272,190) codeword after E7) is whole
        m.pre[2] = 0xE7;
        m.nbytes = 34;
        m.good = [](const uint8_t*) { return true; };
        break;
    }
    return m;
}

template <class F> void parallelFor(size_t n, int threads, F fn)
{
    int nt = threads > 0 ? threads : (int)std::max(1u, std::thread::hardware_concurrency());
    nt = (int)std::min<size_t>(nt, std::max<size_t>(1, n));
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (int t = 0; t < nt; t++)
        th.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
        });
    for (auto& t : th) t.join();
}

struct LineSig {
    std::vector<float> y; // normalised samples
    float edge = 0;
    bool ok = false;
};

void getLine(const uint8_t* p, const VbiGeometry& g, LineSig& s)
{
    float info[4];
    fd_prepare(p, g.fmt, g.validSamples, g.lineSamples, info);
    s.ok = info[3] > 0;
    s.edge = info[0];
    if (!s.ok) return;
    std::vector<float> raw(g.validSamples);
    for (int k = 0; k < g.validSamples; k++) raw[k] = fd_get(p, g.fmt, k);
    s.y.resize(g.validSamples);
    mlse::normalize(raw.data(), g.validSamples, s.y.data());
}

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

std::vector<FastPacket> fdSecondPass(VbiFile& vf, const std::vector<FastLineJob>& jobs, int threads, std::atomic<bool>* cancel,
                                     const std::function<void(double)>& progress, const std::function<void(const std::string&)>& note,
                                     std::vector<std::pair<int, int>>* rejected)
{
    std::vector<FastPacket> out;
    const VbiGeometry& g = vf.geom();
    const int64_t frames = g.frames;
    const double spb = g.lineSamples / g.fastRate;
    std::vector<uint8_t> buf;
    for (size_t jn = 0; jn < jobs.size(); jn++) {
        const FastLineJob& j = jobs[jn];
        mlse::Engine eng(modelFor(j, spb));
        // --- start-up: learn the channel on the first lines ---------------
        std::vector<double> rel, mets;
        LineSig s;
        const int64_t learnN = std::min<int64_t>(frames, 600);
        vf.read(0, learnN, buf);
        for (int64_t f = 0; f < learnN && eng.learned() < 200; f++) {
            getLine(&buf[(size_t)f * g.frameBytes() + (size_t)j.index * g.lineBytes], g, s);
            if (!s.ok) continue;
            mlse::Result r = eng.decodeLearn(s.y.data(), g.validSamples, s.edge - 4 * spb, s.edge + 4 * spb, spb / 5);
            if (r.good) { rel.push_back(r.off - s.edge); mets.push_back(r.met); }
        }
        char b[240];
        if (!eng.hasPrior() || rel.size() < 5) {
            snprintf(b, sizeof b, "MLSE (per-line channel): line %d field %d: no packets to learn the channel on, first reading kept",
                     j.lineNo, j.field);
            note(b);
            continue;
        }
        const double d0 = median(rel);
        const double gate = 4 * median(mets);
        // --- whole recording ------------------------------------------------
        const int chunk = 512;
        size_t lines = 0, good = 0, kept = 0;
        std::vector<FastPacket> pk;
        for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
            if (cancel && *cancel) return {};
            int got = (int)vf.read(f0, std::min<int64_t>(chunk, frames - f0), buf);
            if (got <= 0) break;
            std::vector<LineSig> ls(got);
            std::vector<mlse::Result> rs(got);
            parallelFor(got, threads, [&](size_t f) {
                getLine(&buf[f * g.frameBytes() + (size_t)j.index * g.lineBytes], g, ls[f]);
                if (!ls[f].ok) return;
                double c = ls[f].edge + d0;
                rs[f] = eng.decode(ls[f].y.data(), g.validSamples, c - 1.5 * spb, c + 1.5 * spb, spb / 16);
            });
            for (int f = 0; f < got; f++) {
                if (!ls[f].ok || rs[f].data.empty()) continue;
                lines++;
                good += rs[f].good;
                eng.learn(rs[f]);
                if (rs[f].met > gate && !rs[f].good) continue; // no packet on this line in this frame
                FastPacket p;
                p.frame = f0 + f;
                p.lineNo = j.lineNo;
                p.field = j.field;
                p.kind = j.kind; // E: NABTS sync, not NABTS data
                p.bytes.assign(eng.M.pre, eng.M.pre + 3);
                p.bytes.insert(p.bytes.end(), rs[f].data.begin(), rs[f].data.end());
                if (p.kind == 'X')
                    for (auto& v : p.bytes) v = rev8(v); // the 2D service is read MSB first
                p.syncErr = rs[f].good ? 0 : 1;
                pk.push_back(std::move(p));
                kept++;
            }
            if (progress) progress(((double)jn + (double)(f0 + got) / frames) / jobs.size());
        }
        snprintf(b, sizeof b,
                 "MLSE (per-line channel): line %d field %d (%c): %zu lines with signal, %zu packets, %zu pass the check (%.0f%%)",
                 j.lineNo, j.field, j.kind, lines, kept, good, 100.0 * good / std::max<size_t>(1, lines));
        note(b);
        // a line that mostly fails the check is not this service: under a quarter
        // it is dropped, under a half only checked packets stay
        if (j.kind == 'X' && good * 4 < lines) { // 2D: the check knows little, keep the first reading
            snprintf(b, sizeof b, "  line %d field %d: the 2D check fails here, first reading kept", j.lineNo, j.field);
            note(b);
            continue;
        }
        if (j.kind != 'E' && good * 4 < lines) {
            snprintf(b, sizeof b, "  line %d field %d: not %c after all (%zu of %zu checked), dropped", j.lineNo, j.field, j.kind, good, lines);
            note(b);
            if (rejected) rejected->push_back({j.lineNo, j.field});
            continue;
        }
        if (j.kind != 'E' && good * 2 < lines) {
            pk.erase(std::remove_if(pk.begin(), pk.end(), [](const FastPacket& p) { return p.syncErr != 0; }), pk.end());
            snprintf(b, sizeof b, "  line %d field %d: mostly not %c, %zu checked packets kept", j.lineNo, j.field, j.kind, pk.size());
            note(b);
        }
        out.insert(out.end(), pk.begin(), pk.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const FastPacket& a, const FastPacket& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        if (a.field != b.field) return a.field < b.field;
        return a.lineNo < b.lineNo;
    });
    return out;
}
