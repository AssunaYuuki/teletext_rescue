// Checks the JTES renderer's mosaics against the 34 composite cells measured on the TU-TX100
// (JTES Decoder 0.9.56, tu-tx100-mosaic-composites.json, exported as composites.txt by this tool's .bat):
// each line: overlay-set overlay-code spacing-set spacing-code stl hex-raster (- = none)
#include "jtes.h"
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    FILE* f = fopen(argc > 1 ? argv[1] : "tools/jtes/composites.txt", "r");
    if (!f) { printf("no composites.txt\n"); return 1; }
    char a[8], b[8], c[8], d[8], hex[256];
    int stl, n = 0, bad = 0;
    while (fscanf(f, "%7s %7s %7s %7s %d %255s", a, b, c, d, &stl, hex) == 6) {
        JtesPage p;
        JtesUnit u;
        u.type = 0x20;
        auto& v = u.data;
        v = {0x90, 0x50};                       // background black, foreground white (default)
        if (stl) v.push_back(0x9A);
        if (strcmp(a, "-")) { v.insert(v.end(), {0x1B, 0x28, (uint8_t)strtol(a, 0, 16)}); v.push_back((uint8_t)strtol(b, 0, 16)); }
        if (strcmp(c, "-")) v.insert(v.end(), {0x1B, 0x28, (uint8_t)strtol(c, 0, 16)});
        v.push_back((uint8_t)strtol(d, 0, 16));
        p.units.push_back(u);
        JtesImage im = jtesRender(p);
        int diff = 0;
        std::string got;
        for (int y = 0; y < 24; y++) {
            unsigned want = std::stoul(std::string(hex + y * 4, 4), nullptr, 16), row = 0;
            for (int x = 0; x < 16; x++)
                if (im.px[(size_t)(12 + y) * im.w + x] == 0xFFFFFF) row |= 0x8000 >> x;
            diff += __builtin_popcount(row ^ want);
        }
        n++;
        if (diff) { bad++; printf("%s %s + %s %s stl %d: %d dots differ\n", a, b, c, d, stl, diff); }
    }
    printf("%d cells, %d differ\n", n, bad);
    return bad ? 1 : 0;
}
