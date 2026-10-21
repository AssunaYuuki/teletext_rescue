// One window per decoded service (as src/gui_services.cpp in the Windows program):
//   StarSight and Gemstar programme grids, CC captions, XDS, the Silent Radio LED sign, AMOL,
//   Japanese teletext, the 5.73 Mbit/s data summary.
#include "draw.h"
#include "amol.h"
#include "gemstar.h"
#include "jtes.h"
#include "silentradio.h"
#include "slicer.h"
#include "ssjson.h"
#include "starsight.h"
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

namespace xg {
namespace {
const ImU32 C_CHAN = rgb(0x3a, 0x7b, 0xd5), C_TIME = rgb(0xff, 0xa3, 0x1a), C_PROG = rgb(0x2e, 0xa0, 0x6a), C_OTHER = rgb(0x8a, 0x8d, 0x93);
const ImU32 C_PAY = rgb(0x8e, 0x5c, 0xd9);

std::string wtitle(const std::string &t, const ResultPtr &r) { return t + " \xE2\x80\x94 " + ui::file_name(*r) + "##" + t + std::to_string((intptr_t)r.get()); }
bool focused() { return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows); }
bool key(ImGuiKey k) { return ImGui::IsKeyPressed(k, true); }

// a simple list: columns, rows given by a callback; returns the row clicked (or -1)
int list(const char *id, const std::vector<std::pair<const char *, float>> &cols, int rows, int sel,
         const std::function<std::string(int, int)> &cell, const std::function<ImU32(int)> &colour = nullptr, bool follow = false)
{
    int hit = -1;
    if (ImGui::BeginTable(id, (int)cols.size(), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (auto &c : cols) ImGui::TableSetupColumn(c.first, c.second > 0 ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch, c.second > 0 ? px(c.second) : 0);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(rows);
        if (follow && sel >= 0) clip.IncludeItemByIndex(sel);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                ImGui::TableNextRow();
                ImU32 c = colour ? colour(r) : FG;
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                for (int k = 0; k < (int)cols.size(); k++) {
                    ImGui::TableSetColumnIndex(k);
                    std::string s = cell(r, k);
                    if (k == 0) {
                        ImGui::PushID(r);
                        if (ImGui::Selectable(s.c_str(), r == sel, ImGuiSelectableFlags_SpanAllColumns)) hit = r;
                        ImGui::PopID();
                        if (follow && r == sel) ImGui::SetScrollHereY(0.5f);
                    } else
                        ImGui::TextUnformatted(s.c_str());
                }
                ImGui::PopStyleColor();
            }
        ImGui::EndTable();
    }
    return hit;
}

// ================================================================ StarSight programme guide
const int SPAN = 120;
struct GuideWin : Win {
    ResultPtr R;
    const SsDatabase *db = nullptr;
    std::map<int, std::vector<SsSlot>> slots;
    std::vector<int> chans;
    SsTime view = 0, sel_t = 0;
    int top = 0, sel = 0, vis = 1;
    float rowh = 30, chw = 130;
    Rc grid;

