// OpenCL без SDK: OpenCL.dll загружается при первом обращении; нет видеокарты — available() == false.
#pragma once
#include <string>
#include <vector>
#include <cstddef>

namespace gpu {
bool available();                 // есть устройство GPU с OpenCL
std::string device_name();
size_t local_mem();               // байт локальной памяти рабочей группы
size_t max_group();               // потоков в группе (NVIDIA 1024, AMD и Intel 256)
struct Device { std::string name, vendor; size_t mem_mb = 0, max_alloc_mb = 0, max_group = 256; unsigned vendor_id = 0; int units = 0; bool integrated = false; };
size_t max_alloc();               // байт в одном буфере (видеокарта может меньше, чем у неё памяти)
std::vector<Device> devices();    // все видеокарты с OpenCL (любого производителя)
void use(int index);              // индекс в devices(); -1 — считать на процессоре; -2 — лучшая видеокарта
int chosen_device();
int best_device();

struct Buffer {
    void *mem = nullptr; size_t size = 0;
    Buffer() {}
    Buffer(size_t n, const void *host = nullptr, bool read_only = false);
    ~Buffer();
    Buffer(const Buffer &) = delete; Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&o) noexcept : mem(o.mem), size(o.size) { o.mem = nullptr; o.size = 0; }
    Buffer &operator=(Buffer &&o) noexcept;
    void write(const void *p, size_t n);
    void read(void *p, size_t n) const;
};

struct Kernel {
    void *prog = nullptr, *k = nullptr;
    Kernel(const std::string &src, const char *name, const std::string &opts = "");   // бросает runtime_error с журналом сборки
    ~Kernel();
    Kernel(const Kernel &) = delete; Kernel &operator=(const Kernel &) = delete;
    void arg(int i, const Buffer &b);
    void arg(int i, int v);
    void arg(int i, float v);
    void run(size_t global, size_t local);        // ждёт завершения
    size_t group_limit() const;                    // сколько потоков в группе допускает это ядро на этой видеокарте
};
}
