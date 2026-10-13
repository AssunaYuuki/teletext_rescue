// Teletext window: the page editor of the original Teletext Rescue, whole, for the pages built from
// the recording (625 / 525-line VBI, DVB .ts, .t42 / .t34 / .t33 streams):
//   left   - page list with search, sortable columns (page, section, received, versions, note),
//            edited pages blue, service pages accented, deleted grey; the divider can be dragged;
//   right  - tabs "Page" and "Tape quality":
//            Page: page / version navigation, concealed text, complete page (assembled from versions),
//                  "as on a TV screen", the SAA5050 screen with a cursor (click and type, arrows,
//                  Backspace / Delete, Enter, PageUp / PageDown), FLOF colour keys (edit, double-click
//                  to go), row / version / page actions, the text and mosaic colour palettes and the
//                  attribute codes, row / column / code of the cursor;
//            Tape quality: received / unreadable / parity errors per minute (charts and table);
//   menus  - File (save, export output.t42 + HTML, complete pages only, subtitles .srt, PNG, folder),
//            Character set (G0 set and the second set selected by ESC), Tools (clean stream, restore
//            words with the built-in dictionaries, the recording's own words and Yandex Speller).
// Ctrl+S saves, Ctrl+Z undoes (300 steps); edits go to pages_edited.json of the project.
#include "gui_app.h"
#include "pagebuild.h"
#include "project.h"
#include "ttpages.h"
#include "teletext.h"
#include <array>
#include <uxtheme.h>
#include <windowsx.h>

