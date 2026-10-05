#include "flac.h"
#include "vbidecode.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace {
// чтение битов, старший бит байта первым
struct Bits {
    const u8 *p; size_t n, pos = 0;                  // pos — в битах
    Bits(const u8 *p_, size_t n_, size_t byte_pos) : p(p_), n(n_), pos(byte_pos * 8) {}
    bool left(size_t k) const { return pos + k <= n * 8; }
    uint32_t get(int k) {                            // k ≤ 32
        if (!left(k)) throw std::runtime_error("FLAC: unexpected end of data");
        uint64_t v = 0;
        for (int i = 0; i < k;) {
            size_t b = pos >> 3; int off = pos & 7, take = std::min(8 - off, k - i);
            v = (v << take) | ((p[b] >> (8 - off - take)) & ((1u << take) - 1));
            pos += take; i += take;
        }
        return (uint32_t)v;
    }
    int32_t sget(int k) { if (k == 0) return 0; uint32_t v = get(k); return (int32_t)(v << (32 - k)) >> (32 - k); }
    uint32_t unary() {                               // нули до единицы
        uint32_t c = 0;
        for (;;) {
            if (!left(1)) throw std::runtime_error("FLAC: unexpected end of data");
            size_t b = pos >> 3; int off = pos & 7;
            u8 x = (u8)(p[b] << off);
            if (x == 0) { c += 8 - off; pos += 8 - off; continue; }
            int z = __builtin_clz((unsigned)x) - 24;
            c += z; pos += z + 1; return c;
        }
    }
    void align() { pos = (pos + 7) & ~(size_t)7; }
};
u8 crc8(const u8 *d, size_t n) {
    u8 c = 0;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c & 0x80) ? (u8)((c << 1) ^ 0x07) : (u8)(c << 1); }
    return c;
}
uint16_t crc16(const u8 *d, size_t n) {
    static uint16_t T[256]; static bool init = false;
    if (!init) { for (int i = 0; i < 256; i++) { uint16_t c = (uint16_t)(i << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1); T[i] = c; } init = true; }
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ T[((c >> 8) ^ d[i]) & 0xFF]);
    return c;
}
struct Info { uint32_t min_block = 0, max_block = 0, rate = 0, channels = 0, bps = 0; uint64_t total = 0; size_t first_frame = 0; };

