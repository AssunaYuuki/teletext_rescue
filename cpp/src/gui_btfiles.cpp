// «Бизнес-Текст»: the table of the files of the catalogue - every file one row, its blocks drawn one by one as
// they come (live, every second) or as the recording brought them; under it the file chosen, block by block
#include "gui_app.h"
#include "biznestext.h"
#include "finds.h"
#include "arjcheck.h"
#include "pcxfix.h"
#include "faxg4.h"
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <shellapi.h>

namespace ui {
namespace {
const char* const kBtName = "\xD0\x91\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x81-\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82";   // Бизнес-Текст
// a block: not yet / some rows / every row once / sure (sum of row 23 right, or every row twice alike) / sum wrong
const COLORREF B_NONE = RGB(0x3a, 0x3d, 0x44), B_PART = RGB(0xd8, 0x8a, 0x1c), B_ROWS = RGB(0x4f, 0x8f, 0xd8),
               B_SURE = RGB(0x2e, 0xb0, 0x6a), B_WRONG = RGB(0xd8, 0x4a, 0x4a), ONAIR = RGB(0xff, 0xff, 0xff);

COLORREF block_colour(char c)
{
    return c == 'c' ? B_SURE : c == 'r' ? B_ROWS : c == 'p' ? B_PART : c == 'x' ? B_WRONG : B_NONE;
}

std::string size_of(long n)
{
    if (n < 0) return "?";
    if (n < 10000) return fmt("%ld B", n);
    return fmt("%.1f KB", n / 1024.0);
}

// "3-7, 12, 40-41" for the blocks whose map character is one of 'which'
std::string ranges(const std::string& map, const char* which, int limit = 40)
{
    std::string s;
    int n = 0;
    for (size_t i = 0; i < map.size();) {
        if (!strchr(which, map[i])) { i++; continue; }
        size_t j = i;
        while (j + 1 < map.size() && strchr(which, map[j + 1])) j++;
        if (++n > limit) { s += ", \xE2\x80\xA6"; break; }
        s += (s.empty() ? "" : ", ") + (i == j ? fmt("%zu", i + 1) : fmt("%zu-%zu", i + 1, j + 1));
        i = j + 1;
    }
    return s;
}

// a PNG (or JPEG, GIF, BMP) through GDI+: DVB subtitle pictures, the .png copies of PCX files
bool load_image_file(const std::string& path, Image& im)
{
    static ULONG_PTR token = 0;
    if (!token) { Gdiplus::GdiplusStartupInput in; if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false; }
    Gdiplus::Bitmap bmp(W(path).c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::Rect r(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
    im = Image(w, h);
    for (int y = 0; y < h; y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride);
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x], a = p >> 24, bg = 0x30;   // see-through parts on dark grey
            auto ch = [&](int sh) { return ((p >> sh & 255) * a + bg * (255 - a)) / 255; };
            im.px[(size_t)y * w + x] = (uint32_t)(ch(16) << 16 | ch(8) << 8 | ch(0));
        }
    }
    bmp.UnlockBits(&bd);
    return true;
}

struct BtFilesWin : Window {
    std::string title, subtitle;
    std::vector<Find> items;                      // the files of the catalogue (Find::bt set)
    std::function<std::vector<Find>()> source;    // live: asked every second
    std::function<std::string()> activity;        // live: the «Бизнес-Текст» status line
    Header hd;
    HWND act = nullptr;
    int sel = -1, top = 0;                        // the file chosen, the first row shown
    RECT table{}, card{}, view{}, pic{};
    int rowH = 0, headH = 0, listRows = 0;
    // the file itself under its blocks: its text, the files inside an archive (each one shown), a picture
    HWND ed = nullptr, alst = nullptr;
    std::string shownKey, picNote;
    Image img;
    ArjCheck arj;
    int amem = -1;                                // the file of the archive shown

