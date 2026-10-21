// Teletext Rescue with windows on Linux and macOS: SDL3 + Dear ImGui. The main window holds the start
// screen (open a recording, recent recordings, what the program reads); a recording is decoded in the
// background and its results open in the "What is in" window and one window per service.
#include "xgui.h"
#include "draw.h"
#include "version.h"
#include "export.h"
#include "finds.h"
#include "flac.h"
#include "legacy_bridge.h"
#include "rescache.h"
#include "slicer.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"
#include "png.h"
#include <cmath>
#include <ctime>
#include <deque>
#include <thread>

namespace xg {

SDL_Window *g_window = nullptr;
SDL_GLContext g_gl = nullptr;
ImFont *F[4] = {};

ImU32 service_colour(const std::string &k0)
{
    std::string k = lower(k0);
    if (k.rfind("starsight", 0) == 0) return rgb(0xf2, 0xd0, 0x4b);
    if (k.rfind("encrypted datacast", 0) == 0) return rgb(0xff, 0x5c, 0x8a);
    if (k.find("silent radio") != std::string::npos) return rgb(0xff, 0xa3, 0x1a);
    if (k.rfind("nabts", 0) == 0) return rgb(0x7c, 0x6c, 0xff);
    if (k.find("wst") != std::string::npos || k.find("teletext") != std::string::npos) return rgb(0x3f, 0xa7, 0xff);
    if (k.rfind("cc", 0) == 0) return rgb(0x3c, 0xcf, 0x7a);
    if (k.rfind("gemstar", 0) == 0) return rgb(0xe8, 0x3c, 0xc8);
    if (k.rfind("tv-inform", 0) == 0) return rgb(0xd0, 0x3c, 0x3c);
    if (k.rfind("amol", 0) == 0 || k.rfind("vitc", 0) == 0) return rgb(0x4a, 0xd0, 0xc8);
    if (k.rfind("vps", 0) == 0 || k.rfind("wss", 0) == 0) return rgb(0xc8, 0xc8, 0x4a);
    if (k.rfind("test", 0) == 0) return rgb(0x55, 0x5a, 0x63);
    if (k.rfind("data", 0) == 0) return rgb(0xb0, 0x7c, 0xff);
    if (k.rfind("empty", 0) == 0) return rgb(0x2a, 0x2c, 0x31);
    return rgb(0x3a, 0x3d, 0x44);
}

// ---------------------------------------------------------------- pictures
// the few OpenGL 1.1 calls for pictures, taken from the driver through SDL (no OpenGL library linked on Linux)
#ifdef _WIN32
#define GLAPI_ __stdcall
#else
#define GLAPI_
#endif
namespace gl {
typedef void(GLAPI_ *GenT)(int, unsigned *);
typedef void(GLAPI_ *DelT)(int, const unsigned *);
typedef void(GLAPI_ *BindT)(unsigned, unsigned);
typedef void(GLAPI_ *ParT)(unsigned, unsigned, int);
typedef void(GLAPI_ *StoreT)(unsigned, int);
typedef void(GLAPI_ *ImgT)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void(GLAPI_ *SubT)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void(GLAPI_ *ReadT)(int, int, int, int, unsigned, unsigned, void *);
typedef void(GLAPI_ *ViewT)(int, int, int, int);
typedef void(GLAPI_ *ColT)(float, float, float, float);
typedef void(GLAPI_ *ClrT)(unsigned);
GenT GenTextures; DelT DeleteTextures; BindT BindTexture; ParT TexParameteri; StoreT PixelStorei; ImgT TexImage2D;
SubT TexSubImage2D; ReadT ReadPixels; ViewT Viewport; ColT ClearColor; ClrT Clear;
const unsigned TEXTURE_2D = 0x0DE1, MIN_FILTER = 0x2801, MAG_FILTER = 0x2800, NEAREST = 0x2600, WRAP_S = 0x2802, WRAP_T = 0x2803,
               CLAMP_TO_EDGE = 0x812F, RGBA = 0x1908, BGRA = 0x80E1, UNSIGNED_BYTE = 0x1401, UNPACK_ROW_LENGTH = 0x0CF2,
               PACK_ALIGNMENT = 0x0D05, COLOR_BUFFER_BIT = 0x4000;
template <class T> static void get(T &f, const char *name) { f = (T)SDL_GL_GetProcAddress(name); }
static void load()
{
    get(GenTextures, "glGenTextures"); get(DeleteTextures, "glDeleteTextures"); get(BindTexture, "glBindTexture");
    get(TexParameteri, "glTexParameteri"); get(PixelStorei, "glPixelStorei"); get(TexImage2D, "glTexImage2D");
    get(TexSubImage2D, "glTexSubImage2D"); get(ReadPixels, "glReadPixels"); get(Viewport, "glViewport");
    get(ClearColor, "glClearColor"); get(Clear, "glClear");
}
}

void Tex::set(const uint32_t *px, int W, int H)
{
    // 0xRRGGBB words are B, G, R, 0 in memory: uploaded as BGRA with the alpha byte set to FF
    std::vector<uint32_t> tmp((size_t)W * H);
    for (size_t i = 0; i < tmp.size(); i++) tmp[i] = px[i] | 0xFF000000u;
    if (!t || w != W || h != H) {
        clear();
        gl::GenTextures(1, &t);
        if (!t) return;
        gl::BindTexture(gl::TEXTURE_2D, t);
        gl::TexParameteri(gl::TEXTURE_2D, gl::MIN_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::MAG_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D, gl::WRAP_T, gl::CLAMP_TO_EDGE);
        gl::PixelStorei(gl::UNPACK_ROW_LENGTH, 0);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA, W, H, 0, gl::BGRA, gl::UNSIGNED_BYTE, tmp.data());
        w = W; h = H;
        return;
    }
    gl::BindTexture(gl::TEXTURE_2D, t);
    gl::PixelStorei(gl::UNPACK_ROW_LENGTH, 0);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, W, H, gl::BGRA, gl::UNSIGNED_BYTE, tmp.data());
}
void Tex::clear()
{
    if (t) gl::DeleteTextures(1, &t);
    t = 0; w = h = 0;
}
void image_fit(Tex &t, ImVec2 avail, double ar, ImVec2 *at, ImVec2 *size)
{
    if (!t.t || avail.x < 4 || avail.y < 4) return;
    double a = t.w * ar / t.h;
    float w = std::min(avail.x, (float)(avail.y * a)), h = (float)(w / a);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 o(p.x + (avail.x - w) / 2, p.y + (avail.y - h) / 2);
    ImGui::GetWindowDrawList()->AddImage(t.id(), o, ImVec2(o.x + w, o.y + h));
    ImGui::Dummy(avail);
    if (at) *at = o;
    if (size) *size = ImVec2(w, h);
}

