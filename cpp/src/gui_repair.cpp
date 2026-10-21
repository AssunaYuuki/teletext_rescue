// VBI repair window (VHSTTX 2.7 "VBI Repair"): a slider for each control of vbirepair.h; every change goes to the
// window that opened it (the VBI lines window shows the repaired signal, the capture window decodes it). With a
// recording (RepairSource) also: the frames selected (start / end from the opener's frame), lines moved by hand
// (Stabilize VBI), ranges of frames with settings of their own (tuning ranges), a measure of what the settings do
// to the teletext (packets and parity errors per line, as recorded and repaired) and saving: the whole recording
// or the selection as .vbi, the selection's teletext as .t42.
// The settings are kept between sessions (state "vbi_repair", the ranges per recording in "vbi_repair_ranges").
#include "gui_app.h"
#include "live.h"
#include "vbifile.h"
#include "vbirepair.h"
#include <commctrl.h>

namespace ui {
namespace {

enum {
    ID_RESET = 900, ID_ACT0 = 901, ID_SLIDER0 = 1000,
    ID_SEL_START = 1100, ID_SEL_END, ID_SEL_ALL, ID_GO_START, ID_SHIFT_SET, ID_SHIFT_CLEAR, ID_RANGE_ADD, ID_RANGE_UPDATE,
    ID_RANGE_DELETE, ID_RANGE_LOAD, ID_MEASURE, ID_SAVE_ALL, ID_SAVE_SEL, ID_SAVE_T42, ID_RANGES, ID_LINES, ID_SHIFT_LINE,
    ID_SHIFT_VAL, ID_VHSTTX,
};

Json repairJson(const VbiRepair& r)
{
    Json j = Json::object();
    for (auto& c : VbiRepair::controls()) j[c.key] = r.*c.v;
    Json s = Json::object();
    for (auto& kv : r.lineShift) if (kv.second) s[std::to_string(kv.first)] = kv.second;
    j["shift"] = s;
    return j;
}
VbiRepair repairOf(const Json* j)
{
    VbiRepair r;
    if (!j || !j->is_obj()) return r;
    for (auto& c : VbiRepair::controls()) r.*c.v = std::clamp((int)j->get_num(c.key, c.neutral), 0, 100);
    if (const Json* s = j->find("shift"); s && s->is_obj())
        for (auto& kv : s->o) r.lineShift[atoi(kv.first.c_str())] = std::clamp((int)kv.second.num(), -64, 64);
    return r;
}

struct RepairWin : Window {
    std::string sub;
    VbiRepair r;
    std::function<void(const VbiRepair&)> onChange;
    std::vector<std::pair<std::string, std::function<void(const VbiRepair&)>>> actions;
    bool hasSrc = false;
    RepairSource src;
    VbiGeometry geom;
    int64_t selFrom = 0, selTo = -1;
    std::vector<VbiRepairRange> ranges;
    Header hd;
    std::vector<HWND> sliders, names, values;
    HWND note = nullptr;
    // the recording part
    HWND g_sel = nullptr, sel_lbl = nullptr, b_start = nullptr, b_end = nullptr, b_all = nullptr, b_go = nullptr;
    HWND g_shift = nullptr, sh_line = nullptr, sh_val = nullptr, b_shset = nullptr, b_shclr = nullptr, sh_lbl = nullptr;
    HWND g_rng = nullptr, rng = nullptr, b_radd = nullptr, b_rupd = nullptr, b_rdel = nullptr, b_rload = nullptr;
    HWND g_meas = nullptr, b_meas = nullptr, meas = nullptr, meas_lbl = nullptr;
    HWND b_sall = nullptr, b_ssel = nullptr, b_st42 = nullptr;

    std::string lineName(int l) const { return src.lineName ? src.lineName(l) : std::to_string(l); }
    int64_t curFrame() const { return src.frame ? src.frame() : 0; }
    int64_t selEnd() const { return selTo < 0 ? geom.frames - 1 : selTo; }