    int offset() const { return db->utcOffset(); }
    SsTime local(SsTime t) const { return t + offset(); }
    std::string tlabel(SsTime utc) const { int y, mo, d, h, mi, wd; ssCivil(local(utc), y, mo, d, h, mi, wd); return fmt("%d:%02d %s", h % 12 == 0 ? 12 : h % 12, mi, h < 12 ? "AM" : "PM"); }
    std::string dlabel(SsTime utc) const { int y, mo, d, h, mi, wd; ssCivil(local(utc), y, mo, d, h, mi, wd); return fmt("%s %d %s %d", DAYS[wd], d, MONTHS[mo - 1], y); }
    std::string call(int id) const
    {
        auto it = db->channels.find(id);
        if (it != db->channels.end() && !it->second.name.empty()) return it->second.callLetters();
        std::string lab, cl;
        if (ssKnownChannel(id, lab, cl)) return !lab.empty() ? lab : cl.substr(0, 5);
        return fmt("%d", id);
    }
    std::string cname(int id) const
    {
        auto it = db->channels.find(id);
        if (it != db->channels.end() && !it->second.name.empty()) return it->second.name;
        std::string lab, cl;
        if (ssKnownChannel(id, lab, cl)) return cl.empty() ? lab : cl;
        return fmt("channel %d", id);
    }
    void build()
    {
        db = &R->db;
        for (int id : db->channelIds()) { auto v = db->slotsFor(id); if (!v.empty()) slots[id] = std::move(v); }
        for (auto &kv : slots) chans.push_back(kv.first);
        std::stable_sort(chans.begin(), chans.end(), [this](int a, int b) {
            bool na = db->channels.count(a) && !db->channels.at(a).name.empty(), nb = db->channels.count(b) && !db->channels.at(b).name.empty();
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
        bool any = false;
        SsTime next = INT64_MAX;
        for (auto &kv : slots)
            for (auto &s : kv.second) {
                if (s.start < t + SPAN && s.start + s.duration > t) any = true;
                if (s.start >= t && !(s.flags & 0x01)) next = std::min(next, s.start);
            }
        if (!any && next != INT64_MAX) t = next;
        view = local(t) / 30 * 30 - offset();
        sel_t = t;
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
    float x_of(SsTime t) const { float x0 = grid.l + chw; return x0 + (float)(t - view) * (grid.r - x0) / SPAN; }
    void move(int dr, int dt)
    {
        if (chans.empty()) return;
        if (dr) {
            sel = std::max(0, std::min((int)chans.size() - 1, sel + dr));
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
        }
        if (dt) {
            const SsSlot *s = selected();
            sel_t = dt > 0 ? (s ? s->start + s->duration : sel_t + 30) : (s ? s->start - 1 : sel_t - 30);
            const SsSlot *n = selected();
            if (n && dt < 0) sel_t = std::max(n->start, view - 30 * 8);
        }
        while (sel_t >= view + SPAN) view += 30;
        while (sel_t < view) view -= 30;
    }
    void draw() override
    {
        header("StarSight", fmt("programme guide \xC2\xB7 %zu channels", chans.size()));
        int b = header_buttons({"Now", "Export JSON\xE2\x80\xA6", "Text report"}, {80, 130, 120});
        if (b == 0) now();
        if (b == 1) {
            ResultPtr r = R;
            open_folder_dialog("Folder for the JSON", dirname(R->path), [r](std::string d) {
                std::string err, p = path_join(d, stem_path(ui::file_name(*r)) + ".json");
                if (!ssExportJson(p, r->db, r.get(), err)) message("Export", err); else message("Export", "Written: " + p);
            });
        }
        if (b == 2) {
            std::string s = summarize(*R) + "\n=== StarSight command log ===\n";
            for (auto &l : R->db.log) s += l + "\n";
            text_window("StarSight \xE2\x80\x94 " + ui::file_name(*R), "packets, repairs and every command received", replace_all(s, "\r\n", "\n"));
        }
        if (focused()) {
            if (key(ImGuiKey_UpArrow)) move(-1, 0);
            if (key(ImGuiKey_DownArrow)) move(1, 0);
            if (key(ImGuiKey_LeftArrow)) move(0, -1);
            if (key(ImGuiKey_RightArrow)) move(0, 1);
            if (key(ImGuiKey_PageUp)) move(-vis, 0);
            if (key(ImGuiKey_PageDown)) move(vis, 0);
            if (key(ImGuiKey_Home)) now();
            if (key(ImGuiKey_Tab)) { int d = ImGui::GetIO().KeyShift ? -1440 : 1440; view += d; sel_t += d; }
        }
        rowh = px(30); chw = px(130);
        Rc card = take(px(110));
        ImGui::Dummy(ImVec2(0, px(4)));
        Rc days = take(px(28));
        Rc ruler = take(px(26));
        float avail = ImGui::GetContentRegionAvail().y;
        grid = take(std::max(rowh, avail - px(160)));
        ImGui::Dummy(ImVec2(0, px(4)));
        Rc info = take(-1);
        vis = std::max(1, (int)(grid.h() / rowh));
        // card
        fillr(card, PANEL);
        float x = card.l + px(18);
        text(x, card.t + px(10), "StarSight", C_TIME, BOLD, 24);
        SsTime clock = db->nowTime();
        text(x, card.b - px(44), clock ? "broadcast clock " + dlabel(clock) + "  " + tlabel(clock) : "no broadcast clock in this recording", ACCENT2, NORM, 9);
        text(x, card.b - px(24), "on-screen programme guide for TV sets and VCRs, carried by PBS stations; listings from the show-list packets", DIM, NORM, 9);
        const SsStats &st = db->stats;
        int good = st.crcOk + st.fixedSingle + st.fixedChase + st.fixedCombine;
        tiles(card, {{fmt("%d", st.packets), "packets"}, {fmt("%d", good), "good (CRC)"}, {fmt("%zu", chans.size()), "channels"},
                     {fmt("%zu", db->showLists.size()), "show lists"}, {fmt("%zu", db->programs.size()), "titles"}});
        // days
        int64_t today = local(clock ? clock : view) / 1440, vday = local(view) / 1440;
        float dw = (days.w() - chw) / 7;
        text(days.l, days.t + px(6), dlabel(view).substr(0, dlabel(view).rfind(' ')), FG, BOLD, 10);
        ImVec2 mp = ImGui::GetMousePos();
        bool click = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0);
        for (int i = 0; i < 7; i++) {
            Rc r{days.l + chw + i * dw + 1, days.t, days.l + chw + (i + 1) * dw - 2, days.b};
            int64_t day = today + i;
            int y, mo, d, h, mi, wd;
            ssCivil(day * 1440, y, mo, d, h, mi, wd);
            bool cur = day == vday;
            fillr(r, cur ? ACCENT : FIELD);
            text_in(r, fmt("%s %d", DAYS[wd], d), cur ? BG : FG, cur ? BOLD : NORM, 10, CENTER);
            if (click && r.in(mp)) {
                SsTime clk = clock ? clock : view, tod = local(view) % 1440;
                view = local(clk) / 1440 * 1440 + i * 1440 + tod - offset();
                sel_t = view;
            }
        }
        // ruler
        fillr(ruler, FIELD);
        for (int k = 0; k < SPAN / 30; k++) {
            SsTime t = view + k * 30;
            float xx = x_of(t);
            if (k) fillr({xx, ruler.t + px(4), xx + 1, ruler.b - px(4)}, LINE);
            text(xx + px(8), ruler.t + px(5), tlabel(t), MUTED, NORM, 10);
        }
        // grid
        fillr(grid, PANEL);
        if (ImGui::IsWindowHovered() && grid.in(mp)) {
            float wh = ImGui::GetIO().MouseWheel;
            if (wh != 0) move(wh > 0 ? -3 : 3, 0);
        }
        if (click && grid.in(mp) && mp.x > grid.l + chw) {
            int r = (int)((mp.y - grid.t) / rowh);
            if (top + r < (int)chans.size()) { sel = top + r; sel_t = view + (SsTime)((mp.x - grid.l - chw) * SPAN / (grid.w() - chw)); }
        }
        SsTime nowt = clock;
        for (int r = 0; r < vis; r++) {
            int idx = top + r;
            if (idx >= (int)chans.size()) break;
            Rc row{grid.l, grid.t + r * rowh, grid.r, grid.t + (r + 1) * rowh};
            int ch = chans[idx];
            bool selrow = idx == sel;
            Rc cc{row.l, row.t + 1, row.l + chw - px(6), row.b - 1};
            fillr(cc, selrow ? LINE : FIELD);
            bool pay = false;
            for (auto &s : slots[ch]) pay |= (s.flags & 0x40) != 0;
            fillr({cc.l, cc.t, cc.l + px(4), cc.b}, pay ? C_PAY : C_CHAN);
            Rc ct{cc.l + px(12), cc.t, cc.r - px(4), cc.b};
            text_in(ct, call(ch), FG, BOLD, 10, LEFT);
            text_in(ct, fmt("%d", ch % 1000), DIM, NORM, 9, RIGHT);
            for (auto &s : slots[ch]) {
                if (s.start + s.duration <= view || s.start >= view + SPAN) continue;
                float x0 = std::max(x_of(s.start), grid.l + chw), x1 = std::min(x_of(s.start + s.duration), grid.r);
                Rc b{x0 + 1, row.t + 1, x1 - 1, row.b - 1};
                bool is_sel = selrow && sel_t >= s.start && sel_t < s.start + s.duration;
                bool past = nowt && s.start + s.duration <= nowt;
                fillr(b, is_sel ? ACCENT : past ? rgb(0x29, 0x2b, 0x30) : FIELD);
                if (s.flags & 0x40) fillr({b.l, b.b - px(3), b.r, b.b}, C_PAY);
                std::string t = R->db.titleOf(s);
                if (s.start < view) t = "\xE2\x97\x80 " + t;
                text_in({b.l + px(8), b.t, b.r - px(4), b.b}, t, is_sel ? BG : past ? MUTED : FG, is_sel ? BOLD : NORM, 10, LEFT);
            }
            fillr({grid.l + chw, row.b - 1, grid.r, row.b}, BG);
        }
        if (nowt && nowt >= view && nowt < view + SPAN) { float xx = x_of(nowt); fillr({xx - 1, ruler.t, xx + 1, grid.b}, C_TIME); }
        if (chans.empty()) text_in(grid, "No schedules were received in this recording.", MUTED, NORM, 10, CENTER);
        // the selected show
        fillr(info, PANEL);
        const SsSlot *s = selected();
        float ix = info.l + px(18), iy = info.t + px(10);
        if (!s) { if (!chans.empty()) text(ix, iy, "no show at this time on " + cname(chans[sel]), MUTED); return; }
        text(ix, iy, R->db.titleOf(*s), FG, BOLD, 14);
        iy += px(30);
        std::string when = tlabel(s->start) + " \xE2\x80\x93 " + tlabel(s->start + s->duration) + "   \xC2\xB7   " + dlabel(s->start) + "   \xC2\xB7   " + cname(chans[sel]) + fmt("  (%d)", chans[sel] % 1000);
        text(ix, iy, when, ACCENT2);
        for (auto &n : slots[chans[sel]])
            if (n.start >= s->start + s->duration) { text(ix + text_w(when, NORM, 10) + px(30), iy, "next  " + tlabel(n.start) + "  " + R->db.titleOf(n), MUTED); break; }
        iy += px(22);
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
        text(ix, iy, attrs, MUTED, NORM, 9);
        iy += px(22);
        std::string desc;
        if (d && !d->text.empty()) desc = d->partial ? d->text + "\xE2\x80\xA6" : d->text;
        else if (d && d->compressed) desc = "The description is Huffman-compressed with codes not recovered yet.";
        else if (s->description) desc = fmt("Description #%d was not received in this recording.", s->description);
        else desc = "No description was sent for this show.";
        if (d && !d->advisories.empty()) { desc += "\nAdvisory:"; for (auto &a : d->advisories) desc += " " + a; }
        text_wrap({ix, iy, info.r - px(18), info.b - px(8)}, desc, d && !d->text.empty() ? FG : DIM, NORM, 10);
    }
};

// ================================================================ CC captions
struct CapWin : Win {
    ResultPtr R;
    struct Ln { std::string svc, text; };
    std::vector<Ln> all;
    std::vector<std::string> svcs;
    std::string cur, where, summary;
    std::vector<int> shown;
    int sel = 0;
    bool follow = false;
    void load()
    {
        for (auto &li : R->lines) if (li.kind == "Captions" || li.kind.rfind("XDS", 0) == 0) where += (where.empty() ? "" : ", ") + fmt("line %d field %d", li.lineNo, li.field);
        std::map<std::string, int> n;
        if (!R->captionCues.empty())   // with times: the tape's VITC or the place in the recording
            for (auto &c : R->captionCues) {
                all.push_back({c.svc, "[" + captionTime(*R, c.frame) + "] " + c.text});
                if (!n[c.svc]++) svcs.push_back(c.svc);
            }
        else
            for (auto &l : R->captions) {
                auto c = l.find(": ");
                if (l.rfind("---", 0) == 0 || c == std::string::npos || c > 4) continue;
                all.push_back({l.substr(0, c), l.substr(c + 2)});
                if (!n[l.substr(0, c)]++) svcs.push_back(l.substr(0, c));
            }
        std::sort(svcs.begin(), svcs.end(), [](const std::string &a, const std::string &b) { return (a[0] == 'C') != (b[0] == 'C') ? a[0] == 'C' : a < b; });
        if (!svcs.empty()) cur = svcs[0];
        summary = fmt("%zu lines", all.size());
        for (auto &k : svcs) summary += fmt("  \xC2\xB7  %s %d", k.c_str(), n[k]);
        summary += "  \xC2\xB7  CC = captions, T = text mode";
        refill();
    }
    void refill()
    {
        shown.clear();
        for (size_t i = 0; i < all.size(); i++) if (all[i].svc == cur) shown.push_back((int)i);
        sel = 0;
    }
    void draw() override
    {
        header("CC captions", where);
        if (header_buttons({"Text report"}, {120}) == 0) {
            std::string s;
            for (auto &x : R->captions) s += x + "\n";
            text_window("CC captions \xE2\x80\x94 " + ui::file_name(*R), "every caption and text-mode line in the order received", s);
        }
        for (size_t i = 0; i < svcs.size(); i++) {
            if (i) ImGui::SameLine();
            bool on = svcs[i] == cur;
            if (on ? accent_button(svcs[i].c_str(), ImVec2(px(64), 0)) : ImGui::Button(svcs[i].c_str(), ImVec2(px(64), 0))) { cur = svcs[i]; refill(); }
        }
        if (focused() && !shown.empty()) {
            if (key(ImGuiKey_UpArrow)) { sel = std::max(0, sel - 1); follow = true; }
            if (key(ImGuiKey_DownArrow)) { sel = std::min((int)shown.size() - 1, sel + 1); follow = true; }
        }
        // the TV screen with the caption box
        Rc screen = take(px(170));
        fillr(screen, rgb(0x11, 0x11, 0x11));
        Rc in = screen.inset(px(10));
        fillr(in, rgb(0x1d, 0x22, 0x2c));
        if (!cur.empty()) text(in.r - px(12), in.t + px(8), cur + fmt("  \xC2\xB7  line %d of %zu", sel + 1, shown.size()), MUTED, NORM, 9, RIGHT);
        if (!shown.empty()) {
            float lh = px(30);
            int n = std::min(3, sel + 1);
            float y = in.b - px(18) - n * lh;
            for (int k = sel - n + 1; k <= sel; k++) {
                const std::string &t = all[shown[k]].text;
                float w = text_w(t, MONO, 15);
                float x = (in.l + in.r - w) / 2;
                fillr({x - px(8), y, x + w + px(8), y + lh - px(2)}, rgb(0, 0, 0));
                text(x, y + px(3), t, k == sel ? rgb(255, 255, 255) : rgb(0xb8, 0xb8, 0xb8), MONO, 15);
                y += lh;
            }
        }
        wrapped(ACCENT2, summary);
        int h = list("##cap", {{"#", 60}, {"Service", 70}, {"Text", -1}}, (int)shown.size(), sel, [this](int r, int c) {
            return c == 0 ? fmt("%d", r + 1) : c == 1 ? all[shown[r]].svc : all[shown[r]].text;
        }, nullptr, follow);
        follow = false;
        if (h >= 0) sel = h;
    }
};

// ================================================================ XDS
struct XdsWin : Win {
    ResultPtr R;
    struct Ev { std::string cls, key, val; };
    std::vector<Ev> ev;
    std::map<std::string, std::string> topv;
    std::vector<std::pair<Ev, int>> rows;
    void load()
    {
        for (auto &l : R->xds) {
            if (l.rfind("XDS ", 0) != 0) continue;
            std::string s = l.substr(4);
            size_t sp = s.find(' ');
            Ev e;
            e.cls = s.substr(0, sp);
            std::string rest = sp == std::string::npos ? "" : s.substr(sp + 1);
            size_t c = rest.find(':');
            if (rest.rfind("time of day", 0) == 0) { e.key = "time of day"; e.val = strip(rest.substr(11)); }
            else if (rest.rfind("local time zone", 0) == 0) { e.key = "time zone"; e.val = strip(rest.substr(15)); }
            else if (c != std::string::npos) { e.key = strip(rest.substr(0, c)); e.val = strip(rest.substr(c + 1)); }
            else e.key = rest;
            ev.push_back(e);
            topv[e.key] = e.val;
        }
        std::map<std::string, size_t> at;
        for (auto &e : ev) {
            std::string k = e.cls + "|" + e.key + "|" + e.val;
            auto it = at.find(k);
            if (it == at.end()) { at[k] = rows.size(); rows.push_back({e, 1}); } else rows[it->second].second++;
        }
    }
    std::string get(const char *k) const { auto it = topv.find(k); return it == topv.end() ? "" : it->second; }
    ImU32 colour(const Ev &e) const
    {
        if (e.cls == "Channel") return C_CHAN;
        if (e.key == "time of day" || e.key == "time zone") return C_TIME;
        if (e.cls == "Current" || e.cls == "Future") return C_PROG;
        return FG;
    }
    void draw() override
    {
        header("XDS", "extended data services \xC2\xB7 line 21 field 2");
        if (header_buttons({"Text report"}, {120}) == 0) {
            std::string s;
            for (auto &x : R->xds) s += x + "\n";
            text_window("XDS \xE2\x80\x94 " + ui::file_name(*R), "every XDS packet in the order received", s);
        }
        Rc top = take(px(140));
        float cw = std::max(px(300), top.w() * 2 / 5);
        Rc card{top.l, top.t, top.l + cw, top.b}, clock{card.r + px(10), top.t, top.r, top.b};
        fillr(card, PANEL);
        std::string call = get("call letters"), net = get("network");
        if (call.empty()) call = get("station");
        text(card.l + px(18), card.t + px(10), call.empty() ? (net.empty() ? "\xE2\x80\x94" : net) : call, FG, BOLD, 30);
        if (!net.empty() && !call.empty()) {
            Rc nb{card.l + px(20), card.t + px(76), card.l + px(36) + text_w(net, BOLD, 10), card.t + px(104)};
            fillr(nb, C_CHAN);
            text(nb.l + px(8), nb.t + px(4), net, rgb(255, 255, 255), BOLD, 10);
        }
        std::string tz = get("time zone");
        text(card.l + px(20), card.b - px(26), tz.empty() ? "station data from XDS" : "time zone " + tz, MUTED, NORM, 9);
        fillr(clock, rgb(0x11, 0x11, 0x11));
        Rc in = clock.inset(px(10));
        fillr(in, rgb(0, 0, 0));
        int h = 0, mi = 0, mo = 0, d = 0, y = 0, z = 0;
        std::string t = get("time of day");
        if (sscanf(t.c_str(), "%d:%d UTC, %d/%d/%d", &h, &mi, &mo, &d, &y) == 5) {
            int64_t days = 0;
            {   // days from civil
                int yy = y - (mo <= 2);
                int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
                unsigned yoe = (unsigned)(yy - era * 400), doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                days = era * 146097 + (int64_t)doe - 719468;
            }
            int64_t s = days * 86400 + h * 3600 + mi * 60;
            bool local = sscanf(tz.c_str(), "UTC%d", &z) == 1;
            if (local) s += (int64_t)z * 3600 + (tz.find("DST") != std::string::npos ? 3600 : 0);
            time_t tt = (time_t)s;
            std::tm l = *gmtime(&tt);
            text(in.l + px(18), in.t + px(8), fmt("%02d:%02d", l.tm_hour, l.tm_min), C_TIME, MONO, 30);
            text(in.l + px(20), in.t + px(70), fmt("%s %d %s %d", DAYS[l.tm_wday], l.tm_mday, MONTHS[l.tm_mon], l.tm_year + 1900) + (local ? "  local" : "  UTC"), ACCENT2, MONO, 12);
            if (local) text(in.r - px(16), in.t + px(74), fmt("%02d:%02d UTC", h, mi), MUTED, NORM, 9, RIGHT);
        } else
            text(in.l + px(18), in.t + px(20), "no time-of-day packets", MUTED, BOLD, 13);
        text(in.r - px(16), in.t + px(10), "last time packet in the recording", DIM, NORM, 9, RIGHT);
        ImGui::Dummy(ImVec2(0, px(4)));
        Rc prog = take(px(90));
        fillr(prog, PANEL);
        std::string pn = get("program name");
        if (pn.empty()) pn = get("programme name");
        float yy = prog.t + px(10), x = prog.l + px(18);
        text(x, yy, "On air", MUTED, NORM, 9);
        yy += px(18);
        text(x, yy, pn.empty() ? "the station sent no programme name" : pn, pn.empty() ? DIM : FG, BOLD, 13);
        yy += px(30);
        std::string meta;
        for (auto &kv : topv)
            if (kv.first.find("rating") != std::string::npos || kv.first.find("length") != std::string::npos || kv.first.find("type") != std::string::npos)
                meta += (meta.empty() ? "" : "   \xC2\xB7   ") + kv.first + " " + kv.second;
        text(x, yy, meta, ACCENT2, NORM, 9);
        ImGui::Dummy(ImVec2(0, px(4)));
        list("##xds", {{"Class", 90}, {"Packet", 170}, {"Value", -1}, {"Times", 60}}, (int)rows.size(), -1, [this](int r, int c) {
            auto &e = rows[r];
            return c == 0 ? e.first.cls : c == 1 ? e.first.key : c == 2 ? e.first.val : fmt("%d", e.second);
        }, [this](int r) { return colour(rows[r].first); });
    }
};

// ================================================================ Silent Radio LED sign
struct SignWin : Win {
    ResultPtr R;
    std::vector<std::string> zones;
    int zi = 0;
    std::vector<int> items;
    std::vector<uint16_t> state = std::vector<uint16_t>(SR_W, 0), prev = std::vector<uint16_t>(SR_W, 0);
    int it = 0, fr = 0;
    bool playing = true, follow = false;
    double t0 = 0, el = 0;
    std::string where, caption;
    int packets = 0;
    static double now() { return SDL_GetTicks() / 1000.0; }
    const SilentRadioMessage &item() { return R->silentRadio[items[it]]; }
    void init()
    {
        for (auto &li : R->lines) if (li.kind.rfind("Silent Radio", 0) == 0) where = fmt("line %d", li.lineNo);
        std::set<std::string> zs;
        for (auto &x : R->silentRadio) { zs.insert(x.zone); packets += x.seq >= 0; }
        zones.assign(zs.begin(), zs.end());
        std::sort(zones.begin(), zones.end(), [](const std::string &a, const std::string &b) { return a.empty() != b.empty() ? !a.empty() : a < b; });
        select();
    }
    void select()
    {
        items.clear();
        for (size_t i = 0; i < R->silentRadio.size(); i++)
            if (zones.empty() || R->silentRadio[i].zone == zones[zi]) items.push_back((int)i);
        go(0);
    }
    void go(int i)
    {
        int n = (int)items.size();
        if (!n) { caption = "The sign carrier is there, but no messages were sent while recording."; return; }
        it = ((i % n) + n) % n; fr = 0; t0 = now(); el = 0;
        follow = true;
        caption.clear();
        for (auto &t : item().texts) caption += (caption.empty() ? "" : "  \xC2\xB7  ") + t;
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
        state = item().frames[fr].at(el, prev);
    }
    void draw() override
    {
        tick();
        header("Silent Radio", "LED news sign \xC2\xB7 " + where + fmt(" \xC2\xB7 %d sign packets, %zu zones", packets, zones.size()));
        std::string zl = zones.empty() ? "zone" : (zones[zi].empty() ? std::string("other") : "zone " + zones[zi]) + " \xE2\x96\xBE";
        int b = header_buttons({zl, "\xE2\x97\x80\xE2\x97\x80", playing ? "Pause" : "Play", "\xE2\x96\xB6\xE2\x96\xB6"}, {100, 40, 100, 40});
        if (b == 0 && !zones.empty()) { zi = (zi + 1) % zones.size(); select(); }
        if (b == 1) go(it - 1);
        if (b == 2) { playing = !playing; if (playing) t0 = now() - el; }
        if (b == 3) go(it + 1);
        float d = std::max(3.f, std::min(px(8), (ImGui::GetContentRegionAvail().x - px(20)) / SR_W));
        Rc sign = take(d * SR_H + px(20));
        sign.r = sign.l + d * SR_W + px(20);
        fillr(sign, rgb(0x11, 0x11, 0x11));
        Rc in{sign.l + px(10), sign.t + px(10), sign.l + px(10) + d * SR_W, sign.t + px(10) + d * SR_H};
        fillr(in, rgb(0, 0, 0));
        for (int x = 0; x < SR_W; x++)
            for (int y = 0; y < SR_H; y++)
                DL()->AddCircleFilled(ImVec2(in.l + x * d + d / 2, in.t + y * d + d / 2), d * 0.42f, (state[x] >> y) & 1 ? rgb(0xff, 0xa3, 0x1a) : rgb(0x2a, 0x16, 0x06), 8);
        wrapped(ACCENT2, caption);
        double rate = ui::frame_rate(*R);
        int h = list("##sr", {{"Recording", 100}, {"Packet", 60}, {"Copies", 60}, {"Text", -1}}, (int)items.size(), it, [this, rate](int r, int c) {
            auto &m = R->silentRadio[items[r]];
            return c == 0 ? ui::rec_time(m.frame / rate) : c == 1 ? (m.seq >= 0 ? fmt("%04X", m.seq) : std::string("\xE2\x80\x94")) : c == 2 ? (m.copies > 1 ? fmt("%d", m.copies) : std::string()) : m.text;
        }, nullptr, follow);
        follow = false;
        if (h >= 0) { go(h); follow = false; }
    }
};

// ================================================================ AMOL
struct AmolWin : Win {
    ResultPtr R;
    const AmolStream *A = nullptr;
    int sel = -1;
    bool stamps = true;
    const AmolStamp *cur() const { return sel >= 0 && sel < (int)A->stamps.size() ? &A->stamps[sel] : nullptr; }
    void init()
    {
        for (auto &a : R->amol) if (!A || (A->stamps.empty() && !a.stamps.empty())) A = &a;
        static const AmolStream none;
        if (!A) A = &none;
        stamps = !A->stamps.empty();
        if (stamps) sel = 0;
    }
    void draw() override
    {
        header("AMOL", fmt("Nielsen automated measurement of lineups \xC2\xB7 line %d field %d", A->lineNo, A->field));
        if (header_buttons({"Text report"}, {120}) == 0) {
            std::string s;
            for (auto &a : R->amol) for (auto &x : a.report) s += x + "\n";
            s += "\n";
            for (auto &a : R->amol) for (auto &t : a.stamps) s += ui::rec_time(t.frame / ui::frame_rate(*R)) + "  " + t.text() + (t.parityOk ? "" : "  (parity error, copies agree)") + "\n";
            text_window("AMOL \xE2\x80\x94 " + ui::file_name(*R), "Nielsen AMOL I words, timestamps and other frame addresses", s);
        }
        const AmolStamp *t = cur();
        Rc top = take(px(140));
        float cw = std::max(px(300), top.w() / 2);
        Rc card{top.l, top.t, top.l + cw, top.b}, clock{card.r + px(10), top.t, top.r, top.b};
        fillr(card, PANEL);
        int sid = t ? t->sid : (!A->stamps.empty() ? A->stamps[0].sid : -1);
        text(card.l + px(18), card.t + px(10), sid >= 0 ? fmt("SID %d", sid) : "\xE2\x80\x94", FG, BOLD, 30);
        std::string net = sid >= 0 ? amolSourceName(sid) : "";
        if (!net.empty()) {
            Rc nb{card.l + px(20), card.t + px(76), card.l + px(36) + text_w(net, BOLD, 10), card.t + px(104)};
            fillr(nb, service_colour("AMOL"));
            text(nb.l + px(8), nb.t + px(4), net, rgb(0, 0, 0), BOLD, 10);
            text(nb.r + px(10), nb.t + px(5), "network feed (same ID on all its recordings)", MUTED, NORM, 9);
        }
        text(card.l + px(20), card.b - px(26), fmt("%lld words \xC2\xB7 %lld with data \xC2\xB7 %lld parity errors \xC2\xB7 %zu seconds stamped", (long long)A->words,
                                                   (long long)(A->parityOk + A->parityBad), (long long)A->parityBad, A->stamps.size()), MUTED, NORM, 9);
        fillr(clock, rgb(0x11, 0x11, 0x11));
        Rc in = clock.inset(px(10));
        fillr(in, rgb(0, 0, 0));
        if (t) {
            text(in.l + px(18), in.t + px(8), fmt("%d:%02d:%02d", t->hour, t->minute, t->second), service_colour("AMOL"), MONO, 30);
            text(in.l + px(20), in.t + px(70), fmt("%s %d  %s", MONTHS[(t->month + 11) % 12], t->day, t->pm ? "PM" : "AM"), ACCENT2, MONO, 12);
            text(in.r - px(16), in.t + px(74), ui::rec_time(t->frame / ui::frame_rate(*R)) + " in the recording", MUTED, NORM, 9, RIGHT);
        } else
            text(in.l + px(18), in.t + px(20), "no timestamps on this line", MUTED, BOLD, 13);
        text(in.r - px(16), in.t + px(10), "station clock, frame addresses 2/3", DIM, NORM, 9, RIGHT);
        ImGui::Dummy(ImVec2(0, px(4)));
        Rc strip = take(px(100));
        fillr(strip, PANEL);
        struct Fd { const char *name; int len; ImU32 c; };
        static const Fd fields[] = {{"Ref", 3, rgb(0xf2, 0xe0, 0x3c)}, {"Start", 4, rgb(0xa8, 0x2a, 0x2a)}, {"Frame", 5, rgb(0x3a, 0x5b, 0xff)},
                                    {"Source", 6, rgb(0xff, 0x40, 0x40)}, {"Month", 4, rgb(0x3c, 0xcf, 0x3c)}, {"Day", 5, rgb(0xff, 0x3c, 0xf0)},
                                    {"Hour", 4, rgb(0x3c, 0xe8, 0xe8)}, {"Minute", 6, rgb(0xb0, 0x4c, 0xc8)}, {"Second", 6, rgb(0x2e, 0xa8, 0xe8)},
                                    {"PM", 1, rgb(0x2e, 0xa0, 0x6a)}, {"Spare", 3, rgb(0xa8, 0xa0, 0x20)}, {"P", 1, rgb(0x2e, 0x80, 0x50)}};
        uint64_t w = t ? t->bits : 0;
        float x0 = strip.l + px(16), cwb = std::max(4.f, (strip.w() - px(32)) / kAmolBits);
        float ty = strip.t + px(10), cy = ty + px(22), ch = px(22);
        int bit = 0;
        for (auto &f : fields) {
            float x = x0 + bit * cwb, wpx = f.len * cwb;
            text(x + wpx / 2, ty, f.name, f.c, NORM, 9, CENTER);
            for (int k = 0; k < f.len; k++, bit++) {
                int v = t ? (int)((w >> (kAmolBits - 1 - bit)) & 1) : 0;
                Rc r{x0 + bit * cwb + 1, cy, x0 + (bit + 1) * cwb - 1, cy + ch};
                fillr(r, f.c);
                fillr(r.inset(2), v ? rgb(0xe8, 0xe8, 0xe8) : rgb(0x10, 0x10, 0x10));
                if (t) text((r.l + r.r) / 2, cy + ch + px(2), v ? "1" : "0", f.c, MONO, 10, CENTER);
            }
            if (t) {
                int val = amolField(w, bit - f.len, f.len);
                std::string s = f.len >= 4 && strcmp(f.name, "Ref") && strcmp(f.name, "Start") && strcmp(f.name, "Spare") ? fmt("%d", val) : !strcmp(f.name, "PM") ? (val ? "PM" : "AM") : "";
                if (!strcmp(f.name, "P")) s = amolParityOk(w) ? "ok" : "bad";
                text(x + wpx / 2, cy + ch + px(20), s, f.c, BOLD, 10, CENTER);
            }
        }
        ImGui::Dummy(ImVec2(0, px(4)));
        double fr = ui::frame_rate(*R);
        if (stamps) {
            int h = list("##amol", {{"Recording", 110}, {"Source", 90}, {"Date", 80}, {"Station time", 120}, {"Parity", -1}}, (int)A->stamps.size(), sel, [this, fr](int r, int c) {
                auto &t = A->stamps[r];
                std::string net = amolSourceName(t.sid);
                return c == 0 ? ui::rec_time(t.frame / fr) : c == 1 ? fmt("%d", t.sid) + (net.empty() ? "" : " " + net) : c == 2 ? fmt("%s %d", MONTHS[(t.month + 11) % 12], t.day)
                     : c == 3 ? fmt("%d:%02d:%02d %s", t.hour, t.minute, t.second, t.pm ? "PM" : "AM") : std::string(t.parityOk ? "ok" : "copies agree");
            }, [this](int r) { return A->stamps[r].parityOk ? FG : ACCENT; });
            if (h >= 0) sel = h;
        } else
            list("##amolo", {{"Frame address", 110}, {"Payload (35 bits)", 160}, {"Times", 80}, {"First seen", -1}}, (int)A->other.size(), -1, [this, fr](int r, int c) {
                auto &o = A->other[r];
                return c == 0 ? fmt("%d", o.fa) : c == 1 ? fmt("%09llx", (unsigned long long)o.payload) : c == 2 ? fmt("%d", o.count) : ui::rec_time(o.firstFrame / fr);
            });
    }
};

// ================================================================ Gemstar GUIDE Plus+
const ImU32 C_MOVIE = rgb(0x8e, 0x5c, 0xd9), C_KIDS = rgb(0x2e, 0xa0, 0x6a), C_SHOW = rgb(0x3a, 0x7b, 0xd5);
int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int &y, int &m, int &d, int &wd)
{
    wd = (int)(((z % 7) + 11) % 7);
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
    int64_t t = 0, dur = 0;
    std::string title, category, year, rating, cast, desc;
    int stars = 0;
    int64_t code = -1, frame = 0;
    bool movie() const { return !year.empty(); }
    bool kids() const { return rating.rfind("TVY", 0) == 0; }
    ImU32 colour() const { return movie() ? C_MOVIE : kids() ? C_KIDS : C_SHOW; }
};
GemShow gem_show(const GemstarShow &s)
{
    GemShow g;
    g.title = s.title;
    std::vector<std::string> lines;
    for (auto *src : {&s.line1, &s.line2})
        for (auto &l : split(*src, '\n')) { std::string t = trim_sp(l); if (!t.empty() && t != "&") lines.push_back(t); }
    if (g.title.size() > 3 && g.title.compare(g.title.size() - 3, 3, "...") == 0)
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].rfind("...", 0) == 0) { g.title = trim_sp(g.title.substr(0, g.title.size() - 3)) + " " + trim_sp(lines[i].substr(3)); lines.erase(lines.begin() + i); break; }
    for (size_t i = 0; i < lines.size(); i++) {
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
        if (yi < 0 && (rating.empty() || !l.empty())) continue;
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

struct GemWin : Win {
    ResultPtr R;
    std::vector<GemShow> shows;
    std::map<int, std::vector<int>> rows;
    std::vector<int> all, chans;
    bool busy = true;
    std::string keyNote;
    std::vector<int64_t> dayList;
    GemstarClock clk;
    int zone = 5, span = 240, top = 0, sel = 0, vis = 1;
    int64_t view = 0, sel_t = 0;
    float rowh = 32, chw = 100;
    Rc grid;
    struct Row { std::string t, where, kind, block, host, content; int copies; ImU32 c; };
    std::vector<Row> packets;

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
        if (shows.empty()) fill_list();
    }
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
        sel_t = view; sel = 0; top = 0;
        chans.clear();
        refresh();
        if (!chans.empty() && !at(chans[sel], sel_t))
            for (int i : rows[chans[sel]]) if (shows[i].t >= view) { sel_t = shows[i].t; break; }
    }
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
        top = std::max(0, std::min(top, std::max(0, (int)chans.size() - vis)));
    }
    const GemShow *at(int ch, int64_t t) const
    {
        auto it = rows.find(ch);
        if (it == rows.end()) return nullptr;
        for (int i : it->second) if (t >= shows[i].t && t < shows[i].t + shows[i].dur) return &shows[i];
        return nullptr;
    }
    const GemShow *selected() const { return chans.empty() ? nullptr : at(chans[sel], sel_t); }
    float x_of(int64_t t) const { float x0 = grid.l + chw; return x0 + (float)(t - view) * (grid.r - x0) / span; }
    bool row_has_view(int ch) const
    {
        for (int i : rows.at(ch)) if (shows[i].t < view + span && shows[i].t + shows[i].dur > view) return true;
        return false;
    }
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
        if (sel >= top + vis) top = sel - vis + 1;
    }
    void move(int dr, int dt)
    {
        if (chans.empty()) return;
        if (dr) sel = std::max(0, std::min((int)chans.size() - 1, sel + dr));
        if (dt) {
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
        fit();
    }
    float pill(float x, float y, const std::string &s, ImU32 bg, ImU32 fg)
    {
        float w = text_w(s, BOLD, 10);
        Rc r{x, y, x + w + px(16), y + pt(10) + px(6)};
        fillr(r, bg);
        text_in(r, s, fg, BOLD, 10, CENTER);
        return r.r + px(8);
    }
    void fill_list()
    {
        double fr = ui::frame_rate(*R);
        for (auto &s : R->gemstar) {
            std::string where = fmt("%d / %d", s.lineNo, s.field);
            for (auto &p : s.packets) {
                GemstarTypeA h = gemstarTypeA(p);
                GemstarClock c = gemstarClock(p);
                if (c.valid) packets.push_back({ui::rec_time(p.frame / fr), where, "G-Clock", "", "", p.text().substr(7), p.copies, C_TIME});
                else if (h.ok) {
                    std::string hex;
                    for (size_t k = 0; k < h.payload.size() && k < 40; k++) hex += fmt("%s%02X", k ? " " : "", h.payload[k]);
                    if (h.payload.size() > 40) hex += " \xE2\x80\xA6";
                    packets.push_back({ui::rec_time(p.frame / fr), where, h.name, fmt("%d / %d", h.block, h.total), fmt("%04X", h.host), fmt("v%02X  ", h.version) + hex, p.copies, FG});
                } else
                    packets.push_back({ui::rec_time(p.frame / fr), where, fmt("type %02X", p.type), "", "", p.text(), p.copies, FG});
            }
            bool typeB = false;
            for (auto &l : s.guide) {
                std::string t = trim_sp(l);
                if (t.rfind("payload scrambled", 0) == 0) { if (keyNote.empty()) keyNote = "Type A " + t; continue; }
                if (t.rfind("Type B", 0) == 0) typeB = true;
                if (!typeB || t.empty() || t == "Other messages:") continue;
                packets.push_back({"", where, "Type B", "", "", t, 1, ACCENT2});
            }
        }
    }
    void draw() override
    {
        header("Gemstar", shows.empty() ? std::string("VCR Plus+ / GUIDE Plus+ data") : fmt("programme guide \xC2\xB7 %zu programmes on %zu channels", shows.size(), all.size()));
        int b = shows.empty() ? header_buttons({"Text report"}, {120}) + 3
                              : header_buttons({"Home", fmt("%d hours", span / 60), busy ? "All channels" : "Busy channels", "Text report"}, {80, 100, 120, 120});
        if (b == 0) home();
        if (b == 1) set_span(span >= 480 ? 120 : span * 2);
        if (b == 2) { busy = !busy; fit(); }
        if (b == 3) text_window("Gemstar \xE2\x80\x94 " + ui::file_name(*R), "Type A packets, Type B guide data decrypted as Gemstar's own monitor program FREQCCD_ does it, then the raw words", ui::gemstar_text(*R));
        ImU32 gem = service_colour("Gemstar");
        Rc card = take(px(110));
        fillr(card, PANEL);
        fillr({card.l, card.t, card.l + px(5), card.b}, gem);
        float x = card.l + px(20);
        text(x, card.t + px(8), "GUIDE Plus+", gem, BOLD, 24);
        static const char *WD[] = {"?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        std::string c = clk.valid ? fmt("broadcast clock %s %d %s %d  %d:%02d  (zone %d)", WD[clk.weekday & 7], clk.day, MONTHS[(clk.month + 11) % 12], clk.year, clk.hour, clk.minute, clk.zone)
                                  : "no clock packet in this recording";
        text(x, card.b - px(44), c, ACCENT2, NORM, 9);
        text(x, card.b - px(24), !shows.empty() ? fmt("Gemstar VCR Plus+ / GUIDE Plus+; listings from the Type B Show Information messages; times UTC\xE2\x88\x92%d", zone)
                                                : !keyNote.empty() ? keyNote : std::string("Gemstar VCR Plus+ / GUIDE Plus+ packets; no Show Information messages to build a guide from"), DIM, NORM, 9);
        int64_t words = 0, pk = 0, blocks = 0, good = 0, ta = 0;
        for (auto &s : R->gemstar) {
            words += s.words2x + s.words1x, pk += s.packets.size(), blocks += s.blocks, good += s.blocksGood;
            for (auto &p : s.packets) ta += gemstarTypeA(p).ok;
        }
        std::vector<std::pair<std::string, std::string>> t = {{fmt("%lld", (long long)words), "words"}, {fmt("%lld", (long long)pk), "packets"}};
        if (ta) t.push_back({fmt("%lld", (long long)ta), "Type A"});
        if (blocks) t.push_back({fmt("%lld/%lld", (long long)good, (long long)blocks), "Type B blocks ok"});
        if (!shows.empty()) t.push_back({fmt("%zu", shows.size()), "programmes"}), t.push_back({fmt("%zu", all.size()), "channels"});
        tiles(card, t);
        ImGui::Dummy(ImVec2(0, px(4)));
        if (shows.empty()) {
            list("##gem", {{"Recording", 96}, {"Line / field", 84}, {"Packet", 190}, {"Block", 64}, {"HostID", 64}, {"Payload (scrambled) / content", -1}, {"Copies", 56}},
                 (int)packets.size(), -1, [this](int r, int c) {
                     auto &p = packets[r];
                     return c == 0 ? p.t : c == 1 ? p.where : c == 2 ? p.kind : c == 3 ? p.block : c == 4 ? p.host : c == 5 ? p.content : (p.copies > 1 ? fmt("%d", p.copies) : std::string());
                 }, [this](int r) { return packets[r].c; });
            return;
        }
        if (focused()) {
            if (key(ImGuiKey_UpArrow)) move(-1, 0);
            if (key(ImGuiKey_DownArrow)) move(1, 0);
            if (key(ImGuiKey_LeftArrow)) move(0, -1);
            if (key(ImGuiKey_RightArrow)) move(0, 1);
            if (key(ImGuiKey_PageUp)) move(-vis, 0);
            if (key(ImGuiKey_PageDown)) move(vis, 0);
            if (key(ImGuiKey_Home)) home();
            if (key(ImGuiKey_Tab)) { int d = ImGui::GetIO().KeyShift ? -1440 : 1440; view += d; sel_t += d; fit(); }
            if ((key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) && span > 120) set_span(span / 2);
            if ((key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) && span < 480) set_span(span * 2);
        }
        rowh = px(32); chw = px(100);
        Rc days = take(px(28));
        Rc ruler = take(px(26));
        float avail = ImGui::GetContentRegionAvail().y;
        grid = take(std::max(rowh, avail - px(210)));
        ImGui::Dummy(ImVec2(0, px(4)));
        Rc info = take(-1);
        vis = std::max(1, (int)(grid.h() / rowh));
        ImVec2 mp = ImGui::GetMousePos();
        bool click = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0);
        // days
        float dw = std::min(px(130), (days.w() - chw) / std::max<int>(1, (int)dayList.size()));
        text(days.l, days.t + px(6), dlabel(view).substr(dlabel(view).find(' ') + 1), FG, BOLD, 10);
        for (size_t i = 0; i < dayList.size(); i++) {
            Rc r{days.l + chw + i * dw + 1, days.t, days.l + chw + (i + 1) * dw - 2, days.b};
            int y, m, d, wd;
            civil_from_days(dayList[i], y, m, d, wd);
            bool cur = dayList[i] == view / 1440;
            fillr(r, cur ? ACCENT : FIELD);
            text_in(r, fmt("%s %d %s", DAYS[wd], d, MONTHS[m - 1]), cur ? BG : FG, cur ? BOLD : NORM, 10, CENTER);
            if (click && r.in(mp)) {
                int best = -1;
                for (auto &s : shows) {
                    if (s.t / 1440 != dayList[i]) continue;
                    int64_t v = s.t / 30 * 30;
                    int n = 0;
                    for (auto &o : shows) n += o.t < v + span && o.t + o.dur > v;
                    if (n > best) best = n, view = v;
                }
                sel_t = view;
                for (size_t k = 0; k < chans.size(); k++) if (at(chans[k], sel_t)) { sel = (int)k; break; }
                fit();
            }
        }
        float lx = days.r;
        for (auto &kv : std::vector<std::pair<std::string, ImU32>>{{"other", C_SHOW}, {"children", C_KIDS}, {"movie", C_MOVIE}}) {
            lx -= text_w(kv.first, NORM, 9);
            text(lx, days.t + px(7), kv.first, MUTED, NORM, 9);
            fillr({lx - px(16), days.t + px(11), lx - px(5), days.t + px(19)}, kv.second);
            lx -= px(30);
        }
        // ruler
        fillr(ruler, FIELD);
        int step = span <= 240 ? 30 : 60;
        for (int64_t tt = view; tt < view + span; tt += step) {
            float xx = x_of(tt);
            if (tt != view) fillr({xx, ruler.t + px(4), xx + 1, ruler.b - px(4)}, LINE);
            text(xx + px(6), ruler.t + px(5), tt % 1440 == 0 ? dlabel(tt).substr(0, 3) + " " + tlabel(tt) : tlabel(tt), tt % 1440 == 0 ? ACCENT2 : MUTED);
        }
        // grid
        fillr(grid, PANEL);
        if (ImGui::IsWindowHovered() && grid.in(mp)) {
            float wh = ImGui::GetIO().MouseWheel;
            if (wh != 0) {
                if (ImGui::GetIO().KeyShift) { view += wh > 0 ? -30 : 30; refresh(); }
                else move(wh > 0 ? -3 : 3, 0);
            }
        }
        if (click && grid.in(mp) && mp.x > grid.l + chw) {
            int r = (int)((mp.y - grid.t) / rowh);
            if (top + r < (int)chans.size()) {
                sel = top + r;
                int ch = chans[sel];
                sel_t = view + (int64_t)((mp.x - grid.l - chw) * span / (grid.w() - chw));
                if (!row_has_view(ch)) {
                    const GemShow *nx, *pv;
                    neighbours(ch, nx, pv);
                    bool right = mp.x > (grid.l + chw + grid.r) / 2;
                    const GemShow *go = right ? (nx ? nx : pv) : (pv ? pv : nx);
                    if (go) { sel_t = go->t; view = go->t / 30 * 30 - 30; }
                }
                fit();
            }
        }
        for (int r = 0; r < vis && top + r < (int)chans.size(); r++) {
            int idx = top + r, ch = chans[idx];
            bool selrow = idx == sel;
            Rc row{grid.l, grid.t + r * rowh, grid.r, grid.t + (r + 1) * rowh};
            Rc cc{row.l, row.t + 1, row.l + chw - px(6), row.b - 1};
            fillr(cc, selrow ? LINE : FIELD);
            fillr({cc.l, cc.t, cc.l + px(4), cc.b}, gem);
            Rc ct{cc.l + px(12), cc.t, cc.r - px(10), cc.b};
            text_in(ct, "CH", DIM, NORM, 9, LEFT);
            text_in(ct, fmt("%d", ch), FG, BOLD, 14, RIGHT);
            for (int i : rows[ch]) {
                auto &s = shows[i];
                if (s.t + s.dur <= view || s.t >= view + span) continue;
                float x0 = std::max(x_of(s.t), grid.l + chw), x1 = std::min(x_of(s.t + s.dur), grid.r);
                Rc b{x0 + 1, row.t + 1, x1 - 1, row.b - 1};
                bool is_sel = selrow && sel_t >= s.t && sel_t < s.t + s.dur;
                fillr(b, is_sel ? ACCENT : FIELD);
                fillr({b.l, b.t, b.l + px(3), b.b}, s.colour());
                text_in({b.l + px(10), b.t, b.r - px(4), b.b}, (s.t < view ? "\xE2\x97\x80 " : "") + s.title, is_sel ? BG : FG, is_sel ? BOLD : NORM, 10, LEFT);
            }
            if (!row_has_view(ch)) {
                Rc er{grid.l + chw + px(10), row.t, grid.r - px(10), row.b};
                const GemShow *nx, *pv;
                neighbours(ch, nx, pv);
                if (nx) text_in(er, day_prefix(nx->t, view) + tlabel(nx->t) + "   " + nx->title + "  \xE2\x96\xB6", DIM, NORM, 9, RIGHT);
                if (pv) text_in({er.l, er.t, (er.l + er.r) / 2, er.b}, "\xE2\x97\x80  " + pv->title + "   " + day_prefix(pv->t, view) + tlabel(pv->t), DIM, NORM, 9, LEFT);
            }
            fillr({grid.l + chw, row.b - 1, grid.r, row.b}, BG);
        }
        // the selected programme
        fillr(info, PANEL);
        const GemShow *s = selected();
        float ix = info.l + px(20), iy = info.t + px(12);
        if (!s) { if (!chans.empty()) text(ix, iy, fmt("nothing at this time on channel %d  \xE2\x80\x94  \xE2\x86\x90 \xE2\x86\x92 go to its programmes", chans[sel]), MUTED); return; }
        fillr({info.l, info.t, info.l + px(5), info.b}, s->colour());
        text(ix, iy, s->title, FG, BOLD, 15);
        iy += px(32);
        text(ix, iy, tlabel(s->t) + " \xE2\x80\x93 " + tlabel(s->t + s->dur) + "   \xC2\xB7   " + dlabel(s->t) + "   \xC2\xB7   " + fmt("channel %d   \xC2\xB7   %lld min", s->ch, (long long)s->dur), ACCENT2);
        iy += px(26);
        float pxx = ix;
        if (!s->category.empty()) pxx = pill(pxx, iy, s->category, s->colour(), rgb(255, 255, 255));
        if (!s->year.empty()) pxx = pill(pxx, iy, s->year, FIELD, FG);
        if (s->stars) { std::string st; for (int k = 0; k < 4; k++) st += k < s->stars ? "\xE2\x98\x85" : "\xE2\x98\x86"; pxx = pill(pxx, iy, st, FIELD, C_TIME); }
        if (!s->rating.empty()) pxx = pill(pxx, iy, s->rating, rgb(0xe8, 0xe8, 0xe8), rgb(0x10, 0x10, 0x10));
        if (!s->cast.empty()) text(pxx + px(4), iy + px(3), "with " + s->cast, MUTED);
        if (pxx != ix || !s->cast.empty()) iy += px(34);
        text_wrap({ix, iy, info.r - px(20), info.b - px(30)}, s->desc.empty() ? "No description was sent for this programme." : s->desc, s->desc.empty() ? DIM : FG, NORM, 10);
        std::string foot = (s->code >= 0 ? fmt("programme No. %lld   \xC2\xB7   ", (long long)s->code) : std::string()) +
                           fmt("received at %s, line %d field %d", ui::rec_time(s->frame / ui::frame_rate(*R)).c_str(), s->lineNo, s->field);
        text(ix, info.b - px(24), foot, DIM, NORM, 9);
    }
};

