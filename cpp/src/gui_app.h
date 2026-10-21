// Windows of the program: one per step and one per decoded service.
#pragma once
#include "ui/gui.h"
#include "decoder.h"
#include "services.h"
#include "finds.h"
#include <functional>
#include <map>
#include <memory>

struct LiveVps;   // live.h
struct VbiRepair;  // vbirepair.h
namespace ui {
// common header: bold title, muted subtitle, buttons on the right
struct Header {   // shared by the service windows
    HWND title = nullptr, sub = nullptr;
    std::vector<std::pair<HWND, int>> buttons;   // button, width
    void create(HWND h, const std::string &t, const std::string &s)
    {
        title = label(h, t); SendMessageW(title, WM_SETFONT, (WPARAM)font(h, 12, true), TRUE);
        sub = label(h, s); set_label_colour(sub, MUTED);
    }
    HWND add(HWND h, int id, const std::string &text, int width) { HWND b = button(h, id, text); buttons.push_back({b, width}); return b; }
    int layout(HWND h)                            // returns the y below the header
    {
        RECT rc; GetClientRect(h, &rc);
        int p = S(h, 14), y = S(h, 10), bh = S(h, 30);
        HDC dc = GetDC(h); HGDIOBJ of = SelectObject(dc, font(h, 12, true));
        std::wstring t = W(get_text(title)); SIZE sz; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        SelectObject(dc, of); ReleaseDC(h, dc);
        int tw = sz.cx + S(h, 10);
        MoveWindow(title, p, y + S(h, 4), tw, S(h, 24), TRUE);
        int x = rc.right - p;
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) { x -= S(h, it->second); MoveWindow(it->first, x, y, S(h, it->second), bh, TRUE); x -= S(h, 8); }
        MoveWindow(sub, p + tw + S(h, 4), y + S(h, 8), std::max(0, x - p - tw - S(h, 8)), S(h, 20), TRUE);
        return y + bh + S(h, 10);
    }
};

void open_service(HWND owner, const ResultPtr &r, const Service &s);

void recording_window(HWND owner, const ResultPtr &r);
void lines_window(HWND owner, const ResultPtr &r);      // raw VBI lines: raster, waveform, over time; line types by hand
// decodes a recording with line types set by hand (VBI lines window -> main window)
std::function<void(const std::string &, VbiFormat, const std::map<int, LineForce> &)> &decode_lines_hook();
void starsight_window(HWND owner, const ResultPtr &r);
void captions_window(HWND owner, const ResultPtr &r);
void xds_window(HWND owner, const ResultPtr &r);
void sign_window(HWND owner, const ResultPtr &r);
void amol_window(HWND owner, const ResultPtr &r);
void gemstar_window(HWND owner, const ResultPtr &r);
void jtes_window(HWND owner, const ResultPtr &r);
void tvinform_window(HWND owner, const ResultPtr &r);
// files and texts the recording brought (finds.h); the live one asks source() every 2 s
void finds_window(HWND owner, const ResultPtr &r);
void finds_window_live(HWND owner, std::function<std::vector<Find>()> source, const std::string &subtitle,
                       std::function<std::string()> activity = nullptr);   // activity: a live status line
void vps_window_live(HWND owner, std::function<LiveVps()> source);   // VPS / WSS of the capture
void teletext_window(HWND owner, const ResultPtr &r);
void nabts_window(HWND owner, const ResultPtr &r);
void data_window(HWND owner, const ResultPtr &r, const std::string &kind);
void text_window(HWND owner, const std::string &title, const std::string &sub, const std::string &body);
// capture card: picture and VBI decoded live, recording (gui_capture.cpp)
void capture_window(HWND owner, bool start = false, const std::string& testFile = "");
std::function<void(const std::string &)> &open_recording_hook();   // the main window opens a recording with it
// a recording the repair window works on: measure, ranges of frames, line shifts, saving (none in live)
struct RepairSource {
    std::string path;                         // the .vbi (empty: sliders only)
    int format = 0;                           // VbiFormat of the recording
    std::function<int64_t()> frame;           // the opener's current frame
    std::function<std::string(int)> lineName; // name of a stored line ("17 f1")
    std::function<void(int64_t)> go;          // move the opener to a frame
};
// VBI repair (VHSTTX controls): every change goes to onChange; actions = buttons in its header
HWND repair_window(HWND owner, const std::string &subtitle, std::function<void(const VbiRepair &)> onChange,
                   std::vector<std::pair<std::string, std::function<void(const VbiRepair &)>>> actions = {},
                   const RepairSource *src = nullptr);
VbiRepair repair_saved();   // the settings last used
// the VBI Repair tool of VHSTTX itself (teletext.exe vbirepair -c <card> <file>) on a recording; the place of
// teletext.exe is asked once and kept (state "vhsttx_exe"); false if the format has no VHSTTX card
bool vhsttx_repair(HWND owner, const std::string &vbi, int format);

// keys a top-level window handles before IsDialogMessage (editor: Ctrl+S / Ctrl+Z, Enter in fields)
std::map<HWND, std::function<bool(MSG &)>> &pretranslators();
}
