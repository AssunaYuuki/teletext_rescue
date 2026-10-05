#include "gui_app.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace ui {
HBRUSH brBG, brPANEL, brFIELD;
HWND g_main = nullptr;
static HINSTANCE g_inst;

COLORREF service_colour(const std::string &k0) {
    std::string k = lower(k0);
    if (k.rfind("encrypted datacast", 0) == 0) return RGB(0xff, 0x5c, 0x8a);
    if (k.find("silent radio") != std::string::npos) return RGB(0xff, 0xa3, 0x1a);
    if (k.rfind("nabts", 0) == 0) return RGB(0x7c, 0x6c, 0xff);
    if (k.find("wst") != std::string::npos || k.find("teletext") != std::string::npos) return RGB(0x3f, 0xa7, 0xff);
    if (k.rfind("cc", 0) == 0) return RGB(0x3c, 0xcf, 0x7a);
    if (k.rfind("amol", 0) == 0 || k.rfind("vitc", 0) == 0) return RGB(0x4a, 0xd0, 0xc8);
    if (k.rfind("vps", 0) == 0 || k.rfind("wss", 0) == 0) return RGB(0xc8, 0xc8, 0x4a);
    if (k.rfind("test", 0) == 0) return RGB(0x55, 0x5a, 0x63);
    if (k.rfind("data", 0) == 0) return RGB(0xb0, 0x7c, 0xff);
    if (k.rfind("empty", 0) == 0) return RGB(0x2a, 0x2c, 0x31);
    if (k.rfind("start of the picture", 0) == 0) return RGB(0x33, 0x36, 0x3c);
    return RGB(0x3a, 0x3d, 0x44);
}

std::wstring W(const std::string &s) { return widen(s); }
std::string N(const std::wstring &s) { return narrow(s); }
HINSTANCE inst() { return g_inst; }

// ---------------------------------------------------------------- тёмный режим (меню, заголовки)
using fnSetPreferredAppMode = int(WINAPI *)(int);
using fnAllowDarkModeForWindow = bool(WINAPI *)(HWND, bool);
using fnFlushMenuThemes = void(WINAPI *)();
static fnAllowDarkModeForWindow pAllowDark = nullptr;

