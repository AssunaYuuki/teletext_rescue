// OpenCL slicer. OpenCL.dll ships with the GPU driver, so it is loaded at
// run time and no SDK is needed to build. The kernel source is the very same
// slicer_core.h that the CPU path compiles.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "slicer.h"
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
// Linux, macOS: the OpenCL library of the driver, loaded the same way
#include <dlfcn.h>
typedef void* HMODULE;
static HMODULE LoadLibraryA(const char*)
{
    for (const char* n : {"libOpenCL.so.1", "libOpenCL.so", "/System/Library/Frameworks/OpenCL.framework/OpenCL"})
        if (HMODULE h = dlopen(n, RTLD_NOW)) return h;
    return nullptr;
}
#define GetProcAddress dlsym
#define FreeLibrary dlclose
#endif

#define GLOBAL __global
#define FN
#define SLICER_SRC(...) static const char* kCoreSrc = #__VA_ARGS__;
#include "slicer_core.h"
#undef SLICER_SRC
#define FASTDATA_SRC(...) static const char* kFastSrc = #__VA_ARGS__;
#include "fastdata_core.h"
#undef FASTDATA_SRC

namespace {

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_ulong cl_bitfield;
typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;

constexpr cl_bitfield CL_DEVICE_TYPE_GPU = 1 << 2;
constexpr cl_bitfield CL_MEM_READ_WRITE = 1 << 0;
constexpr cl_bitfield CL_MEM_WRITE_ONLY = 1 << 1;
constexpr cl_bitfield CL_MEM_READ_ONLY = 1 << 2;
constexpr cl_uint CL_DEVICE_NAME = 0x102B;
constexpr cl_uint CL_PROGRAM_BUILD_LOG = 0x1183;
constexpr cl_uint CL_TRUE = 1;

#ifdef _WIN32
#define CLAPI __stdcall
#else
#define CLAPI
#endif
typedef cl_int(CLAPI* PFN_GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
typedef cl_int(CLAPI* PFN_GetDeviceIDs)(cl_platform_id, cl_bitfield, cl_uint, cl_device_id*, cl_uint*);
typedef cl_int(CLAPI* PFN_GetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
typedef cl_context(CLAPI* PFN_CreateContext)(const intptr_t*, cl_uint, const cl_device_id*, void*, void*, cl_int*);
typedef cl_command_queue(CLAPI* PFN_CreateCommandQueue)(cl_context, cl_device_id, cl_bitfield, cl_int*);
typedef cl_program(CLAPI* PFN_CreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
typedef cl_int(CLAPI* PFN_BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void*, void*);
typedef cl_int(CLAPI* PFN_GetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
typedef cl_kernel(CLAPI* PFN_CreateKernel)(cl_program, const char*, cl_int*);
typedef cl_mem(CLAPI* PFN_CreateBuffer)(cl_context, cl_bitfield, size_t, void*, cl_int*);
typedef cl_int(CLAPI* PFN_SetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
typedef cl_int(CLAPI* PFN_EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*,
                                                const size_t*, cl_uint, const cl_event*, cl_event*);
typedef cl_int(CLAPI* PFN_EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void*, cl_uint,
                                              const cl_event*, cl_event*);
typedef cl_int(CLAPI* PFN_EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void*, cl_uint,
                                             const cl_event*, cl_event*);
typedef cl_int(CLAPI* PFN_Finish)(cl_command_queue);
typedef cl_int(CLAPI* PFN_Release)(void*);

const char* kKernelSrc =
    "__kernel void slice_kernel(__global const unsigned char* raw, int fmt, int lineBytes, int linesPerFrame,\n"
    "                           int nsamp, float spl, __global const int* jobs, int njobs, int frames,\n"
    "                           __global float* out)\n"
    "{\n"
    "    int g = get_global_id(0);\n"
    "    if (g >= njobs * frames) return;\n"
    "    int f = g / njobs, j = g - f * njobs;\n"
    "    __global const unsigned char* line = raw + ((size_t)f * linesPerFrame + jobs[4 * j]) * lineBytes;\n"
    "    slice_line(line, fmt, nsamp, spl, jobs[4 * j + 1], jobs[4 * j + 2] * 0.001f, jobs[4 * j + 3] * 0.01f, out + (size_t)g * 20);\n"
    "}\n";

const char* kFastKernelSrc =
    "__kernel void fast_kernel(__global const unsigned char* raw, int fmt, int lineBytes, int linesPerFrame,\n"
    "                          int nsamp, float spl, __global const int* jl, __global const float* jf, int njobs,\n"
    "                          int frames, __global float* info, __global unsigned char* scratch,\n"
    "                          __global unsigned char* bits)\n"
    "{\n"
    "    int g = get_global_id(0);\n"
    "    if (g >= njobs * frames) return;\n"
    "    int f = g / njobs, j = g - f * njobs;\n"
    "    __global const unsigned char* line = raw + ((size_t)f * linesPerFrame + jl[j]) * lineBytes;\n"
    "    __global float* inf = info + (size_t)g * 4;\n"
    "    __global unsigned char* out = bits + (size_t)g * 400;\n"
    "    fd_prepare(line, fmt, nsamp, spl, inf);\n"
    "    if (inf[3] <= 0.0f) { for (int k = 0; k < 400; k++) out[k] = 0; return; }\n"
    "    fd_viterbi(line, fmt, nsamp, jf[j * 18 + 1], jf[j * 18], jf + j * 18 + 2, inf, 400,\n"
    "               scratch + (size_t)g * 400 * 64, out);\n"
    "}\n";

class GpuSlicer : public Slicer {
public:
    ~GpuSlicer() override
    {
        if (inBuf_) Release_(inBuf_);
        if (outBuf_) Release_(outBuf_);
        if (jobBuf_) Release_(jobBuf_);
        if (kernel_) ReleaseKernel_(kernel_);
        if (fastKernel_) ReleaseKernel_(fastKernel_);
        for (cl_mem m : {fInfo_, fScratch_, fBits_, fJl_, fJf_})
            if (m) Release_(m);
        if (prog_) ReleaseProgram_(prog_);
        if (queue_) ReleaseQueue_(queue_);
        if (ctx_) ReleaseContext_(ctx_);
        if (dll_) FreeLibrary(dll_);
    }

    bool init(std::string& err)
    {
        dll_ = LoadLibraryA("OpenCL.dll");
        if (!dll_) { err = "OpenCL.dll not found (no GPU driver with OpenCL)"; return false; }
#define LOAD(var, name)                                                   \
    var = (decltype(var))(void*)GetProcAddress(dll_, name);               \
    if (!var) { err = std::string("OpenCL.dll lacks ") + name; return false; }
        LOAD(GetPlatformIDs_, "clGetPlatformIDs");
        LOAD(GetDeviceIDs_, "clGetDeviceIDs");
        LOAD(GetDeviceInfo_, "clGetDeviceInfo");
        LOAD(CreateContext_, "clCreateContext");
        LOAD(CreateCommandQueue_, "clCreateCommandQueue");
        LOAD(CreateProgramWithSource_, "clCreateProgramWithSource");
        LOAD(BuildProgram_, "clBuildProgram");
        LOAD(GetProgramBuildInfo_, "clGetProgramBuildInfo");
        LOAD(CreateKernel_, "clCreateKernel");
        LOAD(CreateBuffer_, "clCreateBuffer");
        LOAD(SetKernelArg_, "clSetKernelArg");
        LOAD(EnqueueNDRangeKernel_, "clEnqueueNDRangeKernel");
        LOAD(EnqueueWriteBuffer_, "clEnqueueWriteBuffer");
        LOAD(EnqueueReadBuffer_, "clEnqueueReadBuffer");
        LOAD(Finish_, "clFinish");
        LOAD(Release_, "clReleaseMemObject");
        LOAD(ReleaseKernel_, "clReleaseKernel");
        LOAD(ReleaseProgram_, "clReleaseProgram");
        LOAD(ReleaseQueue_, "clReleaseCommandQueue");
        LOAD(ReleaseContext_, "clReleaseContext");
#undef LOAD
        cl_platform_id plats[8];
        cl_uint np = 0;
        if (GetPlatformIDs_(8, plats, &np) != 0 || np == 0) { err = "no OpenCL platforms"; return false; }
        for (cl_uint i = 0; i < np && !dev_; i++) {
            cl_uint nd = 0;
            if (GetDeviceIDs_(plats[i], CL_DEVICE_TYPE_GPU, 1, &dev_, &nd) != 0 || nd == 0) dev_ = nullptr;
        }
        if (!dev_) { err = "no OpenCL GPU device"; return false; }
        char nm[256] = {};
        GetDeviceInfo_(dev_, CL_DEVICE_NAME, sizeof nm - 1, nm, nullptr);
        devName_ = nm;
        cl_int e = 0;
        ctx_ = CreateContext_(nullptr, 1, &dev_, nullptr, nullptr, &e);
        if (!ctx_) { err = "clCreateContext failed " + std::to_string(e); return false; }
        queue_ = CreateCommandQueue_(ctx_, dev_, 0, &e);
        if (!queue_) { err = "clCreateCommandQueue failed " + std::to_string(e); return false; }
        std::string src = "#define GLOBAL __global\n#define FN\n";
        src += kCoreSrc;
        src += "\n";
        src += kKernelSrc;
        src += "\n";
        src += kFastSrc;
        src += "\n";
        src += kFastKernelSrc;
        const char* s = src.c_str();
        prog_ = CreateProgramWithSource_(ctx_, 1, &s, nullptr, &e);
        if (!prog_) { err = "clCreateProgramWithSource failed"; return false; }
        if (BuildProgram_(prog_, 1, &dev_, "-cl-fast-relaxed-math", nullptr, nullptr) != 0) {
            size_t len = 0;
            GetProgramBuildInfo_(prog_, dev_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &len);
            std::string log(len + 1, '\0');
            GetProgramBuildInfo_(prog_, dev_, CL_PROGRAM_BUILD_LOG, len, &log[0], nullptr);
            err = "OpenCL build failed: " + log.substr(0, 2000);
            return false;
        }
        kernel_ = CreateKernel_(prog_, "slice_kernel", &e);
        if (kernel_) fastKernel_ = CreateKernel_(prog_, "fast_kernel", &e);
        if (!kernel_) { err = "clCreateKernel failed"; return false; }
        return true;
    }

    std::string name() const override { return "GPU OpenCL (" + devName_ + ")"; }

    bool run(const uint8_t* chunk, int frames, const VbiGeometry& g, const std::vector<SliceJob>& jobs,
             std::vector<float>& out, std::string& err) override
    {
        const int nj = (int)jobs.size();
        size_t inSize = (size_t)frames * g.frameBytes();
        size_t outSize = (size_t)frames * nj * kSliceOut * sizeof(float);
        out.assign((size_t)frames * nj * kSliceOut, 0.0f);
        if (!ensure(inBuf_, inCap_, inSize, CL_MEM_READ_ONLY) || !ensure(outBuf_, outCap_, outSize, CL_MEM_WRITE_ONLY) ||
            !ensure(jobBuf_, jobCap_, nj * 4 * sizeof(int), CL_MEM_READ_ONLY)) {
            err = "clCreateBuffer failed";
            return false;
        }
        std::vector<int> jb;
        for (auto& j : jobs) {
            jb.push_back(j.line);
            jb.push_back(j.mode);
            jb.push_back((int)(j.phase * 1000.0f + (j.phase >= 0 ? 0.5f : -0.5f)));
            jb.push_back((int)(j.anchor * 100.0f + 0.5f));
        }
        cl_int e = EnqueueWriteBuffer_(queue_, inBuf_, CL_TRUE, 0, inSize, chunk, 0, nullptr, nullptr);
        e |= EnqueueWriteBuffer_(queue_, jobBuf_, CL_TRUE, 0, jb.size() * sizeof(int), jb.data(), 0, nullptr, nullptr);
        int nsamp = g.validSamples;
        float spl = g.lineSamples;
        int a = 0;
        SetKernelArg_(kernel_, a++, sizeof(cl_mem), &inBuf_);
        SetKernelArg_(kernel_, a++, sizeof(int), &g.fmt);
        SetKernelArg_(kernel_, a++, sizeof(int), &g.lineBytes);
        SetKernelArg_(kernel_, a++, sizeof(int), &g.linesPerFrame);
        SetKernelArg_(kernel_, a++, sizeof(int), &nsamp);
        SetKernelArg_(kernel_, a++, sizeof(float), &spl);
        SetKernelArg_(kernel_, a++, sizeof(cl_mem), &jobBuf_);
        SetKernelArg_(kernel_, a++, sizeof(int), &nj);
        SetKernelArg_(kernel_, a++, sizeof(int), &frames);
        SetKernelArg_(kernel_, a++, sizeof(cl_mem), &outBuf_);
        size_t local = 64, global = ((size_t)frames * nj + local - 1) / local * local;
        e |= EnqueueNDRangeKernel_(queue_, kernel_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
        e |= EnqueueReadBuffer_(queue_, outBuf_, CL_TRUE, 0, outSize, out.data(), 0, nullptr, nullptr);
        Finish_(queue_);
        if (e != 0) { err = "OpenCL enqueue failed"; return false; }
        return true;
    }

private:
    bool runFast(const uint8_t* chunk, int frames, const VbiGeometry& g, const std::vector<FastJob>& jobs,
                 std::vector<uint8_t>& bits, std::string& err) override
    {
        const int nj = (int)jobs.size();
        bits.assign((size_t)frames * nj * kFastBits, 0);
        if (!fastKernel_ || nj == 0) { err = "OpenCL fast kernel unavailable"; return false; }
        std::vector<int> jl;
        std::vector<float> jf;
        for (auto& j : jobs) {
            jl.push_back(j.line);
            jf.push_back(j.phi);
            jf.push_back(j.spb);
            jf.insert(jf.end(), j.H, j.H + 16);
        }
        // the Viterbi traceback needs 20 KB per line, so go in batches
        const int batch = std::max(1, (int)((256u << 20) / ((size_t)nj * kFastBits * 64)));
        for (int f0 = 0; f0 < frames; f0 += batch) {
            int fr = std::min(batch, frames - f0);
            size_t items = (size_t)fr * nj;
            size_t inSize = (size_t)fr * g.frameBytes();
            if (!ensure(inBuf_, inCap_, inSize, CL_MEM_READ_ONLY) ||
                !ensure(fInfo_, fInfoCap_, items * 4 * sizeof(float), CL_MEM_READ_WRITE) ||
                !ensure(fScratch_, fScratchCap_, items * kFastBits * 64, CL_MEM_READ_WRITE) ||
                !ensure(fBits_, fBitsCap_, items * kFastBits, CL_MEM_WRITE_ONLY) ||
                !ensure(fJl_, fJlCap_, jl.size() * sizeof(int), CL_MEM_READ_ONLY) ||
                !ensure(fJf_, fJfCap_, jf.size() * sizeof(float), CL_MEM_READ_ONLY)) {
                err = "clCreateBuffer failed";
                return false;
            }
            cl_int e = EnqueueWriteBuffer_(queue_, inBuf_, CL_TRUE, 0, inSize, chunk + (size_t)f0 * g.frameBytes(), 0,
                                           nullptr, nullptr);
            e |= EnqueueWriteBuffer_(queue_, fJl_, CL_TRUE, 0, jl.size() * sizeof(int), jl.data(), 0, nullptr, nullptr);
            e |= EnqueueWriteBuffer_(queue_, fJf_, CL_TRUE, 0, jf.size() * sizeof(float), jf.data(), 0, nullptr, nullptr);
            int nsamp = g.validSamples;
            float spl = g.lineSamples;
            int a = 0;
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &inBuf_);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &g.fmt);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &g.lineBytes);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &g.linesPerFrame);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &nsamp);
            SetKernelArg_(fastKernel_, a++, sizeof(float), &spl);
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &fJl_);
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &fJf_);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &nj);
            SetKernelArg_(fastKernel_, a++, sizeof(int), &fr);
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &fInfo_);
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &fScratch_);
            SetKernelArg_(fastKernel_, a++, sizeof(cl_mem), &fBits_);
            size_t local = 32, global = (items + local - 1) / local * local;
            e |= EnqueueNDRangeKernel_(queue_, fastKernel_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, fBits_, CL_TRUE, 0, items * kFastBits,
                                    &bits[(size_t)f0 * nj * kFastBits], 0, nullptr, nullptr);
            Finish_(queue_);
            if (e != 0) { err = "OpenCL fast enqueue failed"; return false; }
        }
        return true;
    }

    bool ensure(cl_mem& buf, size_t& cap, size_t need, cl_bitfield flags)
    {
        if (need == 0) need = 4;
        if (buf && cap >= need) return true;
        if (buf) Release_(buf);
        cl_int e = 0;
        buf = CreateBuffer_(ctx_, flags, need, nullptr, &e);
        cap = buf ? need : 0;
        return buf != nullptr;
    }

    HMODULE dll_ = nullptr;
    cl_device_id dev_ = nullptr;
    cl_context ctx_ = nullptr;
    cl_command_queue queue_ = nullptr;
    cl_program prog_ = nullptr;
    cl_kernel kernel_ = nullptr, fastKernel_ = nullptr;
    cl_mem fInfo_ = nullptr, fScratch_ = nullptr, fBits_ = nullptr, fJl_ = nullptr, fJf_ = nullptr;
    size_t fInfoCap_ = 0, fScratchCap_ = 0, fBitsCap_ = 0, fJlCap_ = 0, fJfCap_ = 0;
    cl_mem inBuf_ = nullptr, outBuf_ = nullptr, jobBuf_ = nullptr;
    size_t inCap_ = 0, outCap_ = 0, jobCap_ = 0;
    std::string devName_;

    PFN_GetPlatformIDs GetPlatformIDs_ = nullptr;
    PFN_GetDeviceIDs GetDeviceIDs_ = nullptr;
    PFN_GetDeviceInfo GetDeviceInfo_ = nullptr;
    PFN_CreateContext CreateContext_ = nullptr;
    PFN_CreateCommandQueue CreateCommandQueue_ = nullptr;
    PFN_CreateProgramWithSource CreateProgramWithSource_ = nullptr;
    PFN_BuildProgram BuildProgram_ = nullptr;
    PFN_GetProgramBuildInfo GetProgramBuildInfo_ = nullptr;
    PFN_CreateKernel CreateKernel_ = nullptr;
    PFN_CreateBuffer CreateBuffer_ = nullptr;
    PFN_SetKernelArg SetKernelArg_ = nullptr;
    PFN_EnqueueNDRangeKernel EnqueueNDRangeKernel_ = nullptr;
    PFN_EnqueueWriteBuffer EnqueueWriteBuffer_ = nullptr;
    PFN_EnqueueReadBuffer EnqueueReadBuffer_ = nullptr;
    PFN_Finish Finish_ = nullptr;
    cl_int(CLAPI* Release_)(cl_mem) = nullptr;
    cl_int(CLAPI* ReleaseKernel_)(cl_kernel) = nullptr;
    cl_int(CLAPI* ReleaseProgram_)(cl_program) = nullptr;
    cl_int(CLAPI* ReleaseQueue_)(cl_command_queue) = nullptr;
    cl_int(CLAPI* ReleaseContext_)(cl_context) = nullptr;
};

} // namespace