// ---------------------------------------------------------------- windows, tasks, dialogs
static std::vector<std::unique_ptr<Win>> g_wins, g_new;
void add_window(std::unique_ptr<Win> w) { g_new.push_back(std::move(w)); }

static std::vector<std::shared_ptr<Task>> g_tasks;
void run_task(const std::string &title, std::function<void(Task &)> work, std::function<void(bool)> then)
{
    auto t = std::make_shared<Task>();
    t->title = title; t->then = then;
    g_tasks.push_back(t);
    std::thread([t, work] {
        try { work(*t); t->ok = !t->cancel; }
        catch (Cancelled &) { t->ok = false; }
        catch (std::exception &e) { t->ok = false; t->error = e.what(); }
        t->finished = true;
    }).detach();
}

struct Dialog {
    std::string title, text;
    std::vector<std::string> opts;
    std::function<void(int)> pick;
    bool opened = false;
    bool input = false;
    char buf[256] = "";
    std::function<void(std::string)> got;
};
static std::deque<Dialog> g_dialogs;
void message(const std::string &title, const std::string &text) { g_dialogs.push_back({title, text, {"OK"}, nullptr}); }
void choose(const std::string &title, const std::string &text, const std::vector<std::string> &opts, std::function<void(int)> on_pick)
{
    g_dialogs.push_back({title, text, opts, on_pick});
}

// file dialogs answer on another thread: the answer waits here for the main loop
static std::mutex g_cbmu;
static std::vector<std::function<void()>> g_callbacks;
static void post(std::function<void()> f) { std::lock_guard<std::mutex> l(g_cbmu); g_callbacks.push_back(std::move(f)); }
struct DlgCtx { std::function<void(std::string)> then; };
static void SDLCALL dlg_done(void *ud, const char *const *files, int)
{
    auto *c = (DlgCtx *)ud;
    std::string f = files && files[0] ? files[0] : "";
    auto then = c->then;
    delete c;
    if (!f.empty()) post([then, f] { then(f); });
}
void ask_text(const std::string &title, const std::string &prompt, const std::string &value, std::function<void(std::string)> then)
{
    Dialog d;
    d.title = title; d.text = prompt; d.opts = {"OK"}; d.input = true; d.got = then;
    snprintf(d.buf, sizeof d.buf, "%s", value.c_str());
    g_dialogs.push_back(d);
}
void open_file_dialog(const std::string &what, const char *patterns, std::function<void(std::string)> then)
{
    static std::string name, pat;
    name = what; pat = patterns;
    static SDL_DialogFileFilter flt[2];
    flt[0] = {name.c_str(), pat.c_str()};
    flt[1] = {"All files", "*"};
    SDL_ShowOpenFileDialog(dlg_done, new DlgCtx{then}, g_window, flt, 2, nullptr, false);
}
void open_folder_dialog(const std::string &, const std::string &start, std::function<void(std::string)> then)
{
    static std::string s;
    s = start;
    SDL_ShowOpenFolderDialog(dlg_done, new DlgCtx{then}, g_window, s.empty() ? nullptr : s.c_str(), false);
}
void open_path(const std::string &p)
{
    std::string u = "file://";
    for (unsigned char c : p) {
        if (c == '\\') u += '/';
        else if (c == ' ' || c == '#' || c == '%' || c == '?') u += fmt("%%%02X", c);
        else u += (char)c;
    }
#ifdef _WIN32
    if (u.size() > 7 && u[7] != '/') u.insert(7, "/");
#endif
    SDL_OpenURL(u.c_str());
}

