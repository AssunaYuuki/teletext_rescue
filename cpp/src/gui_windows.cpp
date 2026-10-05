// Стартовый экран (недавние файлы, табло «время и температура»), окно «Что в записи»,
// проигрыватель табло Silent Radio.
#include "gui_app.h"
#include "vbi_auto.h"
#include "silent_radio.h"
#include <windowsx.h>
#include <winhttp.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace ui {
// ================================================================ табло «время и температура»
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

std::string http_get(const wchar_t *host, const wchar_t *path) {
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
void weather_thread() {
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
int icon_of(std::string c) {
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
std::vector<int> tcols(const std::string &s) {
    std::vector<int> out;
    for (char32_t ch : to_u32(s)) {
        std::vector<int> g = ch == 0xB0 ? std::vector<int>{0x06, 0x09, 0x09, 0x06} : sr_glyph(ch);
        out.insert(out.end(), g.begin(), g.end()); out.push_back(0);
    }
    if (!out.empty()) out.pop_back();
    return out;
}
void put_line(std::vector<int> &buf, const std::string &s, int row, int x0 = -9999, int right = -9999) {
    auto c = tcols(s);
    int x = x0 == -9999 ? (LCOLS - (int)c.size()) / 2 : x0;
    if (right != -9999) x = right - (int)c.size();
    for (size_t k = 0; k < c.size(); k++) if (x + (int)k >= 0 && x + (int)k < LCOLS) buf[x + k] |= c[k] << row;
}
}

struct LedBoard : Window {
    int dot = 4; std::vector<int> shown = std::vector<int>(LCOLS, 0), prev = std::vector<int>(LCOLS, 0);
    int page = 0; DWORD t_page = GetTickCount();
    int pages() { std::lock_guard<std::mutex> lk(wx_mu); return wx ? 4 : 2; }
    std::vector<int> frame(int pg, bool colon) {
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
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
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

// ================================================================ стартовый экран
struct StartScreen : Window {
    StartActions act;
    HWND title, sub, b_vbi, b_stream, b_proj, hint, rec_h, svc_h, formats;
    std::vector<HWND> recent_btns; std::vector<Json> recent;
    std::vector<std::pair<std::string, std::string>> services = {
        {"teletext", "WST teletext \xE2\x80\x94 625-line (PAL/SECAM) and 525-line (NTSC)"}, {"NABTS", "NABTS / NAPLPS \xE2\x80\x94 CBS ExtraVision, NBC Teletext, animated pages"},
        {"Silent Radio", "Silent Radio \xE2\x80\x94 LED news sign service (line 21)"}, {"encrypted datacast", "Encrypted datacast \xE2\x80\x94 packets, addresses, schedule"},
        {"CC", "CC captions (line 21)"}, {"AMOL", "AMOL (Nielsen programme ID and time), VITC timecode"}, {"test", "Test signals (VITS), VPS / WSS detection"}};
    HWND led = nullptr;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            title = label(hwnd, "Teletext Rescue"); SendMessageW(title, WM_SETFONT, (WPARAM)font(hwnd, 22, true, L"Segoe UI Semibold"), TRUE); set_label_colour(title, ACCENT);
            sub = label(hwnd, "Teletext, NABTS, Silent Radio, captions and data\nhidden in the vertical blanking interval"); set_label_colour(sub, MUTED);
            b_vbi = button(hwnd, 1, "\xF0\x9F\x93\xBC  Open .vbi recording\xE2\x80\xA6", BTN_ACCENT);
            b_stream = button(hwnd, 2, "Open stream .t42 / .t34 / .t33 / .ts\xE2\x80\xA6", BTN_BIG);
            b_proj = button(hwnd, 3, "Open project\xE2\x80\xA6", BTN_BIG);
            hint = label(hwnd, "A recording is examined automatically: the capture chip and format (bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx, 4fsc .tbc) and every VBI line are identified and everything readable is decoded and opened.");
            set_label_colour(hint, MUTED);
            rec_h = label(hwnd, "Recent"); SetPropW(rec_h, L"card", (HANDLE)1); SendMessageW(rec_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            svc_h = label(hwnd, "What it reads"); SetPropW(svc_h, L"card", (HANDLE)1); SendMessageW(svc_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            formats = label(hwnd, "Chips: bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx (PAL/SECAM/NTSC); .tbc (4fsc, 16 bit); streams .t42, .t34, .t33, DVB .ts");
            SetPropW(formats, L"card", (HANDLE)1); set_label_colour(formats, MUTED);
            recent = recent_list();
            for (size_t i = 0; i < recent.size() && i < 8; i++) {
                std::string p = recent[i].get_str("path"), k = recent[i].get_str("kind");
                std::string ic = k == "vbi" ? "\xF0\x9F\x93\xBC" : k == "project" ? "\xF0\x9F\x93\x84" : "\xE3\x80\xB0";
                recent_btns.push_back(button(hwnd, 100 + (int)i, ic + "  " + basename(p) + "     " + dirname(p) + "    " + recent[i].get_str("time"), BTN_LINK));
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
            MoveWindow(b_stream, px + S(hwnd, 272), y, S(hwnd, 320), S(hwnd, 44), TRUE);
            MoveWindow(b_proj, px + S(hwnd, 604), y, S(hwnd, 160), S(hwnd, 44), TRUE);
            y += S(hwnd, 54);
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
            if (id == 1) act.open_vbi(); else if (id == 2) act.open_stream(); else if (id == 3) act.open_project();
            else if (id >= 100 && id - 100 < (int)recent.size()) { Json r = recent[id - 100]; act.open_recent(r); }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
    RECT card1{}, card2{};
};

HWND start_screen(HWND parent, const StartActions &a) {
    auto *s = new StartScreen; s->act = a;
    Window::register_class(L"TRStart", brBG);
    return CreateWindowExW(0, L"TRStart", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, nullptr, inst(), s);
}

// ================================================================ окно «Что в записи»
struct RecWin : Window {
    Json R; std::function<void(const Json &)> opener;
    HWND h_title, h_sub, map_h, res_h, vits, b_report, b_close; std::vector<HWND> rbtn, rnote;
    RECT mapr{}, resr{}; int scroll = 0;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            const Json &Pp = R["probe"];
            h_title = label(hwnd, basename(R.get_str("file"))); SendMessageW(h_title, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            int units = Pp["units"].integer(); std::string unit = Pp.get_str("unit", "field");
            double secs = units / (unit == "field" ? 59.94 : (R.get_str("format").find("ntsc") != std::string::npos ? 29.97 : 25.0));
            h_sub = label(hwnd, fmt("%s \xC2\xB7 %d %ss \xC2\xB7 %.0f min %02.0f s", R.get_str("format").c_str(), units, unit.c_str(), floor(secs / 60), fmod(secs, 60)));
            set_label_colour(h_sub, MUTED);
            map_h = label(hwnd, "VBI lines"); SetPropW(map_h, L"card", (HANDLE)1); SendMessageW(map_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            res_h = label(hwnd, "Results"); SetPropW(res_h, L"card", (HANDLE)1); SendMessageW(res_h, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE);
            std::string vt; for (auto &t : R["vits_text"].a) vt += t.str() + "\n";
            vits = label(hwnd, vt); SetPropW(vits, L"card", (HANDLE)1); set_label_colour(vits, MUTED);
            int i = 0;
            for (auto &r : R["results"].a) {
                rbtn.push_back(button(hwnd, 100 + i, "\xE2\x96\xB6  " + r.get_str("service"), BTN_BIG));
                std::string p = r.get_str("path"); while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
                HWND n = label(hwnd, basename(p) + (r.has("note") ? "  \xE2\x80\x94 " + r.get_str("note") : "")); SetPropW(n, L"card", (HANDLE)1); set_label_colour(n, MUTED);
                rnote.push_back(n); i++;
            }
            if (rbtn.empty()) { HWND n = label(hwnd, "Nothing the program can decode\nwas found in this recording."); SetPropW(n, L"card", (HANDLE)1); set_label_colour(n, MUTED); rnote.push_back(n); }
            b_report = button(hwnd, 50, "Report (text)"); b_close = button(hwnd, IDCANCEL, "Close");
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 18), y = p;
            MoveWindow(h_title, p, y, rc.right - 2 * p, S(hwnd, 26), TRUE); y += S(hwnd, 28);
            MoveWindow(h_sub, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 32);
            int rw = S(hwnd, 300);
            mapr = {p, y, rc.right - p - rw - S(hwnd, 12), rc.bottom - p};
            resr = {rc.right - p - rw, y, rc.right - p, rc.bottom - p};
            int cp = S(hwnd, 12);
            MoveWindow(map_h, mapr.left + cp, mapr.top + cp, S(hwnd, 200), S(hwnd, 24), TRUE);
            int vh = S(hwnd, 18) * std::max<int>(0, (int)R["vits_text"].size());
            MoveWindow(vits, mapr.left + cp, mapr.bottom - cp - vh, mapr.right - mapr.left - 2 * cp, vh, TRUE);
            MoveWindow(res_h, resr.left + cp, resr.top + cp, rw - 2 * cp, S(hwnd, 24), TRUE);
            int ry = resr.top + cp + S(hwnd, 32);
            for (size_t i = 0; i < rnote.size(); i++) {
                if (i < rbtn.size()) { MoveWindow(rbtn[i], resr.left + cp, ry, rw - 2 * cp, S(hwnd, 38), TRUE); ry += S(hwnd, 42); }
                MoveWindow(rnote[i], resr.left + cp, ry, rw - 2 * cp, S(hwnd, 40), TRUE); ry += S(hwnd, 46);
            }
            MoveWindow(b_report, resr.left + cp, ry + S(hwnd, 8), rw - 2 * cp, S(hwnd, 32), TRUE);
            MoveWindow(b_close, resr.left + cp, resr.bottom - cp - S(hwnd, 32), rw - 2 * cp, S(hwnd, 32), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc); fill(dc, rc, BG); fill(dc, mapr, PANEL); fill(dc, resr, PANEL);
            // две колонки: строки первого поля слева, второго (номера ≥ 263) справа; высота ряда подгоняется
            std::map<int, std::vector<const Json *>> rows;
            for (auto &L : R["probe"]["lines"].a) rows[L["tv_line"].integer()].push_back(&L);
            std::vector<std::pair<int, std::vector<const Json *>>> col[2];
            for (auto &kv : rows) col[kv.first >= 263 ? 1 : 0].push_back(kv);
            int ncol = col[1].empty() ? 1 : 2;
            int top = mapr.top + S(hwnd, 44), bottom = mapr.bottom - S(hwnd, 12) - S(hwnd, 18) * (int)R["vits_text"].size();
            size_t nmax = std::max(col[0].size(), col[1].size());
            int h = std::min(S(hwnd, 26), std::max(S(hwnd, 12), (int)((bottom - top) / std::max<size_t>(1, nmax))));
            int gap = S(hwnd, 12), cwid = (mapr.right - mapr.left - S(hwnd, 12) - gap * (ncol - 1)) / ncol;
            HFONT fnt = font(hwnd, h >= S(hwnd, 20) ? 9 : 8);
            for (int c = 0; c < ncol; c++) {
                int left = mapr.left + S(hwnd, 12) + c * (cwid + gap);
                int x0 = left + S(hwnd, 96), W = left + cwid, y = top;
                for (auto &kv : col[c]) {
                    if (y + h > bottom + 1) break;
                    auto Ls = kv.second;
                    std::stable_sort(Ls.begin(), Ls.end(), [](const Json *a, const Json *b) {
                        bool na = (*a)["parity"].is_null(), nb = (*b)["parity"].is_null();
                        if (na != nb) return !na; return (*a)["parity"].integer() < (*b)["parity"].integer(); });
                    // второе поле: сквозной номер кадра и номер строки внутри поля
                    std::string fmtname = R.get_str("format"); int tl = kv.first; std::string lab = fmt("line %d", tl);
                    const VbiFormat *vf = vbi_format(fmtname);
                    bool pal = vf && !vf->ntsc, by_field = vf && vf->field_unit;
                    if (pal && tl >= 313) lab = fmt("%d (f2: %d)", tl, tl - 313);
                    else if (!pal && tl >= 263) lab = fmt("%d (f2: %d)", tl, tl - 263);
                    else if (by_field) lab = fmt("%d / %d", tl, tl + (pal ? 313 : 263));
                    text_out(dc, left, y + (h - S(hwnd, 15)) / 2, lab, MUTED, fnt);
                    double cw = (double)(W - x0) / std::max<size_t>(1, Ls.size());
                    for (size_t i = 0; i < Ls.size(); i++) {
                        const Json &L = *Ls[i];
                        std::string kind = L["kind"].str();
                        COLORREF cc = service_colour(kind);
                        RECT bx{x0 + (int)(i * cw) + 1, y + 1, x0 + (int)((i + 1) * cw) - 2, y + h - 1};
                        fill(dc, bx, cc);
                        if (kind != "empty") {
                            std::string txt = L.has("label") ? L["label"].str() : kind;
                            size_t cut = txt.find(" ("); if (cut != std::string::npos) txt = txt.substr(0, cut);
                            if (!L["parity"].is_null()) txt = std::string(1, "AB"[L["parity"].integer()]) + ": " + txt;
                            if (L.has("read") && !L["read"].is_null() && L["read"].num() != 0) txt += fmt("  · read %.0f%%", L["read"].num() * 100);
                            bool darkt = (GetRValue(cc) * 3 + GetGValue(cc) * 6 + GetBValue(cc)) > 1100;
                            HRGN clip = CreateRectRgn(bx.left, bx.top, bx.right, bx.bottom); SelectClipRgn(dc, clip);
                            text_out(dc, bx.left + S(hwnd, 6), y + (h - S(hwnd, 15)) / 2, txt, darkt ? RGB(0x11, 0x11, 0x11) : FG, fnt);
                            SelectClipRgn(dc, nullptr); DeleteObject(clip);
                        }
                    }
                    y += h;
                }
            }
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == IDCANCEL) DestroyWindow(hwnd);
            else if (id == 50) { std::string rep = path_join(vbi_report_dir(R.get_str("file")), "report.txt"); if (exists(rep)) open_path(rep); }
            else if (id >= 100 && id - 100 < (int)R["results"].size()) opener(R["results"][id - 100]);
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
void recording_window(HWND owner, const Json &report, std::function<void(const Json &)> opener) {
    auto *w = new RecWin; w->R = report; w->opener = opener;
    w->create(L"TRRec", "What is in " + basename(report.get_str("file")), WS_OVERLAPPEDWINDOW, S(owner, 1040), S(owner, 680), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}

// ================================================================ табло Silent Radio
struct SignWin : Window {
    Json data; std::string folder; std::vector<std::string> zones; int zi = 0;
    HWND zone_b, pbtn, prevb, nextb, browser, cap, lst;
    std::vector<int> state = std::vector<int>(SR_W, 0), prev = std::vector<int>(SR_W, 0);
    int it = 0, fr = 0; bool playing = true; double t0 = 0, el = 0;
    RECT sign{};
    double now() { return GetTickCount64() / 1000.0; }
    const Json &items() { return data["zones"][zones[zi]]; }
    double dur(const Json &f) {
        if (f.has("f")) return f["t"].num();
        size_t n = f.has("scroll") ? f["scroll"].size() : 0;
        if (!f.has("scroll")) for (auto &l : f["scroll2"].a) n = std::max(n, l["cols"].size());
        return (SR_W + n) / f["speed"].num(40);
    }
    std::vector<int> cols(const Json &f, double e) {
        std::vector<int> out(SR_W, 0);
        if (f.has("f")) {
            for (int x = 0; x < SR_W && x < (int)f["f"].size(); x++) out[x] = f["f"][x].integer();
            if (f.get_str("fx") == "wipe" && e < 0.4) { int k = (int)(e / 0.4 * SR_W); for (int x = k; x < SR_W; x++) out[x] = prev[x]; }
            return out;
        }
        int off = (int)(e * f["speed"].num(40));
        auto line = [&](const Json &c, int row) { for (int x = 0; x < SR_W; x++) { int k = x - SR_W + off; if (k >= 0 && k < (int)c.size()) out[x] |= c[k].integer() << row; } };
        if (f.has("scroll")) line(f["scroll"], f["row"].integer()); else for (auto &l : f["scroll2"].a) line(l["cols"], l["row"].integer());
        return out;
    }
    void select() {
        set_text(zone_b, "zone " + zones[zi] + " \xE2\x96\xBE");
        ListView_DeleteAllItems(lst);
        int i = 0; for (auto &x : items().a) { lv_set(lst, i, 0, x.get_str("seq")); lv_set(lst, i, 1, x.get_str("title")); i++; }
        go(0);
    }
    void go(int i, bool from_list = false) {
        int n = (int)items().size(); if (!n) return;
        it = ((i % n) + n) % n; fr = 0; t0 = now(); el = 0;
        if (!from_list) { ListView_SetItemState(lst, -1, 0, LVIS_SELECTED); ListView_SetItemState(lst, it, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lst, it, FALSE); }
        std::string c; for (auto &t : items()[it]["text"].a) c += (c.empty() ? "" : "  \xC2\xB7  ") + t.str();
        set_text(cap, c);
    }
    void tick() {
        if (!playing || zones.empty() || !items().size()) return;
        const Json *item = &items()[it]; const Json *f = &(*item)["frames"][fr];
        el = now() - t0;
        if (el >= dur(*f)) {
            if (f->has("f")) for (int x = 0; x < SR_W && x < (int)(*f)["f"].size(); x++) prev[x] = (*f)["f"][x].integer();
            fr++; t0 = now(); el = 0;
            if (fr >= (int)(*item)["frames"].size()) { go(it + 1); return; }
            f = &(*item)["frames"][fr];
        }
        auto c = cols(*f, el);
        if (c != state) { state = c; InvalidateRect(hwnd, &sign, FALSE); }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            for (auto &kv : data["zones"].o) zones.push_back(kv.first);
            std::sort(zones.begin(), zones.end());
            HWND t = label(hwnd, "Silent Radio"); SendMessageW(t, WM_SETFONT, (WPARAM)font(hwnd, 12, true), TRUE); SetPropW(t, L"title", (HANDLE)1);
            zone_b = button(hwnd, 301, "zone"); pbtn = button(hwnd, 302, "\xE2\x8F\xB8 Pause"); prevb = button(hwnd, 303, "\xE2\x8F\xAE"); nextb = button(hwnd, 304, "\xE2\x8F\xAD");
            browser = button(hwnd, 305, "Open in browser");
            cap = label(hwnd, ""); set_label_colour(cap, ACCENT2);
            lst = listview(hwnd, 10, {{"", 70}, {"", 820}});
            SetWindowLongW(lst, GWL_STYLE, GetWindowLongW(lst, GWL_STYLE) | LVS_NOCOLUMNHEADER);
            SetTimer(hwnd, 1, 30, nullptr);
            if (!zones.empty()) select();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = S(hwnd, 10), bh = S(hwnd, 30);
            HWND t = FindWindowExW(hwnd, nullptr, L"STATIC", L"Silent Radio");
            MoveWindow(t, p, y + S(hwnd, 4), S(hwnd, 120), S(hwnd, 24), TRUE);
            int x = p + S(hwnd, 130);
            MoveWindow(zone_b, x, y, S(hwnd, 90), bh, TRUE); x += S(hwnd, 100);
            MoveWindow(pbtn, x, y, S(hwnd, 100), bh, TRUE); x += S(hwnd, 106);
            MoveWindow(prevb, x, y, S(hwnd, 40), bh, TRUE); x += S(hwnd, 46);
            MoveWindow(nextb, x, y, S(hwnd, 40), bh, TRUE);
            MoveWindow(browser, rc.right - p - S(hwnd, 150), y, S(hwnd, 150), bh, TRUE);
            y += bh + S(hwnd, 10);
            int d = std::max(3, std::min(8, (int)(rc.right - 2 * p - S(hwnd, 20)) / SR_W));
            sign = {p, y, p + d * SR_W + S(hwnd, 20), y + d * SR_H + S(hwnd, 20)};
            y = sign.bottom + S(hwnd, 6);
            MoveWindow(cap, p, y, rc.right - 2 * p, S(hwnd, 40), TRUE); y += S(hwnd, 44);
            MoveWindow(lst, p, y, rc.right - 2 * p, rc.bottom - y - p, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_TIMER: tick(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(mem, bm);
            fill(mem, rc, BG);
            fill(mem, sign, RGB(0x11, 0x11, 0x11));
            int pad = S(hwnd, 10), d = (sign.right - sign.left - 2 * pad) / SR_W;
            RECT in{sign.left + pad, sign.top + pad, sign.left + pad + d * SR_W, sign.top + pad + d * SR_H}; fill(mem, in, RGB(0, 0, 0));
            HBRUSH on = CreateSolidBrush(RGB(0xff, 0xa3, 0x1a)), off = CreateSolidBrush(RGB(0x2a, 0x16, 0x06));
            HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
            for (int x = 0; x < SR_W; x++) for (int y = 0; y < SR_H; y++) {
                SelectObject(mem, (state[x] >> y) & 1 ? on : off);
                Ellipse(mem, in.left + x * d + 1, in.top + y * d + 1, in.left + x * d + d, in.top + y * d + d);
            }
            SelectObject(mem, op); DeleteObject(on); DeleteObject(off);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == NM_CLICK) { int i = ((NMITEMACTIVATE *)l)->iItem; if (i >= 0) go(i, true); }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 301 && !zones.empty()) { zi = (zi + 1) % zones.size(); select(); }
            else if (id == 302) { playing = !playing; set_text(pbtn, playing ? "\xE2\x8F\xB8 Pause" : "\xE2\x96\xB6 Play"); if (playing) t0 = now() - el; }
            else if (id == 303) go(it - 1);
            else if (id == 304) go(it + 1);
            else if (id == 305) open_path(path_join(folder, "index.html"));
            return 0;
        }
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
void sign_window(HWND owner, const std::string &folder) {
    auto *w = new SignWin; w->folder = folder;
    w->data = load_json(path_join(folder, "packets.json"), Json::object());
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, SR_W * 8 + 60));
    w->create(L"TRSign", "Silent Radio \xE2\x80\x94 " + w->data.get_str("source"), WS_OVERLAPPEDWINDOW, width, S(owner, 660), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}
