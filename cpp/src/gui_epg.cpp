// Окна служебных данных строки 21 и программы передач:
//   XDS — карточка станции, часы эфира (местное время по поясу из XDS), передача, рейтинг, ход по записи;
//   StarSight — транспорт программы передач: пакеты, нумерация, потери, байты пакета.
#include "gui_app.h"
#include <windowsx.h>
#include <algorithm>
#include <ctime>

namespace ui {
namespace {
const COLORREF C_CHAN = RGB(0x3a, 0x7b, 0xd5), C_TIME = RGB(0xff, 0xa3, 0x1a), C_PROG = RGB(0x2e, 0xa0, 0x6a), C_OTHER = RGB(0x8a, 0x8d, 0x93);
std::string rec_time(double s) { return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)); }

// общая полоса хода по записи: отметки событий, текущее место; щелчок — переход
struct Timeline {
    RECT r{};
    void paint(HDC dc, int units, double pos, const std::vector<std::pair<double, COLORREF>> &marks) const {
        fill(dc, r, RGB(0x24, 0x26, 0x2b));
        if (units <= 0) return;
        int W = r.right - r.left;
        for (auto &m : marks) { int x = r.left + (int)(m.first / units * W); RECT b{x, r.top + 4, x + 2, r.bottom - 4}; fill(dc, b, m.second); }
        int cx = r.left + (int)(pos / units * W);
        RECT c{cx - 1, r.top, cx + 2, r.bottom}; fill(dc, c, RGB(255, 255, 255));
    }
    bool hit(POINT p, int units, double &pos) const {
        if (!PtInRect(&r, p) || units <= 0) return false;
        pos = (double)(p.x - r.left) / (r.right - r.left) * units; return true;
    }
};

