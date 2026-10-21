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
#define JTES_SRC(...) static const char* kJtesSrc = #__VA_ARGS__;
#include "jtes_core.h"
#undef JTES_SRC

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

const char* kJtesKernelSrc =
    "__kernel void jtes_decode_kernel(__global const unsigned char* lines, int n, __global const int* jl,\n"
    "                                 __global const float* jf, int njobs, __global ulong* back,\n"
    "                                 __global unsigned char* word, __global unsigned char* bits,\n"
    "                                 __global float* cost, __global int* ec)\n"
    "{\n"
    "    int g = get_global_id(0);\n"
    "    if (g >= njobs) return;\n"
    "    jt_decode(lines + (size_t)jl[g] * n, n, jf[g * 18], jf[g * 18 + 1], jf + (size_t)g * 18 + 2, back + (size_t)g * 299,\n"
    "              word + (size_t)g * 34, bits + (size_t)g * 37, cost + g, ec + g);\n"
    "}\n"
    "__kernel void jtes_cand_kernel(__global const unsigned char* lines, int n, __global const int* jl, int nl,\n"
    "                               __global const float* pred, __global float* costs, __global float* st,\n"
    "                               __global float* spb, __global int* cnt, __global float* fit)\n"
    "{\n"
    "    int g = get_global_id(0);\n"
    "    if (g >= nl) return;\n"
    "    jt_cand(lines + (size_t)jl[g] * n, n, pred, costs + (size_t)g * 1128, st + (size_t)g * 8, spb + (size_t)g * 8, cnt + g, fit + g);\n"
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
        for (cl_mem m : {jLines_, jJl_, jJf_, jBack_, jWord_, jBits_, jCost_, jEc_, jPred_, jCosts_, jSt_, jSpb_, jCnt_, jFit_})
            if (m) Release_(m);
        if (jDecK_) ReleaseKernel_(jDecK_);
        if (jCandK_) ReleaseKernel_(jCandK_);
        if (jProg_) ReleaseProgram_(jProg_);
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

    // ---- Japanese teletext (jtes_core.h): its own program, built at the first use
    bool jtesBuild(std::string& err)
    {
        if (jDecK_) return true;
        if (jFailed_) { err = jErr_; return false; }
        std::string src = "#define GLOBAL __global\n#define FN\n#define JT_CONST __constant\n#define JT_U64 ulong\n";
        src += kJtesSrc;
        src += "\n";
        src += kJtesKernelSrc;
        const char* sp = src.c_str();
        cl_int e = 0;
        jProg_ = CreateProgramWithSource_(ctx_, 1, &sp, nullptr, &e);
        if (!jProg_ || BuildProgram_(jProg_, 1, &dev_, "", nullptr, nullptr) != 0) {
            std::string log;
            if (jProg_) {
                size_t len = 0;
                GetProgramBuildInfo_(jProg_, dev_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &len);
                log.assign(len + 1, '\0');
                GetProgramBuildInfo_(jProg_, dev_, CL_PROGRAM_BUILD_LOG, len, &log[0], nullptr);
            }
            jFailed_ = true;
            jErr_ = err = "OpenCL build of the Japanese teletext receiver failed: " + log.substr(0, 2000);
            return false;
        }
        jDecK_ = CreateKernel_(jProg_, "jtes_decode_kernel", &e);
        jCandK_ = CreateKernel_(jProg_, "jtes_cand_kernel", &e);
        if (!jDecK_ || !jCandK_) { jFailed_ = true; jErr_ = err = "clCreateKernel (Japanese teletext) failed"; return false; }
        return true;
    }
    bool jtesLines(const uint8_t* lines, int64_t nLines, int n, std::string& err) override
    {
        if (!jtesBuild(err)) return false;
        size_t sz = (size_t)nLines * n;
        if (!ensure(jLines_, jLinesCap_, sz, CL_MEM_READ_ONLY)) { err = "clCreateBuffer failed"; return false; }
        if (sz && EnqueueWriteBuffer_(queue_, jLines_, CL_TRUE, 0, sz, lines, 0, nullptr, nullptr) != 0) { err = "OpenCL upload failed"; return false; }
        jN_ = n;
        return true;
    }
    bool jtesDecode(const std::vector<JtesJob>& jobs, std::vector<JtesOut>& out, std::string& err) override
    {
        out.assign(jobs.size(), JtesOut());
        if (!jtesBuild(err)) return false;
        const size_t batch = 65536;
        std::vector<int> jl;
        std::vector<float> jf;
        std::vector<uint8_t> word, bits;
        std::vector<float> cost;
        std::vector<int> ec;
        for (size_t b0 = 0; b0 < jobs.size(); b0 += batch) {
            size_t m = std::min(batch, jobs.size() - b0);
            jl.resize(m); jf.resize(m * 18);
            for (size_t i = 0; i < m; i++) {
                const JtesJob& j = jobs[b0 + i];
                jl[i] = j.line;
                jf[i * 18] = j.st; jf[i * 18 + 1] = j.spb;
                for (int k = 0; k < 16; k++) jf[i * 18 + 2 + k] = j.h[k];
            }
            if (!ensure(jJl_, jJlCap_, m * sizeof(int), CL_MEM_READ_ONLY) || !ensure(jJf_, jJfCap_, m * 18 * sizeof(float), CL_MEM_READ_ONLY) ||
                !ensure(jBack_, jBackCap_, m * 299 * 8, CL_MEM_READ_WRITE) || !ensure(jWord_, jWordCap_, m * 34, CL_MEM_WRITE_ONLY) ||
                !ensure(jBits_, jBitsCap_, m * 37, CL_MEM_WRITE_ONLY) || !ensure(jCost_, jCostCap_, m * sizeof(float), CL_MEM_WRITE_ONLY) ||
                !ensure(jEc_, jEcCap_, m * sizeof(int), CL_MEM_WRITE_ONLY)) { err = "clCreateBuffer failed"; return false; }
            cl_int e = EnqueueWriteBuffer_(queue_, jJl_, CL_TRUE, 0, m * sizeof(int), jl.data(), 0, nullptr, nullptr);
            e |= EnqueueWriteBuffer_(queue_, jJf_, CL_TRUE, 0, m * 18 * sizeof(float), jf.data(), 0, nullptr, nullptr);
            int nj = (int)m, a = 0;
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jLines_);
            SetKernelArg_(jDecK_, a++, sizeof(int), &jN_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jJl_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jJf_);
            SetKernelArg_(jDecK_, a++, sizeof(int), &nj);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jBack_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jWord_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jBits_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jCost_);
            SetKernelArg_(jDecK_, a++, sizeof(cl_mem), &jEc_);
            size_t local = 64, global = (m + local - 1) / local * local;
            e |= EnqueueNDRangeKernel_(queue_, jDecK_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
            word.resize(m * 34); bits.resize(m * 37); cost.resize(m); ec.resize(m);
            e |= EnqueueReadBuffer_(queue_, jWord_, CL_TRUE, 0, m * 34, word.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jBits_, CL_TRUE, 0, m * 37, bits.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jCost_, CL_TRUE, 0, m * sizeof(float), cost.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jEc_, CL_TRUE, 0, m * sizeof(int), ec.data(), 0, nullptr, nullptr);
            Finish_(queue_);
            if (e != 0) { err = "OpenCL Japanese teletext decode failed"; return false; }
            for (size_t i = 0; i < m; i++) {
                JtesOut& o = out[b0 + i];
                o.cost = cost[i]; o.ec = ec[i];
                memcpy(o.word, &word[i * 34], 34);
                memcpy(o.bits, &bits[i * 37], 37);
            }
        }
        return true;
    }
    bool jtesCand(const std::vector<int>& lines, const float* pred, std::vector<JtesCand>& out, std::string& err) override
    {
        out.assign(lines.size(), JtesCand());
        if (!jtesBuild(err)) return false;
        if (!ensure(jPred_, jPredCap_, 312 * sizeof(float), CL_MEM_READ_ONLY)) { err = "clCreateBuffer failed"; return false; }
        if (EnqueueWriteBuffer_(queue_, jPred_, CL_TRUE, 0, 312 * sizeof(float), pred, 0, nullptr, nullptr) != 0) { err = "OpenCL upload failed"; return false; }
        const size_t batch = 32768;
        std::vector<float> st, spb, fit;
        std::vector<int> cnt;
        for (size_t b0 = 0; b0 < lines.size(); b0 += batch) {
            size_t m = std::min(batch, lines.size() - b0);
            if (!ensure(jJl_, jJlCap_, m * sizeof(int), CL_MEM_READ_ONLY) || !ensure(jCosts_, jCostsCap_, m * 1128 * sizeof(float), CL_MEM_READ_WRITE) ||
                !ensure(jSt_, jStCap_, m * 8 * sizeof(float), CL_MEM_WRITE_ONLY) || !ensure(jSpb_, jSpbCap_, m * 8 * sizeof(float), CL_MEM_WRITE_ONLY) ||
                !ensure(jCnt_, jCntCap_, m * sizeof(int), CL_MEM_WRITE_ONLY) || !ensure(jFit_, jFitCap_, m * sizeof(float), CL_MEM_WRITE_ONLY)) {
                err = "clCreateBuffer failed";
                return false;
            }
            cl_int e = EnqueueWriteBuffer_(queue_, jJl_, CL_TRUE, 0, m * sizeof(int), &lines[b0], 0, nullptr, nullptr);
            int nl = (int)m, a = 0;
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jLines_);
            SetKernelArg_(jCandK_, a++, sizeof(int), &jN_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jJl_);
            SetKernelArg_(jCandK_, a++, sizeof(int), &nl);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jPred_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jCosts_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jSt_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jSpb_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jCnt_);
            SetKernelArg_(jCandK_, a++, sizeof(cl_mem), &jFit_);
            size_t local = 64, global = (m + local - 1) / local * local;
            e |= EnqueueNDRangeKernel_(queue_, jCandK_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
            st.resize(m * 8); spb.resize(m * 8); cnt.resize(m); fit.resize(m);
            e |= EnqueueReadBuffer_(queue_, jSt_, CL_TRUE, 0, m * 8 * sizeof(float), st.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jSpb_, CL_TRUE, 0, m * 8 * sizeof(float), spb.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jCnt_, CL_TRUE, 0, m * sizeof(int), cnt.data(), 0, nullptr, nullptr);
            e |= EnqueueReadBuffer_(queue_, jFit_, CL_TRUE, 0, m * sizeof(float), fit.data(), 0, nullptr, nullptr);
            Finish_(queue_);
            if (e != 0) { err = "OpenCL Japanese teletext start search failed"; return false; }
            for (size_t i = 0; i < m; i++) {
                JtesCand& c = out[b0 + i];
                c.n = cnt[i]; c.fit = fit[i];
                memcpy(c.st, &st[i * 8], sizeof c.st);
                memcpy(c.spb, &spb[i * 8], sizeof c.spb);
            }
        }
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
    cl_program jProg_ = nullptr;
    cl_kernel jDecK_ = nullptr, jCandK_ = nullptr;
    bool jFailed_ = false;
    std::string jErr_;
    int jN_ = 0;
    cl_mem jLines_ = nullptr, jJl_ = nullptr, jJf_ = nullptr, jBack_ = nullptr, jWord_ = nullptr, jBits_ = nullptr, jCost_ = nullptr,
           jEc_ = nullptr, jPred_ = nullptr, jCosts_ = nullptr, jSt_ = nullptr, jSpb_ = nullptr, jCnt_ = nullptr, jFit_ = nullptr;
    size_t jLinesCap_ = 0, jJlCap_ = 0, jJfCap_ = 0, jBackCap_ = 0, jWordCap_ = 0, jBitsCap_ = 0, jCostCap_ = 0, jEcCap_ = 0,
           jPredCap_ = 0, jCostsCap_ = 0, jStCap_ = 0, jSpbCap_ = 0, jCntCap_ = 0, jFitCap_ = 0;

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
