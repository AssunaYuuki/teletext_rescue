// Картинки: RGB-буфер, запись PNG, отрисовка страницы телетекста (знакогенератор SAA5050).
#pragma once
#include "teletext.h"

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;     // 0xRRGGBB
    Image() {}
    Image(int W, int H, uint32_t c = 0) : w(W), h(H), px((size_t)W * H, c) {}
    void set(int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < w && y < h) px[(size_t)y * w + x] = c; }
    uint32_t get(int x, int y) const { return px[(size_t)y * w + x]; }
    void fill(int x, int y, int W, int H, uint32_t c) {
        for (int yy = std::max(0, y); yy < std::min(h, y + H); yy++)
            for (int xx = std::max(0, x); xx < std::min(w, x + W); xx++) px[(size_t)yy * w + xx] = c;
    }
    Image scaled(int W, int H) const;    // ближайший сосед
};

Bytes png_encode(const Image &im);
void png_save(const std::string &path, const Image &im);

struct RenderOpts { bool reveal = false, flash_on = true, tv = false; int cursor_r = -1, cursor_c = 0; };
// 480 × 500 (клетка 12 × 20)
Image render_teletext(const Rows &rows, const Charset &t, const Charset *t2, const Over *over, const RenderOpts &o,
                      bool *has_flash = nullptr, bool *has_box = nullptr);