// ---------------------------------------------------------------- settings
static std::string state_file()
{
    char *d = SDL_GetPrefPath("AssunaYuuki", "TeletextRescue");
    std::string p = d ? std::string(d) + "state.json" : "state.json";
    SDL_free(d);
    return p;
}
Json &state()
{
    static Json s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        try { s = load_json(state_file(), Json::object()); } catch (...) { s = Json::object(); }
        if (!s.is_obj()) s = Json::object();
    }
    return s;
}
void save_state() { try { save_json(state_file(), state(), 1); } catch (...) {} }
static void recent_add(const std::string &path)
{
    Json old = state()["recent"], now = Json::array();
    time_t t = time(nullptr);
    char tb[32];
    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", localtime(&t));
    Json e = Json::object(); e["path"] = path; e["time"] = std::string(tb);
    now.push(e);
    for (size_t i = 0; i < old.size() && now.size() < 12; i++)
        if (old[i].get_str("path") != path) now.push(old[i]);
    state()["recent"] = now;
    save_state();
}

// ---------------------------------------------------------------- sound
static SDL_AudioStream *g_audio = nullptr;
void stop_sound()
{
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    g_audio = nullptr;
}
void play_wav(const std::vector<uint8_t> &wav)
{
    stop_sound();
    SDL_AudioSpec spec;
    Uint8 *buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav.data(), wav.size()), true, &spec, &buf, &len)) return;
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio) {
        SDL_PutAudioStreamData(g_audio, buf, (int)len);
        SDL_ResumeAudioStreamDevice(g_audio);
    }
    SDL_free(buf);
}

