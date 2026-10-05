// Окно AMOL: что передавалось в строке AMOL — часы эфира и код источника — как на табло, с ходом по записи,
// 48 бит пакета по полям и список отметок времени.
#include "gui_app.h"
#include <windowsx.h>
#include <algorithm>

namespace ui {
namespace {
struct Pk { int u; uint8_t b[48]; };
int val(const uint8_t *b, int a, int n) { int v = 0; for (int i = a; i < a + n; i++) v = (v << 1) | b[i]; return v; }
// поля пакета: начало, длина, подпись, цвет
struct Fld { int a, n; const char *name; COLORREF c; };
const Fld FL[] = {
    {0, 3, "phase", RGB(0x55, 0x58, 0x60)}, {3, 4, "start", RGB(0x55, 0x58, 0x60)}, {7, 5, "frame", RGB(0x3a, 0x7b, 0xd5)},
    {12, 6, "source ID", RGB(0xff, 0xa3, 0x1a)}, {18, 4, "month", RGB(0x2e, 0xa0, 0x6a)}, {22, 5, "day", RGB(0x2e, 0xa0, 0x6a)},
    {27, 4, "hour", RGB(0xc8, 0x4b, 0x9e)}, {31, 6, "minute", RGB(0xc8, 0x4b, 0x9e)}, {37, 6, "second", RGB(0xc8, 0x4b, 0x9e)},
    {43, 1, "PM", RGB(0xc8, 0x4b, 0x9e)}, {44, 3, "spare", RGB(0x55, 0x58, 0x60)}, {47, 1, "P", RGB(0x8a, 0x8d, 0x93)},
};
bool has_time(const uint8_t *b) { int mo = val(b, 18, 4), dy = val(b, 22, 5), hh = val(b, 27, 4); return mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31 && hh <= 12; }
std::string stamp(const uint8_t *b) {
    return fmt("%d/%d  %d:%02d:%02d %s", val(b, 18, 4), val(b, 22, 5), val(b, 27, 4), val(b, 31, 6), val(b, 37, 6), b[43] ? "PM" : "AM");
}
bool parity_odd(const uint8_t *b) { int ones = 0; for (int i = 12; i < 48; i++) ones += b[i]; return ones % 2 == 1; }
}

struct AmolWin : Window {
    Json data; std::vector<Pk> pk; std::vector<size_t> stamps;   // индексы пакетов, где меняется отметка времени
    double rate = 59.94; int units = 0; std::string unit = "field";
    double pos = 0;                   // текущее место записи (поле/кадр)
    bool playing = false; ULONGLONG last = 0;
    HWND title, sub, playb, repb, summary, lst;
    RECT clock{}, bits{}, tl{};
    size_t cur_packet() const {
        if (pk.empty()) return 0;
        auto it = std::upper_bound(pk.begin(), pk.end(), (int)pos, [](int u, const Pk &p) { return u < p.u; });
        return it == pk.begin() ? 0 : (size_t)(it - pk.begin() - 1);
    }
    const Pk *cur_stamp() const {                 // последняя отметка времени не позже текущего места
        if (pk.empty()) return nullptr;
        for (size_t i = cur_packet() + 1; i-- > 0;) if (has_time(pk[i].b)) return &pk[i];
        for (auto &p : pk) if (has_time(p.b)) return &p;
        return nullptr;
    }
    void load() {
        rate = data["rate"].num(59.94); units = data["units"].integer(); unit = data.get_str("unit");
        for (auto &e : data["packets"].a) {
            Pk p; p.u = e[0].integer(); std::string hx = e[1].str();
            for (int i = 0; i < 12 && i < (int)hx.size(); i++) { int v = isdigit((u8)hx[i]) ? hx[i] - '0' : toupper((u8)hx[i]) - 'A' + 10; for (int k = 0; k < 4; k++) p.b[i * 4 + k] = (v >> (3 - k)) & 1; }
            pk.push_back(p);
        }
        std::string last;
        for (size_t i = 0; i < pk.size(); i++) if (has_time(pk[i].b)) { std::string s = stamp(pk[i].b) + fmt("|%d", val(pk[i].b, 12, 6)); if (s != last) stamps.push_back(i); last = s; }
        if (!pk.empty()) pos = pk[0].u;
    }
    std::string rec_time(double u) const { double s = u / rate; return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)); }
    void fill_list() {
        ListView_DeleteAllItems(lst);
        int r = 0;
        for (size_t i : stamps) {
            const Pk &p = pk[i];
            lv_set(lst, r, 0, rec_time(p.u)); lv_set(lst, r, 1, stamp(p.b)); lv_set(lst, r, 2, fmt("%d", val(p.b, 12, 6)));
            lv_set(lst, r, 3, fmt("%d", val(p.b, 7, 5))); lv_set(lst, r, 4, parity_odd(p.b) ? "odd" : "even"); r++;
            if (r >= 20000) break;
        }
        // сводка
        std::map<int, int> sids; int odd = 0, nt = 0;
        for (auto &p : pk) { if (has_time(p.b)) { sids[val(p.b, 12, 6)]++; nt++; } odd += parity_odd(p.b); }
        std::string s = fmt("%zu packets in %s %ss", pk.size(), rec_time(units).c_str(), unit.c_str());
        if (!sids.empty()) {
            int best = sids.begin()->first; for (auto &kv : sids) if (kv.second > sids[best]) best = kv.first;
            s += fmt("  \xC2\xB7  source ID %d", best);
        }
        if (!stamps.empty()) s += "  \xC2\xB7  on air " + stamp(pk[stamps.front()].b) + "  \xE2\x80\x93  " + stamp(pk[stamps.back()].b);
        s += fmt("  \xC2\xB7  odd parity in %.0f%%", 100.0 * odd / std::max<size_t>(1, pk.size()));
        set_text(summary, s);
    }
    void seek(double u, bool from_list = false) {
        pos = std::max(0.0, std::min((double)std::max(units, 1), u));
        if (!from_list && !stamps.empty()) {
            size_t c = cur_packet(); int row = 0;
            for (size_t k = 0; k < stamps.size(); k++) if (stamps[k] <= c) row = (int)k;
            ListView_SetItemState(lst, -1, 0, LVIS_SELECTED); ListView_SetItemState(lst, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(lst, row, FALSE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void paint(HDC mem, RECT rc) {
        fill(mem, rc, BG);
        const Pk *ts = cur_stamp();
        size_t ci = cur_packet();
        const Pk *p = pk.empty() ? nullptr : &pk[ci];
        // табло
        fill(mem, clock, RGB(0x11, 0x11, 0x11));
        RECT in{clock.left + S(hwnd, 10), clock.top + S(hwnd, 10), clock.right - S(hwnd, 10), clock.bottom - S(hwnd, 10)};
        fill(mem, in, RGB(0, 0, 0));
        HFONT big = font(hwnd, 34, true, L"Consolas"), mid = font(hwnd, 14, true, L"Consolas"), small = font(hwnd, 9);
        int cy = in.top + (in.bottom - in.top) / 2;
        text_out(mem, in.left + S(hwnd, 18), cy - S(hwnd, 30), ts ? stamp(ts->b) : "--/--  --:--:--", ACCENT, big);
        text_out(mem, in.right - S(hwnd, 18), cy - S(hwnd, 30), ts ? fmt("SID %d", val(ts->b, 12, 6)) : "SID --", ACCENT2, mid, TA_RIGHT);
        text_out(mem, in.right - S(hwnd, 18), cy + S(hwnd, 2), "recording " + rec_time(pos), MUTED, small, TA_RIGHT);
        text_out(mem, in.left + S(hwnd, 20), in.bottom - S(hwnd, 22), "time of the broadcast and source ID sent in the AMOL line", DIM, small);
        // биты пакета
        if (p) {
            int w = (bits.right - bits.left) / 48, x0 = bits.left, y0 = bits.top, h = S(hwnd, 30);
            text_out(mem, x0, y0 - S(hwnd, 20), fmt("packet at %s %d", unit.c_str(), p->u), MUTED, small);
            for (auto &f : FL) {
                for (int i = f.a; i < f.a + f.n; i++) {
                    RECT c{x0 + i * w + 1, y0, x0 + (i + 1) * w - 1, y0 + h};
                    fill(mem, c, p->b[i] ? f.c : RGB(0x24, 0x26, 0x2b));
                    text_out(mem, (c.left + c.right) / 2, y0 + S(hwnd, 6), p->b[i] ? "1" : "0", p->b[i] ? RGB(0, 0, 0) : DIM, small, TA_CENTER);
                }
                int xa = x0 + f.a * w, xb = x0 + (f.a + f.n) * w;
                RECT u{xa + 1, y0 + h + S(hwnd, 3), xb - 1, y0 + h + S(hwnd, 5)}; fill(mem, u, f.c);
                std::string v = f.n > 1 ? fmt("%d", val(p->b, f.a, f.n)) : (p->b[f.a] ? "1" : "0");
                if (!strcmp(f.name, "PM")) v = p->b[43] ? "PM" : "AM";
                if (!strcmp(f.name, "P")) v = parity_odd(p->b) ? "odd" : "even";
                text_out(mem, (xa + xb) / 2, y0 + h + S(hwnd, 8), f.name, MUTED, small, TA_CENTER);
                text_out(mem, (xa + xb) / 2, y0 + h + S(hwnd, 24), v, FG, small, TA_CENTER);
            }
        }
        // ход по записи: где есть пакеты, и текущее место
        fill(mem, tl, RGB(0x24, 0x26, 0x2b));
        if (units > 0) {
            int W = tl.right - tl.left;
            std::vector<int> dens(std::max(1, W), 0);
            for (auto &q : pk) dens[std::min(W - 1, (int)((double)q.u / units * W))]++;
            int mx = 1; for (int d : dens) mx = std::max(mx, d);
            for (int x = 0; x < W; x++) if (dens[x]) { int hh = std::max(2, (int)((tl.bottom - tl.top - 4) * (double)dens[x] / mx)); RECT b{tl.left + x, tl.bottom - 2 - hh, tl.left + x + 1, tl.bottom - 2}; fill(mem, b, RGB(0x7a, 0x55, 0x1a)); }
            int cx = tl.left + (int)(pos / units * W);
            RECT cur{cx - 1, tl.top, cx + 2, tl.bottom}; fill(mem, cur, ACCENT);
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            title = label(hwnd, "AMOL"); SendMessageW(title, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            sub = label(hwnd, fmt("line %d%s  \xC2\xB7  ", data["line"].integer(), data.get_str("field").empty() ? "" : (" field " + data.get_str("field")).c_str()) + data.get_str("source"));
            set_label_colour(sub, MUTED);
            playb = button(hwnd, 401, "\xE2\x96\xB6 Play"); repb = button(hwnd, 402, "Text report");
            summary = label(hwnd, ""); set_label_colour(summary, ACCENT2);
            lst = listview(hwnd, 10, {{"Recording", 110}, {"On air", 190}, {"Source ID", 80}, {"Frame", 60}, {"Parity", 60}});
            load(); fill_list(); seek(pos);
            SetTimer(hwnd, 1, 40, nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = S(hwnd, 10), bh = S(hwnd, 30);
            MoveWindow(title, p, y + S(hwnd, 4), S(hwnd, 60), S(hwnd, 24), TRUE);
            MoveWindow(sub, p + S(hwnd, 64), y + S(hwnd, 8), rc.right - 2 * p - S(hwnd, 330), S(hwnd, 20), TRUE);
            MoveWindow(playb, rc.right - p - S(hwnd, 250), y, S(hwnd, 100), bh, TRUE);
            MoveWindow(repb, rc.right - p - S(hwnd, 140), y, S(hwnd, 140), bh, TRUE);
            y += bh + S(hwnd, 10);
            clock = {p, y, rc.right - p, y + S(hwnd, 120)}; y = clock.bottom + S(hwnd, 34);
            bits = {p, y, rc.right - p, y + S(hwnd, 30)}; y += S(hwnd, 30 + 46);
            tl = {p, y, rc.right - p, y + S(hwnd, 34)}; y = tl.bottom + S(hwnd, 8);
            MoveWindow(summary, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 26);
            MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_TIMER: {
            ULONGLONG t = GetTickCount64();
            if (playing) {
                double dt = (t - last) / 1000.0;
                size_t before = cur_packet();
                pos += dt * rate;
                if (pos >= units) { pos = units; playing = false; set_text(playb, "\xE2\x96\xB6 Play"); }
                if (cur_packet() != before || !playing) seek(pos); else { InvalidateRect(hwnd, &tl, FALSE); InvalidateRect(hwnd, &clock, FALSE); }
            }
            last = t;
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_MOUSEMOVE: {
            if (m == WM_MOUSEMOVE && !(w & MK_LBUTTON)) return 0;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&tl, pt) && units > 0) seek((double)(pt.x - tl.left) / (tl.right - tl.left) * units);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == NM_CLICK || nh->code == LVN_ITEMACTIVATE) { int i = ((NMITEMACTIVATE *)l)->iItem; if (i >= 0 && i < (int)stamps.size()) seek(pk[stamps[i]].u, true); }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 401) {
                playing = !playing; last = GetTickCount64();
                if (playing && pos >= units) pos = pk.empty() ? 0 : pk[0].u;
                set_text(playb, playing ? "\xE2\x8F\xB8 Pause" : "\xE2\x96\xB6 Play");
            } else if (id == 402) { std::string r = data.get_str("report"); if (exists(r)) open_path(r); }
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_SPACE) SendMessageW(hwnd, WM_COMMAND, 401, 0);
            return 0;
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

void amol_window(HWND owner, const std::string &json) {
    auto *w = new AmolWin;
    w->data = load_json(json, Json::object());
    if (w->data.get_str("report").empty()) w->data["report"] = stem_path(json) + ".txt";
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, 900));
    w->create(L"TRAmol", "AMOL \xE2\x80\x94 " + w->data.get_str("source"), WS_OVERLAPPEDWINDOW, width, S(owner, 720), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}
