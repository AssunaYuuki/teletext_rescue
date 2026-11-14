// DVB subtitles (EN 300 743) of a transport stream as pictures: every display set (page composition, regions, CLUTs,
// objects of 2/4/8-bit run-length pixels, both fields) drawn on the display, cut to what it covers and written as PNG
// (dvbts.cpp ts_subtitles). The subtitles are pictures of text, not text: they are shown as they were on the screen.
namespace {
struct SubClut { uint32_t c[256]; uint8_t a[256]; };   // colour 0xRRGGBB, alpha 0 (clear) .. 255
struct SubObj { int id = 0, x = 0, y = 0; };
struct SubRegion { int w = 0, h = 0, depth = 2, clut = 0, fill = -1; std::vector<uint8_t> px; std::vector<SubObj> objs; };
struct SubBits {
    const u8* d; size_t n, p = 0;
    int u(int k) { int v = 0; while (k--) { v = v << 1 | (p / 8 < n ? d[p / 8] >> (7 - p % 8) & 1 : 0); p++; } return v; }
    void align() { p = (p + 7) & ~(size_t)7; }
};
void subDefaultClut(SubClut& c)
{
    // EN 300 743 10: the default 2- / 4- / 8-bit CLUTs, near enough: 0 clear, then white, black, greys and colours
    for (int i = 0; i < 256; i++) {
        int r = i & 1 ? 255 : 0, g = i & 2 ? 255 : 0, b = i & 4 ? 255 : 0;
        if (i & 8) { r /= 2; g /= 2; b /= 2; }
        c.c[i] = (uint32_t)(r << 16 | g << 8 | b);
        c.a[i] = i ? 255 : 0;
    }
    c.c[1] = 0xFFFFFF; c.c[2] = 0x000000; c.c[3] = 0x808080;
}
uint32_t ycc(int y, int cr, int cb)
{
    auto cl = [](double v) { return (uint32_t)std::max(0.0, std::min(255.0, v)); };
    return cl(y + 1.402 * (cr - 128)) << 16 | cl(y - 0.344 * (cb - 128) - 0.714 * (cr - 128)) << 8 | cl(y + 1.772 * (cb - 128));
}
// one field of an object's pixels into a region (lines y0, y0+2, ...)
void subPixels(const u8* d, size_t n, SubRegion& r, int ox, int oy, int y0)
{
    int x = ox, y = oy + y0;
    uint8_t map24[4] = {0, 7, 8, 15}, map28[4] = {0x00, 0x77, 0x88, 0xFF}, map48[16];
    for (int i = 0; i < 16; i++) map48[i] = (uint8_t)(i * 0x11);
    auto put = [&](int run, int code) {
        for (int k = 0; k < run; k++, x++)
            if (x >= 0 && x < r.w && y >= 0 && y < r.h) r.px[(size_t)y * r.w + x] = (uint8_t)code;
    };
    size_t i = 0;
    while (i < n) {
        int type = d[i++];
        if (type == 0xF0) { x = ox; y += 2; continue; }   // end of the object's line
        if (type == 0x20 && i + 2 <= n) { map24[0] = d[i] >> 4; map24[1] = d[i] & 15; map24[2] = d[i + 1] >> 4; map24[3] = d[i + 1] & 15; i += 2; continue; }
        if (type == 0x21 && i + 4 <= n) { for (int k = 0; k < 4; k++) map28[k] = d[i + k]; i += 4; continue; }
        if (type == 0x22 && i + 8 <= n) { for (int k = 0; k < 16; k++) map48[k] = (uint8_t)(k % 2 ? d[i + k / 2] & 15 : d[i + k / 2] >> 4); i += 8; continue; }
        SubBits b{d + i, n - i};
        auto code2 = [&](int c) { return r.depth == 2 ? c : r.depth == 4 ? map24[c] : map28[c]; };
        auto code4 = [&](int c) { return r.depth == 8 ? map48[c] : c; };
        if (type == 0x10) {        // 2-bit/pixel code string
            for (;;) {
                int c = b.u(2);
                if (c) { put(1, code2(c)); continue; }
                if (b.u(1)) { int run = b.u(3) + 3; put(run, code2(b.u(2))); continue; }
                if (b.u(1)) { put(1, code2(0)); continue; }
                int sw = b.u(2);
                if (sw == 0) break;
                if (sw == 1) put(2, code2(0));
                else if (sw == 2) { int run = b.u(4) + 12; put(run, code2(b.u(2))); }
                else { int run = b.u(8) + 29; put(run, code2(b.u(2))); }
                if (b.p / 8 >= b.n) break;
            }
        } else if (type == 0x11) {   // 4-bit/pixel code string
            for (;;) {
                int c = b.u(4);
                if (c) { put(1, code4(c)); continue; }
                if (!b.u(1)) { int run = b.u(3); if (!run) break; put(run + 2, code4(0)); continue; }
                if (!b.u(1)) { int run = b.u(2) + 4; put(run, code4(b.u(4))); continue; }
                int sw = b.u(2);
                if (sw == 0) put(1, code4(0));
                else if (sw == 1) put(2, code4(0));
                else if (sw == 2) { int run = b.u(4) + 9; put(run, code4(b.u(4))); }
                else { int run = b.u(8) + 25; put(run, code4(b.u(4))); }
                if (b.p / 8 >= b.n) break;
            }
        } else if (type == 0x12) {   // 8-bit/pixel code string
            for (;;) {
                int c = b.u(8);
                if (c) { put(1, c); continue; }
                if (!b.u(1)) { int run = b.u(7); if (!run) break; put(run, 0); continue; }
                int run = b.u(7); put(run, b.u(8));
                if (b.p / 8 >= b.n) break;
            }
        } else break;   // a data type it does not know: the rest of the field is not read
        b.align();
        i += b.p / 8;
    }
}
}  // namespace

