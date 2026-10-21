// "Found files and texts": every file and text of its own that a recording or the live capture brought,
// with where (service, lines) and when it was found; a fax page or a text shown under the list.
#include "gui_app.h"
#include "finds.h"
#include "faxg4.h"
#include "pcxfix.h"
#include <algorithm>
#include <uxtheme.h>
#include <shellapi.h>
#include <filesystem>

namespace ui {
namespace {
const COLORREF C_FILE = RGB(0x2e, 0xa0, 0x6a), C_ANN = RGB(0x3a, 0x7b, 0xd5), C_TEXT = RGB(0xff, 0xa3, 0x1a), C_TEST = RGB(0x8a, 0x8d, 0x93);

void fpaint(HWND h, const std::function<void(HDC, RECT)> &draw)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ ob = SelectObject(mem, bm);
    fill(mem, rc, BG);
    draw(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps);
}

std::string size_text(uint64_t n)
{
    if (!n) return "";
    if (n < 10000) return fmt("%llu bytes", (unsigned long long)n);
    return fmt("%.1f KB", n / 1024.0);
}

void open_find(const std::string &path);

struct FindsWin : Window {
    std::string title, subtitle;
    std::vector<Find> items;
    std::function<std::vector<Find>()> source;   // live: asked every 2 s
    std::function<std::string()> activity;       // live: what the decoding does now (a line under the header)
    Header hd; HWND lst = nullptr, ed = nullptr, act = nullptr;
    int sel = -1; bool rot = true;
    RECT card{}, pic{};
    std::vector<uint32_t> img; int iw = 0, ih = 0;
    std::string picNote, shownPath;

