#include "slicer.h"
#include "slicer_core.h"
#include "fastdata_core.h"
#include "jtes_core.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>

namespace {

class CpuSlicer : public Slicer {
public:
    explicit CpuSlicer(int threads) : threads_(threads > 0 ? threads : 1) {}
    std::string name() const override { return "CPU (" + std::to_string(threads_) + " threads)"; }

    bool run(const uint8_t* chunk, int frames, const VbiGeometry& g, const std::vector<SliceJob>& jobs,
             std::vector<float>& out, std::string&) override
    {
        const int nj = (int)jobs.size();
        out.assign((size_t)frames * nj * kSliceOut, 0.0f);
        auto work = [&](int f0, int f1) {
            for (int f = f0; f < f1; f++)
                for (int j = 0; j < nj; j++) {
                    const uint8_t* line = chunk + ((size_t)f * g.linesPerFrame + jobs[j].line) * g.lineBytes;
                    slice_line(line, g.fmt, g.validSamples, g.lineSamples, jobs[j].mode, jobs[j].phase, jobs[j].anchor,
                               &out[((size_t)f * nj + j) * kSliceOut]);
                }
        };
        int nt = std::min(threads_, std::max(1, frames / 16));
        std::vector<std::thread> pool;
        int per = (frames + nt - 1) / nt;
        for (int t = 0; t < nt; t++) {
            int a = t * per, b = std::min(frames, a + per);
            if (a < b) pool.emplace_back(work, a, b);
        }
        for (auto& th : pool) th.join();
        return true;
    }

    bool runFast(const uint8_t* chunk, int frames, const VbiGeometry& g, const std::vector<FastJob>& jobs,
                 std::vector<uint8_t>& bits, std::string&) override
    {
        const int nj = (int)jobs.size();
        bits.assign((size_t)frames * nj * kFastBits, 0);
        auto work = [&](int f0, int f1) {
            std::vector<uint8_t> scratch(kFastBits * 64);
            for (int f = f0; f < f1; f++)
                for (int j = 0; j < nj; j++) {
                    const uint8_t* line = chunk + ((size_t)f * g.linesPerFrame + jobs[j].line) * g.lineBytes;
                    float info[4];
                    fd_prepare(line, g.fmt, g.validSamples, g.lineSamples, info);
                    if (info[3] <= 0) continue;
                    fd_viterbi(line, g.fmt, g.validSamples, jobs[j].spb, jobs[j].phi, jobs[j].H, info, kFastBits,
                               scratch.data(), &bits[((size_t)f * nj + j) * kFastBits]);
                }
        };
        int nt = std::min(threads_, std::max(1, frames / 8));
        std::vector<std::thread> pool;
        int per = (frames + nt - 1) / nt;
        for (int t = 0; t < nt; t++) {
            int a = t * per, b = std::min(frames, a + per);
            if (a < b) pool.emplace_back(work, a, b);
        }
        for (auto& th : pool) th.join();
        return true;
    }

    bool jtesLines(const uint8_t* lines, int64_t nLines, int n, std::string&) override
    {
        jl_ = lines; jn_ = nLines; jw_ = n;
        return true;
    }
    bool jtesDecode(const std::vector<JtesJob>& jobs, std::vector<JtesOut>& out, std::string&) override
    {
        out.assign(jobs.size(), JtesOut());
        each(jobs.size(), [&](size_t i) {
            std::vector<unsigned long long> back(299);
            const JtesJob& j = jobs[i];
            JtesOut& o = out[i];
            jt_decode(jl_ + (size_t)j.line * jw_, jw_, j.st, j.spb, j.h, back.data(), o.word, o.bits, &o.cost, &o.ec);
        });
        return true;
    }
    bool jtesCand(const std::vector<int>& lines, const float* pred, std::vector<JtesCand>& out, std::string&) override
    {
        out.assign(lines.size(), JtesCand());
        each(lines.size(), [&](size_t i) {
            std::vector<float> costs(1128);
            JtesCand& c = out[i];
            jt_cand(jl_ + (size_t)lines[i] * jw_, jw_, pred, costs.data(), c.st, c.spb, &c.n, &c.fit);
        });
        return true;
    }

private:
    // f(0 .. n-1) over the threads
    void each(size_t n, const std::function<void(size_t)>& f)
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        int nt = (int)std::min<size_t>(threads_, std::max<size_t>(1, n / 4));
        for (int t = 0; t < nt; t++)
            pool.emplace_back([&] { for (size_t i; (i = next++) < n;) f(i); });
        for (auto& th : pool) th.join();
    }
    int threads_;
    const uint8_t* jl_ = nullptr;
    int64_t jn_ = 0;
    int jw_ = 0;
};

} // namespace

std::unique_ptr<Slicer> makeCpuSlicer(int threads)
{
    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
    return std::make_unique<CpuSlicer>(threads);
}
