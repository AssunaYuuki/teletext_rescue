// The BBC Micro window: the emulator (bbcmicro.h) with its picture, keyboard and sound; a program (telesoftware,
// a listing, a file of a .ssd disc) is typed into BASIC and run.
#include "gui_app.h"
#include "bbcmicro.h"
#include "telesoft.h"
#include "util.h"
#include <mmsystem.h>
#include <shellapi.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace ui {
namespace {

std::string exe_dir()
{
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return dirname(N(p));
}

// the ROMs: roms\os12.rom and roms\basic2.rom next to the program (also os.rom / BASIC.ROM, the names jsbeeb uses)
bool find_roms(Bytes& os, Bytes& basic, std::string& where)
{
    for (const std::string& d : {path_join(exe_dir(), "roms"), path_join(dirname(exe_dir()), "roms"), exe_dir()}) {
        for (auto& on : {"os12.rom", "os.rom", "OS12.ROM"})
            if (exists(path_join(d, on))) { os = read_file(path_join(d, on)); break; }
        for (auto& bn : {"basic2.rom", "BASIC.ROM", "basic.rom"})
            if (exists(path_join(d, bn))) { basic = read_file(path_join(d, bn)); break; }
        if (os.size() == 16384 && basic.size() == 16384) { where = d; return true; }
        os.clear(); basic.clear();
    }
    where = path_join(exe_dir(), "roms");
    return false;
}

// a program as BASIC is to get it: lines ended by CR, NEW before and RUN after
std::string as_typed(const std::string& text)
{
    std::string t;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        t += (c == '\n' || c == '\x07') ? '\r' : c;
    }
    // listings of telesoftware keep their escapes
    if (t.find("||") != std::string::npos || t.find("|`") != std::string::npos) t = telesoftUnescape(t);
    std::string out = "NEW\r";
    for (auto& l : split(t, '\r')) {
        std::string s = l;
        if (s.find("not received ***]") != std::string::npos) continue;
        if (strip(s).empty()) continue;
        out += s + "\r";
    }
    return out + "RUN\r";
}

// the files of a .ssd disc image (Acorn DFS): name -> data
std::vector<std::pair<std::string, std::string>> ssd_files(const Bytes& d)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (d.size() < 512) return out;
    int n = d[256 + 5] / 8;
    for (int i = 0; i < n && i < 31; i++) {
        std::string nm((const char*)&d[8 + 8 * i], 7);
        while (!nm.empty() && nm.back() == ' ') nm.pop_back();
        char dir = (char)(d[15 + 8 * i] & 0x7F);
        const uint8_t* e = &d[256 + 8 + 8 * i];
        int len = e[4] | e[5] << 8 | ((e[6] >> 4) & 3) << 16;
        int st = e[7] | (e[6] & 3) << 8;
        if ((size_t)st * 256 + len > d.size()) continue;
        out.push_back({std::string(1, dir) + "." + nm, std::string((const char*)&d[(size_t)st * 256], (size_t)len)});
    }
    return out;
}

constexpr UINT WM_BBC_FRAME = WM_APP + 41;

struct BbcWin : Window {
    Header hd;
    HWND bTurbo = nullptr, bSound = nullptr, status = nullptr;
    bbc::Machine m;
    std::mutex mu;
    std::thread th;
    std::atomic<bool> quit{false}, turbo{false}, soundOn{true}, framePosted{false};
    std::vector<uint32_t> shown;
    std::string program, progName, romDir;
    std::string initialPath;   // a file to run when the window opens
    int64_t pasteAt = -1;   // frame from which the program is typed in (the machine has started)
    std::string pending;
    bool ready = false;
    // sound
    HWAVEOUT wo = nullptr;
    static constexpr int NB = 10;
    WAVEHDR hdr[NB]{};
    std::vector<int16_t> buf[NB];
    std::vector<int16_t> snd;
    RECT screen{};