// ================================================================ Japanese teletext
struct JtesWin : Win {
    ResultPtr R;
    int sel = -1;
    bool sound = true, follow = false;
    Tex tex;
    std::vector<JtesFrame> frames;
    size_t frame = 0;
    double frameAt = 0;
    std::string text_, sub;
    int tunes = 0;
    void init()
    {
        std::set<int> progs;
        for (auto &p : R->jtes.pages) { progs.insert(p.programme); tunes += !jtesTune(p).empty(); }
        sub = fmt("\xE6\x96\x87\xE5\xAD\x97\xE5\xA4\x9A\xE9\x87\x8D\xE6\x94\xBE\xE9\x80\x81 (JTES, BT.653 system D) \xC2\xB7 %zu pages in %zu programmes \xC2\xB7 %d with a tune", R->jtes.pages.size(), progs.size(), tunes);
        if (!R->jtes.pages.empty()) select(0);
    }
    void play()
    {
        stop_sound();
        if (sel < 0) return;
        auto tune = jtesTune(R->jtes.pages[sel]);
        if (!tune.empty()) play_wav(jtesWave(tune));
    }
    void show(size_t k)
    {
        if (frames.empty()) return;
        frame = k % frames.size();
        tex.set(frames[frame].img.px.data(), frames[frame].img.w, frames[frame].img.h);
        double s = frames[frame].seconds;
        frameAt = SDL_GetTicks() / 1000.0 + (s <= 0 ? 3 : s);
    }
    void select(int i)
    {
        sel = i;
        const JtesPage &p = R->jtes.pages[i];
        JtesImage im = jtesRender(p);
        tex.set(im.px.data(), im.w, im.h);
        frames = jtesFrames(p);
        show(0);
        text_ = jtesText(p);
        if (sound) play(); else stop_sound();
    }
    void draw() override
    {
        header("Japanese teletext", sub);
        int b = header_buttons({sound ? "Sound: on" : "Sound: off", "Play", "Export\xE2\x80\xA6", "Text report"}, {110, 70, 100, 110});
        if (b == 0) { sound = !sound; if (sound) play(); else stop_sound(); }
        if (b == 1) { play(); show(0); }
        if (b == 2) {
            ResultPtr r = R;
            open_folder_dialog("Export the Japanese teletext pages", dirname(R->path), [r](std::string d) {
                int n = jtesExport(r->jtes, path_join(d, stem_path(ui::file_name(*r)) + "_jtes"));
                message("Export", fmt("%d pages written (PNG, text, WAV for the tunes).", n));
            });
        }
        if (b == 3) {
            std::string s;
            for (auto &x : R->jtes.report) s += x + "\n";
            s += "\n";
            for (auto &p : R->jtes.pages) s += "=== " + jtesPageName(p) + "  " + p.title + "\n" + jtesText(p) + "\n\n";
            text_window("Japanese teletext \xE2\x80\x94 " + ui::file_name(*R), "every page as text", s);
        }
        if (!frames.empty() && SDL_GetTicks() / 1000.0 >= frameAt) show(frame + 1);
        int n = (int)R->jtes.pages.size();
        if (focused() && n) {
            if (key(ImGuiKey_UpArrow) && sel > 0) { select(sel - 1); follow = true; }
            if (key(ImGuiKey_DownArrow) && sel + 1 < n) { select(sel + 1); follow = true; }
        }
        ImVec2 av = ImGui::GetContentRegionAvail();
        float lw = std::min(px(400), av.x * 2 / 5);
        ImGui::BeginChild("##jl", ImVec2(lw, av.y));
        ImGui::PushFont(F[0]);
        int h = list("##jtes", {{"Page", 64}, {"\xE2\x99\xAA", 28}, {"Title", -1}}, n, sel, [this](int r, int c) {
            auto &p = R->jtes.pages[r];
            return c == 0 ? jtesPageName(p) + (p.channel != 0x92 ? fmt(" %02X", p.channel) : std::string()) : c == 1 ? (jtesTune(p).empty() ? std::string() : std::string("\xE2\x99\xAA")) : p.title;
        }, [this](int r) { return R->jtes.pages[r].gaps ? MUTED : FG; }, follow);
        ImGui::PopFont();
        follow = false;
        if (h >= 0 && h != sel) select(h);
        ImGui::EndChild();
        ImGui::SameLine(0, px(10));
        ImGui::BeginChild("##jp", ImVec2(0, av.y));
        ImVec2 a2 = ImGui::GetContentRegionAvail();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(0x11, 0x11, 0x11));
        ImGui::BeginChild("##pic", ImVec2(a2.x, a2.y - px(180)));
        image_fit(tex, ImGui::GetContentRegionAvail(), 1.1);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        text_box("##jt", text_, ImVec2(-1, -1));
        ImGui::EndChild();
    }
    ~JtesWin() { stop_sound(); }
};