    // every find of every service as a row: «Бизнес-Текст» files with their blocks as received, the others (ТВ-информ,
    // Norpak, data pages, texts) with what their find says (blocks had, bytes not read)
    static std::string short_service(const std::string& s) { size_t k = s.find(" ("); return k == std::string::npos ? s : s.substr(0, k); }
    static std::vector<Find> only_bt(std::vector<Find> v)
    {
        std::vector<Find> out;
        for (auto& f : v) {
            if (!f.bt) {
                BtProgress p;
                p.name = f.name; p.date = f.when; p.kind = f.type; p.path = f.path; p.content = f.status;
                p.size = f.size ? (long)f.size : -1;
                p.blockBytes = f.blockBytes > 0 ? f.blockBytes : 256;
                p.blocks = std::max(0, std::max(f.blocks, f.have));
                p.written = !f.path.empty();
                if (f.kind == Find::Text) { p.content = fmt("text, %zu lines", f.text.size()) + (f.status.empty() ? "" : "; " + f.status); p.written = true; p.readPct = 100; }
                if (p.blocks > 0 && p.blocks <= 5000) {
                    p.map.assign((size_t)p.blocks, '.');
                    if (f.kind == Find::File) {
                        std::vector<int> un((size_t)p.blocks, 0);
                        for (long u : f.unread) if (u >= 0 && u / p.blockBytes < p.blocks) un[(size_t)(u / p.blockBytes)]++;
                        for (int b = 0; b < p.blocks; b++) {
                            bool had = f.unread.empty() ? b < f.have : un[(size_t)b] < p.blockBytes;
                            if (!had) continue;
                            p.map[(size_t)b] = un[(size_t)b] ? 'p' : (f.have >= f.blocks && f.unread.empty()) ? 'c' : 'r';
                        }
                    }
                    for (char c : p.map) { p.blocksHave += c != '.'; p.blocksWhole += c == 'r' || c == 'c'; p.blocksSure += c == 'c'; }
                    p.rows = p.blocks; p.rowsHave = p.blocksWhole;
                }
                if (f.kind == Find::File && p.size > 0) p.readPct = 100.0 * std::max(0L, p.size - (long)f.unread.size()) / p.size;
                p.service = short_service(f.service);
                f.bt = std::make_shared<const BtProgress>(std::move(p));
            } else if (f.bt->service.empty()) {
                BtProgress p = *f.bt;
                p.service = kBtName;
                f.bt = std::make_shared<const BtProgress>(std::move(p));
            }
            out.push_back(std::move(f));
        }
        // by service, then by name (the «Бизнес-Текст» sender sends its files in the order of their names)
        std::stable_sort(out.begin(), out.end(), [](const Find& a, const Find& b) {
            return a.bt->service != b.bt->service ? a.bt->service < b.bt->service : a.name < b.name;
        });
        return out;
    }
    const BtProgress* chosen() const { return sel >= 0 && sel < (int)items.size() ? items[sel].bt.get() : nullptr; }

