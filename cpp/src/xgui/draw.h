// Drawing for the hand-drawn windows (programme grids, clocks, the LED sign): rectangles and text in
// the window's draw list, measured like the Windows program's GDI drawing.
#pragma once
#include "xgui.h"

namespace xg {
extern float g_scale;
inline float px(float v) { return v * g_scale; }

struct Rc {
    float l = 0, t = 0, r = 0, b = 0;
    float w() const { return r - l; }
    float h() const { return b - t; }
    bool in(ImVec2 p) const { return p.x >= l && p.x < r && p.y >= t && p.y < b; }
    Rc inset(float d) const { return {l + d, t + d, r - d, b - d}; }
};
// a rectangle of the given height taken from the top of the free space of the window
Rc take(float h);
inline ImDrawList *DL() { return ImGui::GetWindowDrawList(); }
inline void fillr(Rc r, ImU32 c) { DL()->AddRectFilled(ImVec2(r.l, r.t), ImVec2(r.r, r.b), c); }

// fonts by look: normal, bold, monospace; sizes in points of the Windows program (10 = normal text)
enum Face { NORM, BOLD, MONO };
ImFont *face(Face f);
inline float pt(float points) { return px(points * 1.45f); }
float text_w(const std::string &s, Face f, float points);
enum { LEFT = 0, CENTER = 1, RIGHT = 2 };
// text at x, y (top); align LEFT / CENTER / RIGHT of x
void text(float x, float y, const std::string &s, ImU32 c, Face f = NORM, float points = 10, int align = LEFT);
// text inside a rectangle: horizontal align, centred vertically, cut with "..." when too long
void text_in(Rc r, const std::string &s, ImU32 c, Face f, float points, int align, bool vcenter = true);
// wrapped text inside a rectangle, from the top
void text_wrap(Rc r, const std::string &s, ImU32 c, Face f, float points);
// tiles in the top-right of a card: big value, label under it
void tiles(Rc card, const std::vector<std::pair<std::string, std::string>> &t);
// buttons in the header row, right-aligned; returns the index clicked or -1
int header_buttons(const std::vector<std::string> &labels, const std::vector<float> &widths);

extern const char *DAYS[7];
extern const char *MONTHS[12];
}
