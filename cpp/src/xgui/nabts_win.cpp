// NABTS window (as src/gui_nabts.cpp): the record catalogue and the NAPLPS pages drawn as on the
// receiver screen, in time.
#include "draw.h"
#include "nabts.h"
#include "png.h"

namespace xg {
namespace {
const std::vector<std::pair<std::string, std::pair<int, int>>> GRIDS = {{"256 \xC3\x97 200 (as the receiver)", {256, 200}}, {"512 \xC3\x97 400", {512, 400}}, {"768 \xC3\x97 600", {768, 600}}};

struct NabtsWin : Win {
    ResultPtr R;
    std::vector<nabts::Record> recs;
    std::vector<std::array<std::string, 5>> rows;
    int grid = 0, sel = -1;
    bool play = true, follow = false;
    std::unique_ptr<nabts::Player> player;
    double t0 = 0;
    std::vector<nabts::Col> key;
    Tex tex;
    std::string msg, info, txt;
    double now() { return SDL_GetTicks() / 1000.0; }
    std::pair<int, int> G() { return GRIDS[grid].second; }
    void load()
    {
        recs = R->nabtsRecords;
        auto &summ = R->nabtsSummary;
        int pages = 0;
        for (auto &r : recs) pages += r.page != nullptr;
        msg = fmt("packets %ld, groups assembled %ld, records %zu, pages %d", summ.packets, summ.groups.count("complete") ? summ.groups.at("complete") : 0L, recs.size(), pages);
        if (!summ.foreign.empty()) {
            msg += ";  not teletext: ";
            bool f = true;
            for (auto &kv : summ.foreign) { msg += fmt("%schannel %03X type %d (%ld)", f ? "" : ", ", kv.first.first, kv.first.second, kv.second); f = false; }
        }
        static const char *tn[4] = {"page", "one-off", "application", "priority"};
        int first = -1;
        for (size_t i = 0; i < recs.size(); i++) {
            auto &r = recs[i];
            std::string title;
            for (auto &l : split(r.text, '\n')) if (!strip(l).empty()) { title = strip(l); break; }
            if (r.chain_pos) title = fmt("[continuation %d of %s] ", r.chain_pos, nabts::address_text(r.chain_base).c_str()) + title;
            if (!r.purpose.empty()) title = "(" + r.purpose + ") " + title;
            rows.push_back({nabts::record_label(r), r.type < 4 ? tn[r.type] : std::to_string(r.type), std::to_string(r.seen), nabts::flags_text(r), title});
            if (first < 0 && r.page) first = (int)i;
        }
        if (!recs.empty()) select(std::max(0, first));
    }
    void select(int i)
    {
        sel = i;
        follow = true;
        show();
    }
    void reinterpret()
    {
        nabts::interpret(recs, G().first, G().second);
        msg = "receiver " + GRIDS[grid].first;
        show();
    }
    void show()
    {
        player.reset();
        tex.clear();
        if (sel < 0) return;
        auto &r = recs[sel];
        if (r.page) {
            player.reset(new nabts::Player(*r.page, G().first, G().second));
            if (play) { t0 = now(); player->advance(0.0); }
            else { player->advance(0, true); t0 = now() - player->end(); }
            key.clear();
            frame(true);
        }
        if (r.page) txt = r.text;
        else if (r.type == 2) {
            txt = "Application record (CEA-516 \xC2\xA7" "7.2.2 functions), not a page:\n\n";
            for (u8 b : r.data) txt += (char)((b & 0x7F) == '\r' ? '\n' : (b & 0x7F) >= 0x20 || (b & 0x7F) == '\n' ? (b & 0x7F) : ' ');
        } else
            txt = "The record has no data to display.";
        info = nabts::record_label(r) + fmt(" \xC2\xB7 received %d times, intact %d", r.seen, r.intact_n) + (r.copies_voted ? fmt(", voted from %d copies", r.copies_voted) : "") +
               fmt(" \xC2\xB7 %zu bytes", r.data.size()) + (r.page ? fmt(" \xC2\xB7 on-screen display %.1f s", r.page->end) : "");
        if (r.chain_pos) info += fmt(" \xC2\xB7 continuation #%d of chain ", r.chain_pos) + nabts::address_text(r.chain_base) + " (drawn over the previous ones)";
    }
    void frame(bool force = false)
    {
        if (!player) return;
        double T = now() - t0;
        bool nw = player->advance(T);
        auto k = player->blink_key(T);
        if (nw || k != key || force) {
            key = k;
            Image im = player->image(T);
            tex.set(im.px.data(), im.w, im.h);
        }
    }
    Image page_png(const nabts::Record &r) { return nabts::render_page(*r.page, G().first, G().second).scaled(1536, 1152); }
    void draw() override
    {
        frame();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Receiver:");
        ImGui::SameLine();
        if (ImGui::Button((GRIDS[grid].first + " \xE2\x96\xBE").c_str(), ImVec2(px(220), 0))) { grid = (grid + 1) % GRIDS.size(); reinterpret(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Draw as on screen", &play)) show();
        ImGui::SameLine();
        if (ImGui::Button("Replay")) show();
        ImGui::SameLine();
        if (ImGui::Button("Save PNG\xE2\x80\xA6")) {
            if (sel < 0 || !recs[sel].page) message("PNG", "This record has no page.");
            else {
                Image im = page_png(recs[sel]);
                std::string name = nabts::record_name(recs[sel]) + ".png";
                open_folder_dialog("Folder for the PNG", dirname(R->path), [this, im, name](std::string d) { png_save(path_join(d, name), im); msg = "saved: " + path_join(d, name); });
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save all pages (PNG + text)\xE2\x80\xA6"))
            open_folder_dialog("Folder for pages", dirname(R->path), [this](std::string d) {
                std::string out = path_join(d, basename(stem_path(R->path)) + "_nabts");
                make_dirs(out);
                std::string t;
                int n = 0;
                for (auto &r : recs) {
                    t += "=== " + nabts::record_label(r) + fmt("  type %d  received %d  ", r.type, r.seen) + nabts::flags_text(r) + " " + r.purpose + "\n";
                    if (r.page) { png_save(path_join(out, nabts::record_name(r) + ".png"), page_png(r)); t += r.text + "\n"; n++; }
                    t += "\n";
                }
                write_text(path_join(out, "records.txt"), t);
                msg = fmt("saved %d pages to ", n) + out;
                open_path(out);
            });
        ImGui::SameLine();
        if (ImGui::Button("Save all pages as HTML\xE2\x80\xA6")) {
            bool any = false;
            for (auto &r : recs) any |= r.page != nullptr;
            if (!any) message("HTML", "There are no pages to save.");
            else
                open_folder_dialog("Folder for the HTML pages", dirname(R->path), [this](std::string d) {
                    Progress pr;
                    int n = nabts_html_export(recs, d, G().first, G().second, pr);
                    msg = fmt("saved %d pages to ", n) + d;
                    open_path(d);
                });
        }
        wrapped(MUTED, msg);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !recs.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && sel > 0) select(sel - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && sel + 1 < (int)recs.size()) select(sel + 1);
        }
        ImVec2 av = ImGui::GetContentRegionAvail();
        float lw = av.x / 3;
        ImGui::BeginChild("##nl", ImVec2(lw, av.y));
        int hit = -1;
        if (ImGui::BeginTable("##recs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, px(150));
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, px(70));
            ImGui::TableSetupColumn("Received", ImGuiTableColumnFlags_WidthFixed, px(60));
            ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, px(120));
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin((int)rows.size());
            if (follow && sel >= 0) clip.IncludeItemByIndex(sel);
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                    ImGui::TableNextRow();
                    for (int c = 0; c < 5; c++) {
                        ImGui::TableSetColumnIndex(c);
                        if (c == 0) {
                            ImGui::PushID(r);
                            if (ImGui::Selectable(rows[r][0].c_str(), r == sel, ImGuiSelectableFlags_SpanAllColumns)) hit = r;
                            ImGui::PopID();
                            if (follow && r == sel) ImGui::SetScrollHereY(0.5f);
                        } else
                            ImGui::TextUnformatted(rows[r][c].c_str());
                    }
                }
            follow = false;
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (hit >= 0 && hit != sel) select(hit);
        ImGui::SameLine();
        ImGui::BeginChild("##nr", ImVec2(0, av.y));
        ImVec2 a2 = ImGui::GetContentRegionAvail();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(0x20, 0x20, 0x20));
        ImGui::BeginChild("##canvas", ImVec2(a2.x, a2.y - px(230)));
        // 4:3 screen
        if (tex.t) image_fit(tex, ImGui::GetContentRegionAvail(), (4.0 / 3.0) * tex.h / tex.w);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        wrapped(MUTED, info);
        text_box("##ntext", txt, ImVec2(-1, -1));
        ImGui::EndChild();
    }
};
}  // namespace

void nabts_window(const ResultPtr &r)
{
    auto w = std::make_unique<NabtsWin>();
    w->R = r;
    w->title = "NABTS \xE2\x80\x94 " + ui::file_name(*r) + "##nabts" + std::to_string((intptr_t)r.get());
    w->size = ImVec2(px(1300), px(860));
    w->load();
    add_window(std::move(w));
}
}