    void show_values()
    {
        auto& cs = VbiRepair::controls();
        for (size_t i = 0; i < cs.size(); i++) {
            int v = r.*cs[i].v;
            set_text(values[i], v == cs[i].neutral ? std::string(cs[i].neutral ? "50 (as is)" : "off") : std::to_string(v));
        }
        set_text(note, r.active() ? "on: " + r.text() : "nothing changed: the lines are decoded as recorded");
        if (sh_lbl) {
            std::string s;
            for (auto& kv : r.lineShift) if (kv.second) s += (s.empty() ? "" : ", ") + lineName(kv.first) + fmt(" %+d", kv.second);
            set_text(sh_lbl, s.empty() ? "no line moved" : "moved: " + s);
        }
    }
    void changed()
    {
        show_values();
        Json st = load_state();
        st["vbi_repair"] = repairJson(r);
        save_state(st);
        if (onChange) onChange(r);
    }
    void set_sliders()
    {
        auto& cs = VbiRepair::controls();
        for (size_t i = 0; i < cs.size(); i++) SendMessageW(sliders[i], TBM_SETPOS, TRUE, r.*cs[i].v);
    }
    // ---------------------------------------------------------------- selection, ranges
    void show_sel()
    {
        double fps = geom.pal ? 25.0 : 30000.0 / 1001;
        int64_t e = selEnd();
        set_text(sel_lbl, fmt("frames %lld \xE2\x80\x93 %lld (%lld frames, %.1f s) of %lld", (long long)selFrom + 1, (long long)e + 1,
                              (long long)(e - selFrom + 1), (e - selFrom + 1) / fps, (long long)geom.frames));
    }
    void save_ranges()
    {
        Json st = load_state();
        Json all = st.find("vbi_repair_ranges") && st.find("vbi_repair_ranges")->is_obj() ? *st.find("vbi_repair_ranges") : Json::object();
        Json a = Json::array();
        for (auto& rg : ranges) {
            Json j = Json::object();
            j["from"] = (double)rg.from; j["to"] = (double)rg.to; j["r"] = repairJson(rg.r);
            a.push(j);
        }
        all[src.path] = a;
        st["vbi_repair_ranges"] = all;
        save_state(st);
    }
    void load_ranges()
    {
        Json st = load_state();
        const Json* all = st.find("vbi_repair_ranges");
        const Json* a = all && all->is_obj() ? all->find(src.path) : nullptr;
        if (!a || !a->is_arr()) return;
        for (auto& j : a->a) {
            VbiRepairRange rg;
            rg.from = (int64_t)j.get_num("from", 0); rg.to = (int64_t)j.get_num("to", 0);
            rg.r = repairOf(j.find("r"));
            ranges.push_back(rg);
        }
    }
    void show_ranges()
    {
        ListView_DeleteAllItems(rng);
        for (size_t i = 0; i < ranges.size(); i++) {
            lv_set(rng, (int)i, 0, fmt("%lld", (long long)ranges[i].from + 1));
            lv_set(rng, (int)i, 1, fmt("%lld", (long long)ranges[i].to + 1));
            lv_set(rng, (int)i, 2, ranges[i].r.active() ? ranges[i].r.text() : "as recorded");
        }
    }
    int sel_range() const { return ListView_GetNextItem(rng, -1, LVNI_SELECTED); }
    // ---------------------------------------------------------------- measure
    void measure()
    {
        int64_t from = curFrame(), count = 100;
        auto stats = std::make_shared<VbiRepairMeasure>();
        std::string path = src.path;
        VbiFormat f = (VbiFormat)src.format;
        VbiRepair rr = r;
        HWND me = hwnd;
        set_text(meas_lbl, "measuring\xE2\x80\xA6");
        run_task(hwnd, "Measure the repair", [=](Progress& pr) {
            pr.step(fmt("frames %lld\xE2\x80\x93%lld: every line read as recorded and repaired", (long long)from + 1, (long long)(from + count)));
            std::string err;
            if (!liveRepairMeasure(path, f, rr, from, count, *stats, err)) throw std::runtime_error(err);
        }, [this, me, stats, from](bool ok) {
            if (!IsWindow(me)) return;
            if (!ok) { set_text(meas_lbl, "the measure failed"); return; }
            ListView_DeleteAllItems(meas);
            int row = 0;
            for (auto& s : stats->lines) {
                if (!s.packets[0] && !s.packets[1]) continue;
                lv_set(meas, row, 0, lineName(s.index));
                lv_set(meas, row, 1, fmt("%d \xE2\x86\x92 %d", s.packets[0], s.packets[1]));
                int d = s.packets[1] - s.packets[0];
                lv_set(meas, row, 2, d > 0 ? fmt("+%d", d) : d < 0 ? fmt("%d", d) : std::string("same"));
                row++;
            }
            long p0 = stats->packets[0], p1 = stats->packets[1], b0 = stats->bad[0], b1 = stats->bad[1], n0 = stats->bytes[0], n1 = stats->bytes[1];
            if (!row) { set_text(meas_lbl, "no teletext line in these frames (the measure reads WST teletext)"); return; }
            set_text(meas_lbl, fmt("frames %lld\xE2\x80\x93%lld: packets %ld \xE2\x86\x92 %ld, parity errors %.2f%% \xE2\x86\x92 %.2f%%  %s",
                                   (long long)from + 1, (long long)from + 100, p0, p1, n0 ? 100.0 * b0 / n0 : 0.0, n1 ? 100.0 * b1 / n1 : 0.0,
                                   p1 > p0 || (p1 == p0 && n1 && n0 && (double)b1 / n1 < (double)b0 / n0) ? "\xE2\x80\x94 the repair helps"
                                   : p1 < p0 ? "\xE2\x80\x94 the repair loses packets" : ""));
        });
    }
    // ---------------------------------------------------------------- saving
    void save_vbi(bool selection)
    {
        std::string out = save_file(hwnd, selection ? "Repaired selection" : "Repaired copy of the recording",
                                    stem_path(basename(src.path)) + (selection ? "_repaired_part.vbi" : "_repaired.vbi"), "vbi");
        if (out.empty()) return;
        std::string in = src.path;
        VbiFormat f = (VbiFormat)src.format;
        VbiRepair rr = r;
        auto rg = ranges;
        int64_t a = selection ? selFrom : 0, b = selection ? selEnd() : -1;
        run_task(hwnd, "VBI repair", [=](Progress& pr) {
            std::string err;
            pr.step("repairing the VBI lines" + std::string(rr.active() ? " (" + rr.text() + ")" : "") + (rg.empty() ? "" : fmt(", %zu ranges", rg.size())));
            if (!vbiRepairFile(in, out, f, rr, rg, a, b, [&](double x) { pr.progress((long long)(x * 1000), 1000); }, &pr.cancel, err))
                throw std::runtime_error(err);
        }, [out](bool ok) { if (ok && open_recording_hook()) open_recording_hook()(out); });
    }
    void save_t42()
    {
        std::string out = save_file(hwnd, "Teletext of the repaired selection", stem_path(basename(src.path)) + "_repaired.t42", "t42");
        if (out.empty()) return;
        std::string in = src.path, tmp = out + ".repair.vbi";
        VbiFormat f = (VbiFormat)src.format;
        VbiRepair rr = r;
        auto rg = ranges;
        int64_t a = selFrom, b = selEnd();
        auto note = std::make_shared<std::string>();
        run_task(hwnd, "Teletext of the repaired selection", [=](Progress& pr) {
            std::string err;
            pr.step("repairing the selection");
            if (!vbiRepairFile(in, tmp, f, rr, rg, a, b, [&](double x) { pr.progress((long long)(x * 500), 1000); }, &pr.cancel, err))
                throw std::runtime_error(err);
            VbiFile vf;
            if (!vf.open(tmp, err, f)) throw std::runtime_error(err);
            VbiGeometry g = vf.geom();
            vf.close();
            pr.step("reading the teletext (MLSE, as the live decoder)");
            LiveReadStats st;
            auto pk = liveReadTeletext(tmp, g, g.frames, false, [&](double x, const std::string&) { pr.progress(500 + (long long)(x * 500), 1000); },
                                       &pr.cancel, st);
            _wremove(widen(tmp).c_str());
            write_file(out, pk.data(), pk.size());
            *note = fmt("%zu packets", pk.size() / 42);
            pr.log("written " + out + ": " + *note);
        }, [this, out, note](bool ok) { if (ok && IsWindow(hwnd)) set_text(meas_lbl, "written " + out + ": " + *note); });
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            hd.create(hwnd, "VBI repair", sub);
            hd.add(hwnd, ID_RESET, "Reset", 80);
            for (size_t i = 0; i < actions.size(); i++) hd.add(hwnd, ID_ACT0 + (int)i, actions[i].first, 250);
            if (hasSrc) hd.add(hwnd, ID_VHSTTX, "Open in VHSTTX VBI Repair\xE2\x80\xA6", 230);
            auto& cs = VbiRepair::controls();
            for (size_t i = 0; i < cs.size(); i++) {
                names.push_back(label(hwnd, cs[i].label));
                HWND s = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10, hwnd,
                                         (HMENU)(INT_PTR)(ID_SLIDER0 + i), inst(), nullptr);
                SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
                SendMessageW(s, TBM_SETPAGESIZE, 0, 10);
                SendMessageW(s, TBM_SETPOS, TRUE, r.*cs[i].v);
                sliders.push_back(s);
                HWND v = label(hwnd, "");
                set_label_colour(v, ACCENT2);
                values.push_back(v);
            }
            note = label(hwnd, "");
            set_label_colour(note, MUTED);
            if (hasSrc) {
                g_sel = label(hwnd, "Frames"); SendMessageW(g_sel, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
                sel_lbl = label(hwnd, ""); set_label_colour(sel_lbl, ACCENT2);
                b_start = button(hwnd, ID_SEL_START, "Start = this frame"); b_end = button(hwnd, ID_SEL_END, "End = this frame");
                b_all = button(hwnd, ID_SEL_ALL, "Whole recording"); b_go = button(hwnd, ID_GO_START, "Go to start");
                g_shift = label(hwnd, "Move a line (Stabilize VBI)"); SendMessageW(g_shift, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
                sh_line = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, 300, hwnd,
                                          (HMENU)(INT_PTR)ID_SHIFT_LINE, inst(), nullptr);
                SendMessageW(sh_line, WM_SETFONT, (WPARAM)font(hwnd), TRUE);
                for (int i = 0; i < geom.linesPerFrame; i++) SendMessageW(sh_line, CB_ADDSTRING, 0, (LPARAM)W("line " + lineName(i)).c_str());
                SendMessageW(sh_line, CB_SETCURSEL, 0, 0);
                sh_val = edit(hwnd, ID_SHIFT_VAL, "0", ES_CENTER);
                b_shset = button(hwnd, ID_SHIFT_SET, "Set (samples, + = later)"); b_shclr = button(hwnd, ID_SHIFT_CLEAR, "Clear all");
                sh_lbl = label(hwnd, ""); set_label_colour(sh_lbl, MUTED);
                g_rng = label(hwnd, "Ranges of frames with settings of their own"); SendMessageW(g_rng, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
                rng = listview(hwnd, ID_RANGES, {{"From", 70}, {"To", 70}, {"Settings", 360}});
                b_radd = button(hwnd, ID_RANGE_ADD, "Add the selection"); b_rupd = button(hwnd, ID_RANGE_UPDATE, "Update");
                b_rload = button(hwnd, ID_RANGE_LOAD, "Load its settings"); b_rdel = button(hwnd, ID_RANGE_DELETE, "Delete");
                g_meas = label(hwnd, "What the settings do to the teletext"); SendMessageW(g_meas, WM_SETFONT, (WPARAM)font(hwnd, 10, true), TRUE);
                b_meas = button(hwnd, ID_MEASURE, "Measure 100 frames from this frame", BTN_ACCENT);
                meas = listview(hwnd, ID_LINES, {{"Line", 110}, {"Packets: recorded \xE2\x86\x92 repaired", 240}, {"Difference", 120}});
                meas_lbl = label(hwnd, "teletext packets read by the live decoder on each line, and the share of text bytes with a parity error, as recorded and repaired");
                set_label_colour(meas_lbl, MUTED);
                b_sall = button(hwnd, ID_SAVE_ALL, "Save repaired VBI\xE2\x80\xA6");
                b_ssel = button(hwnd, ID_SAVE_SEL, "Save the selection as VBI\xE2\x80\xA6");
                b_st42 = button(hwnd, ID_SAVE_T42, "Save the selection's teletext .t42\xE2\x80\xA6");
                load_ranges();
                show_ranges();
                show_sel();
            }
            show_values();
            return 0;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y0 = hd.layout(hwnd), y = y0, rowH = S(hwnd, 30), bh = S(hwnd, 28), gap = S(hwnd, 6);
            int leftW = hasSrc ? std::max(S(hwnd, 460), (int)rc.right * 45 / 100) : (int)rc.right - 2 * p;
            int nameW = S(hwnd, 190), valW = S(hwnd, 80), sw = std::max(S(hwnd, 80), leftW - nameW - valW - S(hwnd, 16));
            for (size_t i = 0; i < sliders.size(); i++) {
                MoveWindow(names[i], p, y + S(hwnd, 5), nameW, S(hwnd, 20), TRUE);
                MoveWindow(sliders[i], p + nameW + S(hwnd, 8), y, sw, S(hwnd, 28), TRUE);
                MoveWindow(values[i], p + nameW + sw + S(hwnd, 16), y + S(hwnd, 5), valW, S(hwnd, 20), TRUE);
                y += rowH;
            }
            MoveWindow(note, p, y + S(hwnd, 6), leftW, S(hwnd, 40), TRUE);
            if (!hasSrc) return 0;
            int x = p + leftW + S(hwnd, 20), rw = rc.right - x - p;
            y = y0;
            auto row = [&](std::vector<std::pair<HWND, int>> hs) {
                int xx = x;
                for (auto& h : hs) { MoveWindow(h.first, xx, y, h.second, bh, TRUE); xx += h.second + gap; }
                y += bh + gap;
            };
            MoveWindow(g_sel, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 22);
            MoveWindow(sel_lbl, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 24);
            int q = (rw - 3 * gap) / 4;
            row({{b_start, q}, {b_end, q}, {b_all, q}, {b_go, q}});
            y += gap;
            MoveWindow(g_shift, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 22);
            row({{sh_line, S(hwnd, 150)}, {sh_val, S(hwnd, 60)}, {b_shset, S(hwnd, 190)}, {b_shclr, S(hwnd, 90)}});
            MoveWindow(sh_lbl, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 26);
            MoveWindow(g_rng, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 22);
            int rh = S(hwnd, 110);
            MoveWindow(rng, x, y, rw, rh, TRUE); y += rh + gap;
            row({{b_radd, q}, {b_rupd, q}, {b_rload, q}, {b_rdel, q}});
            y += gap;
            MoveWindow(g_meas, x, y, rw, S(hwnd, 20), TRUE); y += S(hwnd, 22);
            row({{b_meas, std::min(rw, S(hwnd, 300))}});
            int bottom = rc.bottom - p - bh - gap;
            int mh = std::max(S(hwnd, 80), bottom - y - S(hwnd, 46));
            MoveWindow(meas, x, y, rw, mh, TRUE); y += mh + gap;
            MoveWindow(meas_lbl, x, y, rw, S(hwnd, 38), TRUE);
            y = bottom;
            int t = (rw - 2 * gap) / 3;
            row({{b_sall, t}, {b_ssel, t}, {b_st42, t}});
            return 0;
        }
        case WM_HSCROLL: {
            for (size_t i = 0; i < sliders.size(); i++)
                if ((HWND)l == sliders[i]) {
                    int v = (int)SendMessageW(sliders[i], TBM_GETPOS, 0, 0);
                    auto& c = VbiRepair::controls()[i];
                    if (r.*c.v != v) { r.*c.v = v; changed(); }
                }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = (NMHDR*)l;
            if (nh->hwndFrom == meas && nh->code == NM_DBLCLK && src.go) src.go(curFrame());
            if (nh->hwndFrom == rng && nh->code == NM_DBLCLK) {
                int i = sel_range();
                if (i >= 0 && src.go) src.go(ranges[i].from);
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == ID_RESET) { r = VbiRepair(); set_sliders(); changed(); }
            else if (id >= ID_ACT0 && id < ID_ACT0 + (int)actions.size()) actions[id - ID_ACT0].second(r);
            else if (id == ID_SEL_START) { selFrom = curFrame(); if (selTo >= 0 && selTo < selFrom) selTo = -1; show_sel(); }
            else if (id == ID_SEL_END) { selTo = std::max(curFrame(), selFrom); show_sel(); }
            else if (id == ID_SEL_ALL) { selFrom = 0; selTo = -1; show_sel(); }
            else if (id == ID_GO_START) { if (src.go) src.go(selFrom); }
            else if (id == ID_SHIFT_SET) {
                int ln = (int)SendMessageW(sh_line, CB_GETCURSEL, 0, 0);
                int v = std::clamp(atoi(get_text(sh_val).c_str()), -64, 64);
                if (ln >= 0) { if (v) r.lineShift[ln] = v; else r.lineShift.erase(ln); changed(); }
            } else if (id == ID_SHIFT_CLEAR) { r.lineShift.clear(); changed(); }
            else if (id == ID_RANGE_ADD) { ranges.push_back({selFrom, selEnd(), r}); save_ranges(); show_ranges(); }
            else if (id == ID_RANGE_UPDATE) {
                int i = sel_range();
                if (i >= 0) { ranges[i] = {selFrom, selEnd(), r}; save_ranges(); show_ranges(); }
            } else if (id == ID_RANGE_LOAD) {
                int i = sel_range();
                if (i >= 0) { r = ranges[i].r; selFrom = ranges[i].from; selTo = ranges[i].to; set_sliders(); changed(); show_sel(); }
            } else if (id == ID_RANGE_DELETE) {
                int i = sel_range();
                if (i >= 0) { ranges.erase(ranges.begin() + i); save_ranges(); show_ranges(); }
            } else if (id == ID_MEASURE) measure();
            else if (id == ID_SAVE_ALL) save_vbi(false);
            else if (id == ID_SAVE_SEL) save_vbi(true);
            else if (id == ID_SAVE_T42) save_t42();
            else if (id == ID_VHSTTX) vhsttx_repair(hwnd, src.path, src.format);
            return 0;
        }
        case WM_NCDESTROY: { LRESULT res = Window::proc(m, w, l); delete this; return res; }
        }
        return Window::proc(m, w, l);
    }
};

} // namespace

