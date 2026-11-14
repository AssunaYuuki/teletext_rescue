#include "opencl.h"
#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#define CL_API __stdcall
#else
#include <dlfcn.h>
#define CL_API
#endif
#include <mutex>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <tuple>
#include <cstring>
#include <cmath>
#include <vector>

namespace {
typedef int32_t cl_int; typedef uint32_t cl_uint; typedef uint64_t cl_ulong; typedef uint64_t cl_bitfield;
typedef void *cl_platform_id, *cl_device_id, *cl_context, *cl_command_queue, *cl_mem, *cl_program, *cl_kernel, *cl_event;
const cl_bitfield CL_DEVICE_TYPE_GPU = 1 << 2;
const cl_uint CL_DEVICE_NAME = 0x102B, CL_DEVICE_LOCAL_MEM_SIZE = 0x1023, CL_PROGRAM_BUILD_LOG = 0x1183;
const cl_uint CL_DEVICE_VENDOR = 0x102C, CL_DEVICE_MAX_COMPUTE_UNITS = 0x1002, CL_DEVICE_MAX_WORK_GROUP_SIZE = 0x1004,
              CL_DEVICE_GLOBAL_MEM_SIZE = 0x101F, CL_DEVICE_HOST_UNIFIED_MEMORY = 0x1035, CL_KERNEL_WORK_GROUP_SIZE = 0x11B0,
              CL_DEVICE_VENDOR_ID = 0x1001, CL_DEVICE_MAX_MEM_ALLOC_SIZE = 0x1010;
const cl_bitfield CL_MEM_READ_WRITE = 1, CL_MEM_READ_ONLY = 4, CL_MEM_COPY_HOST_PTR = 32;

#define FN(ret, name, args) typedef ret(CL_API *name##_t) args; name##_t name;
struct Api {
    FN(cl_int, clGetPlatformIDs, (cl_uint, cl_platform_id *, cl_uint *))
    FN(cl_int, clGetDeviceIDs, (cl_platform_id, cl_bitfield, cl_uint, cl_device_id *, cl_uint *))
    FN(cl_int, clGetDeviceInfo, (cl_device_id, cl_uint, size_t, void *, size_t *))
    FN(cl_context, clCreateContext, (const intptr_t *, cl_uint, const cl_device_id *, void *, void *, cl_int *))
    FN(cl_command_queue, clCreateCommandQueue, (cl_context, cl_device_id, cl_bitfield, cl_int *))
    FN(cl_program, clCreateProgramWithSource, (cl_context, cl_uint, const char **, const size_t *, cl_int *))
    FN(cl_int, clBuildProgram, (cl_program, cl_uint, const cl_device_id *, const char *, void *, void *))
    FN(cl_int, clGetProgramBuildInfo, (cl_program, cl_device_id, cl_uint, size_t, void *, size_t *))
    FN(cl_kernel, clCreateKernel, (cl_program, const char *, cl_int *))
    FN(cl_mem, clCreateBuffer, (cl_context, cl_bitfield, size_t, void *, cl_int *))
    FN(cl_int, clSetKernelArg, (cl_kernel, cl_uint, size_t, const void *))
    FN(cl_int, clEnqueueNDRangeKernel, (cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *))
    FN(cl_int, clEnqueueReadBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *))
    FN(cl_int, clEnqueueWriteBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *))
    FN(cl_int, clFinish, (cl_command_queue))
    FN(cl_int, clReleaseMemObject, (cl_mem))
    FN(cl_int, clReleaseKernel, (cl_kernel))
    FN(cl_int, clReleaseProgram, (cl_program))
    FN(cl_int, clReleaseCommandQueue, (cl_command_queue))
    FN(cl_int, clReleaseContext, (cl_context))
    FN(cl_int, clGetKernelWorkGroupInfo, (cl_kernel, cl_device_id, cl_uint, size_t, void *, size_t *))
    // for the cache of built programs (optional: without them every program is built from source)
    FN(cl_int, clGetProgramInfo, (cl_program, cl_uint, size_t, void *, size_t *))
    FN(cl_program, clCreateProgramWithBinary, (cl_context, cl_uint, const cl_device_id *, const size_t *, const unsigned char **, cl_int *, cl_int *))
} api;
#undef FN

