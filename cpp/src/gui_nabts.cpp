// Окно NABTS .t33: каталог записей и страницы NAPLPS, нарисованные как на экране приёмника.
#include "gui_app.h"
#include "nabts.h"
#include <windowsx.h>
#include <thread>

namespace ui {
namespace {
const std::vector<std::pair<std::string, std::pair<int, int>>> GRIDS = {{"256 \xC3\x97 200 (as the receiver)", {256, 200}}, {"512 \xC3\x97 400", {512, 400}}, {"768 \xC3\x97 600", {768, 600}}};

struct NabtsWin : Window {
    std::string path;
    std::vector<nabts::Record> recs; bool loaded = false;
    int grid = 0;
    HWND b_grid, c_play, b_replay, b_png, b_all, b_html, msg, lst, info, text;
    std::unique_ptr<nabts::Player> player; Image img; double t0 = 0; std::vector<nabts::Col> key;
    RECT canvas{};
    double now() { return GetTickCount64() / 1000.0; }
    std::pair<int, int> G() { return GRIDS[grid].second; }
    void status(const std::string &t) { set_text(msg, t); }
    int current() { int i = ListView_GetNextItem(lst, -1, LVNI_SELECTED); return i >= 0 && i < (int)recs.size() ? i : -1; }

    void load() {
        status("parsing the stream\xE2\x80\xA6");
        std::string p = path; HWND h = hwnd;
        std::thread([this, p, h] {
            auto *res = new std::pair<std::vector<nabts::Record>, nabts::Summary>();
            std::string err;
            try {
                Progress pr;
                pr.on_progress = [h](long long a, long long b, const std::string &) { PostMessageW(h, WM_APP + 5, (WPARAM)(100 * a / std::max(1LL, b)), 0); };
                res->first = nabts::read_t33(p, res->second, &pr);
                nabts::interpret(res->first, 256, 200);
            } catch (std::exception &e) { err = e.what(); }
            if (!err.empty()) { delete res; auto *e = new std::string(err); PostMessageW(h, WM_APP + 7, 0, (LPARAM)e); }
            else PostMessageW(h, WM_APP + 6, 0, (LPARAM)res);
        }).detach();
    }
    void loaded_cb(std::pair<std::vector<nabts::Record>, nabts::Summary> *res) {
        recs = std::move(res->first); auto summ = res->second; delete res; loaded = true;
        int pages = 0; for (auto &r : recs) pages += r.page != nullptr;
        std::string s = fmt("packets %ld, groups assembled %ld, records %zu, pages %d", summ.packets, summ.groups["complete"], recs.size(), pages);
        if (!summ.foreign.empty()) { s += ";  not teletext: "; bool f = true; for (auto &kv : summ.foreign) { s += fmt("%schannel %03X type %d (%ld)", f ? "" : ", ", kv.first.first, kv.first.second, kv.second); f = false; } }
        status(s);
        static const char *tn[4] = {"page", "one-off", "application", "priority"};
        int first = -1;
        for (size_t i = 0; i < recs.size(); i++) {
            auto &r = recs[i];
            std::string title;
            for (auto &l : split(r.text, '\n')) if (!strip(l).empty()) { title = strip(l); break; }
            if (r.chain_pos) title = fmt("[continuation %d of %s] ", r.chain_pos, nabts::address_text(r.chain_base).c_str()) + title;
            if (!r.purpose.empty()) title = "(" + r.purpose + ") " + title;
            lv_set(lst, (int)i, 0, nabts::record_label(r)); lv_set(lst, (int)i, 1, r.type < 4 ? tn[r.type] : std::to_string(r.type));
            lv_set(lst, (int)i, 2, std::to_string(r.seen)); lv_set(lst, (int)i, 3, nabts::flags_text(r)); lv_set(lst, (int)i, 4, title);
            if (first < 0 && r.page) first = (int)i;
        }
        if (!recs.empty()) { int f = std::max(0, first); ListView_SetItemState(lst, f, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lst, f, FALSE); }
    }
    void reinterpret() {
        if (!loaded) return;
        status("redrawing\xE2\x80\xA6"); UpdateWindow(msg);
        nabts::interpret(recs, G().first, G().second);
        status("receiver " + GRIDS[grid].first); show();
    }
    void show() {
        KillTimer(hwnd, 1); player.reset(); img = Image();
        int i = current(); if (i < 0) { InvalidateRect(hwnd, &canvas, FALSE); return; }
        auto &r = recs[i];
        if (r.page) {
            player.reset(new nabts::Player(*r.page, G().first, G().second));
            if (checked(c_play)) { t0 = now(); player->advance(0.0); }
            else { player->advance(0, true); t0 = now() - player->end(); }
            key.clear(); frame();
        }
        std::string t;
        if (r.page) t = r.text;
        else if (r.type == 2) { t = "Application record (CEA-516 \xC2\xA7" "7.2.2 functions), not a page:\n\n"; for (u8 b : r.data) t += (char)((b & 0x7F) == '\r' ? '\n' : (b & 0x7F) >= 0x20 || (b & 0x7F) == '\n' ? (b & 0x7F) : ' '); }
        else t = "The record has no data to display.";
        set_text(text, replace_all(t, "\n", "\r\n"));
        std::string in = nabts::record_label(r) + fmt(" \xC2\xB7 received %d times, intact %d", r.seen, r.intact_n) + (r.copies_voted ? fmt(", voted from %d copies", r.copies_voted) : "") +
                         fmt(" \xC2\xB7 %zu bytes", r.data.size()) + (r.page ? fmt(" \xC2\xB7 on-screen display %.1f s", r.page->end) : "");
        if (r.chain_pos) in += fmt(" \xC2\xB7 continuation #%d of chain ", r.chain_pos) + nabts::address_text(r.chain_base) + " (drawn over the previous ones)";
        set_text(info, in);
        InvalidateRect(hwnd, &canvas, FALSE);
    }
    void frame() {
        if (!player) return;
        double T = now() - t0;
        bool nw = player->advance(T);
        auto k = player->blink_key(T);
        if (nw || k != key || img.w == 0) { key = k; img = player->image(T); InvalidateRect(hwnd, &canvas, FALSE); }
        if (!player->done()) SetTimer(hwnd, 1, 50, nullptr);
        else if (player->has_blink()) SetTimer(hwnd, 1, 100, nullptr);
        else KillTimer(hwnd, 1);
    }
    Image page_png(const nabts::Record &r) { return nabts::render_page(*r.page, G().first, G().second).scaled(1536, 1152); }
    void save_png() {
        int i = current();
        if (i < 0 || !recs[i].page) { ask(hwnd, "PNG", "This record has no page.", MB_ICONINFORMATION); return; }
        std::string f = save_file(hwnd, "Save page as PNG", nabts::record_name(recs[i]) + ".png", "png");
        if (f.empty()) return;
        png_save(f, page_png(recs[i])); status("saved: " + f);
    }
    void save_all() {
        std::string d = pick_folder(hwnd, "Folder for pages", dirname(path));
        if (d.empty()) return;
        std::string out = path_join(d, basename(stem_path(path)) + "_nabts"); make_dirs(out);
        std::string txt; int n = 0;
        for (auto &r : recs) {
            txt += "=== " + nabts::record_label(r) + fmt("  type %d  received %d  ", r.type, r.seen) + nabts::flags_text(r) + " " + r.purpose + "\n";
            if (r.page) { png_save(path_join(out, nabts::record_name(r) + ".png"), page_png(r)); txt += r.text + "\n"; n++; if (n % 10 == 0) { status(fmt("saved %d\xE2\x80\xA6", n)); UpdateWindow(msg); } }
            txt += "\n";
        }
        write_text(path_join(out, "records.txt"), txt);
        status(fmt("saved %d pages to ", n) + out); open_path(out);
    }
    void save_html() {
        bool any = false; for (auto &r : recs) any |= r.page != nullptr;
        if (!any) { ask(hwnd, "HTML", "There are no pages to save.", MB_ICONINFORMATION); return; }
        std::string d = pick_folder(hwnd, "Folder for the HTML pages", dirname(path));
        if (d.empty()) return;
        Progress pr; HWND m = msg;
        pr.on_progress = [m](long long a, long long b, const std::string &) { if (a % 5 == 0 || a == b) { set_text(m, fmt("HTML: %lld of %lld pages\xE2\x80\xA6", a, b)); UpdateWindow(m); } };
        int n = nabts_html_export(recs, d, G().first, G().second, pr);
        status(fmt("saved %d pages to ", n) + d); open_path(d);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            HWND r = label(hwnd, "Receiver:"); SetPropW(r, L"lbl", (HANDLE)1);
            b_grid = button(hwnd, 201, GRIDS[0].first + " \xE2\x96\xBE");
            c_play = button(hwnd, 202, "Draw as on screen", BTN_CHECK); set_checked(c_play, true);
            b_replay = button(hwnd, 203, "\xE2\x96\xB6 Replay"); b_png = button(hwnd, 204, "Save PNG\xE2\x80\xA6");
            b_all = button(hwnd, 205, "Save all pages (PNG + text)\xE2\x80\xA6"); b_html = button(hwnd, 206, "Save all pages as HTML\xE2\x80\xA6");
            msg = label(hwnd, ""); set_label_colour(msg, MUTED);
            lst = listview(hwnd, 10, {{"Record", 150}, {"Type", 70}, {"Received", -60}, {"Flags", 120}, {"Title", 260}});
            info = label(hwnd, ""); set_label_colour(info, MUTED);
            text = edit(hwnd, 11, "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL);
            SendMessageW(text, WM_SETFONT, (WPARAM)font(hwnd, 10, false, L"Consolas"), TRUE);
            load();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 8), bh = S(hwnd, 30), x = p, y = p;
            HWND r = FindWindowExW(hwnd, nullptr, L"STATIC", L"Receiver:");
            MoveWindow(r, x, y + S(hwnd, 6), S(hwnd, 70), S(hwnd, 20), TRUE); x += S(hwnd, 72);
            MoveWindow(b_grid, x, y, S(hwnd, 220), bh, TRUE); x += S(hwnd, 230);
            MoveWindow(c_play, x, y, S(hwnd, 160), bh, TRUE); x += S(hwnd, 166);
            MoveWindow(b_replay, x, y, S(hwnd, 90), bh, TRUE); x += S(hwnd, 104);
            MoveWindow(b_png, x, y, S(hwnd, 100), bh, TRUE); x += S(hwnd, 106);
            MoveWindow(b_all, x, y, S(hwnd, 220), bh, TRUE); x += S(hwnd, 226);
            MoveWindow(b_html, x, y, S(hwnd, 190), bh, TRUE); x += S(hwnd, 200);
            y += bh + S(hwnd, 4);
            MoveWindow(msg, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE);
            y += S(hwnd, 24);
            int lw = (rc.right - 3 * p) / 3;
            MoveWindow(lst, p, y, lw, rc.bottom - y - p, TRUE);
            int rx = 2 * p + lw, rw = rc.right - rx - p, th = S(hwnd, 200);
            canvas = {rx, y, rx + rw, rc.bottom - p - th - S(hwnd, 26)};
            MoveWindow(info, rx, canvas.bottom + S(hwnd, 2), rw, S(hwnd, 22), TRUE);
            MoveWindow(text, rx, rc.bottom - p - th, rw, th, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HRGN rg = CreateRectRgnIndirect(&rc); HRGN cr = CreateRectRgnIndirect(&canvas); CombineRgn(rg, rg, cr, RGN_DIFF);
            HBRUSH bg = CreateSolidBrush(BG); FillRgn(dc, rg, bg); DeleteObject(bg); DeleteObject(rg); DeleteObject(cr);
            fill(dc, canvas, RGB(0x20, 0x20, 0x20));
            if (img.w) {
                int W = canvas.right - canvas.left, H = canvas.bottom - canvas.top;
                int wv = std::min(W, H * 4 / 3), hv = wv * 3 / 4;
                blit(dc, img, canvas.left + (W - wv) / 2, canvas.top + (H - hv) / 2, wv, hv, false);
            }
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: if (w == 1) frame(); return 0;
        case WM_APP + 5: status(fmt("parsing the stream\xE2\x80\xA6 %d%%", (int)w)); return 0;
        case WM_APP + 6: loaded_cb((std::pair<std::vector<nabts::Record>, nabts::Summary> *)l); return 0;
        case WM_APP + 7: { auto *e = (std::string *)l; status("error"); ask(hwnd, "NABTS", *e, MB_ICONERROR); delete e; return 0; }
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == LVN_ITEMCHANGED) { auto *nv = (NMLISTVIEW *)l; if ((nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED)) show(); }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 201) { grid = (grid + 1) % GRIDS.size(); set_text(b_grid, GRIDS[grid].first + " \xE2\x96\xBE"); reinterpret(); }
            else if (id == 202 || id == 203) show();
            else if (id == 204) save_png();
            else if (id == 205) save_all();
            else if (id == 206) save_html();
            return 0;
        }
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
}

void nabts_window(HWND owner, const std::string &t33) {
    auto *w = new NabtsWin; w->path = t33;
    w->create(L"TRNabts", "NABTS \xE2\x80\x94 " + basename(t33), WS_OVERLAPPEDWINDOW, S(owner, 1300), S(owner, 860), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}
