// Teletext Rescue — главное окно: проекты страниц телетекста, просмотр и правка, экспорт;
// записи .vbi, потоки .t42/.t34/.t33/.ts открываются отсюда.
#include "gui_app.h"
#include "decode_vbi.h"
#include "pagebuild.h"
#include "project.h"
#include "tools.h"
#include "vbi_auto.h"
#include "opencl.h"
#include <shellapi.h>
#include <exception>
#include <cstdio>
#include <uxtheme.h>
#include <windowsx.h>

using namespace ui;

namespace {
enum {
    ID_SEARCH = 100, ID_LIST, ID_SAVE, ID_EXPORT, ID_UNDO, ID_OPENHTML, ID_TAB_PAGE, ID_TAB_QUAL, ID_SQUASH, ID_WORDS,
    ID_PREV, ID_PNUM, ID_NEXT, ID_VPREV, ID_VNEXT, ID_REVEAL, ID_FULL, ID_TV,
    ID_KEY0 = 130, ID_ACT0 = 140, ID_PAL0 = 160, ID_PAL2_0 = 180, ID_QLIST = 200,
    M_OPEN_STREAM = 1000, M_OPEN_T33, M_OPEN_VBI, M_OPEN_PROJECT, M_SAVE, M_EXPORT, M_EXPORT_FULL, M_EXPORT_SRT, M_OPEN_FOLDER, M_EXIT,
    M_START, M_ABOUT, M_SQUASH, M_WORDS, M_DEVICE, M_RECENT0 = 1100, M_CS0 = 1200, M_CS2_0 = 1300,
};
const COLORREF TT[8] = {RGB(0, 0, 0), RGB(255, 0, 0), RGB(0, 255, 0), RGB(255, 255, 0), RGB(0, 0, 255), RGB(255, 0, 255), RGB(0, 255, 255), RGB(255, 255, 255)};
const COLORREF KEYC[5] = {RGB(0xdd, 0x22, 0x22), RGB(0x22, 0xaa, 0x22), RGB(0xcc, 0xcc, 0x22), RGB(0x22, 0xcc, 0xcc), RGB(0x99, 0x99, 0x99)};
const std::vector<std::pair<std::string, std::string>> CS2 = {{"", "none"}, {"latin", "Latin"}, {"cyr2", "Cyrillic (Russian)"}, {"cyr1", "Cyrillic (Serbian)"}, {"cyr3", "Cyrillic (Ukrainian)"}};

struct App;
App *g_app = nullptr;

struct Screen : Window {   // экран страницы телетекста
    App *app = nullptr;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};
struct Chart : Window {    // три диаграммы карты качества
    App *app = nullptr;
    int hover_chart = -1, hover_i = -1;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};

struct App : Window {
    Project P; bool has_project = false;
    Pages pages; std::vector<std::string> ids; std::string cur; size_t ver = 0;
    std::set<std::string> edited; bool dirty = false;
    std::vector<std::pair<std::string, Page>> undo;
    int cursor_r = 1, cursor_c = 0; bool flash_on = true; bool last_flash = false, last_box = false;
    std::map<std::string, std::vector<Version>> full_cache;
    std::string sort_key = "p"; std::vector<std::string> list_ids;
    std::string kpage;
    int left_w = 0; bool dragging = false; bool tab_quality = false;
    Json quality;
    HMENU menu = nullptr, rmenu = nullptr, csmenu = nullptr;
    HWND start = nullptr;
    // элементы
    HWND b_save, b_export, b_undo, b_html, b_squash, b_words, msg, search, hint, list, tab_page, tab_qual;
    HWND b_prev, pnum, b_next, b_vprev, vinfo, b_vnext, c_reveal, c_full, c_tv, note;
    Screen screen; Chart chart;
    HWND keys_l, keys[5], keys_hint, acts[6], pal[14], pal_l1, pal_l2, pal2[7], stat, help;
    HWND q_title, q_list, q_help;

