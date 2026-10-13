// "What is in" window: the map of the VBI lines (field 1 and field 2) and a button per service found.
#include "xgui.h"
#include "ssjson.h"
#include <cmath>

namespace xg {

struct RecWin : Win {
    ResultPtr R;
    std::vector<Service> svc;
    struct Row { int tv; std::string kind, text; double read; };
    std::vector<Row> rows;
    std::string sub, head;
    void build()
    {
        const VbiGeometry &g = R->geom;
        int per = std::max(1, g.linesPerFrame / 2), f2 = g.pal ? 313 : 263;
        for (int f = 1; f <= 2; f++)
            for (int i = 0; i < per; i++) {
                int line = g.firstLine + i;
                Row r{line + (f == 2 ? f2 : 0), "empty", "", 0};
                for (auto &li : R->lines)
                    if (li.lineNo == line && li.field == f) {
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
        svc = ui::services_of(*R);
        double secs = g.frames / ui::frame_rate(*R);
        sub = fmt("%s \xC2\xB7 %lld frames \xC2\xB7 %.0f min %02.0f s \xC2\xB7 %s \xC2\xB7 %.0f s", formatName(g.format), (long long)g.frames, floor(secs / 60), fmod(secs, 60),
                  R->slicerName.c_str(), R->totalSeconds);
        for (auto &x : headline(*R)) head += (head.empty() ? "" : "   \xC2\xB7   ") + x;
    }
    void map(ImVec2 size)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        std::vector<const Row *> col[2];
        int f2 = R->geom.pal ? 313 : 263;
        for (auto &r : rows) col[r.tv >= f2 ? 1 : 0].push_back(&r);
        int ncol = col[1].empty() ? 1 : 2;
        size_t nmax = std::max(col[0].size(), col[1].size());
        float h = std::min(26.f, std::max(14.f, size.y / std::max<size_t>(1, nmax)));
        float gap = 12, cw = (size.x - gap * (ncol - 1)) / ncol;
        for (int c = 0; c < ncol; c++) {
            float left = p0.x + c * (cw + gap), x0 = left + 96, right = left + cw, y = p0.y;
            for (auto *r : col[c]) {
                if (y + h > p0.y + size.y + 1) break;
                std::string lab = r->tv >= f2 ? fmt("%d (f2: %d)", r->tv, r->tv - f2) : fmt("line %d", r->tv);
                float ty = y + (h - ImGui::GetTextLineHeight()) / 2;
                dl->AddText(ImVec2(left, ty), MUTED, lab.c_str());
                ImU32 cc = service_colour(r->kind);
                dl->AddRectFilled(ImVec2(x0 + 1, y + 1), ImVec2(right - 2, y + h - 1), cc);
                if (r->kind != "empty") {
                    std::string t = r->text;
                    if (r->read > 0) t += fmt("  \xC2\xB7 signal %.0f%%", r->read * 100);
                    ImVec4 v = v4(cc);
                    bool dark = v.x * 3 + v.y * 6 + v.z > 4.3f;
                    dl->PushClipRect(ImVec2(x0, y), ImVec2(right - 2, y + h), true);
                    dl->AddText(ImVec2(x0 + 6, ty), dark ? rgb(0x11, 0x11, 0x11) : FG, t.c_str());
                    dl->PopClipRect();
                }
                y += h;
            }
        }
        ImGui::Dummy(size);
    }
    void draw() override
    {
        header(ui::file_name(*R), "");
        wrapped(MUTED, sub);
        ImGui::PushFont(F[1]);
        wrapped(ACCENT2, head);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4));
        float rw = 300;
        ImVec2 av = ImGui::GetContentRegionAvail();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, PANEL);
        ImGui::BeginChild("##map", ImVec2(av.x - rw - 12, av.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(F[1]); ImGui::TextUnformatted("VBI lines"); ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 170);
        if (ImGui::Button("View VBI lines", ImVec2(170, 0))) lines_window(R);
        map(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        ImGui::SameLine(0, 12);
        ImGui::BeginChild("##res", ImVec2(rw, av.y), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(F[1]); ImGui::TextUnformatted("Results"); ImGui::PopFont();
        ImGui::BeginChild("##list", ImVec2(-1, -180));
        for (size_t i = 0; i < svc.size(); i++) {
            ImGui::PushID((int)i);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
            if (ImGui::Button(("\xE2\x96\xB6  " + svc[i].name).c_str(), ImVec2(-1, 36))) open_service(R, svc[i]);
            ImGui::PopStyleVar();
            ImGui::PushTextWrapPos(0);
            wrapped(MUTED, svc[i].where + "  \xE2\x80\x94 " + svc[i].note);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PopID();
        }
        if (svc.empty()) wrapped(MUTED, "Nothing the program can decode\nwas found in this recording.");
        ImGui::EndChild();
        if (accent_button("Export everything decoded\xE2\x80\xA6", ImVec2(-1, 32))) export_all(R);
        if (ImGui::Button("Report (text)", ImVec2(-1, 32))) open_service(R, Service{"Report", "", "", "", ui::ServiceWindow::Report, ""});
        ImGui::BeginDisabled(R->db.empty());
        if (ImGui::Button("Export StarSight JSON\xE2\x80\xA6", ImVec2(-1, 32))) {
            ResultPtr r = R;
            open_folder_dialog("Folder for the JSON", dirname(R->path), [r](std::string d) {
                std::string err, p = path_join(d, stem_path(ui::file_name(*r)) + ".json");
                if (!ssExportJson(p, r->db, r.get(), err)) message("Export", err);
                else message("Export", "Written: " + p);
            });
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Close", ImVec2(-1, 32))) open = false;
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
};

void recording_window(const ResultPtr &r)
{
    auto w = std::make_unique<RecWin>();
    w->R = r;
    w->title = "What is in " + ui::file_name(*r) + "##rec" + std::to_string((intptr_t)r.get());
    w->size = ImVec2(1040, 680);
    w->build();
    add_window(std::move(w));
}
}