    void start_audio()
    {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = bbc::Machine::RATE; f.wBitsPerSample = 16;
        f.nBlockAlign = 2; f.nAvgBytesPerSec = f.nSamplesPerSec * 2;
        if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = nullptr; return; }
        for (int i = 0; i < NB; i++) { buf[i].resize(882 * 2); hdr[i].dwFlags = WHDR_DONE; }
    }
    void play(const std::vector<int16_t>& s)
    {
        if (!wo || s.empty()) return;
        for (int i = 0; i < NB; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            size_t n = std::min(s.size(), buf[i].size());
            std::copy(s.begin(), s.begin() + n, buf[i].begin());
            hdr[i] = WAVEHDR{};
            hdr[i].lpData = (LPSTR)buf[i].data();
            hdr[i].dwBufferLength = (DWORD)(n * 2);
            waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
            return;
        }
        // all busy: the sound is behind, this frame's is dropped
    }
    void stop_audio()
    {
        if (!wo) return;
        waveOutReset(wo);
        for (int i = 0; i < NB; i++) if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
        waveOutClose(wo);
        wo = nullptr;
    }

    void loop()
    {
        timeBeginPeriod(1);
        LARGE_INTEGER fq, t0; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
        double next = 0;
        std::vector<int16_t> s;
        while (!quit) {
            bool fast;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (!pending.empty() && (int64_t)m.frames() >= pasteAt) { m.paste(pending); pending.clear(); }
                fast = turbo || m.pasting();
                int n = fast ? 8 : 1;
                for (int i = 0; i < n; i++) m.runFrame();
                shown = m.picture();
                s.clear();
                m.takeSound(s);
            }
            if (!fast && soundOn) play(s);
            if (!framePosted.exchange(true)) PostMessageW(hwnd, WM_BBC_FRAME, 0, 0);
            next += 0.02;
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            double now = (double)(t.QuadPart - t0.QuadPart) / fq.QuadPart;
            if (now - next > 0.2 || fast) next = now;   // behind (or running fast): no catching up
            double wait = next - now;
            if (wait > 0) Sleep((DWORD)(wait * 1000));
        }
        timeEndPeriod(1);
    }

    void run_program(const std::string& text, const std::string& name)
    {
        std::lock_guard<std::mutex> lk(mu);
        m.reset(true);
        pending = as_typed(text);
        pasteAt = (int64_t)m.frames() + 120;   // the MOS and BASIC start first
        progName = name;
        program = text;
        set_text(hd.sub, name.empty() ? "BBC Micro Model B \xC2\xB7 MOS 1.20, BASIC II" : name + " \xC2\xB7 typed into BASIC and run");
        if (hwnd) SetWindowTextW(hwnd, W(name.empty() ? std::string("BBC Micro") : "BBC Micro \xE2\x80\x94 " + name).c_str());
    }

    void load_file()
    {
        std::string p = open_file(hwnd, "A BBC BASIC program",
                                  {{"Programs (*.bbc, *.txt, *.bas, *.ssd)", "*.bbc;*.txt;*.bas;*.ssd"}, {"All files", "*.*"}});
        if (!p.empty()) load_path(p);
    }

    // a program file: typed into BASIC and run at once (a disc: its program; asked only when it has several)
    void load_path(const std::string& p)
    {
        try {
            Bytes d = read_file(p);
            if (ends_with_i(p, ".ssd")) {
                auto files = ssd_files(d);
                std::vector<std::string> names;
                std::vector<int> idx;
                for (size_t i = 0; i < files.size(); i++)
                    if (files[i].first != "$.!BOOT") { names.push_back(files[i].first + fmt("  (%zu bytes)", files[i].second.size())); idx.push_back((int)i); }
                if (names.empty()) { ask(hwnd, "Disc", "No files on this disc.", MB_ICONINFORMATION); return; }
                // a disc made for one program (its !BOOT runs AUTORUN): that program, no question
                int k = names.size() == 1 ? 0 : -1;
                if (k < 0)
                    for (size_t i = 0; i < idx.size(); i++) if (files[(size_t)idx[i]].first == "$.AUTORUN") k = (int)i;
                if (k < 0) k = choose(hwnd, "Disc " + basename(p), "Which program to run (it is typed into BASIC as *EXEC does):", names, 0);
                if (k < 0) return;
                auto& f = files[(size_t)idx[(size_t)k]];
                std::string t = f.second;
                if (t.rfind("NEW\r", 0) == 0) t.erase(0, 4);
                size_t r = t.rfind("RUN\r");
                if (r != std::string::npos && r + 4 == t.size()) t.resize(r);
                run_program(t, f.first == "$.AUTORUN" ? basename(stem_path(p)) : f.first.substr(2));
            } else {
                if (!d.empty() && d[0] == 0x0D && d.size() > 4) {
                    ask(hwnd, "Program", "This is a tokenised BASIC file (SAVEd); give the program as text (LIST, *SPOOL) or a telesoftware file.", MB_ICONINFORMATION);
                    return;
                }
                run_program(std::string(d.begin(), d.end()), basename(stem_path(p)));
            }
        } catch (std::exception& e) { ask(hwnd, "Program", e.what(), MB_ICONERROR); }
        SetFocus(hwnd);
    }

    // ---------------------------------------------------------------- keyboard
    static int vk_key(WPARAM vk, bool shift)
    {
        using namespace bbc;
        switch (vk) {
        case VK_SHIFT: return K_SHIFT;
        case VK_CONTROL: return K_CTRL;
        case VK_LEFT: return K_LEFT;
        case VK_RIGHT: return K_RIGHT;
        case VK_UP: return K_UP;
        case VK_DOWN: return K_DOWN;
        case VK_RETURN: return K_RETURN;
        case VK_BACK: case VK_DELETE: return K_DELETE;
        case VK_ESCAPE: return K_ESCAPE;
        case VK_TAB: return K_TAB;
        case VK_END: return K_COPY;
        case VK_CAPITAL: return K_CAPSLOCK;
        case VK_SPACE: return K_SPACE;
        case VK_F10: return K_F0;
        case VK_F1: return K_F1; case VK_F2: return K_F2; case VK_F3: return K_F3; case VK_F4: return K_F4; case VK_F5: return K_F5;
        case VK_F6: return K_F6; case VK_F7: return K_F7; case VK_F8: return K_F8; case VK_F9: return K_F9;
        }
        static const int letters[26] = {K_A, K_B, K_C, K_D, K_E, K_F, K_G, K_H, K_I, K_J, K_K, K_L, K_M,
                                        K_N, K_O, K_P, K_Q, K_R, K_S, K_T, K_U, K_V, K_W, K_X, K_Y, K_Z};
        static const int digits[10] = {K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9};
        if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
        if (vk >= '0' && vk <= '9' && !shift) return digits[vk - '0'];   // shifted digits: by their character
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return digits[vk - VK_NUMPAD0];
        return -1;
    }
    bool key_event(WPARAM vk, bool down)
    {
        bool shift = down && GetKeyState(VK_SHIFT) < 0;   // a digit is let go whatever the shift is then
        int k = vk_key(vk, shift);
        if (k < 0) return false;
        std::lock_guard<std::mutex> lk(mu);
        m.key(k, down);
        return true;
    }

    // ---------------------------------------------------------------- window
    void layout()
    {
        RECT rc; GetClientRect(hwnd, &rc);
        int y = hd.layout(hwnd);
        int sh = S(hwnd, 22);
        MoveWindow(status, S(hwnd, 14), rc.bottom - sh - S(hwnd, 4), rc.right - S(hwnd, 28), sh, TRUE);
        int top = y, bot = rc.bottom - sh - S(hwnd, 8);
        int aw = rc.right - S(hwnd, 28), ah = bot - top;
        // the picture 640 x 512 as a 4:3 television
        int w = aw, h = w * 3 / 4;
        if (h > ah) { h = ah; w = h * 4 / 3; }
        screen = {(rc.right - w) / 2, top + (ah - h) / 2, (rc.right - w) / 2 + w, top + (ah - h) / 2 + h};
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void paint(HDC dc)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ ob = SelectObject(mem, bm);
        fill(mem, rc, BG);
        std::vector<uint32_t> px;
        { std::lock_guard<std::mutex> lk(mu); px = shown; }
        if (px.size() == (size_t)bbc::Machine::W * bbc::Machine::H) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = bbc::Machine::W; bi.bmiHeader.biHeight = -bbc::Machine::H;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(mem, HALFTONE);
            StretchDIBits(mem, screen.left, screen.top, screen.right - screen.left, screen.bottom - screen.top, 0, 0, bbc::Machine::W,
                          bbc::Machine::H, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
    }
    void update_status()
    {
        std::string s;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!pending.empty() || m.pasting()) s = "typing the program into BASIC\xE2\x80\xA6";
            else s = std::string(m.capsLock() ? "CAPS LOCK  " : "") + (turbo ? "turbo  " : "");
        }
        s += "  \xC2\xB7  keys: F1-F9 = f1-f9, F10 = f0, End = COPY, Esc = ESCAPE, Pause = BREAK";
        if (get_text(status) != s) set_text(status, s);
    }

    LRESULT proc(UINT msg, WPARAM w, LPARAM l) override
    {
        switch (msg) {
        case WM_CREATE: {
            dark_window(hwnd);
            hd.create(hwnd, "BBC Micro", "BBC Micro Model B \xC2\xB7 MOS 1.20, BASIC II");
            hd.add(hwnd, 701, "Load program\xE2\x80\xA6", 130);
            hd.add(hwnd, 702, "Run again", 90);
            hd.add(hwnd, 703, "BREAK", 70);
            hd.add(hwnd, 704, "Power off / on", 120);
            bTurbo = hd.add(hwnd, 705, "Turbo", 70);
            bSound = hd.add(hwnd, 706, "Sound: on", 90);
            status = label(hwnd, "");
            set_label_colour(status, MUTED);
            buffer_label(status);
            Bytes os, basic;
            if (!find_roms(os, basic, romDir)) {
                set_text(status, "");
                ask(hwnd, "BBC Micro", "The ROMs of the BBC Micro are needed: put os12.rom (MOS 1.20) and basic2.rom (BASIC II), 16 KB each, in\n" + romDir +
                                         "\n(jsbeeb's os.rom and BASIC.ROM are the same).", MB_ICONINFORMATION);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            std::string err;
            if (!m.setRoms(os, basic, err)) { ask(hwnd, "BBC Micro", err, MB_ICONERROR); PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            m.reset(true);
            ready = true;
            if (!program.empty()) {
                std::string p = program, n = progName;
                run_program(p, n);
            }
            DragAcceptFiles(hwnd, TRUE);
            start_audio();
            th = std::thread([this] { loop(); });
            SetTimer(hwnd, 1, 500, nullptr);
            layout();
            if (!initialPath.empty()) { std::string p = initialPath; initialPath.clear(); load_path(p); }
            return 0;
        }
        case WM_DROPFILES: {   // a program dropped on the window: run
            wchar_t buf[MAX_PATH] = L"";
            DragQueryFileW((HDROP)w, 0, buf, MAX_PATH);
            DragFinish((HDROP)w);
            if (ready && buf[0]) load_path(N(buf));
            return 0;
        }
        case WM_BBC_FRAME:
            framePosted = false;
            InvalidateRect(hwnd, &screen, FALSE);
            return 0;
        case WM_TIMER: update_status(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); paint(dc); EndPaint(hwnd, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (w == VK_PAUSE) { std::lock_guard<std::mutex> lk(mu); m.reset(false); return 0; }
            if (w == VK_F4 && (GetKeyState(VK_MENU) < 0)) break;   // Alt+F4 closes
            if (!ready) break;
            if ((l & 0x40000000) && w != VK_SHIFT && w != VK_CONTROL) return 0;   // auto-repeat: the BBC repeats by itself
            if (key_event(w, true)) return 0;
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (!ready) break;
            if (key_event(w, false)) return 0;
            if (msg == WM_SYSKEYUP) break;
            return 0;
        case WM_CHAR: {
            if (!ready) return 0;
            wchar_t c = (wchar_t)w;
            // letters, space, digits, Return, Delete, Tab, Escape, Ctrl+letter go as keys (WM_KEYDOWN); the rest by character
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c < 32 || c == 127) return 0;
            if (c >= '0' && c <= '9' && GetKeyState(VK_SHIFT) >= 0) return 0;
            std::string s;
            if (c == 0xA3) s = "`";   // £
            else if (c < 128) s = std::string(1, (char)c);
            if (!s.empty()) { std::lock_guard<std::mutex> lk(mu); m.typeChars(s); }
            return 0;
        }
        case WM_KILLFOCUS: { std::lock_guard<std::mutex> lk(mu); m.releaseAll(); return 0; }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (!ready) return 0;
            if (id == 701) load_file();
            else if (id == 702) { if (!program.empty()) run_program(program, progName); else { std::lock_guard<std::mutex> lk(mu); m.reset(true); } }
            else if (id == 703) { std::lock_guard<std::mutex> lk(mu); m.reset(false); }
            else if (id == 704) { std::lock_guard<std::mutex> lk(mu); m.reset(true); pending.clear(); }
            else if (id == 705) { turbo = !turbo; set_text(bTurbo, turbo ? "Turbo: on" : "Turbo"); }
            else if (id == 706) { soundOn = !soundOn; set_text(bSound, soundOn ? "Sound: on" : "Sound: off"); }
            SetFocus(hwnd);
            return 0;
        }
        case WM_DESTROY:
            quit = true;
            if (th.joinable()) th.join();
            stop_audio();
            break;
        case WM_NCDESTROY: { LRESULT r = Window::proc(msg, w, l); delete this; return r; }
        }
        return Window::proc(msg, w, l);
    }
};

}  // namespace