// ---------------------------------------------------------------- widgets
void header(const std::string &title, const std::string &sub)
{
    ImGui::PushFont(F[1]);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    if (!sub.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, MUTED);
        ImGui::TextUnformatted(sub.c_str());
        ImGui::PopStyleColor();
    }
}
bool accent_button(const char *label, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ACCENT2);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0xe0, 0x8a, 0x10));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x11, 0x11, 0x11));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}
void wrapped(ImU32 c, const std::string &s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}
void text_box(const char *id, const std::string &text, ImVec2 size)
{
    // InputTextMultiline wants a writable buffer; it stays read-only
    static std::map<std::string, std::string> bufs;
    std::string &b = bufs[id];
    if (b != text) b = text;
    ImGui::PushFont(F[3]);
    ImGui::InputTextMultiline(id, b.data(), b.size() + 1, size, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
}

// ---------------------------------------------------------------- text window
struct TextWin : Win {
    std::string sub, body;
    void draw() override
    {
        header(title.substr(0, title.find("##")), sub);
        if (ImGui::Button("Copy all")) ImGui::SetClipboardText(body.c_str());
        text_box(("##t" + title).c_str(), body);
    }
};
void text_window(const std::string &title, const std::string &sub, const std::string &body)
{
    auto w = std::make_unique<TextWin>();
    static int n = 0;
    w->title = title + "##text" + std::to_string(++n);
    w->sub = sub; w->body = body;
    add_window(std::move(w));
}

// ---------------------------------------------------------------- export
void export_all(const ResultPtr &r)
{
    open_folder_dialog("Folder for the export", dirname(r->path), [r](std::string d) {
        std::string out = path_join(d, basename(stem_path(r->path)) + "_decoded");
        run_task("Export", [r, out](Task &t) {
            Progress pr;
            pr.on_step = [&](const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.step = s; };
            pr.on_log = [&](const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.log.push_back(s); };
            std::string res = exportResult(*r, out, pr);
            std::lock_guard<std::mutex> l(t.mu);
            t.log.push_back(res);
        }, [out](bool ok) { if (ok) open_path(out); });
    });
}

// ---------------------------------------------------------------- opening a recording
static VbiFormat g_format = VbiFormat::Unknown;
static bool g_unverified = false;

// what decodes: graphics card (OpenCL) or processor; then(gpu)
static void pick_device(bool force, std::function<void(bool)> then)
{
    Json &st = state();
    int saved = st["device"].is_null() ? -2 : st["device"].integer();
    bool ask = st.get_bool("device_ask", true);
    std::string name;
    uint64_t mem = 0;
    bool have = gpuDeviceInfo(name, mem);
    if (!force && (!ask || !have)) { then(have && saved != -1); return; }
    std::vector<std::string> opts;
    if (have) opts.push_back("Graphics card: " + name + fmt("  (%llu MB)", (unsigned long long)mem) + "  \xE2\x80\x94 fastest");
    opts.push_back("Processor (CPU) \xE2\x80\x94 slower, works on any computer");
    if (have) {
        opts.push_back("Graphics card, and do not ask again");
        opts.push_back("Processor, and do not ask again");
    }
    choose("Decode on", have ? "What should decode the recording?" : "No graphics card with OpenCL was found \xE2\x80\x94 the recording is decoded on the processor.",
           opts, [have, then](int c) {
               bool gpu = have && (c == 0 || c == 2);
               state()["device"] = gpu ? 0 : -1;
               state()["device_ask"] = c < 2;
               save_state();
               then(gpu);
           });
}

static void decode(const std::string &f, bool reunpack, bool again)
{
    pick_device(false, [f, reunpack, again](bool gpu) {
        auto res = std::make_shared<DecodeResult>();
        VbiFormat fm = g_format;
        bool unv = g_unverified;
        run_task("Opening " + basename(f), [f, res, gpu, fm, unv, reunpack, again](Task &t) {
            DecodeOptions opt;
            opt.gpu = gpu; opt.force = fm; opt.keepUnverified = unv; opt.cancel = &t.cancel; opt.reunpack = reunpack; opt.again = again;
            opt.progress = [&](double, const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.step = s; };
            opt.log = [&](const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.log.push_back(s); };
            opt.count = [&](int64_t a, int64_t b) { std::lock_guard<std::mutex> l(t.mu); t.done = a; t.all = b; };
            std::string err;
            if (!decodeFile(f, opt, *res, err)) {
                if (t.cancel) throw Cancelled();
                throw std::runtime_error(err == "no data lines found" ? "Nothing the program can decode was found in this recording." : err);
            }
        }, [res](bool ok) { if (ok) open_results(res); });
    });
}

static void open_recording(const std::string &f)
{
    if (!exists(f)) { message("Could not open", "The file does not exist:\n" + f); return; }
    recent_add(f);
    if (resultsCached(f)) {
        std::vector<std::string> opts = {"Open the saved results (no decoding)", "Decode the recording again"};
        if (is_flac(f)) opts.push_back("Unpack the FLAC and decode again");
        choose("Already decoded", basename(f) + " was already decoded.\nResults: " + legacyReportDir(f) + "\n\nDecode it again?", opts, [f](int c) {
            if (c == 0) {
                auto res = std::make_shared<DecodeResult>();
                run_task("Opening " + basename(f), [f, res](Task &t) {
                    { std::lock_guard<std::mutex> l(t.mu); t.step = "Opening the saved results"; }
                    std::string err;
                    if (!loadResults(f, *res, err)) throw std::runtime_error(err);
                }, [res](bool ok) { if (ok) open_results(res); });
            } else
                decode(f, c == 2, true);
        });
        return;
    }
    if (is_flac(f) && flac_cached(f)) {
        choose("FLAC recording", "This recording was already unpacked:\n" + flac_cache_path(f), {"Use the unpacked recording", "Unpack it again"},
               [f](int c) { decode(f, c == 1, false); });
        return;
    }
    decode(f, false, false);
}

void open_results(const ResultPtr &r)
{
    for (auto &s : ui::services_of(*r))
        if (s.window != ui::ServiceWindow::Sign || !r->silentRadio.empty()) open_service(r, s);
    recording_window(r);
}

void open_service(const ResultPtr &r, const Service &s)
{
    using ui::ServiceWindow;
    switch (s.window) {
    case ServiceWindow::StarSight: starsight_window(r); break;
    case ServiceWindow::Captions: captions_window(r); break;
    case ServiceWindow::Xds: xds_window(r); break;
    case ServiceWindow::Sign: sign_window(r); break;
    case ServiceWindow::Amol: amol_window(r); break;
    case ServiceWindow::Nabts: nabts_window(r); break;
    case ServiceWindow::Teletext: teletext_window(r); break;
    case ServiceWindow::Data: data_window(r, s.dataKind); break;
    case ServiceWindow::Gemstar: gemstar_window(r); break;
    case ServiceWindow::Jtes: jtes_window(r); break;
    case ServiceWindow::Vitc: {
        std::string t;
        for (auto &l : r->vitcReport) t += l + "\n";
        for (size_t i = 0; i < r->vitc.size(); i += 25) t += "[" + ui::rec_time(r->vitc[i].frame / ui::frame_rate(*r)) + "]  " + r->vitc[i].text() + "\n";
        text_window("VITC time code \xE2\x80\x94 " + ui::file_name(*r), "time code of the tape, read from the VBI", t);
        break;
    }
    case ServiceWindow::Finds: {   // files and texts as a list (the Windows program has its own window)
        std::string t;
        for (auto &f : findsOf(*r)) {
            t += f.name + (f.shortName.empty() ? "" : " (" + f.shortName + ")") + "  [" + f.type + "]  " + f.status + "\n    " + f.service + ", " + f.where +
                 (f.when.empty() ? "" : "  " + f.when) + (f.path.empty() ? "" : "\n    " + f.path) + (f.note.empty() ? "" : "\n    " + f.note) + "\n";
            for (auto &l : f.text) t += "    " + l + "\n";
            t += "\n";
        }
        text_window("Found files and texts \xE2\x80\x94 " + ui::file_name(*r), "files and texts the recording brought", t);
        break;
    }
    case ServiceWindow::Tvinform:
        text_window("TV-Inform \xE2\x80\x94 " + ui::file_name(*r), "\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC (GOST R 50822-95), voted carousel pages", ui::tvinform_text(*r));
        break;
    case ServiceWindow::LegacyFile:
        if (ends_with_i(s.dataKind, ".txt")) text_window(s.name + " \xE2\x80\x94 " + ui::file_name(*r), s.note, read_text(s.dataKind));
        else open_path(s.dataKind);
        break;
    case ServiceWindow::Report: text_window("Report \xE2\x80\x94 " + ui::file_name(*r), "everything the decoder found and did", ui::full_report(*r)); break;
    }
}

// ---------------------------------------------------------------- the main window
static void open_vbi_dialog()
{
    open_file_dialog("VBI recording or stream", "vbi;tbc;flac;t42;t34;t33;ts;mts;m2ts", [](std::string f) { open_recording(f); });
}
static void open_packets_dialog()
{
    open_file_dialog("Teletext / NABTS packets", "t42;t34;t33", [](std::string f) { open_recording(f); });
}

static void card(const char *id, ImVec2 size, const std::function<void()> &body)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PANEL);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
    body();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static bool g_quit = false;