bool vhsttx_repair(HWND owner, const std::string& vbi, int format)
{
    // the cards of VHSTTX 2.7 (teletext/vbi/config.py) the formats here match
    const char* card = nullptr;
    switch ((VbiFormat)format) {
    case VbiFormat::BttvPal: card = "bt8x8"; break;
    case VbiFormat::Cx88Pal: card = "cx88"; break;
    case VbiFormat::Saa713xPal: card = "saa7131"; break;
    case VbiFormat::TbcPal: card = "tbc-vbi"; break;
    case VbiFormat::TbcFullPal: card = "tbc"; break;
    default: break;
    }
    if (!card) {
        ask(owner, "VHSTTX VBI Repair", "VHSTTX knows only these 625-line layouts: bttv (2048 x 16 lines), cx88 (2048 x 18), "
            "SAA713x (1440 x 16), ld-decode/vhs-decode TBC (VBI crop or full fields). This recording is another one.", MB_ICONINFORMATION);
        return false;
    }
    Json st = load_state();
    std::string exe = st.get_str("vhsttx_exe");
    if (exe.empty() || !exists(exe)) {
        exe = open_file(owner, "Where is teletext.exe of VHSTTX (dist\\VHSTTX-Windows\\teletext.exe)?", {{"teletext.exe", "teletext.exe"}, {"Programs", "*.exe"}});
        if (exe.empty()) return false;
        st["vhsttx_exe"] = exe;
        save_state(st);
    }
    std::wstring cmd = L"\"" + widen(exe) + L"\" vbirepair -c " + widen(card) + L" \"" + widen(vbi) + L"\"";
    std::wstring dir = widen(exe.substr(0, exe.find_last_of("\\/")));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        ask(owner, "VHSTTX VBI Repair", "Could not start " + exe, MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

VbiRepair repair_saved()
{
    Json st = load_state();
    return repairOf(st.find("vbi_repair"));
}

HWND repair_window(HWND owner, const std::string& subtitle, std::function<void(const VbiRepair&)> onChange,
                   std::vector<std::pair<std::string, std::function<void(const VbiRepair&)>>> actions, const RepairSource* src)
{
    auto* w = new RepairWin;
    w->sub = subtitle;
    w->r = repair_saved();
    w->onChange = std::move(onChange);
    w->actions = std::move(actions);
    if (src && !src->path.empty()) {
        VbiFile vf;
        std::string err;
        if (vf.open(src->path, err, (VbiFormat)src->format) && !vf.geom().storedPerField) {
            w->hasSrc = true;
            w->src = *src;
            w->geom = vf.geom();
        }
    }
    w->create(L"TRRepair", "VBI repair", WS_OVERLAPPEDWINDOW, S(owner, w->hasSrc ? 1400 : 900), S(owner, w->hasSrc ? 900 : 680), owner);
    if (w->hwnd) ShowWindow(w->hwnd, SW_SHOW);
    if (w->onChange) w->onChange(w->r);   // the opener starts from the saved settings
    return w->hwnd;
}
}