// ---------------------------------------------------------------- built programs kept on disk
// Building a program from source costs the driver 0.5-2 s (NVIDIA: no cache of its own for OpenCL), and the
// decoder builds several a recording: the binary of each is kept, keyed by device, driver, options and source
// (%LOCALAPPDATA%\TeletextRescue\clcache, or TR_CLCACHE)
const cl_uint CL_DRIVER_VERSION_ = 0x102D, CL_PROGRAM_BINARY_SIZES_ = 0x1165, CL_PROGRAM_BINARIES_ = 0x1166;
std::string cl_cache_dir() {
    if (const char *e = getenv("TR_CLCACHE")) return e;
    const char *b = getenv("LOCALAPPDATA");
    if (!b) b = getenv("TEMP");
    return b ? std::string(b) + "/TeletextRescue/clcache" : std::string();
}
std::string cl_cache_file(cl_device_id d, const std::string &src, const std::string &opts) {
    std::string dir = cl_cache_dir();
    if (dir.empty()) return "";
    char nm[256] = {0}, dv[256] = {0};
    api.clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof nm - 1, nm, nullptr);
    api.clGetDeviceInfo(d, CL_DRIVER_VERSION_, sizeof dv - 1, dv, nullptr);
    uint64_t h = 1469598103934665603ull;
    for (const std::string &s : {std::string(nm), std::string(dv), opts, src}) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xFF; h *= 1099511628211ull;
    }
    char b[40]; snprintf(b, sizeof b, "/legacy_%016llx.bin", (unsigned long long)h);
    return dir + b;
}
cl_program cl_cached_program(cl_context ctx, cl_device_id d, const std::string &file) {
    if (file.empty() || !api.clCreateProgramWithBinary) return nullptr;
    FILE *f = fopen(file.c_str(), "rb");
    if (!f) return nullptr;
    std::vector<unsigned char> bin;
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bin.insert(bin.end(), buf, buf + n);
    fclose(f);
    if (bin.empty()) return nullptr;
    const unsigned char *p = bin.data(); size_t len = bin.size(); cl_int st = 0, e = 0;
    cl_program pr = api.clCreateProgramWithBinary(ctx, 1, &d, &len, &p, &st, &e);
    if (!pr || e || st) { if (pr) api.clReleaseProgram(pr); return nullptr; }
    if (api.clBuildProgram(pr, 1, &d, "", nullptr, nullptr) != 0) { api.clReleaseProgram(pr); return nullptr; }
    return pr;
}
void cl_keep_program(cl_program pr, const std::string &file) {
    if (file.empty() || !api.clGetProgramInfo) return;
    size_t len = 0;
    if (api.clGetProgramInfo(pr, CL_PROGRAM_BINARY_SIZES_, sizeof len, &len, nullptr) != 0 || !len) return;
    std::vector<unsigned char> bin(len);
    unsigned char *p = bin.data();
    if (api.clGetProgramInfo(pr, CL_PROGRAM_BINARIES_, sizeof p, &p, nullptr) != 0) return;
    std::string dir = file.substr(0, file.find_last_of('/'));
#ifdef _WIN32
    for (size_t i = 0; i <= dir.size(); i++)   // the folders on the way
        if (i == dir.size() || ((dir[i] == '/' || dir[i] == '\\') && i > 2)) CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
#endif
    std::string tmp = file + ".tmp" + std::to_string((uintptr_t)&bin);
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(bin.data(), 1, len, f) == len;
    fclose(f);
    if (!ok || std::rename(tmp.c_str(), file.c_str()) != 0) std::remove(tmp.c_str());
}

bool loaded = false, ok = false; std::string name; size_t lmem = 0, maxwg = 256;
std::vector<cl_device_id> devs; std::vector<gpu::Device> infos;
int chosen = -2, active = -3;                      // -2 — выбрать лучшую видеокарту, -1 — процессор
cl_device_id dev = nullptr; cl_context ctx = nullptr; cl_command_queue q = nullptr;
std::once_flag once;
std::mutex mu;   // одна очередь на программу

