#include "slicer.h"
#include "slicer_core.h"
#include "fastdata_core.h"
#include <algorithm>
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

private:
    int threads_;
};

} // namespace

std::unique_ptr<Slicer> makeCpuSlicer(int threads)
{
    if (threads <= 0) threads = (int)std::max(1u, std::thread::hardware_concurrency());
    return std::make_unique<CpuSlicer>(threads);
}
