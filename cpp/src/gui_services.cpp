// One window per decoded service, drawn like the rest of the program:
//   StarSight — the programme guide itself: channels, schedule grid, show details;
//   CC captions — a caption screen with the lines of each service (CC1-CC4, T1-T4);
//   XDS — station card, the broadcast clock, every packet;
//   Silent Radio — the LED sign with its messages;
//   Japanese teletext — the pages as the TU-TX100 tuner draws them, tunes played;
//   Gemstar — the GUIDE Plus+ programme grid (the packets when no programmes were sent);
//   5.73 Mbit/s data — summary and recovered text; and plain text windows (report).
#include "gui_app.h"
#include <mmsystem.h>
#include "ssjson.h"
#include "slicer.h"
#include "faxg4.h"
#include <set>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>

namespace ui {
namespace {
const COLORREF C_CHAN = RGB(0x3a, 0x7b, 0xd5), C_TIME = RGB(0xff, 0xa3, 0x1a), C_PROG = RGB(0x2e, 0xa0, 0x6a), C_OTHER = RGB(0x8a, 0x8d, 0x93);
const COLORREF C_PAY = RGB(0x8e, 0x5c, 0xd9);


void paint_buffered(HWND h, const std::function<void(HDC, RECT)> &draw)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ ob = SelectObject(mem, bm);
    fill(mem, rc, BG);
    draw(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps);
}

void draw_text(HDC dc, RECT r, const std::string &s, COLORREF c, HFONT f, UINT flags)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    std::wstring w = W(s);
    DrawTextW(dc, w.c_str(), (int)w.size(), &r, flags | DT_NOPREFIX);
    SelectObject(dc, o);
}

// tiles in the top-right of a card: big value, label under it
void tiles(HWND h, HDC dc, RECT card, const std::vector<std::pair<std::string, std::string>> &t)
{
    HFONT smallb = font(h, 10, true), small = font(h, 9);
    int tx = card.right - S(h, 16);
    for (int i = (int)t.size() - 1; i >= 0; i--) {
        int w = S(h, 110); RECT r{tx - w, card.top + S(h, 14), tx, card.top + S(h, 74)};
        fill(dc, r, FIELD);
        text_out(dc, (r.left + r.right) / 2, r.top + S(h, 8), t[i].first, FG, smallb, TA_CENTER);
        text_out(dc, (r.left + r.right) / 2, r.top + S(h, 32), t[i].second, MUTED, small, TA_CENTER);
        tx -= w + S(h, 8);
    }
}

void open_window(Window *w, HWND owner, const wchar_t *cls, const std::string &title, int width, int height)
{
    width = std::min(GetSystemMetrics(SM_CXSCREEN) - 40, S(owner, width));
    w->create(cls, title, WS_OVERLAPPEDWINDOW, width, S(owner, height), owner);
    ShowWindow(w->hwnd, SW_SHOW);
}

