#include "draw.h"

namespace xg {
float g_scale = 1;
const char *DAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

Rc take(float h)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (h < 0) h = ImGui::GetContentRegionAvail().y;
    ImGui::Dummy(ImVec2(w, std::max(1.f, h)));
    return {p.x, p.y, p.x + w, p.y + h};
}

ImFont *face(Face f) { return f == BOLD ? F[1] : f == MONO ? F[3] : F[0]; }

float text_w(const std::string &s, Face f, float points)
{
    return face(f)->CalcTextSizeA(pt(points), FLT_MAX, 0, s.c_str()).x;
}

void text(float x, float y, const std::string &s, ImU32 c, Face f, float points, int align)
{
    if (s.empty()) return;
    float w = align == LEFT ? 0 : text_w(s, f, points);
    if (align == CENTER) x -= w / 2;
    else if (align == RIGHT) x -= w;
    DL()->AddText(face(f), pt(points), ImVec2(x, y), c, s.c_str());
}

void text_in(Rc r, const std::string &s0, ImU32 c, Face f, float points, int align, bool vcenter)
{
    if (s0.empty() || r.w() <= 2) return;
    std::string s = s0;
    float w = text_w(s, f, points);
    if (w > r.w()) {   // cut at a UTF-8 boundary and add an ellipsis
        std::u32string u = to_u32(s);
        while (!u.empty() && text_w(from_u32(u) + "\xE2\x80\xA6", f, points) > r.w()) u.pop_back();
        s = from_u32(u) + "\xE2\x80\xA6";
        w = text_w(s, f, points);
    }
    float x = align == CENTER ? r.l + (r.w() - w) / 2 : align == RIGHT ? r.r - w : r.l;
    float h = pt(points);
    float y = vcenter ? r.t + (r.h() - h) / 2 : r.t;
    DL()->PushClipRect(ImVec2(r.l, r.t), ImVec2(r.r, r.b), true);
    DL()->AddText(face(f), h, ImVec2(x, y), c, s.c_str());
    DL()->PopClipRect();
}

void text_wrap(Rc r, const std::string &s, ImU32 c, Face f, float points)
{
    DL()->PushClipRect(ImVec2(r.l, r.t), ImVec2(r.r, r.b), true);
    DL()->AddText(face(f), pt(points), ImVec2(r.l, r.t), c, s.c_str(), nullptr, r.w());
    DL()->PopClipRect();
}

void tiles(Rc card, const std::vector<std::pair<std::string, std::string>> &t)
{
    float tx = card.r - px(16);
    for (int i = (int)t.size() - 1; i >= 0; i--) {
        float w = px(110);
        Rc r{tx - w, card.t + px(14), tx, card.t + px(74)};
        fillr(r, FIELD);
        text((r.l + r.r) / 2, r.t + px(8), t[i].first, FG, BOLD, 10, CENTER);
        text((r.l + r.r) / 2, r.t + px(34), t[i].second, MUTED, NORM, 9, CENTER);
        tx -= w + px(8);
    }
}

int header_buttons(const std::vector<std::string> &labels, const std::vector<float> &widths)
{
    float total = 0;
    for (float w : widths) total += px(w) + px(8);
    float x = ImGui::GetWindowContentRegionMax().x - total + px(8);
    ImGui::SameLine(std::max(x, ImGui::GetCursorPosX() + px(20)));
    int hit = -1;
    for (size_t i = 0; i < labels.size(); i++) {
        if (i) ImGui::SameLine(0, px(8));
        if (ImGui::Button(labels[i].c_str(), ImVec2(px(widths[i]), 0))) hit = (int)i;
    }
    return hit;
}
}
