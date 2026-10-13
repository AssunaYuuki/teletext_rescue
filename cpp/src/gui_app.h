// Windows of the program: one per step and one per decoded service.
#pragma once
#include "ui/gui.h"
#include "decoder.h"
#include "services.h"
#include <functional>
#include <map>
#include <memory>

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
void lines_window(HWND owner, const ResultPtr &r);      // raw VBI lines: raster, waveform, over time
void starsight_window(HWND owner, const ResultPtr &r);
void captions_window(HWND owner, const ResultPtr &r);
void xds_window(HWND owner, const ResultPtr &r);
void sign_window(HWND owner, const ResultPtr &r);
void amol_window(HWND owner, const ResultPtr &r);
void gemstar_window(HWND owner, const ResultPtr &r);
void jtes_window(HWND owner, const ResultPtr &r);
void teletext_window(HWND owner, const ResultPtr &r);
void nabts_window(HWND owner, const ResultPtr &r);
void data_window(HWND owner, const ResultPtr &r, const std::string &kind);
void text_window(HWND owner, const std::string &title, const std::string &sub, const std::string &body);

// keys a top-level window handles before IsDialogMessage (editor: Ctrl+S / Ctrl+Z, Enter in fields)
std::map<HWND, std::function<bool(MSG &)>> &pretranslators();
}
