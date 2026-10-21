// The windowed program on Linux and macOS (and Windows, for testing): SDL3 for the window, sound and
// file dialogs, Dear ImGui for the windows. The same decoders and the same windows as the Windows
// program (src/gui_*.cpp); colours and layout follow it.
#pragma once
#include "services.h"
#include "ui/json.h"
#include "ui/util.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xg {
using ui::ResultPtr;
using ui::Service;

// the colours of the Windows program
inline ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
const ImU32 BG = rgb(0x1b, 0x1c, 0x1f), PANEL = rgb(0x24, 0x26, 0x2b), FIELD = rgb(0x2d, 0x30, 0x36), LINE = rgb(0x3a, 0x3d, 0x44);
const ImU32 FG = rgb(0xe3, 0xe3, 0xe3), MUTED = rgb(0xa0, 0xa3, 0xa8), DIM = rgb(0x7a, 0x7d, 0x83);
const ImU32 ACCENT = rgb(0xff, 0xa3, 0x1a), ACCENT2 = rgb(0xff, 0xbe, 0x5c), SEL = rgb(0x5a, 0x3d, 0x10);
ImU32 service_colour(const std::string &kind);

// fonts: [0] normal, [1] bold, [2] big title, [3] monospace
extern ImFont *F[4];

// a picture on the screen (RGB pixels 0xRRGGBB), uploaded again when changed
struct Tex {
    unsigned t = 0;           // OpenGL texture (0: none)
    int w = 0, h = 0;
    void set(const uint32_t *px, int w, int h);
    void clear();
    ~Tex() { clear(); }
    ImTextureID id() const { return (ImTextureID)(intptr_t)t; }
};
// draw a picture into the free space of the window, keeping its aspect (ar = width / height of a dot)
void image_fit(Tex &t, ImVec2 avail, double dotAspect = 1.0, ImVec2 *at = nullptr, ImVec2 *size = nullptr);

// a window of the program: drawn every frame until open is false
struct Win {
    std::string title;        // unique ImGui name (also the window title)
    bool open = true;
    ImVec2 size{900, 640};
    virtual ~Win() = default;
    virtual void draw() = 0;          // contents, inside Begin / End
    virtual void flags(int &) {}
};
void add_window(std::unique_ptr<Win> w);

// background work with a progress window (decoding, export); done() runs on the main thread
struct Task {
    std::string title;
    std::atomic<bool> cancel{false}, finished{false};
    bool ok = false;
    std::string error;
    std::mutex mu;
    std::string step;
    std::vector<std::string> log;
    int64_t done = 0, all = 0;
    std::function<void(bool)> then;
};
void run_task(const std::string &title, std::function<void(Task &)> work, std::function<void(bool)> then);

// dialogs
void message(const std::string &title, const std::string &text);
// a choice between options; on_pick(index) or nothing when cancelled
void choose(const std::string &title, const std::string &text, const std::vector<std::string> &opts, std::function<void(int)> on_pick);
// a line of text from the user; then(text) unless cancelled
void ask_text(const std::string &title, const std::string &prompt, const std::string &value, std::function<void(std::string)> then);
void open_file_dialog(const std::string &what, const char *patterns, std::function<void(std::string)> then);
void open_folder_dialog(const std::string &what, const std::string &start, std::function<void(std::string)> then);
void open_path(const std::string &path);   // file manager / default program

// the program's settings (recent recordings, device), in the user's settings folder
Json &state();
void save_state();

// sound: a .wav file in memory, played until the next one or stop
void play_wav(const std::vector<uint8_t> &wav);
void stop_sound();

// header of a service window: bold title, muted subtitle
void header(const std::string &title, const std::string &sub);
bool accent_button(const char *label, ImVec2 size = ImVec2(0, 0));
// text with word wrap, in a colour
void wrapped(ImU32 c, const std::string &s);
// a read-only text box filling the space (selectable, copyable)
void text_box(const char *id, const std::string &text, ImVec2 size = ImVec2(-1, -1));

// the windows
void open_results(const ResultPtr &r);
void open_service(const ResultPtr &r, const Service &s);
void recording_window(const ResultPtr &r);
void text_window(const std::string &title, const std::string &sub, const std::string &body);
void jtes_window(const ResultPtr &r);
void teletext_window(const ResultPtr &r);
void starsight_window(const ResultPtr &r);
void captions_window(const ResultPtr &r);
void xds_window(const ResultPtr &r);
void sign_window(const ResultPtr &r);
void amol_window(const ResultPtr &r);
void gemstar_window(const ResultPtr &r);
void nabts_window(const ResultPtr &r);
void data_window(const ResultPtr &r, const std::string &kind);
void lines_window(const ResultPtr &r);
void export_all(const ResultPtr &r);

extern SDL_Window *g_window;
extern SDL_GLContext g_gl;
}