namespace ui {
std::map<HWND, std::function<bool(MSG &)>> &pretranslators()
{
    static std::map<HWND, std::function<bool(MSG &)>> m;
    return m;
}

// bar chart of the quality map (from the original program)
static const int CL = 40, CB = 24, CT = 10, CR = 8;
void bar_chart(HDC dc, RECT r, const std::vector<double> &vals, const std::vector<std::string> &labels, HWND ref)
{
    fill(dc, r, PANEL);
    int L = S(ref, CL), B = S(ref, CB), T = S(ref, CT), R = S(ref, CR);
    int W = r.right - r.left, H = r.bottom - r.top;
    size_t n = vals.size();
    if (!n || W < L + R + 10 || H < T + B + 10) return;
    double hi = 0; for (double v : vals) hi = std::max(hi, v);
    hi = hi * 1.15; if (hi <= 0) hi = 1;
    auto y = [&](double v) { return r.top + T + (int)((H - T - B) * (1 - v / hi)); };
    HFONT f = font(ref, 8);
    HPEN grid = CreatePen(PS_SOLID, 1, LINE), axis = CreatePen(PS_SOLID, 1, MUTED);
    HGDIOBJ op = SelectObject(dc, grid);
    for (int i = 0; i <= 4; i++) {
        double v = hi * i / 4; int yy = y(v);
        MoveToEx(dc, r.left + L, yy, nullptr); LineTo(dc, r.right - R, yy);
        text_out(dc, r.left + L - 4, yy - S(ref, 7), hi < 2 ? fmt("%.2f", v) : fmt("%.1f", v), MUTED, f, TA_RIGHT);
    }
    double bw = (double)(W - L - R) / n;
    for (size_t i = 0; i < n; i++) {
        int x0 = r.left + L + (int)(i * bw) + 1, x1 = r.left + L + (int)((i + 1) * bw) - 1;
        RECT b{x0, y(vals[i]), std::max(x0 + 1, x1), r.bottom - B};
        fill(dc, b, ACCENT);
        if (n <= 24 || i % ((n + 23) / 24) == 0) text_out(dc, (x0 + x1) / 2, r.bottom - B + 2, i < labels.size() ? labels[i] : "", MUTED, f, TA_CENTER);
    }
    SelectObject(dc, axis);
    MoveToEx(dc, r.left + L, r.bottom - B, nullptr); LineTo(dc, r.right - R, r.bottom - B);
    SelectObject(dc, op); DeleteObject(grid); DeleteObject(axis);
}
int bar_hit(RECT r, size_t n, POINT p, HWND ref)
{
    int L = S(ref, CL), R = S(ref, CR);
    if (!n || p.x < r.left + L || p.x >= r.right - R || p.y < r.top || p.y >= r.bottom) return -1;
    double bw = (double)(r.right - r.left - L - R) / n;
    int i = (int)((p.x - r.left - L) / bw);
    return i >= 0 && i < (int)n ? i : -1;
}

namespace {
enum {
    ID_SEARCH = 100, ID_LIST, ID_SAVE, ID_EXPORT, ID_UNDO, ID_OPENHTML, ID_TAB_PAGE, ID_TAB_QUAL, ID_SQUASH, ID_WORDS,
    ID_PREV, ID_PNUM, ID_NEXT, ID_VPREV, ID_VNEXT, ID_REVEAL, ID_FULL, ID_TV, ID_CODES,
    ID_KEY0 = 130, ID_ACT0 = 140, ID_PAL0 = 160, ID_PAL2_0 = 180, ID_QLIST = 200,
    M_SAVE = 1000, M_EXPORT, M_EXPORT_FULL, M_EXPORT_SRT, M_PNG, M_OPEN_FOLDER, M_CLOSE, M_SQUASH, M_WORDS,
    M_CS0 = 1200, M_CS2_0 = 1300,
};
const COLORREF TT[8] = {RGB(0, 0, 0), RGB(255, 0, 0), RGB(0, 255, 0), RGB(255, 255, 0), RGB(0, 0, 255), RGB(255, 0, 255), RGB(0, 255, 255), RGB(255, 255, 255)};
const COLORREF KEYC[5] = {RGB(0xdd, 0x22, 0x22), RGB(0x22, 0xaa, 0x22), RGB(0xcc, 0xcc, 0x22), RGB(0x22, 0xcc, 0xcc), RGB(0x99, 0x99, 0x99)};
const std::vector<std::pair<std::string, std::string>> CS2 = {{"", "none"}, {"latin", "Latin"}, {"cyr2", "Cyrillic (Russian)"}, {"cyr1", "Cyrillic (Serbian)"}, {"cyr3", "Cyrillic (Ukrainian)"}};

struct TeletextWin;
struct Screen : Window {   // the page screen
    TeletextWin *app = nullptr;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};
struct Chart : Window {    // the three charts of the quality map
    TeletextWin *app = nullptr;
    int hover_chart = -1, hover_i = -1;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};

struct TeletextWin : Window {
    ResultPtr R;
    std::string projDir;                       // the project shown: the recording's or its cleaned copy
    Project P; bool has_project = false;
    Pages pages; std::vector<std::string> ids; std::string cur; size_t ver = 0;
    std::set<std::string> edited; bool dirty = false;
    std::vector<std::pair<std::string, Page>> undo;
    int cursor_r = 1, cursor_c = 0; bool flash_on = true, last_flash = false, last_box = false;
    std::map<std::string, std::vector<Version>> full_cache;
    std::string sort_key = "p"; std::vector<std::string> list_ids;
    std::string kpage, clean_note;
    int left_w = 0; bool dragging = false, tab_quality = false;
    Json quality;
    HMENU menu = nullptr, csmenu = nullptr;
    HWND b_save, b_export, b_undo, b_html, b_squash, b_words, msg, search, hint, list, tab_page, tab_qual;
    HWND b_prev, pnum, b_next, b_vprev, vinfo, b_vnext, c_reveal, c_full, c_tv, c_codes, note;
    Screen screen; Chart chart;
    HWND keys_l, keys[5], keys_hint, acts[6], pal[14], pal_l1, pal_l2, pal2[7], stat, help;
    HWND q_title, q_list, q_help;

