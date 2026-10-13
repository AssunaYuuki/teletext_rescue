#include "saa5050.h"
#include <map>
#include <mutex>

namespace {
struct G9 { uint32_t cp; uint8_t r[9]; };
const G9 GLYPHS[] = {
#include "gen_saa5050.inc"
};
const uint8_t *find(uint32_t cp) {
    static std::map<uint32_t, const uint8_t *> idx;
    static std::once_flag once;
    std::call_once(once, [] { for (auto &g : GLYPHS) idx[g.cp] = g.r; });
    auto it = idx.find(cp);
    return it == idx.end() ? nullptr : it->second;
}
bool pix(const uint8_t *g, int c, int r) {
    int mc = c - 1, mr = r - 1;
    return mc >= 0 && mc < 5 && mr >= 0 && mr < 9 && ((g[mr] >> (4 - mc)) & 1);
}
}

const Glyph20 *saa_rounded(char32_t cp) {
    static std::map<uint32_t, Glyph20> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(cp);
    if (it != cache.end()) return &it->second;
    const uint8_t *g = find(cp);
    if (!g) return nullptr;
    Glyph20 o{};
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 6; c++) {
            int x = 2 * c, y = 2 * r;
            if (pix(g, c, r)) { o[y][x] = o[y][x + 1] = o[y + 1][x] = o[y + 1][x + 1] = true; continue; }
            bool w = pix(g, c - 1, r), e = pix(g, c + 1, r), n = pix(g, c, r - 1), s = pix(g, c, r + 1);
            if (w && n && !pix(g, c - 1, r - 1)) o[y][x] = true;
            if (e && n && !pix(g, c + 1, r - 1)) o[y][x + 1] = true;
            if (w && s && !pix(g, c - 1, r + 1)) o[y + 1][x] = true;
            if (e && s && !pix(g, c + 1, r + 1)) o[y + 1][x + 1] = true;
        }
    return &(cache[cp] = o);
}