// ---------------------------------------------------------------- XDS
struct XdsWin : Window {
    Json data; double rate = 59.94, pos = 0; int units = 0; bool playing = false; ULONGLONG last = 0;
    struct Ev { double u; std::string key, val; bool has_t = false; int y = 0, mo = 0, d = 0, h = 0, mi = 0, dw = 0; };
    std::vector<Ev> ev;
    HWND title, sub, playb, repb, lst;
    RECT card{}, clock{}, prog{}; Timeline tl;
    void load() {
        rate = data["rate"].num(59.94); units = data["units"].integer();
        for (auto &e : data["events"].a) {
            Ev x; x.u = e[0].num(); x.key = e[1].str(); x.val = e[2].str();
            if (e.size() >= 4 && e[3].is_arr() && e[3].size() >= 6) { x.has_t = true; x.y = e[3][0].integer(); x.mo = e[3][1].integer(); x.d = e[3][2].integer(); x.h = e[3][3].integer(); x.mi = e[3][4].integer(); x.dw = e[3][5].integer(); }
            ev.push_back(x);
        }
        if (!ev.empty()) pos = ev[0].u;
    }
    COLORREF colour(const std::string &k) const {
        if (k == "network" || k == "station" || k == "tape delay" || k == "transmission ID") return C_CHAN;
        if (k == "time" || k == "time zone") return C_TIME;
        if (k.find("programme") != std::string::npos || k == "rating" || k.rfind("description", 0) == 0) return C_PROG;
        return C_OTHER;
    }
    std::string top(const char *k) const { const Json *t = data["top"].find(k); return t ? t->str() : ""; }
    // время эфира в текущем месте записи: последняя отметка времени + прошедшее с неё
    bool now_utc(std::tm &out) const {
        const Ev *b = nullptr;
        for (auto &e : ev) if (e.has_t && e.u <= pos) b = &e;
        if (!b) for (auto &e : ev) if (e.has_t) { b = &e; break; }
        if (!b) return false;
        std::tm t{}; t.tm_year = b->y - 1900; t.tm_mon = b->mo - 1; t.tm_mday = b->d; t.tm_hour = b->h; t.tm_min = b->mi;
        time_t s = _mkgmtime(&t) + (time_t)((pos - b->u) / rate);
        gmtime_s(&out, &s); return true;
    }
    void fill_list() {
        ListView_DeleteAllItems(lst);
        int r = 0; std::string prev;
        for (auto &e : ev) {
            std::string k = e.key + "|" + e.val;
            if (k == prev) continue;                    // повторы подряд свёрнуты
            prev = k;
            lv_set(lst, r, 0, rec_time(e.u / rate)); lv_set(lst, r, 1, e.key); lv_set(lst, r, 2, e.val); r++;
        }
    }
    void paint(HDC mem, RECT rc) {
        fill(mem, rc, BG);
        HFONT huge = font(hwnd, 40, true), mid = font(hwnd, 14, true), small = font(hwnd, 9), smallb = font(hwnd, 10, true);
        HFONT led = font(hwnd, 38, true, L"Consolas"), ledm = font(hwnd, 13, true, L"Consolas");
        // карточка станции
        fill(mem, card, PANEL);
        std::string st = top("station"), net = top("network");
        std::string call = st.substr(0, st.find(' '));
        text_out(mem, card.left + S(hwnd, 18), card.top + S(hwnd, 10), call.empty() ? "\xE2\x80\x94" : call, FG, huge);
        if (!net.empty()) {
            RECT nb{card.left + S(hwnd, 20), card.top + S(hwnd, 76), card.left + S(hwnd, 20) + S(hwnd, 16) + (int)net.size() * S(hwnd, 12), card.top + S(hwnd, 104)};
            fill(mem, nb, C_CHAN); text_out(mem, nb.left + S(hwnd, 8), nb.top + S(hwnd, 3), net, RGB(255, 255, 255), smallb);
        }
        if (st.find('(') != std::string::npos) text_out(mem, card.left + S(hwnd, 130), card.top + S(hwnd, 80), st.substr(st.find('(')), MUTED, small);
        std::string tz;
        if (data.has("tz")) {
            static const char *ZN[] = {"", "", "", "", "Atlantic", "Eastern", "Central", "Mountain", "Pacific", "Alaska", "Hawaii", "Samoa"};
            int z = data["tz"].integer();
            tz = fmt("time zone UTC\xE2\x88\x92%d", z) + (z >= 4 && z <= 11 ? std::string(" (") + ZN[z] + ")" : "") + (data.get_bool("dst") ? ", observes daylight saving" : "");
        }
        text_out(mem, card.left + S(hwnd, 20), card.bottom - S(hwnd, 26), tz.empty() ? "station data from XDS" : tz, MUTED, small);
        // часы эфира
        fill(mem, clock, RGB(0x11, 0x11, 0x11));
        RECT in{clock.left + S(hwnd, 10), clock.top + S(hwnd, 10), clock.right - S(hwnd, 10), clock.bottom - S(hwnd, 10)}; fill(mem, in, RGB(0, 0, 0));
        std::tm u{};
        if (now_utc(u)) {
            static const char *DW[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}, *MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            std::tm l = u;
            bool local = data.has("tz");
            if (local) { time_t s = _mkgmtime(&u) - (time_t)data["tz"].integer() * 3600; gmtime_s(&l, &s); }
            text_out(mem, in.left + S(hwnd, 18), in.top + S(hwnd, 8), fmt("%02d:%02d:%02d", l.tm_hour, l.tm_min, l.tm_sec), C_TIME, led);
            text_out(mem, in.left + S(hwnd, 20), in.top + S(hwnd, 70), fmt("%s %d %s %d", DW[l.tm_wday], l.tm_mday, MO[l.tm_mon], l.tm_year + 1900) + (local ? "  local" : "  UTC"), ACCENT2, ledm);
            if (local) text_out(mem, in.right - S(hwnd, 16), in.top + S(hwnd, 74), fmt("%02d:%02d UTC %s %d %s", u.tm_hour, u.tm_min, DW[u.tm_wday], u.tm_mday, MO[u.tm_mon]), MUTED, small, TA_RIGHT);
        } else text_out(mem, in.left + S(hwnd, 18), in.top + S(hwnd, 20), "no time-of-day packets", MUTED, mid);
        text_out(mem, in.right - S(hwnd, 16), in.top + S(hwnd, 10), "recording " + rec_time(pos / rate), DIM, small, TA_RIGHT);
        // передача
        fill(mem, prog, PANEL);
        std::string pn = top("programme"), rt = top("rating"), pl = top("programme length"), ps = top("programme start"), nx = top("next programme");
        int y = prog.top + S(hwnd, 10), x = prog.left + S(hwnd, 18);
        text_out(mem, x, y, "On air", MUTED, small); y += S(hwnd, 18);
        text_out(mem, x, y, pn.empty() ? "the station sent no programme name" : pn, pn.empty() ? DIM : FG, mid); y += S(hwnd, 30);
        std::string meta;
        for (auto &p : {std::make_pair("rating ", rt), std::make_pair("length ", pl), std::make_pair("started ", ps), std::make_pair("next: ", nx)}) if (!p.second.empty()) meta += (meta.empty() ? "" : "   \xC2\xB7   ") + std::string(p.first) + p.second;
        if (!meta.empty()) text_out(mem, x, y, meta, ACCENT2, small);
        // полоса
        std::vector<std::pair<double, COLORREF>> marks;
        for (auto &e : ev) marks.push_back({e.u, colour(e.key)});
        tl.paint(mem, units, pos, marks);
        int lx = tl.r.left, ly = tl.r.bottom + S(hwnd, 6);
        for (auto &k : {std::make_pair("station", C_CHAN), std::make_pair("time", C_TIME), std::make_pair("programme", C_PROG), std::make_pair("other", C_OTHER)}) {
            RECT sw{lx, ly + S(hwnd, 4), lx + S(hwnd, 10), ly + S(hwnd, 14)}; fill(mem, sw, k.second);
            text_out(mem, lx + S(hwnd, 14), ly, k.first, MUTED, small); lx += S(hwnd, 100);
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            title = label(hwnd, "XDS"); SendMessageW(title, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            sub = label(hwnd, fmt("extended data services \xC2\xB7 line %d field %s \xC2\xB7 ", data["line"].integer(), data.get_str("field").c_str()) + data.get_str("source"));
            set_label_colour(sub, MUTED);
            playb = button(hwnd, 401, "\xE2\x96\xB6 Play"); repb = button(hwnd, 402, "Text report");
            lst = listview(hwnd, 10, {{"Recording", 100}, {"Packet", 150}, {"Value", 420}});
            load(); fill_list();
            SetTimer(hwnd, 1, 100, nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = S(hwnd, 10), bh = S(hwnd, 30);
            MoveWindow(title, p, y + S(hwnd, 4), S(hwnd, 50), S(hwnd, 24), TRUE);
            MoveWindow(sub, p + S(hwnd, 54), y + S(hwnd, 8), rc.right - 2 * p - S(hwnd, 320), S(hwnd, 20), TRUE);
            MoveWindow(playb, rc.right - p - S(hwnd, 250), y, S(hwnd, 100), bh, TRUE);
            MoveWindow(repb, rc.right - p - S(hwnd, 140), y, S(hwnd, 140), bh, TRUE);
            y += bh + S(hwnd, 10);
            int cw = std::max((int)S(hwnd, 300), (int)(rc.right - 2 * p) * 2 / 5);
            card = {p, y, p + cw, y + S(hwnd, 140)};
            clock = {card.right + S(hwnd, 10), y, rc.right - p, y + S(hwnd, 140)};
            y = card.bottom + S(hwnd, 10);
            prog = {p, y, rc.right - p, y + S(hwnd, 90)}; y = prog.bottom + S(hwnd, 10);
            tl.r = {p, y, rc.right - p, y + S(hwnd, 30)}; y = tl.r.bottom + S(hwnd, 30);
            MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_TIMER: {
            ULONGLONG t = GetTickCount64();
            if (playing) { pos += (t - last) / 1000.0 * rate; if (pos >= units) { pos = units; playing = false; set_text(playb, "\xE2\x96\xB6 Play"); } InvalidateRect(hwnd, nullptr, FALSE); }
            last = t; return 0;
        }
        case WM_LBUTTONDOWN: case WM_MOUSEMOVE: {
            if (m == WM_MOUSEMOVE && !(w & MK_LBUTTON)) return 0;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; double np;
            if (tl.hit(pt, units, np)) { pos = np; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            paint(mem, rc); BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == NM_CLICK) {
                    int i = ((NMITEMACTIVATE *)l)->iItem;
                    if (i >= 0) { wchar_t buf[64]; ListView_GetItemText(lst, i, 0, buf, 64); int h = 0, mi = 0; double s = 0; if (swscanf(buf, L"%d:%d:%lf", &h, &mi, &s) == 3) { pos = (h * 3600 + mi * 60 + s) * rate; InvalidateRect(hwnd, nullptr, FALSE); } }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 401) { playing = !playing; last = GetTickCount64(); set_text(playb, playing ? "\xE2\x8F\xB8 Pause" : "\xE2\x96\xB6 Play"); }
            else if (id == 402) { std::string r = data.get_str("report"); if (exists(r)) open_path(r); }
            return 0;
        }
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ---------------------------------------------------------------- StarSight
struct StarWin : Window {
    Json data; double rate = 59.94, pos = 0; int units = 0, sel = 0;
    struct Pk { double u; char field; int seq, type; std::vector<u8> b; };
    std::vector<Pk> pk;
    int lost = 0;
    HWND title, sub, repb, lst;
    RECT card{}, hex{}; Timeline tl;
    void load() {
        rate = data["rate"].num(59.94); units = data["units"].integer();
        for (auto &e : data["packets"].a) {
            Pk p; p.u = e[0].num(); p.field = e[1].str().empty() ? 'A' : e[1].str()[0]; p.seq = e[2].integer(); p.type = e[3].integer();
            std::string h = e[4].str(); for (size_t i = 0; i + 1 < h.size(); i += 2) p.b.push_back((u8)std::stoi(h.substr(i, 2), nullptr, 16));
            pk.push_back(p);
        }
        for (size_t i = 1; i < pk.size(); i++) { int d = (pk[i].seq - pk[i - 1].seq + 256) % 256; if (d > 1 && d < 40) lost += d - 1; }
    }
    COLORREF tcol(int t) const { return t == 0xE8 ? C_TIME : t == 0x00 ? C_CHAN : C_PROG; }
    void select(int i, bool from_list = false) {
        if (pk.empty()) return;
        sel = std::max(0, std::min((int)pk.size() - 1, i)); pos = pk[sel].u;
        if (!from_list) { ListView_SetItemState(lst, -1, 0, LVIS_SELECTED); ListView_SetItemState(lst, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lst, sel, FALSE); }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void paint(HDC mem, RECT rc) {
        fill(mem, rc, BG);
        HFONT big = font(hwnd, 26, true), smallb = font(hwnd, 10, true), small = font(hwnd, 9), mono = font(hwnd, 10, false, L"Consolas");
        fill(mem, card, PANEL);
        int x = card.left + S(hwnd, 18), y = card.top + S(hwnd, 10);
        text_out(mem, x, y, "StarSight", C_TIME, big);
        text_out(mem, x, card.bottom - S(hwnd, 44), "on-screen programme guide for TV sets and VCRs, carried by PBS stations", MUTED, small);
        text_out(mem, x, card.bottom - S(hwnd, 24), "the listings are compressed and encrypted; shown here: packets, numbering, timing and bytes", DIM, small);
        // плитки
        int nE8 = 0, n00 = 0; size_t bytes = 0; for (auto &p : pk) { (p.type == 0xE8 ? nE8 : n00)++; bytes += p.b.size(); }
        double secs = units / rate;
        std::vector<std::pair<std::string, std::string>> tiles = {
            {fmt("%zu", pk.size()), "packets"},
            {pk.empty() ? "-" : fmt("#%02X\xE2\x80\x93#%02X", pk.front().seq, pk.back().seq), "numbering"},
            {fmt("%d", lost), "lost"},
            {fmt("%.0f", secs > 0 ? bytes / secs : 0), "bytes/s"},
            {fmt("%d / %d", nE8, n00), "type E8 / 00"}};
        int tx = card.right - S(hwnd, 16);
        for (int i = (int)tiles.size() - 1; i >= 0; i--) {
            int w = S(hwnd, 110); RECT t{tx - w, card.top + S(hwnd, 14), tx, card.top + S(hwnd, 74)};
            fill(mem, t, FIELD);
            text_out(mem, (t.left + t.right) / 2, t.top + S(hwnd, 8), tiles[i].first, FG, smallb, TA_CENTER);
            text_out(mem, (t.left + t.right) / 2, t.top + S(hwnd, 32), tiles[i].second, MUTED, small, TA_CENTER);
            tx -= w + S(hwnd, 8);
        }
        // полоса пакетов
        std::vector<std::pair<double, COLORREF>> marks;
        for (auto &p : pk) marks.push_back({p.u, tcol(p.type)});
        tl.paint(mem, units, pos, marks);
        // байты выбранного пакета
        fill(mem, hex, RGB(0x11, 0x11, 0x11));
        if (!pk.empty()) {
            const Pk &p = pk[sel];
            text_out(mem, hex.left + S(hwnd, 10), hex.top + S(hwnd, 6), fmt("packet #%02X \xC2\xB7 type %02X \xC2\xB7 field %c \xC2\xB7 %zu bytes \xC2\xB7 at %s", p.seq, p.type, p.field, p.b.size(), rec_time(p.u / rate).c_str()), FG, smallb);
            int cw = S(hwnd, 24), rh = S(hwnd, 18), x0 = hex.left + S(hwnd, 60), y0 = hex.top + S(hwnd, 30);
            int per = std::max(8, (int)((hex.right - x0 - S(hwnd, 10)) / cw) / 8 * 8);
            for (size_t i = 0; i < p.b.size(); i++) {
                int r = (int)(i / per), c = (int)(i % per);
                int yy = y0 + r * rh; if (yy + rh > hex.bottom) break;
                if (c == 0) text_out(mem, hex.left + S(hwnd, 10), yy, fmt("%04zX", i), DIM, mono);
                COLORREF col = FG;
                if (i < 3) col = MUTED; else if (i == 3) col = tcol(p.type); else if (i == 4) col = MUTED; else if (i == 8) col = C_TIME;
                if (p.b[i] == 3) col = DIM;
                text_out(mem, x0 + c * cw, yy, fmt("%02X", p.b[i]), col, mono);
            }
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            title = label(hwnd, "StarSight"); SendMessageW(title, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            sub = label(hwnd, fmt("programme guide data \xC2\xB7 line %d \xC2\xB7 ", data["line"].integer()) + data.get_str("source")); set_label_colour(sub, MUTED);
            repb = button(hwnd, 402, "Text report");
            lst = listview(hwnd, 10, {{"Recording", 100}, {"Field", 50}, {"Packet", 70}, {"Type", 60}, {"Bytes", 60}});
            load();
            for (size_t i = 0; i < pk.size(); i++) {
                lv_set(lst, (int)i, 0, rec_time(pk[i].u / rate)); lv_set(lst, (int)i, 1, std::string(1, pk[i].field));
                lv_set(lst, (int)i, 2, fmt("#%02X", pk[i].seq)); lv_set(lst, (int)i, 3, fmt("%02X", pk[i].type)); lv_set(lst, (int)i, 4, pk[i].b.size() >= 400 ? std::string("400+") : fmt("%zu", pk[i].b.size()));
            }
            select(0);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = S(hwnd, 10), bh = S(hwnd, 30);
            MoveWindow(title, p, y + S(hwnd, 4), S(hwnd, 100), S(hwnd, 24), TRUE);
            MoveWindow(sub, p + S(hwnd, 104), y + S(hwnd, 8), rc.right - 2 * p - S(hwnd, 270), S(hwnd, 20), TRUE);
            MoveWindow(repb, rc.right - p - S(hwnd, 140), y, S(hwnd, 140), bh, TRUE);
            y += bh + S(hwnd, 10);
            card = {p, y, rc.right - p, y + S(hwnd, 130)}; y = card.bottom + S(hwnd, 10);
            tl.r = {p, y, rc.right - p, y + S(hwnd, 30)}; y = tl.r.bottom + S(hwnd, 10);
            int lw = S(hwnd, 380);
            MoveWindow(lst, p, y, lw, std::max(0L, rc.bottom - y - p), TRUE);
            hex = {p + lw + S(hwnd, 10), y, rc.right - p, rc.bottom - p};
            ListView_EnsureVisible(lst, 0, FALSE); ListView_EnsureVisible(lst, sel, FALSE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; double np;
            if (tl.hit(pt, units, np)) { int b = 0; double bd = 1e18; for (size_t i = 0; i < pk.size(); i++) if (fabs(pk[i].u - np) < bd) { bd = fabs(pk[i].u - np); b = (int)i; } select(b); }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            paint(mem, rc); BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == NM_CLICK || nh->code == LVN_ITEMCHANGED) { int i = nh->code == NM_CLICK ? ((NMITEMACTIVATE *)l)->iItem : ListView_GetNextItem(lst, -1, LVNI_SELECTED); if (i >= 0 && i != sel) select(i, true); }
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == 402) { std::string r = stem_path(data.get_str("json_path")) + ".txt"; if (exists(r)) open_path(r); }
            return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
}

void xds_window(HWND owner, const std::string &json) {
    auto *w = new XdsWin; w->data = load_json(json, Json::object());
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, 940));
    w->create(L"TRXds", "XDS \xE2\x80\x94 " + w->data.get_str("source"), WS_OVERLAPPEDWINDOW, width, S(owner, 760), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
void starsight_window(HWND owner, const std::string &json) {
    auto *w = new StarWin; w->data = load_json(json, Json::object()); w->data["json_path"] = json;
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, 1100));
    w->create(L"TRStar", "StarSight \xE2\x80\x94 " + w->data.get_str("source"), WS_OVERLAPPEDWINDOW, width, S(owner, 760), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}
