#include "dvbts.h"

static int rev8(int x) { int r = 0; for (int i = 0; i < 8; i++) r |= ((x >> i) & 1) << (7 - i); return r; }

// the packet size and where the first packet starts: 0x47 on 20 packets in a row (fewer in a short file)
static int ts_layout(const u8 *d, uint64_t n, uint64_t &start) {
    for (int size : {188, 192, 204}) {
        for (uint64_t off = 0; off < (uint64_t)size && off < n; off++) {
            uint64_t need = std::min<uint64_t>(20, (n - off) / size);
            if (need < 3) break;
            bool ok = true;
            for (uint64_t k = 0; k < need && ok; k++) ok = d[off + k * size] == 0x47;
            if (ok) { start = off; return size; }
        }
    }
    return 0;
}

int ts_packet_size(const std::string &path) {
    try {
        MappedFile mf(path);
        uint64_t start = 0;
        return ts_layout(mf.data(), std::min<uint64_t>(mf.size(), 1 << 16), start);
    } catch (...) { return 0; }
}

#include "png.h"
#include "tsinfo.inc"
#include "dvbsub.inc"

std::vector<Packet42> ts_teletext_packets(const std::string &ts, Progress &pr) {
    MappedFile mf(ts);
    const u8 *d = mf.data(); uint64_t n = mf.size();
    uint64_t off = 0;
    int stride = ts_layout(d, std::min<uint64_t>(n, 1 << 16), off);
    if (!stride) { stride = 188; off = 0; while (off < 188 && off < n && !(d[off] == 0x47 && (off + 188 >= n || d[off + 188] == 0x47))) off++; }
    std::map<int, Bytes> pes;
    std::vector<Packet42> pk;
    auto flush = [&](Bytes &b) {
        if (b.size() < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xBD) { b.clear(); return; }
        size_t h = 9 + b[8];
        if (h >= b.size()) { b.clear(); return; }
        if (b[h] < 0x10 || b[h] > 0x1F) { b.clear(); return; }
        size_t i = h + 1;
        while (i + 2 <= b.size()) {
            int id = b[i], len = b[i + 1];
            if (i + 2 + len > b.size()) break;
            if ((id == 0x02 || id == 0x03) && len == 0x2C) {
                Packet42 p;
                for (int k = 0; k < 42; k++) p[k] = (u8)rev8(b[i + 4 + k]);
                pk.push_back(p);
            }
            i += 2 + len;
        }
        b.clear();
    };
    for (uint64_t p = off; p + 188 <= n; p += stride) {
        const u8 *t = d + p;
        if (t[0] != 0x47) continue;
        int pid = ((t[1] & 0x1F) << 8) | t[2];
        bool pusi = t[1] & 0x40;
        int afc = (t[3] >> 4) & 3;
        int h = 4;
        if (afc == 2) continue;
        if (afc == 3) h += 1 + t[4];
        if (h >= 188) continue;
        auto &b = pes[pid];
        if (pusi) { flush(b); if (t[h] == 0 && t[h + 1] == 0 && t[h + 2] == 1) b.assign(t + h, t + 188); }
        else if (!b.empty()) b.insert(b.end(), t + h, t + 188);
        if ((p / 188) % 200000 == 0) pr.progress(p, n);
    }
    for (auto &kv : pes) flush(kv.second);
    return pk;
}

size_t ts_to_t42(const std::string &ts, const std::string &out, Progress &pr) {
    auto pk = ts_teletext_packets(ts, pr);
    write_t42(out, pk);
    pr.log(fmt("DVB teletext: %zu packets written to ", pk.size()) + out);
    return pk.size();
}