static void main_window()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40, 24));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open .vbi recording\xE2\x80\xA6")) open_vbi_dialog();
            if (ImGui::MenuItem("Open teletext / NABTS packets (.t42, .t34, .t33)\xE2\x80\xA6")) open_packets_dialog();
            if (ImGui::BeginMenu("Recent")) {
                Json &rec = state()["recent"];
                if (rec.size() == 0) ImGui::MenuItem("(empty)", nullptr, false, false);
                for (size_t i = 0; i < rec.size(); i++) {
                    std::string p = rec[i].get_str("path");
                    if (ImGui::MenuItem((basename(p) + "   \xE2\x80\x94   " + dirname(p) + "##r" + std::to_string(i)).c_str())) { open_recording(p); break; }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) g_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Decode on: graphics card or processor\xE2\x80\xA6")) pick_device(true, [](bool) {});
            if (ImGui::BeginMenu("Recording format (capture chip)")) {
                if (ImGui::MenuItem("Recognise automatically", nullptr, g_format == VbiFormat::Unknown)) g_format = VbiFormat::Unknown;
                for (int i = 1; i < (int)VbiFormat::Count; i++)
                    if (ImGui::MenuItem(formatName((VbiFormat)i), nullptr, (int)g_format == i)) g_format = (VbiFormat)i;
                ImGui::EndMenu();
            }
            ImGui::MenuItem("Keep StarSight schedules from packets that fail the CRC", nullptr, &g_unverified);
            // a found file (Бизнес-Текст, TV-Inform) received twice: each copy fills what the other did not read
            if (ImGui::MenuItem("Combine two receptions of a found file\xE2\x80\xA6"))
                open_file_dialog("A found file (first reception)", "*", [](std::string a) {
                    open_file_dialog("The same file from another reception", "*", [a](std::string b) {
                        if (b.empty() || b == a) return;
                        run_task("Combining two receptions", [a, b](Task &t) {
                            std::string out, log = combineReceptions(a, {b}, out);
                            std::lock_guard<std::mutex> l(t.mu);
                            t.log.push_back(log);
                        }, [](bool) {});
                    });
                });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About Teletext Rescue"))
                message("About", "Teletext Rescue " TR_VERSION "\n\nRecovers data hidden in the vertical blanking interval of VBI recordings:\n"
                                 "WST teletext (625 and 525 lines, DVB), Japanese teletext (JTES), NABTS / NAPLPS,\n"
                                 "StarSight and Gemstar GUIDE Plus+ programme guides, EIA-608 captions and XDS, AMOL,\n"
                                 "Silent Radio LED sign feed, TV-Inform, Interactive Network and other datacasts.\n\n"
                                 "Copyright (c) 2026 AssunaYuuki. GNU General Public License v3.0.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::PushFont(F[2]);
    ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
    ImGui::TextUnformatted("Teletext Rescue");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, MUTED);
    ImGui::TextUnformatted("Teletext, programme guides, captions and data\nhidden in the vertical blanking interval");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 14));
    if (accent_button("  Open .vbi recording\xE2\x80\xA6  ", ImVec2(260, 44))) open_vbi_dialog();
    ImGui::SameLine(0, 12);
    if (ImGui::Button("Open packets .t42 / .t34 / .t33\xE2\x80\xA6", ImVec2(280, 44))) open_packets_dialog();
    ImGui::SameLine(0, 12);
    if (ImGui::Button("Decode on: graphics card or processor\xE2\x80\xA6", ImVec2(340, 44))) pick_device(true, [](bool) {});
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 760);
    wrapped(MUTED, "A recording is examined automatically: the capture chip and format (bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx, 4fsc .tbc) "
                   "and every VBI line are identified and everything readable is decoded and opened.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 12));
    float cw = (ImGui::GetContentRegionAvail().x - 12) / 2, ch = ImGui::GetContentRegionAvail().y - 16;
    card("##recent", ImVec2(cw, ch), [] {
        ImGui::PushFont(F[1]); ImGui::TextUnformatted("Recent"); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6));
        Json &rec = state()["recent"];
        if (rec.size() == 0) wrapped(MUTED, "Nothing yet \xE2\x80\x94 open a recording.");
        for (size_t i = 0; i < rec.size() && i < 10; i++) {
            std::string p = rec[i].get_str("path");
            ImGui::PushID((int)i);
            ImGui::PushStyleColor(ImGuiCol_Button, 0);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
            if (ImGui::Button((basename(p) + "     " + dirname(p) + "    " + rec[i].get_str("time")).c_str(), ImVec2(-1, 34))) { std::string q = p; ImGui::PopStyleVar(); ImGui::PopStyleColor(); ImGui::PopID(); open_recording(q); return; }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    });
    ImGui::SameLine(0, 12);
    card("##reads", ImVec2(cw, ch), [] {
        ImGui::PushFont(F[1]); ImGui::TextUnformatted("What it reads"); ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6));
        static const std::pair<const char *, const char *> svc[] = {
            {"WST teletext", "WST teletext \xE2\x80\x94 625 and 525 lines, DVB"},
            {"teletext", "Japanese teletext (\xE6\x96\x87\xE5\xAD\x97\xE5\xA4\x9A\xE9\x87\x8D\xE6\x94\xBE\xE9\x80\x81, JTES)"},
            {"StarSight", "StarSight programme guide \xE2\x80\x94 channels, schedules, titles"},
            {"Gemstar", "Gemstar GUIDE Plus+ programme guide"},
            {"CC", "CC captions, text mode and XDS station data (line 21)"},
            {"Silent Radio", "Silent Radio \xE2\x80\x94 LED news sign service"},
            {"NABTS", "NABTS / NAPLPS (CBS ExtraVision, NBC Teletext)"},
            {"encrypted datacast", "Datacasts at 5.73 Mbit/s"},
            {"AMOL", "Nielsen AMOL \xE2\x80\x94 source ID and station clock"},
            {"tv-inform", "TV-Inform (\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC), Russia"}};
        for (auto &s : svc) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float th = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + th / 2 - 6), ImVec2(p.x + 12, p.y + th / 2 + 6), service_colour(s.first));
            ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
            ImGui::TextUnformatted(s.second);
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::Dummy(ImVec2(0, 8));
        wrapped(MUTED, "Chips: bt8x8, cx88, saa713x, cx23885, ivtv/cx18, em28xx (PAL/SECAM/NTSC); .tbc (4fsc, 16 bit); FLAC-compressed captures");
    });
    ImGui::End();
}