std::unique_ptr<Slicer> makeGpuSlicer(std::string& err)
{
    auto s = std::make_unique<GpuSlicer>();
    if (!s->init(err)) return nullptr;
    return s;
}

bool gpuDeviceInfo(std::string& name, uint64_t& memMB)
{
    HMODULE dll = LoadLibraryA("OpenCL.dll");
    if (!dll) return false;
    auto getPlatforms = (PFN_GetPlatformIDs)(void*)GetProcAddress(dll, "clGetPlatformIDs");
    auto getDevices = (PFN_GetDeviceIDs)(void*)GetProcAddress(dll, "clGetDeviceIDs");
    auto getInfo = (PFN_GetDeviceInfo)(void*)GetProcAddress(dll, "clGetDeviceInfo");
    bool ok = false;
    cl_platform_id plats[8];
    cl_uint np = 0;
    if (getPlatforms && getDevices && getInfo && getPlatforms(8, plats, &np) == 0)
        for (cl_uint i = 0; i < np && !ok; i++) {
            cl_device_id dev = nullptr;
            cl_uint nd = 0;
            if (getDevices(plats[i], CL_DEVICE_TYPE_GPU, 1, &dev, &nd) != 0 || nd == 0) continue;
            char nm[256] = {};
            cl_ulong mem = 0;
            getInfo(dev, CL_DEVICE_NAME, sizeof nm - 1, nm, nullptr);
            getInfo(dev, 0x101F /* CL_DEVICE_GLOBAL_MEM_SIZE */, sizeof mem, &mem, nullptr);
            name = nm;
            memMB = mem >> 20;
            ok = true;
        }
    FreeLibrary(dll);
    return ok;
}
