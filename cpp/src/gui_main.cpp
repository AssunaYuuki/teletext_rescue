// Teletext Rescue — main window: start screen (recent recordings, the "time & temperature" LED board,
// what the program reads); a .vbi recording is examined in a progress window and its results open in
// the "What is in" window and one window per service.
#include "version.h"
#include "gui_app.h"
#include "flac.h"
#include "rescache.h"
#include "slicer.h"
#include "ssjson.h"
#include "export.h"
#include <shellapi.h>
#include <windowsx.h>
#include <winhttp.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ui {

void open_service(HWND owner, const ResultPtr &r, const Service &s)
{
    switch (s.window) {
    case ServiceWindow::StarSight: starsight_window(owner, r); break;
    case ServiceWindow::Captions: captions_window(owner, r); break;
    case ServiceWindow::Xds: xds_window(owner, r); break;
    case ServiceWindow::Sign: sign_window(owner, r); break;
    case ServiceWindow::Amol: amol_window(owner, r); break;
    case ServiceWindow::Nabts: nabts_window(owner, r); break;
    case ServiceWindow::Teletext: teletext_window(owner, r); break;
    case ServiceWindow::Data: data_window(owner, r, s.dataKind); break;
    case ServiceWindow::Gemstar: gemstar_window(owner, r); break;
    case ServiceWindow::Jtes: jtes_window(owner, r); break;
    case ServiceWindow::Tvinform: tvinform_window(owner, r); break;
    case ServiceWindow::Finds: finds_window(owner, r); break;
    case ServiceWindow::LegacyFile:
        if (ends_with_i(s.dataKind, ".txt")) text_window(owner, s.name + " \xE2\x80\x94 " + file_name(*r), s.note, read_text(s.dataKind));
        else open_path(s.dataKind);
        break;
    case ServiceWindow::Vitc: {
        std::string t;
        for (auto &l : r->vitcReport) t += l + "\n";
        t += "\nrecording -> time code (every second)\n";
        int64_t step = std::max<int64_t>(1, (int64_t)std::lround(frame_rate(*r)));
        for (size_t i = 0; i < r->vitc.size(); i++)
            if (i == 0 || r->vitc[i].frame / step != r->vitc[i - 1].frame / step)
                t += "[" + rec_time(r->vitc[i].frame / frame_rate(*r)) + "]  " + r->vitc[i].text() + fmt("  line %d f%d", r->vitc[i].lineNo, r->vitc[i].field) +
                     (r->vitc[i].user ? fmt("  user bits %08X", r->vitc[i].user) : "") + "\n";
        text_window(owner, "VITC time code \xE2\x80\x94 " + file_name(*r), "time code of the tape, read from the VBI; captions are timed by it", t);
        break;
    }
    case ServiceWindow::Report: text_window(owner, "Report \xE2\x80\x94 " + file_name(*r), "everything the decoder found and did", full_report(*r)); break;
    }
}