void init(HINSTANCE i) {
    g_inst = i;
    brBG = CreateSolidBrush(BG); brPANEL = CreateSolidBrush(PANEL); brFIELD = CreateSolidBrush(FIELD);
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        auto setMode = (fnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        pAllowDark = (fnAllowDarkModeForWindow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        auto flush = (fnFlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (setMode) setMode(2);      // ForceDark: тёмные всплывающие меню и полосы прокрутки
        if (flush) flush();
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

int dpi(HWND h) {
    static auto f = (UINT(WINAPI *)(HWND))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = (f && h) ? f(h) : 0;
    if (!d) { HDC dc = GetDC(nullptr); d = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    return (int)d;
}
int S(HWND h, int v) { return MulDiv(v, dpi(h), 96); }

HFONT font(HWND h, int pt, bool bold, const wchar_t *face) {
    static std::map<std::tuple<int, int, bool, std::wstring>, HFONT> cache;
    int d = dpi(h);
    auto key = std::make_tuple(d, pt, bold, std::wstring(face));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HFONT f = CreateFontW(-MulDiv(pt, d, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    return cache[key] = f;
}

void dark_window(HWND h) {
    BOOL v = TRUE;
    if (DwmSetWindowAttribute(h, 20, &v, sizeof v) != S_OK) DwmSetWindowAttribute(h, 19, &v, sizeof v);
    COLORREF c = BG; DwmSetWindowAttribute(h, 35, &c, sizeof c);
    if (pAllowDark) pAllowDark(h, true);
}

// ---------------------------------------------------------------- элементы
struct BtnInfo { BtnStyle st; COLORREF colour; bool hot = false; };
static std::map<HWND, BtnInfo> g_btn;
static std::map<HWND, COLORREF> g_label_col;
static std::map<HWND, bool> g_check;

static LRESULT CALLBACK btn_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        auto &b = g_btn[h];
        if (!b.hot) { b.hot = true; InvalidateRect(h, nullptr, FALSE); TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); }
    } else if (m == WM_MOUSELEAVE) { g_btn[h].hot = false; InvalidateRect(h, nullptr, FALSE); }
    else if (m == WM_SETCURSOR && (g_btn[h].st == BTN_LINK)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
    else if (m == WM_LBUTTONUP && g_btn[h].st == BTN_CHECK) { g_check[h] = !g_check[h]; InvalidateRect(h, nullptr, FALSE); }
    else if (m == WM_NCDESTROY) { g_btn.erase(h); g_check.erase(h); }
    return DefSubclassProc(h, m, w, l);
}

HWND button(HWND parent, int id, const std::string &text, BtnStyle st, COLORREF colour) {
    HWND h = CreateWindowExW(0, L"BUTTON", W(text).c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    int pt = (st == BTN_ACCENT || st == BTN_BIG) ? 11 : 10;
    SendMessageW(h, WM_SETFONT, (WPARAM)font(parent, pt, st == BTN_ACCENT || st == BTN_BIG), TRUE);
    g_btn[h] = {st, colour};
    SetWindowSubclass(h, btn_sub, 1, 0);
    return h;
}
bool checked(HWND h) { return g_check[h]; }
void set_button_style(HWND h, BtnStyle st) { g_btn[h].st = st; InvalidateRect(h, nullptr, FALSE); }
void set_checked(HWND h, bool v) { g_check[h] = v; InvalidateRect(h, nullptr, FALSE); }

HWND label(HWND parent, const std::string &text, int id, DWORD extra) {
    HWND h = CreateWindowExW(0, L"STATIC", W(text).c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX | extra, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font(parent), TRUE);
    return h;
}
void set_label_colour(HWND h, COLORREF c) { g_label_col[h] = c; InvalidateRect(h, nullptr, TRUE); }

HWND edit(HWND parent, int id, const std::string &text, DWORD extra) {
    HWND h = CreateWindowExW(0, L"EDIT", W(text).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font(parent), TRUE);
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    return h;
}

static LRESULT CALLBACK lv_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_NOTIFY) {
        auto *nh = (NMHDR *)l;
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(h)) {
            auto *cd = (NMCUSTOMDRAW *)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                RECT r = cd->rc; fill(cd->hdc, r, FIELD);
                RECT ln{r.right - 1, r.top + 4, r.right, r.bottom - 4}; fill(cd->hdc, ln, LINE);
                wchar_t buf[128] = {0}; HDITEMW hi{}; hi.mask = HDI_TEXT | HDI_FORMAT; hi.pszText = buf; hi.cchTextMax = 127;
                Header_GetItem(nh->hwndFrom, (int)cd->dwItemSpec, &hi);
                SetBkMode(cd->hdc, TRANSPARENT); SetTextColor(cd->hdc, MUTED);
                HGDIOBJ of = SelectObject(cd->hdc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
                RECT tr = r; tr.left += 6; tr.right -= 6;
                DrawTextW(cd->hdc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | ((hi.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT));
                SelectObject(cd->hdc, of);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}

HWND listview(HWND parent, int id, const std::vector<std::pair<std::string, int>> &cols) {
    HWND h = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    HWND hdr = ListView_GetHeader(h);
    SetWindowTheme(hdr, L"DarkMode_ItemsView", nullptr);
    ListView_SetBkColor(h, PANEL); ListView_SetTextBkColor(h, PANEL); ListView_SetTextColor(h, FG);
    SendMessageW(h, WM_SETFONT, (WPARAM)font(parent), TRUE);
    SetWindowSubclass(h, lv_sub, 2, 0);
    for (size_t i = 0; i < cols.size(); i++) {
        std::wstring t = W(cols[i].first);
        LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.pszText = t.data(); c.cx = S(parent, abs(cols[i].second));
        c.fmt = cols[i].second < 0 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(h, (int)i, &c);
    }
    return h;
}
void lv_set(HWND lv, int row, int col, const std::string &s) {
    std::wstring t = W(s);
    if (col == 0) {
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.pszText = t.data(); it.lParam = row;
        ListView_InsertItem(lv, &it);
    } else ListView_SetItemText(lv, row, col, t.data());
}
LRESULT lv_custom_draw(LPARAM lp, const std::function<COLORREF(int)> &tc) {
    auto *cd = (NMLVCUSTOMDRAW *)lp;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        cd->clrText = tc ? tc((int)cd->nmcd.dwItemSpec) : FG;
        cd->clrTextBk = PANEL;
        return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}

void set_text(HWND h, const std::string &s) { SetWindowTextW(h, W(s).c_str()); }
std::string get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(n + 1, 0); GetWindowTextW(h, w.data(), n + 1); w.resize(n);
    return N(w);
}

void fill(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void text_out(HDC dc, int x, int y, const std::string &s, COLORREF c, HFONT f, UINT align) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); SetTextAlign(dc, align);
    std::wstring w = W(s);
    TextOutW(dc, x, y, w.c_str(), (int)w.size());
    SetTextAlign(dc, 0);
    SelectObject(dc, o);
}

bool draw_item(LPARAM lp) {
    auto *d = (DRAWITEMSTRUCT *)lp;
    if (d->CtlType != ODT_BUTTON) return false;
    auto it = g_btn.find(d->hwndItem);
    if (it == g_btn.end()) return false;
    BtnInfo &b = it->second;
    HDC dc = d->hDC; RECT r = d->rcItem;
    bool down = d->itemState & ODS_SELECTED, dis = d->itemState & ODS_DISABLED, focus = d->itemState & ODS_FOCUS;
    wchar_t buf[512]; GetWindowTextW(d->hwndItem, buf, 512);
    HFONT f = (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    HWND parent = GetParent(d->hwndItem);
    COLORREF pbg = BG;
    if (b.st == BTN_CHECK) {
        fill(dc, r, pbg);
        int sz = S(parent, 14), y = (r.top + r.bottom - sz) / 2;
        RECT box{r.left + 1, y, r.left + 1 + sz, y + sz};
        bool on = g_check[d->hwndItem];
        fill(dc, box, on ? ACCENT : FIELD);
        if (on) {
            HPEN p = CreatePen(PS_SOLID, std::max(2, S(parent, 2)), BG); HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, box.left + sz / 5, box.top + sz / 2, nullptr); LineTo(dc, box.left + sz * 2 / 5, box.bottom - sz / 4); LineTo(dc, box.right - sz / 6, box.top + sz / 4);
            SelectObject(dc, op); DeleteObject(p);
        }
        RECT tr{box.right + S(parent, 6), r.top, r.right, r.bottom};
        SetTextColor(dc, dis ? DIM : FG);
        DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else if (b.st == BTN_LINK) {
        fill(dc, r, b.hot ? LINE : PANEL);
        SetTextColor(dc, ACCENT2);
        RECT tr = r; tr.left += S(parent, 4);
        DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    } else {
        COLORREF bg = FIELD, fg = dis ? DIM : FG;
        if (b.st == BTN_ACCENT) { bg = down ? RGB(0xd9, 0x84, 0x00) : b.hot ? ACCENT2 : ACCENT; fg = BG; }
        else if (b.st == BTN_COLOUR) { bg = b.colour; fg = (GetRValue(bg) * 3 + GetGValue(bg) * 6 + GetBValue(bg)) > 1200 ? RGB(0, 0, 0) : RGB(255, 255, 255); if (b.hot) fg = ACCENT; }
        else { bg = down ? SEL : b.hot ? LINE : FIELD; }
        fill(dc, r, pbg);
        HPEN pen = CreatePen(PS_SOLID, 1, (b.hot || focus) && b.st != BTN_ACCENT ? ACCENT : (b.st == BTN_COLOUR ? bg : LINE));
        HBRUSH br = CreateSolidBrush(bg);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        int rr = S(parent, 6);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, rr, rr);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen); DeleteObject(br);
        SetTextColor(dc, fg);
        DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    }
    SelectObject(dc, of);
    return true;
}

LRESULT ctl_colour(UINT msg, WPARAM wp, LPARAM lp) {
    HDC dc = (HDC)wp; HWND h = (HWND)lp;
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        SetTextColor(dc, FG); SetBkColor(dc, FIELD); return (LRESULT)brFIELD;
    }
    auto it = g_label_col.find(h);
    SetTextColor(dc, it != g_label_col.end() ? it->second : FG);
    // подписи на «карточках» (панелях) — по свойству окна
    bool card = GetPropW(h, L"card") != nullptr;
    SetBkColor(dc, card ? PANEL : BG);
    SetBkMode(dc, OPAQUE);
    if (msg == WM_CTLCOLORSTATIC && (GetWindowLongW(h, GWL_STYLE) & ES_READONLY)) { SetBkColor(dc, PANEL); return (LRESULT)brPANEL; }
    return (LRESULT)(card ? brPANEL : brBG);
}

void blit(HDC dc, const Image &im, int x, int y, int w, int h, bool smooth) {
    if (im.w <= 0 || im.h <= 0) return;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = im.w; bi.bmiHeader.biHeight = -im.h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, smooth ? HALFTONE : COLORONCOLOR);
    if (smooth) SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, x, y, w, h, 0, 0, im.w, im.h, im.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// ---------------------------------------------------------------- диалоги
static std::string run_file_dialog(HWND owner, const std::string &title, const std::vector<std::pair<std::string, std::string>> &filters,
                                   bool save, bool folder, const std::string &def_name, const std::string &ext, const std::string &start) {
    IFileDialog *dlg = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  save ? IID_IFileSaveDialog : IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr)) return "";
    std::wstring wt = W(title); dlg->SetTitle(wt.c_str());
    DWORD opts; dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM;
    if (folder) opts |= FOS_PICKFOLDERS;
    dlg->SetOptions(opts);
    std::vector<std::wstring> store; std::vector<COMDLG_FILTERSPEC> spec;
    for (auto &f : filters) { store.push_back(W(f.first)); store.push_back(W(f.second)); }
    for (size_t i = 0; i < filters.size(); i++) spec.push_back({store[2 * i].c_str(), store[2 * i + 1].c_str()});
    if (!spec.empty()) dlg->SetFileTypes((UINT)spec.size(), spec.data());
    if (!def_name.empty()) dlg->SetFileName(W(def_name).c_str());
    if (!ext.empty()) dlg->SetDefaultExtension(W(ext).c_str());
    if (!start.empty()) {
        IShellItem *si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(W(start).c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); }
    }
    std::string res;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { res = N(p); CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return res;
}
std::string open_file(HWND owner, const std::string &title, const std::vector<std::pair<std::string, std::string>> &filters) {
    return run_file_dialog(owner, title, filters, false, false, "", "", "");
}
std::string save_file(HWND owner, const std::string &title, const std::string &def_name, const std::string &ext) {
    return run_file_dialog(owner, title, {{ext + " files", "*." + ext}}, true, false, def_name, ext, "");
}
std::string pick_folder(HWND owner, const std::string &title, const std::string &start) {
    return run_file_dialog(owner, title, {}, false, true, "", "", start);
}
int ask(HWND owner, const std::string &title, const std::string &text, UINT flags) {
    return MessageBoxW(owner, W(text).c_str(), W(title).c_str(), flags);
}
void open_path(const std::string &p) { ShellExecuteW(nullptr, L"open", W(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// простое окно ввода строки
struct InputDlg : Window {
    std::string prompt, value; bool ok = false, done = false;
    HWND ed = nullptr, lab = nullptr, bok = nullptr, bcancel = nullptr;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            lab = label(hwnd, prompt); ed = edit(hwnd, 1000, value);
            bok = button(hwnd, IDOK, "OK", BTN_ACCENT); bcancel = button(hwnd, IDCANCEL, "Cancel");
            RECT r; GetClientRect(hwnd, &r);
            int p = S(hwnd, 14), bw = S(hwnd, 96), bh = S(hwnd, 32);
            int lines = 1; for (char c : prompt) lines += c == '\n';
            int lh = S(hwnd, 20) * lines;
            MoveWindow(lab, p, p, r.right - 2 * p, lh, TRUE);
            MoveWindow(ed, p, p + lh + S(hwnd, 6), r.right - 2 * p, S(hwnd, 26), TRUE);
            MoveWindow(bok, r.right - p - 2 * bw - S(hwnd, 8), r.bottom - p - bh, bw, bh, TRUE);
            MoveWindow(bcancel, r.right - p - bw, r.bottom - p - bh, bw, bh, TRUE);
            SetFocus(ed); SendMessageW(ed, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(w) != BN_CLICKED) return 0;          // уведомления поля ввода
            if (LOWORD(w) == IDOK) { value = get_text(ed); ok = true; DestroyWindow(hwnd); }
            else if (LOWORD(w) == IDCANCEL) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: done = true; return 0;
        }
        return Window::proc(m, w, l);
    }
};
// выбор одного из вариантов: по кнопке на вариант, «больше не спрашивать», «Отмена»
struct ChooseDlg : Window {
    std::string prompt, check; std::vector<std::string> opts; int def = 0, res = -1; bool done = false, remember = false;
    HWND lab = nullptr, chk = nullptr, bcancel = nullptr; std::vector<HWND> bs;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            RECT r; GetClientRect(hwnd, &r);
            int p = S(hwnd, 14), bh = S(hwnd, 40), y = p;
            int lines = 1; for (char c : prompt) lines += c == '\n';
            lab = label(hwnd, prompt); MoveWindow(lab, p, y, r.right - 2 * p, S(hwnd, 20) * lines, TRUE); y += S(hwnd, 20) * lines + S(hwnd, 10);
            for (size_t i = 0; i < opts.size(); i++) {
                HWND b = button(hwnd, 2000 + (int)i, opts[i], (int)i == def ? BTN_ACCENT : BTN_NORMAL);
                MoveWindow(b, p, y, r.right - 2 * p, bh, TRUE); y += bh + S(hwnd, 8); bs.push_back(b);
            }
            if (!check.empty()) { chk = button(hwnd, 1999, check, BTN_CHECK); MoveWindow(chk, p, y + S(hwnd, 4), r.right - 2 * p - S(hwnd, 110), S(hwnd, 26), TRUE); }
            bcancel = button(hwnd, IDCANCEL, "Cancel"); MoveWindow(bcancel, r.right - p - S(hwnd, 96), y, S(hwnd, 96), S(hwnd, 32), TRUE);
            if (!bs.empty()) SetFocus(bs[std::min<size_t>(def, bs.size() - 1)]);
            return 0;
        }
        case WM_COMMAND: {
            if (HIWORD(w) != BN_CLICKED) return 0;
            int id = LOWORD(w);
            if (id >= 2000 && id < 2000 + (int)opts.size()) { res = id - 2000; remember = chk && checked(chk); DestroyWindow(hwnd); }
            else if (id == IDCANCEL) DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: done = true; return 0;
        }
        return Window::proc(m, w, l);
    }
};
int choose(HWND owner, const std::string &title, const std::string &prompt, const std::vector<std::string> &opts, int def,
           const std::string &check, bool *remember) {
    Window::register_class(L"TRChoose", brBG);
    ChooseDlg d; d.prompt = prompt; d.opts = opts; d.def = def; d.check = check;
    int lines = 1; for (char c : prompt) lines += c == '\n';
    d.create(L"TRChoose", title, WS_POPUP | WS_CAPTION | WS_SYSMENU, S(owner, 560),
             S(owner, 110 + 20 * lines + 48 * (int)opts.size()), owner, WS_EX_DLGMODALFRAME);
    RECT orc; GetWindowRect(owner, &orc); RECT dr; GetWindowRect(d.hwnd, &dr);
    SetWindowPos(d.hwnd, nullptr, (orc.left + orc.right - (dr.right - dr.left)) / 2, (orc.top + orc.bottom - (dr.bottom - dr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(d.hwnd, SW_SHOW);
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageW(d.hwnd, WM_COMMAND, IDCANCEL, 0); continue; }
        if (!IsDialogMessageW(d.hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    if (remember) *remember = d.remember;
    return d.res;
}

bool input(HWND owner, const std::string &title, const std::string &prompt, std::string &value) {
    Window::register_class(L"TRInput", brBG);
    InputDlg d; d.prompt = prompt; d.value = value;
    int lines = 1; for (char c : prompt) lines += c == '\n';
    d.create(L"TRInput", title, WS_POPUP | WS_CAPTION | WS_SYSMENU, S(owner, 520), S(owner, 150 + 20 * lines), owner, WS_EX_DLGMODALFRAME);
    RECT orc; GetWindowRect(owner, &orc); RECT dr; GetWindowRect(d.hwnd, &dr);
    SetWindowPos(d.hwnd, nullptr, (orc.left + orc.right - (dr.right - dr.left)) / 2, (orc.top + orc.bottom - (dr.bottom - dr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(d.hwnd, SW_SHOW);
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && IsChild(d.hwnd, msg.hwnd)) { SendMessageW(d.hwnd, WM_COMMAND, IDOK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageW(d.hwnd, WM_COMMAND, IDCANCEL, 0); continue; }
        if (!IsDialogMessageW(d.hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    if (d.ok) value = d.value;
    return d.ok;
}

// ---------------------------------------------------------------- настройки
std::string state_path() {
    wchar_t buf[MAX_PATH];
    std::string d;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) d = N(buf);
    d = path_join(d, "Teletext Rescue");
    make_dirs(d);
    return path_join(d, "gui_state.json");
}
Json load_state() { Json j = load_json(state_path(), Json::object()); return j.is_obj() ? j : Json::object(); }
void save_state(const Json &j) { try { save_json(state_path(), j, 1); } catch (...) {} }
static std::string path_key(const std::string &p) { return lower(replace_all(abspath(p), "/", "\\")); }
std::vector<Json> recent_list() {
    std::vector<Json> out; std::set<std::string> seen;
    Json st = load_state();                     // не временный объект: цикл идёт по его полю
    for (auto &r : st["recent"].a)
        if (exists(r.get_str("path")) && seen.insert(path_key(r.get_str("path"))).second) out.push_back(r);
    return out;
}
void recent_add(const std::string &path, const std::string &kind) {
    Json d = load_state();
    Json rec = Json::array();
    Json e = Json::object(); e["path"] = replace_all(abspath(path), "/", "\\"); e["kind"] = kind; e["time"] = now_str();
    rec.push(e);
    for (auto &r : d["recent"].a) if (path_key(r.get_str("path")) != path_key(path) && rec.size() < 12) rec.push(r);
    d["recent"] = rec;
    save_state(d);
}

// ---------------------------------------------------------------- окна
LRESULT Window::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DRAWITEM: if (draw_item(l)) return TRUE; break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: return ctl_colour(m, w, l);
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd, &r); fill((HDC)w, r, BG); return 1; }
    }
    return DefWindowProcW(hwnd, m, w, l);
}
LRESULT CALLBACK Window::thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window *self;
    if (m == WM_NCCREATE) { self = (Window *)((CREATESTRUCTW *)l)->lpCreateParams; self->hwnd = h; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self); }
    else self = (Window *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCDESTROY) {
        // окно может удалить себя в своём обработчике — после него объект не трогаем
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return self->proc(m, w, l);
    }
    return self->proc(m, w, l);
}
void Window::register_class(const std::wstring &cls, HBRUSH bg) {
    static std::set<std::wstring> done;
    if (done.count(cls)) return;
    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = thunk; wc.hInstance = g_inst; wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = bg ? bg : brBG;
    wc.hIcon = LoadIconW(g_inst, L"IDI_APP"); wc.hIconSm = wc.hIcon;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassExW(&wc);
    done.insert(cls);
}
HWND Window::create(const std::wstring &cls, const std::string &title, DWORD style, int w, int h, HWND parent, DWORD ex) {
    register_class(cls);
    return CreateWindowExW(ex, cls.c_str(), W(title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, w, h, parent, nullptr, g_inst, this);
}

void post_ui(std::function<void()> fn) {
    auto *p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(g_main, WM_APP_CALL, 0, (LPARAM)p)) delete p;
}

// ---------------------------------------------------------------- окно хода работы
struct Runner : Window {
    std::string title;
    std::function<void(Progress &)> work; std::function<void(bool)> on_done;
    Progress pr; std::thread th; bool finished = false, ok = false;
    HWND step_l, bar, eta, log, btn;
    std::chrono::steady_clock::time_point t_step = std::chrono::steady_clock::now();
    std::mutex mu; std::deque<std::function<void()>> queue;
    void push(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(mu); queue.push_back(std::move(fn)); }
        PostMessageW(hwnd, WM_APP + 2, 0, 0);
    }
    void add_log(const std::string &t) {
        std::string s = replace_all(t, "\n", "\r\n") + "\r\n";
        int n = GetWindowTextLengthW(log);
        if (n > 200000) { SendMessageW(log, EM_SETSEL, 0, 50000); SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(log); }
        SendMessageW(log, EM_SETSEL, n, n); SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)W(s).c_str());
    }
    void layout() {
        RECT r; GetClientRect(hwnd, &r); int p = S(hwnd, 12), y = p;
        MoveWindow(step_l, p, y, r.right - 2 * p, S(hwnd, 22), TRUE); y += S(hwnd, 26);
        MoveWindow(bar, p, y, r.right - 2 * p, S(hwnd, 12), TRUE); y += S(hwnd, 18);
        MoveWindow(eta, p, y, r.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 24);
        int bh = S(hwnd, 32);
        MoveWindow(log, p, y, r.right - 2 * p, r.bottom - y - bh - 2 * p, TRUE);
        MoveWindow(btn, r.right - p - S(hwnd, 110), r.bottom - p - bh, S(hwnd, 110), bh, TRUE);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            step_l = label(hwnd, "starting\xE2\x80\xA6"); SendMessageW(step_l, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
            bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 10, 10, hwnd, nullptr, inst(), nullptr);
            SetWindowTheme(bar, L"", L"");
            SendMessageW(bar, PBM_SETBARCOLOR, 0, ACCENT); SendMessageW(bar, PBM_SETBKCOLOR, 0, PANEL);
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
            eta = label(hwnd, ""); set_label_colour(eta, MUTED);
            log = edit(hwnd, 0, "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL);
            SendMessageW(log, WM_SETFONT, (WPARAM)font(hwnd, 9, false, L"Consolas"), TRUE);
            btn = button(hwnd, IDCANCEL, "Cancel");
            layout();
            pr.on_step = [this](const std::string &t) { push([this, t] { t_step = std::chrono::steady_clock::now(); set_text(step_l, t); set_text(eta, "");
                SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) | PBS_MARQUEE); SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30); add_log(t); }); };
            pr.on_log = [this](const std::string &t) { push([this, t] { add_log(t); }); };
            pr.on_progress = [this](long long a, long long b, const std::string &s) {
                static thread_local DWORD last = 0; DWORD now = GetTickCount();
                if (now - last < 150 && a < b) return;
                last = now;
                push([this, a, b, s] {
                    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
                    SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) & ~PBS_MARQUEE);
                    SendMessageW(bar, PBM_SETRANGE32, 0, (LPARAM)std::max<long long>(1, std::min<long long>(b, INT_MAX)));
                    SendMessageW(bar, PBM_SETPOS, (WPARAM)std::min<long long>(a, INT_MAX), 0);
                    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_step).count();
                    double left = a ? el * (b - a) / a : 0;
                    set_text(eta, fmt("%.0f%%   \xC2\xB7   %lld of %lld   \xC2\xB7   %.0f s elapsed", 100.0 * a / std::max(1LL, b), a, b, el) +
                                      (a && el > 3 ? fmt(", about %.0f s left", left) : "") + "   " + s);
                });
            };
            th = std::thread([this] {
                bool good = false; std::string err;
                try { work(pr); good = true; }
                catch (Cancelled &) { err = "cancelled"; }
                catch (std::exception &e) { err = e.what(); }
                push([this, good, err] { finish(good, err); });
            });
            return 0;
        }
        case WM_APP + 2: {
            std::deque<std::function<void()>> q;
            { std::lock_guard<std::mutex> lk(mu); q.swap(queue); }
            for (auto &f : q) f();
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_COMMAND: if (LOWORD(w) == IDCANCEL) cancel(); return 0;
        case WM_CLOSE: cancel(); return 0;
        case WM_DESTROY: if (th.joinable()) th.join(); return 0;
        case WM_NCDESTROY: { HWND owner = GetWindow(hwnd, GW_OWNER); LRESULT r = Window::proc(m, w, l); bool o = ok; auto cb = on_done; bool f = finished;
            delete this; if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); } if (f && cb) cb(o); return r; }
        }
        return Window::proc(m, w, l);
    }
    void finish(bool good, const std::string &err) {
        finished = true; ok = good;
        if (th.joinable()) th.join();
        if (good) { DestroyWindow(hwnd); return; }
        if (err == "cancelled") { DestroyWindow(hwnd); return; }
        set_text(step_l, "error \xE2\x80\x94 details below"); set_label_colour(step_l, RGB(0xff, 0x70, 0x70));
        add_log(err);
        SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
        set_text(btn, "Close");
        on_done = nullptr;
    }
    void cancel() {
        if (finished) { DestroyWindow(hwnd); return; }
        if (ask(hwnd, "Cancel", "Stop the work?", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        pr.cancel = true;
        set_text(step_l, "stopping\xE2\x80\xA6");
    }
};

void run_task(HWND owner, const std::string &title, std::function<void(Progress &)> work, std::function<void(bool)> on_done) {
    auto *r = new Runner; r->title = title; r->work = std::move(work); r->on_done = std::move(on_done);
    r->create(L"TRRunner", title, WS_OVERLAPPEDWINDOW, S(owner, 720), S(owner, 420), owner);
    RECT orc; GetWindowRect(owner, &orc); RECT dr; GetWindowRect(r->hwnd, &dr);
    SetWindowPos(r->hwnd, nullptr, (orc.left + orc.right - (dr.right - dr.left)) / 2, (orc.top + orc.bottom - (dr.bottom - dr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(r->hwnd, SW_SHOW);
    EnableWindow(owner, FALSE);
}
}
