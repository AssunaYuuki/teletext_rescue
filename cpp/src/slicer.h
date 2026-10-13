#pragma once
#include "fastdata.h"
#include "vbifile.h"
#include <memory>
#include <string>
#include <vector>

enum SliceMode { MODE_CC = 0, MODE_SILENT_RADIO = 1, MODE_FAST = 2, MODE_AMOL = 3, MODE_TEST = 4, MODE_GEMSTAR = 6, MODE_TVINFORM = 7 };

struct SliceJob {
    int line; // line index inside the frame (0..linesPerFrame-1)
    int mode; // SliceMode
    float phase = 0; // data sampling offset in bits
    float anchor = 0; // start-bit centre in samples, 0 = search
};

constexpr int kSliceOut = 20; // floats per sliced line, see slicer_core.h

class Slicer {
public:
    virtual ~Slicer() = default;
    virtual std::string name() const = 0;
    // Slices `jobs` on every frame of the chunk. out = frames * jobs * kSliceOut.
    virtual bool run(const uint8_t* chunk, int frames, const VbiGeometry& g,
                     const std::vector<SliceJob>& jobs, std::vector<float>& out, std::string& err) = 0;
    // MLSE decoding of 5.7 Mbit/s lines. bits = frames * jobs * kFastBits.
    virtual bool runFast(const uint8_t* chunk, int frames, const VbiGeometry& g, const std::vector<FastJob>& jobs,
                         std::vector<uint8_t>& bits, std::string& err) = 0;
};

std::unique_ptr<Slicer> makeCpuSlicer(int threads);
// Returns nullptr (and a reason in err) when no OpenCL GPU is usable.
std::unique_ptr<Slicer> makeGpuSlicer(std::string& err);
// Name and memory of the first OpenCL GPU, without building the kernels.
bool gpuDeviceInfo(std::string& name, uint64_t& memMB);