// видеоадаптеры, как их называет Windows: «AMD Radeon RX 560 Series», «NVIDIA GeForce RTX 3080 Ti»…
struct WinAdapter { unsigned vendor; size_t mem_mb; std::string name; };
#ifndef _WIN32
std::vector<WinAdapter> windows_adapters() { return {}; }
#else
std::vector<WinAdapter> windows_adapters() {
    std::vector<WinAdapter> out;
    HMODULE h = LoadLibraryA("dxgi.dll");
    if (!h) return out;
    typedef HRESULT(WINAPI * Create_t)(REFIID, void **);
    auto create = (Create_t)GetProcAddress(h, "CreateDXGIFactory1");
    if (!create) return out;
    IDXGIFactory1 *f = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), (void **)&f)) || !f) return out;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            char buf[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, buf, sizeof buf - 1, nullptr, nullptr);
            out.push_back({d.VendorId, (size_t)(d.DedicatedVideoMemory >> 20), buf});
        }
        a->Release();
    }
    f->Release();
    return out;
}
#endif
std::string clean_name(std::string s) {
    for (const char *junk : {"(R)", "(r)", "(TM)", "(tm)", "(C)"}) for (size_t k; (k = s.find(junk)) != std::string::npos;) s.erase(k, strlen(junk));
    std::string o; for (char c : s) { if (c == ' ' && (o.empty() || o.back() == ' ')) continue; o += c; }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// OpenCL.dll и список видеокарт всех производителей (NVIDIA, AMD, Intel)
void load() {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return;
#define LOAD(n) api.n = (decltype(api.n))GetProcAddress(h, #n); if (!api.n) return;
#else
    void *h = nullptr;
#ifdef __APPLE__
    for (const char *nm : {"/System/Library/Frameworks/OpenCL.framework/OpenCL"}) if (!h) h = dlopen(nm, RTLD_NOW);
#else
    for (const char *nm : {"libOpenCL.so.1", "libOpenCL.so"}) if (!h) h = dlopen(nm, RTLD_NOW);
#endif
    if (!h) return;
#define LOAD(n) api.n = (decltype(api.n))dlsym(h, #n); if (!api.n) return;
#endif
    LOAD(clGetPlatformIDs) LOAD(clGetDeviceIDs) LOAD(clGetDeviceInfo) LOAD(clCreateContext) LOAD(clCreateCommandQueue)
    LOAD(clCreateProgramWithSource) LOAD(clBuildProgram) LOAD(clGetProgramBuildInfo) LOAD(clCreateKernel) LOAD(clCreateBuffer)
    LOAD(clSetKernelArg) LOAD(clEnqueueNDRangeKernel) LOAD(clEnqueueReadBuffer) LOAD(clEnqueueWriteBuffer) LOAD(clFinish)
    LOAD(clReleaseMemObject) LOAD(clReleaseKernel) LOAD(clReleaseProgram) LOAD(clReleaseCommandQueue) LOAD(clReleaseContext)
    LOAD(clGetKernelWorkGroupInfo)
#undef LOAD
#ifdef _WIN32
    api.clGetProgramInfo = (decltype(api.clGetProgramInfo))GetProcAddress(h, "clGetProgramInfo");
    api.clCreateProgramWithBinary = (decltype(api.clCreateProgramWithBinary))GetProcAddress(h, "clCreateProgramWithBinary");
#else
    api.clGetProgramInfo = (decltype(api.clGetProgramInfo))dlsym(h, "clGetProgramInfo");
    api.clCreateProgramWithBinary = (decltype(api.clCreateProgramWithBinary))dlsym(h, "clCreateProgramWithBinary");
#endif
    cl_platform_id plats[16]; cl_uint np = 0;
    if (api.clGetPlatformIDs(16, plats, &np) != 0) return;
    for (cl_uint i = 0; i < np && i < 16; i++) {
        cl_device_id d[16]; cl_uint nd = 0;
        if (api.clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 16, d, &nd) != 0) continue;
        for (cl_uint k = 0; k < nd && k < 16; k++) {
            gpu::Device g;
            char buf[256] = {0};
            api.clGetDeviceInfo(d[k], CL_DEVICE_NAME, sizeof buf - 1, buf, nullptr); g.name = buf;
            // AMD называет устройство кодом кристалла (Baffin, Ellesmere, gfx1030…); продажное имя — в CL_DEVICE_BOARD_NAME_AMD
            char bn[256] = {0}; size_t bl = 0;
            if (api.clGetDeviceInfo(d[k], 0x4038, sizeof bn - 1, bn, &bl) == 0 && bl > 1 && bn[0]) g.name = bn;
            char vb[256] = {0};
            api.clGetDeviceInfo(d[k], CL_DEVICE_VENDOR, sizeof vb - 1, vb, nullptr); g.vendor = vb;
            cl_ulong gm = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof gm, &gm, nullptr); g.mem_mb = (size_t)(gm >> 20);
            cl_uint cu = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof cu, &cu, nullptr); g.units = (int)cu;
            cl_uint um = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof um, &um, nullptr); g.integrated = um != 0;
            size_t wg = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof wg, &wg, nullptr); g.max_group = wg ? wg : 256;
            cl_ulong ma = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof ma, &ma, nullptr); g.max_alloc_mb = ma ? (size_t)(ma >> 20) : g.mem_mb / 4;
            cl_uint vid = 0; api.clGetDeviceInfo(d[k], CL_DEVICE_VENDOR_ID, sizeof vid, &vid, nullptr); g.vendor_id = vid;
            while (!g.name.empty() && g.name.back() == ' ') g.name.pop_back();
            devs.push_back(d[k]); infos.push_back(g);
        }
    }
    // имя как в Windows: тот же производитель и ближайший объём памяти (встроенная — с малым объёмом своей памяти)
    auto wa = windows_adapters();
    for (auto &g : infos) {
        int best = -1; double bd = 1e18;
        for (int i = 0; i < (int)wa.size(); i++) {
            if (wa[i].vendor != g.vendor_id) continue;
            double dd = std::abs((double)wa[i].mem_mb - (double)(g.integrated ? 0 : g.mem_mb));
            if (dd < bd) { bd = dd; best = i; }
        }
        if (best >= 0 && !wa[best].name.empty()) g.name = wa[best].name;
        g.name = clean_name(g.name);
    }
    loaded = true;
}
int best() {                                       // отдельная видеокарта лучше встроенной; потом — больше блоков и памяти
    int b = -1;
    if (const char *want = getenv("TR_GPU"))       // TR_GPU=<часть имени>: эта видеокарта (как в slicer_gpu.cpp pickGpu)
        for (int i = 0; i < (int)infos.size(); i++) if (*want && infos[i].name.find(want) != std::string::npos) return i;
    for (int i = 0; i < (int)infos.size(); i++) {
        auto key = [&](int j) { return std::make_tuple(!infos[j].integrated, (long long)infos[j].units * 1000 + (long long)(infos[j].mem_mb / 64)); };
        if (b < 0 || key(i) > key(b)) b = i;
    }
    return b;
}
void release() {
    if (q) api.clReleaseCommandQueue(q);
    if (ctx) api.clReleaseContext(ctx);
    q = nullptr; ctx = nullptr; dev = nullptr; ok = false; name.clear();
}
// очередь на выбранном устройстве (заново — если выбор изменился)
void init() {
    std::call_once(once, load);
    int want = chosen == -2 ? best() : chosen;
    if (want >= (int)devs.size()) want = best();
    if (want == active) return;
    release(); active = want;
    if (want < 0) return;
    dev = devs[want]; name = infos[want].name; maxwg = infos[want].max_group;
    if (const char *mg = getenv("TR_MAX_GROUP")) maxwg = std::min<size_t>(maxwg, std::max(32, atoi(mg)));   // проверка: как у AMD/Intel
    cl_ulong lm = 0; api.clGetDeviceInfo(dev, CL_DEVICE_LOCAL_MEM_SIZE, sizeof lm, &lm, nullptr); lmem = (size_t)lm;
    cl_int e = 0;
    ctx = api.clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &e);
    if (!ctx || e) { ctx = nullptr; return; }
    q = api.clCreateCommandQueue(ctx, dev, 0, &e);
    if (!q || e) { q = nullptr; return; }
    ok = true;
}
void need() { std::lock_guard<std::mutex> lk(mu); init(); if (!ok) throw std::runtime_error("no OpenCL GPU"); }
}