static void draw_tasks()
{
    for (size_t i = 0; i < g_tasks.size();) {
        auto t = g_tasks[i];
        if (t->finished) {
            g_tasks.erase(g_tasks.begin() + i);
            if (!t->ok && !t->cancel && !t->error.empty()) message(t->title, t->error);
            if (t->then) t->then(t->ok);
            continue;
        }
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(640, 380), ImGuiCond_Appearing);
        ImGuiWindowClass wc;   // a window of its own, as the others
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&wc);
        ImGui::Begin((t->title + "##task" + std::to_string((intptr_t)t.get())).c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        std::string step;
        std::vector<std::string> log;
        int64_t d, a;
        {
            std::lock_guard<std::mutex> l(t->mu);
            step = t->step; d = t->done; a = t->all;
            size_t from = t->log.size() > 200 ? t->log.size() - 200 : 0;
            log.assign(t->log.begin() + from, t->log.end());
        }
        ImGui::TextUnformatted(step.empty() ? "Working\xE2\x80\xA6" : step.c_str());
        float frac = a > 0 ? (float)d / a : (float)fmod(ImGui::GetTime() * 0.4, 1.0);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ACCENT);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), a > 0 ? fmt("%lld / %lld frames", (long long)d, (long long)a).c_str() : "");
        ImGui::PopStyleColor();
        ImGui::BeginChild("##log", ImVec2(-1, -40), ImGuiChildFlags_Borders);
        ImGui::PushFont(F[3]);
        for (auto &l : log) ImGui::TextUnformatted(l.c_str());
        ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        if (ImGui::Button(t->cancel ? "Stopping\xE2\x80\xA6" : "Cancel", ImVec2(120, 0))) t->cancel = true;
        ImGui::End();
        i++;
    }
}