bool is_bbc_file(const std::string& path) { return ends_with_i(path, ".bbc") || ends_with_i(path, ".ssd"); }

void bbc_open_file(HWND owner, const std::string& path)
{
    auto* w = new BbcWin;
    w->initialPath = path;
    int width = S(owner, 900), height = S(owner, 780);
    w->create(L"TRBbcMicro", "BBC Micro \xE2\x80\x94 " + basename(stem_path(path)), WS_OVERLAPPEDWINDOW, width, height, owner);
    ShowWindow(w->hwnd, SW_SHOW);
    SetFocus(w->hwnd);
}

bool open_bbc_program(HWND owner, const std::string& path)
{
    if (!is_bbc_file(path)) return false;
    bbc_open_file(owner, path);
    return true;
}

void bbc_window(HWND owner, const std::string& program, const std::string& name)
{
    auto* w = new BbcWin;
    w->program = program;
    w->progName = name;
    int width = S(owner, 900), height = S(owner, 780);
    w->create(L"TRBbcMicro", name.empty() ? "BBC Micro" : "BBC Micro \xE2\x80\x94 " + name, WS_OVERLAPPEDWINDOW, width, height, owner);
    ShowWindow(w->hwnd, SW_SHOW);
    SetFocus(w->hwnd);
}
}  // namespace ui