// ================================================================ dark menu bar (undocumented UAH messages)
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
typedef union { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static void paint_menu_line(HWND h)
{
    MENUBARINFO mbi{sizeof mbi};
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return;
    RECT rc, rw; GetClientRect(h, &rc); MapWindowPoints(h, nullptr, (POINT *)&rc, 2);
    GetWindowRect(h, &rw); OffsetRect(&rc, -rw.left, -rw.top);
    RECT line = rc; line.bottom = line.top; line.top--;
    HDC dc = GetWindowDC(h); fill(dc, line, BG); ReleaseDC(h, dc);
}

bool dark_menubar(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT &res)
{
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

// ================================================================ "time & temperature" LED board
namespace {
const int LCOLS = 96, LROWS = 15;
const char *ICONS[7][13] = {
    {"......#......", "..#...#...#..", "...#.....#...", ".....###.....", "....#####....", "#..#######..#", "...#######...", "#..#######..#", "....#####....", ".....###.....", "...#.....#...", "..#...#...#..", "......#......"},
    {".............", ".............", ".....###.....", "...##...##...", "..#.......#..", ".##........#.", "#...........#", "#...........#", ".###########.", ".............", ".............", ".............", "............."},
    {".#..#........", "..###........", "#######......", "..####.###...", ".#..##...##..", "...#.......#.", "..#.........#", ".#..........#", ".###########.", ".............", ".............", ".............", "............."},
    {".....###.....", "...##...##...", "..#.......#..", ".#.........#.", "#...........#", ".###########.", ".............", "..#...#...#..", ".#...#...#...", ".............", "...#...#...#.", "..#...#...#..", "............."},
    {".....###.....", "...##...##...", "..#.......#..", ".#.........#.", "#...........#", ".###########.", ".............", ".#...#...#...", "..#...#...#.", ".#...#...#...", ".............", "...#...#...#.", "............."},
    {".....###.....", "...##...##...", "..#.......#..", ".#.........#.", "#...........#", ".#####.#####.", ".....##......", "....##.......", "...#####.....", ".....##......", "....##.......", "....#........", "............."},
    {".............", ".............", "#############", ".............", ".###########.", ".............", "#############", ".............", ".###########.", ".............", "#############", ".............", "............."}};
enum { I_SUN, I_CLOUD, I_PARTLY, I_RAIN, I_SNOW, I_STORM, I_FOG };

struct Weather { std::string place, cond, temp, wind, hum; };
std::mutex wx_mu; std::shared_ptr<Weather> wx; std::atomic<bool> wx_started{false};

std::string http_get(const wchar_t *host, const wchar_t *path)
{
    std::string out;
    HINTERNET s = WinHttpOpen(L"curl/8", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;
    WinHttpSetTimeouts(s, 20000, 20000, 20000, 20000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD n = 0;
        while (WinHttpQueryDataAvailable(r, &n) && n) {
            std::string buf(n, 0); DWORD got = 0;
            if (!WinHttpReadData(r, buf.data(), n, &got)) break;
            out.append(buf.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}

void weather_thread()
{
    for (;;) {
        std::string t = strip(http_get(L"wttr.in", L"/?format=%l|%C|%t|%w|%h&m&lang=en"));
        auto p = split(t, '|');
        if (p.size() >= 5 && !strip(p[2]).empty() && t.find('<') == std::string::npos) {
            auto w = std::make_shared<Weather>();
            std::string place = strip(split(p[0], ',')[0]);
            for (auto &ch : place) ch = (char)toupper((u8)ch);
            w->place = place; w->cond = strip(p[1]); w->temp = strip(p[2]); w->hum = strip(p[4]);
            for (char ch : p[3]) if (isalnum((u8)ch) || ch == '/' || ch == '.') w->wind += ch;
            { std::lock_guard<std::mutex> lk(wx_mu); wx = w; }
            Sleep(30 * 60 * 1000);
        } else Sleep(120 * 1000);
    }
}

int icon_of(std::string c)
{
    c = lower(c);
    auto has = [&](const char *w) { return c.find(w) != std::string::npos; };
    if (has("thunder") || has("storm")) return I_STORM;
    if (has("snow") || has("sleet") || has("ice") || has("blizzard")) return I_SNOW;
    if (has("rain") || has("drizzle") || has("shower")) return I_RAIN;
    if (has("fog") || has("mist") || has("haze") || has("smog") || has("smoke")) return I_FOG;
    if (has("partly") || has("patchy")) return I_PARTLY;
    if (has("cloud") || has("overcast")) return I_CLOUD;
    return I_SUN;
}

std::vector<int> tcols(const std::string &s)
{
    std::vector<int> out;
    for (char32_t ch : to_u32(s)) {
        std::vector<int> g = ch == 0xB0 ? std::vector<int>{0x06, 0x09, 0x09, 0x06} : led_glyph(ch);
        out.insert(out.end(), g.begin(), g.end()); out.push_back(0);
    }
    if (!out.empty()) out.pop_back();
    return out;
}

void put_line(std::vector<int> &buf, const std::string &s, int row, int x0 = -9999, int right = -9999)
{
    auto c = tcols(s);
    int x = x0 == -9999 ? (LCOLS - (int)c.size()) / 2 : x0;
    if (right != -9999) x = right - (int)c.size();
    for (size_t k = 0; k < c.size(); k++) if (x + (int)k >= 0 && x + (int)k < LCOLS) buf[x + k] |= c[k] << row;
}
}

struct LedBoard : Window {
    std::vector<int> shown = std::vector<int>(LCOLS, 0), prev = std::vector<int>(LCOLS, 0);
    int page = 0; DWORD t_page = GetTickCount();
    int pages() { std::lock_guard<std::mutex> lk(wx_mu); return wx ? 4 : 2; }
    std::vector<int> frame(int pg, bool colon)
    {
        std::vector<int> buf(LCOLS, 0);
        SYSTEMTIME st; GetLocalTime(&st);
        std::shared_ptr<Weather> w; { std::lock_guard<std::mutex> lk(wx_mu); w = wx; }
        int np = w ? 4 : 2;
        if (pg == 0) {
            auto hh = tcols(fmt("%02d", st.wHour)), mm = tcols(fmt("%02d", st.wMinute)), dots = tcols(":");
            std::vector<int> c = hh; c.push_back(0);
            for (int v : dots) c.push_back(colon ? v : 0);
            c.push_back(0); c.insert(c.end(), mm.begin(), mm.end());
            int x = (LCOLS - (int)c.size()) / 2;
            for (size_t k = 0; k < c.size(); k++) buf[x + k] |= c[k];
            static const char *dn[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
            static const char *mn[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
            put_line(buf, fmt("%s %02d %s", dn[st.wDayOfWeek], st.wDay, mn[st.wMonth - 1]), 8);
        } else if (pg == np - 1) { put_line(buf, "TELETEXT", 0); put_line(buf, "RESCUE", 8); }
        else if (pg == 1 && w) {
            int ic = icon_of(w->cond);
            for (int y = 0; y < 13; y++) for (int x = 0; x < 13; x++) if (ICONS[ic][y][x] == '#' && 2 + x < LCOLS && 1 + y < LROWS) buf[2 + x] |= 1 << (1 + y);
            put_line(buf, w->place.substr(0, 12), 0, 19);
            put_line(buf, replace_all(w->temp, "+", ""), 8, 19);
        } else if (w) {
            std::string c = w->cond; for (auto &ch : c) ch = (char)toupper((u8)ch);
            put_line(buf, c.substr(0, 15), 0);
            std::string wd = w->wind; for (auto &ch : wd) ch = (char)toupper((u8)ch);
            put_line(buf, ("WIND " + wd + "  " + w->hum).substr(0, 16), 8);
        }
        return buf;
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            if (!wx_started.exchange(true)) std::thread(weather_thread).detach();
            SetTimer(hwnd, 1, 40, nullptr); return 0;
        case WM_TIMER: {
            DWORD now = GetTickCount();
            if (now - t_page >= 4000) { prev = shown; page = (page + 1) % pages(); t_page = now; }
            double el = (now - t_page) / 1000.0;
            auto target = frame(page % pages(), (now / 500) % 2 == 0);
            int k = el >= 0.5 ? LCOLS : (int)(el / 0.5 * LCOLS);
            bool ch = false;
            for (int x = 0; x < LCOLS; x++) { int v = x < k ? target[x] : prev[x]; if (v != shown[x]) { shown[x] = v; ch = true; } }
            if (ch) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            fill(mem, rc, RGB(0x14, 0x14, 0x14));
            int d = std::max<int>(2, std::min<int>((rc.right - S(hwnd, 24)) / LCOLS, (rc.bottom - S(hwnd, 24)) / LROWS));
            int ox = (rc.right - d * LCOLS) / 2, oy = (rc.bottom - d * LROWS) / 2;
            RECT in{ox - S(hwnd, 8), oy - S(hwnd, 8), ox + d * LCOLS + S(hwnd, 8), oy + d * LROWS + S(hwnd, 8)};
            fill(mem, in, RGB(0x0a, 0x07, 0x03));
            HBRUSH on = CreateSolidBrush(RGB(0xff, 0xb2, 0x1e)), off = CreateSolidBrush(RGB(0x2a, 0x1a, 0x08));
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
            for (int x = 0; x < LCOLS; x++) for (int y = 0; y < LROWS; y++) {
                SelectObject(mem, (shown[x] >> y) & 1 ? on : off);
                Ellipse(mem, ox + x * d + 1, oy + y * d + 1, ox + x * d + d, oy + y * d + d);
            }
            SelectObject(mem, op); DeleteObject(on); DeleteObject(off);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ start screen
struct StartActions {
    std::function<void()> open_vbi, open_packets, pick_device, capture, view_lines;
    std::function<void(const Json &)> open_recent;
};

struct StartScreen : Window {
    StartActions act;
    HWND title, sub, b_vbi, b_pk, b_dev, b_cap, b_view, hint, rec_h, svc_h, formats;
    std::vector<HWND> recent_btns; std::vector<Json> recent;
    std::vector<std::pair<std::string, std::string>> services = {
        {"StarSight", "StarSight programme guide \xE2\x80\x94 channels, schedules, titles (PBS, line 16)"},
        {"CC", "CC captions, text mode and XDS station data (line 21)"},
        {"Silent Radio", "Silent Radio \xE2\x80\x94 LED news sign service"},
        {"NABTS", "NABTS packets at 5.73 Mbit/s"},
        {"WST teletext", "WST teletext \xE2\x80\x94 525-line (NTSC)"},
        {"encrypted datacast", "Encrypted datacast with framing 2D (5.73 Mbit/s)"},
        {"AMOL", "Nielsen AMOL \xE2\x80\x94 source ID and station clock (line 20)"}};
    HWND led = nullptr;
    RECT card1{}, card2{};
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            title = label(hwnd, "Teletext Rescue"); SendMessageW(title, WM_SETFONT, (WPARAM)font(hwnd, 22, true, L"Segoe UI Semibold"), TRUE); set_label_colour(title, ACCENT);
            sub = label(hwnd, "StarSight, Silent Radio, captions and data\nhidden in the vertical blanking interval"); set_label_colour(sub, MUTED);
            b_vbi = button(hwnd, 1, "\xF0\x9F\x93\xBC  Open .vbi recording\xE2\x80\xA6", BTN_ACCENT);
            b_pk = button(hwnd, 3, "Open packets .t42 / .t34 / .t33\xE2\x80\xA6", BTN_BIG);
            b_dev = button(hwnd, 2, "Decode on: graphics card or processor\xE2\x80\xA6", BTN_BIG);
            b_cap = button(hwnd, 4, "\xF0\x9F\x93\xBA  Capture card (live)\xE2\x80\xA6", BTN_BIG);
            b_view = button(hwnd, 5, "\xF0\x9F\x93\x88  View VBI lines (no decoding)\xE2\x80\xA6", BTN_LINK);
            hint = label(hwnd, "A recording is examined automatically: the capture chip and format (bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx, 4fsc .tbc) and every VBI line are identified and everything readable is decoded and opened.");
            set_label_colour(hint, MUTED);
            rec_h = label(hwnd, "Recent"); SetPropW(rec_h, L"card", (HANDLE)1); SendMessageW(rec_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            svc_h = label(hwnd, "What it reads"); SetPropW(svc_h, L"card", (HANDLE)1); SendMessageW(svc_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            formats = label(hwnd, "Chips: bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx (PAL/SECAM/NTSC); .tbc (4fsc, 16 bit)");
            SetPropW(formats, L"card", (HANDLE)1); set_label_colour(formats, MUTED);
            recent = recent_list();
            for (size_t i = 0; i < recent.size() && i < 8; i++) {
                std::string p = recent[i].get_str("path");
                recent_btns.push_back(button(hwnd, 100 + (int)i, "\xF0\x9F\x93\xBC  " + basename(p) + "     " + dirname(p) + "    " + recent[i].get_str("time"), BTN_LINK));
            }
            if (recent.empty()) { HWND e = label(hwnd, "Nothing yet \xE2\x80\x94 open a recording."); SetPropW(e, L"card", (HANDLE)1); set_label_colour(e, MUTED); recent_btns.push_back(e); }
            auto *b = new LedBoard;
            Window::register_class(L"TRLed", brBG);
            led = CreateWindowExW(0, L"TRLed", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, nullptr, inst(), b);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int px = S(hwnd, 40), y = S(hwnd, 28);
            int lw = S(hwnd, 96 * 4 + 24), lh = S(hwnd, 15 * 4 + 24);
            MoveWindow(led, rc.right - px - lw, y, lw, lh, TRUE);
            MoveWindow(title, px, y, rc.right - 2 * px - lw, S(hwnd, 44), TRUE); y += S(hwnd, 46);
            MoveWindow(sub, px, y, rc.right - 2 * px - lw, S(hwnd, 44), TRUE); y += S(hwnd, 60);
            MoveWindow(b_vbi, px, y, S(hwnd, 260), S(hwnd, 44), TRUE);
            MoveWindow(b_pk, px + S(hwnd, 272), y, S(hwnd, 280), S(hwnd, 44), TRUE);
            MoveWindow(b_dev, px + S(hwnd, 564), y, S(hwnd, 340), S(hwnd, 44), TRUE);
            MoveWindow(b_cap, px + S(hwnd, 916), y, S(hwnd, 250), S(hwnd, 44), TRUE);
            y += S(hwnd, 50);
            MoveWindow(b_view, px, y, S(hwnd, 420), S(hwnd, 30), TRUE);
            y += S(hwnd, 36);
            MoveWindow(hint, px, y, std::min((int)rc.right - 2 * px, S(hwnd, 760)), S(hwnd, 40), TRUE); y += S(hwnd, 56);
            int cw = (rc.right - 2 * px - S(hwnd, 12)) / 2, ch = rc.bottom - y - S(hwnd, 40);
            card1 = {px, y, px + cw, y + ch}; card2 = {px + cw + S(hwnd, 12), y, rc.right - px, y + ch};
            int cp = S(hwnd, 16);
            MoveWindow(rec_h, card1.left + cp, card1.top + cp, cw - 2 * cp, S(hwnd, 26), TRUE);
            int ry = card1.top + cp + S(hwnd, 34);
            for (HWND h : recent_btns) { MoveWindow(h, card1.left + cp, ry, cw - 2 * cp, S(hwnd, 34), TRUE); ry += S(hwnd, 40); }
            MoveWindow(svc_h, card2.left + cp, card2.top + cp, cw - 2 * cp, S(hwnd, 26), TRUE);
            MoveWindow(formats, card2.left + cp, card2.top + cp + S(hwnd, 34) + (int)services.size() * S(hwnd, 26) + S(hwnd, 10), cw - 2 * cp, S(hwnd, 40), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            fill(dc, rc, BG); fill(dc, card1, PANEL); fill(dc, card2, PANEL);
            int cp = S(hwnd, 16), y = card2.top + cp + S(hwnd, 34);
            for (auto &s : services) {
                RECT sq{card2.left + cp, y + S(hwnd, 5), card2.left + cp + S(hwnd, 12), y + S(hwnd, 17)};
                fill(dc, sq, service_colour(s.first));
                text_out(dc, card2.left + cp + S(hwnd, 22), y, s.second, FG, font(hwnd));
                y += S(hwnd, 26);
            }
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 1) act.open_vbi(); else if (id == 2) act.pick_device(); else if (id == 3) act.open_packets(); else if (id == 4) act.capture(); else if (id == 5) act.view_lines();
            else if (id >= 100 && id - 100 < (int)recent.size()) { Json r = recent[id - 100]; act.open_recent(r); }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ "What is in" window
struct RecWin : Window {
    ResultPtr R;
    std::vector<Service> svc;
    HWND h_title, h_sub, h_head, map_h, res_h, b_report, b_json, b_export, b_close, b_lines; std::vector<HWND> rbtn, rnote;
    RECT mapr{}, resr{};
    struct Row { int tv; std::string kind, text; double read; };
    std::vector<Row> rows;
    void build_rows()
    {
        const VbiGeometry &g = R->geom;
        int per = std::max(1, g.linesPerFrame / 2);
        for (int f = 1; f <= 2; f++)
            for (int i = 0; i < per; i++) {
                int l = (f - 1) * per + i, line, fld;
                lineOf(g, l, line, fld);
                Row r{tvLineOf(g, l), "empty", "", 0};
                for (auto &li : R->lines)
                    if (li.lineNo == line && li.field == fld) {
                        std::string k = li.kind;
                        r.read = li.detectRate;
                        if (k == "StarSight") { r.kind = "StarSight"; r.text = "StarSight programme guide"; }
                        else if (k == "Captions") { r.kind = "CC captions"; r.text = "CC captions"; }
                        else if (k.rfind("XDS", 0) == 0) { r.kind = "CC XDS"; r.text = "XDS / CC3"; }
                        else if (k.rfind("Silent Radio", 0) == 0) { r.kind = "Silent Radio"; r.text = k; }
                        else if (k == "NABTS") { r.kind = "NABTS"; r.text = "NABTS"; }
                        else if (k == "WST teletext") { r.kind = "WST teletext"; r.text = "WST teletext (525 lines)"; }
                        else if (k.rfind("Encrypted datacast", 0) == 0) { r.kind = "encrypted datacast"; r.text = "encrypted datacast, 5.73 Mbit/s (framing 2D)"; }
                        else if (k.rfind("AMOL", 0) == 0) { r.kind = "AMOL"; r.text = "Nielsen AMOL I"; }
                        else if (k.rfind("Test", 0) == 0) { r.kind = "test"; r.text = "test signal (VITS)"; }
                        else { r.kind = "data"; r.text = k.empty() ? "data at the caption rate" : k; }
                    }
                rows.push_back(r);
            }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            build_rows();
            svc = services_of(*R);
            h_title = label(hwnd, file_name(*R)); SendMessageW(h_title, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            double secs = R->geom.frames / frame_rate(*R);
            h_sub = label(hwnd, fmt("%s \xC2\xB7 %lld frames \xC2\xB7 %.0f min %02.0f s \xC2\xB7 %s \xC2\xB7 %.0f s", formatName(R->geom.format),
                                    (long long)R->geom.frames, floor(secs / 60), fmod(secs, 60), R->slicerName.c_str(), R->totalSeconds));
            set_label_colour(h_sub, MUTED);
            // what is on the recording in one look: colour system, ТВ-информ yes / no, teletext
            {
                std::string hl;
                for (auto &x : headline(*R)) hl += (hl.empty() ? "" : "   \xC2\xB7   ") + x;
                h_head = label(hwnd, hl);
                SendMessageW(h_head, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
                set_label_colour(h_head, ACCENT2);
            }
            map_h = label(hwnd, "VBI lines"); SetPropW(map_h, L"card", (HANDLE)1); SendMessageW(map_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            res_h = label(hwnd, "Results"); SetPropW(res_h, L"card", (HANDLE)1); SendMessageW(res_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            int i = 0;
            for (auto &s : svc) {
                rbtn.push_back(button(hwnd, 100 + i, "\xE2\x96\xB6  " + s.name, BTN_BIG));
                HWND n = label(hwnd, s.where + "  \xE2\x80\x94 " + s.note); SetPropW(n, L"card", (HANDLE)1); set_label_colour(n, MUTED);
                rnote.push_back(n); i++;
            }
            if (rbtn.empty()) { HWND n = label(hwnd, "Nothing the program can decode\nwas found in this recording."); SetPropW(n, L"card", (HANDLE)1); set_label_colour(n, MUTED); rnote.push_back(n); }
            b_report = button(hwnd, 50, "Report (text)");
            b_json = button(hwnd, 51, "Export StarSight JSON\xE2\x80\xA6");
            b_export = button(hwnd, 52, "Export everything decoded\xE2\x80\xA6", BTN_ACCENT);
            EnableWindow(b_json, !R->db.empty());
            b_close = button(hwnd, IDCANCEL, "Close");
            b_lines = button(hwnd, 53, "\xF0\x9F\x93\x88  View VBI lines");
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 18), y = p;
            MoveWindow(h_title, p, y, rc.right - 2 * p, S(hwnd, 26), TRUE); y += S(hwnd, 28);
            MoveWindow(h_sub, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 24);
            MoveWindow(h_head, p, y, rc.right - 2 * p, S(hwnd, 40), TRUE); y += S(hwnd, 46);
            int rw = S(hwnd, 300);
            mapr = {p, y, rc.right - p - rw - S(hwnd, 12), rc.bottom - p};
            resr = {rc.right - p - rw, y, rc.right - p, rc.bottom - p};
            int cp = S(hwnd, 12);
            MoveWindow(map_h, mapr.left + cp, mapr.top + cp, S(hwnd, 200), S(hwnd, 24), TRUE);
            MoveWindow(b_lines, mapr.right - cp - S(hwnd, 170), mapr.top + cp - S(hwnd, 2), S(hwnd, 170), S(hwnd, 28), TRUE);
            MoveWindow(res_h, resr.left + cp, resr.top + cp, rw - 2 * cp, S(hwnd, 24), TRUE);
            int ry = resr.top + cp + S(hwnd, 32);
            for (size_t i = 0; i < rnote.size(); i++) {
                if (i < rbtn.size()) { MoveWindow(rbtn[i], resr.left + cp, ry, rw - 2 * cp, S(hwnd, 38), TRUE); ry += S(hwnd, 42); }
                MoveWindow(rnote[i], resr.left + cp, ry, rw - 2 * cp, S(hwnd, 40), TRUE); ry += S(hwnd, 46);
            }
            // report, export and close sit at the bottom of the column
            int by = resr.bottom - cp - S(hwnd, 32);
            MoveWindow(b_close, resr.left + cp, by, rw - 2 * cp, S(hwnd, 32), TRUE); by -= S(hwnd, 40);
            MoveWindow(b_json, resr.left + cp, by, rw - 2 * cp, S(hwnd, 32), TRUE); by -= S(hwnd, 40);
            MoveWindow(b_report, resr.left + cp, by, rw - 2 * cp, S(hwnd, 32), TRUE); by -= S(hwnd, 40);
            MoveWindow(b_export, resr.left + cp, by, rw - 2 * cp, S(hwnd, 32), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            fill(mem, rc, BG); fill(mem, mapr, PANEL); fill(mem, resr, PANEL);
            // two columns: field 1 on the left, field 2 on the right
            std::vector<const Row *> col[2];
            int f2 = R->geom.pal ? 313 : 263;
            for (auto &r : rows) col[r.tv >= f2 ? 1 : 0].push_back(&r);
            int ncol = col[1].empty() ? 1 : 2;
            int top = mapr.top + S(hwnd, 44), bottom = mapr.bottom - S(hwnd, 12);
            size_t nmax = std::max(col[0].size(), col[1].size());
            int h = std::min(S(hwnd, 26), std::max(S(hwnd, 12), (int)((bottom - top) / std::max<size_t>(1, nmax))));
            int gap = S(hwnd, 12), cwid = (mapr.right - mapr.left - S(hwnd, 12) - gap * (ncol - 1)) / ncol;
            HFONT fnt = font(hwnd, h >= S(hwnd, 20) ? 9 : 8);
            for (int c = 0; c < ncol; c++) {
                int left = mapr.left + S(hwnd, 12) + c * (cwid + gap);
                int x0 = left + S(hwnd, 96), Wd = left + cwid, y = top;
                for (auto *r : col[c]) {
                    if (y + h > bottom + 1) break;
                    std::string lab = r->tv >= f2 ? fmt("%d (f2: %d)", r->tv, r->tv - f2) : fmt("line %d", r->tv);
                    text_out(mem, left, y + (h - S(hwnd, 15)) / 2, lab, MUTED, fnt);
                    COLORREF cc = service_colour(r->kind);
                    RECT bx{x0 + 1, y + 1, Wd - 2, y + h - 1};
                    fill(mem, bx, cc);
                    if (r->kind != "empty") {
                        std::string txt = r->text;
                        if (r->read > 0) txt += fmt("  \xC2\xB7 signal %.0f%%", r->read * 100);
                        bool darkt = (GetRValue(cc) * 3 + GetGValue(cc) * 6 + GetBValue(cc)) > 1100;
                        HRGN clip = CreateRectRgn(bx.left, bx.top, bx.right, bx.bottom); SelectClipRgn(mem, clip);
                        text_out(mem, bx.left + S(hwnd, 6), y + (h - S(hwnd, 15)) / 2, txt, darkt ? RGB(0x11, 0x11, 0x11) : FG, fnt);
                        SelectClipRgn(mem, nullptr); DeleteObject(clip);
                    }
                    y += h;
                }
            }
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == IDCANCEL) DestroyWindow(hwnd);
            else if (id == 50) open_service(hwnd, R, Service{"Report", "", "", "", ServiceWindow::Report, ""});
            else if (id == 53) lines_window(hwnd, R);
            else if (id == 51) {
                std::string p = save_file(hwnd, "Export StarSight JSON", stem_path(file_name(*R)) + ".json", "json");
                std::string err;
                if (!p.empty() && !ssExportJson(p, R->db, R.get(), err)) ask(hwnd, "Export", err, MB_ICONERROR);
            } else if (id == 52) {
                // one folder next to the recording (or where the user picks) with a file per service
                std::string d = pick_folder(hwnd, "Folder for the export", dirname(R->path));
                if (d.empty()) return 0;
                std::string out = path_join(d, basename(stem_path(R->path)) + "_decoded");
                auto res = std::make_shared<std::string>();
                ResultPtr r = R;
                run_task(hwnd, "Export", [r, out, res](Progress &pr) { *res = exportResult(*r, out, pr); pr.log(*res); },
                         [out](bool ok) { if (ok) open_path(out); });
            } else if (id >= 100 && id - 100 < (int)svc.size()) open_service(hwnd, R, svc[id - 100]);
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

void recording_window(HWND owner, const ResultPtr &r)
{
    auto *w = new RecWin; w->R = r;
    w->create(L"TRRec", "What is in " + file_name(*r), WS_OVERLAPPEDWINDOW, S(owner, 1040), S(owner, 680), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}

// ================================================================ main window
namespace {
enum {
    M_OPEN_VBI = 1000, M_OPEN_PK = 1010, M_CAPTURE = 1011, M_VIEW_LINES = 1012, M_EXIT = 1001, M_DEVICE, M_UNVERIFIED, M_ABOUT, M_RECENT0 = 1100, M_FORMAT0 = 1200,
};

struct App : Window {
    HMENU menu = nullptr, rmenu = nullptr, fmenu = nullptr;
    HWND start = nullptr;
    VbiFormat format = VbiFormat::Unknown;
    bool unverified = false;

    void build_menu()
    {
        menu = CreateMenu();
        HMENU fm = CreatePopupMenu();
        AppendMenuW(fm, MF_STRING, M_OPEN_VBI, L"Open .vbi recording\x2026");
        AppendMenuW(fm, MF_STRING, M_OPEN_PK, L"Open teletext / NABTS packets (.t42, .t34, .t33)\x2026");
        AppendMenuW(fm, MF_STRING, M_VIEW_LINES, L"View VBI lines (no decoding), set line types by hand\x2026");
        AppendMenuW(fm, MF_STRING, M_CAPTURE, L"Capture card (live)\x2026");
        rmenu = CreatePopupMenu();
        AppendMenuW(fm, MF_POPUP, (UINT_PTR)rmenu, L"Recent");
        AppendMenuW(fm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fm, MF_STRING, M_EXIT, L"Exit");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)fm, L"File");
        HMENU tm = CreatePopupMenu();
        AppendMenuW(tm, MF_STRING, M_DEVICE, L"Decode on: graphics card or processor\x2026");
        fmenu = CreatePopupMenu();
        AppendMenuW(fmenu, MF_STRING, M_FORMAT0, L"Recognise automatically");
        for (int i = 1; i < (int)VbiFormat::Count; i++) AppendMenuW(fmenu, MF_STRING, M_FORMAT0 + i, W(formatName((VbiFormat)i)).c_str());
        AppendMenuW(tm, MF_POPUP, (UINT_PTR)fmenu, L"Recording format (capture chip)");
        AppendMenuW(tm, MF_STRING, M_UNVERIFIED, L"Keep StarSight schedules from packets that fail the CRC");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)tm, L"Tools");
        HMENU hm = CreatePopupMenu();
        AppendMenuW(hm, MF_STRING, M_ABOUT, L"About Teletext Rescue");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)hm, L"Help");
        SetMenu(hwnd, menu);
        update_menu();
    }
    void update_menu()
    {
        CheckMenuRadioItem(fmenu, M_FORMAT0, M_FORMAT0 + (int)VbiFormat::Count - 1, M_FORMAT0 + (int)format, MF_BYCOMMAND);
        CheckMenuItem(menu, M_UNVERIFIED, unverified ? MF_CHECKED : MF_UNCHECKED);
    }
    void fill_recent()
    {
        while (GetMenuItemCount(rmenu) > 0) DeleteMenu(rmenu, 0, MF_BYPOSITION);
        auto rec = recent_list();
        if (rec.empty()) AppendMenuW(rmenu, MF_STRING | MF_GRAYED, 0, L"(empty)");
        for (size_t i = 0; i < rec.size(); i++) {
            std::string p = rec[i].get_str("path");
            AppendMenuW(rmenu, MF_STRING, M_RECENT0 + i, W(basename(p) + "   \xE2\x80\x94   " + dirname(p)).c_str());
        }
    }
    void show_start()
    {
        if (start) DestroyWindow(start);
        StartActions a;
        a.open_vbi = [this] { menu_vbi(); };
        a.open_packets = [this] { menu_packets(); };
        a.pick_device = [this] { pick_device(true); };
        a.capture = [this] { capture_window(hwnd); };
        a.view_lines = [this] { view_lines(); };
        a.open_recent = [this](const Json &r) { open_vbi(r.get_str("path")); };
        auto *s = new StartScreen; s->act = a;
        Window::register_class(L"TRStart", brBG);
        start = CreateWindowExW(0, L"TRStart", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd, nullptr, inst(), s);
        RECT rc; GetClientRect(hwnd, &rc);
        MoveWindow(start, 0, 0, rc.right, rc.bottom, TRUE);
    }
    void menu_packets()
    {
        std::string f = open_file(hwnd, "Packets", {{"Teletext / NABTS packets", "*.t42;*.t34;*.t33"}, {"All files", "*.*"}});
        if (!f.empty()) open_vbi(f);
    }
    void menu_vbi()
    {
        std::string f = open_file(hwnd, "VBI recording", {{"VBI recording or DVB stream", "*.vbi;*.tbc;*.flac;*.t42;*.t34;*.t33;*.ts;*.mts;*.m2ts"}, {"All files", "*.*"}});
        if (!f.empty()) open_vbi(f);
    }
    // what decodes the recording: the graphics card (OpenCL) or the processor; the choice can be remembered
    // returns 1 graphics card, 0 processor, -1 cancelled
    int pick_device(bool force)
    {
        Json st = load_state();
        int saved = st["device"].is_null() ? -2 : st["device"].integer();
        bool ask_it = st.get_bool("device_ask", true);
        std::string name; uint64_t mem = 0;
        bool have = gpuDeviceInfo(name, mem);
        if (!force && (!ask_it || !have)) return have && saved != -1 ? 1 : 0;
        std::vector<std::string> opts;
        if (have) opts.push_back("Graphics card: " + name + fmt("  (%llu MB)", (unsigned long long)mem) + "  \xE2\x80\x94 fastest");
        opts.push_back("Processor (CPU) \xE2\x80\x94 slower, works on any computer");
        int def = have && saved != -1 ? 0 : (int)opts.size() - 1;
        bool remember = false;
        int r = choose(hwnd, "Decode on", have ? "What should decode the recording?"
                                                : "No graphics card with OpenCL was found \xE2\x80\x94 the recording is decoded on the processor.",
                       opts, def, "Remember and do not ask again", &remember);
        if (r < 0) return -1;
        int dev = have && r == 0 ? 0 : -1;
        st["device"] = (double)dev; st["device_ask"] = !remember; save_state(st);
        return dev == 0 ? 1 : 0;
    }
    // the raw lines of a recording, nothing decoded: line types can be set by hand there and the
    // recording decoded with them
    void view_lines()
    {
        std::string f = open_file(hwnd, "VBI recording", {{"VBI recording", "*.vbi;*.tbc;*.flac"}, {"All files", "*.*"}});
        if (f.empty()) return;
        auto res = std::make_shared<DecodeResult>();
        VbiFormat fmtv = format;
        run_task(hwnd, "Opening " + basename(f), [f, res, fmtv](Progress &pr) {
            DecodeOptions opt;
            opt.probeOnly = true; opt.force = fmtv; opt.cancel = &pr.cancel;
            std::string last;
            opt.progress = [&](double, const std::string &s) { if (s != last) { last = s; if (pr.on_step) pr.on_step(s); } };
            opt.log = [&](const std::string &s) { pr.log(s); };
            std::string err;
            if (!decodeFile(f, opt, *res, err)) {
                if (pr.cancel) throw Cancelled();
                throw std::runtime_error(err);
            }
        }, [this, res](bool ok) { if (ok) lines_window(hwnd, res); });
    }
    void open_vbi(const std::string &f, const std::map<int, LineForce> *force = nullptr, VbiFormat fmtForce = VbiFormat::Unknown)
    {
        if (!exists(f)) { ask(hwnd, "Could not open", "The file does not exist:\n" + f, MB_ICONERROR); return; }
        recent_add(f, "vbi");
        // as in the original program: what was decoded is kept in "<recording>_vbi"; a recording
        // decoded before opens from there at once, or is decoded (and a FLAC unpacked) again
        bool again = false, redo = false;
        if (force) redo = true;   // line types set by hand: always decoded again, with the viewer's format
        else if (resultsCached(f)) {
            std::vector<std::string> opts = {"Open the saved results (no decoding)", "Decode the recording again"};
            if (is_flac(f)) opts.push_back("Unpack the FLAC and decode again");
            int c = choose(hwnd, "Already decoded",
                           basename(f) + " was already decoded.\nResults: " + legacyReportDir(f) + "\n\nDecode it again?", opts, 0);
            if (c < 0) return;
            if (c == 0) {
                auto res = std::make_shared<DecodeResult>();
                run_task(hwnd, "Opening " + basename(f), [f, res](Progress &pr) {
                    pr.step("Opening the saved results");
                    std::string err;
                    if (!loadResults(f, *res, err)) throw std::runtime_error(err);
                    // files and texts (the «Бизнес-Текст» files put together and written), telling which
                    prepareFinds(*res, [&](const std::string &s) { pr.step(s); });
                }, [this, res](bool ok) { if (ok) show_results(res); });
                return;
            }
            redo = true;
            again = c == 2;
        } else if (!force && is_flac(f) && flac_cached(f)) {
            // unpacked before but never decoded to the end
            int c = choose(hwnd, "FLAC recording", "This recording was already unpacked:\n" + flac_cache_path(f),
                           {"Use the unpacked recording", "Unpack it again"}, 0);
            if (c < 0) return;
            again = c == 1;
        }
        int dev = pick_device(false);
        if (dev < 0) return;
        auto res = std::make_shared<DecodeResult>();
        bool gpu = dev == 1;
        VbiFormat fmtv = force && fmtForce != VbiFormat::Unknown ? fmtForce : format; bool unv = unverified;
        std::map<int, LineForce> lf = force ? *force : std::map<int, LineForce>{};
        run_task(hwnd, "Opening " + basename(f), [f, res, gpu, fmtv, unv, again, redo, lf](Progress &pr) {
            DecodeOptions opt;
            opt.gpu = gpu; opt.force = fmtv; opt.keepUnverified = unv; opt.cancel = &pr.cancel; opt.reunpack = again; opt.again = redo;
            opt.lineForce = lf;
            std::string last;
            opt.progress = [&](double, const std::string &s) { if (s != last) { last = s; if (pr.on_step) pr.on_step(s); } };
            opt.log = [&](const std::string &s) { pr.log(s); };
            opt.count = [&](int64_t a, int64_t b) { if (pr.on_progress) pr.on_progress(a, b, ""); };
            std::string err;
            if (!decodeFile(f, opt, *res, err)) {
                if (pr.cancel) throw Cancelled();
                throw std::runtime_error(err == "no data lines found" ? "Nothing the program can decode was found in this recording." : err);
            }
            prepareFinds(*res, [&](const std::string &s) { pr.step(s); pr.log(s); });
        }, [this, res](bool ok) { if (ok) show_results(res); });
    }
    void show_results(const ResultPtr &r)
    {
        for (auto &s : services_of(*r))
            if (s.window != ServiceWindow::Sign || !r->silentRadio.empty()) open_service(hwnd, r, s);
        recording_window(hwnd, r);
        // recordings started from the start screen show up in "Recent"
        show_start();
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        LRESULT res;
        if (dark_menubar(hwnd, m, w, l, res)) return res;
        switch (m) {
        case WM_CREATE:
            g_main = hwnd; dark_window(hwnd);
            open_recording_hook() = [this](const std::string &f) { open_vbi(f); };
            decode_lines_hook() = [this](const std::string &f, VbiFormat fm, const std::map<int, LineForce> &lf) { open_vbi(f, &lf, fm); };
            build_menu(); show_start();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        case WM_SIZE: if (start) { RECT rc; GetClientRect(hwnd, &rc); MoveWindow(start, 0, 0, rc.right, rc.bottom, TRUE); } return 0;
        case WM_GETMINMAXINFO: { auto *mm = (MINMAXINFO *)l; mm->ptMinTrackSize = {S(hwnd, 900), S(hwnd, 640)}; return 0; }
        case WM_DPICHANGED: { RECT *r = (RECT *)l; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER); return 0; }
        case WM_APP_CALL: { auto *f = (std::function<void()> *)l; (*f)(); delete f; return 0; }
        case WM_DROPFILES: {
            wchar_t buf[MAX_PATH]; DragQueryFileW((HDROP)w, 0, buf, MAX_PATH); DragFinish((HDROP)w);
            open_vbi(N(buf));
            return 0;
        }
        case WM_INITMENUPOPUP: if ((HMENU)w == rmenu) fill_recent(); return 0;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id >= M_RECENT0 && id < M_RECENT0 + 20) { auto rec = recent_list(); if (id - M_RECENT0 < (int)rec.size()) open_vbi(rec[id - M_RECENT0].get_str("path")); return 0; }
            if (id >= M_FORMAT0 && id < M_FORMAT0 + (int)VbiFormat::Count) { format = (VbiFormat)(id - M_FORMAT0); update_menu(); return 0; }
            switch (id) {
            case M_OPEN_VBI: menu_vbi(); break;
            case M_OPEN_PK: menu_packets(); break;
            case M_CAPTURE: capture_window(hwnd); break;
            case M_VIEW_LINES: view_lines(); break;
            case M_DEVICE: pick_device(true); break;
            case M_UNVERIFIED: unverified = !unverified; update_menu(); break;
            case M_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            case M_ABOUT:
                ask(hwnd, "About", "Teletext Rescue " TR_VERSION "\n\nRecovers data hidden in the vertical blanking interval of VBI recordings:\n"
                                   "WST teletext (625 and 525 lines, DVB), Japanese teletext (JTES), NABTS / NAPLPS,\n"
                                   "StarSight and Gemstar GUIDE Plus+ programme guides, EIA-608 captions and XDS, AMOL,\n"
                                   "Silent Radio LED sign feed, TV-Inform, Interactive Network and other datacasts.\n\n"
                                   "Copyright (c) 2026 AssunaYuuki. GNU General Public License v3.0.", MB_ICONINFORMATION);
                break;
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return Window::proc(m, w, l);
    }
};
}
}  // namespace ui

using namespace ui;

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show)
{
    ui::init(hi);
    App app;
    app.create(L"TRMain", "Teletext Rescue " TR_VERSION, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 1320, 920);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = std::min(S(app.hwnd, 1320), (int)(wa.right - wa.left)), h = std::min(S(app.hwnd, 920), (int)(wa.bottom - wa.top));
    SetWindowPos(app.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER);
    ShowWindow(app.hwnd, show);
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::string a = narrow(argv[i]);
        if (a == "--capture") { capture_window(app.hwnd, true); break; }   // the capture window, started
        if (a == "--test-file" && i + 1 < argc) { capture_window(app.hwnd, false, narrow(argv[i + 1])); break; }   // a .vbi played to it
        if (a.rfind("--", 0) != 0) { app.open_vbi(a); break; }
    }
    LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        HWND top = GetAncestor(msg.hwnd, GA_ROOT);
        auto pt = ui::pretranslators().find(top);
        if (pt != ui::pretranslators().end() && pt->second(msg)) continue;
        if (top && top != app.hwnd && IsDialogMessageW(top, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