std::vector<std::string> ts_subtitles(const std::string &path, const std::string &dir, const std::map<int, std::string> &langs) {
    std::vector<std::string> index;
    MappedFile mf(path);
    const u8 *d = mf.data(); uint64_t n = mf.size();
    uint64_t off = 0;
    int stride = ts_layout(d, std::min<uint64_t>(n, 1 << 16), off);
    if (!stride) return index;
    // the PES packets of the subtitle streams (data identifier 0x20), with their PTS
    std::map<int, Bytes> cur;
    std::map<int, std::vector<std::pair<double, Bytes>>> pes;
    double base = -1;
    auto flush = [&](int pid) {
        Bytes& b = cur[pid];
        if (b.size() > 14 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xBD) {
            size_t h = 9 + b[8];
            double pt = -1;
            if (b[7] & 0x80) pt = ((int64_t)((b[9] >> 1) & 7) << 30 | (int64_t)b[10] << 22 | (int64_t)(b[11] >> 1) << 15 | b[12] << 7 | b[13] >> 1) / 90000.0;
            if (h + 2 < b.size() && b[h] == 0x20) pes[pid].push_back({pt, Bytes(b.begin() + (long)h + 2, b.end())});
        }
        b.clear();
    };
    for (uint64_t p = off; p + 188 <= n; p += stride) {
        const u8 *t = d + p;
        if (t[0] != 0x47) continue;
        int pid = ((t[1] & 0x1F) << 8) | t[2], afc = (t[3] >> 4) & 3, h = 4;
        if (!(afc & 1)) continue;
        if (afc == 3) h += 1 + t[4];
        if (h >= 188) continue;
        if (t[1] & 0x40) {
            const u8* q = t + h;
            if (h + 14 <= 188 && q[0] == 0 && q[1] == 0 && q[2] == 1 && (q[7] & 0x80) && q[3] != 0xBD && base < 0)
                base = ((int64_t)((q[9] >> 1) & 7) << 30 | (int64_t)q[10] << 22 | (int64_t)(q[11] >> 1) << 15 | q[12] << 7 | q[13] >> 1) / 90000.0;
            if (cur.count(pid)) flush(pid);
            if (h + 4 <= 188 && q[0] == 0 && q[1] == 0 && q[2] == 1 && q[3] == 0xBD) cur[pid].assign(q, t + 188);
        } else if (cur.count(pid) && !cur[pid].empty()) cur[pid].insert(cur[pid].end(), t + h, t + 188);
    }
    for (auto& kv : cur) flush(kv.first);
    for (auto& kv : pes) {
        if (kv.second.empty()) continue;
        if (base < 0) base = kv.second[0].first;
        int pid = kv.first;
        std::string lang = langs.count(pid) ? langs.at(pid) : fmt("pid%d", pid);
        std::map<int, SubClut> cluts;
        std::map<int, SubRegion> regions;
        std::vector<std::pair<int, std::pair<int, int>>> page;   // region id, position
        int dw = 720, dh = 576, nr = 0;
        double shownAt = -1;
        std::string shownFile;
        Image shownIm;
        auto close = [&](double until) {   // the picture on the screen until now
            if (shownFile.empty()) return;
            index.push_back(shownFile + "\t" + lang + "\t" + fmt("%d", pid) + "\t" + fmt("%.2f\t%.2f\t%d\t%d", shownAt - base, until - base, shownIm.w, shownIm.h));
            shownFile.clear();
        };
        for (auto& pk : kv.second) {
            const Bytes& b = pk.second;
            bool end = false;
            for (size_t i = 0; i + 6 <= b.size() && b[i] == 0x0F;) {
                int type = b[i + 1], len = b[i + 4] << 8 | b[i + 5];
                if (i + 6 + len > b.size()) break;
                const u8* s = &b[i + 6];
                if (type == 0x14 && len >= 5) { dw = (s[1] << 8 | s[2]) + 1; dh = (s[3] << 8 | s[4]) + 1; }
                else if (type == 0x10 && len >= 2) {       // page composition: the regions shown and where
                    if (((s[1] >> 2) & 3) != 0) regions.clear();   // acquisition point / mode change: everything new
                    page.clear();
                    for (int k = 2; k + 6 <= len; k += 6) page.push_back({s[k], {s[k + 2] << 8 | s[k + 3], s[k + 4] << 8 | s[k + 5]}});
                } else if (type == 0x11 && len >= 10) {    // region composition
                    SubRegion& r = regions[s[0]];
                    int w = s[2] << 8 | s[3], hh = s[4] << 8 | s[5], depth = (s[6] >> 2) & 7;
                    r.depth = depth == 1 ? 2 : depth == 2 ? 4 : 8;
                    r.clut = s[7];
                    int fillCode = r.depth == 8 ? s[8] : r.depth == 4 ? s[9] >> 4 : (s[9] >> 2) & 3;
                    if (w != r.w || hh != r.h || r.px.empty()) { r.w = w; r.h = hh; r.px.assign((size_t)w * hh, 0); }
                    if (s[1] & 0x08) std::fill(r.px.begin(), r.px.end(), (uint8_t)fillCode);   // fill flag
                    r.objs.clear();
                    for (int k = 10; k + 6 <= len;) {
                        SubObj o;
                        o.id = s[k] << 8 | s[k + 1];
                        int ot = s[k + 2] >> 6;
                        o.x = (s[k + 2] & 15) << 8 | s[k + 3];
                        o.y = (s[k + 4] & 15) << 8 | s[k + 5];
                        r.objs.push_back(o);
                        k += 6 + (ot == 1 || ot == 2 ? 2 : 0);
                    }
                } else if (type == 0x12 && len >= 2) {     // CLUT definition
                    bool fresh = !cluts.count(s[0]);
                    SubClut& c = cluts[s[0]];
                    if (fresh) subDefaultClut(c);
                    for (int k = 2; k + 2 <= len;) {
                        int e = s[k], full = s[k + 1] & 1, y, cr, cb, tt;
                        if (full) { if (k + 6 > len) break; y = s[k + 2]; cr = s[k + 3]; cb = s[k + 4]; tt = s[k + 5]; k += 6; }
                        else { if (k + 4 > len) break; y = s[k + 2] & 0xFC; cr = (s[k + 2] & 3) << 6 | (s[k + 3] & 0xC0) >> 2; cb = (s[k + 3] & 0x3C) << 2; tt = (s[k + 3] & 3) * 85; k += 4; }
                        c.c[e] = ycc(y, cr, cb);
                        c.a[e] = y == 0 ? 0 : (uint8_t)(255 - tt);
                    }
                } else if (type == 0x13 && len >= 7) {     // object data: its pixels into every region that shows it
                    int id = s[0] << 8 | s[1], method = (s[2] >> 2) & 3;
                    if (method == 0) {
                        int tl = s[3] << 8 | s[4], bl = s[5] << 8 | s[6];
                        const u8* top = s + 7;
                        const u8* bot = bl ? top + tl : top;
                        if (7 + tl + bl <= len)
                            for (auto& rr : regions)
                                for (auto& o : rr.second.objs)
                                    if (o.id == id) { subPixels(top, (size_t)tl, rr.second, o.x, o.y, 0); subPixels(bot, (size_t)(bl ? bl : tl), rr.second, o.x, o.y, 1); }
                    }
                } else if (type == 0x80) end = true;
                i += 6 + len;
            }
            if (!end) continue;
            // the display set is complete: what it shows replaces what was shown
            double at = pk.first >= 0 ? pk.first : shownAt;
            close(at);
            Image im(dw, dh, 0x303030);
            int x0 = dw, y0 = dh, x1 = -1, y1 = -1;
            for (auto& pg : page) {
                auto it = regions.find(pg.first);
                if (it == regions.end()) continue;
                const SubRegion& r = it->second;
                SubClut def;
                subDefaultClut(def);
                const SubClut& c = cluts.count(r.clut) ? cluts[r.clut] : def;
                for (int y = 0; y < r.h; y++)
                    for (int x = 0; x < r.w; x++) {
                        int v = r.px[(size_t)y * r.w + x], X = pg.second.first + x, Y = pg.second.second + y;
                        if (!c.a[v] || X < 0 || Y < 0 || X >= dw || Y >= dh) continue;
                        uint32_t bg = im.px[(size_t)Y * dw + X], fg = c.c[v];
                        int a = c.a[v];
                        auto mix = [&](int sh) { return (((fg >> sh & 255) * a + (bg >> sh & 255) * (255 - a)) / 255) << sh; };
                        im.px[(size_t)Y * dw + X] = (uint32_t)(mix(16) | mix(8) | mix(0));
                        x0 = std::min(x0, X); y0 = std::min(y0, Y); x1 = std::max(x1, X); y1 = std::max(y1, Y);
                    }
            }
            if (x1 < 0) continue;   // an empty page: the screen cleared
            int m = 8;
            x0 = std::max(0, x0 - m); y0 = std::max(0, y0 - m); x1 = std::min(dw - 1, x1 + m); y1 = std::min(dh - 1, y1 + m);
            Image cut(x1 - x0 + 1, y1 - y0 + 1);
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) cut.px[(size_t)(y - y0) * cut.w + (x - x0)] = im.px[(size_t)y * dw + x];
            double rel = at - base;
            std::string name = fmt("%s_%03d_%02d-%02d-%04.1f.png", lang.c_str(), ++nr, (int)(rel / 3600), (int)(rel / 60) % 60, fmod(rel, 60.0));
            make_dirs(dir);
            png_save(path_join(dir, name), cut);
            shownFile = name; shownAt = at; shownIm = cut;
        }
        // the last one: shown to the end of what was recorded of the stream
        close(kv.second.back().first >= 0 ? kv.second.back().first + 4 : shownAt + 4);
    }
    return index;
}