    // ---------------------------------------------------------------- creation
    void build_menu()
    {
        menu = CreateMenu();
        HMENU fm = CreatePopupMenu();
        AppendMenuW(fm, MF_STRING, M_SAVE, L"Save\tCtrl+S");
        AppendMenuW(fm, MF_STRING, M_EXPORT, L"Export \x2192 output.t42 + HTML");
        AppendMenuW(fm, MF_STRING, M_EXPORT_FULL, L"Export complete pages only \x2192 output_full.t42 + HTML");
        AppendMenuW(fm, MF_STRING, M_EXPORT_SRT, L"Export subtitles .srt\x2026");
        AppendMenuW(fm, MF_STRING, M_PNG, L"Save page as PNG\x2026");
        AppendMenuW(fm, MF_STRING, M_OPEN_FOLDER, L"Open export folder");
        AppendMenuW(fm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fm, MF_STRING, M_CLOSE, L"Close");
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
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)tm, L"Tools");
        SetMenu(hwnd, menu);
    }
    void create_controls()
    {
        b_save = button(hwnd, ID_SAVE, "Save (Ctrl+S)"); b_export = button(hwnd, ID_EXPORT, "Export \xE2\x86\x92 output.t42 + HTML");
        b_undo = button(hwnd, ID_UNDO, "Undo (Ctrl+Z)"); b_html = button(hwnd, ID_OPENHTML, "Open html/index.html");
        b_squash = button(hwnd, ID_SQUASH, "Clean stream"); b_words = button(hwnd, ID_WORDS, "Restore words\xE2\x80\xA6");
        msg = label(hwnd, ""); set_label_colour(msg, MUTED);
        search = edit(hwnd, ID_SEARCH);
        hint = label(hwnd, "search by number or title"); set_label_colour(hint, DIM);
        list = listview(hwnd, ID_LIST, {{"Page", 50}, {"Section", 200}, {"Received", -66}, {"Versions", -60}, {"Note", 80}});
        tab_page = button(hwnd, ID_TAB_PAGE, "Page"); tab_qual = button(hwnd, ID_TAB_QUAL, "Tape quality");
        b_prev = button(hwnd, ID_PREV, "\xE2\x97\x80 page"); pnum = edit(hwnd, ID_PNUM, "", ES_CENTER); b_next = button(hwnd, ID_NEXT, "page \xE2\x96\xB6");
        b_vprev = button(hwnd, ID_VPREV, "\xE2\x97\x80"); vinfo = label(hwnd, "", 0, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS); b_vnext = button(hwnd, ID_VNEXT, "\xE2\x96\xB6");
        c_reveal = button(hwnd, ID_REVEAL, "concealed text", BTN_CHECK); c_full = button(hwnd, ID_FULL, "complete page", BTN_CHECK); c_tv = button(hwnd, ID_TV, "as on a TV screen", BTN_CHECK); c_codes = button(hwnd, ID_CODES, "show control codes", BTN_CHECK);
        note = label(hwnd, ""); set_label_colour(note, ACCENT);
        Window::register_class(L"TRTtScreen", brBG);
        screen.app = this;
        CreateWindowExW(0, L"TRTtScreen", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd, nullptr, inst(), &screen);
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
        Window::register_class(L"TRTtChart", brBG);
        chart.app = this;
        CreateWindowExW(0, L"TRTtChart", L"", WS_CHILD, 0, 0, 10, 10, hwnd, nullptr, inst(), &chart);
        q_list = listview(hwnd, ID_QLIST, {{"Recording minute", -130}, {"Broadcast", 170}, {"Lines received, %", -140}, {"Unreadable, %", -120}, {"Parity errors, %", -130}});
        q_help = label(hwnd, "Received \xE2\x80\x94 share of teletext VBI lines that yielded a packet; unreadable \xE2\x80\x94 signal present but no decoder got a packet; errors \xE2\x80\x94 bytes with bad parity in received packets.");
        set_label_colour(q_help, DIM);
        left_w = S(hwnd, 470);
    }
    std::vector<HWND> page_ctrls()
    {
        std::vector<HWND> v = {b_prev, pnum, b_next, b_vprev, vinfo, b_vnext, c_reveal, c_full, c_tv, c_codes, note, screen.hwnd, keys_l, keys_hint, stat, help, pal_l1, pal_l2};
        for (auto h : keys) v.push_back(h);
        for (auto h : acts) v.push_back(h);
        for (auto h : pal) v.push_back(h);
        for (auto h : pal2) v.push_back(h);
        return v;
    }
    void show_tab()
    {
        for (HWND h : page_ctrls()) ShowWindow(h, tab_quality ? SW_HIDE : SW_SHOW);
        for (HWND h : {q_title, chart.hwnd, q_list, q_help}) ShowWindow(h, tab_quality ? SW_SHOW : SW_HIDE);
        set_button_style(tab_page, tab_quality ? BTN_NORMAL : BTN_ACCENT);
        set_button_style(tab_qual, tab_quality ? BTN_ACCENT : BTN_NORMAL);
        layout();
    }
    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int p = S(hwnd, 8), bh = S(hwnd, 30), x = p, y = p;
        auto place = [&](HWND h, int w, int hh = 0) { MoveWindow(h, x, y, w, hh ? hh : bh, TRUE); x += w + S(hwnd, 6); };
        HDC dc = GetDC(hwnd); HGDIOBJ of = SelectObject(dc, font(hwnd));
        auto tw = [&](HWND h) { std::wstring t = W(get_text(h)); SIZE s; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &s); return (int)s.cx + S(hwnd, 24); };
        place(b_save, tw(b_save)); place(b_export, tw(b_export)); place(b_undo, tw(b_undo)); place(b_html, tw(b_html));
        place(b_squash, tw(b_squash)); place(b_words, tw(b_words));
        MoveWindow(msg, x + p, y + S(hwnd, 6), std::max(0, (int)rc.right - x - 2 * p), S(hwnd, 20), TRUE);
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
            // rows of buttons that wrap: measure first, then place
            auto flow = [&](const std::vector<std::pair<HWND, int>> &items, int y0, bool put) {
                int fx = rx, fy = y0, rowh = S(hwnd, 34);
                for (auto &it : items) {
                    if (fx > rx && fx + it.second > rx + rw) { fx = rx; fy += rowh; }
                    if (put) MoveWindow(it.first, fx, fy, it.second, it.first == keys_l || it.first == keys_hint || it.first == pal_l1 || it.first == pal_l2 ? S(hwnd, 24) : bh, TRUE);
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
            place(c_reveal, tw(c_reveal) + S(hwnd, 6)); place(c_full, tw(c_full) + S(hwnd, 6)); place(c_tv, tw(c_tv) + S(hwnd, 6)); place(c_codes, tw(c_codes) + S(hwnd, 6));
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

    // ---------------------------------------------------------------- project
    void status(const std::string &t) { set_text(msg, t); }
    bool load_project(const std::string &path)
    {
        Project np;
        if (!np.open(path)) { status("the teletext project is missing: " + path); return false; }
        Pages pg; std::set<std::string> ed; std::string src;
        try { pg = np.load_state(ed, src); if (pg.empty()) throw std::runtime_error("the project has no pages"); }
        catch (std::exception &e) { status(e.what()); return false; }
        projDir = path;
        P = np; has_project = true; pages = pg; edited = ed; dirty = false; undo.clear(); full_cache.clear();
        ids.clear(); for (auto &kv : pages) ids.push_back(kv.first);
        cur = pages.count("100") ? "100" : ids[0]; ver = 0; cursor_r = 1; cursor_c = 0; kpage.clear();
        SetWindowTextW(hwnd, W("Teletext \xE2\x80\x94 " + file_name(*R) + "  \xC2\xB7  " + P.system + (path != R->teletext.project ? "  \xC2\xB7  cleaned" : "")).c_str());
        update_charset_menu();
        fill_list(); open_page(cur); build_quality();
        std::string sl = P.service_line();
        status(fmt("%zu pages \xC2\xB7 ", ids.size()) + P.system + (sl.empty() ? "" : " \xC2\xB7 " + sl));
        return true;
    }
    void update_charset_menu()
    {
        for (size_t i = 0; i < CHARSET_NAMES.size(); i++) CheckMenuItem(csmenu, M_CS0 + i, CHARSET_NAMES[i].first == P.charset ? MF_CHECKED : MF_UNCHECKED);
        for (size_t i = 0; i < CS2.size(); i++) CheckMenuItem(csmenu, M_CS2_0 + i, CS2[i].first == P.charset2 ? MF_CHECKED : MF_UNCHECKED);
    }
    bool can_leave()
    {
        if (!dirty) return true;
        int r = ask(hwnd, "Unsaved edits", "Save the edited pages?", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) save();
        return true;
    }

    // ---------------------------------------------------------------- list
    void fill_list()
    {
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
    void select_in_list()
    {
        auto it = std::find(list_ids.begin(), list_ids.end(), cur);
        if (it == list_ids.end()) return;
        int i = (int)(it - list_ids.begin());
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, i, FALSE);
    }
    COLORREF row_colour(int row)
    {
        if (row < 0 || row >= (int)list_ids.size()) return FG;
        auto &p = list_ids[row]; auto &pg = pages[p];
        if (pg.deleted) return RGB(0x99, 0x99, 0x99);
        if (edited.count(p)) return RGB(0x5a, 0xa9, 0xff);
        if (pg.ex.boxed) return ACCENT;
        return FG;
    }

    // ---------------------------------------------------------------- navigation and view
    Page &page() { return pages[cur]; }
    const std::vector<Version> &versions()
    {
        Page &pg = page();
        if (!checked(c_full)) return pg.versions;
        auto it = full_cache.find(cur);
        if (it == full_cache.end()) it = full_cache.emplace(cur, P.full_versions(pg)).first;
        return it->second;
    }
    void open_page(const std::string &p)
    {
        if (!pages.count(p)) { status("there is no page " + p); return; }
        set_flofs();
        cur = p; ver = 0; cursor_r = 1; cursor_c = 0;
        select_in_list();
        draw(); SetFocus(screen.hwnd);
    }
    void step(int d)
    {
        if (cur.empty() || ids.empty()) return;
        auto it = std::find(ids.begin(), ids.end(), cur);
        int i = (int)(it - ids.begin()); i = (i + d + (int)ids.size()) % (int)ids.size();
        open_page(ids[i]);
    }
    void step_ver(int d) { if (cur.empty()) return; size_t n = versions().size(); if (!n) return; ver = (ver + d + n) % n; draw(); }
    Version &snap_mut() { auto &V = page().versions; return V[std::min(ver, V.size() - 1)]; }
    const Version &snap() { auto &V = versions(); return V[std::min(ver, V.size() - 1)]; }
    bool editable() { if (checked(c_full)) { status("the complete page is assembled from versions \xE2\x80\x94 untick \xE2\x80\x9C" "complete page\xE2\x80\x9D to edit"); return false; } return true; }
    Image current_image()
    {
        const Version &s = snap(); Page &pg = page();
        const Charset &t = P.table(&pg);
        Over x = P.fitted(s.rows, P.overlay(pg, &s), t);
        RenderOpts o; o.reveal = checked(c_reveal); o.flash_on = flash_on; o.tv = checked(c_tv); o.codes = checked(c_codes); o.cursor_r = cursor_r; o.cursor_c = cursor_c;
        return render_teletext(s.rows, t, P.table2(), &x, o, &last_flash, &last_box);
    }
    void draw(bool keep_flash = false)
    {
        if (cur.empty() || !has_project || page().versions.empty()) return;
        if (!keep_flash) flash_on = true;
        InvalidateRect(screen.hwnd, nullptr, FALSE);
        KillTimer(hwnd, 1);
        current_image();      // refresh last_flash / last_box
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
                           fmt(" \xC2\xB7 received %d times, subpages %d", pg.ex.tx, pg.ex.subpages) +
                           (s.t > 0 ? "  \xC2\xB7  " + rec_time(s.t) + " into the recording" : ""));
    }

    // ---------------------------------------------------------------- editing
    void push() { undo.push_back({cur, page()}); if (undo.size() > 300) undo.erase(undo.begin()); }
    void touch() { full_cache.clear(); dirty = true; edited.insert(cur); status("there are unsaved changes (Ctrl+S saves, Ctrl+Z undoes)"); InvalidateRect(list, nullptr, FALSE); }
    Row &row(int r) { auto &s = snap_mut(); if (!s.rows.count(r)) { Row x; x.fill(0x20); s.rows[r] = x; } return s.rows[r]; }
    bool set_char(int code)
    {
        if (!editable()) return false;
        if (cursor_r == 0 && cursor_c < 8) { status("the first 8 positions of the header are filled automatically"); return false; }
        push(); row(cursor_r)[cursor_c] = (uint8_t)code; touch(); return true;
    }
    void move(int dr, int dc)
    {
        int r = cursor_r + dr, c = cursor_c + dc;
        if (c > 39) { c = 0; r++; }
        if (c < 0) { c = 39; r--; }
        cursor_r = std::max(0, std::min(24, r)); cursor_c = c; draw();
    }
    void insert(int code) { if (cur.empty()) return; if (set_char(code)) move(0, 1); SetFocus(screen.hwnd); }
    void on_key(WPARAM vk)
    {
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
    void on_char(wchar_t ch)
    {
        if (cur.empty() || ch < 0x20 || ch == 0x7F) return;
        auto rev = charset_reverse(P.table(&page()));
        auto it = rev.find((char32_t)ch);
        if (it == rev.end()) { status("the character \xE2\x80\x9C" + from_cp(ch) + "\xE2\x80\x9D is not in this page's character set"); return; }
        if (set_char(it->second)) move(0, 1);
    }
    void action(int i)
    {
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
    void set_flof(int i)
    {
        if (kpage.empty() || !pages.count(kpage)) return;
        std::string v = strip(get_text(keys[i]));
        Page &pg = pages[kpage];
        std::vector<std::string> F = pg.ex.flof; F.resize(5);
        if (!v.empty() && !(v.size() == 3 && isdigit((uint8_t)v[0]) && isdigit((uint8_t)v[1]) && isdigit((uint8_t)v[2]) && v[0] >= '1' && v[0] <= '8')) {
            status("page number: three digits, 100\xE2\x80\x93" "899"); set_text(keys[i], F[i]); return;
        }
        if (F[i] == v) return;
        undo.push_back({kpage, pg}); F[i] = v;
        bool any = false; for (auto &x : F) if (!x.empty()) any = true;
        pg.ex.flof = any ? F : std::vector<std::string>();
        dirty = true; edited.insert(kpage); status("there are unsaved changes");
    }
    void do_undo()
    {
        if (undo.empty()) { status("nothing to undo"); return; }
        auto u = undo.back(); undo.pop_back(); pages[u.first] = u.second;
        full_cache.clear(); dirty = true; edited.insert(u.first);
        if (u.first != cur) open_page(u.first); else draw();
    }

    // ---------------------------------------------------------------- saving and export
    void save()
    {
        if (cur.empty()) return;
        set_flofs();
        try { P.save_state(pages, edited); dirty = false; status("saved to " + P.rel(P.edited_path) + fmt(" (pages changed: %zu)", edited.size())); }
        catch (std::exception &e) { ask(hwnd, "Save", e.what(), MB_ICONERROR); }
    }
    void do_export(bool full)
    {
        if (cur.empty()) return;
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
    void save_png()
    {
        if (cur.empty()) return;
        Image im = current_image();
        std::string f = save_file(hwnd, "Save page as PNG", basename(stem_path(R->path)) + "_" + cur + fmt("_v%zu.png", ver + 1), "png");
        if (!f.empty()) { png_save(f, im.scaled(im.w * 2, im.h * 2)); status("saved: " + f); }
    }
    // Clean stream (as vhs-teletext "squash"): damaged words restored, one copy of every subpage
    // assembled by majority, garbage versions dropped -> clean.t42, built into "clean/" and shown here;
    // pressed again it goes back to the pages as received
    void do_squash()
    {
        if (cur.empty()) return;
        if (projDir != R->teletext.project) {
            if (!can_leave()) return;
            load_project(R->teletext.project); set_text(b_squash, "Clean stream"); layout(); return;
        }
        if (!edited.empty()) save();
        auto res = std::make_shared<std::string>();
        auto out = std::make_shared<std::string>();
        std::string dir = projDir;
        run_task(hwnd, "Clean stream", [dir, res, out](Progress &pr) { *out = ttClean(dir, pr, *res); pr.log(*res); },
                 [this, res, out](bool ok) {
                     if (!ok) { status("cleaning failed"); return; }
                     if (load_project(*out)) { status("cleaned: " + *res); set_text(b_squash, "Show as received"); layout(); }
                 });
    }
    // restore damaged words: built-in dictionaries, the recording's own words, Yandex Speller;
    // a replacement is taken only when every changed letter is a 1-2 bit error
    void do_words()
    {
        if (cur.empty()) return;
        int which = choose(hwnd, "Restore words", "Find damaged words (built-in dictionary, words of this recording and Yandex Speller) on:",
                           {"This page (" + cur + ")", "All pages"}, 0);
        if (which < 0) return;
        std::set<std::string> one{cur};
        Pages copy = pages; std::map<std::string, Page> before;
        status("restoring words (dictionary + Yandex Speller)\xE2\x80\xA6");
        UpdateWindow(hwnd);
        // in the cleaned view every page has one copy: the received copies come from the recording's project
        Pages orig; const Pages *ev = nullptr;
        if (projDir != R->teletext.project) {
            Project po; std::set<std::string> e2; std::string s2;
            if (po.open(R->teletext.project)) try { orig = po.load_state(e2, s2); ev = &orig; } catch (...) {}
        }
        auto fx = P.restore_words(copy, &before, true, which == 0 ? &one : nullptr, ev);
        if (fx.empty()) { ask(hwnd, "Restore words", "No damaged words with a confident replacement were found.\n\n" + P.words_note, MB_ICONINFORMATION); status(""); return; }
        int n = 0; for (auto &f : fx) n += f.n;
        std::string txt = fmt("%d words on %zu pages can be restored:\n\n", n, before.size());
        for (size_t i = 0; i < fx.size() && i < 30; i++) txt += fx[i].page + ":  " + fx[i].from + "  \xE2\x86\x92  " + fx[i].to + (fx[i].n > 1 ? fmt("  (%d)", fx[i].n) : "") + "\n";
        if (fx.size() > 30) txt += fmt("\xE2\x80\xA6 and %zu more\n", fx.size() - 30);
        if (!P.words_note.empty()) txt += "\n" + P.words_note + "\n";
        txt += "\nApply? Every page can be undone (Ctrl+Z).";
        if (ask(hwnd, "Restore words", txt, MB_YESNO | MB_ICONQUESTION) != IDYES) { status(""); return; }
        for (auto &b : before) { undo.push_back({b.first, b.second}); pages[b.first] = copy[b.first]; edited.insert(b.first); }
        while (undo.size() > 300) undo.erase(undo.begin());
        full_cache.clear(); dirty = true; fill_list(); draw();
        status(fmt("%d words restored on %zu pages \xE2\x80\x94 there are unsaved changes", n, before.size()));
    }
    void export_srt()
    {
        if (cur.empty()) return;
        std::vector<std::pair<std::string, int>> c6;
        try { if (exists(P.stream)) c6 = subtitle_pages(P.stream); } catch (...) {}
        std::string hintt;
        if (c6.empty()) hintt = "The stream has no pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6).";
        else { hintt = "Pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6): "; for (size_t i = 0; i < c6.size() && i < 6; i++) hintt += (i ? ", " : "") + c6[i].first + fmt(" (%d)", c6[i].second); }
        std::string pg = c6.empty() ? "888" : c6[0].first;
        if (!input(hwnd, "Subtitles .srt", hintt + "\n\nSubtitle page number:", pg)) return;
        pg = strip(pg); for (auto &ch : pg) ch = (char)toupper((uint8_t)ch);
        bool okp = pg.size() == 3 && pg[0] >= '1' && pg[0] <= '8';
        for (size_t i = 1; i < pg.size() && okp; i++) okp = isxdigit((uint8_t)pg[i]);
        if (!okp) { ask(hwnd, "Subtitles .srt", "Page number: three characters, the first is the magazine 1\xE2\x80\x93" "8 (e.g. 888).", MB_ICONERROR); return; }
        Project pc = P; auto res = std::make_shared<std::string>();
        run_task(hwnd, "Subtitles from page " + pg, [pc, pg, res](Progress &pr) {
            pr.step("subtitles from page " + pg);
            auto r = pc.export_srt(pg);
            *res = r.second ? fmt("subtitles: %d captions from page ", r.second) + pg + " \xE2\x86\x92 " + pc.rel(r.first)
                            : "page " + pg + " has no subtitles (no transmitted pages with the C6 flag) \xE2\x80\x94 an empty " + pc.rel(r.first) + " was written";
        }, [this, res](bool ok) { status(ok ? *res : "subtitle export error"); });
    }

    // ---------------------------------------------------------------- quality map
    void build_quality()
    {
        quality = load_json(P.quality_path);
        ListView_DeleteAllItems(q_list);
        if (!quality.is_obj()) { set_text(q_title, "No quality map: it is built when the pages come from a teletext VBI recording."); InvalidateRect(chart.hwnd, nullptr, TRUE); return; }
        set_text(q_title, "Tape quality per minute of " + P.name + " (broadcast time in brackets)");
        int i = 0;
        for (auto &d : quality["minutes"].a) {
            lv_set(q_list, i, 0, std::to_string(d["minute"].integer())); lv_set(q_list, i, 1, d.get_str("air"));
            lv_set(q_list, i, 2, fmt("%.1f", d["received"].num())); lv_set(q_list, i, 3, fmt("%.1f", d["unreadable"].num()));
            lv_set(q_list, i, 4, fmt("%.3f", d["parity"].num())); i++;
        }
        InvalidateRect(chart.hwnd, nullptr, TRUE);
    }

    // ---------------------------------------------------------------- keys handled before the controls
    bool pre_translate(MSG &m)
    {
        if (GetAncestor(m.hwnd, GA_ROOT) != hwnd) return false;
        if (m.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (m.wParam == 'S') { save(); return true; }
            if (m.wParam == 'Z' && m.hwnd != search && m.hwnd != pnum) { do_undo(); return true; }
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            if (m.hwnd == pnum) { open_page(strip(get_text(pnum))); return true; }
            for (int i = 0; i < 5; i++) if (m.hwnd == keys[i]) { set_flof(i); return true; }
        }
        if (m.message == WM_LBUTTONDBLCLK)
            for (int i = 0; i < 5; i++) if (m.hwnd == keys[i]) { set_flof(i); std::string v = strip(get_text(keys[i])); if (!v.empty()) open_page(v); return true; }
        // the screen takes every key itself (arrows, Enter, Backspace) - not the dialog navigation
        if (m.hwnd == screen.hwnd && (m.message == WM_KEYDOWN || m.message == WM_CHAR)) { TranslateMessage(&m); DispatchMessageW(&m); return true; }
        return false;
    }

    // ---------------------------------------------------------------- messages
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        LRESULT res;
        if (dark_menubar(hwnd, m, w, l, res)) return res;
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            build_menu(); create_controls(); show_tab();
            pretranslators()[hwnd] = [this](MSG &mm) { return pre_translate(mm); };
            if (!load_project(R->teletext.project)) status("the teletext project is missing: " + R->teletext.project);
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: { auto *mm = (MINMAXINFO *)l; mm->ptMinTrackSize = {S(hwnd, 1000), S(hwnd, 760)}; return 0; }
        case WM_TIMER: if (w == 1) { flash_on = !flash_on; draw(true); } return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l);
            if (abs(x - (left_w + S(hwnd, 4))) < S(hwnd, 6)) { dragging = true; SetCapture(hwnd); }
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
            case M_EXPORT_SRT: export_srt(); break;
            case M_PNG: save_png(); break;
            case ID_SQUASH: case M_SQUASH: do_squash(); break;
            case ID_WORDS: case M_WORDS: do_words(); break;
            case M_OPEN_FOLDER: if (has_project) open_path(P.outdir); break;
            case M_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            case ID_UNDO: do_undo(); break;
            case ID_OPENHTML: if (has_project) { std::string p = path_join(path_join(P.outdir, "html"), "index.html"); if (exists(p)) open_path(p); else status("html/index.html does not exist yet \xE2\x80\x94 run Export first"); } break;
            case ID_TAB_PAGE: tab_quality = false; show_tab(); break;
            case ID_TAB_QUAL: tab_quality = true; show_tab(); break;
            case ID_PREV: step(-1); break; case ID_NEXT: step(1); break;
            case ID_VPREV: step_ver(-1); break; case ID_VNEXT: step_ver(1); break;
            case ID_REVEAL: case ID_TV: case ID_CODES: draw(); break;
            case ID_FULL: ver = 0; draw(); break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (!cur.empty()) set_flofs();
            if (!can_leave()) return 0;
            DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, 1); pretranslators().erase(hwnd); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

LRESULT Screen::proc(UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        if (app->has_project && !app->cur.empty() && !app->page().versions.empty()) {
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

LRESULT Chart::proc(UINT m, WPARAM w, LPARAM l)
{
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
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom)); HGDIOBJ ob = SelectObject(mem, bm);
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

void teletext_window(HWND owner, const ResultPtr &r)
{
    auto *w = new TeletextWin; w->R = r;
    w->create(L"TRTeletext", "Teletext \xE2\x80\x94 " + file_name(*r), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, S(owner, 1320), S(owner, 920), owner);
    if (w->hwnd) ShowWindow(w->hwnd, SW_SHOW);
}
}