// ================================================================ StarSight programme guide
const int SPAN = 120;                 // minutes across the grid
const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct GuideWin : Window {
    ResultPtr R;
    const SsDatabase *db = nullptr;
    std::map<int, std::vector<SsSlot>> slots;
    std::vector<int> chans;
    SsTime view = 0, sel_t = 0;       // UTC minutes: left edge of the grid, instant in the selected show
    int top = 0, sel = 0;
    Header hd;
    RECT card{}, days{}, ruler{}, grid{}, info{};
    int rowh = 30, chw = 130;

    int offset() const { return db->utcOffset(); }
    SsTime local(SsTime t) const { return t + offset(); }
    std::string tlabel(SsTime utc) const
    {
        int y, mo, d, h, mi, wd; ssCivil(local(utc), y, mo, d, h, mi, wd);
        return fmt("%d:%02d %s", h % 12 == 0 ? 12 : h % 12, mi, h < 12 ? "AM" : "PM");
    }
    std::string dlabel(SsTime utc) const
    {
        int y, mo, d, h, mi, wd; ssCivil(local(utc), y, mo, d, h, mi, wd);
        return fmt("%s %d %s %d", DAYS[wd], d, MONTHS[mo - 1], y);
    }
    std::string call(int id) const
    {
        auto it = db->channels.find(id);
        if (it != db->channels.end() && !it->second.name.empty()) return it->second.callLetters();
        std::string lab, cl;   // channel packet not received
        if (ssKnownChannel(id, lab, cl)) return !lab.empty() ? lab : cl.substr(0, 5);
        return fmt("%d", id);
    }
    std::string cname(int id) const
    {
        auto it = db->channels.find(id);
        if (it != db->channels.end() && !it->second.name.empty()) return it->second.name;
        std::string lab, cl;
        if (ssKnownChannel(id, lab, cl)) return (cl.empty() ? lab : cl);
        return fmt("channel %d", id);
    }
    void build()
    {
        db = &R->db;
        for (int id : db->channelIds()) { auto v = db->slotsFor(id); if (!v.empty()) slots[id] = std::move(v); }
        for (auto &kv : slots) chans.push_back(kv.first);
        std::stable_sort(chans.begin(), chans.end(), [this](int a, int b) {
            auto na = db->channels.count(a) && !db->channels.at(a).name.empty(), nb = db->channels.count(b) && !db->channels.at(b).name.empty();
            if (na != nb) return na;
            if (na) return db->channels.at(a).name < db->channels.at(b).name;
            return a < b;
        });
        now();
    }
    void now()
    {
        SsTime t = db->nowTime();
        if (!t) t = db->firstTime();
        // the broadcast clock may precede the schedules (lists are sent days ahead): then the first show after it
        bool any = false; SsTime next = INT64_MAX;
        for (auto &kv : slots) for (auto &s : kv.second) {
            if (s.start < t + SPAN && s.start + s.duration > t) any = true;
            if (s.start >= t && !(s.flags & 0x01)) next = std::min(next, s.start);
        }
        if (!any && next != INT64_MAX) t = next;
        view = local(t) / 30 * 30 - offset();
        sel_t = t;
        // start on the first channel that has a show at that moment
        for (size_t i = 0; i < chans.size(); i++) if (slot_at(chans[i], sel_t)) { sel = (int)i; break; }
        top = std::max(0, std::min(top, sel));
    }
    const SsSlot *slot_at(int ch, SsTime t) const
    {
        auto it = slots.find(ch);
        if (it == slots.end()) return nullptr;
        for (auto &s : it->second) if (t >= s.start && t < s.start + s.duration) return &s;
        return nullptr;
    }
    const SsSlot *selected() const { return chans.empty() ? nullptr : slot_at(chans[sel], sel_t); }
    int rows_visible() const { return std::max(1, (int)(grid.bottom - grid.top) / std::max(1, rowh)); }
    int x_of(SsTime t) const { int x0 = grid.left + chw; return x0 + (int)((t - view) * (grid.right - x0) / SPAN); }
    void move(int dr, int dt)
    {
        if (chans.empty()) return;
        if (dr) {
            sel = std::max(0, std::min((int)chans.size() - 1, sel + dr));
            if (sel < top) top = sel;
            if (sel >= top + rows_visible()) top = sel - rows_visible() + 1;
        }
        if (dt) {
            const SsSlot *s = selected();
            sel_t = dt > 0 ? (s ? s->start + s->duration : sel_t + 30) : (s ? s->start - 1 : sel_t - 30);
            const SsSlot *n = selected();
            if (n && dt < 0) sel_t = std::max(n->start, view - 30 * 8);
        }
        while (sel_t >= view + SPAN) view += 30;
        while (sel_t < view) view -= 30;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void paint(HDC dc, RECT rc)
    {
        HFONT big = font(hwnd, 26, true), smallb = font(hwnd, 10, true), small = font(hwnd, 9), norm = font(hwnd, 10), mid = font(hwnd, 14, true);
        // card
        fill(dc, card, PANEL);
        int x = card.left + S(hwnd, 18);
        text_out(dc, x, card.top + S(hwnd, 10), "StarSight", C_TIME, big);
        SsTime clock = db->nowTime();
        text_out(dc, x, card.bottom - S(hwnd, 44), clock ? "broadcast clock " + dlabel(clock) + "  " + tlabel(clock) : "no broadcast clock in this recording", ACCENT2, small);
        text_out(dc, x, card.bottom - S(hwnd, 24), "on-screen programme guide for TV sets and VCRs, carried by PBS stations; listings from the show-list packets", DIM, small);
        const SsStats &st = db->stats;
        int good = st.crcOk + st.fixedSingle + st.fixedChase + st.fixedCombine;
        tiles(hwnd, dc, card, {{fmt("%d", st.packets), "packets"}, {fmt("%d", good), "good (CRC)"}, {fmt("%zu", chans.size()), "channels"},
                               {fmt("%zu", db->showLists.size()), "show lists"}, {fmt("%zu", db->programs.size()), "titles"}});
        // days
        fill(dc, days, BG);
        int64_t today = local(clock ? clock : view) / 1440, vday = local(view) / 1440;
        int dw = (days.right - days.left - chw) / 7;
        text_out(dc, days.left, days.top + S(hwnd, 6), dlabel(view).substr(0, dlabel(view).rfind(' ')), FG, smallb);
        for (int i = 0; i < 7; i++) {
            RECT r{days.left + chw + i * dw + 1, days.top, days.left + chw + (i + 1) * dw - 2, days.bottom};
            int64_t day = today + i;
            int y, mo, d, h, mi, wd; ssCivil(day * 1440, y, mo, d, h, mi, wd);
            bool cur = day == vday;
            fill(dc, r, cur ? ACCENT : FIELD);
            draw_text(dc, r, fmt("%s %d", DAYS[wd], d), cur ? BG : FG, cur ? smallb : norm, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // time ruler
        fill(dc, ruler, FIELD);
        for (int k = 0; k < SPAN / 30; k++) {
            SsTime t = view + k * 30;
            int xx = x_of(t);
            if (k) { RECT ln{xx, ruler.top + S(hwnd, 4), xx + 1, ruler.bottom - S(hwnd, 4)}; fill(dc, ln, LINE); }
            text_out(dc, xx + S(hwnd, 8), ruler.top + S(hwnd, 5), tlabel(t), MUTED, norm);
        }
        // grid
        fill(dc, grid, PANEL);
        SsTime nowt = clock;
        int vis = rows_visible();
        for (int r = 0; r < vis; r++) {
            int idx = top + r;
            RECT row{grid.left, grid.top + r * rowh, grid.right, grid.top + (r + 1) * rowh};
            if (idx >= (int)chans.size()) break;
            int ch = chans[idx];
            bool selrow = idx == sel;
            RECT cc{row.left, row.top + 1, row.left + chw - S(hwnd, 6), row.bottom - 1};
            fill(dc, cc, selrow ? LINE : FIELD);
            bool pay = false; for (auto &s : slots[ch]) pay |= (s.flags & 0x40) != 0;
            RECT bar{cc.left, cc.top, cc.left + S(hwnd, 4), cc.bottom}; fill(dc, bar, pay ? C_PAY : C_CHAN);
            RECT ct{cc.left + S(hwnd, 12), cc.top, cc.right - S(hwnd, 4), cc.bottom};
            draw_text(dc, ct, call(ch), FG, smallb, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, ct, fmt("%d", ch % 1000), DIM, small, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            for (auto &s : slots[ch]) {
                if (s.start + s.duration <= view || s.start >= view + SPAN) continue;
                int x0 = std::max(x_of(s.start), (int)(grid.left + chw)), x1 = std::min(x_of(s.start + s.duration), (int)grid.right);
                RECT b{x0 + 1, row.top + 1, x1 - 1, row.bottom - 1};
                bool is_sel = selrow && sel_t >= s.start && sel_t < s.start + s.duration;
                bool past = nowt && s.start + s.duration <= nowt;
                COLORREF bg = is_sel ? ACCENT : past ? RGB(0x29, 0x2b, 0x30) : FIELD;
                fill(dc, b, bg);
                if (s.flags & 0x40) { RECT pb{b.left, b.bottom - S(hwnd, 3), b.right, b.bottom}; fill(dc, pb, C_PAY); }
                std::string t = R->db.titleOf(s);
                if (s.start < view) t = "\xE2\x97\x80 " + t;
                RECT tr{b.left + S(hwnd, 8), b.top, b.right - S(hwnd, 4), b.bottom};
                draw_text(dc, tr, t, is_sel ? BG : past ? MUTED : FG, is_sel ? smallb : norm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            RECT ln{grid.left + chw, row.bottom - 1, grid.right, row.bottom}; fill(dc, ln, BG);
        }
        if (nowt && nowt >= view && nowt < view + SPAN) { int xx = x_of(nowt); RECT c{xx - 1, ruler.top, xx + 1, grid.bottom}; fill(dc, c, C_TIME); }
        if (chans.empty()) {
            RECT m = grid; InflateRect(&m, -S(hwnd, 20), -S(hwnd, 20));
            draw_text(dc, m, "No schedules were received in this recording.", MUTED, norm, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // the selected show
        fill(dc, info, PANEL);
        const SsSlot *s = selected();
        int ix = info.left + S(hwnd, 18), iy = info.top + S(hwnd, 10);
        if (!s) { text_out(dc, ix, iy, chans.empty() ? "" : "no show at this time on " + cname(chans[sel]), MUTED, norm); return; }
        text_out(dc, ix, iy, R->db.titleOf(*s), FG, mid); iy += S(hwnd, 30);
        std::string when = tlabel(s->start) + " \xE2\x80\x93 " + tlabel(s->start + s->duration) + "   \xC2\xB7   " + dlabel(s->start) + "   \xC2\xB7   " + cname(chans[sel]) + fmt("  (%d)", chans[sel] % 1000);
        text_out(dc, ix, iy, when, ACCENT2, norm);
        // what follows on the same channel
        for (auto &n : slots[chans[sel]])
            if (n.start >= s->start + s->duration) {
                SIZE wsz; HGDIOBJ of = SelectObject(dc, norm);
                std::wstring ww = W(when); GetTextExtentPoint32W(dc, ww.c_str(), (int)ww.size(), &wsz); SelectObject(dc, of);
                text_out(dc, ix + wsz.cx + S(hwnd, 30), iy, "next  " + tlabel(n.start) + "  " + R->db.titleOf(n), MUTED, norm);
                break;
            }
        iy += S(hwnd, 22);
        std::string attrs;
        auto add = [&](const std::string &a) { attrs += (attrs.empty() ? "" : "   \xC2\xB7   ") + a; };
        const SsDescription *d = nullptr;
        auto dit = R->db.descriptions.find(s->description);
        if (dit != R->db.descriptions.end()) d = &dit->second;
        auto pit = R->db.programs.find(s->program);
        if (d && d->year) add(fmt("%d", d->year));
        if (d && !d->rating.empty()) add(d->ratingSystem + " " + d->rating);
        if (d && d->critic) add(std::string(d->critic, '*') + " critics");
        if (pit != R->db.programs.end()) { if (pit->second.cc) add("closed captions"); if (pit->second.stereo) add("stereo"); if (pit->second.bw) add("black and white"); }
        if (s->flags & 0x40) add("pay-per-view");
        if (s->flags & 0x01) add("continues from the previous day");
        add(fmt("%d min", s->duration));
        text_out(dc, ix, iy, attrs, MUTED, small); iy += S(hwnd, 22);
        std::string desc;
        if (d && !d->text.empty()) desc = d->partial ? d->text + "\xE2\x80\xA6" : d->text;
        else if (d && d->compressed) desc = "The description is Huffman-compressed with codes not recovered yet.";
        else if (s->description) desc = fmt("Description #%d was not received in this recording.", s->description);
        else desc = "No description was sent for this show.";
        if (d && !d->advisories.empty()) { desc += "\nAdvisory:"; for (auto &a : d->advisories) desc += " " + a; }
        RECT dr{ix, iy, info.right - S(hwnd, 18), info.bottom - S(hwnd, 8)};
        draw_text(dc, dr, desc, d && !d->text.empty() ? FG : DIM, norm, DT_LEFT | DT_WORDBREAK);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            build();
            hd.create(hwnd, "StarSight", fmt("programme guide \xC2\xB7 %zu channels \xC2\xB7 ", chans.size()) + file_name(*R));
            hd.add(hwnd, 401, "Now", 80);
            hd.add(hwnd, 402, "Export JSON\xE2\x80\xA6", 130);
            hd.add(hwnd, 403, "Text report", 120);
            return 0;
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            rowh = S(hwnd, 30); chw = S(hwnd, 130);
            card = {p, y, rc.right - p, y + S(hwnd, 110)}; y = card.bottom + S(hwnd, 10);
            days = {p, y, rc.right - p, y + S(hwnd, 28)}; y = days.bottom + S(hwnd, 6);
            ruler = {p, y, rc.right - p, y + S(hwnd, 26)}; y = ruler.bottom;
            int ih = S(hwnd, 150);
            grid = {p, y, rc.right - p, std::max<LONG>(y + rowh, rc.bottom - p - ih - S(hwnd, 10))};
            info = {p, grid.bottom + S(hwnd, 10), rc.right - p, rc.bottom - p};
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            switch (w) {
            case VK_UP: move(-1, 0); break; case VK_DOWN: move(1, 0); break;
            case VK_LEFT: move(0, -1); break; case VK_RIGHT: move(0, 1); break;
            case VK_PRIOR: move(-rows_visible(), 0); break; case VK_NEXT: move(rows_visible(), 0); break;
            case VK_TAB: { int d = GetKeyState(VK_SHIFT) < 0 ? -1440 : 1440; view += d; sel_t += d; InvalidateRect(hwnd, nullptr, FALSE); break; }
            case VK_HOME: now(); InvalidateRect(hwnd, nullptr, FALSE); break;
            }
            return 0;
        case WM_MOUSEWHEEL: move(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3, 0); return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&grid, pt) && pt.x > grid.left + chw) {
                int r = (pt.y - grid.top) / rowh;
                if (top + r < (int)chans.size()) { sel = top + r; sel_t = view + (SsTime)(pt.x - grid.left - chw) * SPAN / (grid.right - grid.left - chw); }
            } else if (PtInRect(&days, pt) && pt.x > days.left + chw) {
                int i = (pt.x - days.left - chw) / std::max(1, (int)(days.right - days.left - chw) / 7);
                SsTime clock = db->nowTime() ? db->nowTime() : view;
                SsTime tod = local(view) % 1440;
                view = local(clock) / 1440 * 1440 + i * 1440 + tod - offset();
                sel_t = view;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 401) { now(); InvalidateRect(hwnd, nullptr, FALSE); SetFocus(hwnd); }
            else if (id == 402) {
                std::string p = save_file(hwnd, "Export StarSight JSON", stem_path(file_name(*R)) + ".json", "json");
                std::string err;
                if (!p.empty() && !ssExportJson(p, R->db, R.get(), err)) ask(hwnd, "Export", err, MB_ICONERROR);
            } else if (id == 403) {
                std::string s = summarize(*R) + "\n=== StarSight command log ===\n";
                for (auto &l2 : R->db.log) s += l2 + "\n";
                text_window(hwnd, "StarSight \xE2\x80\x94 " + file_name(*R), "packets, repairs and every command received", replace_all(s, "\r\n", "\n"));
            }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ CC captions
struct CapWin : Window {
    ResultPtr R;
    struct Ln { std::string svc, text, time; };
    std::vector<Ln> all;
    std::vector<std::string> svcs; std::string cur;
    std::vector<int> shown;           // indexes into all for the current service
    int sel = 0;
    Header hd; HWND summary, lst; std::vector<HWND> sbtn;
    RECT screen{};
    void load()
    {
        std::map<std::string, int> n;
        if (!R->captionCues.empty())   // with times: the tape's VITC or the place in the recording
            for (auto &c : R->captionCues) {
                all.push_back({c.svc, c.text, captionTime(*R, c.frame)});
                if (!n[c.svc]++) svcs.push_back(c.svc);
            }
        else
            for (auto &l : R->captions) {
                auto c = l.find(": ");
                if (l.rfind("---", 0) == 0 || c == std::string::npos || c > 4) continue;
                all.push_back({l.substr(0, c), l.substr(c + 2), ""});
                if (!n[l.substr(0, c)]++) svcs.push_back(l.substr(0, c));
            }
        std::sort(svcs.begin(), svcs.end(), [](const std::string &a, const std::string &b) { return (a[0] == 'C') != (b[0] == 'C') ? a[0] == 'C' : a < b; });
        if (!svcs.empty()) cur = svcs[0];
        std::string s = fmt("%zu lines", all.size());
        for (auto &k : svcs) s += fmt("  \xC2\xB7  %s %d", k.c_str(), n[k]);
        s += "  \xC2\xB7  CC = captions, T = text mode";
        if (!R->captionCues.empty()) s += captionsUseVitc(*R) ? "  \xC2\xB7  times: VITC time code" : "  \xC2\xB7  times: place in the recording";
        set_text(summary, s);
    }
    void refill()
    {
        shown.clear();
        for (size_t i = 0; i < all.size(); i++) if (all[i].svc == cur) shown.push_back((int)i);
        ListView_DeleteAllItems(lst);
        for (size_t r = 0; r < shown.size() && r < 50000; r++) {
            lv_set(lst, (int)r, 0, fmt("%zu", r + 1)); lv_set(lst, (int)r, 1, all[shown[r]].time); lv_set(lst, (int)r, 2, all[shown[r]].svc); lv_set(lst, (int)r, 3, all[shown[r]].text);
        }
        for (size_t i = 0; i < sbtn.size(); i++) set_button_style(sbtn[i], svcs[i] == cur ? BTN_ACCENT : BTN_NORMAL);
        select(0);
    }
    void select(int i, bool from_list = false)
    {
        if (shown.empty()) { InvalidateRect(hwnd, &screen, FALSE); return; }
        sel = std::max(0, std::min((int)shown.size() - 1, i));
        if (!from_list) { ListView_SetItemState(lst, -1, 0, LVIS_SELECTED); ListView_SetItemState(lst, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lst, sel, FALSE); }
        InvalidateRect(hwnd, &screen, FALSE);
    }
    void paint(HDC dc, RECT)
    {
        // a TV screen with the caption box: the selected line and the ones before it
        fill(dc, screen, RGB(0x11, 0x11, 0x11));
        RECT in{screen.left + S(hwnd, 10), screen.top + S(hwnd, 10), screen.right - S(hwnd, 10), screen.bottom - S(hwnd, 10)};
        fill(dc, in, RGB(0x1d, 0x22, 0x2c));
        HFONT cf = font(hwnd, 16, true, L"Consolas"), small = font(hwnd, 9);
        text_out(dc, in.right - S(hwnd, 12), in.top + S(hwnd, 8), cur.empty() ? "" : cur + fmt("  \xC2\xB7  line %d of %zu", sel + 1, shown.size()), MUTED, small, TA_RIGHT);
        if (shown.empty()) return;
        int lh = S(hwnd, 30), n = std::min(3, sel + 1);
        int y = in.bottom - S(hwnd, 18) - n * lh;
        for (int k = sel - n + 1; k <= sel; k++) {
            std::string t = all[shown[k]].text;
            HGDIOBJ o = SelectObject(dc, cf);
            std::wstring wt = W(t); SIZE sz; GetTextExtentPoint32W(dc, wt.c_str(), (int)wt.size(), &sz);
            SelectObject(dc, o);
            int x = (in.left + in.right - sz.cx) / 2;
            RECT b{x - S(hwnd, 8), y, x + sz.cx + S(hwnd, 8), y + lh - S(hwnd, 2)};
            fill(dc, b, RGB(0, 0, 0));
            text_out(dc, x, y + S(hwnd, 3), t, k == sel ? RGB(255, 255, 255) : RGB(0xb8, 0xb8, 0xb8), cf);
            y += lh;
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            std::string where;
            for (auto &li : R->lines) if (li.kind == "Captions" || li.kind.rfind("XDS", 0) == 0) where += (where.empty() ? "" : ", ") + fmt("line %d field %d", li.lineNo, li.field);
            hd.create(hwnd, "CC captions", where + "  \xC2\xB7  " + file_name(*R));
            hd.add(hwnd, 403, "Save .srt\xE2\x80\xA6", 110);
            hd.add(hwnd, 402, "Text report", 120);
            summary = label(hwnd, ""); set_label_colour(summary, ACCENT2);
            lst = listview(hwnd, 10, {{"#", -60}, {"Time", 110}, {"Service", 70}, {"Text", 600}});
            load();
            for (size_t i = 0; i < svcs.size(); i++) sbtn.push_back(button(hwnd, 500 + (int)i, svcs[i]));
            refill();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            int x = p;
            for (HWND b : sbtn) { MoveWindow(b, x, y, S(hwnd, 64), S(hwnd, 30), TRUE); x += S(hwnd, 70); }
            if (!sbtn.empty()) y += S(hwnd, 40);
            screen = {p, y, rc.right - p, y + S(hwnd, 170)}; y = screen.bottom + S(hwnd, 10);
            MoveWindow(summary, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 26);
            MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            ListView_Scroll(lst, 0, -(1 << 24));          // scrolled while the list was 10 px high: back to the top
            ListView_EnsureVisible(lst, sel, FALSE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == LVN_ITEMCHANGED) { int i = ListView_GetNextItem(lst, -1, LVNI_SELECTED); if (i >= 0 && i != sel) select(i, true); }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id >= 500 && id - 500 < (int)svcs.size()) { cur = svcs[id - 500]; refill(); }
            else if (id == 403 && !cur.empty()) {
                if (R->captionCues.empty()) { ask(hwnd, "CC captions", "These results were saved by an older version without caption times.\nDecode the recording again to save subtitles.", MB_ICONINFORMATION); return 0; }
                std::string out = save_file(hwnd, "Save " + cur + " as subtitles", stem_path(file_name(*R)) + "_" + cur + ".srt", "srt");
                if (!out.empty()) write_text(out, captionsSrt(*R, cur));
            }
            else if (id == 402) {
                std::string s; for (auto &x : captionsTimed(*R)) s += x + "\n";
                text_window(hwnd, "CC captions \xE2\x80\x94 " + file_name(*R), "every caption and text-mode line in the order received", s);
            }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ XDS
struct XdsWin : Window {
    ResultPtr R;
    struct Ev { std::string cls, key, val; };
    std::vector<Ev> ev;
    std::map<std::string, std::string> top;   // latest value per key
    Header hd; HWND lst;
    RECT card{}, clock{}, prog{};
    void load()
    {
        // "XDS Channel network:  PBS", "XDS Misc time of day 17:25 UTC, 5/3/1994"
        for (auto &l : R->xds) {
            if (l.rfind("XDS ", 0) != 0) continue;
            std::string s = l.substr(4);
            size_t sp = s.find(' ');
            Ev e; e.cls = s.substr(0, sp);
            std::string rest = sp == std::string::npos ? "" : s.substr(sp + 1);
            size_t c = rest.find(':');
            if (rest.rfind("time of day", 0) == 0) { e.key = "time of day"; e.val = strip(rest.substr(11)); }
            else if (rest.rfind("local time zone", 0) == 0) { e.key = "time zone"; e.val = strip(rest.substr(15)); }
            else if (c != std::string::npos) { e.key = strip(rest.substr(0, c)); e.val = strip(rest.substr(c + 1)); }
            else { e.key = rest; }
            ev.push_back(e);
            top[e.key] = e.val;
        }
    }
    COLORREF colour(const Ev &e) const
    {
        if (e.cls == "Channel") return C_CHAN;
        if (e.key == "time of day" || e.key == "time zone") return C_TIME;
        if (e.cls == "Current" || e.cls == "Future") return C_PROG;
        return C_OTHER;
    }
    std::string get(const char *k) const { auto it = top.find(k); return it == top.end() ? "" : it->second; }
    void paint(HDC dc, RECT)
    {
        HFONT huge = font(hwnd, 40, true), mid = font(hwnd, 14, true), small = font(hwnd, 9), smallb = font(hwnd, 10, true);
        HFONT led = font(hwnd, 38, true, L"Consolas"), ledm = font(hwnd, 13, true, L"Consolas");
        fill(dc, card, PANEL);
        std::string call = get("call letters"), net = get("network");
        if (call.empty()) call = get("station");
        text_out(dc, card.left + S(hwnd, 18), card.top + S(hwnd, 10), call.empty() ? (net.empty() ? "\xE2\x80\x94" : net) : call, FG, huge);
        if (!net.empty() && !call.empty()) {
            RECT nb{card.left + S(hwnd, 20), card.top + S(hwnd, 76), card.left + S(hwnd, 36) + (int)net.size() * S(hwnd, 12), card.top + S(hwnd, 104)};
            fill(dc, nb, C_CHAN); text_out(dc, nb.left + S(hwnd, 8), nb.top + S(hwnd, 3), net, RGB(255, 255, 255), smallb);
        }
        std::string tz = get("time zone");
        text_out(dc, card.left + S(hwnd, 20), card.bottom - S(hwnd, 26), tz.empty() ? "station data from XDS" : "time zone " + tz, MUTED, small);
        // the broadcast clock: the last time-of-day packet, local time by the zone sent
        fill(dc, clock, RGB(0x11, 0x11, 0x11));
        RECT in{clock.left + S(hwnd, 10), clock.top + S(hwnd, 10), clock.right - S(hwnd, 10), clock.bottom - S(hwnd, 10)}; fill(dc, in, RGB(0, 0, 0));
        int h = 0, mi = 0, mo = 0, d = 0, y = 0, z = 0;
        std::string t = get("time of day");
        if (sscanf(t.c_str(), "%d:%d UTC, %d/%d/%d", &h, &mi, &mo, &d, &y) == 5) {
            std::tm u{}; u.tm_year = y - 1900; u.tm_mon = mo - 1; u.tm_mday = d; u.tm_hour = h; u.tm_min = mi;
            time_t s = _mkgmtime(&u);
            bool local = sscanf(tz.c_str(), "UTC%d", &z) == 1;
            if (local) s += (time_t)z * 3600 + (tz.find("DST") != std::string::npos ? 3600 : 0);
            std::tm l; gmtime_s(&l, &s);
            text_out(dc, in.left + S(hwnd, 18), in.top + S(hwnd, 8), fmt("%02d:%02d", l.tm_hour, l.tm_min), C_TIME, led);
            text_out(dc, in.left + S(hwnd, 20), in.top + S(hwnd, 70), fmt("%s %d %s %d", DAYS[l.tm_wday], l.tm_mday, MONTHS[l.tm_mon], l.tm_year + 1900) + (local ? "  local" : "  UTC"), ACCENT2, ledm);
            if (local) text_out(dc, in.right - S(hwnd, 16), in.top + S(hwnd, 74), fmt("%02d:%02d UTC", h, mi), MUTED, small, TA_RIGHT);
        } else text_out(dc, in.left + S(hwnd, 18), in.top + S(hwnd, 20), "no time-of-day packets", MUTED, mid);
        text_out(dc, in.right - S(hwnd, 16), in.top + S(hwnd, 10), "last time packet in the recording", DIM, small, TA_RIGHT);
        // programme
        fill(dc, prog, PANEL);
        std::string pn = get("program name"); if (pn.empty()) pn = get("programme name");
        int yy = prog.top + S(hwnd, 10), x = prog.left + S(hwnd, 18);
        text_out(dc, x, yy, "On air", MUTED, small); yy += S(hwnd, 18);
        text_out(dc, x, yy, pn.empty() ? "the station sent no programme name" : pn, pn.empty() ? DIM : FG, mid); yy += S(hwnd, 30);
        std::string meta;
        for (auto &kv : top) if (kv.first.find("rating") != std::string::npos || kv.first.find("length") != std::string::npos || kv.first.find("type") != std::string::npos)
            meta += (meta.empty() ? "" : "   \xC2\xB7   ") + kv.first + " " + kv.second;
        if (!meta.empty()) text_out(dc, x, yy, meta, ACCENT2, small);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            load();
            hd.create(hwnd, "XDS", "extended data services \xC2\xB7 line 21 field 2 \xC2\xB7 " + file_name(*R));
            hd.add(hwnd, 402, "Text report", 120);
            lst = listview(hwnd, 10, {{"Class", 90}, {"Packet", 170}, {"Value", 460}, {"Times", -60}});
            // repeats folded: one row per packet and value, with how often it came
            std::vector<std::pair<Ev, int>> rows;
            std::map<std::string, size_t> at;
            for (auto &e : ev) {
                std::string k = e.cls + "|" + e.key + "|" + e.val;
                auto it = at.find(k);
                if (it == at.end()) { at[k] = rows.size(); rows.push_back({e, 1}); } else rows[it->second].second++;
            }
            for (size_t i = 0; i < rows.size(); i++) {
                lv_set(lst, (int)i, 0, rows[i].first.cls); lv_set(lst, (int)i, 1, rows[i].first.key);
                lv_set(lst, (int)i, 2, rows[i].first.val); lv_set(lst, (int)i, 3, fmt("%d", rows[i].second));
            }
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            int cw = std::max((int)S(hwnd, 300), (int)(rc.right - 2 * p) * 2 / 5);
            card = {p, y, p + cw, y + S(hwnd, 140)};
            clock = {card.right + S(hwnd, 10), y, rc.right - p, y + S(hwnd, 140)};
            y = card.bottom + S(hwnd, 10);
            prog = {p, y, rc.right - p, y + S(hwnd, 90)}; y = prog.bottom + S(hwnd, 10);
            MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: { auto *nh = (NMHDR *)l; if (nh->hwndFrom == lst && nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l); return 0; }
        case WM_COMMAND:
            if (LOWORD(w) == 402) { std::string s; for (auto &x : R->xds) s += x + "\n"; text_window(hwnd, "XDS \xE2\x80\x94 " + file_name(*R), "every XDS packet in the order received", s); }
            return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ Silent Radio LED sign
// The sign plays the items of one zone (A, B, C) frame by frame, as the
// receiver did: still pictures with holds and wipes, scrolling text.
struct SignWin : Window {
    ResultPtr R;
    Header hd; HWND zone_b, pbtn, prevb, nextb, cap, lst;
    std::vector<std::string> zones; int zi = 0;
    std::vector<int> items;           // indexes into R->silentRadio for this zone
    std::vector<uint16_t> state = std::vector<uint16_t>(SR_W, 0), prev = std::vector<uint16_t>(SR_W, 0);
    int it = 0, fr = 0; bool playing = true; double t0 = 0, el = 0;
    RECT sign{};
    static double now() { return GetTickCount64() / 1000.0; }
    const SilentRadioMessage &item() { return R->silentRadio[items[it]]; }
    void select()
    {
        items.clear();
        for (size_t i = 0; i < R->silentRadio.size(); i++)
            if (zones.empty() || R->silentRadio[i].zone == zones[zi]) items.push_back((int)i);
        if (!zones.empty()) set_text(zone_b, (zones[zi].empty() ? std::string("other") : "zone " + zones[zi]) + " \xE2\x96\xBE");
        ListView_DeleteAllItems(lst);
        double rate = frame_rate(*R);
        for (size_t i = 0; i < items.size(); i++) {
            auto &m = R->silentRadio[items[i]];
            lv_set(lst, (int)i, 0, rec_time(m.frame / rate));
            lv_set(lst, (int)i, 1, m.seq >= 0 ? fmt("%04X", m.seq) : "\xE2\x80\x94");
            lv_set(lst, (int)i, 2, m.copies > 1 ? fmt("%d", m.copies) : "");
            lv_set(lst, (int)i, 3, m.text);
        }
        go(0);
    }
    void go(int i, bool from_list = false)
    {
        int n = (int)items.size(); if (!n) return;
        it = ((i % n) + n) % n; fr = 0; t0 = now(); el = 0;
        if (!from_list) { ListView_SetItemState(lst, -1, 0, LVIS_SELECTED); ListView_SetItemState(lst, it, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(lst, it, FALSE); }
        std::string c;
        for (auto &t : item().texts) c += (c.empty() ? "" : "  \xC2\xB7  ") + t;
        set_text(cap, c);
    }
    void tick()
    {
        if (!playing || items.empty()) return;
        const SilentRadioMessage &m = item();
        if (m.frames.empty()) return;
        el = now() - t0;
        if (el >= m.frames[fr].duration()) {
            if (!m.frames[fr].scroll) prev = m.frames[fr].at(1e9, prev);
            fr++; t0 = now(); el = 0;
            if (fr >= (int)m.frames.size()) { go(it + 1); return; }
        }
        auto c = item().frames[fr].at(el, prev);
        if (c != state) { state = c; InvalidateRect(hwnd, &sign, FALSE); }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            std::string where;
            for (auto &li : R->lines) if (li.kind.rfind("Silent Radio", 0) == 0) where = fmt("line %d", li.lineNo);
            std::set<std::string> zs; int packets = 0;
            for (auto &x : R->silentRadio) { zs.insert(x.zone); packets += x.seq >= 0; }
            zones.assign(zs.begin(), zs.end());
            std::sort(zones.begin(), zones.end(), [](const std::string &a, const std::string &b) { return a.empty() != b.empty() ? !a.empty() : a < b; });
            hd.create(hwnd, "Silent Radio", "LED news sign \xC2\xB7 " + where + fmt(" \xC2\xB7 %d sign packets, %zu zones \xC2\xB7 ", packets, zones.size()) + file_name(*R));
            zone_b = hd.add(hwnd, 301, "zone", 100);
            prevb = hd.add(hwnd, 303, "\xE2\x8F\xAE", 40);
            pbtn = hd.add(hwnd, 302, "\xE2\x8F\xB8 Pause", 100);
            nextb = hd.add(hwnd, 304, "\xE2\x8F\xAD", 40);
            cap = label(hwnd, ""); set_label_colour(cap, ACCENT2);
            lst = listview(hwnd, 10, {{"Recording", 100}, {"Packet", 60}, {"Copies", 60}, {"Text", 900}});
            if (R->silentRadio.empty()) set_text(cap, "The sign carrier is there, but no messages were sent while recording.");
            SetTimer(hwnd, 1, 30, nullptr);
            select();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            int d = std::max(3, std::min(8, (int)(rc.right - 2 * p - S(hwnd, 20)) / SR_W));
            sign = {p, y, p + d * SR_W + S(hwnd, 20), y + d * SR_H + S(hwnd, 20)};
            y = sign.bottom + S(hwnd, 6);
            MoveWindow(cap, p, y, rc.right - 2 * p, S(hwnd, 40), TRUE); y += S(hwnd, 44);
            MoveWindow(lst, p, y, rc.right - 2 * p, rc.bottom - y - p, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_TIMER: tick(); return 0;
        case WM_PAINT:
            paint_buffered(hwnd, [this](HDC dc, RECT) {
                fill(dc, sign, RGB(0x11, 0x11, 0x11));
                int pad = S(hwnd, 10), d = (sign.right - sign.left - 2 * pad) / SR_W;
                RECT in{sign.left + pad, sign.top + pad, sign.left + pad + d * SR_W, sign.top + pad + d * SR_H}; fill(dc, in, RGB(0, 0, 0));
                HBRUSH on = CreateSolidBrush(RGB(0xff, 0xa3, 0x1a)), offb = CreateSolidBrush(RGB(0x2a, 0x16, 0x06));
                HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
                for (int x = 0; x < SR_W; x++)
                    for (int y = 0; y < SR_H; y++) {
                        SelectObject(dc, (state[x] >> y) & 1 ? on : offb);
                        Ellipse(dc, in.left + x * d + 1, in.top + y * d + 1, in.left + x * d + d, in.top + y * d + d);
                    }
                SelectObject(dc, op); DeleteObject(on); DeleteObject(offb);
            });
            return 0;
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
            return 0;
        }
        case WM_DESTROY: KillTimer(hwnd, 1); return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ AMOL (Nielsen)
struct AmolWin : Window {
    ResultPtr R;
    const AmolStream *A = nullptr;
    Header hd; HWND lst;
    RECT card{}, clock{}, strip{};
    int sel = -1;                     // selected stamp
    bool stamps = true;               // the list shows timestamps (else other payloads)
    const AmolStamp *cur() const { return sel >= 0 && sel < (int)A->stamps.size() ? &A->stamps[sel] : nullptr; }
    void select(int i) { sel = i; InvalidateRect(hwnd, nullptr, FALSE); }
    void paint(HDC dc, RECT)
    {
        HFONT huge = font(hwnd, 40, true), mid = font(hwnd, 14, true), small = font(hwnd, 9), smallb = font(hwnd, 10, true);
        HFONT led = font(hwnd, 38, true, L"Consolas"), ledm = font(hwnd, 13, true, L"Consolas"), mono = font(hwnd, 10, true, L"Consolas");
        const AmolStamp *t = cur();
        // source card
        fill(dc, card, PANEL);
        int sid = t ? t->sid : (!A->stamps.empty() ? A->stamps[0].sid : -1);
        text_out(dc, card.left + S(hwnd, 18), card.top + S(hwnd, 10), sid >= 0 ? fmt("SID %d", sid) : "\xE2\x80\x94", FG, huge);
        std::string net = sid >= 0 ? amolSourceName(sid) : "";
        if (!net.empty()) {
            RECT nb{card.left + S(hwnd, 20), card.top + S(hwnd, 76), card.left + S(hwnd, 36) + (int)net.size() * S(hwnd, 12), card.top + S(hwnd, 104)};
            fill(dc, nb, service_colour("AMOL")); text_out(dc, nb.left + S(hwnd, 8), nb.top + S(hwnd, 3), net, RGB(0, 0, 0), smallb);
            text_out(dc, nb.right + S(hwnd, 10), nb.top + S(hwnd, 5), "network feed (same ID on all its recordings)", MUTED, small);
        }
        text_out(dc, card.left + S(hwnd, 20), card.bottom - S(hwnd, 26),
                 fmt("%lld words \xC2\xB7 %lld with data \xC2\xB7 %lld parity errors \xC2\xB7 %zu seconds stamped", (long long)A->words,
                     (long long)(A->parityOk + A->parityBad), (long long)A->parityBad, A->stamps.size()), MUTED, small);
        // the timestamp as the station sent it
        fill(dc, clock, RGB(0x11, 0x11, 0x11));
        RECT in{clock.left + S(hwnd, 10), clock.top + S(hwnd, 10), clock.right - S(hwnd, 10), clock.bottom - S(hwnd, 10)}; fill(dc, in, RGB(0, 0, 0));
        if (t) {
            text_out(dc, in.left + S(hwnd, 18), in.top + S(hwnd, 8), fmt("%d:%02d:%02d", t->hour, t->minute, t->second), service_colour("AMOL"), led);
            text_out(dc, in.left + S(hwnd, 20), in.top + S(hwnd, 70), fmt("%s %d  %s", MONTHS[(t->month + 11) % 12], t->day, t->pm ? "PM" : "AM"), ACCENT2, ledm);
            text_out(dc, in.right - S(hwnd, 16), in.top + S(hwnd, 74), rec_time(t->frame / frame_rate(*R)) + " in the recording", MUTED, small, TA_RIGHT);
        } else text_out(dc, in.left + S(hwnd, 18), in.top + S(hwnd, 20), "no timestamps on this line", MUTED, mid);
        text_out(dc, in.right - S(hwnd, 16), in.top + S(hwnd, 10), "station clock, frame addresses 2/3", DIM, small, TA_RIGHT);
        // the 48 bits of the word, coloured by field
        fill(dc, strip, PANEL);
        struct F { const char *name; int len; COLORREF c; };
        static const F fields[] = {{"Ref", 3, RGB(0xf2, 0xe0, 0x3c)}, {"Start", 4, RGB(0xa8, 0x2a, 0x2a)}, {"Frame", 5, RGB(0x3a, 0x5b, 0xff)},
                                   {"Source", 6, RGB(0xff, 0x40, 0x40)}, {"Month", 4, RGB(0x3c, 0xcf, 0x3c)}, {"Day", 5, RGB(0xff, 0x3c, 0xf0)},
                                   {"Hour", 4, RGB(0x3c, 0xe8, 0xe8)}, {"Minute", 6, RGB(0xb0, 0x4c, 0xc8)}, {"Second", 6, RGB(0x2e, 0xa8, 0xe8)},
                                   {"PM", 1, RGB(0x2e, 0xa0, 0x6a)}, {"Spare", 3, RGB(0xa8, 0xa0, 0x20)}, {"P", 1, RGB(0x2e, 0x80, 0x50)}};
        uint64_t w = t ? t->bits : 0;
        int x0 = strip.left + S(hwnd, 16), cw = std::max(4, (int)(strip.right - strip.left - S(hwnd, 32)) / kAmolBits);
        int ty = strip.top + S(hwnd, 10), cy = ty + S(hwnd, 22), ch = S(hwnd, 22);
        int bit = 0;
        for (auto &f : fields) {
            int x = x0 + bit * cw, wpx = f.len * cw;
            text_out(dc, x + wpx / 2, ty, f.name, f.c, small, TA_CENTER);
            for (int k = 0; k < f.len; k++, bit++) {
                int v = t ? (int)((w >> (kAmolBits - 1 - bit)) & 1) : 0;
                RECT r{x0 + bit * cw + 1, cy, x0 + (bit + 1) * cw - 1, cy + ch};
                fill(dc, r, f.c);
                RECT inr{r.left + 2, r.top + 2, r.right - 2, r.bottom - 2};
                fill(dc, inr, v ? RGB(0xe8, 0xe8, 0xe8) : RGB(0x10, 0x10, 0x10));
                if (t) text_out(dc, (r.left + r.right) / 2, cy + ch + S(hwnd, 2), v ? "1" : "0", f.c, mono, TA_CENTER);
            }
            if (t) {
                int val = amolField(w, bit - f.len, f.len);
                std::string s = f.len >= 4 && strcmp(f.name, "Ref") && strcmp(f.name, "Start") && strcmp(f.name, "Spare") ? fmt("%d", val)
                              : !strcmp(f.name, "PM") ? (val ? "PM" : "AM") : "";
                if (!strcmp(f.name, "P")) s = amolParityOk(w) ? "ok" : "bad";
                text_out(dc, x + wpx / 2, cy + ch + S(hwnd, 20), s, f.c, smallb, TA_CENTER);
            }
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            for (auto &a : R->amol) if (!A || (A->stamps.empty() && !a.stamps.empty())) A = &a;
            static const AmolStream none;
            if (!A) A = &none;
            stamps = !A->stamps.empty();
            hd.create(hwnd, "AMOL", fmt("Nielsen automated measurement of lineups \xC2\xB7 line %d field %d \xC2\xB7 ", A->lineNo, A->field) + file_name(*R));
            hd.add(hwnd, 402, "Text report", 120);
            double fr = frame_rate(*R);
            if (stamps) {
                lst = listview(hwnd, 10, {{"Recording", 110}, {"Source", 90}, {"Date", 80}, {"Station time", 120}, {"Parity", -60}});
                for (size_t i = 0; i < A->stamps.size(); i++) {
                    auto &t = A->stamps[i];
                    std::string net = amolSourceName(t.sid);
                    lv_set(lst, (int)i, 0, rec_time(t.frame / fr));
                    lv_set(lst, (int)i, 1, fmt("%d", t.sid) + (net.empty() ? "" : " " + net));
                    lv_set(lst, (int)i, 2, fmt("%s %d", MONTHS[(t.month + 11) % 12], t.day));
                    lv_set(lst, (int)i, 3, fmt("%d:%02d:%02d %s", t.hour, t.minute, t.second, t.pm ? "PM" : "AM"));
                    lv_set(lst, (int)i, 4, t.parityOk ? "ok" : "copies agree");
                }
                sel = 0;
            } else {
                lst = listview(hwnd, 10, {{"Frame address", 110}, {"Payload (35 bits)", 160}, {"Times", 80}, {"First seen", -60}});
                for (size_t i = 0; i < A->other.size(); i++) {
                    auto &o = A->other[i];
                    lv_set(lst, (int)i, 0, fmt("%d", o.fa)); lv_set(lst, (int)i, 1, fmt("%09llx", (unsigned long long)o.payload));
                    lv_set(lst, (int)i, 2, fmt("%d", o.count)); lv_set(lst, (int)i, 3, rec_time(o.firstFrame / fr));
                }
            }
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            int cw = std::max((int)S(hwnd, 300), (int)(rc.right - 2 * p) * 1 / 2);
            card = {p, y, p + cw, y + S(hwnd, 140)};
            clock = {card.right + S(hwnd, 10), y, rc.right - p, y + S(hwnd, 140)};
            y = card.bottom + S(hwnd, 10);
            strip = {p, y, rc.right - p, y + S(hwnd, 100)}; y = strip.bottom + S(hwnd, 10);
            MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW)
                    return lv_custom_draw(l, [this](int row) { return stamps && row < (int)A->stamps.size() && !A->stamps[row].parityOk ? ACCENT : FG; });
                if (nh->code == LVN_ITEMCHANGED && stamps) { int i = ListView_GetNextItem(lst, -1, LVNI_SELECTED); if (i >= 0 && i != sel) select(i); }
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == 402) {
                std::string s; for (auto &a : R->amol) for (auto &x : a.report) s += x + "\n";
                s += "\n";
                for (auto &a : R->amol) for (auto &t : a.stamps) s += rec_time(t.frame / frame_rate(*R)) + "  " + t.text() + (t.parityOk ? "" : "  (parity error, copies agree)") + "\n";
                text_window(hwnd, "AMOL \xE2\x80\x94 " + file_name(*R), "Nielsen AMOL I words, timestamps and other frame addresses", s);
            }
            return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ Gemstar (VCR Plus+ / GUIDE Plus+)
// the programme guide of the Type B Show Information messages, drawn like the StarSight grid;
// a recording without programmes gets the packet list instead
const COLORREF C_MOVIE = RGB(0x8e, 0x5c, 0xd9), C_KIDS = RGB(0x2e, 0xa0, 0x6a), C_SHOW = RGB(0x3a, 0x7b, 0xd5);

int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int &y, int &m, int &d, int &wd)
{
    wd = (int)(((z % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1); m = (int)(mp < 10 ? mp + 3 : mp - 9); y = (int)(yoe + era * 400) + (m <= 2);
}

bool is_rating(const std::string &t)
{
    static const char *base[] = {"G", "PG", "PG13", "R", "NC17", "NR", "X", "TVY", "TVY7", "TVG", "TVPG", "TV14", "TVMA"};
    size_t dash = t.find('-');
    std::string b = t.substr(0, dash);
    if (dash != std::string::npos) {
        std::string suf = t.substr(dash + 1);
        if (suf.empty()) return false;
        for (char c : suf) if (c < 'A' || c > 'Z') return false;
    }
    for (auto *x : base) if (b == x) return true;
    return false;
}

std::string trim_sp(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

struct GemShow {
    int ch = 0, lineNo = 0, field = 1;
    int64_t t = 0, dur = 0;           // local minutes since 1970
    std::string title, category, year, rating, cast, desc;
    int stars = 0;
    int64_t code = -1, frame = 0;
    bool movie() const { return !year.empty(); }
    bool kids() const { return rating.rfind("TVY", 0) == 0; }
    COLORREF colour() const { return movie() ? C_MOVIE : kids() ? C_KIDS : C_SHOW; }
};

// title + the two text lines of a Show Information record -> full title, the movie line, the description
GemShow gem_show(const GemstarShow &s)
{
    GemShow g;
    g.title = s.title;
    std::vector<std::string> lines;
    for (auto *src : {&s.line1, &s.line2})
        for (auto &l : split(*src, '\n')) { std::string t = trim_sp(l); if (!t.empty() && t != "&") lines.push_back(t); }
    // a long title goes on in a text line: "Roman Spring of..." + "...Mrs. Stone"
    if (g.title.size() > 3 && g.title.compare(g.title.size() - 3, 3, "...") == 0)
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].rfind("...", 0) == 0) { g.title = trim_sp(g.title.substr(0, g.title.size() - 3)) + " " + trim_sp(lines[i].substr(3)); lines.erase(lines.begin() + i); break; }
    for (size_t i = 0; i < lines.size(); i++) {
        // the movie line: "Comedy-drama 1992 ** Steve Martin, Debra Winger, PG"; or a rating alone ("TVY")
        std::string l = lines[i], rating;
        size_t c = l.rfind(", ");
        std::string last = trim_sp(c == std::string::npos ? l : l.substr(c + 2));
        if (is_rating(last)) rating = last, l = c == std::string::npos ? "" : l.substr(0, c);
        auto tok = split(l, ' ');
        int yi = -1;
        for (int k = 0; k < (int)tok.size() && k < 4; k++) {
            const std::string &v = tok[k];
            if (v.size() == 4 && (v[0] == '1' || v[0] == '2') && isdigit((uint8_t)v[1]) && isdigit((uint8_t)v[2]) && isdigit((uint8_t)v[3])) { yi = k; break; }
        }
        if (yi < 0 && (rating.empty() || !l.empty())) continue;   // prose
        g.rating = rating;
        if (yi >= 0) {
            for (int k = 0; k < yi; k++) g.category += (k ? " " : "") + tok[k];
            g.year = tok[yi];
            size_t k = yi + 1;
            if (k < tok.size() && !tok[k].empty() && tok[k].find_first_not_of('*') == std::string::npos) g.stars = (int)tok[k++].size();
            for (; k < tok.size(); k++) g.cast += (g.cast.empty() ? "" : " ") + tok[k];
            while (!g.cast.empty() && (g.cast.back() == ',' || g.cast.back() == ' ')) g.cast.pop_back();
        }
        lines.erase(lines.begin() + i);
        break;
    }
    for (auto &l : lines) g.desc += (g.desc.empty() ? "" : "\n") + l;
    g.ch = s.channel; g.dur = s.duration; g.code = s.code; g.frame = s.frame;
    return g;
}

struct GemWin : Window {
    ResultPtr R;
    std::vector<GemShow> shows;
    std::map<int, std::vector<int>> rows;   // channel -> shows by start
    std::vector<int> all, chans;            // every channel, the rows shown
    bool busy = true;                       // only channels with a programme in the grid
    std::string keyNote;                    // why the Type A payload stays scrambled
    std::vector<int64_t> dayList;           // local days that have programmes
    GemstarClock clk;
    int zone = 5, span = 240, top = 0, sel = 0;
    int64_t view = 0, sel_t = 0;
    Header hd; HWND lst = nullptr, spanBtn = nullptr, busyBtn = nullptr;
    RECT card{}, days{}, ruler{}, grid{}, info{};
    int rowh = 32, chw = 100;

    static std::string tlabel(int64_t t) { int h = (int)(t / 60 % 24), m = (int)(t % 60); return fmt("%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM"); }
    static std::string dlabel(int64_t t) { int y, m, d, wd; civil_from_days(t / 1440, y, m, d, wd); return fmt("%s %d %s %d", DAYS[wd], d, MONTHS[m - 1], y); }
    static std::string day_prefix(int64_t t, int64_t ref) { int y, m, d, wd; civil_from_days(t / 1440, y, m, d, wd); return t / 1440 != ref / 1440 ? std::string(DAYS[wd]) + " " : ""; }
    void build()
    {
        for (auto &s : R->gemstar)
            for (auto &p : s.packets) { GemstarClock c = gemstarClock(p); if (c.valid && !clk.valid) clk = c; }
        if (clk.valid && clk.zone >= 3 && clk.zone <= 11) zone = clk.zone;
        for (auto &s : R->gemstar)
            for (auto &x : s.shows) {
                GemShow g = gem_show(x);
                g.t = days_from_civil(x.year, x.month, x.day) * 1440 + x.start - zone * 60;
                g.lineNo = s.lineNo; g.field = s.field;
                bool dup = false;
                for (auto &o : shows) dup |= o.ch == g.ch && o.t == g.t && o.title == g.title;
                if (!dup) shows.push_back(g);
            }
        for (int i = 0; i < (int)shows.size(); i++) rows[shows[i].ch].push_back(i);
        std::set<int64_t> ds;
        for (auto &kv : rows) {
            std::sort(kv.second.begin(), kv.second.end(), [this](int a, int b) { return shows[a].t < shows[b].t; });
            all.push_back(kv.first);
            for (int i : kv.second) ds.insert(shows[i].t / 1440);
        }
        dayList.assign(ds.begin(), ds.end());
        chans = all;
        home();
    }
    // the stretch of the guide with the most programmes in it
    void home()
    {
        if (shows.empty()) return;
        int best = -1;
        for (auto &s : shows) {
            int64_t v = s.t / 30 * 30;
            int n = 0;
            for (auto &o : shows) n += o.t < v + span && o.t + o.dur > v;
            if (n > best) best = n, view = v;
        }
        sel_t = view;
        sel = 0; top = 0;
        chans.clear();
        refresh();
        if (!at(chans[sel], sel_t)) for (int i : rows[chans[sel]]) if (shows[i].t >= view) { sel_t = shows[i].t; break; }
    }
    // the rows for the grid as it is now; the selected channel stays selected when it is still there
    void refresh()
    {
        int cur = sel < (int)chans.size() ? chans[sel] : -1;
        chans.clear();
        for (int ch : all) if (!busy || row_has_view(ch)) chans.push_back(ch);
        if (chans.empty()) chans = all;
        auto it = std::find(chans.begin(), chans.end(), cur);
        sel = 0;
        if (it != chans.end()) sel = (int)(it - chans.begin());
        else for (size_t i = 0; i < chans.size(); i++) if (at(chans[i], sel_t)) { sel = (int)i; break; }
        top = std::max(0, std::min(top, std::max(0, (int)chans.size() - rows_visible())));
    }
    const GemShow *at(int ch, int64_t t) const
    {
        auto it = rows.find(ch);
        if (it == rows.end()) return nullptr;
        for (int i : it->second) if (t >= shows[i].t && t < shows[i].t + shows[i].dur) return &shows[i];
        return nullptr;
    }
    const GemShow *selected() const { return chans.empty() ? nullptr : at(chans[sel], sel_t); }
    int rows_visible() const { return std::max(1, (int)(grid.bottom - grid.top) / std::max(1, rowh)); }
    int x_of(int64_t t) const { int x0 = grid.left + chw; return x0 + (int)((t - view) * (grid.right - x0) / span); }
    bool row_has_view(int ch) const
    {
        for (int i : rows.at(ch)) if (shows[i].t < view + span && shows[i].t + shows[i].dur > view) return true;
        return false;
    }
    // the programmes an empty row points at: the next one after the grid, the last one before it
    void neighbours(int ch, const GemShow *&nx, const GemShow *&pv) const
    {
        nx = pv = nullptr;
        for (int i : rows.at(ch)) { if (shows[i].t >= view + span && !nx) nx = &shows[i]; if (shows[i].t + shows[i].dur <= view) pv = &shows[i]; }
    }
    void fit()
    {
        while (sel_t >= view + span) view += 30;
        while (sel_t < view) view -= 30;
        refresh();
        if (sel < top) top = sel;
        if (sel >= top + rows_visible()) top = sel - rows_visible() + 1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void move(int dr, int dt)
    {
        if (chans.empty()) return;
        if (dr) sel = std::max(0, std::min((int)chans.size() - 1, sel + dr));
        if (dt) {   // the next / previous programme on this channel, wherever it is
            auto &v = rows[chans[sel]];
            const GemShow *cur = selected();
            int64_t ref = cur ? cur->t : sel_t;
            if (dt > 0) { for (int i : v) if (shows[i].t > ref) { sel_t = shows[i].t; break; } }
            else for (auto it = v.rbegin(); it != v.rend(); ++it) if (shows[*it].t < ref) { sel_t = shows[*it].t; break; }
        }
        fit();
    }
    void set_span(int s)
    {
        int64_t mid = view + span / 2;
        span = s;
        view = (mid - span / 2) / 30 * 30;
        set_text(spanBtn, fmt("%d hours", span / 60));
        InvalidateRect(spanBtn, nullptr, TRUE);
        fit();
    }
    // a coloured pill with text; returns where the next one goes
    int pill(HDC dc, int x, int y, const std::string &s, COLORREF bg, COLORREF fg, HFONT f)
    {
        SIZE sz; HGDIOBJ of = SelectObject(dc, f); std::wstring w = W(s); GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz); SelectObject(dc, of);
        RECT r{x, y, x + sz.cx + S(hwnd, 16), y + sz.cy + S(hwnd, 6)};
        fill(dc, r, bg);
        draw_text(dc, r, s, fg, f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return r.right + S(hwnd, 8);
    }
    void paint(HDC dc, RECT)
    {
        HFONT big = font(hwnd, 26, true), smallb = font(hwnd, 10, true), small = font(hwnd, 9), norm = font(hwnd, 10), mid = font(hwnd, 15, true);
        COLORREF gem = service_colour("Gemstar");
        // card
        fill(dc, card, PANEL);
        RECT stripe{card.left, card.top, card.left + S(hwnd, 5), card.bottom}; fill(dc, stripe, gem);
        int x = card.left + S(hwnd, 20);
        text_out(dc, x, card.top + S(hwnd, 8), "GUIDE Plus+", gem, big);
        static const char *WD[] = {"?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::string c = clk.valid ? fmt("broadcast clock %s %d %s %d  %d:%02d  (zone %d)", WD[clk.weekday & 7], clk.day, MONTHS[clk.month - 1], clk.year, clk.hour, clk.minute, clk.zone)
                                  : "no clock packet in this recording";
        text_out(dc, x, card.bottom - S(hwnd, 44), c, ACCENT2, small);
        text_out(dc, x, card.bottom - S(hwnd, 24), !shows.empty() ? fmt("Gemstar VCR Plus+ / GUIDE Plus+; listings from the Type B Show Information messages; times UTC\xE2\x88\x92%d", zone)
                                                   : !keyNote.empty() ? keyNote : std::string("Gemstar VCR Plus+ / GUIDE Plus+ packets; no Show Information messages to build a guide from"), DIM, small);
        int64_t words = 0, pk = 0, blocks = 0, good = 0, ta = 0;
        for (auto &s : R->gemstar) {
            words += s.words2x + s.words1x, pk += s.packets.size(), blocks += s.blocks, good += s.blocksGood;
            for (auto &p : s.packets) ta += gemstarTypeA(p).ok;
        }
        std::vector<std::pair<std::string, std::string>> t = {{fmt("%lld", (long long)words), "words"}, {fmt("%lld", (long long)pk), "packets"}};
        if (ta) t.push_back({fmt("%lld", (long long)ta), "Type A"});
        if (blocks) t.push_back({fmt("%lld/%lld", (long long)good, (long long)blocks), "Type B blocks ok"});
        if (!shows.empty()) t.push_back({fmt("%zu", shows.size()), "programmes"}), t.push_back({fmt("%zu", all.size()), "channels"});
        tiles(hwnd, dc, card, t);
        if (shows.empty()) return;
        // days that have programmes
        int dw = std::min<int>(S(hwnd, 130), (days.right - days.left - chw) / std::max<int>(1, (int)dayList.size()));
        text_out(dc, days.left, days.top + S(hwnd, 6), dlabel(view).substr(dlabel(view).find(' ') + 1), FG, smallb);
        for (size_t i = 0; i < dayList.size(); i++) {
            RECT r{days.left + chw + (int)i * dw + 1, days.top, days.left + chw + (int)(i + 1) * dw - 2, days.bottom};
            int y, m, d, wd; civil_from_days(dayList[i], y, m, d, wd);
            bool cur = dayList[i] == view / 1440;
            fill(dc, r, cur ? ACCENT : FIELD);
            draw_text(dc, r, fmt("%s %d %s", DAYS[wd], d, MONTHS[m - 1]), cur ? BG : FG, cur ? smallb : norm, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // legend, right of the days
        int lx = days.right;
        for (auto &kv : std::vector<std::pair<std::string, COLORREF>>{{"other", C_SHOW}, {"children", C_KIDS}, {"movie", C_MOVIE}}) {
            HGDIOBJ of = SelectObject(dc, small); SIZE sz; std::wstring w = W(kv.first); GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz); SelectObject(dc, of);
            lx -= sz.cx;
            text_out(dc, lx, days.top + S(hwnd, 7), kv.first, MUTED, small);
            RECT sw{lx - S(hwnd, 16), days.top + S(hwnd, 11), lx - S(hwnd, 5), days.top + S(hwnd, 19)}; fill(dc, sw, kv.second);
            lx = sw.left - S(hwnd, 14);
        }
        // time ruler
        fill(dc, ruler, FIELD);
        int step = span <= 240 ? 30 : 60;
        for (int64_t tt = view; tt < view + span; tt += step) {
            int xx = x_of(tt);
            if (tt != view) { RECT ln{xx, ruler.top + S(hwnd, 4), xx + 1, ruler.bottom - S(hwnd, 4)}; fill(dc, ln, LINE); }
            text_out(dc, xx + S(hwnd, 6), ruler.top + S(hwnd, 5), tt % 1440 == 0 ? dlabel(tt).substr(0, 3) + " " + tlabel(tt) : tlabel(tt), tt % 1440 == 0 ? ACCENT2 : MUTED, norm);
        }
        // grid
        fill(dc, grid, PANEL);
        int vis = rows_visible();
        for (int r = 0; r < vis && top + r < (int)chans.size(); r++) {
            int idx = top + r, ch = chans[idx];
            bool selrow = idx == sel;
            RECT row{grid.left, grid.top + r * rowh, grid.right, grid.top + (r + 1) * rowh};
            RECT cc{row.left, row.top + 1, row.left + chw - S(hwnd, 6), row.bottom - 1};
            fill(dc, cc, selrow ? LINE : FIELD);
            RECT bar{cc.left, cc.top, cc.left + S(hwnd, 4), cc.bottom}; fill(dc, bar, gem);
            RECT ct{cc.left + S(hwnd, 12), cc.top, cc.right - S(hwnd, 10), cc.bottom};
            draw_text(dc, ct, "CH", DIM, small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, ct, fmt("%d", ch), FG, mid, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            for (int i : rows[ch]) {
                auto &s = shows[i];
                if (s.t + s.dur <= view || s.t >= view + span) continue;
                int x0 = std::max(x_of(s.t), (int)(grid.left + chw)), x1 = std::min(x_of(s.t + s.dur), (int)grid.right);
                RECT b{x0 + 1, row.top + 1, x1 - 1, row.bottom - 1};
                bool is_sel = selrow && sel_t >= s.t && sel_t < s.t + s.dur;
                fill(dc, b, is_sel ? ACCENT : FIELD);
                RECT kb{b.left, b.top, b.left + S(hwnd, 3), b.bottom}; fill(dc, kb, s.colour());
                std::string tt = (s.t < view ? "\xE2\x97\x80 " : "") + s.title;
                RECT tr{b.left + S(hwnd, 10), b.top, b.right - S(hwnd, 4), b.bottom};
                draw_text(dc, tr, tt, is_sel ? BG : FG, is_sel ? smallb : norm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (!row_has_view(ch)) {   // where this channel's programmes are
                RECT er{grid.left + chw + S(hwnd, 10), row.top, grid.right - S(hwnd, 10), row.bottom};
                const GemShow *nx, *pv; neighbours(ch, nx, pv);
                if (nx) draw_text(dc, er, day_prefix(nx->t, view) + tlabel(nx->t) + "   " + nx->title + "  \xE2\x96\xB6", DIM, small, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (pv) draw_text(dc, er, "\xE2\x97\x80  " + pv->title + "   " + day_prefix(pv->t, view) + tlabel(pv->t), DIM, small, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            RECT ln{grid.left + chw, row.bottom - 1, grid.right, row.bottom}; fill(dc, ln, BG);
        }
        // the selected programme
        fill(dc, info, PANEL);
        const GemShow *s = selected();
        int ix = info.left + S(hwnd, 20), iy = info.top + S(hwnd, 12);
        if (!s) { text_out(dc, ix, iy, fmt("nothing at this time on channel %d  \xE2\x80\x94  \xE2\x86\x90 \xE2\x86\x92 go to its programmes", chans[sel]), MUTED, norm); return; }
        RECT kb{info.left, info.top, info.left + S(hwnd, 5), info.bottom}; fill(dc, kb, s->colour());
        text_out(dc, ix, iy, s->title, FG, mid); iy += S(hwnd, 32);
        text_out(dc, ix, iy, tlabel(s->t) + " \xE2\x80\x93 " + tlabel(s->t + s->dur) + "   \xC2\xB7   " + dlabel(s->t) + "   \xC2\xB7   " + fmt("channel %d   \xC2\xB7   %lld min", s->ch, (long long)s->dur), ACCENT2, norm);
        iy += S(hwnd, 26);
        int px = ix;
        if (!s->category.empty()) px = pill(dc, px, iy, s->category, s->colour(), RGB(0xff, 0xff, 0xff), smallb);
        if (!s->year.empty()) px = pill(dc, px, iy, s->year, FIELD, FG, smallb);
        if (s->stars) { std::string st; for (int k = 0; k < 4; k++) st += k < s->stars ? "\xE2\x98\x85" : "\xE2\x98\x86"; px = pill(dc, px, iy, st, FIELD, C_TIME, smallb); }
        if (!s->rating.empty()) px = pill(dc, px, iy, s->rating, RGB(0xe8, 0xe8, 0xe8), RGB(0x10, 0x10, 0x10), smallb);
        if (!s->cast.empty()) text_out(dc, px + S(hwnd, 4), iy + S(hwnd, 3), "with " + s->cast, MUTED, norm);
        if (px != ix || !s->cast.empty()) iy += S(hwnd, 34);
        RECT dr{ix, iy, info.right - S(hwnd, 20), info.bottom - S(hwnd, 30)};
        draw_text(dc, dr, s->desc.empty() ? "No description was sent for this programme." : s->desc, s->desc.empty() ? DIM : FG, norm, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        std::string foot = (s->code >= 0 ? fmt("programme No. %lld   \xC2\xB7   ", (long long)s->code) : std::string()) +
                           fmt("received at %s, line %d field %d", rec_time(s->frame / frame_rate(*R)).c_str(), s->lineNo, s->field);
        text_out(dc, ix, info.bottom - S(hwnd, 24), foot, DIM, small);
    }
    std::vector<COLORREF> rowColour;
    void fill_list()
    {
        double fr = frame_rate(*R);
        int i = 0;
        auto row = [&](const std::string &t, const std::string &where, const std::string &kind, const std::string &block, const std::string &host,
                       const std::string &content, int copies, COLORREF c) {
            lv_set(lst, i, 0, t); lv_set(lst, i, 1, where); lv_set(lst, i, 2, kind); lv_set(lst, i, 3, block); lv_set(lst, i, 4, host);
            lv_set(lst, i, 5, content); lv_set(lst, i, 6, copies > 1 ? fmt("%d", copies) : "");
            rowColour.push_back(c); i++;
        };
        for (auto &s : R->gemstar) {
            std::string where = fmt("%d / %d", s.lineNo, s.field);
            for (auto &p : s.packets) {
                GemstarTypeA h = gemstarTypeA(p);
                GemstarClock c = gemstarClock(p);
                if (c.valid) row(rec_time(p.frame / fr), where, "G-Clock", "", "", p.text().substr(7), p.copies, C_TIME);
                else if (h.ok) {
                    std::string hex;
                    for (size_t k = 0; k < h.payload.size() && k < 40; k++) hex += fmt("%s%02X", k ? " " : "", h.payload[k]);
                    if (h.payload.size() > 40) hex += " \xE2\x80\xA6";
                    row(rec_time(p.frame / fr), where, h.name, fmt("%d / %d", h.block, h.total), fmt("%04X", h.host),
                        fmt("v%02X  ", h.version) + hex, p.copies, FG);
                } else row(rec_time(p.frame / fr), where, fmt("type %02X", p.type), "", "", p.text(), p.copies, FG);
            }
            // Type B: what gemstarGuide read from the download blocks
            bool typeB = false;
            for (auto &l : s.guide) {
                std::string t = trim_sp(l);
                if (t.rfind("payload scrambled", 0) == 0) { if (keyNote.empty()) keyNote = "Type A " + t; continue; }
                if (t.rfind("Type B", 0) == 0) typeB = true;
                if (!typeB || t.empty() || t == "Other messages:") continue;
                row("", where, "Type B", "", "", t, 1, ACCENT2);
            }
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            build();
            hd.create(hwnd, "Gemstar", (shows.empty() ? std::string("VCR Plus+ / GUIDE Plus+ data") : fmt("programme guide \xC2\xB7 %zu programmes on %zu channels", shows.size(), all.size())) + " \xC2\xB7 " + file_name(*R));
            if (!shows.empty()) { hd.add(hwnd, 401, "Home", 80); spanBtn = hd.add(hwnd, 404, fmt("%d hours", span / 60), 100); busyBtn = hd.add(hwnd, 405, "All channels", 120); }
            hd.add(hwnd, 403, "Text report", 120);
            if (shows.empty()) {
                lst = listview(hwnd, 10, {{"Recording", 96}, {"Line / field", 84}, {"Packet", 190}, {"Block", 64}, {"HostID", 64}, {"Payload (scrambled) / content", 560}, {"Copies", -56}});
                fill_list();
            }
            return 0;
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            rowh = S(hwnd, 32); chw = S(hwnd, 100);
            card = {p, y, rc.right - p, y + S(hwnd, 110)}; y = card.bottom + S(hwnd, 10);
            if (lst) MoveWindow(lst, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            days = {p, y, rc.right - p, y + S(hwnd, 28)}; y = days.bottom + S(hwnd, 6);
            ruler = {p, y, rc.right - p, y + S(hwnd, 26)}; y = ruler.bottom;
            int ih = S(hwnd, 200);
            grid = {p, y, rc.right - p, std::max<LONG>(y + rowh, rc.bottom - p - ih - S(hwnd, 10))};
            info = {p, grid.bottom + S(hwnd, 10), rc.right - p, rc.bottom - p};
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (shows.empty()) return 0;
            switch (w) {
            case VK_UP: move(-1, 0); break; case VK_DOWN: move(1, 0); break;
            case VK_LEFT: move(0, -1); break; case VK_RIGHT: move(0, 1); break;
            case VK_PRIOR: move(-rows_visible(), 0); break; case VK_NEXT: move(rows_visible(), 0); break;
            case VK_TAB: { int d = GetKeyState(VK_SHIFT) < 0 ? -1440 : 1440; view += d; sel_t += d; fit(); break; }
            case VK_HOME: home(); InvalidateRect(hwnd, nullptr, FALSE); break;
            case VK_ADD: case VK_OEM_PLUS: if (span > 120) set_span(span / 2); break;
            case VK_SUBTRACT: case VK_OEM_MINUS: if (span < 480) set_span(span * 2); break;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (shows.empty()) break;
            if (GetKeyState(VK_SHIFT) < 0) { view += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -30 : 30; refresh(); InvalidateRect(hwnd, nullptr, FALSE); }
            else move(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3, 0);
            return 0;
        case WM_LBUTTONDOWN: {
            if (shows.empty()) return 0;
            SetFocus(hwnd);
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&grid, pt) && pt.x > grid.left + chw) {
                int r = (pt.y - grid.top) / rowh;
                if (top + r < (int)chans.size()) {
                    sel = top + r;
                    int ch = chans[sel];
                    sel_t = view + (int64_t)(pt.x - grid.left - chw) * span / (grid.right - grid.left - chw);
                    if (!row_has_view(ch)) {   // an empty row: go to the programme its hint points at
                        const GemShow *nx, *pv; neighbours(ch, nx, pv);
                        bool right = pt.x > (grid.left + chw + grid.right) / 2;
                        const GemShow *go = right ? (nx ? nx : pv) : (pv ? pv : nx);
                        if (go) { sel_t = go->t; view = go->t / 30 * 30 - 30; }
                    }
                }
            } else if (PtInRect(&days, pt) && pt.x > days.left + chw) {
                int dw = std::min<int>(S(hwnd, 130), (days.right - days.left - chw) / std::max<int>(1, (int)dayList.size()));
                size_t i = (pt.x - days.left - chw) / std::max(1, dw);
                if (i < dayList.size()) {
                    // the busiest stretch of that day
                    int best = -1;
                    for (auto &s : shows) {
                        if (s.t / 1440 != dayList[i]) continue;
                        int64_t v = s.t / 30 * 30; int n = 0;
                        for (auto &o : shows) n += o.t < v + span && o.t + o.dur > v;
                        if (n > best) best = n, view = v;
                    }
                    sel_t = view;
                    for (size_t k = 0; k < chans.size(); k++) if (at(chans[k], sel_t)) { sel = (int)k; break; }
                }
            }
            fit();
            return 0;
        }
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (lst && nh->hwndFrom == lst && nh->code == NM_CUSTOMDRAW)
                return lv_custom_draw(l, [this](int row) { return row >= 0 && row < (int)rowColour.size() ? rowColour[row] : FG; });
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 401) { home(); InvalidateRect(hwnd, nullptr, FALSE); SetFocus(hwnd); }
            else if (id == 404) { set_span(span >= 480 ? 120 : span * 2); SetFocus(hwnd); }
            else if (id == 405) { busy = !busy; set_text(busyBtn, busy ? "All channels" : "Busy channels"); InvalidateRect(busyBtn, nullptr, TRUE); fit(); SetFocus(hwnd); }
            else if (id == 403) text_window(hwnd, "Gemstar \xE2\x80\x94 " + file_name(*R), "Type A packets, Type B guide data decrypted as Gemstar's own monitor program FREQCCD_ does it, then the raw words", gemstar_text(*R));
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ Japanese teletext (JTES)
// the page list, the page as the TU-TX100 tuner draws it, its text; a page with a tune plays it when shown
struct JtesWin : Window {
    ResultPtr R;
    Header hd; HWND lst = nullptr, txt = nullptr, soundBtn = nullptr;
    RECT pic{};
    int sel = -1;
    bool sound = true;
    JtesImage img;
    std::vector<JtesFrame> frames;     // an animated page (TIME codes): played on a timer, over and over
    size_t frame = 0;
    std::vector<uint8_t> wav;          // kept alive while PlaySound plays it
    void showFrame(size_t k)
    {
        KillTimer(hwnd, 7);
        if (frames.empty()) return;
        frame = k % frames.size();
        img = frames[frame].img;
        InvalidateRect(hwnd, &pic, FALSE);
        double s = frames[frame].seconds;
        if (s <= 0) s = 3;                 // the last screen stays a while, then the animation starts again
        SetTimer(hwnd, 7, (UINT)(s * 1000), nullptr);
    }
    const JtesPage *cur() const { return sel >= 0 && sel < (int)R->jtes.pages.size() ? &R->jtes.pages[sel] : nullptr; }
    void play()
    {
        PlaySoundW(nullptr, nullptr, 0);
        wav.clear();
        const JtesPage *p = cur();
        if (!p) return;
        auto tune = jtesTune(*p);
        if (tune.empty()) return;
        wav = jtesWave(tune);
        PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    void select(int i)
    {
        sel = i;
        const JtesPage *p = cur();
        if (!p) return;
        img = jtesRender(*p);
        frames = jtesFrames(*p);
        showFrame(0);
        set_text(txt, replace_all(jtesText(*p), "\n", "\r\n"));
        InvalidateRect(hwnd, &pic, FALSE);
        if (sound) play();
        else PlaySoundW(nullptr, nullptr, 0);
    }
    void paint(HDC dc, RECT)
    {
        fill(dc, pic, RGB(0x11, 0x11, 0x11));
        if (img.w <= 0) return;
        // the tuner's dots are a little wider than tall on a 4:3 screen (248 x 204 page)
        int aw = pic.right - pic.left - S(hwnd, 16), ah = pic.bottom - pic.top - S(hwnd, 16);
        double ar = img.w * 1.1 / img.h;
        int w = std::min(aw, (int)(ah * ar)), h = (int)(w / ar);
        int x = pic.left + (pic.right - pic.left - w) / 2, y = pic.top + (pic.bottom - pic.top - h) / 2;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = img.w; bi.bmiHeader.biHeight = -img.h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, w, h, 0, 0, img.w, img.h, img.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            const JtesResult &J = R->jtes;
            std::set<int> progs;
            int tunes = 0;
            for (auto &p : J.pages) { progs.insert(p.programme); tunes += !jtesTune(p).empty(); }
            hd.create(hwnd, "Japanese teletext", fmt("\xE6\x96\x87\xE5\xAD\x97\xE5\xA4\x9A\xE9\x87\x8D\xE6\x94\xBE\xE9\x80\x81 (JTES, BT.653 system D) \xC2\xB7 %zu pages in %zu programmes \xC2\xB7 %d with a tune \xC2\xB7 ", J.pages.size(), progs.size(), tunes) + file_name(*R));
            soundBtn = hd.add(hwnd, 405, "Sound: on", 110);
            hd.add(hwnd, 406, "Play", 70);
            hd.add(hwnd, 402, "Export\xE2\x80\xA6", 100);
            hd.add(hwnd, 403, "Text report", 110);
            lst = listview(hwnd, 10, {{"Page", 64}, {"\xE2\x99\xAA", 28}, {"Title", -60}});
            HFONT jf = CreateFontW(-S(hwnd, 14), 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"MS Gothic");
            SendMessageW(lst, WM_SETFONT, (WPARAM)jf, TRUE);
            for (size_t i = 0; i < J.pages.size(); i++) {
                auto &p = J.pages[i];
                lv_set(lst, (int)i, 0, jtesPageName(p) + (p.channel != 0x92 ? fmt(" %02X", p.channel) : std::string()));
                lv_set(lst, (int)i, 1, jtesTune(p).empty() ? "" : "\xE2\x99\xAA");
                lv_set(lst, (int)i, 2, p.title);
            }
            txt = edit(hwnd, 0, "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL);
            SendMessageW(txt, WM_SETFONT, (WPARAM)jf, TRUE);
            if (!J.pages.empty()) { ListView_SetItemState(lst, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            int lw = std::min((int)S(hwnd, 400), (int)(rc.right - 2 * p) * 2 / 5);
            MoveWindow(lst, p, y, lw, std::max(0L, rc.bottom - y - p), TRUE);
            int x0 = p + lw + S(hwnd, 10), th = S(hwnd, 170);
            pic = {x0, y, rc.right - p, std::max<LONG>(y + S(hwnd, 100), rc.bottom - p - th - S(hwnd, 10))};
            MoveWindow(txt, x0, pic.bottom + S(hwnd, 10), rc.right - p - x0, std::max(0L, rc.bottom - p - pic.bottom - S(hwnd, 10)), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l, [this](int row) { return row < (int)R->jtes.pages.size() && R->jtes.pages[row].gaps ? MUTED : FG; });
                if (nh->code == LVN_ITEMCHANGED) { int i = ListView_GetNextItem(lst, -1, LVNI_SELECTED); if (i >= 0 && i != sel) select(i); }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 405) {
                sound = !sound;
                set_text(soundBtn, sound ? "Sound: on" : "Sound: off");
                InvalidateRect(soundBtn, nullptr, TRUE);
                if (sound) play(); else PlaySoundW(nullptr, nullptr, 0);
            } else if (id == 406) { play(); showFrame(0); }
            else if (id == 402) {
                std::string d = pick_folder(hwnd, "Export the Japanese teletext pages");
                if (!d.empty()) {
                    int n = jtesExport(R->jtes, path_join(d, stem_path(file_name(*R)) + "_jtes"));
                    ask(hwnd, "Export", fmt("%d pages written (PNG, text, WAV for the tunes).", n), MB_ICONINFORMATION);
                }
            } else if (id == 403) {
                std::string s;
                for (auto &x : R->jtes.report) s += x + "\n";
                s += "\n";
                for (auto &p : R->jtes.pages) s += "=== " + jtesPageName(p) + "  " + p.title + "\n" + jtesText(p) + "\n\n";
                text_window(hwnd, "Japanese teletext \xE2\x80\x94 " + file_name(*R), "every page as text", s);
            }
            return 0;
        }
        case WM_TIMER:
            if (w == 7) { showFrame(frame + 1); return 0; }
            break;
        case WM_DESTROY: KillTimer(hwnd, 7); PlaySoundW(nullptr, nullptr, 0); break;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ ТВ-информ
// One row per line that carries it; for the chosen line a card (what it is) and its text:
// the document sent, or the inserter's test message; "Packets" shows every chunk as received.
struct TviWin : Window {
    ResultPtr R;
    Header hd; HWND lst = nullptr, ed = nullptr, pkb = nullptr, flist = nullptr;
    std::vector<TviSummary> sum;
    int sel = 0, fsel = -1; bool packets = false, rot = true;
    RECT card{}, pic{};
    std::vector<uint32_t> img; int iw = 0, ih = 0;   // the chosen file as a picture (a fax page)
    std::string picNote;
    bool files() const { return false; }   // files and documents: the window "Found files and texts"
    static std::string base(const std::string &p) { return p.substr(p.find_last_of("\\/") + 1); }
    void loadPic()
    {
        img.clear(); iw = ih = 0; picNote.clear();
        const auto &fl = R->tvinform[sel].files;
        if (fsel < 0 || fsel >= (int)fl.size()) return;
        std::vector<uint8_t> t;
        try { Bytes b = read_file(fl[fsel]); t.assign(b.begin(), b.end()); } catch (...) { picNote = "the file is not on disk any more: " + fl[fsel]; return; }
        int w = 0, h = 0;
        std::vector<uint8_t> px;
        if (!faxTiffDecode(t, w, h, px, picNote)) { picNote = fmt("%zu bytes - not a picture this window can show; Open file opens it", t.size()); return; }
        iw = rot ? h : w; ih = rot ? w : h;
        img.assign((size_t)iw * ih, 0xFFFFFF);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (px[(size_t)y * w + x]) {
                    int X = rot ? y : x, Y = rot ? w - 1 - x : y;   // a quarter turn to the left: fax maps are sent on their side
                    img[(size_t)Y * iw + X] = 0;
                }
        InvalidateRect(hwnd, &pic, FALSE);
    }
    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int p = S(hwnd, 14), y = hd.layout(hwnd);
        int lh = S(hwnd, 30) + (int)std::min<size_t>(sum.size(), 6) * S(hwnd, 22);
        MoveWindow(lst, p, y, rc.right - 2 * p, lh, TRUE); y += lh + S(hwnd, 10);
        card = {p, y, rc.right - p, y + S(hwnd, 104)}; y = card.bottom + S(hwnd, 10);
        int h = std::max(0L, rc.bottom - y - p);
        if (files()) {
            int fw = S(hwnd, 280);
            MoveWindow(flist, p, y, fw, h, TRUE);
            pic = {p + fw + S(hwnd, 10), y, rc.right - p, y + h};
            ShowWindow(flist, SW_SHOW); ShowWindow(ed, SW_HIDE);
        } else {
            MoveWindow(ed, p, y, rc.right - 2 * p, h, TRUE);
            pic = {};
            ShowWindow(flist, SW_HIDE); ShowWindow(ed, SW_SHOW);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    static COLORREF colour(const TviSummary &s)
    {
        return s.kind == TviSummary::Data ? C_PROG : s.kind == TviSummary::Test ? C_TIME : s.kind == TviSummary::Packets ? C_CHAN : C_OTHER;
    }
    static const char *kind_name(const TviSummary &s)
    {
        return s.kind == TviSummary::Data ? "DATA" : s.kind == TviSummary::Test ? "TEST" : s.kind == TviSummary::Packets ? "PACKETS" : "IDLE";
    }
    std::string where(size_t i) const
    {
        const TviStream &t = R->tvinform[i];
        if (t.rate < 0) {
            std::string w;
            for (auto &li : R->lines)
                if (li.kind.rfind("TV-Inform packets", 0) == 0) w += (w.empty() ? "lines " : ", ") + fmt("%d", li.lineNo) + (li.field == 2 ? "/f2" : "");
            return w.empty() ? "lines 7-9" : w;
        }
        if (t.rate == 2) {   // fТ3: one stream over several lines
            std::set<std::pair<int, int>> ls;
            for (auto &w : t.words) ls.insert({w.field, w.lineNo});
            std::string s;
            for (auto &x : ls) s += (s.empty() ? "lines " : ", ") + fmt("%d", x.second);
            return s.empty() ? "?" : s;
        }
        return t.words.empty() ? "?" : fmt("line %d field %d", t.words[0].lineNo, t.words[0].field);
    }
    std::string clock(size_t i) const
    {
        int r = R->tvinform[i].rate;
        return r < 0 ? "5.73 MHz" : r == 0 ? "f\xD0\xA2" "1 1.73 MHz" : r == 1 ? "f\xD0\xA2" "2 3.47 MHz" : "f\xD0\xA2" "3 6.94 MHz";
    }
    void show()
    {
        if (sum.empty()) return;
        const TviStream &t = R->tvinform[sel];
        std::string body;
        if (packets) body = t.rate < 0 || t.serial.empty() ? tvinform_text_one(t) : "recording    frame    kind    hdr  chars\n" + tviSerialText(t, frame_rate(*R));
        else if (sum[sel].kind == TviSummary::Data || !t.files.empty())
            body = "What this line brought (" + std::string(t.files.empty() ? "the document" : "the files") +
                   ") is in the window \"Found files and texts\" \xE2\x80\x94 the button Found files on top.\n"
                   "Packets shows what the line carried as it was received.";
        else for (auto &l : sum[sel].text) body += l + "\n";
        set_text(ed, replace_all(body, "\n", "\r\n"));
        set_text(pkb, packets ? "Text" : "Packets");
        ListView_DeleteAllItems(flist);
        for (size_t i = 0; i < t.files.size(); i++) lv_set(flist, (int)i, 0, base(t.files[i]));
        fsel = t.files.empty() ? -1 : 0;
        if (fsel == 0) ListView_SetItemState(flist, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        loadPic();
        layout();
    }
    static std::string tvinform_text_one(const TviStream &t) { std::string s; for (auto &l : t.report) s += l + "\n"; return s; }
    void paint(HDC dc, RECT)
    {
        if (sum.empty()) return;
        const TviSummary &s = sum[sel];
        HFONT big = font(hwnd, 15, true), small = font(hwnd, 9), smallb = font(hwnd, 10, true);
        fill(dc, card, PANEL);
        int x = card.left + S(hwnd, 18), y = card.top + S(hwnd, 14);
        std::string k = kind_name(s);
        RECT pill{x, y, x + S(hwnd, 16) + (int)k.size() * S(hwnd, 9), y + S(hwnd, 24)};
        fill(dc, pill, colour(s));
        text_out(dc, pill.left + S(hwnd, 8), pill.top + S(hwnd, 3), k, RGB(255, 255, 255), smallb);
        text_out(dc, pill.right + S(hwnd, 12), y, s.what, FG, big);
        y += S(hwnd, 36);
        const TviStream &t = R->tvinform[sel];
        std::string sub = where(sel) + "  \xC2\xB7  " + clock(sel);
        if (t.rate >= 0) sub += fmt("  \xC2\xB7  %d bits per line", t.nbits);
        if (!s.charset.empty()) sub += "  \xC2\xB7  " + s.charset + ", levels inverted";
        text_out(dc, x, y, sub, ACCENT2, small);
        y += S(hwnd, 20);
        std::string note = s.kind == TviSummary::Test ? "teleprinter test RYRY..., 1234567890 and the name of the system: no files were being sent on this line"
                         : s.kind == TviSummary::Data ? "a file sent through the system, as received (gaps = chunks not sent on this line)"
                         : s.kind == TviSummary::Packets ? "error-corrected packet stream; see the text for what it carries"
                         : "the line carries the ТВ-информ carrier but no readable characters";
        text_out(dc, x, y, note, MUTED, small);
        if (t.rate >= 0) {
            std::vector<std::pair<std::string, std::string>> tl = {{fmt("%d", (int)t.words.size()), "lines sliced"}, {fmt("%d", s.dataLines), "lines with data"}};
            if (s.fixed) tl.push_back({fmt("%d", s.fixed), "bits corrected"});
            if (s.ecTest) tl.push_back({fmt("%d", s.ecTest), "ECC test words"});
            tiles(hwnd, dc, card, tl);
        } else if (!t.files.empty())
            tiles(hwnd, dc, card, {{fmt("%zu", t.files.size()), "files received"}});
        if (pic.right > pic.left) {
            fill(dc, pic, RGB(0x11, 0x11, 0x11));
            int ch = S(hwnd, 22);
            text_out(dc, pic.left + S(hwnd, 8), pic.top + S(hwnd, 4), picNote, MUTED, small);
            if (iw > 0) {
                int aw = pic.right - pic.left - S(hwnd, 16), ah = pic.bottom - pic.top - ch - S(hwnd, 8);
                double ar = (double)iw / ih;
                int w = std::min(aw, (int)(ah * ar)), h = (int)(w / ar);
                int x = pic.left + (pic.right - pic.left - w) / 2, y = pic.top + ch + (ah - h) / 2;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = iw; bi.bmiHeader.biHeight = -ih;
                bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchDIBits(dc, x, y, w, h, 0, 0, iw, ih, img.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            }
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            for (auto &t : R->tvinform) {
                sum.push_back(tviSummarize(t));
                if (!t.files.empty())   // packets that carried files: name them
                    sum.back().what = t.files.size() == 1 ? "file received: " + base(t.files[0]) : fmt("%zu files received: ", t.files.size()) + base(t.files[0]) + ", ...";
            }
            hd.create(hwnd, "\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC",
                      fmt("Russian VBI datacast (GOST R 50822-95) \xC2\xB7 %zu lines \xC2\xB7 ", sum.size()) + file_name(*R));
            pkb = hd.add(hwnd, 401, "Packets", 100);
            hd.add(hwnd, 402, "Report", 90);
            hd.add(hwnd, 403, "Save text\xE2\x80\xA6", 110);
            hd.add(hwnd, 407, "Found files\xE2\x80\xA6", 120);
            flist = listview(hwnd, 11, {{"File", 260}});
            lst = listview(hwnd, 10, {{"Line", 130}, {"Clock", 100}, {"Kind", 70}, {"Content", 330}, {"Characters", 90}, {"First words", 400}});
            for (size_t i = 0; i < sum.size(); i++) {
                std::string first;
                for (auto &x : sum[i].text) if (!strip(x).empty()) { first = strip(x); break; }
                lv_set(lst, (int)i, 0, where(i)); lv_set(lst, (int)i, 1, clock(i)); lv_set(lst, (int)i, 2, kind_name(sum[i]));
                lv_set(lst, (int)i, 3, sum[i].what); lv_set(lst, (int)i, 4, sum[i].charset); lv_set(lst, (int)i, 5, first.substr(0, 200));
                if (sum[i].kind == TviSummary::Data && sum[sel].kind != TviSummary::Data) sel = (int)i;
            }
            ed = edit(hwnd, 0, "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL);
            SendMessageW(ed, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(ed, WM_SETFONT, (WPARAM)font(hwnd, 10, false, L"Consolas"), TRUE);
            for (size_t i = 0; i < sum.size(); i++)   // a line that brought files comes first
                if (!R->tvinform[i].files.empty()) { sel = (int)i; break; }
            if (!sum.empty()) ListView_SetItemState(lst, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            show();
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            auto *nh = (NMHDR *)l;
            if (nh->hwndFrom == lst) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l, [this](int row) { return row >= 0 && row < (int)sum.size() ? colour(sum[row]) : FG; });
                if (nh->code == LVN_ITEMCHANGED) {
                    auto *iv = (NMLISTVIEW *)l;
                    if ((iv->uNewState & LVIS_SELECTED) && iv->iItem >= 0 && iv->iItem != sel) { sel = iv->iItem; show(); }
                }
            }
            if (nh->hwndFrom == flist) {
                if (nh->code == NM_CUSTOMDRAW) return lv_custom_draw(l);
                if (nh->code == LVN_ITEMCHANGED) {
                    auto *iv = (NMLISTVIEW *)l;
                    if ((iv->uNewState & LVIS_SELECTED) && iv->iItem >= 0 && iv->iItem != fsel) { fsel = iv->iItem; loadPic(); }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            static const std::vector<std::string> none;
            const auto &fl = sum.empty() ? none : R->tvinform[sel].files;
            bool hasF = fsel >= 0 && fsel < (int)fl.size();
            if (id == 401) { packets = !packets; show(); }
            else if (id == 404) { rot = !rot; loadPic(); }
            else if (id == 405 && hasF) open_path(fl[fsel]);
            else if (id == 406 && hasF) open_path(fl[fsel].substr(0, fl[fsel].find_last_of("\\/")));
            else if (id == 407) finds_window(hwnd, R);
            else if (id == 402) text_window(hwnd, "TV-Inform report \xE2\x80\x94 " + file_name(*R), "everything the decoder found on the ТВ-информ lines", tvinform_text(*R));
            else if (id == 403 && !sum.empty()) {
                std::string p = save_file(hwnd, "Save text", "tvinform_" + where(sel) + ".txt", "txt");
                std::string body;
                for (auto &x : sum[sel].text) body += x + "\n";
                if (!p.empty()) { try { write_text(p, body); } catch (std::exception &e) { ask(hwnd, "Save", e.what(), MB_ICONERROR); } }
            }
            return 0;
        }
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};

// ================================================================ 5.73 Mbit/s data and plain text
struct TextWin : Window {
    std::string title, subtitle, body, summary_text;
    Header hd; HWND summary = nullptr, ed = nullptr;
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE:
            dark_window(hwnd);
            hd.create(hwnd, title, subtitle);
            hd.add(hwnd, 402, "Save as text\xE2\x80\xA6", 140);
            if (!summary_text.empty()) { summary = label(hwnd, summary_text); set_label_colour(summary, ACCENT2); }
            ed = edit(hwnd, 0, "", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOVSCROLL);
            SendMessageW(ed, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(ed, WM_SETFONT, (WPARAM)font(hwnd, 9, false, L"Consolas"), TRUE);
            SetWindowLongW(ed, GWL_STYLE, GetWindowLongW(ed, GWL_STYLE) & ~ES_AUTOHSCROLL);
            set_text(ed, replace_all(replace_all(body, "\r\n", "\n"), "\n", "\r\n"));
            return 0;
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd);
            if (summary) { MoveWindow(summary, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE); y += S(hwnd, 26); }
            MoveWindow(ed, p, y, rc.right - 2 * p, std::max(0L, rc.bottom - y - p), TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == 402) {
                std::string p = save_file(hwnd, "Save as text", title.substr(0, title.find(" \xE2\x80\x94")) + ".txt", "txt");
                if (!p.empty()) { try { write_text(p, body); } catch (std::exception &e) { ask(hwnd, "Save", e.what(), MB_ICONERROR); } }
            }
            return 0;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
}  // namespace

void starsight_window(HWND owner, const ResultPtr &r)
{
    auto *w = new GuideWin; w->R = r;
    open_window(w, owner, L"TRGuide", "StarSight \xE2\x80\x94 " + file_name(*r), 1180, 860);
    SetFocus(w->hwnd);
}
void gemstar_window(HWND owner, const ResultPtr &r)
{
    auto *w = new GemWin; w->R = r;
    open_window(w, owner, L"TRGemstar", "Gemstar \xE2\x80\x94 " + file_name(*r), 1180, 860);
    SetFocus(w->hwnd);
}
void jtes_window(HWND owner, const ResultPtr &r)
{
    auto *w = new JtesWin; w->R = r;
    open_window(w, owner, L"TRJtes", "Japanese teletext \xE2\x80\x94 " + file_name(*r), 1240, 820);
}
void captions_window(HWND owner, const ResultPtr &r)
{
    auto *w = new CapWin; w->R = r;
    open_window(w, owner, L"TRCaptions", "CC captions \xE2\x80\x94 " + file_name(*r), 940, 760);
}
void xds_window(HWND owner, const ResultPtr &r)
{
    auto *w = new XdsWin; w->R = r;
    open_window(w, owner, L"TRXds", "XDS \xE2\x80\x94 " + file_name(*r), 940, 760);
}
void sign_window(HWND owner, const ResultPtr &r)
{
    auto *w = new SignWin; w->R = r;
    open_window(w, owner, L"TRSign", "Silent Radio \xE2\x80\x94 " + file_name(*r), SR_W * 8 + 60, 660);
}
void amol_window(HWND owner, const ResultPtr &r)
{
    auto *w = new AmolWin; w->R = r;
    open_window(w, owner, L"TRAmol", "AMOL \xE2\x80\x94 " + file_name(*r), 1000, 780);
}
void data_window(HWND owner, const ResultPtr &r, const std::string &kind)
{
    auto *w = new TextWin;
    char want = kind == "NABTS" ? 'N' : kind == "WST teletext" ? 'W' : 'X';
    size_t n = 0; for (auto &p : r->fastPackets) n += p.kind == want;
    std::string where;
    std::set<std::string> seen;
    for (auto &li : r->lines)
        if (li.mode == MODE_FAST && seen.insert(fmt("%d", li.lineNo)).second) where += (where.empty() ? "" : ", ") + fmt("line %d", li.lineNo);
    w->title = kind == "framing 2D" ? "Encrypted datacast (framing 2D)" : kind;
    w->subtitle = "5.73 Mbit/s \xC2\xB7 " + where + " \xC2\xB7 " + file_name(*r);
    w->summary_text = fmt("%zu packets of this kind, %zu packets at 5.73 Mbit/s in all, %zu text messages recovered", n, r->fastPackets.size(), r->fastText.size());
    for (auto &l : r->fastReport) w->body += l + "\n";
    w->body += "\n=== recovered text ===\n";
    for (auto &l : r->fastText) w->body += l + "\n";
    open_window(w, owner, L"TRData", w->title + " \xE2\x80\x94 " + file_name(*r), 1000, 720);
}
void tvinform_window(HWND owner, const ResultPtr &r)
{
    auto *w = new TviWin; w->R = r;
    open_window(w, owner, L"TRTvinform", "\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC \xE2\x80\x94 " + file_name(*r), 1100, 820);
}
void text_window(HWND owner, const std::string &title, const std::string &sub, const std::string &body)
{
    auto *w = new TextWin; w->title = title; w->subtitle = sub; w->body = body;
    open_window(w, owner, L"TRText", title, 1000, 720);
}
}