static void draw_dialogs()
{
    if (g_dialogs.empty()) return;
    Dialog &d = g_dialogs.front();
    std::string id = d.title + "##dlg";
    if (!d.opened) { ImGui::OpenPopup(id.c_str()); d.opened = true; }
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(560);
        ImGui::TextUnformatted(d.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        int picked = -2;
        if (d.input) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(560);
            if (ImGui::InputText("##in", d.buf, sizeof d.buf, ImGuiInputTextFlags_EnterReturnsTrue)) picked = 0;
            if (ImGui::Button("Cancel", ImVec2(120, 0))) picked = -1;
            ImGui::SameLine();
        }
        for (size_t i = 0; i < d.opts.size(); i++)
            if (i == 0 ? accent_button(d.opts[i].c_str(), ImVec2(d.opts.size() > 1 ? 560 : 120, 0)) : ImGui::Button(d.opts[i].c_str(), ImVec2(560, 0))) picked = (int)i;
        if (d.opts.size() > 1 && ImGui::Button("Cancel", ImVec2(560, 0))) picked = -1;
        if (d.input && picked == 0) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            auto got = d.got;
            std::string v = d.buf;
            g_dialogs.pop_front();
            if (got) got(v);
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) picked = -1;
        if (picked != -2) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            auto pick = d.pick;
            g_dialogs.pop_front();
            if (pick && picked >= 0) pick(picked);
            return;
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- fonts and style
// Latin, Cyrillic, punctuation, arrows, shapes and the symbols the windows use (dashes, ellipsis, triangles, stars, notes)
static const ImWchar *latin_ranges()
{
    static ImVector<ImWchar> r;
    if (r.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
        static const ImWchar extra[] = {0x2000, 0x206F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2300, 0x23FF, 0x2500, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0x3000, 0x303F, 0};
        b.AddRanges(extra);
        b.BuildRanges(&r);
    }
    return r.Data;
}
static ImFont *load_font(const std::vector<std::string> &files, float size, const std::vector<std::string> &cjk, const ImWchar *ranges)
{
    ImGuiIO &io = ImGui::GetIO();
    ImFont *f = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    for (auto &p : files)
        if (exists(p)) { f = io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg, latin_ranges()); break; }
    if (!f) { ImFontConfig c; c.SizePixels = size; f = io.Fonts->AddFontDefault(&c); }
    // kanji and kana from a second font, merged in
    ImFontConfig m;
    m.MergeMode = true; m.OversampleH = 1; m.PixelSnapH = true;
    for (auto &p : cjk)
        if (exists(p)) {
            io.Fonts->AddFontFromFileTTF(p.c_str(), size, &m, ranges ? ranges : latin_ranges());
            if (ranges) io.Fonts->AddFontFromFileTTF(p.c_str(), size, &m, latin_ranges());   // symbols the first font lacks
            break;
        }
    return f;
}

static void setup_fonts(float scale)
{
    std::vector<std::string> sans, bold, mono, cjk;
#ifdef _WIN32
    sans = {"C:/Windows/Fonts/segoeui.ttf"}; bold = {"C:/Windows/Fonts/segoeuib.ttf"}; mono = {"C:/Windows/Fonts/consola.ttf"};
    cjk = {"C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc", "C:/Windows/Fonts/YuGothM.ttc"};
#elif defined(__APPLE__)
    sans = {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc"};
    bold = {"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"};
    mono = {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf"};
    cjk = {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf",
           "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc"};
#else
    sans = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
    bold = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf"};
    mono = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"};
    cjk = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
           "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
           "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf", "/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf"};
#endif
    ImGuiIO &io = ImGui::GetIO();
    const ImWchar *jp = io.Fonts->GetGlyphRangesJapanese();
    F[0] = load_font(sans, 15 * scale, cjk, jp);
    F[1] = load_font(bold.empty() ? sans : bold, 16 * scale, cjk, jp);
    F[2] = load_font(bold.empty() ? sans : bold, 30 * scale, cjk, nullptr);
    F[3] = load_font(mono, 14 * scale, cjk, jp);
    io.FontDefault = F[0];
}

static void setup_style(float scale)
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 6; s.FrameRounding = 4; s.ChildRounding = 6; s.PopupRounding = 4; s.GrabRounding = 4;
    s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(8, 6); s.WindowPadding = ImVec2(14, 12);
    s.WindowBorderSize = 1; s.ChildBorderSize = 0;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = v4(BG);
    c[ImGuiCol_ChildBg] = v4(BG);
    c[ImGuiCol_PopupBg] = v4(PANEL);
    c[ImGuiCol_MenuBarBg] = v4(PANEL);
    c[ImGuiCol_Border] = v4(LINE);
    c[ImGuiCol_Text] = v4(FG);
    c[ImGuiCol_TextDisabled] = v4(DIM);
    c[ImGuiCol_FrameBg] = v4(FIELD);
    c[ImGuiCol_FrameBgHovered] = v4(LINE);
    c[ImGuiCol_FrameBgActive] = v4(LINE);
    c[ImGuiCol_TitleBg] = v4(PANEL);
    c[ImGuiCol_TitleBgActive] = v4(rgb(0x2d, 0x2a, 0x24));
    c[ImGuiCol_TitleBgCollapsed] = v4(PANEL);
    c[ImGuiCol_Button] = v4(FIELD);
    c[ImGuiCol_ButtonHovered] = v4(LINE);
    c[ImGuiCol_ButtonActive] = v4(SEL);
    c[ImGuiCol_Header] = v4(SEL);
    c[ImGuiCol_HeaderHovered] = v4(rgb(0x45, 0x34, 0x18));
    c[ImGuiCol_HeaderActive] = v4(SEL);
    c[ImGuiCol_CheckMark] = v4(ACCENT);
    c[ImGuiCol_SliderGrab] = v4(ACCENT);
    c[ImGuiCol_SliderGrabActive] = v4(ACCENT2);
    c[ImGuiCol_Tab] = v4(PANEL);
    c[ImGuiCol_TabHovered] = v4(LINE);
    c[ImGuiCol_TabSelected] = v4(SEL);
    c[ImGuiCol_TableHeaderBg] = v4(PANEL);
    c[ImGuiCol_TableRowBgAlt] = v4(rgb(0x20, 0x22, 0x26));
    c[ImGuiCol_ScrollbarBg] = v4(BG);
    c[ImGuiCol_PlotHistogram] = v4(ACCENT);
    c[ImGuiCol_TextSelectedBg] = v4(SEL);
    c[ImGuiCol_NavHighlight] = v4(ACCENT);
    s.ScaleAllSizes(scale);
}