    // the summary over all files: how many are whole, coming, not yet
    std::string summary() const
    {
        int whole = 0, sure = 0, coming = 0, none = 0, dec = 0;
        long rows = 0, have = 0, put = 0;
        std::string now;
        for (auto& f : items) {
            const BtProgress& p = *f.bt;
            rows += p.rows; have += p.rowsHave; put += p.mended;
            dec += p.decrypted;
            if (!p.blocksHave) none++;
            else if (p.blocksWhole == p.blocks && p.blocks > 0) { whole++; sure += p.blocksSure == p.blocks; }
            else coming++;
            if (p.nowBlock > 0) now = fmt("%s block %d", p.name.c_str(), p.nowBlock);
        }
        std::string s = fmt("%zu found  \xC2\xB7  %d with every block (%d of them sure)  \xC2\xB7  %d coming  \xC2\xB7  %d not yet",
                            items.size(), whole, sure, coming, none);
        if (rows) s += fmt("  \xC2\xB7  rows %ld of %ld (%.1f%%)", have, rows, 100.0 * have / rows);
        if (put) s += fmt("  \xC2\xB7  %ld rows put back", put);
        if (dec) s += fmt("  \xC2\xB7  %d decrypted", dec);
        if (!now.empty()) s += "  \xC2\xB7  \xE2\x96\xB6 on the air: " + now;
        return s;
    }

    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int p = S(hwnd, 14), y = hd.layout(hwnd);
        if (act) { MoveWindow(act, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 24); }
        y += S(hwnd, 26);   // the summary line (drawn)
        rowH = S(hwnd, 26); headH = S(hwnd, 24);
        int rows = std::clamp((int)items.size(), 4, 24);
        int th = std::min(headH + rows * rowH, std::max(headH + 4 * rowH, (int)(rc.bottom * 2 / 5) - y));
        table = {p, y, rc.right - p, y + th};
        listRows = std::max(1, (th - headH) / rowH);
        top = std::clamp(top, 0, std::max(0, (int)items.size() - listRows));
        card = {p, table.bottom + S(hwnd, 10), rc.right - p, std::min(rc.bottom - p, table.bottom + S(hwnd, 10) + S(hwnd, 196))};
        view = {p, card.bottom + S(hwnd, 8), rc.right - p, rc.bottom - p};
        load();
        place_view();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void place_view()
    {
        const bool isArj = !arj.files.empty();
        int lw = isArj ? std::min(S(hwnd, 380), (int)(view.right - view.left) / 3) : 0;
        // moved and shown only when that changes (every second the same place: no repaint, no flicker)
        auto put = [this](HWND c, RECT r, bool show) {
            RECT now{};
            GetWindowRect(c, &now);
            MapWindowPoints(nullptr, hwnd, (POINT *)&now, 2);
            if (show && (now.left != r.left || now.top != r.top || now.right != r.right || now.bottom != r.bottom))
                MoveWindow(c, r.left, r.top, std::max(0L, r.right - r.left), std::max(0L, r.bottom - r.top), TRUE);
            if (show != (IsWindowVisible(c) != FALSE)) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
        };
        put(alst, RECT{view.left, view.top, view.left + lw, view.bottom}, isArj);
        RECT r{view.left + (lw ? lw + S(hwnd, 8) : 0), view.top, view.right, view.bottom};
        if (img.w > 0) pic = r; else pic = {};
        put(ed, r, img.w <= 0 && view.bottom > view.top + 20);
    }
    // DOS text (CP866) as it reads; a file that is not text as hex
    static std::string dos_text(const std::vector<uint8_t>& d)
    {
        long txt = 0;
        for (auto x : d) txt += (x >= 32) || x == 13 || x == 10 || x == 9;
        if (d.empty() || txt * 10 < (long)d.size() * 8) {
            std::string s = fmt("not text: %zu bytes, the first ones as hex\n\n", d.size());
            for (size_t i = 0; i < d.size() && i < 8192; i += 16) {
                s += fmt("%06zx  ", i);
                for (size_t k = i; k < i + 16 && k < d.size(); k++) s += fmt("%02x ", d[k]);
                s += "\n";
            }
            return s;
        }
        std::string s;
        for (auto x : d) {
            if (x == 13) continue;
            if (x == 10 || x == 9) s += (char)x;
            else if (x < 32) s += ' ';
            else if (x < 128) s += (char)x;
            else s += utf8Of866(x);
        }
        return s;
    }
    void show_text(const std::string& t)
    {
        img = Image(); picNote.clear();
        std::string u;
        for (char c : t) { if (c == '\r') continue; if (c == '\n') u += "\r\n"; else u += c; }
        if (u != shownText) { shownText = u; set_text(ed, u); }
    }
    std::string shownText;
    bool show_picture(const std::vector<uint8_t>& t, const std::vector<long>& unread)
    {
        img = Image(); picNote.clear();
        Bytes b(t.begin(), t.end());
        if (isPcx(b)) {
            PcxRepair info;
            if (pcxDecode(b, unread, img, false, &info)) { picNote = info.note(); return true; }
            // the header received damaged (planes or bytes a line wrong): the size is known, the rest is tried
            int w = (b[8] | b[9] << 8) - (b[4] | b[5] << 8) + 1, bpp = b[3];
            if (w > 0 && w <= 8192 && (bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8)) {
                int bpl = (w * bpp + 15) / 16 * 2;
                for (int planes : {4, 1, 3}) {
                    Bytes c = b;
                    c[65] = (uint8_t)planes; c[66] = (uint8_t)(bpl & 255); c[67] = (uint8_t)(bpl >> 8);
                    img = Image();
                    if (pcxDecode(c, unread, img, false, &info) && img.w > 0) { picNote = info.note() + fmt("  \xC2\xB7  header mended (%d planes, %d bytes a line)", planes, bpl); return true; }
                }
            }
            img = Image();
            return false;
        }
        if (t.size() > 8 && ((t[0] == 'I' && t[1] == 'I' && t[2] == 42) || (t[0] == 'M' && t[1] == 'M' && t[3] == 42))) {
            int w = 0, h = 0;
            std::vector<uint8_t> px;
            if (!faxTiffDecode(t, w, h, px, picNote)) return false;
            img = Image(w, h, 0xFFFFFF);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[(size_t)y * w + x]) img.px[(size_t)y * w + x] = 0;
            return true;
        }
        return false;
    }
    void show_member()
    {
        if (amem < 0 || amem >= (int)arj.files.size()) return;
        const ArjEntry& e = arj.files[(size_t)amem];
        if (show_picture(e.data, {})) { picNote = e.name + "  \xC2\xB7  " + picNote; return; }
        std::string head = e.name + fmt("  \xC2\xB7  %u bytes", e.size) + (e.ok ? "  \xC2\xB7  CRC right" : "  \xC2\xB7  " + e.why + fmt(", %u of %u bytes unpacked", e.got, e.size)) + "\n\n";
        show_text(head + dos_text(e.data));
    }
    // the file of the row chosen, again only when it was written again (live) or another row was chosen
    void load()
    {
        const Find* f = sel >= 0 && sel < (int)items.size() ? &items[sel] : nullptr;
        const BtProgress* b = f ? f->bt.get() : nullptr;
        std::string path = b && !b->path.empty() ? b->path : f ? f->path : "";
        std::string key = !f ? "" : f->name + "|" + path + "|" + f->status + fmt("|%.3f|%zu", b ? b->readPct : 0.0, f->text.size());
        if (key == shownKey) return;
        shownKey = key;
        arj = ArjCheck(); amem = -1;
        ListView_DeleteAllItems(alst);
        if (!f) { show_text(""); return; }
        if (!f->text.empty()) {
            std::string t = f->service + "  \xC2\xB7  " + f->where + (f->when.empty() ? "" : "  \xC2\xB7  " + f->when) + "\n\n";
            for (auto& l : f->text) t += l + "\n";
            show_text(t);
            return;
        }
        if (path.empty()) { show_text(b && b->blocksHave ? "Not written yet: the file is written when its blocks have come (live: every 30 s while new blocks come)." : "Nothing of this file has come yet."); return; }
        std::vector<uint8_t> t;
        try { Bytes d = read_file(path); t.assign(d.begin(), d.end()); } catch (...) { show_text("The file is not on disk: " + path); return; }
        if (t.size() > 4 && t[0] == 0x60 && t[1] == 0xEA) {   // an ARJ archive: its files, the first one shown
            arj = arjCheck(t);
            for (size_t i = 0; i < arj.files.size(); i++) {
                const ArjEntry& e = arj.files[i];
                lv_set(alst, (int)i, 0, e.name);
                lv_set(alst, (int)i, 1, fmt("%u", e.size));
                lv_set(alst, (int)i, 2, e.ok ? "OK" : e.why);
            }
            if (!arj.files.empty()) {
                amem = 0;
                ListView_SetItemState(alst, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                show_member();
                return;
            }
        }
        if (show_picture(t, f->unread)) return;
        if (t.size() > 8 && ((t[0] == 0x89 && t[1] == 'P') || (t[0] == 0xFF && t[1] == 0xD8) || (t[0] == 'G' && t[1] == 'I') || (t[0] == 'B' && t[1] == 'M'))) {
            img = Image(); picNote.clear();
            if (load_image_file(path, img)) { picNote = fmt("%dx%d", img.w, img.h) + (f->when.empty() ? "" : "  \xC2\xB7  " + f->when) + "  \xC2\xB7  " + f->service; return; }
        }
        if (exists(path + ".txt")) { try { show_text(read_text(path + ".txt")); return; } catch (...) {} }
        show_text(dos_text(t));
    }

    // columns: x of each from the table's left; the map takes what is left
    struct Col { const char* name; int w; bool right; };
    std::vector<Col> cols() const
    {
        return {{"", 18, false}, {"Service", 110, false}, {"File", 150, false}, {"Size", 66, true}, {"Date", 116, false}, {"Blocks", 74, true},
                {"Rows", 56, true}, {"Sure", 48, true}, {"Put back", 62, true}, {"Written", 190, false}, {"Read", 56, true}};
    }

    void paint(HDC dc, RECT rc)
    {
        HFONT f = font(hwnd, 9), fb = font(hwnd, 9, true), mono = font(hwnd, 9, false, L"Consolas"), big = font(hwnd, 12, true);
        int p = S(hwnd, 14);
        text_out(dc, p, table.top - S(hwnd, 24), summary(), ACCENT2, f);
        fill(dc, table, PANEL);
        // the header row
        auto cs = cols();
        int x = table.left + S(hwnd, 6);
        std::vector<int> xs;
        for (auto& c : cs) { xs.push_back(x); x += S(hwnd, c.w); }
        const int mapX = x + S(hwnd, 8), mapR = table.right - S(hwnd, 10);
        for (size_t i = 0; i < cs.size(); i++) {
            int cx = cs[i].right ? xs[i] + S(hwnd, cs[i].w) - S(hwnd, 8) : xs[i];
            text_out(dc, cx, table.top + S(hwnd, 4), cs[i].name, MUTED, fb, cs[i].right ? TA_RIGHT : 0);
        }
        text_out(dc, mapX, table.top + S(hwnd, 4), "Blocks one by one", MUTED, fb);
        RECT hl{table.left, table.top + headH - 1, table.right, table.top + headH};
        fill(dc, hl, LINE);
        if (items.empty()) {
            text_out(dc, table.left + S(hwnd, 12), table.top + headH + S(hwnd, 8),
                     source ? "Nothing yet: files and texts of every service appear here as soon as the capture finds them\xE2\x80\xA6" : "This recording brought no files or texts of their own.",
                     MUTED, f);
        }
        HRGN clip = CreateRectRgn(table.left, table.top + headH, table.right, table.bottom);
        SelectClipRgn(dc, clip);
        for (int i = top; i < (int)items.size() && i < top + listRows + 1; i++) {
            const BtProgress& b = *items[i].bt;
            int y = table.top + headH + (i - top) * rowH;
            RECT rr{table.left, y, table.right, y + rowH};
            if (i == sel) fill(dc, rr, SEL);
            else if (b.nowBlock > 0) fill(dc, rr, RGB(0x2b, 0x33, 0x3d));
            int ty = y + (rowH - S(hwnd, 15)) / 2;
            const bool whole = b.blocks > 0 && b.blocksWhole == b.blocks, sure = whole && b.blocksSure == b.blocks;
            COLORREF nc = !b.blocksHave ? DIM : sure ? B_SURE : whole ? B_ROWS : FG;
            auto cell = [&](int c, const std::string& s, COLORREF col, HFONT ff) {
                int cx = cs[c].right ? xs[c] + S(hwnd, cs[c].w) - S(hwnd, 8) : xs[c];
                RECT r{xs[c], y, xs[c] + S(hwnd, cs[c].w) - S(hwnd, 4), y + rowH};
                if (cs[c].right) { text_out(dc, cx, ty, s, col, ff, TA_RIGHT); return; }
                HGDIOBJ o = SelectObject(dc, ff);
                SetTextColor(dc, col); SetBkMode(dc, TRANSPARENT);
                std::wstring w = W(s);
                DrawTextW(dc, w.c_str(), (int)w.size(), &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(dc, o);
            };
            const bool btRow = b.service == kBtName;
            if (b.nowBlock > 0) cell(0, "\xE2\x96\xB6", ONAIR, fb);
            cell(1, b.service, MUTED, f);
            cell(2, b.name, nc, fb);
            cell(3, size_of(b.size), MUTED, f);
            cell(4, b.date.empty() ? "" : b.date.substr(0, 14), MUTED, f);
            cell(5, b.blocks ? fmt("%d/%d", b.blocksHave, b.blocks) : "", FG, mono);
            cell(6, b.rows ? fmt("%.0f%%", 100.0 * b.rowsHave / b.rows) : "", FG, mono);
            cell(7, b.blocksSure ? fmt("%d", b.blocksSure) : "", B_SURE, mono);
            cell(8, b.mended ? fmt("%d", b.mended) : "", ACCENT2, mono);
            std::string wr = !btRow ? b.content
                           : !b.written ? (b.blocksHave ? "not yet" : "") : b.open ? "sent open" : b.decrypted ? "decrypted" : "as sent (encrypted)";
            if (btRow && b.written && !b.content.empty()) wr += ", " + b.content;
            cell(9, wr, b.written ? (b.checkOk ? B_SURE : FG) : DIM, f);
            cell(10, b.written && (btRow || b.readPct > 0) ? fmt("%.1f%%", b.readPct) : "", b.checkOk ? B_SURE : FG, mono);
            // the blocks: one cell each
            int n = (int)b.map.size(), mw = mapR - mapX;
            if (n > 0 && mw > 10) {
                int by = y + S(hwnd, 6), bh = rowH - S(hwnd, 12);
                for (int k = 0; k < n; k++) {
                    int x0 = mapX + (int)((long long)k * mw / n), x1 = mapX + (int)((long long)(k + 1) * mw / n);
                    if (x1 - x0 >= 4) x1--;
                    if (x1 <= x0) x1 = x0 + 1;
                    RECT br{x0, by, x1, by + bh};
                    fill(dc, br, block_colour(b.map[(size_t)k]));
                }
                if (b.nowBlock > 0 && b.nowBlock <= n) {   // the block on the air now
                    int x0 = mapX + (int)((long long)(b.nowBlock - 1) * mw / n);
                    RECT mk{x0, y + S(hwnd, 2), x0 + std::max(2, mw / n), y + S(hwnd, 5)};
                    fill(dc, mk, ONAIR);
                    RECT mk2{x0, by + bh + S(hwnd, 1), x0 + std::max(2, mw / n), by + bh + S(hwnd, 4)};
                    fill(dc, mk2, ONAIR);
                }
            }
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        if ((int)items.size() > listRows) {   // where the rows shown are in the list
            int tr = table.bottom - table.top - headH, sh = std::max(S(hwnd, 20), tr * listRows / (int)items.size());
            int sy = table.top + headH + (tr - sh) * top / std::max(1, (int)items.size() - listRows);
            RECT sb{table.right - S(hwnd, 4), sy, table.right - S(hwnd, 1), sy + sh};
            fill(dc, sb, LINE);
        }
        // the file chosen
        fill(dc, card, PANEL);
        const BtProgress* b = chosen();
        int cx = card.left + S(hwnd, 16), cy = card.top + S(hwnd, 12);
        // the legend
        {
            int lx = card.right - S(hwnd, 16);
            const std::pair<COLORREF, const char*> leg[] = {{B_WRONG, "sum wrong"}, {B_SURE, "sure"}, {B_ROWS, "every row"}, {B_PART, "some rows"}, {B_NONE, "not yet"}};
            HGDIOBJ o = SelectObject(dc, f);
            for (auto& l : leg) {
                std::wstring w = W(l.second);
                SIZE sz; GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
                lx -= sz.cx;
                text_out(dc, lx, cy + S(hwnd, 2), l.second, MUTED, f);
                lx -= S(hwnd, 16);
                RECT r{lx, cy + S(hwnd, 4), lx + S(hwnd, 11), cy + S(hwnd, 15)};
                fill(dc, r, l.first);
                lx -= S(hwnd, 14);
            }
            SelectObject(dc, o);
        }
        if (!b) {
            text_out(dc, cx, cy, items.empty() ? "" : "Choose a file above to see its blocks.", MUTED, f);
            return;
        }
        text_out(dc, cx, cy, b->name, FG, big);
        cy += S(hwnd, 28);
        if (sel >= 0 && sel < (int)items.size() && !items[sel].text.empty()) {
            const Find& fd = items[sel];
            text_out(dc, cx, cy, fd.service + "  \xC2\xB7  " + fd.where + (fd.when.empty() ? "" : "  \xC2\xB7  " + fd.when) + fmt("  \xC2\xB7  text, %zu lines (below)", fd.text.size()), ACCENT2, f);
            return;
        }
        std::string l1 = (b->kind.empty() ? std::string() : b->kind + "  \xC2\xB7  ") + size_of(b->size) + (b->size >= 0 ? fmt(" (%ld bytes)", b->size) : "") +
                         fmt("  \xC2\xB7  %d blocks of %d bytes", b->blocks, b->blockBytes) + (b->date.empty() ? "" : "  \xC2\xB7  " + b->date) +
                         (b->page.empty() ? "" : "  \xC2\xB7  page " + b->page) + fmt("  \xC2\xB7  announced %d times", b->announced);
        text_out(dc, cx, cy, l1, ACCENT2, f);
        cy += S(hwnd, 19);
        std::string l2 = fmt("blocks: %d received, %d with every row, %d sure", b->blocksHave, b->blocksWhole, b->blocksSure) +
                         (b->sumWrong ? fmt(", %d with the sum wrong", b->sumWrong) : "") +
                         fmt("  \xC2\xB7  rows %d of %d", b->rowsHave, b->rows) +
                         (b->mended ? fmt("  \xC2\xB7  %d rows sent apart from their block put back (checked by the block sums)", b->mended) : "");
        text_out(dc, cx, cy, l2, FG, f);
        cy += S(hwnd, 19);
        std::string miss = ranges(b->map, "."), part = ranges(b->map, "p");
        std::string l3 = (miss.empty() ? std::string("every block came") : "not yet: " + miss) + (part.empty() ? "" : "   \xC2\xB7   rows missing in: " + part);
        text_out(dc, cx, cy, l3, miss.empty() && part.empty() ? B_SURE : MUTED, f);
        cy += S(hwnd, 19);
        std::string l4 = !b->written ? std::string("not written yet") + (source ? " (the files are written every 30 s while new blocks come)" : "")
                                     : std::string(b->open ? "sent open" : b->decrypted ? "decrypted" : "as sent, not decrypted") + fmt(", %.1f%% read", b->readPct) +
                                           (b->check.empty() ? "" : "  \xC2\xB7  check: " + b->check) + "  \xC2\xB7  " + b->path;
        {
            RECT r{cx, cy, card.right - S(hwnd, 16), cy + S(hwnd, 18)};
            HGDIOBJ o = SelectObject(dc, f);
            SetTextColor(dc, b->checkOk ? B_SURE : MUTED); SetBkMode(dc, TRANSPARENT);
            std::wstring w = W(l4);
            DrawTextW(dc, w.c_str(), (int)w.size(), &r, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, o);
        }
        cy += S(hwnd, 26);
        // every block a square, numbered by tens
        int n = (int)b->map.size();
        if (n <= 0) return;
        int avail = card.right - S(hwnd, 16) - cx - S(hwnd, 40), cell = S(hwnd, 14);
        int per = std::max(10, avail / cell / 10 * 10);
        int lines = (n + per - 1) / per;
        int roomH = card.bottom - S(hwnd, 10) - cy;
        while (lines * cell > roomH && cell > S(hwnd, 6)) { cell--; per = std::max(10, avail / cell / 10 * 10); lines = (n + per - 1) / per; }
        for (int k = 0; k < n; k++) {
            int row = k / per, col = k % per;
            int x0 = cx + S(hwnd, 40) + col * cell, y0 = cy + row * cell;
            if (y0 + cell > card.bottom) break;
            if (col == 0) text_out(dc, cx + S(hwnd, 34), y0 + (cell - S(hwnd, 13)) / 2, fmt("%d", k + 1), DIM, mono, TA_RIGHT);
            RECT br{x0, y0, x0 + cell - 2, y0 + cell - 2};
            fill(dc, br, block_colour(b->map[(size_t)k]));
            if (k + 1 == b->nowBlock) {
                RECT o1{x0 - 1, y0 - 1, x0 + cell - 1, y0}, o2{x0 - 1, y0 + cell - 2, x0 + cell - 1, y0 + cell - 1};
                RECT o3{x0 - 1, y0 - 1, x0, y0 + cell - 1}, o4{x0 + cell - 2, y0 - 1, x0 + cell - 1, y0 + cell - 1};
                fill(dc, o1, ONAIR); fill(dc, o2, ONAIR); fill(dc, o3, ONAIR); fill(dc, o4, ONAIR);
            }
        }
    }

    void paint_picture(HDC dc)
    {
        if (img.w <= 0 || pic.right <= pic.left) return;
        HFONT small = font(hwnd, 9);
        fill(dc, pic, RGB(0x11, 0x11, 0x11));
        int ch = S(hwnd, 22);
        text_out(dc, pic.left + S(hwnd, 8), pic.top + S(hwnd, 4), picNote, MUTED, small);
        int aw = pic.right - pic.left - S(hwnd, 16), ah = pic.bottom - pic.top - ch - S(hwnd, 8);
        if (aw <= 0 || ah <= 0) return;
        double ar = (double)img.w / img.h;
        int w = std::min(aw, (int)(ah * ar)), h = (int)(w / ar);
        int px = pic.left + (pic.right - pic.left - w) / 2, py = pic.top + ch + (ah - h) / 2;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = img.w; bi.bmiHeader.biHeight = -img.h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, px, py, w, h, 0, 0, img.w, img.h, img.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }

    int row_at(int y) const
    {
        if (y < table.top + headH || y >= table.bottom) return -1;
        int i = top + (y - table.top - headH) / std::max(1, rowH);
        return i < (int)items.size() ? i : -1;
    }

    void refresh()
    {
        if (!source) return;
        if (activity && act) { std::string a = activity(); if (a != get_text(act)) set_text(act, a); }
        std::string keep = sel >= 0 && sel < (int)items.size() ? items[sel].name : "";
        items = only_bt(source());
        sel = items.empty() ? -1 : std::max(0, std::min(sel, (int)items.size() - 1));
        for (size_t i = 0; i < items.size(); i++) if (items[i].name == keep) sel = (int)i;
        layout();
    }

    void open_chosen(bool folder)
    {
        const BtProgress* b = chosen();
        std::string path = b ? b->path : "";
        if (path.empty() && sel >= 0 && sel < (int)items.size()) path = items[sel].path;
        if (path.empty()) {
            ask(hwnd, "Files", b && b->blocksHave ? "This file is not written yet: it is written when the key is found (every 30 s while new blocks come)."
                                                 : "Nothing of this file has come yet.", MB_ICONINFORMATION);
            return;
        }
        if (folder) {
            std::wstring w = W(path);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        if (open_bbc_program(hwnd, path)) return;   // telesoftware: runs in the BBC Micro window
        std::string o = openableCopy(path);
        if (!o.empty()) open_path(o);
        else {
            std::wstring w = W(path);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
    }

    // the table as text (tab between the columns): for a message or a spreadsheet
    std::string as_text() const
    {
        std::string s = "File\tSize\tDate\tBlocks\tOf\tRows %\tSure\tPut back\tWritten\tRead %\tNot yet\tRows missing in\tPath\r\n";
        for (auto& f : items) {
            const BtProgress& b = *f.bt;
            s += b.name + "\t" + fmt("%ld", b.size) + "\t" + b.date + "\t" + fmt("%d\t%d\t%.1f\t%d\t%d\t", b.blocksHave, b.blocks, b.rows ? 100.0 * b.rowsHave / b.rows : 0.0, b.blocksSure, b.mended) +
                 (!b.written ? "no" : b.open ? "sent open" : b.decrypted ? "decrypted" : "as sent") + "\t" + (b.written ? fmt("%.1f", b.readPct) : "") + "\t" +
                 ranges(b.map, ".", 1000) + "\t" + ranges(b.map, "p", 1000) + "\t" + b.path + "\r\n";
        }
        return s;
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            hd.create(hwnd, title, subtitle);
            hd.add(hwnd, 601, "Open file", 100);
            hd.add(hwnd, 602, "Folder", 80);
            hd.add(hwnd, 603, "Copy table", 100);
            hd.add(hwnd, 604, "List\xE2\x80\xA6", 70);
            if (activity) { act = label(hwnd, activity()); set_label_colour(act, MUTED); buffer_label(act); }
            ed = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                 0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetWindowTheme(ed, L"DarkMode_Explorer", nullptr);
            SendMessageW(ed, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(ed, WM_SETFONT, (WPARAM)font(hwnd, 10, false, L"Consolas"), TRUE);
            alst = listview(hwnd, 605, {{"Inside the archive", 170}, {"Bytes", 70}, {"", 110}});
            ShowWindow(alst, SW_HIDE);
            if (source) { items = only_bt(source()); SetTimer(hwnd, 1, 1000, nullptr); }
            sel = items.empty() ? -1 : 0;
            layout();
            return 0;
        case WM_TIMER: refresh(); return 0;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == alst && nh->code == LVN_ITEMCHANGED) {
                auto *iv = (NMLISTVIEW *)l;
                if ((iv->uNewState & LVIS_SELECTED) && iv->iItem >= 0 && iv->iItem != amem) { amem = iv->iItem; show_member(); place_view(); InvalidateRect(hwnd, nullptr, FALSE); }
            }
            if (nh->hwndFrom == alst && nh->code == NM_CUSTOMDRAW)
                return lv_custom_draw(l, [this](int row) { return row >= 0 && row < (int)arj.files.size() && arj.files[(size_t)row].ok ? B_SURE : FG; });
            return 0;
        }

        case WM_SIZE: layout(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ ob = SelectObject(mem, bm);
            fill(mem, rc, BG);
            paint(mem, rc);
            paint_picture(mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: {
            int i = row_at(GET_Y_LPARAM_(l));
            if (i >= 0 && i != sel) { sel = i; layout(); }
            SetFocus(hwnd);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int i = row_at(GET_Y_LPARAM_(l));
            if (i >= 0) { sel = i; open_chosen(false); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3;
            top = std::clamp(top + d, 0, std::max(0, (int)items.size() - listRows));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_UP || w == VK_DOWN) {
                if (!items.empty()) {
                    sel = std::clamp(sel + (w == VK_UP ? -1 : 1), 0, (int)items.size() - 1);
                    if (sel < top) top = sel;
                    if (sel >= top + listRows) top = sel - listRows + 1;
                    layout();
                }
                return 0;
            }
            if (w == VK_RETURN) { open_chosen(false); return 0; }
            break;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 601) open_chosen(false);
            else if (id == 602) open_chosen(true);
            else if (id == 604) {   // saving a text, combining two receptions: the list window
                if (source) finds_window_live(hwnd, source, subtitle, activity);
                else finds_window_items(hwnd, items, subtitle);
            }
            else if (id == 603) {
                std::wstring t = W(as_text());
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
                    if (g) {
                        memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(g);
                        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
                    }
                    CloseClipboard();
                }
            }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
    static int GET_Y_LPARAM_(LPARAM l) { return (int)(short)HIWORD(l); }
};

void open_bt(BtFilesWin* w, HWND owner)
{
    int width = S(owner, 1380), height = S(owner, 900);
    w->create(L"TRBtFiles", w->title, WS_OVERLAPPEDWINDOW, width, height, owner);
    // on its screen whole, in the middle (the monitor's own work area: right with any scaling)
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    RECT r; GetWindowRect(w->hwnd, &r);
    int ww = std::min((int)(r.right - r.left), (int)(wa.right - wa.left)), hh = std::min((int)(r.bottom - r.top), (int)(wa.bottom - wa.top));
    SetWindowPos(w->hwnd, nullptr, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - hh) / 2, ww, hh, SWP_NOZORDER);
    ShowWindow(w->hwnd, SW_SHOW);
}
}  // namespace

bool has_bt_files(const std::vector<Find>& items)
{
    for (auto& f : items) if (f.bt) return true;
    return false;
}

void bt_files_window(HWND owner, const std::vector<Find>& items, const std::string& subtitle)
{
    auto* w = new BtFilesWin;
    w->items = BtFilesWin::only_bt(items);
    w->title = "Found files \xE2\x80\x94 every service";
    w->subtitle = subtitle;
    open_bt(w, owner);
}

void bt_files_window_live(HWND owner, std::function<std::vector<Find>()> source, std::function<std::string()> activity, const std::string& subtitle)
{
    auto* w = new BtFilesWin;
    w->source = std::move(source);
    w->activity = std::move(activity);
    w->title = "Found files \xE2\x80\x94 every service, live";
    w->subtitle = subtitle;
    open_bt(w, owner);
}
}  // namespace ui