    static const char *kind_name(const Find &f)
    {
        return f.kind == Find::File ? "FILE" : f.kind == Find::Announced ? "ANNOUNCED" : f.kind == Find::Text ? "TEXT" : "TEST";
    }
    static COLORREF colour(const Find &f)
    {
        return f.kind == Find::File ? C_FILE : f.kind == Find::Announced ? C_ANN : f.kind == Find::Text ? C_TEXT : C_TEST;
    }
    bool hasPic() const { return iw > 0; }
    void loadPic()
    {
        const Find *f = sel >= 0 && sel < (int)items.size() ? &items[sel] : nullptr;
        std::string want = f && f->kind == Find::File ? f->path : "";
        // live: the file is written again as blocks come - drawn again when its status changes
        std::string key = want.empty() ? "" : want + "|" + f->status + (rot ? "|r" : "");
        if (key == shownPath && !want.empty()) return;
        shownPath = key;
        img.clear(); iw = ih = 0; picNote.clear();
        if (want.empty()) return;
        std::vector<uint8_t> t;
        try { Bytes b = read_file(want); t.assign(b.begin(), b.end()); } catch (...) { picNote = "the file is not on disk any more: " + want; return; }
        if (isPcx(t)) {
            // the lines not received yet as a dark checker: the picture fills up as the blocks come
            Image im;
            PcxRepair info;
            if (!pcxDecode(t, f->unread, im, false, &info)) return;
            iw = im.w; ih = im.h;
            img = im.px;
            picNote = info.note() + (f->unread.empty() ? "" : fmt("; %zu bytes not received", f->unread.size()));
            return;
        }
        int w = 0, h = 0;
        std::vector<uint8_t> px;
        if (!faxTiffDecode(t, w, h, px, picNote)) { picNote.clear(); return; }
        iw = rot ? h : w; ih = rot ? w : h;
        img.assign((size_t)iw * ih, 0xFFFFFF);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (px[(size_t)y * w + x]) {
                    int X = rot ? y : x, Y = rot ? w - 1 - x : y;   // fax maps are sent on their side
                    img[(size_t)Y * iw + X] = 0;
                }
    }
    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int p = S(hwnd, 14), y = hd.layout(hwnd);
        if (act) { MoveWindow(act, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 26); }
        // the list: every row up to 40% of the window, never fewer than 6 rows (+ the scroll bar)
        int rows = (int)std::clamp<size_t>(items.size(), 6, 30);
        int lh = std::min(S(hwnd, 48) + rows * S(hwnd, 22), std::max(S(hwnd, 48) + 6 * S(hwnd, 22), (int)(rc.bottom * 2 / 5)));
        MoveWindow(lst, p, y, rc.right - 2 * p, lh, TRUE); y += lh + S(hwnd, 10);
        card = {p, y, rc.right - p, y + S(hwnd, 112)}; y = card.bottom + S(hwnd, 10);
        int h = std::max(0L, rc.bottom - y - p);
        if (hasPic()) { pic = {p, y, rc.right - p, y + h}; ShowWindow(ed, SW_HIDE); }
        else { pic = {}; MoveWindow(ed, p, y, rc.right - 2 * p, h, TRUE); ShowWindow(ed, SW_SHOW); }
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    void fill_list()
    {
        SendMessageW(lst, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(lst);
        for (size_t i = 0; i < items.size(); i++) {
            const Find &f = items[i];
            lv_set(lst, (int)i, 0, kind_name(f));
            lv_set(lst, (int)i, 1, f.name);
            lv_set(lst, (int)i, 2, f.shortName);
            lv_set(lst, (int)i, 3, f.type);
            lv_set(lst, (int)i, 4, size_text(f.size));
            lv_set(lst, (int)i, 5, f.status);
            lv_set(lst, (int)i, 6, f.service);
            lv_set(lst, (int)i, 7, f.where);
            lv_set(lst, (int)i, 8, f.when);
        }
        if (sel >= 0 && sel < (int)items.size()) ListView_SetItemState(lst, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
    }
    // everything known about a find that has no text of its own, one fact per line
    static std::string describe(const Find &f)
    {
        std::string s = f.name + (f.shortName.empty() ? "" : "   (short name " + f.shortName + ")") + "\n\n";
        auto row = [&](const char *k, const std::string &v) { if (!v.empty()) s += fmt("%-10s ", k) + v + "\n"; };
        row("Kind", kind_name(f));
        row("Type", f.type);
        row("Size", f.size ? size_text(f.size) + fmt(" (%llu bytes)", (unsigned long long)f.size) : "");
        row("Blocks", f.blocks > 0 ? fmt("%d of %d bytes", f.blocks, f.blockBytes) : "");
        row("Status", f.status);
        row("Service", f.service);
        row("Where", f.where);
        row("When", f.when);
        row("On disk", f.path.empty() ? "not written" : f.path);
        if (!f.note.empty()) {
            s += "\n";
            size_t a = 0;   // the note: one part per line ("; " between them)
            while (a < f.note.size()) {
                size_t b = f.note.find("; ", a);
                s += f.note.substr(a, b == std::string::npos ? std::string::npos : b - a) + "\n";
                if (b == std::string::npos) break;
                a = b + 2;
            }
        }
        return s;
    }
    void show()
    {
        loadPic();
        std::string body;
        if (sel >= 0 && sel < (int)items.size()) {
            const Find &f = items[sel];
            if (!f.text.empty()) for (auto &l : f.text) body += l + "\n";
            else if (f.kind == Find::File && !f.path.empty() && f.type == "text") {
                // the file itself is DOS text (CP866): its UTF-8 copy <file>.txt when there is one
                try { body = read_text(exists(f.path + ".txt") ? f.path + ".txt" : f.path); } catch (...) {}
            }
            if (body.empty()) body = describe(f);
        } else if (items.empty())
            body = source ? "Nothing yet. Files and texts appear here as soon as the capture finds them\n(ТВ-информ lines and packets, the «Бизнес-Текст» file catalogue)."
                          : "This recording brought no files or texts of their own.";
        set_text(ed, replace_all(body, "\n", "\r\n"));
        layout();
    }
    void refresh()
    {
        if (!source) return;
        if (activity) { std::string a = activity(); if (a != get_text(act)) set_text(act, a); }
        std::vector<Find> now = source();
        std::string keep = sel >= 0 && sel < (int)items.size() ? items[sel].name : "";
        bool same = now.size() == items.size();
        for (size_t i = 0; same && i < now.size(); i++)
            same = now[i].name == items[i].name && now[i].status == items[i].status && now[i].size == items[i].size;
        if (same) return;
        items = std::move(now);
        sel = items.empty() ? -1 : 0;
        for (size_t i = 0; i < items.size(); i++) if (items[i].name == keep) sel = (int)i;
        fill_list();
        show();
    }
    void paint(HDC dc, RECT)
    {
        if (sel < 0 || sel >= (int)items.size()) return;
        const Find &f = items[sel];
        HFONT big = font(hwnd, 14, true), small = font(hwnd, 9), smallb = font(hwnd, 10, true);
        fill(dc, card, PANEL);
        int x = card.left + S(hwnd, 18), y = card.top + S(hwnd, 14);
        std::string k = kind_name(f);
        RECT pill{x, y, x + S(hwnd, 16) + (int)k.size() * S(hwnd, 9), y + S(hwnd, 24)};
        fill(dc, pill, colour(f));
        text_out(dc, pill.left + S(hwnd, 8), pill.top + S(hwnd, 3), k, RGB(255, 255, 255), smallb);
        std::string nm = f.name + (f.shortName.empty() ? "" : "   (short name " + f.shortName + ")");
        text_out(dc, pill.right + S(hwnd, 12), y, nm, FG, big);
        y += S(hwnd, 34);
        std::string l1 = f.type;
        if (f.size) l1 += (l1.empty() ? "" : "  \xC2\xB7  ") + size_text(f.size);
        if (f.blocks > 0) l1 += fmt("  \xC2\xB7  %d blocks of %d bytes", f.blocks, f.blockBytes);
        if (!f.when.empty()) l1 += "  \xC2\xB7  " + f.when;
        text_out(dc, x, y, l1, ACCENT2, small);
        y += S(hwnd, 20);
        text_out(dc, x, y, f.service + "  \xC2\xB7  " + f.where + "  \xC2\xB7  " + f.status, FG, small);
        y += S(hwnd, 20);
        std::string note = f.note.empty() ? (f.path.empty() ? "" : f.path) : f.note;
        {   // one line, cut with "..." at the card's edge: the whole note is in the text below
            HGDIOBJ o = SelectObject(dc, small);
            SetTextColor(dc, MUTED); SetBkMode(dc, TRANSPARENT);
            RECT r{x, y, card.right - S(hwnd, 18), y + S(hwnd, 20)};
            std::wstring wn = W(note);
            DrawTextW(dc, wn.c_str(), (int)wn.size(), &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, o);
        }
        if (pic.right > pic.left && hasPic()) {
            fill(dc, pic, RGB(0x11, 0x11, 0x11));
            int ch = S(hwnd, 22);
            text_out(dc, pic.left + S(hwnd, 8), pic.top + S(hwnd, 4), picNote, MUTED, small);
            int aw = pic.right - pic.left - S(hwnd, 16), ah = pic.bottom - pic.top - ch - S(hwnd, 8);
            double ar = (double)iw / ih;
            int w = std::min(aw, (int)(ah * ar)), h = (int)(w / ar);
            int px = pic.left + (pic.right - pic.left - w) / 2, py = pic.top + ch + (ah - h) / 2;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = iw; bi.bmiHeader.biHeight = -ih;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, px, py, w, h, 0, 0, iw, ih, img.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            hd.create(hwnd, title, subtitle);
            hd.add(hwnd, 501, "Open file", 100);
            hd.add(hwnd, 502, "Folder", 80);
            hd.add(hwnd, 503, "Save text\xE2\x80\xA6", 110);
            hd.add(hwnd, 504, "Rotate", 80);
            hd.add(hwnd, 505, "Combine with another reception\xE2\x80\xA6", 250);
            lst = listview(hwnd, 10, {{"Kind", 100}, {"Name", 170}, {"Short name", 100}, {"Type", 130}, {"Size", 80}, {"Status", 280},
                                      {"Service", 230}, {"Where", 170}, {"When", 150}});
            // the text wraps at the window's edge (an EDIT made with ES_AUTOHSCROLL never wraps)
            ed = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                 0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetWindowTheme(ed, L"DarkMode_Explorer", nullptr);
            SendMessageW(ed, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(ed, WM_SETFONT, (WPARAM)font(hwnd, 10, false, L"Consolas"), TRUE);
            if (activity) { act = label(hwnd, activity()); set_label_colour(act, ACCENT2); }
            if (source) { items = source(); SetTimer(hwnd, 1, 2000, nullptr); }
            sel = items.empty() ? -1 : 0;
            fill_list();
            show();
            return 0;
        case WM_TIMER: refresh(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_PAINT: fpaint(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l, [this](int row) { return row >= 0 && row < (int)items.size() ? colour(items[row]) : FG; });
                if (nh->code == LVN_ITEMCHANGED) {
                    auto *iv = (NMLISTVIEW *)l;
                    if ((iv->uNewState & LVIS_SELECTED) && iv->iItem >= 0 && iv->iItem != sel) { sel = iv->iItem; show(); }
                }
                if (nh->code == NM_DBLCLK && sel >= 0 && sel < (int)items.size() && !items[sel].path.empty()) open_find(items[sel].path);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            const Find *f = sel >= 0 && sel < (int)items.size() ? &items[sel] : nullptr;
            bool hasF = f && !f->path.empty();
            if (id == 501 && hasF) open_find(f->path);
            else if (id == 502 && hasF) open_path(f->path.substr(0, f->path.find_last_of("\\/")));
            else if (id == 504) { rot = !rot; shownPath.clear(); show(); }
            else if (id == 505 && hasF) {
                // the same file received another time (another recording / live session of the same broadcast):
                // each copy fills what the other did not read; an archive is chosen by its CRCs
                std::string mine = f->path, ext = mine.substr(mine.find_last_of('.') + 1);
                std::string other = open_file(hwnd, "The same file from another reception", {{"The same file", "*." + ext}, {"All files", "*.*"}});
                if (other.empty() || other == mine) return 0;
                auto out = std::make_shared<std::string>();
                run_task(hwnd, "Combining two receptions", [mine, other, out](Progress &pr) {
                    pr.log("putting the two copies together and checking them (an archive can take a minute)");
                    pr.log(combineReceptions(mine, {other}, *out));
                }, [out](bool ok) { if (ok && !out->empty()) open_path(out->substr(0, out->find_last_of("\\/"))); });
            }
            else if (id == 503 && f) {
                const Find keep = *f;   // live: the timer replaces items while the dialog is open
                f = &keep;
                std::string p = save_file(hwnd, "Save text", f->name + ".txt", "txt");
                std::string body = f->name + (f->shortName.empty() ? "" : " (" + f->shortName + ")") + "\n" + f->type + "\n" + f->service + ", " + f->where + "\n" +
                                   f->status + "\n" + f->when + "\n" + f->note + "\n\n";
                for (auto &x : f->text) body += x + "\n";
                if (!p.empty()) { try { write_text(p, body); } catch (std::exception &e) { ask(hwnd, "Save", e.what(), MB_ICONERROR); } }
            }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// a received file: its text made readable (<name>.txt, UTF-8) when there is one; a file Windows has
// no program for (.C01, .COD) is shown selected in its folder instead of the "how to open" question
void open_find(const std::string &path)
{
    std::string o = openableCopy(path);
    if (!o.empty()) { open_path(o); return; }
    std::wstring w = W(path);
    wchar_t exe[MAX_PATH] = L"";
    if ((INT_PTR)FindExecutableW(w.c_str(), nullptr, exe) > 32 && exe[0]) { open_path(path); return; }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void open_finds(FindsWin *w, HWND owner)
{
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, 1240));
    w->create(L"TRFinds", w->title, WS_OVERLAPPEDWINDOW, width, S(owner, 820), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}  // namespace

void finds_window(HWND owner, const ResultPtr &r)
{
    auto *w = new FindsWin;
    w->items = findsOf(*r);
    size_t files = 0, texts = 0;
    for (auto &f : w->items) (f.kind == Find::File || f.kind == Find::Announced ? files : texts)++;
    w->title = "Found files and texts";
    w->subtitle = fmt("%zu files, %zu texts \xC2\xB7 ", files, texts) + file_name(*r);
    open_finds(w, owner);
}

void finds_window_live(HWND owner, std::function<std::vector<Find>()> source, const std::string &subtitle,
                       std::function<std::string()> activity)
{
    auto *w = new FindsWin;
    w->source = std::move(source);
    w->activity = std::move(activity);
    w->title = "Found files and texts \xE2\x80\x94 live";
    w->subtitle = subtitle;
    open_finds(w, owner);
}
}