// a picture of the whole program (TR_XSHOT=file.png): for checking the windows
static void screenshot(const std::string &path)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (w <= 0 || h <= 0) return;
    std::vector<uint32_t> px((size_t)w * h);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
    gl::ReadPixels(0, 0, w, h, gl::BGRA, gl::UNSIGNED_BYTE, px.data());
    Image im(w, h);
    for (int y = 0; y < h; y++)   // OpenGL rows go bottom up
        for (int x = 0; x < w; x++) im.px[(size_t)y * w + x] = px[(size_t)(h - 1 - y) * w + x] & 0xFFFFFF;
    png_save(path, im);
}

static void frame()
{
    // what file dialogs answered
    std::vector<std::function<void()>> cb;
    { std::lock_guard<std::mutex> l(g_cbmu); cb.swap(g_callbacks); }
    for (auto &f : cb) f();
    static const char *only = getenv("TR_XFOCUS");   // checking: only the window with this in its title
    for (auto &w : g_new)
        if (!only || w->title.find(only) != std::string::npos) g_wins.push_back(std::move(w));
    g_new.clear();

    main_window();
    ImGuiViewport *vp = ImGui::GetMainViewport();
    int k = 0;
    for (auto &w : g_wins) {
        if (!w->open) continue;
        ImGui::SetNextWindowSize(w->size, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 60 + (k % 8) * 28, vp->WorkPos.y + 50 + (k % 8) * 28), ImGuiCond_FirstUseEver);
        k++;
        // the system's title bar names, moves and closes it; no second one of ImGui inside
        int fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
        w->flags(fl);
        // a window of its own on the desktop (title bar, taskbar / Dock entry), never merged into the main one
        ImGuiWindowClass wc;
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        wc.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&wc);
        if (ImGui::Begin(w->title.c_str(), &w->open, fl)) w->draw();
        ImGui::End();
    }
    g_wins.erase(std::remove_if(g_wins.begin(), g_wins.end(), [](const std::unique_ptr<Win> &w) { return !w->open; }), g_wins.end());
    draw_tasks();
    draw_dialogs();
}

int run()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    g_scale = scale;
    // OpenGL 3.2 core (what macOS gives), GLSL 150; every window of the program shares this context's textures
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    g_window = SDL_CreateWindow("Teletext Rescue " TR_VERSION, (int)(1320 * scale), (int)(900 * scale),
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) { fprintf(stderr, "OpenGL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(g_window, g_gl);
    SDL_GL_SetSwapInterval(1);
    gl::load();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // windows of the program are windows of the system (as on Windows), not panels inside the main one
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoAutoMerge = true;
    io.ConfigViewportsNoTaskBarIcon = false;
    io.ConfigViewportsNoDecoration = false;   // the system's own title bar, buttons and resizing
    setup_style(scale);
    setup_fonts(scale);
    ImGui_ImplSDL3_InitForOpenGL(g_window, g_gl);
    ImGui_ImplOpenGL3_Init("#version 150");

    // for checking: TR_XOPEN opens a recording (saved results), TR_XSHOT saves a picture and quits
    const char *shot = getenv("TR_XSHOT");
    if (const char *f = getenv("TR_XOPEN")) {
        auto res = std::make_shared<DecodeResult>();
        std::string err;
        if (loadResults(f, *res, err)) open_results(res);
        else fprintf(stderr, "TR_XOPEN: %s\n", err.c_str());
    }
    int frames = 0;
    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) g_quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window)) g_quit = true;
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) open_recording(e.drop.data);
        }
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) SDL_Delay(15);   // the other windows still work
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        int fw = 0, fh = 0;
        SDL_GetWindowSizeInPixels(g_window, &fw, &fh);
        gl::Viewport(0, 0, fw, fh);
        gl::ClearColor(0x1b / 255.f, 0x1c / 255.f, 0x1f / 255.f, 1);
        gl::Clear(gl::COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames == 30) { screenshot(shot); g_quit = true; }
        // the windows of their own: drawn by ImGui into theirs, then the main window's context again
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(g_window, g_gl);
        SDL_GL_SwapWindow(g_window);
    }
    stop_sound();
    g_wins.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(g_gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
}

int main(int, char **) { return xg::run(); }
