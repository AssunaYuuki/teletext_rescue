#include "png.h"
#include "saa5050.h"

static uint32_t crc_table[256];
static struct CrcInit { CrcInit() {
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
} } crc_init_;
static uint32_t crc(const u8 *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

// deflate: LZ77 (хэш по 3 байтам) + фиксированные коды Хаффмана.
namespace {
struct BitW {
    Bytes &o; uint32_t acc = 0; int n = 0;
    explicit BitW(Bytes &b) : o(b) {}
    void put(uint32_t v, int bits) { acc |= v << n; n += bits; while (n >= 8) { o.push_back(acc & 0xFF); acc >>= 8; n -= 8; } }
    void huff(uint32_t code, int len) { uint32_t r = 0; for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i); put(r, len); }
    void flush() { if (n) { o.push_back(acc & 0xFF); acc = 0; n = 0; } }
};
void lit(BitW &w, int v) {
    if (v < 144) w.huff(0x30 + v, 8); else if (v < 256) w.huff(0x190 + v - 144, 9);
    else if (v < 280) w.huff(v - 256, 7); else w.huff(0xC0 + v - 280, 8);
}
const int LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const int LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const int DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
void match(BitW &w, int len, int dist) {
    int i = 28; while (LBASE[i] > len) i--;
    lit(w, 257 + i); if (LEXT[i]) w.put(len - LBASE[i], LEXT[i]);
    int d = 29; while (DBASE[d] > dist) d--;
    w.huff(d, 5); if (DEXT[d]) w.put(dist - DBASE[d], DEXT[d]);
}
Bytes deflate(const Bytes &in) {
    Bytes z = {0x78, 0x01}; BitW w(z);
    w.put(1, 1); w.put(1, 2);
    const int HB = 15, WIN = 32768;
    std::vector<int> head(1 << HB, -1), prev(in.size(), -1);
    auto hash = [&](size_t i) { return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << HB) - 1); };
    size_t i = 0, n = in.size();
    while (i < n) {
        int bl = 0, bd = 0;
        if (i + 2 < n) {
            int h = hash(i), c = head[h], tries = 32;
            while (c >= 0 && (int)i - c <= WIN && tries--) {
                int l = 0; size_t mx = std::min<size_t>(258, n - i);
                while ((size_t)l < mx && in[c + l] == in[i + l]) l++;
                if (l > bl) { bl = l; bd = (int)i - c; if (l == 258) break; }
                c = prev[c];
            }
        }
        size_t adv = bl >= 3 ? bl : 1;
        if (bl >= 3) match(w, bl, bd); else lit(w, in[i]);
        for (size_t k = 0; k < adv; k++, i++) if (i + 2 < n) { int h = hash(i); prev[i] = head[h]; head[h] = (int)i; }
    }
    lit(w, 256); w.flush();
    return z;
}
}

Bytes png_encode(const Image &im) {
    Bytes raw;
    raw.reserve((size_t)(im.w * 3 + 1) * im.h);
    for (int y = 0; y < im.h; y++) {
        raw.push_back(0);
        for (int x = 0; x < im.w; x++) { uint32_t c = im.px[(size_t)y * im.w + x]; raw.push_back((c >> 16) & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back(c & 0xFF); }
    }
    Bytes z = deflate(raw);
    uint32_t a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    z.push_back(ad >> 24); z.push_back(ad >> 16); z.push_back(ad >> 8); z.push_back(ad);
    Bytes out = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    auto chunk = [&](const char *type, const Bytes &d) {
        uint32_t n = (uint32_t)d.size();
        out.push_back(n >> 24); out.push_back(n >> 16); out.push_back(n >> 8); out.push_back(n);
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), d.begin(), d.end());
        uint32_t c = crc(&out[start], out.size() - start) ^ 0xFFFFFFFFu;
        out.push_back(c >> 24); out.push_back(c >> 16); out.push_back(c >> 8); out.push_back(c);
    };
    Bytes ih = {(u8)(im.w >> 24), (u8)(im.w >> 16), (u8)(im.w >> 8), (u8)im.w, (u8)(im.h >> 24), (u8)(im.h >> 16), (u8)(im.h >> 8), (u8)im.h, 8, 2, 0, 0, 0};
    chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
    return out;
}

