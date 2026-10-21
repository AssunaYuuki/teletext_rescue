// Second reading of the 5.7 Mbit/s lines once their kind is known: the MLSE
// of the original Teletext Rescue (mlse.cpp) with a channel model fitted to
// every line and code-constrained bytes. The first pass (fastdata_core.h)
// uses one channel per line position, which is too coarse for worn tape.
#include "fastdata.h"
#include "fastdata_core.h"
#include "mlse.h"
#include "legacy/pre.h"
namespace legacy {
#include "legacy/src/mlse.h"
}
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


// The same MLSE on the GPU: the original engine's MlseBatch (legacy/src/mlse.cpp, the code mlse.cpp was ported
// from) reads thousands of lines at once. Its search window is one for all lines, so each line is moved to put its
// expected start at the same place first. Returns false when the GPU is not there (the processor reads instead).
bool secondPassGpu(VbiFile& vf, const FastLineJob& j, double spb, std::atomic<bool>* cancel,
                   const std::function<void(double)>& progress, size_t jn, size_t jobsN,
                   std::vector<FastPacket>& pk, size_t& lines, size_t& good, size_t& kept, bool& noPrior)
{
    const VbiGeometry& g = vf.geom();
    const int64_t frames = g.frames;
    mlse::Model m = modelFor(j, spb);
    legacy::MlseModel lm;
    lm.spb = m.spb;
    for (int i = 0; i < 3; i++) lm.pre[i] = m.pre[i];
    lm.nbytes = m.nbytes; lm.L = m.L; lm.R = m.R; lm.ntop = m.ntop; lm.refits = std::max(1, m.refits);
    lm.good = m.good;
    lm.code = m.code;
    // start-up: the channel learnt on the first lines (as decodeLearn)
    legacy::MlseDecoder learn(lm);
    std::vector<double> rel, mets;
    std::vector<uint8_t> buf;
    LineSig s;
    const int64_t learnN = std::min<int64_t>(frames, 600);
    vf.read(0, learnN, buf);
    for (int64_t f = 0; f < learnN && learn.n < 200; f++) {
        getLine(&buf[(size_t)f * g.frameBytes() + (size_t)j.index * g.lineBytes], g, s);
        if (!s.ok) continue;
        auto r = learn.decode(s.y.data(), g.validSamples, s.edge - 4 * spb, s.edge + 4 * spb, spb / 5);
        if (r.good) { rel.push_back(r.off - s.edge); mets.push_back(r.met); }
    }
    if (!learn.has_prior() || rel.size() < 5) { noPrior = true; return true; }
    const double d0 = median(rel), gate = 4 * median(mets);
    legacy::Progress lp;
    legacy::MlseBatch bd(lm, learn.prior, learn.n, true, lp);
    if (!bd.on_gpu()) return false;
    const int n = g.validSamples;
    const double A = 2 * spb + 8;   // where every line's expected start is put
    const int64_t chunk = std::max<int64_t>(64, (int64_t)bd.batch());
    std::vector<float> rows;
    for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
        if (cancel && *cancel) return true;
        int got = (int)vf.read(f0, std::min<int64_t>(chunk, frames - f0), buf);
        if (got <= 0) break;
        rows.assign((size_t)got * n, 0.0f);
        std::vector<const float*> rp;
        std::vector<int> idx, shift;
        for (int f = 0; f < got; f++) {
            getLine(&buf[(size_t)f * g.frameBytes() + (size_t)j.index * g.lineBytes], g, s);
            if (!s.ok) continue;
            int sh = (int)std::floor(s.edge + d0 - A);
            float* row = &rows[(size_t)f * n];
            for (int i = 0; i < n; i++) { int k = i + sh; row[i] = k >= 0 && k < n ? s.y[k] : 0.0f; }
            rp.push_back(row); idx.push_back(f); shift.push_back(sh);
        }
        if (rp.empty()) continue;
        auto rs = bd.decode(rp, n, A - 1.5 * spb, A + 1.5 * spb + 1, nullptr, spb / 16);
        for (size_t k = 0; k < rs.size() && k < idx.size(); k++) {
            auto& r = rs[k];
            if (r.data.empty()) continue;
            lines++;
            good += r.good;
            if (r.met > gate && !r.good) continue;   // no packet on this line in this frame
            FastPacket p;
            p.frame = f0 + idx[k];
            p.lineNo = j.lineNo;
            p.field = j.field;
            p.kind = j.kind;
            p.bytes.assign(m.pre, m.pre + 3);
            p.bytes.insert(p.bytes.end(), r.data.begin(), r.data.end());
            if (p.kind == 'X')
                for (auto& v : p.bytes) v = rev8(v);
            p.syncErr = r.good ? 0 : 1;
            pk.push_back(std::move(p));
            kept++;
        }
        if (progress) progress(((double)jn + (double)(f0 + got) / frames) / jobsN);
    }
    return true;
}

} // namespace

std::vector<FastPacket> fdSecondPass(VbiFile& vf, const std::vector<FastLineJob>& jobs, int threads, std::atomic<bool>* cancel,
                                     const std::function<void(double)>& progress, const std::function<void(const std::string&)>& note,
                                     std::vector<std::pair<int, int>>* rejected, bool gpu)
{
    std::vector<FastPacket> out;
    const VbiGeometry& g = vf.geom();
    const int64_t frames = g.frames;
    const double spb = g.lineSamples / g.fastRate;
    std::vector<uint8_t> buf;
    for (size_t jn = 0; jn < jobs.size(); jn++) {
        const FastLineJob& j = jobs[jn];
        char b[240];
        size_t lines = 0, good = 0, kept = 0;
        std::vector<FastPacket> pk;
        bool onGpu = false, noPrior = false;
        // Japanese teletext (E) stays on the processor: its channel follows the tape line by line there, the GPU
        // batch only between thousands of lines (NHK 1996 4fsc: 1418 pages on the processor, 1370 on the GPU)
        if (gpu && j.kind != 'E') onGpu = secondPassGpu(vf, j, spb, cancel, progress, jn, jobs.size(), pk, lines, good, kept, noPrior);
        if (cancel && *cancel) return {};
        if (noPrior) {
            snprintf(b, sizeof b, "MLSE (per-line channel): line %d field %d: no packets to learn the channel on, first reading kept",
                     j.lineNo, j.field);
            note(b);
            continue;
        }
        if (!onGpu) {
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
        }   // processor
        snprintf(b, sizeof b,
                 "MLSE (per-line channel%s): line %d field %d (%c): %zu lines with signal, %zu packets, %zu pass the check (%.0f%%)",
                 onGpu ? ", GPU" : "", j.lineNo, j.field, j.kind, lines, kept, good, 100.0 * good / std::max<size_t>(1, lines));
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