    // ---------------------------------------------------------------- создание
    void build_menu() {
        menu = CreateMenu();
        HMENU fm = CreatePopupMenu();
        AppendMenuW(fm, MF_STRING, M_OPEN_STREAM, L"Open stream .t42 / .t34 / .t33 / DVB .ts…");
        AppendMenuW(fm, MF_STRING, M_OPEN_T33, L"Open NABTS .t33 (ExtraVision)…");
        AppendMenuW(fm, MF_STRING, M_OPEN_VBI, L"Open .vbi recording…");
        AppendMenuW(fm, MF_STRING, M_OPEN_PROJECT, L"Open project (folder)…");
        rmenu = CreatePopupMenu();
        AppendMenuW(fm, MF_POPUP, (UINT_PTR)rmenu, L"Recent");
        AppendMenuW(fm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fm, MF_STRING, M_SAVE, L"Save\tCtrl+S");
        AppendMenuW(fm, MF_STRING, M_EXPORT, L"Export → output.t42 + HTML");
        AppendMenuW(fm, MF_STRING, M_EXPORT_FULL, L"Export complete pages only → output_full.t42 + HTML");
        AppendMenuW(fm, MF_STRING, M_EXPORT_SRT, L"Export subtitles .srt…");
        AppendMenuW(fm, MF_STRING, M_OPEN_FOLDER, L"Open export folder");
        AppendMenuW(fm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fm, MF_STRING, M_START, L"Start screen");
        AppendMenuW(fm, MF_STRING, M_EXIT, L"Exit");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)fm, L"File");
        csmenu = CreatePopupMenu();
        for (size_t i = 0; i < CHARSET_NAMES.size(); i++) AppendMenuW(csmenu, MF_STRING, M_CS0 + i, W(CHARSET_NAMES[i].second).c_str());
        AppendMenuW(csmenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(csmenu, MF_STRING | MF_GRAYED, 0, L"Second set (ESC code):");
        for (size_t i = 0; i < CS2.size(); i++) AppendMenuW(csmenu, MF_STRING, M_CS2_0 + i, W("   " + CS2[i].second).c_str());
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)csmenu, L"Character set");
        HMENU tm = CreatePopupMenu();
        AppendMenuW(tm, MF_STRING, M_SQUASH, L"Clean stream (squash) \x2192 clean.t42");
        AppendMenuW(tm, MF_STRING, M_WORDS, L"Restore words\x2026");
        AppendMenuW(tm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tm, MF_STRING, M_DEVICE, L"Decode on: graphics card or processor\x2026");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)tm, L"Tools");
        HMENU hm = CreatePopupMenu();
        AppendMenuW(hm, MF_STRING, M_ABOUT, L"About Teletext Rescue");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)hm, L"Help");
        SetMenu(hwnd, menu);
    }
    void fill_recent() {
        while (GetMenuItemCount(rmenu) > 0) DeleteMenu(rmenu, 0, MF_BYPOSITION);
        auto rec = recent_list();
        if (rec.empty()) AppendMenuW(rmenu, MF_STRING | MF_GRAYED, 0, L"(empty)");
        for (size_t i = 0; i < rec.size(); i++) {
            std::string p = rec[i].get_str("path");
            AppendMenuW(rmenu, MF_STRING, M_RECENT0 + i, W(basename(p) + "   \xE2\x80\x94   " + dirname(p)).c_str());
        }
    }
    void create_controls() {
        b_save = button(hwnd, ID_SAVE, "Save (Ctrl+S)"); b_export = button(hwnd, ID_EXPORT, "Export \xE2\x86\x92 output.t42 + HTML");
        b_undo = button(hwnd, ID_UNDO, "Undo (Ctrl+Z)"); b_html = button(hwnd, ID_OPENHTML, "Open html/index.html");
        b_squash = button(hwnd, ID_SQUASH, "Clean stream"); b_words = button(hwnd, ID_WORDS, "Restore words");
        msg = label(hwnd, ""); set_label_colour(msg, MUTED);
        search = edit(hwnd, ID_SEARCH);
        hint = label(hwnd, "search by number or title"); set_label_colour(hint, DIM);
        list = listview(hwnd, ID_LIST, {{"Page", 50}, {"Section", 200}, {"Received", -66}, {"Versions", -60}, {"Note", 80}});
        tab_page = button(hwnd, ID_TAB_PAGE, "Page"); tab_qual = button(hwnd, ID_TAB_QUAL, "Tape quality");
        b_prev = button(hwnd, ID_PREV, "\xE2\x97\x80 page"); pnum = edit(hwnd, ID_PNUM, "", ES_CENTER); b_next = button(hwnd, ID_NEXT, "page \xE2\x96\xB6");
        b_vprev = button(hwnd, ID_VPREV, "\xE2\x97\x80"); vinfo = label(hwnd, "", 0, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS); b_vnext = button(hwnd, ID_VNEXT, "\xE2\x96\xB6");
        c_reveal = button(hwnd, ID_REVEAL, "concealed text", BTN_CHECK); c_full = button(hwnd, ID_FULL, "complete page", BTN_CHECK); c_tv = button(hwnd, ID_TV, "as on a TV screen", BTN_CHECK);
        note = label(hwnd, ""); set_label_colour(note, ACCENT);
        Window::register_class(L"TRScreen", brBG);
        screen.app = this;
        CreateWindowExW(0, L"TRScreen", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd, nullptr, inst(), &screen);
        keys_l = label(hwnd, "Colour keys (FLOF):");
        for (int i = 0; i < 5; i++) { keys[i] = edit(hwnd, ID_KEY0 + i, "", ES_CENTER); SendMessageW(keys[i], WM_SETFONT, (WPARAM)font(hwnd, 11, true, L"Consolas"), TRUE); SetWindowTheme(keys[i], L"", L""); }
        keys_hint = label(hwnd, "double-click \xE2\x80\x94 go to"); set_label_colour(keys_hint, DIM);
        const char *an[6] = {"Clear row", "Delete row", "Row from another version\xE2\x80\xA6", "Delete version", "Copy version", "Delete / restore page"};
        for (int i = 0; i < 6; i++) acts[i] = button(hwnd, ID_ACT0 + i, an[i]);
        for (int i = 0; i < 7; i++) { pal[i] = button(hwnd, ID_PAL0 + i, "T", BTN_COLOUR, TT[i + 1]); pal[7 + i] = button(hwnd, ID_PAL0 + 7 + i, "M", BTN_COLOUR, TT[i + 1]); }
        pal_l1 = label(hwnd, " text"); pal_l2 = label(hwnd, " mosaic");
        const char *pn[7] = {"Background = colour", "Black background", "Double height", "Normal height", "Contiguous mosaic", "Separated mosaic", "Concealed"};
        for (int i = 0; i < 7; i++) pal2[i] = button(hwnd, ID_PAL2_0 + i, pn[i]);
        stat = label(hwnd, ""); set_label_colour(stat, MUTED);
        help = label(hwnd, "Click a character and type. Arrows \xE2\x80\x94 move, Backspace/Delete \xE2\x80\x94 erase, Enter \xE2\x80\x94 next row, PageUp/PageDown \xE2\x80\x94 neighbouring pages.");
        set_label_colour(help, DIM);
        q_title = label(hwnd, ""); SendMessageW(q_title, WM_SETFONT, (WPARAM)font(hwnd, 11, true), TRUE);
        Window::register_class(L"TRChart", brBG);
        chart.app = this;
        CreateWindowExW(0, L"TRChart", L"", WS_CHILD, 0, 0, 10, 10, hwnd, nullptr, inst(), &chart);
        q_list = listview(hwnd, ID_QLIST, {{"Recording minute", -130}, {"Broadcast", 170}, {"Lines received, %", -140}, {"Unreadable, %", -120}, {"Parity errors, %", -130}});
        q_help = label(hwnd, "Received \xE2\x80\x94 share of teletext VBI lines that yielded a packet; unreadable \xE2\x80\x94 signal present but no decoder got a packet; errors \xE2\x80\x94 bytes with bad parity in received packets.");
        set_label_colour(q_help, DIM);
        left_w = S(hwnd, 470);
    }
    std::vector<HWND> page_ctrls() {
        std::vector<HWND> v = {b_prev, pnum, b_next, b_vprev, vinfo, b_vnext, c_reveal, c_full, c_tv, note, screen.hwnd, keys_l, keys_hint, stat, help, pal_l1, pal_l2};
        for (auto h : keys) v.push_back(h);
        for (auto h : acts) v.push_back(h);
        for (auto h : pal) v.push_back(h);
        for (auto h : pal2) v.push_back(h);
        return v;
    }
    void show_tab() {
        for (HWND h : page_ctrls()) ShowWindow(h, tab_quality ? SW_HIDE : SW_SHOW);
        for (HWND h : {q_title, chart.hwnd, q_list, q_help}) ShowWindow(h, tab_quality ? SW_SHOW : SW_HIDE);
        set_button_style(tab_page, tab_quality ? BTN_NORMAL : BTN_ACCENT);
        set_button_style(tab_qual, tab_quality ? BTN_ACCENT : BTN_NORMAL);
        layout();
    }
    void layout() {
        RECT rc; GetClientRect(hwnd, &rc);
        if (start) MoveWindow(start, 0, 0, rc.right, rc.bottom, TRUE);
        int p = S(hwnd, 8), bh = S(hwnd, 30), x = p, y = p;
        auto place = [&](HWND h, int w, int hh = 0) { MoveWindow(h, x, y, w, hh ? hh : bh, TRUE); x += w + S(hwnd, 6); };
        HDC dc = GetDC(hwnd); HGDIOBJ of = SelectObject(dc, font(hwnd));
        auto tw = [&](HWND h) { std::wstring t = W(get_text(h)); SIZE s; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &s); return (int)s.cx + S(hwnd, 24); };
        place(b_save, tw(b_save)); place(b_export, tw(b_export)); place(b_undo, tw(b_undo)); place(b_html, tw(b_html));
        place(b_squash, tw(b_squash)); place(b_words, tw(b_words));
        MoveWindow(msg, x + p, y + S(hwnd, 6), rc.right - x - 2 * p, S(hwnd, 20), TRUE);
        int top = y + bh + p;
        int lw = std::min(std::max(left_w, S(hwnd, 300)), (int)rc.right - S(hwnd, 500));
        MoveWindow(search, p, top, lw - p, S(hwnd, 26), TRUE);
        MoveWindow(hint, p, top + S(hwnd, 28), lw - p, S(hwnd, 18), TRUE);
        MoveWindow(list, p, top + S(hwnd, 48), lw - p, rc.bottom - top - S(hwnd, 48) - p, TRUE);
        int rx = lw + S(hwnd, 10), rw = rc.right - rx - p;
        x = rx; y = top;
        place(tab_page, S(hwnd, 90)); place(tab_qual, S(hwnd, 120));
        y += bh + p;
        int ry = y;
        if (!tab_quality) {
            // ряды кнопок с переносом: сначала меряем высоту, потом расставляем
            auto flow = [&](const std::vector<std::pair<HWND, int>> &items, int y0, bool put) {
                int fx = rx, fy = y0, rowh = S(hwnd, 34);
                for (auto &it : items) {
                    if (fx > rx && fx + it.second > rx + rw) { fx = rx; fy += rowh; }
                    if (put) MoveWindow(it.first, fx, fy + (bh - std::min(bh, S(hwnd, 30))) / 2, it.second, it.first == keys_l || it.first == keys_hint || it.first == pal_l1 || it.first == pal_l2 ? S(hwnd, 24) : bh, TRUE);
                    fx += it.second + S(hwnd, 6);
                }
                return fy + rowh - y0;
            };
            std::vector<std::pair<HWND, int>> rk = {{keys_l, tw(keys_l)}};
            for (int i = 0; i < 5; i++) rk.push_back({keys[i], S(hwnd, 52)});
            rk.push_back({keys_hint, tw(keys_hint)});
            std::vector<std::pair<HWND, int>> ra; for (int i = 0; i < 6; i++) ra.push_back({acts[i], tw(acts[i])});
            std::vector<std::pair<HWND, int>> rp;
            for (int i = 0; i < 7; i++) rp.push_back({pal[i], S(hwnd, 28)});
            rp.push_back({pal_l1, tw(pal_l1)});
            for (int i = 0; i < 7; i++) rp.push_back({pal[7 + i], S(hwnd, 28)});
            rp.push_back({pal_l2, tw(pal_l2)});
            std::vector<std::pair<HWND, int>> r2; for (int i = 0; i < 7; i++) r2.push_back({pal2[i], tw(pal2[i])});
            int hk = flow(rk, 0, false), ha = flow(ra, 0, false), hp = flow(rp, 0, false), h2 = flow(r2, 0, false);
            x = rx;
            place(b_prev, S(hwnd, 72)); place(pnum, S(hwnd, 52), S(hwnd, 30)); place(b_next, S(hwnd, 72)); x += S(hwnd, 8);
            place(b_vprev, S(hwnd, 34)); place(vinfo, std::min(S(hwnd, 380), std::max(S(hwnd, 200), rw - (x - rx) - S(hwnd, 50))), bh); place(b_vnext, S(hwnd, 34));
            y += bh + S(hwnd, 4); x = rx;
            place(c_reveal, tw(c_reveal) + S(hwnd, 6)); place(c_full, tw(c_full) + S(hwnd, 6)); place(c_tv, tw(c_tv) + S(hwnd, 6));
            MoveWindow(note, x + S(hwnd, 8), y + S(hwnd, 5), std::max(0, rx + rw - x - S(hwnd, 8)), S(hwnd, 20), TRUE);
            y += bh + S(hwnd, 4);
            int bottom_h = hk + ha + hp + h2 + S(hwnd, 44) + S(hwnd, 6);
            int sh = rc.bottom - y - bottom_h, sw = rw;
            double k = std::min((double)sw / 480, (double)sh / 500);
            if (k < 0.3) k = 0.3;
            int w = (int)(480 * k), h = (int)(500 * k);
            MoveWindow(screen.hwnd, rx + (rw - w) / 2, y + std::max(0, (sh - h) / 2), w, h, TRUE);
            y += std::max(sh, h) + S(hwnd, 6);
            y += flow(rk, y, true); y += flow(ra, y, true); y += flow(rp, y, true); y += flow(r2, y, true);
            MoveWindow(stat, rx, y, rw, S(hwnd, 20), TRUE);
            MoveWindow(help, rx, y + S(hwnd, 20), rw, S(hwnd, 20), TRUE);
        } else {
            MoveWindow(q_title, rx, ry, rw, S(hwnd, 24), TRUE);
            int ch = std::max(S(hwnd, 160), (int)(rc.bottom - ry) / 3);
            MoveWindow(chart.hwnd, rx, ry + S(hwnd, 30), rw, ch, TRUE);
            int ly = ry + S(hwnd, 36) + ch;
            MoveWindow(q_list, rx, ly, rw, rc.bottom - ly - S(hwnd, 50), TRUE);
            MoveWindow(q_help, rx, rc.bottom - S(hwnd, 44), rw, S(hwnd, 40), TRUE);
        }
        SelectObject(dc, of); ReleaseDC(hwnd, dc);
    }

    // ---------------------------------------------------------------- проекты
    void status(const std::string &t) { set_text(msg, t); }
    bool load_project(const std::string &path, bool quiet = false) {
        Project np;
        if (!np.open(path)) { if (!quiet) ask(hwnd, "Could not open", "There is no pages.json in the folder " + path, MB_ICONERROR); return false; }
        Pages pg; std::set<std::string> ed; std::string src;
        try { pg = np.load_state(ed, src); if (pg.empty()) throw std::runtime_error("the project has no pages"); }
        catch (std::exception &e) { if (!quiet) ask(hwnd, "Could not open", e.what(), MB_ICONERROR); return false; }
        P = np; has_project = true; pages = pg; edited = ed; dirty = false; undo.clear(); full_cache.clear();
        ids.clear(); for (auto &kv : pages) ids.push_back(kv.first);
        cur = pages.count("100") ? "100" : ids[0]; ver = 0; cursor_r = 1; cursor_c = 0; kpage.clear();
        SetWindowTextW(hwnd, W(P.title + "  \xC2\xB7  " + P.system + "  \xE2\x80\x94  " + P.proj).c_str());
        update_charset_menu();
        fill_list(); open_page(cur); build_quality();
        status("Loaded: " + src + fmt(", %zu pages, ", ids.size()) + P.system);
        Json st = load_state(); st["last"] = P.proj; save_state(st);
        recent_add(P.proj, "project");
        hide_start();
        return true;
    }
    void update_charset_menu() {
        for (size_t i = 0; i < CHARSET_NAMES.size(); i++) CheckMenuItem(csmenu, M_CS0 + i, CHARSET_NAMES[i].first == P.charset ? MF_CHECKED : MF_UNCHECKED);
        for (size_t i = 0; i < CS2.size(); i++) CheckMenuItem(csmenu, M_CS2_0 + i, CS2[i].first == P.charset2 ? MF_CHECKED : MF_UNCHECKED);
    }
    void show_start() {
        if (start) DestroyWindow(start);
        StartActions a;
        a.open_vbi = [this] { menu_vbi(); };
        a.open_stream = [this] { menu_stream(); };
        a.open_project = [this] { menu_project(); };
        a.open_recent = [this](const Json &r) { open_recent(r); };
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) ShowWindow(c, SW_HIDE);
        start = start_screen(hwnd, a);
        layout();
        SetWindowPos(start, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    void hide_start() {
        if (!start) return;
        DestroyWindow(start); start = nullptr;
        for (HWND h : {b_save, b_export, b_undo, b_html, b_squash, b_words, msg, search, hint, list, tab_page, tab_qual}) ShowWindow(h, SW_SHOW);
        show_tab();
    }
    void open_recent(const Json &r) {
        std::string k = r.get_str("kind"), p = r.get_str("path");
        if (k == "project") { if (can_leave()) load_project(p); }
        else if (k == "vbi") open_vbi(p);
        else open_stream(p);
    }
    bool can_leave() {
        if (!cur.empty()) set_flofs();
        if (!dirty) return true;
        int r = ask(hwnd, "Unsaved edits", "Save changes to the current project?", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) save();
        return true;
    }
    void menu_project() {
        if (!can_leave()) return;
        std::string d = pick_folder(hwnd, "Project folder (with pages.json)");
        if (d.empty()) return;
        if (!Project::is_project(d)) { ask(hwnd, "Not a project", "There is no pages.json in the folder " + d, MB_ICONERROR); return; }
        load_project(d);
    }
    void menu_stream() {
        if (!can_leave()) return;
        std::string f = open_file(hwnd, "Teletext stream .t42 (625-line WST) / .t34 (525-line WST) / .t33 (NABTS) / DVB .ts",
                                  {{"Teletext stream", "*.t42;*.t34;*.t33;*.ts;*.mts;*.m2ts"}, {"All files", "*.*"}});
        if (!f.empty()) open_stream(f);
    }
    void open_stream(const std::string &f) {
        recent_add(f, "stream");
        if (ends_with_i(f, ".t33")) { open_t33(f); return; }
        if (ends_with_i(f, ".ts") || ends_with_i(f, ".mts") || ends_with_i(f, ".m2ts")) {
            std::string t42 = stem_path(f) + ".t42";
            run_task(hwnd, "Reading teletext from " + basename(f), [f, t42](Progress &pr) {
                pr.step("extracting EBU teletext (EN 300 472) from the transport stream");
                if (ts_to_t42(f, t42, pr) == 0) throw std::runtime_error("the transport stream carries no EBU teletext");
            }, [this, t42](bool ok) { if (ok) open_stream(t42); });
            return;
        }
        uint64_t size = file_size(f); int k = ends_with_i(f, ".t34") ? 34 : 42;
        if (size < (uint64_t)k || size % k) { ask(hwnd, "Not a stream", fmt("The file size is not a multiple of %d bytes \xE2\x80\x94 this is not a .t%d packet stream.", k, k), MB_ICONERROR); return; }
        std::string info = fmt("%llu packets", (unsigned long long)(size / k)) + (k == 34 ? " (525 lines, 32-character rows)" : " (625 lines, 40-character rows)");
        std::string out = project_dir(f, info);
        if (!out.empty()) run_build("Building pages from " + basename(f), [f, out](Progress &pr) { build_project(f, out, "", 32, "", pr); }, out);
    }
    void menu_t33() {
        std::string f = open_file(hwnd, "NABTS stream .t33 (ExtraVision, NBC Teletext)", {{"NABTS stream", "*.t33"}, {"All files", "*.*"}});
        if (!f.empty()) open_t33(f);
    }
    void open_t33(const std::string &f) {
        uint64_t size = file_size(f);
        if (size < 33 || size % 33) { ask(hwnd, "Not a stream", "The file size is not a multiple of 33 bytes \xE2\x80\x94 this is not a NABTS .t33 packet stream.", MB_ICONERROR); return; }
        nabts_window(hwnd, f);
    }
    void menu_vbi() {
        if (!can_leave()) return;
        std::string f = open_file(hwnd, "VBI recording", {{"VBI recording", "*.vbi;*.vbi.flac;*.flac"}, {"All files", "*.*"}});
        if (!f.empty()) open_vbi(f);
    }
    void open_vbi(const std::string &f) {
        recent_add(f, "vbi");
        std::string rep = path_join(vbi_report_dir(f), "report.json");
        bool again = false;
        if (exists(rep)) {
            int r = ask(hwnd, "Already decoded", basename(f) + " was already decoded.\n\nYes \xE2\x80\x94 open the results\nNo \xE2\x80\x94 decode the recording again", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            if (r == IDYES) { vbi_results(f); return; }
            again = true;
        }
        if (!pick_device(false)) return;
        run_task(hwnd, "Opening " + basename(f), [f, again](Progress &pr) { vbi_auto(f, again, pr); }, [this, f](bool ok) { if (ok) vbi_results(f); });
    }
    // на чём декодировать: видеокарта (любая с OpenCL: NVIDIA, AMD, Intel) или процессор; выбор запоминается
    bool pick_device(bool force) {
        Json st = load_state();
        int saved = st["device"].is_null() ? -2 : st["device"].integer();
        bool ask_it = st.get_bool("device_ask", true);
        auto devs = gpu::devices();
        if (!force && (!ask_it || devs.empty())) { gpu::use(devs.empty() ? -1 : saved); return true; }
        int best = gpu::best_device();
        std::vector<std::string> opts;
        for (size_t i = 0; i < devs.size(); i++)
            opts.push_back("Graphics card: " + devs[i].name + fmt("  (%zu MB%s)", devs[i].mem_mb, devs[i].integrated ? ", built-in" : "") + ((int)i == best ? "  \xE2\x80\x94 fastest" : ""));
        opts.push_back("Processor (CPU) \xE2\x80\x94 slower, works on any computer");
        int def = saved == -1 ? (int)devs.size() : (saved >= 0 && saved < (int)devs.size() ? saved : std::max(0, best));
        bool remember = false;
        int r = choose(hwnd, "Decode on", devs.empty() ? "No graphics card with OpenCL was found \xE2\x80\x94 the recording is decoded on the processor."
                                                       : "What should decode the recording?", opts, def, "Remember and do not ask again", &remember);
        if (r < 0) return false;
        int dev = r == (int)devs.size() ? -1 : r;
        gpu::use(dev);
        st["device"] = (double)dev; st["device_ask"] = !remember; save_state(st);
        status(dev < 0 ? "decoding on the processor" : "decoding on " + devs[dev].name);
        return true;
    }
    void vbi_results(const std::string &f) {
        Json R = load_json(path_join(vbi_report_dir(f), "report.json"), Json::object());
        if (R["format"].is_null()) { vbi_manual(f); return; }
        for (auto &r : R["results"].a) {
            std::string k = r.get_str("kind");
            if (k == "teletext_project" || k == "t33" || k == "starsight" || r.get_str("service") == "Silent Radio" || is_amol(r) || is_xds(r)) open_result(r);
        }
        recording_window(hwnd, R, [this](const Json &r) { open_result(r); });
    }
    static bool is_xds(const Json &r) {
        std::string p = r.get_str("path");
        return basename(p).rfind("xds_", 0) == 0 && exists(stem_path(p) + ".json");
    }
    static bool is_amol(const Json &r) {
        std::string p = r.get_str("path");
        return basename(p).rfind("amol_", 0) == 0 && exists(stem_path(p) + ".json");
    }
    void open_result(const Json &r) {
        std::string p = r.get_str("path"), k = r.get_str("kind");
        if (is_amol(r)) { amol_window(hwnd, stem_path(p) + ".json"); return; }
        if (is_xds(r)) { xds_window(hwnd, stem_path(p) + ".json"); return; }
        if (k == "starsight" && exists(p)) { starsight_window(hwnd, p); return; }
        if (k == "teletext_project" && Project::is_project(p)) { if (can_leave()) load_project(p); }
        else if (k == "t33") open_t33(p);
        else if (r.get_str("service") == "Silent Radio" && exists(path_join(dirname(p), "packets.json"))) sign_window(hwnd, dirname(p));
        else if (exists(p)) open_path(p);
    }
    void vbi_manual(const std::string &f) {
        uint64_t size = file_size(f); int lpf = 32;
        if (size % (lpf * 2048)) {
            std::string v = "32";
            if (!input(hwnd, "Recording format", "The recording format was not recognised.\nHow many VBI lines of 2048 samples per frame?", v)) return;
            lpf = atoi(v.c_str());
            if (lpf < 1 || lpf > 64) return;
        }
        int n = (int)(size / ((uint64_t)lpf * 2048));
        std::string out = project_dir(f, fmt("%d frames (~%d min of recording)", n, std::max(1, (int)lround(n / 25.0 / 60))));
        if (!out.empty()) run_build("Decoding " + basename(f), [f, out, lpf](Progress &pr) { decode_vbi_project(f, out, lpf, true, true, pr); }, out);
    }
    std::string project_dir(const std::string &src, const std::string &info) {
        std::string out = stem_path(src) + "_teletext";
        if (Project::is_project(out)) {
            int r = ask(hwnd, "Project already exists", "A project has already been built for this file:\n" + out + "\n\nYes \xE2\x80\x94 open it\nNo \xE2\x80\x94 build it again", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return "";
            if (r == IDYES) { load_project(out); return ""; }
            std::string ed = path_join(out, "pages_edited.json");
            if (exists(ed)) { std::error_code ec; fs::rename(::P(ed), ::P(path_join(out, "pages_edited." + now_str("%Y%m%d-%H%M%S") + ".json")), ec); }
        } else if (ask(hwnd, "New project", basename(src) + ": " + info + ".\n\nThe project will be created in the folder\n" + out + "\nThe source file is not changed.", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return "";
        return out;
    }
    void run_build(const std::string &title, std::function<void(Progress &)> work, const std::string &out) {
        run_task(hwnd, title, work, [this, out](bool ok) {
            if (!ok) return;
            if (Project::is_project(out)) { load_project(out); if (!clean_note.empty()) { status(clean_note); clean_note.clear(); } }
            else ask(hwnd, "Build", "The build finished, but no pages were produced.", MB_ICONERROR);
        });
    }

    // ---------------------------------------------------------------- список
    void fill_list() {
        std::string f = lower(strip(get_text(search)));
        struct It { std::string p, title, note; int tx; size_t nv; };
        std::vector<It> items;
        for (auto &p : ids) {
            Page &pg = pages[p]; std::string title = P.page_title(pg, p);
            if (!f.empty() && p.find(f) == std::string::npos && lower(title).find(f) == std::string::npos) continue;
            std::string nt = pg.ex.boxed ? "service" : ""; if (pg.deleted) nt = "deleted";
            items.push_back({p, title, nt, pg.ex.tx, pg.versions.size()});
        }
        std::stable_sort(items.begin(), items.end(), [&](const It &a, const It &b) {
            if (sort_key == "t") return a.title < b.title;
            if (sort_key == "tx") return a.tx > b.tx;
            if (sort_key == "v") return a.nv > b.nv;
            if (sort_key == "n") return a.note < b.note;
            return a.p < b.p;
        });
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        list_ids.clear();
        for (size_t i = 0; i < items.size(); i++) {
            lv_set(list, (int)i, 0, items[i].p); lv_set(list, (int)i, 1, items[i].title);
            lv_set(list, (int)i, 2, std::to_string(items[i].tx)); lv_set(list, (int)i, 3, std::to_string(items[i].nv)); lv_set(list, (int)i, 4, items[i].note);
            list_ids.push_back(items[i].p);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        select_in_list();
    }
    void select_in_list() {
        auto it = std::find(list_ids.begin(), list_ids.end(), cur);
        if (it == list_ids.end()) return;
        int i = (int)(it - list_ids.begin());
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, i, FALSE);
    }
    COLORREF row_colour(int row) {
        if (row < 0 || row >= (int)list_ids.size()) return FG;
        auto &p = list_ids[row]; auto &pg = pages[p];
        if (pg.deleted) return RGB(0x99, 0x99, 0x99);
        if (edited.count(p)) return RGB(0x5a, 0xa9, 0xff);
        if (pg.ex.boxed) return ACCENT;
        return FG;
    }

    // ---------------------------------------------------------------- навигация и показ
    Page &page() { return pages[cur]; }
    const std::vector<Version> &versions() {
        Page &pg = page();
        if (!checked(c_full)) return pg.versions;
        auto it = full_cache.find(cur);
        if (it == full_cache.end()) it = full_cache.emplace(cur, P.full_versions(pg)).first;
        return it->second;
    }
    void open_page(const std::string &p) {
        if (!pages.count(p)) { status("there is no page " + p); return; }
        set_flofs();
        cur = p; ver = 0; cursor_r = 1; cursor_c = 0;
        select_in_list();
        draw(); SetFocus(screen.hwnd);
    }
    void step(int d) {
        if (cur.empty()) return;
        auto it = std::find(ids.begin(), ids.end(), cur);
        int i = (int)(it - ids.begin()); i = (i + d + (int)ids.size()) % (int)ids.size();
        open_page(ids[i]);
    }
    void step_ver(int d) { if (cur.empty()) return; size_t n = versions().size(); ver = (ver + d + n) % n; draw(); }
    Version &snap_mut() { auto &V = page().versions; return V[std::min(ver, V.size() - 1)]; }
    const Version &snap() { auto &V = versions(); return V[std::min(ver, V.size() - 1)]; }
    bool editable() { if (checked(c_full)) { status("the complete page is assembled from versions \xE2\x80\x94 untick \xE2\x80\x9C" "complete page\xE2\x80\x9D to edit"); return false; } return true; }
    Image current_image() {
        const Version &s = snap(); Page &pg = page();
        const Charset &t = P.table(&pg);
        Over x = P.fitted(s.rows, P.overlay(pg, &s), t);
        RenderOpts o; o.reveal = checked(c_reveal); o.flash_on = flash_on; o.tv = checked(c_tv); o.cursor_r = cursor_r; o.cursor_c = cursor_c;
        return render_teletext(s.rows, t, P.table2(), &x, o, &last_flash, &last_box);
    }
    void draw(bool keep_flash = false) {
        if (cur.empty() || !has_project) return;
        if (!keep_flash) flash_on = true;
        InvalidateRect(screen.hwnd, nullptr, FALSE);
        KillTimer(hwnd, 1);
        current_image();      // обновить last_flash / last_box
        if (last_flash) SetTimer(hwnd, 1, flash_on ? 1000 : 333, nullptr);
        set_text(pnum, cur);
        Page &pg = page();
        Page view = pg; view.versions = versions();
        set_text(vinfo, P.version_label(view, std::min(ver, view.versions.size() - 1)));
        if (!last_box && checked(c_tv)) status("this page has no boxes \xE2\x80\x94 on a TV screen its text is not shown");
        std::string nt = P.service_note(pg);
        if (pg.deleted) nt = "page deleted (will not be exported)";
        std::string sv = P.service_line();
        if (!sv.empty()) nt = nt.empty() ? sv : nt + "  \xC2\xB7  " + sv;
        set_text(note, nt);
        for (int i = 0; i < 5; i++) set_text(keys[i], i < (int)pg.ex.flof.size() ? pg.ex.flof[i] : "");
        kpage = cur;
        const Version &s = snap();
        auto r = s.rows.find(cursor_r);
        set_text(stat, fmt("row %d, column %d", cursor_r, cursor_c) + (r != s.rows.end() ? fmt(", code 0x%02x", r->second[cursor_c]) : ", no row") +
                           fmt(" \xC2\xB7 received %d times, subpages %d", pg.ex.tx, pg.ex.subpages));
    }

    // ---------------------------------------------------------------- правка
    void push() { undo.push_back({cur, page()}); if (undo.size() > 300) undo.erase(undo.begin()); }
    void touch() { full_cache.clear(); dirty = true; edited.insert(cur); status("there are unsaved changes"); InvalidateRect(list, nullptr, FALSE); }
    Row &row(int r) { auto &s = snap_mut(); if (!s.rows.count(r)) { Row x; x.fill(0x20); s.rows[r] = x; } return s.rows[r]; }
    bool set_char(int code) {
        if (!editable()) return false;
        if (cursor_r == 0 && cursor_c < 8) { status("the first 8 positions of the header are filled automatically"); return false; }
        push(); row(cursor_r)[cursor_c] = (u8)code; touch(); return true;
    }
    void move(int dr, int dc) {
        int r = cursor_r + dr, c = cursor_c + dc;
        if (c > 39) { c = 0; r++; }
        if (c < 0) { c = 39; r--; }
        cursor_r = std::max(0, std::min(24, r)); cursor_c = c; draw();
    }
    void insert(int code) { if (cur.empty()) return; if (set_char(code)) move(0, 1); SetFocus(screen.hwnd); }
    void on_key(WPARAM vk) {
        if (cur.empty()) return;
        switch (vk) {
        case VK_LEFT: move(0, -1); break; case VK_RIGHT: move(0, 1); break; case VK_UP: move(-1, 0); break; case VK_DOWN: move(1, 0); break;
        case VK_HOME: cursor_c = 0; draw(); break; case VK_END: cursor_c = 39; draw(); break;
        case VK_RETURN: cursor_r = std::min(24, cursor_r + 1); cursor_c = 0; draw(); break;
        case VK_PRIOR: step(-1); break; case VK_NEXT: step(1); break;
        case VK_BACK: move(0, -1); set_char(0x20); draw(); break;
        case VK_DELETE: set_char(0x20); draw(); break;
        }
    }
    void on_char(wchar_t ch) {
        if (cur.empty() || ch < 0x20 || ch == 0x7F) return;
        auto rev = charset_reverse(P.table(&page()));
        auto it = rev.find((char32_t)ch);
        if (it == rev.end()) { status("the character \xE2\x80\x9C" + from_cp(ch) + "\xE2\x80\x9D is not in this page's character set"); return; }
        if (set_char(it->second)) move(0, 1);
    }
    void action(int i) {
        if (cur.empty()) return;
        if (i == 5) { push(); page().deleted = !page().deleted; touch(); fill_list(); draw(); return; }
        if (!editable()) return;
        auto &V = page().versions;
        if (i == 0) { if (cursor_r == 0) return; push(); Row x; x.fill(0x20); snap_mut().rows[cursor_r] = x; touch(); draw(); }
        else if (i == 1) { if (cursor_r == 0) { status("the header cannot be deleted"); return; } push(); snap_mut().rows.erase(cursor_r); snap_mut().c.erase(cursor_r); touch(); draw(); }
        else if (i == 2) {
            std::string v = "1";
            if (!input(hwnd, "Row from another version", fmt("Version number (1\xE2\x80\x93%zu) to take row %d from:", V.size(), cursor_r), v)) return;
            int k = atoi(v.c_str());
            if (k < 1 || k > (int)V.size()) return;
            auto it = V[k - 1].rows.find(cursor_r);
            if (it == V[k - 1].rows.end()) { status("that version does not have this row"); return; }
            Row src = it->second;
            push(); snap_mut().rows[cursor_r] = src; touch(); draw();
        } else if (i == 3) {
            if (V.size() < 2) { status("the last version cannot be deleted \xE2\x80\x94 delete the page instead"); return; }
            push(); V.erase(V.begin() + std::min(ver, V.size() - 1)); ver = ver ? ver - 1 : 0; touch(); draw();
        } else if (i == 4) { push(); Version c = snap_mut(); V.insert(V.begin() + std::min(ver + 1, V.size()), c); ver++; touch(); draw(); }
    }
    void set_flofs() { if (cur.empty()) return; for (int i = 0; i < 5; i++) set_flof(i); }
    void set_flof(int i) {
        if (kpage.empty() || !pages.count(kpage)) return;
        std::string v = strip(get_text(keys[i]));
        Page &pg = pages[kpage];
        std::vector<std::string> F = pg.ex.flof; F.resize(5);
        if (!v.empty() && !(v.size() == 3 && isdigit((u8)v[0]) && isdigit((u8)v[1]) && isdigit((u8)v[2]) && v[0] >= '1' && v[0] <= '8')) {
            status("page number: three digits, 100\xE2\x80\x93" "899"); set_text(keys[i], F[i]); return;
        }
        if (F[i] == v) return;
        undo.push_back({kpage, pg}); F[i] = v;
        bool any = false; for (auto &x : F) if (!x.empty()) any = true;
        pg.ex.flof = any ? F : std::vector<std::string>();
        dirty = true; edited.insert(kpage); status("there are unsaved changes");
    }
    void do_undo() {
        if (undo.empty()) { status("nothing to undo"); return; }
        auto u = undo.back(); undo.pop_back(); pages[u.first] = u.second;
        if (u.first != cur) open_page(u.first); else draw();
        full_cache.clear(); dirty = true; edited.insert(u.first);
    }

    // ---------------------------------------------------------------- сохранение и экспорт
    void save() {
        if (cur.empty()) return;
        set_flofs();
        try { P.save_state(pages, edited); dirty = false; status("saved to " + P.rel(P.edited_path) + fmt(" (pages changed: %zu)", edited.size())); }
        catch (std::exception &e) { ask(hwnd, "Save", e.what(), MB_ICONERROR); }
    }
    void do_export(bool full) {
        if (cur.empty()) return;
        std::string tag = full ? "_full" : "";
        bool clean = false;
        if (full) {
            if (ask(hwnd, "Export", "Write output_full.t42, output_full.html and the folder html_full/ (complete pages only \xE2\x80\x94 one per subpage) to\n" +
                    P.outdir + "?\nExisting files will be replaced.", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        } else {
            int r = ask(hwnd, "Export", "Write output.t42, output.html and the folder html/ to\n" + P.outdir + "\n(existing files will be replaced).\n\n"
                        "Clean the stream as well?\n"
                        "Yes \xE2\x80\x94 output.t42 gets one assembled copy of every subpage, without repeats and damaged copies (like Clean stream)\n"
                        "No \xE2\x80\x94 every version as it was received", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            clean = r == IDYES;
        }
        if (!edited.empty()) save();
        Pages copy = pages; Project pc = P;
        auto res = std::make_shared<std::string>();
        run_task(hwnd, "Export", [copy, pc, full, clean, res](Progress &pr) { pr.step("exporting"); *res = pc.export_all(copy, full, clean); pr.log(*res); },
                 [this, res](bool ok) { status(ok ? *res : "export error"); });
    }
    void do_squash() {
        if (cur.empty()) return;
        // clean.t42: по одной собранной копии каждой подстраницы, испорченные слова восстановлены; сразу открывается
        if (!edited.empty()) save();
        Pages copy = pages; Project pc = P;
        auto res = std::make_shared<std::string>();
        run_task(hwnd, "Clean stream", [copy, pc, res](Progress &pr) mutable {
                     pr.step("restoring damaged words");
                     auto fx = pc.restore_words(copy);
                     int n = 0; for (auto &f : fx) n += f.n;
                     pr.log(fmt("%d words restored", n));
                     pr.step("assembling one copy of every subpage");
                     *res = pc.squash(copy) + (n ? fmt("; %d damaged words restored", n) : "");
                     pr.log(*res);
                 },
                 [this, res, pc](bool ok) {
                     if (!ok) { status("error while writing clean.t42"); return; }
                     clean_note = *res;
                     show_clean(pc);
                 });
    }
    // очищенный поток — отдельный проект в папке clean/ рядом (тот же набор знаков), открывается в этом окне
    void show_clean(const Project &pc) {
        std::string src = path_join(pc.outdir, "clean.t42"), out = path_join(pc.outdir, "clean");
        std::string cs = pc.charset, cs2 = pc.charset2;
        run_build("Opening the cleaned stream", [src, out, cs, cs2](Progress &pr) {
            build_project(src, out, "", 32, "", pr);
            std::string pj = path_join(out, "project.json");
            Json m = load_json(pj, Json::object());
            m["charset"] = cs; m["charset2"] = cs2.empty() ? Json() : Json(cs2); m["cleaned"] = true;
            save_json(pj, m, 1);
        }, out);
    }
    std::string clean_note;                    // что сделала очистка — показывается, когда очищенный поток открыт
    void do_words() {
        if (cur.empty()) return;
        Pages copy = pages; std::map<std::string, Page> before;
        auto fx = P.restore_words(copy, &before);
        if (fx.empty()) { ask(hwnd, "Restore words", "No damaged words with a confident replacement were found.", MB_ICONINFORMATION); return; }
        int n = 0; for (auto &f : fx) n += f.n;
        std::string txt = fmt("%d words on %zu pages can be restored from the recording's own text:\n\n", n, before.size());
        for (size_t i = 0; i < fx.size() && i < 30; i++) txt += fx[i].page + ":  " + fx[i].from + "  \xE2\x86\x92  " + fx[i].to + (fx[i].n > 1 ? fmt("  (%d)", fx[i].n) : "") + "\n";
        if (fx.size() > 30) txt += fmt("\xE2\x80\xA6 and %zu more\n", fx.size() - 30);
        txt += "\nApply? Every page can be undone (Ctrl+Z).";
        if (ask(hwnd, "Restore words", txt, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        for (auto &b : before) { undo.push_back({b.first, b.second}); pages[b.first] = copy[b.first]; edited.insert(b.first); }
        while (undo.size() > 300) undo.erase(undo.begin());
        full_cache.clear(); dirty = true; draw();
        status(fmt("%d words restored on %zu pages \xE2\x80\x94 there are unsaved changes", n, before.size()));
    }
    void export_srt() {
        if (cur.empty()) return;
        std::vector<std::pair<std::string, int>> c6;
        try { if (exists(P.stream)) c6 = subtitle_pages(P.stream); } catch (...) {}
        std::string hintt;
        if (c6.empty()) hintt = "The stream has no pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6).";
        else { hintt = "Pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6): "; for (size_t i = 0; i < c6.size() && i < 6; i++) hintt += (i ? ", " : "") + c6[i].first + fmt(" (%d)", c6[i].second); }
        std::string page = c6.empty() ? "888" : c6[0].first;
        if (!input(hwnd, "Subtitles .srt", hintt + "\n\nSubtitle page number:", page)) return;
        page = strip(page); for (auto &ch : page) ch = (char)toupper((u8)ch);
        bool okp = page.size() == 3 && page[0] >= '1' && page[0] <= '8';
        for (size_t i = 1; i < page.size() && okp; i++) okp = isxdigit((u8)page[i]);
        if (!okp) { ask(hwnd, "Subtitles .srt", "Page number: three characters, the first is the magazine 1\xE2\x80\x93" "8 (e.g. 888).", MB_ICONERROR); return; }
        Project pc = P; auto res = std::make_shared<std::string>();
        run_task(hwnd, "Subtitles from page " + page, [pc, page, res](Progress &pr) {
            pr.step("subtitles from page " + page);
            auto r = pc.export_srt(page);
            *res = r.second ? fmt("subtitles: %d captions from page ", r.second) + page + " \xE2\x86\x92 " + pc.rel(r.first)
                            : "page " + page + " has no subtitles (no transmitted pages with the C6 flag) \xE2\x80\x94 an empty " + pc.rel(r.first) + " was written";
        }, [this, res](bool ok) { status(ok ? *res : "subtitle export error"); });
    }

    // ---------------------------------------------------------------- карта качества
    void build_quality() {
        quality = load_json(P.quality_path);
        ListView_DeleteAllItems(q_list);
        if (!quality.is_obj()) { set_text(q_title, "No quality map: it is built when a project is built from a .vbi recording."); InvalidateRect(chart.hwnd, nullptr, TRUE); return; }
        set_text(q_title, "Tape quality per minute of " + P.name + " (broadcast time in brackets)");
        int i = 0;
        for (auto &d : quality["minutes"].a) {
            lv_set(q_list, i, 0, std::to_string(d["minute"].integer())); lv_set(q_list, i, 1, d.get_str("air"));
            lv_set(q_list, i, 2, fmt("%.1f", d["received"].num())); lv_set(q_list, i, 3, fmt("%.1f", d["unreadable"].num()));
            lv_set(q_list, i, 4, fmt("%.3f", d["parity"].num())); i++;
        }
        InvalidateRect(chart.hwnd, nullptr, TRUE);
    }

    // ---------------------------------------------------------------- сообщения
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        LRESULT res;
        if (dark_menubar(hwnd, m, w, l, res)) return res;
        switch (m) {
        case WM_CREATE: {
            g_main = hwnd; dark_window(hwnd);
            build_menu(); create_controls(); show_tab();
            // при запуске всегда стартовый экран; прошлые проекты и записи — в списке «Recent»
            SetWindowTextW(hwnd, L"Teletext Rescue");
            show_start();
            status("Open a recording or stream: \xE2\x80\x9C" "File\xE2\x80\x9D menu (.vbi, .t42 625-line WST, .t34 525-line WST, .t33 NABTS, DVB .ts) or an existing project");
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: { auto *mm = (MINMAXINFO *)l; mm->ptMinTrackSize = {S(hwnd, 1000), S(hwnd, 760)}; return 0; }
        case WM_DPICHANGED: { RECT *r = (RECT *)l; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER); return 0; }
        case WM_TIMER: if (w == 1) { flash_on = !flash_on; draw(true); } return 0;
        case WM_APP_CALL: { auto *f = (std::function<void()> *)l; (*f)(); delete f; return 0; }
        case WM_DROPFILES: {
            wchar_t buf[MAX_PATH]; DragQueryFileW((HDROP)w, 0, buf, MAX_PATH); DragFinish((HDROP)w);
            std::string f = N(buf);
            if (is_dir(f) && Project::is_project(f)) { if (can_leave()) load_project(f); }
            else if (ends_with_i(f, ".vbi") || ends_with_i(f, ".flac")) { if (can_leave()) open_vbi(f); }
            else open_stream(f);
            return 0;
        }
        case WM_INITMENUPOPUP: if ((HMENU)w == rmenu) fill_recent(); return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l); int lw = left_w;
            if (abs(x - (lw + S(hwnd, 4))) < S(hwnd, 6)) { dragging = true; SetCapture(hwnd); }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l);
            if (dragging) { left_w = std::max(S(hwnd, 300), x - S(hwnd, 4)); layout(); InvalidateRect(hwnd, nullptr, TRUE); }
            else if (abs(x - (left_w + S(hwnd, 4))) < S(hwnd, 6)) SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return 0;
        }
        case WM_LBUTTONUP: if (dragging) { dragging = false; ReleaseCapture(); } return 0;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == list) {
                if (nh->code == LVN_ITEMCHANGED) {
                    auto *nv = (NMLISTVIEW *)l;
                    if ((nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED) && nv->iItem < (int)list_ids.size() && list_ids[nv->iItem] != cur)
                        open_page(list_ids[nv->iItem]);
                } else if (nh->code == LVN_COLUMNCLICK) {
                    static const char *keys_[5] = {"p", "t", "tx", "v", "n"};
                    sort_key = keys_[((NMLISTVIEW *)l)->iSubItem]; fill_list();
                } else if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l, [this](int r) { return row_colour(r); });
            } else if (nh->hwndFrom == q_list && nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            for (int i = 0; i < 5; i++) if ((HWND)l == keys[i]) {
                static HBRUSH kb[5] = {0};
                if (!kb[i]) kb[i] = CreateSolidBrush(KEYC[i]);
                SetTextColor((HDC)w, RGB(0, 0, 0)); SetBkColor((HDC)w, KEYC[i]); return (LRESULT)kb[i];
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == ID_SEARCH && code == EN_CHANGE) { if (has_project) fill_list(); return 0; }
            if (id >= ID_KEY0 && id < ID_KEY0 + 5 && code == EN_KILLFOCUS) { set_flof(id - ID_KEY0); return 0; }
            if (code == EN_CHANGE || code == EN_SETFOCUS || code == EN_KILLFOCUS || code == EN_UPDATE) return 0;
            if (id >= M_RECENT0 && id < M_RECENT0 + 20) { auto rec = recent_list(); if (id - M_RECENT0 < (int)rec.size()) open_recent(rec[id - M_RECENT0]); return 0; }
            if (id >= M_CS0 && id < M_CS0 + (int)CHARSET_NAMES.size()) {
                if (!has_project) return 0;
                P.set_charset(CHARSET_NAMES[id - M_CS0].first); update_charset_menu(); fill_list(); draw();
                status("character set: " + CHARSET_NAMES[id - M_CS0].second + " \xE2\x80\x94 saved in the project; run Export to update the HTML"); return 0;
            }
            if (id >= M_CS2_0 && id < M_CS2_0 + (int)CS2.size()) {
                if (!has_project) return 0;
                P.set_charset2(CS2[id - M_CS2_0].first); update_charset_menu(); fill_list(); draw();
                status("second set (ESC): " + CS2[id - M_CS2_0].second + " \xE2\x80\x94 saved in the project"); return 0;
            }
            if (id >= ID_ACT0 && id < ID_ACT0 + 6) { action(id - ID_ACT0); return 0; }
            if (id >= ID_PAL0 && id < ID_PAL0 + 14) { int i = id - ID_PAL0; insert(i < 7 ? i + 1 : 0x10 + i - 6); return 0; }
            if (id >= ID_PAL2_0 && id < ID_PAL2_0 + 7) { static const int c[7] = {0x1d, 0x1c, 0x0d, 0x0c, 0x19, 0x1a, 0x18}; insert(c[id - ID_PAL2_0]); return 0; }
            switch (id) {
            case ID_SAVE: case M_SAVE: save(); break;
            case ID_EXPORT: case M_EXPORT: do_export(false); break;
            case M_EXPORT_FULL: do_export(true); break;
            case ID_SQUASH: case M_SQUASH: do_squash(); break;
            case ID_WORDS: case M_WORDS: do_words(); break;
            case M_DEVICE: pick_device(true); break;
            case M_EXPORT_SRT: export_srt(); break;
            case M_OPEN_FOLDER: if (has_project) open_path(P.outdir); break;
            case ID_UNDO: do_undo(); break;
            case ID_OPENHTML: if (has_project) { std::string p = path_join(path_join(P.outdir, "html"), "index.html"); if (exists(p)) open_path(p); else status("html/index.html does not exist yet \xE2\x80\x94 run Export first"); } break;
            case ID_TAB_PAGE: tab_quality = false; show_tab(); break;
            case ID_TAB_QUAL: tab_quality = true; show_tab(); break;
            case ID_PREV: step(-1); break; case ID_NEXT: step(1); break;
            case ID_VPREV: step_ver(-1); break; case ID_VNEXT: step_ver(1); break;
            case ID_REVEAL: case ID_TV: draw(); break;
            case ID_FULL: ver = 0; draw(); break;
            case M_OPEN_STREAM: menu_stream(); break;
            case M_OPEN_T33: menu_t33(); break;
            case M_OPEN_VBI: menu_vbi(); break;
            case M_OPEN_PROJECT: menu_project(); break;
            case M_START: show_start(); break;
            case M_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            case M_ABOUT: ask(hwnd, "About", "Teletext Rescue 1.1\nCopyright (c) 2026 AssunaYuuki.\nLicensed under the GNU General Public License v3.0.\n\n"
                                            "Recovers teletext, NABTS, Silent Radio, captions, AMOL, VITC and other data hidden in VBI recordings.", MB_ICONINFORMATION); break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (!cur.empty()) set_flofs();
            if (dirty) {
                int r = ask(hwnd, "Exit", "Save changes before exiting?", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) return 0;
                if (r == IDYES) save();
            }
            DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return Window::proc(m, w, l);
    }
    bool pre_translate(MSG &msg) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'S') { save(); return true; }
            if (msg.wParam == 'Z' && GetForegroundWindow() == hwnd) { HWND f = GetFocus(); if (f == screen.hwnd || f == list || !f || GetParent(f) == hwnd) { if (f != search && f != pnum) { do_undo(); return true; } } }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (msg.hwnd == pnum) { open_page(strip(get_text(pnum))); return true; }
            for (int i = 0; i < 5; i++) if (msg.hwnd == keys[i]) { set_flof(i); return true; }
        }
        if (msg.message == WM_LBUTTONDBLCLK) for (int i = 0; i < 5; i++) if (msg.hwnd == keys[i]) { set_flof(i); std::string v = strip(get_text(keys[i])); if (!v.empty()) open_page(v); return true; }
        return false;
    }
};

LRESULT Screen::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        if (app->has_project && !app->cur.empty()) {
            Image im = app->current_image();
            blit(dc, im, 0, 0, r.right, r.bottom, true);
            if (GetFocus() == hwnd) { HBRUSH b = CreateSolidBrush(ACCENT); FrameRect(dc, &r, b); DeleteObject(b); }
        } else fill(dc, r, BG);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        if (!app->has_project) return 0;
        RECT r; GetClientRect(hwnd, &r);
        app->cursor_r = std::min(24, (int)(GET_Y_LPARAM(l) * 25 / std::max(1L, r.bottom)));
        app->cursor_c = std::min(39, (int)(GET_X_LPARAM(l) * 40 / std::max(1L, r.right)));
        app->draw();
        return 0;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
    case WM_KEYDOWN: if (!(GetKeyState(VK_CONTROL) & 0x8000)) app->on_key(w); return 0;
    case WM_CHAR: if (!(GetKeyState(VK_CONTROL) & 0x8000) && w != '\r' && w != '\b') app->on_char((wchar_t)w); return 0;
    }
    return Window::proc(m, w, l);
}

LRESULT Chart::proc(UINT m, WPARAM w, LPARAM l) {
    static const char *titles[3] = {"Teletext lines not received, %", "Of these unreadable (signal present), %", "Parity errors, % of bytes"};
    auto rects = [&](RECT rc) {
        std::array<RECT, 3> rs; int gap = S(hwnd, 12), cw = (rc.right - 2 * gap) / 3;
        for (int i = 0; i < 3; i++) rs[i] = {i * (cw + gap), S(hwnd, 22), i * (cw + gap) + cw, rc.bottom};
        return rs;
    };
    auto values = [&](int k) {
        std::vector<double> v;
        for (auto &d : app->quality["minutes"].a) v.push_back(k == 0 ? round((100 - d["received"].num()) * 100) / 100 : k == 1 ? d["unreadable"].num() : d["parity"].num());
        return v;
    };
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
        fill(mem, rc, BG);
        if (app->quality.is_obj()) {
            auto rs = rects(rc);
            std::vector<std::string> labels;
            for (auto &d : app->quality["minutes"].a) labels.push_back(std::to_string(d["minute"].integer()));
            for (int k = 0; k < 3; k++) {
                text_out(mem, rs[k].left, 0, titles[k], FG, font(hwnd));
                bar_chart(mem, rs[k], values(k), labels, hwnd);
            }
            if (hover_chart >= 0 && hover_i >= 0) {
                auto &d = app->quality["minutes"][hover_i];
                std::string t = fmt("minute %d (%s): %.3f", d["minute"].integer(), d.get_str("air").c_str(), values(hover_chart)[hover_i]);
                RECT tr{rs[hover_chart].left + S(hwnd, 44), S(hwnd, 26), rs[hover_chart].right - 4, S(hwnd, 46)};
                fill(mem, tr, FIELD);
                text_out(mem, tr.left + 4, tr.top + 2, t, FG, font(hwnd, 9));
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        if (!app->quality.is_obj()) return 0;
        RECT rc; GetClientRect(hwnd, &rc); auto rs = rects(rc);
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hc = -1, hi = -1;
        for (int k = 0; k < 3; k++) { int i = bar_hit(rs[k], app->quality["minutes"].size(), p, hwnd); if (i >= 0) { hc = k; hi = i; } }
        if (hc != hover_chart || hi != hover_i) { hover_chart = hc; hover_i = hi; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0}; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: hover_chart = hover_i = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return Window::proc(m, w, l);
}
}  // namespace

// журнал падений: стек вызовов в %TEMP%\TeletextRescue_crash.txt (адреса относительно начала exe)
static void crash_log(const char *what) {
    void *st[64]; USHORT n = CaptureStackBackTrace(0, 64, st, nullptr);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::string path = narrow(tmp) + "TeletextRescue_crash.txt";
    FILE *f = _wfopen(widen(path).c_str(), L"a");
    if (!f) return;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    fprintf(f, "=== %s %s\n", now_str("%Y-%m-%d %H:%M:%S").c_str(), what);
    for (USHORT i = 0; i < n; i++) fprintf(f, "  0x%llx\n", (unsigned long long)((uintptr_t)st[i] - base));
    fclose(f);
}
static LONG WINAPI seh_handler(EXCEPTION_POINTERS *e) {
    char b[64]; snprintf(b, sizeof b, "exception 0x%08lx at +0x%llx", e->ExceptionRecord->ExceptionCode,
                         (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(nullptr)));
    crash_log(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    std::set_terminate([] {
        std::string w = "terminate";
        if (auto e = std::current_exception()) { try { std::rethrow_exception(e); } catch (std::exception &x) { w += std::string(": ") + x.what(); } catch (...) {} }
        crash_log(w.c_str());
        abort();
    });
    SetUnhandledExceptionFilter(seh_handler);
    ui::init(hi);
    { Json st = load_state(); if (!st["device"].is_null()) gpu::use(st["device"].integer()); }
    App app; g_app = &app;
    app.create(L"TRMain", "Teletext Rescue", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 1320, 920);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = std::min(S(app.hwnd, 1320), (int)(wa.right - wa.left)), h = std::min(S(app.hwnd, 920), (int)(wa.bottom - wa.top));
    SetWindowPos(app.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER);
    // открыть файл, переданный в командной строке (перетаскивание на значок)
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    ShowWindow(app.hwnd, show);
    if (argc > 2 && narrow(argv[1]) == "--sign") sign_window(app.hwnd, narrow(argv[2]));
    else if (argc > 1) {
        std::string f = narrow(argv[1]);
        if (is_dir(f)) app.load_project(f); else if (ends_with_i(f, ".vbi") || ends_with_i(f, ".flac")) app.open_vbi(f); else app.open_stream(f);
    }
    LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (app.pre_translate(msg)) continue;
        HWND top = GetAncestor(msg.hwnd, GA_ROOT);
        if (top && top != app.hwnd && IsDialogMessageW(top, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