void png_save(const std::string &path, const Image &im) { write_file(path, png_encode(im)); }

Image Image::scaled(int W, int H) const {
    Image o(W, H);
    for (int y = 0; y < H; y++) {
        int sy = std::min(h - 1, (int)((long long)y * h / H));
        for (int x = 0; x < W; x++) o.px[(size_t)y * W + x] = px[(size_t)sy * w + std::min(w - 1, (int)((long long)x * w / W))];
    }
    return o;
}

// ---------------------------------------------------------------- страница телетекста -> картинка
static const uint32_t COL[8] = {0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

Image render_teletext(const Rows &rows, const Charset &t, const Charset *t2, const Over *over, const RenderOpts &o,
                      bool *has_flash, bool *has_box) {
    const int CW = 12, CH = 20;
    Image im(40 * CW, 25 * CH, o.tv ? 0x3C3C3C : 0);
    auto cs = level1_cells(rows, t, t2, over, has_flash, has_box);
    for (auto &cell : cs) {
        int x = cell.c * CW, y = cell.r * CH;
        if (o.tv && !cell.box) continue;
        int w = cell.w * CW, h = cell.h * CH;
        im.fill(x, y, std::max(w, CW), std::max(h, cell.bh * CH), COL[cell.bg]);
        if ((cell.conceal && !o.reveal) || (cell.flash && !o.flash_on)) continue;
        if (cell.mosaic) {
            int ch = cell.mosaic; int bits[6] = {ch & 1, ch & 2, ch & 4, ch & 8, ch & 16, ch & 64};
            double cw = w / 2.0, chh = h / 3.0; int g = cell.sep ? 1 : 0;
            for (int i = 0; i < 6; i++) if (bits[i]) {
                double cx = x + (i % 2) * cw, cy = y + (i / 2) * chh;
                int x0 = (int)cx + g, y0 = (int)cy + g, x1 = (int)(cx + cw) - 1 - g, y1 = (int)(cy + chh) - 1 - g;
                im.fill(x0, y0, x1 - x0 + 1, y1 - y0 + 1, COL[cell.fg]);
            }
        } else if (!cell.text.empty() && cell.text != " ") {
            std::u32string u = to_u32(cell.text);
            const Glyph20 *g = u.size() == 1 ? saa_rounded(u[0]) : nullptr;
            if (!g && u.size() > 1) g = saa_rounded(u[0]);
            if (g) {
                for (int yy = 0; yy < h; yy++) {
                    int sy = yy * 20 / h;
                    for (int xx = 0; xx < w; xx++) if ((*g)[sy][xx * 12 / w]) im.set(x + xx, y + yy, COL[cell.fg]);
                }
            } else {
                // нет в знакогенераторе — рамка-заглушка
                im.fill(x + 3, y + 4, w - 6, 1, COL[cell.fg]); im.fill(x + 3, y + h - 5, w - 6, 1, COL[cell.fg]);
                im.fill(x + 3, y + 4, 1, h - 8, COL[cell.fg]); im.fill(x + w - 4, y + 4, 1, h - 8, COL[cell.fg]);
            }
        }
    }
    if (o.cursor_r >= 0) {
        int x = o.cursor_c * CW, y = o.cursor_r * CH;
        uint32_t c = 0xFF8800;
        im.fill(x, y, CW, 2, c); im.fill(x, y + CH - 2, CW, 2, c); im.fill(x, y, 2, CH, c); im.fill(x + CW - 2, y, 2, CH, c);
    }
    return im;
}
