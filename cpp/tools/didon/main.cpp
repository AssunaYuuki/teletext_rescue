// tools/didon: Didon packets (teletext system A, Antiope; 6.203125 Mbit/s = 397 x fH, run-in 55 55, framing E7, a
// 5-byte prefix in Hamming 8/4, data bytes with odd parity) read from a 625-line recording with the per-line channel
// MLSE, written as <out>: per packet frame (int32), stored line (int16), sync errors (int16), 37 bytes after the framing
// code. Prints how many packets each line gave and how many have a whole prefix.
// usage: didon <rec.vbi> <out.bin> [lines e.g. 18-21,331-334] [format key]
#include "vbifile.h"
#include "mlse.h"
#include "fastdata_core.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::vector<int> parseLines(const char *s)
{
    std::vector<int> v;
    std::string t = s;
    size_t p = 0;
    while (p < t.size()) {
        size_t q = t.find(',', p);
        std::string part = t.substr(p, q == std::string::npos ? std::string::npos : q - p);
        size_t d = part.find('-');
        int a = atoi(part.c_str()), b = d == std::string::npos ? a : atoi(part.c_str() + d + 1);
        for (int x = a; x <= b; x++) v.push_back(x);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: didon <rec.vbi> <out.bin> [lines 18-21,331-334] [format]\n"); return 1; }
    VbiFile vf;
    std::string err;
    VbiFormat force = argc > 4 ? formatFromKey(argv[4]) : VbiFormat::Unknown;
    if (!vf.open(argv[1], err, force)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const VbiGeometry &g = vf.geom();
    std::vector<int> tv = parseLines(argc > 3 ? argv[3] : "18-21,331-334");
    std::vector<int> rows;
    for (int t : tv)
        for (int l = 0; l < g.linesPerFrame; l++)
            if (tvLineOf(g, l) == t) rows.push_back(l);
    const double fs = g.lineSamples * g.lineRate, spb = fs / (397 * g.lineRate);
    printf("%s, %lld frames, %.3f samples a bit, %zu lines\n", formatName(g.format), (long long)g.frames, spb, rows.size());
    mlse::Model m;
    m.spb = spb;
    m.pre[0] = 0x55; m.pre[1] = 0x55; m.pre[2] = 0xE7;
    m.nbytes = 37;
    m.good = [](const uint8_t *p) { for (int i = 0; i < 5; i++) if (mlse::hamExact(p[i]) < 0) return false; return true; };
    m.code = [](const uint8_t *, int i) { return i < 5 ? 1 : 2; };
    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::vector<uint8_t> buf;
    const int nt = (int)std::max(1u, std::thread::hardware_concurrency());
    for (int row : rows) {
        mlse::Engine eng(m);
        // learn the channel on lines whose prefix reads
        int64_t tried = 0;
        for (int64_t f0 = 0; f0 < g.frames && eng.learned() < 150 && tried < 20000; f0 += 64) {
            vf.read(f0, 1, buf);
            const uint8_t *p = &buf[(size_t)row * g.lineBytes];
            float info[4];
            fd_prepare(p, g.fmt, g.validSamples, g.lineSamples, info);
            tried++;
            if (info[3] <= 0) continue;
            std::vector<float> raw(g.validSamples), y(g.validSamples);
            for (int k = 0; k < g.validSamples; k++) raw[k] = fd_get(p, g.fmt, k);
            mlse::normalize(raw.data(), g.validSamples, y.data());
            eng.decodeLearn(y.data(), g.validSamples, info[0] - 4 * spb, info[0] + 4 * spb, spb / 5);
        }
        long lines = 0, good = 0;
        if (!eng.hasPrior()) { printf("line %d: no packet to learn the channel on\n", tvLineOf(g, row)); continue; }
        const int chunk = 256;
        for (int64_t f0 = 0; f0 < g.frames; f0 += chunk) {
            int got = (int)vf.read(f0, std::min<int64_t>(chunk, g.frames - f0), buf);
            if (got <= 0) break;
            std::vector<mlse::Result> rs(got);
            std::vector<char> ok(got, 0);
            std::atomic<int> next{0};
            std::vector<std::thread> th;
            for (int t = 0; t < nt; t++)
                th.emplace_back([&] {
                    for (int f; (f = next.fetch_add(1)) < got;) {
                        const uint8_t *p = &buf[(size_t)f * g.frameBytes() + (size_t)row * g.lineBytes];
                        float info[4];
                        fd_prepare(p, g.fmt, g.validSamples, g.lineSamples, info);
                        if (info[3] <= 0) continue;
                        std::vector<float> raw(g.validSamples), y(g.validSamples);
                        for (int k = 0; k < g.validSamples; k++) raw[k] = fd_get(p, g.fmt, k);
                        mlse::normalize(raw.data(), g.validSamples, y.data());
                        rs[f] = eng.decode(y.data(), g.validSamples, info[0] - 3 * spb, info[0] + 3 * spb, spb / 8);
                        ok[f] = 1;
                    }
                });
            for (auto &t : th) t.join();
            for (int f = 0; f < got; f++) {
                if (!ok[f] || rs[f].data.size() < 37) continue;
                lines++;
                good += rs[f].good;
                if (rs[f].good) eng.learn(rs[f]);
                int32_t fr = (int32_t)(f0 + f);
                int16_t ln = (int16_t)tvLineOf(g, row), e = rs[f].good ? 0 : 1;
                fwrite(&fr, 4, 1, out);
                fwrite(&ln, 2, 1, out);
                fwrite(&e, 2, 1, out);
                fwrite(rs[f].data.data(), 1, 37, out);
            }
        }
        printf("line %d: %ld lines read, %ld with a whole prefix (%.0f%%)\n", tvLineOf(g, row), lines, good, lines ? 100.0 * good / lines : 0.0);
        fflush(stdout);
    }
    fclose(out);
    return 0;
}