Info read_info(const u8 *d, size_t n) {
    if (n < 42 || memcmp(d, "fLaC", 4)) throw std::runtime_error("not a FLAC file");
    Info I; size_t p = 4; bool last = false, got = false;
    while (!last) {
        if (p + 4 > n) throw std::runtime_error("FLAC: broken metadata");
        last = d[p] & 0x80; int type = d[p] & 0x7F; size_t len = ((size_t)d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
        p += 4;
        if (type == 0 && len >= 34) {
            Bits b(d, n, p);
            I.min_block = b.get(16); I.max_block = b.get(16); b.get(24); b.get(24);
            I.rate = b.get(20); I.channels = b.get(3) + 1; I.bps = b.get(5) + 1;
            I.total = ((uint64_t)b.get(4) << 32) | b.get(32);
            got = true;
        }
        p += len;
    }
    if (!got) throw std::runtime_error("FLAC: no STREAMINFO");
    I.first_frame = p;
    return I;
}

struct Hdr { uint32_t block = 0; uint64_t first_sample = 0; size_t start = 0, data_pos = 0; };
// заголовок кадра в позиции p; false — здесь не кадр (неверная метка или CRC)
bool parse_header(const u8 *d, size_t n, size_t p, const Info &I, Hdr &h) {
    if (p + 16 > n || d[p] != 0xFF || (d[p + 1] & 0xFE) != 0xF8) return false;
    bool variable = d[p + 1] & 1;
    int bs = d[p + 2] >> 4, sr = d[p + 2] & 15, ch = d[p + 3] >> 4, ss = (d[p + 3] >> 1) & 7;
    if (bs == 0 || sr == 15 || ch != 0 || ss == 3 || ss == 7 || (d[p + 3] & 1)) return false;    // только один канал
    size_t q = p + 4;
    // число в коде UTF-8 (номер кадра или отсчёта)
    u8 c0 = d[q++]; int extra = 0; uint64_t v;
    if (c0 < 0x80) v = c0;
    else if ((c0 & 0xE0) == 0xC0) { v = c0 & 0x1F; extra = 1; }
    else if ((c0 & 0xF0) == 0xE0) { v = c0 & 0x0F; extra = 2; }
    else if ((c0 & 0xF8) == 0xF0) { v = c0 & 0x07; extra = 3; }
    else if ((c0 & 0xFC) == 0xF8) { v = c0 & 0x03; extra = 4; }
    else if ((c0 & 0xFE) == 0xFC) { v = c0 & 0x01; extra = 5; }
    else if (c0 == 0xFE) { v = 0; extra = 6; }
    else return false;
    for (int i = 0; i < extra; i++) { if ((d[q] & 0xC0) != 0x80) return false; v = (v << 6) | (d[q++] & 0x3F); }
    uint32_t block;
    if (bs == 1) block = 192;
    else if (bs <= 5) block = 576u << (bs - 2);
    else if (bs == 6) block = d[q++] + 1u;
    else if (bs == 7) { block = ((uint32_t)d[q] << 8 | d[q + 1]) + 1u; q += 2; }
    else block = 256u << (bs - 8);
    if (sr == 12) q += 1; else if (sr == 13 || sr == 14) q += 2;
    if (q >= n || crc8(d + p, q - p) != d[q]) return false;
    h.block = block;
    h.first_sample = variable ? v : v * (I.min_block == I.max_block ? I.max_block : block);
    h.data_pos = q + 1; h.start = p;
    return true;
}

// один кадр: отсчёты в out; -> позиция байта после кадра (после CRC-16)
size_t decode_frame(const u8 *d, size_t n, const Hdr &h, int bps, std::vector<int32_t> &s) {
    Bits b(d, n, h.data_pos);
    uint32_t N = h.block;
    s.assign(N, 0);
    if (b.get(1) != 0) throw std::runtime_error("FLAC: bad subframe");
    int type = b.get(6);
    int wasted = 0;
    if (b.get(1)) wasted = b.unary() + 1;
    int sb = bps - wasted;
    auto residual = [&](int order) {
        int method = b.get(2);
        if (method > 1) throw std::runtime_error("FLAC: unknown residual coding");
        int pbits = method ? 5 : 4, esc = method ? 31 : 15;
        int porder = b.get(4);
        uint32_t parts = 1u << porder, i = order;
        for (uint32_t pt = 0; pt < parts; pt++) {
            uint32_t cnt = (N >> porder) - (pt == 0 ? order : 0);
            int k = b.get(pbits);
            if (k == esc) { int raw = b.get(5); for (uint32_t j = 0; j < cnt; j++) s[i++] = b.sget(raw); }
            else for (uint32_t j = 0; j < cnt; j++) {
                uint32_t u = (b.unary() << k) | (k ? b.get(k) : 0);
                s[i++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
    };
    if (type == 0) { int32_t v = b.sget(sb); for (auto &x : s) x = v; }
    else if (type == 1) { for (auto &x : s) x = b.sget(sb); }
    else if (type >= 8 && type <= 12) {
        int order = type - 8;
        for (int i = 0; i < order; i++) s[i] = b.sget(sb);
        residual(order);
        for (uint32_t i = order; i < N; i++) {
            int64_t pr = 0;
            switch (order) {
            case 1: pr = s[i - 1]; break;
            case 2: pr = 2LL * s[i - 1] - s[i - 2]; break;
            case 3: pr = 3LL * s[i - 1] - 3LL * s[i - 2] + s[i - 3]; break;
            case 4: pr = 4LL * s[i - 1] - 6LL * s[i - 2] + 4LL * s[i - 3] - s[i - 4]; break;
            }
            s[i] += (int32_t)pr;
        }
    } else if (type >= 32) {
        int order = (type & 31) + 1;
        for (int i = 0; i < order; i++) s[i] = b.sget(sb);
        int prec = b.get(4) + 1;
        if (prec == 16) throw std::runtime_error("FLAC: bad LPC precision");
        int shift = b.sget(5);
        int32_t coef[32];
        for (int i = 0; i < order; i++) coef[i] = b.sget(prec);
        residual(order);
        for (uint32_t i = order; i < N; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)coef[j] * s[i - 1 - j];
            s[i] += (int32_t)(shift >= 0 ? sum >> shift : sum << -shift);
        }
    } else throw std::runtime_error("FLAC: reserved subframe type");
    if (wasted) for (auto &x : s) x = (int32_t)((uint32_t)x << wasted);
    b.align();
    size_t e = b.pos >> 3;                           // CRC-16 всего кадра
    if (e + 2 > n || crc16(d + h.start, e - h.start) != (uint16_t)((d[e] << 8) | d[e + 1])) throw std::runtime_error("FLAC: frame CRC mismatch");
    return e + 2;
}
}

bool is_flac(const std::string &path) {
    FILE *f = ufopen(path, "rb");
    if (!f) return false;
    char m[4] = {0}; size_t k = fread(m, 1, 4, f); fclose(f);
    return k == 4 && !memcmp(m, "fLaC", 4);
}

uint64_t flac_to_raw(const std::string &in, const std::string &out, Progress &pr) {
    MappedFile mf(in);
    const u8 *d = mf.data(); size_t n = (size_t)mf.size();
    Info I = read_info(d, n);
    if (I.channels != 1) throw std::runtime_error(fmt("FLAC: %u channels \xE2\x80\x94 a VBI recording in FLAC has one", I.channels));
    if (I.bps != 8 && I.bps != 16) throw std::runtime_error(fmt("FLAC: %u bits per sample \xE2\x80\x94 expected 8 or 16", I.bps));
    int bpsmp = I.bps / 8;
    uint64_t total = I.total;
    // куски файла по потокам: каждый начинает с первого настоящего кадра в своём куске
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    size_t span = n - I.first_frame, chunk = std::max<size_t>(1 << 20, span / (nt * 4) + 1);
    std::vector<size_t> starts;
    for (size_t c = I.first_frame; c < n; c += chunk) starts.push_back(c);
    size_t nchunks = starts.size();
    std::vector<size_t> first(nchunks, SIZE_MAX);
    std::vector<uint64_t> fsamp(nchunks, 0);
    parallel_for(nchunks, [&](size_t i) {
        Hdr h;
        for (size_t p = starts[i]; p + 16 < n && p < starts[i] + chunk; p++)
            if (d[p] == 0xFF && parse_header(d, n, p, I, h)) {
                try { std::vector<int32_t> s; decode_frame(d, n, h, I.bps, s); } catch (...) { continue; }   // не кадр
                first[i] = p; fsamp[i] = h.first_sample; return;
            }
    });
    // запись: файл заданного размера, каждый поток пишет свои кадры на их место
    if (total == 0) {                                // длина не указана — по последнему кадру
        for (size_t i = nchunks; i-- > 0;) if (first[i] != SIZE_MAX) {
            size_t p = first[i]; Hdr h; std::vector<int32_t> s;
            while (p < n && parse_header(d, n, p, I, h)) { total = h.first_sample + h.block; p = decode_frame(d, n, h, I.bps, s); }
            break;
        }
    }
    uint64_t bytes = total * bpsmp;
    {
        FILE *f = ufopen(out, "wb");
        if (!f) throw std::runtime_error("cannot write " + out);
        if (bytes) { fseek64(f, (int64_t)bytes - 1, SEEK_SET); fputc(0, f); }
        fclose(f);
    }
    std::atomic<uint64_t> done{0};
    std::mutex fmu;
    std::string err;
    parallel_for(nchunks, [&](size_t i) {
        if (first[i] == SIZE_MAX) return;
        size_t end = SIZE_MAX;                       // до начала следующего куска с кадром
        for (size_t j = i + 1; j < nchunks; j++) if (first[j] != SIZE_MAX) { end = first[j]; break; }
        FILE *f = ufopen(out, "r+b");
        if (!f) return;
        std::vector<int32_t> s; std::vector<u8> buf;
        size_t p = first[i]; Hdr h;
        try {
            while (p < n && p < end) {
                if (!parse_header(d, n, p, I, h)) {
                    size_t q = p + 1; while (q + 16 < n && q < end && !(d[q] == 0xFF && parse_header(d, n, q, I, h))) q++;
                    if (q + 16 >= n || q >= end) break;
                    p = q;
                }
                size_t next = decode_frame(d, n, h, I.bps, s);
                uint32_t cnt = (uint32_t)std::min<uint64_t>(h.block, total > h.first_sample ? total - h.first_sample : 0);
                buf.resize((size_t)cnt * bpsmp);
                if (bpsmp == 1) for (uint32_t k = 0; k < cnt; k++) buf[k] = (u8)(s[k] + 128);
                else for (uint32_t k = 0; k < cnt; k++) { uint16_t v = (uint16_t)(s[k] + 32768); buf[2 * k] = (u8)v; buf[2 * k + 1] = (u8)(v >> 8); }
                fseek64(f, (int64_t)(h.first_sample * bpsmp), SEEK_SET);
                fwrite(buf.data(), 1, buf.size(), f);
                uint64_t dn = done += cnt;
                if ((dn / cnt) % 512 == 0) pr.progress((long long)(dn >> 10), (long long)(total >> 10), "unpacking FLAC");
                p = next;
            }
        } catch (std::exception &e) { std::lock_guard<std::mutex> lk(fmu); if (err.empty()) err = e.what(); }
        fclose(f);
    });
    if (!err.empty()) throw std::runtime_error(err);
    pr.log(fmt("FLAC unpacked: %llu bytes (%u-bit samples)", (unsigned long long)bytes, I.bps));
    return bytes;
}
