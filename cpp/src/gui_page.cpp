// Диаграммы карты качества и тёмная строка меню.
#include "gui_app.h"
#include <uxtheme.h>

namespace ui {
static const int CL = 40, CB = 24, CT = 10, CR = 8;

void bar_chart(HDC dc, RECT r, const std::vector<double> &vals, const std::vector<std::string> &labels, HWND ref) {
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

int bar_hit(RECT r, size_t n, POINT p, HWND ref) {
    int L = S(ref, CL), R = S(ref, CR);
    if (!n || p.x < r.left + L || p.x >= r.right - R || p.y < r.top || p.y >= r.bottom) return -1;
    double bw = (double)(r.right - r.left - L - R) / n;
    int i = (int)((p.x - r.left - L) / bw);
    return i >= 0 && i < (int)n ? i : -1;
}

// ---------------------------------------------------------------- тёмная строка меню
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
typedef union { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static void paint_menu_line(HWND h) {
    MENUBARINFO mbi{sizeof mbi};
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return;
    RECT rc, rw; GetClientRect(h, &rc); MapWindowPoints(h, nullptr, (POINT *)&rc, 2);
    GetWindowRect(h, &rw); OffsetRect(&rc, -rw.left, -rw.top);
    RECT line = rc; line.bottom = line.top; line.top--;
    HDC dc = GetWindowDC(h); fill(dc, line, BG); ReleaseDC(h, dc);
}

bool dark_menubar(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT &res) {
    switch (msg) {
    case WM_UAHDRAWMENU: {
        auto *m = (UAHMENU *)lp;
        MENUBARINFO mbi{sizeof mbi}; GetMenuBarInfo(h, OBJID_MENU, 0, &mbi);
        RECT rw; GetWindowRect(h, &rw);
        RECT r = mbi.rcBar; OffsetRect(&r, -rw.left, -rw.top);
        fill(m->hdc, r, BG);
        res = TRUE; return true;
    }
    case WM_UAHDRAWMENUITEM: {
        auto *d = (UAHDRAWMENUITEM *)lp;
        wchar_t buf[256] = {0};
        MENUITEMINFOW mii{sizeof mii}; mii.fMask = MIIM_STRING; mii.dwTypeData = buf; mii.cch = 255;
        GetMenuItemInfoW(d->um.hmenu, d->umi.iPosition, TRUE, &mii);
        bool hot = d->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED);
        fill(d->dis.hDC, d->dis.rcItem, hot ? LINE : BG);
        SetBkMode(d->dis.hDC, TRANSPARENT);
        SetTextColor(d->dis.hDC, (d->dis.itemState & (ODS_INACTIVE | ODS_DISABLED)) ? DIM : FG);
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (d->dis.itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
        DrawTextW(d->dis.hDC, buf, -1, &d->dis.rcItem, flags);
        res = TRUE; return true;
    }
    case WM_NCPAINT: case WM_NCACTIVATE:
        res = DefWindowProcW(h, msg, wp, lp);
        paint_menu_line(h);
        return true;
    }
    return false;
}
}
