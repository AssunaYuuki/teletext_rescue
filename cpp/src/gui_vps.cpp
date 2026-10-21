// "VPS / WSS — live": the fixed service lines of the capture. VPS (line 16): the network by its CNI, the label
// of the programme on the air (or a special code: pause, record inhibit, timer), sound, programme type, the
// broadcasters' own bytes; WSS (line 23): aspect ratio and the rest. Every field explained; the labels as they
// changed, with the time, below.
#include "gui_app.h"
#include "live.h"
#include "vbicodes.h"
#include <algorithm>

namespace ui {
namespace {
void vpaint(HWND h, const std::function<void(HDC, RECT)> &draw)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ ob = SelectObject(mem, bm);
    fill(mem, rc, BG);
    draw(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps);
}

void set_clipboard(HWND h, const std::string &s)
{
    std::wstring w = widen(s);
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

std::string country(int cni)
{
    switch (cni >> 8) {
    case 0xD: return "Germany";
    case 0xA: return "Austria";
    case 0x4: return "Switzerland";
    case 0x7: return "Ukraine";
    }
    return "";
}

struct VpsWin : Window {
    std::function<LiveVps()> source;
    LiveVps v;
    Header hd;
    HWND lst = nullptr;
    RECT cardV{}, cardW{};
    size_t shownHist = (size_t)-1;

    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int p = S(hwnd, 14), y = hd.layout(hwnd);
        cardV = {p, y, rc.right - p, y + S(hwnd, 236)}; y = cardV.bottom + S(hwnd, 10);
        cardW = {p, y, rc.right - p, y + S(hwnd, 92)}; y = cardW.bottom + S(hwnd, 10);
        MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void fill_list()
    {
        size_t n = v.history.size() + v.wssHistory.size();
        if (n == shownHist) return;
        shownHist = n;
        struct Ev { int64_t frame; std::string time, what, text; };
        std::vector<Ev> all;
        for (auto &e : v.history) all.push_back({e.frame, e.time, "VPS", e.text});
        for (auto &e : v.wssHistory) all.push_back({e.frame, e.time, "WSS", e.text});
        std::sort(all.begin(), all.end(), [](const Ev &a, const Ev &b) { return a.frame > b.frame; });   // newest first
        SendMessageW(lst, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(lst);
        for (size_t i = 0; i < all.size(); i++) {
            lv_set(lst, (int)i, 0, all[i].time);
            lv_set(lst, (int)i, 1, fmt("%lld", (long long)all[i].frame));
            lv_set(lst, (int)i, 2, all[i].what);
            lv_set(lst, (int)i, 3, all[i].text);
        }
        SendMessageW(lst, WM_SETREDRAW, TRUE, 0);
    }
    // one row of the card: name, value, what it means
    void row(HDC dc, int x, int &y, const std::string &name, const std::string &value, const std::string &note)
    {
        HFONT f = font(hwnd, 10), fb = font(hwnd, 10, true), fs = font(hwnd, 9);
        text_out(dc, x, y, name, MUTED, f);
        text_out(dc, x + S(hwnd, 150), y, value, FG, fb);
        if (!note.empty()) text_out(dc, x + S(hwnd, 470), y + S(hwnd, 1), note, MUTED, fs);
        y += S(hwnd, 22);
    }
    void paint(HDC dc, RECT)
    {
        HFONT big = font(hwnd, 15, true), small = font(hwnd, 9);
        // VPS
        fill(dc, cardV, PANEL);
        int x = cardV.left + S(hwnd, 18), y = cardV.top + S(hwnd, 12);
        if (!v.everVps) {
            text_out(dc, x, y, "No VPS", FG, big);
            text_out(dc, x, y + S(hwnd, 34), "VPS is line 16 of the first field (Germany, Austria, Switzerland: the label a video recorder starts by).", MUTED, small);
            text_out(dc, x, y + S(hwnd, 54), "Nothing with its start code has come on that line yet; the card must give line 16 in its VBI window.", MUTED, small);
        } else {
            VpsLabel l = vpsLabel(v.bytes);
            std::string name = vpsCniName(l.cni);
            text_out(dc, x, y, (name.empty() ? std::string("network not in the register") : name) + (v.have ? "" : "   (no VPS for the last 5 s)"), v.have ? FG : MUTED, big);
            y += S(hwnd, 32);
            text_out(dc, x, y, fmt("line %d  \xC2\xB7  read in %.0f%% of the frames lately  \xC2\xB7  %lld lines read", v.vpsLine ? v.vpsLine : 16, v.vpsRate * 100, (long long)v.reads), ACCENT2, small);
            y += S(hwnd, 24);
            std::string c = country(l.cni);
            row(dc, x, y, "Network (CNI)", fmt("%03X", l.cni), (c.empty() ? std::string("") : c + ", ") + (name.empty() ? "not in the ETSI TS 101 231 register" : "ETSI TS 101 231 register: " + name));
            std::string lab, note;
            if (l.day == 0 && l.month == 15 && l.minute == 63 && l.hour >= 28) {
                static const char *const sp[] = {"continuation code", "interruption code", "record inhibit", "timer control code"};
                static const char *const ex[] = {"the programme goes on", "the programme is interrupted (adverts, news): a VPS recorder pauses",
                                                 "nothing here is to be recorded (test card, gap)", "no programme label is sent: a recorder set by VPS records by its own clock"};
                lab = sp[l.hour - 28]; note = ex[l.hour - 28];
            } else if (l.day >= 1 && l.month >= 1 && l.month <= 12 && l.hour < 24 && l.minute < 60) {
                lab = fmt("%02d.%02d  %02d:%02d", l.day, l.month, l.hour, l.minute);
                note = "the programme on the air, by its announced start: a VPS recorder set for it records while this is sent";
            } else {
                lab = fmt("%02d.%02d  %02d:%02d", l.day, l.month, l.hour, l.minute);
                note = "not a date: a special or damaged label";
            }
            row(dc, x, y, "Programme label", lab, note);
            static const char *const snd[] = {"not given", "mono", "stereo", "two sound channels (bilingual)"};
            row(dc, x, y, "Sound", snd[l.sound], "byte 5, bits 7-6");
            row(dc, x, y, "Programme type", l.pty == 0xFF || l.pty == 0 ? fmt("%02X (none)", l.pty) : fmt("%02X", l.pty), "byte 15");
            row(dc, x, y, "Source data", fmt("%02X %02X", v.bytes[0], v.bytes[1]), "bytes 3-4: source and control data between the broadcasters");
            std::string hex;
            for (int k = 0; k < 13; k++) hex += fmt("%02X ", v.bytes[k]);
            row(dc, x, y, "Bytes 3-15", hex, "each byte voted over the readings of the last 2 s");
        }
        // WSS
        fill(dc, cardW, PANEL);
        x = cardW.left + S(hwnd, 18); y = cardW.top + S(hwnd, 12);
        if (!v.everWss) {
            text_out(dc, x, y, "No WSS", FG, font(hwnd, 12, true));
            text_out(dc, x, y + S(hwnd, 28), "WSS is line 23 of the first field (wide-screen signalling: aspect ratio, subtitles, copy protection).", MUTED, small);
        } else {
            text_out(dc, x, y, "WSS: " + wssText(v.wssBits) + (v.wssHave ? "" : "   (none for the last 5 s)"), v.wssHave ? FG : MUTED, font(hwnd, 12, true));
            y += S(hwnd, 30);
            std::string bits;
            for (int i = 0; i < 14; i++) bits += ((v.wssBits >> i) & 1) ? '1' : '0';
            text_out(dc, x, y, fmt("line %d  \xC2\xB7  %.0f%% of the frames  \xC2\xB7  %lld lines read  \xC2\xB7  bits b0..b13 ", v.wssLine ? v.wssLine : 23, v.wssRate * 100, (long long)v.wssReads) + bits, ACCENT2, small);
        }
    }
    void refresh()
    {
        v = source();
        fill_list();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            hd.create(hwnd, "VPS / WSS \xE2\x80\x94 live", "the service lines 16 and 23 of the capture, as they come \xC2\xB7 the labels as they changed below");
            hd.add(hwnd, 601, "Copy", 80);
            lst = listview(hwnd, 10, {{"Time", 90}, {"Frame", 90}, {"Service", 70}, {"Label", 900}});
            v = source();
            fill_list();
            SetTimer(hwnd, 1, 1000, nullptr);
            layout();
            return 0;
        case WM_TIMER: refresh(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_PAINT: vpaint(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_COMMAND:
            if (LOWORD(w) == 601) {
                std::string t;
                if (v.everVps) {
                    t += "VPS: " + vpsText(v.bytes) + "\r\n";
                }
                if (v.everWss) t += "WSS: " + wssText(v.wssBits) + "\r\n";
                for (auto &e : v.history) t += e.time + "  VPS  " + e.text + "\r\n";
                for (auto &e : v.wssHistory) t += e.time + "  WSS  " + e.text + "\r\n";
                set_clipboard(hwnd, t);
            }
            return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
}  // namespace

void vps_window_live(HWND owner, std::function<LiveVps()> source)
{
    auto *w = new VpsWin;
    w->source = std::move(source);
    int width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, 1100));
    w->create(L"TRVps", "VPS / WSS \xE2\x80\x94 live", WS_OVERLAPPEDWINDOW, width, S(owner, 720), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}
}