// ================================================================ text with a summary (5.73 Mbit/s data)
struct DataWin : Win {
    std::string head, sub, summary, body;
    void draw() override
    {
        header(head, sub);
        if (header_buttons({"Copy all"}, {110}) == 0) ImGui::SetClipboardText(body.c_str());
        if (!summary.empty()) wrapped(ACCENT2, summary);
        text_box(("##d" + title).c_str(), body);
    }
};
}  // namespace

template <class T> static void open_win(std::unique_ptr<T> w, ImVec2 size)
{
    w->size = ImVec2(px(size.x), px(size.y));
    add_window(std::move(w));
}
void starsight_window(const ResultPtr &r) { auto w = std::make_unique<GuideWin>(); w->R = r; w->title = wtitle("StarSight", r); w->build(); open_win(std::move(w), {1180, 860}); }
void gemstar_window(const ResultPtr &r) { auto w = std::make_unique<GemWin>(); w->R = r; w->title = wtitle("Gemstar", r); w->build(); open_win(std::move(w), {1180, 860}); }
void jtes_window(const ResultPtr &r) { auto w = std::make_unique<JtesWin>(); w->R = r; w->title = wtitle("Japanese teletext", r); w->init(); open_win(std::move(w), {1240, 820}); }
void captions_window(const ResultPtr &r) { auto w = std::make_unique<CapWin>(); w->R = r; w->title = wtitle("CC captions", r); w->load(); open_win(std::move(w), {940, 760}); }
void xds_window(const ResultPtr &r) { auto w = std::make_unique<XdsWin>(); w->R = r; w->title = wtitle("XDS", r); w->load(); open_win(std::move(w), {940, 760}); }
void sign_window(const ResultPtr &r) { auto w = std::make_unique<SignWin>(); w->R = r; w->title = wtitle("Silent Radio", r); w->init(); open_win(std::move(w), {SR_W * 8 + 60.f, 660}); }
void amol_window(const ResultPtr &r) { auto w = std::make_unique<AmolWin>(); w->R = r; w->title = wtitle("AMOL", r); w->init(); open_win(std::move(w), {1000, 780}); }
void data_window(const ResultPtr &r, const std::string &kind)
{
    auto w = std::make_unique<DataWin>();
    char want = kind == "NABTS" ? 'N' : kind == "WST teletext" ? 'W' : 'X';
    size_t n = 0;
    for (auto &p : r->fastPackets) n += p.kind == want;
    std::string where;
    std::set<std::string> seen;
    for (auto &li : r->lines)
        if (li.mode == MODE_FAST && seen.insert(fmt("%d", li.lineNo)).second) where += (where.empty() ? "" : ", ") + fmt("line %d", li.lineNo);
    w->head = kind == "framing 2D" ? "Encrypted datacast (framing 2D)" : kind;
    w->sub = "5.73 Mbit/s \xC2\xB7 " + where;
    w->summary = fmt("%zu packets of this kind, %zu packets at 5.73 Mbit/s in all, %zu text messages recovered", n, r->fastPackets.size(), r->fastText.size());
    for (auto &l : r->fastReport) w->body += l + "\n";
    w->body += "\n=== recovered text ===\n";
    for (auto &l : r->fastText) w->body += l + "\n";
    w->title = wtitle(w->head, r);
    open_win(std::move(w), {1000, 720});
}
}