namespace gpu {
bool g_allowed = true;   // TeletextRescue: false when the user chose the processor
bool available() {
    if (getenv("TR_NO_GPU") || !g_allowed) return false;
    std::lock_guard<std::mutex> lk(mu); init(); return ok;
}
std::string device_name() { available(); return name; }
size_t local_mem() { available(); return lmem; }
size_t max_group() { available(); return maxwg; }
size_t max_alloc() {
    available();
    if (active < 0 || active >= (int)infos.size()) return (size_t)256 << 20;
    return (size_t)std::max<size_t>(64, infos[active].max_alloc_mb) << 20;
}
std::vector<Device> devices() { std::call_once(once, load); return infos; }
void use(int index) { std::lock_guard<std::mutex> lk(mu); chosen = index; }
int chosen_device() { return chosen; }
int best_device() { std::call_once(once, load); return best(); }

Buffer::Buffer(size_t n, const void *host, bool read_only) : size(n) {
    need();
    cl_int e = 0;
    mem = api.clCreateBuffer(ctx, (read_only ? CL_MEM_READ_ONLY : CL_MEM_READ_WRITE) | (host ? CL_MEM_COPY_HOST_PTR : 0),
                             n ? n : 4, (void *)host, &e);
    if (!mem || e) throw std::runtime_error("clCreateBuffer failed (" + std::to_string(e) + "), " + std::to_string(n) + " bytes");
}
Buffer::~Buffer() { if (mem) api.clReleaseMemObject((cl_mem)mem); }
Buffer &Buffer::operator=(Buffer &&o) noexcept {
    if (mem) api.clReleaseMemObject((cl_mem)mem);
    mem = o.mem; size = o.size; o.mem = nullptr; o.size = 0; return *this;
}
void Buffer::write(const void *p, size_t n) { std::lock_guard<std::mutex> lk(mu); api.clEnqueueWriteBuffer(q, (cl_mem)mem, 1, 0, n, p, 0, nullptr, nullptr); }
void Buffer::read(void *p, size_t n) const {
    std::lock_guard<std::mutex> lk(mu);
    cl_int e = api.clEnqueueReadBuffer(q, (cl_mem)mem, 1, 0, n, p, 0, nullptr, nullptr);
    if (e) throw std::runtime_error("clEnqueueReadBuffer failed (" + std::to_string(e) + ")");
}

Kernel::Kernel(const std::string &src, const char *kname, const std::string &opts) {
    need();
    cl_int e = 0;
    const std::string cache = cl_cache_file(dev, src, opts);
    prog = cl_cached_program(ctx, dev, cache);
    if (!prog) {
        const char *s = src.c_str(); size_t len = src.size();
        prog = api.clCreateProgramWithSource(ctx, 1, &s, &len, &e);
        if (!prog || e) throw std::runtime_error("clCreateProgramWithSource failed");
        if (api.clBuildProgram((cl_program)prog, 1, &dev, opts.c_str(), nullptr, nullptr) != 0) {
            std::string log(16384, 0); size_t n = 0;
            api.clGetProgramBuildInfo((cl_program)prog, dev, CL_PROGRAM_BUILD_LOG, log.size(), log.data(), &n);
            log.resize(n);
            throw std::runtime_error("OpenCL build failed: " + log);
        }
        cl_keep_program((cl_program)prog, cache);
    }
    k = api.clCreateKernel((cl_program)prog, kname, &e);
    if (!k || e) throw std::runtime_error(std::string("clCreateKernel failed: ") + kname);
}
Kernel::~Kernel() {
    if (k) api.clReleaseKernel((cl_kernel)k);
    if (prog) api.clReleaseProgram((cl_program)prog);
}
size_t Kernel::group_limit() const {
    size_t n = 0;
    if (api.clGetKernelWorkGroupInfo((cl_kernel)k, dev, CL_KERNEL_WORK_GROUP_SIZE, sizeof n, &n, nullptr) != 0 || !n) return maxwg;
    return n;
}
void Kernel::arg(int i, const Buffer &b) { api.clSetKernelArg((cl_kernel)k, i, sizeof(void *), &b.mem); }
void Kernel::arg(int i, int v) { api.clSetKernelArg((cl_kernel)k, i, sizeof v, &v); }
void Kernel::arg(int i, float v) { api.clSetKernelArg((cl_kernel)k, i, sizeof v, &v); }
void Kernel::run(size_t global, size_t local) {
    std::lock_guard<std::mutex> lk(mu);
    cl_int e = api.clEnqueueNDRangeKernel(q, (cl_kernel)k, 1, nullptr, &global, &local, 0, nullptr, nullptr);
    if (e) throw std::runtime_error("clEnqueueNDRangeKernel failed (" + std::to_string(e) + ")");
    api.clFinish(q);
}
}
