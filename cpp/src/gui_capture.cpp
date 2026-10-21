// "Capture card — live": a capture card's picture and VBI decoded as they come in.
//   Top: card, input (tuner / composite / S-Video), TV standard, tuner channel; Start / Stop,
//   Record (the raw VBI to a .vbi file) and "Decode the recording" (the full decoder, all services).
//   Left: the picture; under it every captured VBI line as a raster, labelled with what is on it.
//   Right: the teletext page being received (type the number, arrows / wheel for the next one),
//   the list of pages seen so far, the rolling header, captions.
#include "gui_app.h"
#include "capture.h"
#include "live.h"
#include "export.h"
#include "linecolour.h"
#include "decoder.h"
#include "rescache.h"
#include <climits>
#include <ctime>
#include <windowsx.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <mmsystem.h>

namespace ui {
std::function<void(const std::string &, VbiFormat, const std::map<int, LineForce> &)> &decode_lines_hook()
{
    static std::function<void(const std::string &, VbiFormat, const std::map<int, LineForce> &)> h;
    return h;
}

std::function<void(const std::string &)> &open_recording_hook()
{
    static std::function<void(const std::string &)> f;
    return f;
}

namespace {
enum {
    ID_DEVICE = 700, ID_INPUT, ID_STD, ID_CHANNEL, ID_TUNE, ID_START, ID_RECORD, ID_DECODE,
    ID_PAGE, ID_PREV, ID_NEXT, ID_SUBPREV, ID_SUBNEXT, ID_CHARSET, ID_REVEAL, ID_WATCH, ID_PICTURE, ID_FINDS, ID_VPS, ID_NABTS, ID_JTES, ID_SSLIVE, ID_GEMLIVE, ID_SERVICES, ID_SOUND, ID_MUTE, ID_TESTFILE,
    ID_SAVE, ID_SYNCUP, ID_SYNCDN, ID_LINES, ID_SYNC0, ID_HSYNC, ID_VSYNC, ID_LAYOUT,
};
constexpr int kAutoSub = INT_MIN;   // subShown: the subpage received last

const std::vector<std::pair<std::string, std::string>> kCharsets = {
    {"", "Characters: automatic"}, {"latin", "Latin (Western Europe)"}, {"latin_e", "Latin (Polish, Czech, Hungarian...)"},
    {"latin_tr", "Latin (Turkish)"}, {"latin_bal", "Latin (Baltic)"}, {"cyr2", "Cyrillic (Russian)"}, {"cyr1", "Cyrillic (Serbian)"},
    {"cyr3", "Cyrillic (Ukrainian)"}, {"greek", "Greek"}, {"hebrew", "Hebrew"}};

HWND combo(HWND parent, int id)
{
    HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, 300, parent,
                             (HMENU)(INT_PTR)id, inst(), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font(parent), TRUE);
    SetWindowTheme(h, L"DarkMode_CFD", nullptr);
    return h;
}
void combo_fill(HWND h, const std::vector<std::string> &items, int sel)
{
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (auto &s : items) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)W(s).c_str());
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}
int combo_sel(HWND h) { return (int)SendMessageW(h, CB_GETCURSEL, 0, 0); }

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

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

struct CaptureWin : Window {
    Header hd;
    HWND c_dev = nullptr, c_input = nullptr, c_std = nullptr, c_cs = nullptr, e_chan = nullptr, b_tune = nullptr, l_chan = nullptr;
    HWND b_start = nullptr, b_watch = nullptr, b_rec = nullptr, b_decode = nullptr, e_page = nullptr, b_prev = nullptr, b_next = nullptr, b_sp = nullptr, b_sn = nullptr;
    std::string vpsShown;
    // the services menu (VPS / WSS, NABTS, Japanese, StarSight, Gemstar): its items as they read now and if they open
    HWND b_services = nullptr;
    std::string svcText[5] = {"VPS / WSS", "NABTS", "Japanese teletext", "StarSight", "Gemstar"};
    bool svcOn[5] = {true, false, false, false, false};
    std::string pages525Shown;   // 525 lines: NABTS / Japanese teletext pages so far
    HWND b_reveal = nullptr, status = nullptr, c_pic = nullptr, b_finds = nullptr, c_snd = nullptr, b_mute = nullptr;
    std::vector<std::string> sndSources;          // combo row -> Capture::setAudio source ("" off, "card", input name)
    std::vector<std::pair<int, int>> picSizes;   // combo row - 1 -> size (row 0 = automatic)
    std::vector<VbiFormat> layList;              // combo row - 1 -> layout set by hand (row 0 = from the driver)
    HWND c_lay = nullptr;
    size_t findsShown = 0;                       // finds counted at the last check
    bool findsOpened = false;                    // the window opened by itself once
    ULONGLONG findsAt = 0;
    Capture cap;
    LiveDecoder dec;
    std::vector<CaptureDeviceInfo> devs;
    std::vector<int> stdIndex;            // combo row -> captureStandards() index
    std::mutex vmu;
    Image video;                          // last picture (RGB)
    bool videoNew = false;
    LiveSnapshot snap;
    int page = 0x100, subShown = kAutoSub;
    bool reveal = false;
    std::string lastRecording;
    ULONGLONG startedAt = 0, recAt = 0;
    std::atomic<bool> videoPosted{false};
    // the picture field by field, 50 a second as on a TV: every frame of the card is split into its two
    // fields (the missing lines interpolated), shown 20 ms apart in the order the card sends them
    std::thread presenter;
    std::condition_variable vcv;
    bool vquit = false;
    uint64_t vseq = 0;
    Image fieldFirst, fieldSecond;
    std::vector<uint8_t> prevLuma;
    double orderScore = 0;   // > 0: the top field comes first
    static constexpr UINT WM_VIDEO = WM_APP + 1;
    static constexpr UINT WM_SAVED = WM_APP + 2;   // lParam: new std::string (what was written), wParam: 1 = ok
    HWND b_save = nullptr, b_lines = nullptr;
    HWND c_hsync = nullptr, c_vsync = nullptr;   // the card's line / frame sync modes (Behold), hidden when it has none
    std::pair<int, int> vbiOwn{0, 0};   // the driver's own VBI lines (first connection without a shift)
    int vbiShift = 0;                    // lines the card's window was moved by (the driver gave them)
    int numShift = 0;                    // lines the screen is moved by (the driver keeps its window): picture and VBI raster
    bool vbiRefused = false;             // the driver did not move its window: only the numbering moves
    // lines 5-6 carry data too on some channels: a start asks the card for 5-22 (18 lines) unless the lines were
    // chosen by hand or the card could not give them (no VBI frame in 3 s: its own lines again, not tried again)
    ULONGLONG widenAt = 0;
    bool widenFailed = false, linesByHand = false;
    std::vector<std::pair<RECT, int>> lineHits;   // line bands of the VBI raster -> line index
    std::string note;                    // shown after the status (saving, VBI window)
    std::atomic<bool> saving{false};
    std::string lastStatus;
    RECT rVideo{}, rLines{}, rPage{}, rList{}, rText{};
    std::vector<std::pair<RECT, int>> listHits;

    // ------------------------------------------------------------ the card
    void fill_devices()
    {
        devs = captureDevices();
        std::vector<std::string> names;
        int sel = 0;
        Json st = load_state();
        std::string want = st.get_str("capture_device");
        for (size_t i = 0; i < devs.size(); i++) {
            names.push_back(devs[i].name + (devs[i].vbi ? "" : "  (no VBI)"));
            if (devs[i].name == want || (want.empty() && devs[i].vbi && !devs[sel].vbi)) sel = (int)i;
        }
        if (names.empty()) names.push_back("No capture card found");
        combo_fill(c_dev, names, sel);
    }
    // a finished .vbi recording instead of the card: played to the live decoder over and over
    // (button "Test file" or a file dropped on the window)
    void test_file(const std::string &path)
    {
        if (path.empty()) return;
        stop();
        // the layout as the recording's decoder finds it (several layouts share a file size: the NHK TBC u16 file
        // read as 8-bit samples was only noise): the saved results, else the decoder's format probe
        auto fmt = std::make_shared<VbiFormat>(VbiFormat::Unknown);
        HWND me = hwnd;
        run_task(hwnd, "Test file: finding its layout", [path, fmt](Progress &pr) {
            DecodeResult r;
            std::string err;
            if (resultsCached(path) && loadResults(path, r, err)) { *fmt = r.geom.format; return; }
            DecodeOptions opt;
            opt.probeOnly = true; opt.cancel = &pr.cancel;
            opt.progress = [&](double, const std::string &s) { if (pr.on_step) pr.on_step(s); };
            if (decodeFile(path, opt, r, err)) *fmt = r.geom.format;
        }, [this, me, path, fmt](bool) {
            if (!IsWindow(me)) return;
            std::string err;
            if (!cap.openFile(path, err, *fmt)) { set_status("Cannot play " + path + ": " + err); return; }
            { std::lock_guard<std::mutex> lk(vmu); video = Image(); }
            start(false);
        });
    }
    // the line / frame sync of the card's video decoder (Behold): VCR / fast modes hold a tape's jumping picture
    void fill_sync()
    {
        int h = cap.syncMode(0), v = cap.syncMode(1);
        combo_fill(c_hsync, {"Line sync: fast track", "Line sync: VCR", "Line sync: poor TV", "Line sync: auto"}, h);
        // frame sync: normal (0) or free running (3) only - free running lets the picture roll through the window
        combo_fill(c_vsync, {"Frame sync: normal", "Frame sync: free running"}, v == 3 ? 1 : v == 0 ? 0 : -1);
        ShowWindow(c_hsync, h >= 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(c_vsync, v >= 0 ? SW_SHOW : SW_HIDE);
    }
    void open_device()
    {
        stop();
        cap.close();
        int i = combo_sel(c_dev);
        combo_fill(c_input, {}, -1);
        if (i < 0 || i >= (int)devs.size()) { set_status("No capture card."); return; }
        std::string err;
        if (!cap.open(i, err)) { set_status("Cannot open " + devs[i].name + ": " + err); return; }
        auto in = cap.inputs();
        Json st = load_state();
        int want = st["capture_input"].is_null() ? -1 : (int)st["capture_input"].integer();
        if (want >= 0 && want < (int)in.size() && devs[i].name == st.get_str("capture_device")) cap.setInput(want);
        combo_fill(c_input, in, cap.input());
        long cur = cap.standard(), avail = cap.standards();
        std::vector<std::string> sn; stdIndex.clear();
        int sel = -1;
        auto &all = captureStandards();
        for (size_t k = 0; k < all.size(); k++) {
            if (avail && !(avail & all[k].bit)) continue;
            if (all[k].bit == cur) sel = (int)sn.size();
            sn.push_back(all[k].name); stdIndex.push_back((int)k);
        }
        combo_fill(c_std, sn, sel);
        fill_sync();
        fill_pictures();
        fill_layouts();
        fill_sound();
        update_tuner();
        if (!devs[i].vbi) set_status(devs[i].name + " gives no raw VBI \xE2\x80\x94 only the picture can be shown.");
        else set_status(devs[i].name + " ready. Press Start.");
    }
    // picture sizes of the card for its standard; the one chosen last for this card is kept
    void fill_pictures()
    {
        picSizes = cap.pictureSizes();
        std::vector<std::string> names = {cap.bt8x8() ? "Picture: auto (384x288, VBI safe)" : "Picture: auto"};
        Json st = load_state();
        std::string want = st.get_str("capture_picture");
        int sel = 0;
        for (size_t k = 0; k < picSizes.size(); k++) {
            auto [w, h] = picSizes[k];
            bool big = cap.bt8x8() && (h > 300 || w > 400);
            names.push_back(fmt("Picture: %dx%d", w, h) + (big ? "  (the card loses VBI fields)" : ""));
            if (fmt("%dx%d", w, h) == want) sel = (int)k + 1;
        }
        combo_fill(c_pic, names, sel);
        if (sel > 0) cap.setPictureSize(picSizes[sel - 1].first, picSizes[sel - 1].second);
        else cap.setPictureSize(0, 0);
    }
    // the layout (card / chip) the VBI is stored and read in: from the driver, or one set by hand (kept)
    void fill_layouts()
    {
        layList.clear();
        std::vector<std::string> names = {"Layout: from the driver"};
        Json st = load_state();
        std::string want = st.get_str("capture_layout");
        int sel = 0;
        for (int i = 1; i < (int)VbiFormat::Count; i++) {
            VbiFormat f = (VbiFormat)i;
            std::string k = formatKey(f);
            if (k.rfind("tbc", 0) == 0) continue;   // not a capture card
            layList.push_back(f);
            names.push_back("Layout: " + k + "  \xE2\x80\x94  " + formatName(f));
            if (k == want) sel = (int)layList.size();
        }
        combo_fill(c_lay, names, sel);
        cap.setLayout(sel > 0 ? layList[sel - 1] : VbiFormat::Unknown);
    }
    // sound: the card's own (Behold) or a sound card input the card is wired to (bt8x8); kept per card.
    // A card with its own sound starts with it, others start silent until an input is chosen
    void fill_sound()
    {
        sndSources = {""};
        std::vector<std::string> names = {"Sound: off"};
        if (cap.cardAudio()) { sndSources.push_back("card"); names.push_back("Sound: from the card"); }
        for (auto &a : audioInputs()) { sndSources.push_back(a); names.push_back("Sound: " + a); }
        Json st = load_state();
        int d = combo_sel(c_dev);
        std::string key = "capture_sound " + (d >= 0 && d < (int)devs.size() ? devs[d].name : std::string());
        std::string want = st[key].is_null() ? (cap.cardAudio() ? "card" : "") : st.get_str(key);
        int sel = 0;
        for (size_t k = 0; k < sndSources.size(); k++) if (sndSources[k] == want) sel = (int)k;
        combo_fill(c_snd, names, sel);
        cap.setAudio(sndSources[sel]);
        cap.setMute(st.get_bool("capture_mute", false));
        set_checked(b_mute, cap.muted());
    }
    void choose_sound()
    {
        int i = combo_sel(c_snd);
        if (i < 0 || i >= (int)sndSources.size()) return;
        bool was = cap.running(), card = sndSources[i] == "card" || cap.audio() == "card";
        cap.setAudio(sndSources[i]);
        if (was && card) dec.reset(cap.geom());   // the graph was built again
        Json st = load_state();
        int d = combo_sel(c_dev);
        if (d >= 0 && d < (int)devs.size()) st["capture_sound " + devs[d].name] = sndSources[i];
        save_state(st);
        if (was && !sndSources[i].empty() && !cap.audioPlaying()) set_status("This sound source does not play (in use by another program?).");
    }
    void toggle_mute()
    {
        cap.setMute(!cap.muted());
        set_checked(b_mute, cap.muted());
        Json st = load_state();
        st["capture_mute"] = cap.muted();
        save_state(st);
    }
    // files and texts found live: the button counts them, the window opens by itself at the first one
    void check_finds()
    {
        if (!cap.running() || GetTickCount64() - findsAt < 2000) return;
        findsAt = GetTickCount64();
        {   // the VPS / WSS button says what came (the window has the rest)
            std::string net = snap.vps.substr(0, snap.vps.find(" (CNI"));
            std::string t = !snap.vps.empty() ? "VPS: " + net + (snap.wss.empty() ? "" : " + WSS") : !snap.wss.empty() ? "WSS \xE2\x9C\x93" : "VPS / WSS";
            if (t != vpsShown) { vpsShown = t; svcText[0] = t; services_button(); }
        }
        {   // NABTS and Japanese teletext (525 lines): the buttons count the pages and open their windows
            auto r = dec.pages525();
            int np = 0, nj = r ? (int)r->jtes.pages.size() : 0, ns = r ? (int)r->db.programs.size() : 0;
            bool ss = r && !r->db.empty();
            int ng = 0;
            if (r) for (auto& gs : r->gemstar) ng += (int)(gs.shows.empty() ? gs.packets.size() : gs.shows.size());
            bool gem = r && !r->gemstar.empty();
            if (r) for (auto& x : r->nabtsRecords) np += x.page != nullptr;
            std::string t = fmt("%d/%d/%d/%d/%d/%d", np, nj, ns, (int)ss, ng, (int)gem);
            if (t != pages525Shown) {
                pages525Shown = t;
                svcText[1] = np ? fmt("NABTS (%d pages)", np) : std::string("NABTS");
                svcText[2] = nj ? fmt("Japanese teletext (%d pages)", nj) : std::string("Japanese teletext");
                svcText[3] = ns ? fmt("StarSight (%d programmes)", ns) : std::string("StarSight");
                svcText[4] = ng ? fmt("Gemstar (%d)", ng) : std::string("Gemstar");
                svcOn[1] = np > 0 || (r && !r->nabtsRecords.empty());
                svcOn[2] = nj > 0;
                svcOn[3] = ss;
                svcOn[4] = gem;
                services_button();
            }
        }
        auto f = dec.finds();
        if (f.size() == findsShown) return;
        findsShown = f.size();
        set_text(b_finds, fmt("\xF0\x9F\x93\x81  Found (%zu)", f.size()));
        if (!f.empty() && !findsOpened) { findsOpened = true; open_finds(); }
    }
    // "Services" says how many have something; the menu lists them all
    void services_button()
    {
        int n = 0;
        for (int i = 1; i < 5; i++) n += svcOn[i];
        set_text(b_services, n ? fmt("Services (%d) \xE2\x96\xBE", n) : std::string("Services \xE2\x96\xBE"));
    }
    void services_menu()
    {
        HMENU m = CreatePopupMenu();
        static const int ids[5] = {ID_VPS, ID_NABTS, ID_JTES, ID_SSLIVE, ID_GEMLIVE};
        for (int i = 0; i < 5; i++) AppendMenuW(m, MF_STRING | (svcOn[i] ? 0 : MF_GRAYED), ids[i], W(svcText[i]).c_str());
        RECT br;
        GetWindowRect(b_services, &br);
        TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom, 0, hwnd, nullptr);
        DestroyMenu(m);
    }
    void open_finds()
    {
        LiveDecoder *d = &dec;
        HWND me = hwnd;
        finds_window_live(hwnd, [d, me] { return IsWindow(me) ? d->finds() : std::vector<Find>(); },
                          "files and texts the capture finds, as they come \xC2\xB7 written to " + dec.filesDir(),
                          [d, me] { return IsWindow(me) ? d->btStatus() : std::string(); });
    }
    void update_tuner()
    {
        bool t = cap.hasTuner() && cap.isTunerInput(cap.input());
        EnableWindow(e_chan, t); EnableWindow(b_tune, t);
        if (t) set_text(e_chan, std::to_string(cap.channel()));
    }
    void save_choice()
    {
        Json st = load_state();
        int i = combo_sel(c_dev);
        if (i >= 0 && i < (int)devs.size()) st["capture_device"] = devs[i].name;
        st["capture_input"] = (double)cap.input();
        save_state(st);
    }
    // record = ask where to save and record at once; false = just watch (Record can still be pressed)
    // lines 5-22 asked at the start: kept when VBI frames come, else the card's own lines again
    void check_widen()
    {
        if (!widenAt || GetTickCount64() - widenAt < 3000) return;
        widenAt = 0;
        if (snap.frames > 0) {
            auto got = cap.vbiLines();
            int f2 = cap.geom().pal ? 313 : 263;
            if (got.first) note = fmt("VBI lines %d-%d / %d-%d (Lines\xE2\x80\xA6 chooses others)", got.first, got.second, got.first + f2, got.second + f2);
            if (got.first && got.first != 5) vbiRefused = true;   // the driver keeps its window: Sync moves the numbering
            return;
        }
        widenFailed = true;
        bool rec = dec.recording();
        if (rec) return;   // a recording goes on as it is
        stop();
        cap.setVbiLines(0, 0);
        start(false);
        note = "the card gives no VBI with lines 5-22: its own lines";
    }
    void start(bool record = true)
    {
        if (cap.running()) return;
        if (!cap.isOpen()) open_device();
        if (!cap.isOpen()) return;
        std::string file = record ? ask_file() : std::string();   // where to record; Cancel: watch only
        cap.onFrame = [this](const uint8_t *f) { dec.push(f, cap.geom().frameBytes()); };
        cap.onVideo = [this](const uint8_t *p, int w, int h, int stride) { picture(p, w, h, stride); };
        std::string err;
        widenAt = 0;
        if (!cap.isFile() && !cap.bt8x8() && !linesByHand && !widenFailed && !vbiShift && !cap.vbiLines().first) {
            cap.setVbiLines(5, 22);   // bt8x8 sends no VBI at all when its lines are changed
            widenAt = GetTickCount64();
        }
        if (!cap.run(err)) {
            if (!widenAt) { set_status("Cannot start: " + err); return; }
            widenAt = 0; widenFailed = true;
            cap.setVbiLines(0, 0);
            if (!cap.run(err)) { set_status("Cannot start: " + err); return; }
        }
        dec.reset(cap.geom());
        dec.setLineOffset(numShift);   // Sync's numbering stays across a restart
        dec.setManual(false);   // a test file is decoded like the card's signal; a line set by hand (right click) still wins
        vbi_note();
        if (cap.isFile()) note = "test file: decoded as live; right click on a line to set its signal by hand";
        findsShown = 0; findsOpened = false;
        startedAt = GetTickCount64();
        subShown = kAutoSub;
        set_text(b_start, "\xE2\x96\xA0  Stop");
        EnableWindow(b_watch, FALSE);
        EnableWindow(b_rec, TRUE);
        save_choice();
        SetTimer(hwnd, 1, 200, nullptr);
        start_presenter();
        if (!file.empty()) begin_record(file);
        else set_status("Not recording \xE2\x80\x94 press Record to write a file.");
    }
    void stop()
    {
        if (dec.recording()) toggle_record();
        cap.stop();
        stop_presenter();
        dec.stopWorker();
        KillTimer(hwnd, 1);
        if (b_start) { set_text(b_start, "\xE2\x96\xB6  Start"); EnableWindow(b_rec, FALSE); EnableWindow(b_watch, TRUE); }
    }
    void toggle_record()
    {
        std::string err;
        if (dec.recording()) {
            dec.record("", err);
            set_text(b_rec, "\xE2\x97\x8F  Record");
            EnableWindow(b_decode, !lastRecording.empty());
            return;
        }
        if (!cap.running()) return;
        std::string f = ask_file();
        if (!f.empty()) begin_record(f);
    }
    // the save dialog, in the folder of the last recording (Videos\TeletextRescue the first time)
    std::string ask_file()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        std::string name = fmt("capture_%04d-%02d-%02d_%02d%02d%02d.vbi", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::string dir = folder();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(widen(dir)), ec);
        std::string f = save_file(hwnd, "Record to (a .t42 with the teletext is written next to it)", name, "vbi", dir);
        if (f.empty()) return "";
        Json st = load_state();
        st["capture_dir"] = std::filesystem::path(widen(f)).parent_path().u8string();
        save_state(st);
        return f;
    }
    void begin_record(const std::string &f)
    {
        std::string err;
        if (!dec.record(f, err)) { set_status("Cannot record: " + err); return; }
        lastRecording = f;
        recAt = GetTickCount64();
        set_text(b_rec, "\xE2\x96\xA0  Stop recording");
        EnableWindow(b_decode, FALSE);
    }
    // where the recordings go: the folder chosen last, else Videos\TeletextRescue
    std::string folder()
    {
        std::string d = load_state().get_str("capture_dir");
        if (!d.empty()) return d;
        PWSTR p = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p))) base = p;
        if (p) CoTaskMemFree(p);
        if (base.empty()) base = L".";
        return (std::filesystem::path(base) / L"TeletextRescue").u8string();
    }
    void decode_recording()
    {
        if (lastRecording.empty() || dec.recording()) return;
        std::map<int, LineForce> lf;
        for (int i = 0; i < 64; i++) if (dec.lineForce(i)) lf[i] = (LineForce)dec.lineForce(i);
        VbiFormat fm = cap.geom().format;
        stop();
        if (!lf.empty() && decode_lines_hook()) decode_lines_hook()(lastRecording, fm, lf);
        else if (open_recording_hook()) open_recording_hook()(lastRecording);
    }
    // right click on a VBI line: its signal set by hand, every kind the recording's decoder knows
    void line_menu(int index, POINT at)
    {
        HMENU m = CreatePopupMenu();
        int cur = dec.lineForce(index);
        bool pal = snap.geom.pal;
        for (int i = 0; i < (int)LineForce::Count; i++) {
            LineForce f = (LineForce)i;
            if (pal ? f == LineForce::SilentRadio : (f == LineForce::Teletext625 || f == LineForce::Norpak || f == LineForce::TviT1 || f == LineForce::TviT2 || f == LineForce::TviT3)) continue;
            std::string name = lineForceName(f);
            if (f == LineForce::Teletext625) name = "Teletext WST 625";
            if (f == LineForce::Auto) name = "Automatic (the program decides)";
            AppendMenuW(m, MF_STRING | (cur == i ? MF_CHECKED : 0), 100 + i, widen(name).c_str());
            if (f == LineForce::None) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
        ClientToScreen(hwnd, &at);
        int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (c < 100) return;
        set_line_force(index, c - 100);
    }
    void set_line_force(int index, int f)
    {
        dec.setLineForce(index, f);
        int no = 0;
        for (auto &ls : snap.lines) if (ls.index == index) no = ls.lineNo;
        note = fmt("line %d: ", no) + (f ? std::string("set by hand to ") + lineForceName((LineForce)f) : std::string("automatic"));
        lastStatus.clear(); update_status(); refresh();
    }
    // everything received so far, at any moment: the session's packets as live.t42 in a folder of its own,
    // and its pages built by the full decoder (subpages, versions) as HTML / PNG / text next to it
    void save_pages()
    {
        if (saving) return;
        std::vector<uint8_t> pk = dec.sessionPackets();
        if (pk.empty()) { note = "nothing to save yet: no teletext received"; return; }
        time_t t = time(nullptr);
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d_%H%M%S", localtime(&t));
        // the folder is chosen each time, starting in the one used last (else the recordings' folder)
        Json st = load_state();
        std::string start = st.get_str("live_save_dir");
        if (start.empty()) start = folder();
        std::error_code ec0;
        std::filesystem::create_directories(std::filesystem::u8path(start), ec0);
        std::string where = pick_folder(hwnd, "Save pages: choose the folder (a folder live_<date> is made in it)", start);
        if (where.empty()) { note = "saving cancelled"; return; }
        st["live_save_dir"] = where;
        save_state(st);
        std::filesystem::path dir = std::filesystem::u8path(where) / std::filesystem::u8path(std::string("live_") + ts);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string t42 = (dir / "live.t42").u8string();
        FILE *f = _wfopen(widen(t42).c_str(), L"wb");
        if (!f) { note = "cannot write " + t42; return; }
        fwrite(pk.data(), 1, pk.size(), f);
        fclose(f);
        saving = true;
        note = fmt("saving %zu packets \xE2\x86\x92 ", pk.size() / 42) + dir.u8string() + " \xE2\x80\xA6";
        EnableWindow(b_save, FALSE);
        HWND h = hwnd;
        std::string out = dir.u8string();
        std::thread([h, t42, out] {
            DecodeOptions opt;
            DecodeResult r;
            std::string err;
            bool ok = false;
            try {
                ok = decodeFile(t42, opt, r, err);
                if (ok) { Progress pr; exportResult(r, out, pr); }
            } catch (std::exception &e) { ok = false; err = e.what(); }
            auto *msg = new std::string(ok ? out : err);
            if (!PostMessageW(h, WM_SAVED, ok, (LPARAM)msg)) delete msg;
        }).detach();
    }
    void saved(bool ok, std::string *msg)
    {
        saving = false;
        EnableWindow(b_save, TRUE);
        std::string m = *msg;
        delete msg;
        if (!ok) { note = "saving failed: " + m; return; }
        std::filesystem::path dir = std::filesystem::u8path(m), html = dir / "teletext" / "output.html";
        std::error_code ec;
        bool page = std::filesystem::exists(html, ec);
        note = "saved \xE2\x86\x92 " + (page ? html : dir).u8string();
        ShellExecuteW(hwnd, L"open", (page ? html : dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    // Lines...: the VBI lines asked of the card, e.g. 5-22 when data sits above line 7 (a shifted tape).
    // 16 lines a field (the card's window moved) or 18 (widened: recorded as the 18-line layout)
    void choose_lines()
    {
        if (!cap.running()) { note = "start the capture first, then choose the lines"; return; }
        auto got = cap.vbiLines();
        std::string v = got.first ? fmt("%d-%d", got.first, got.second) : "5-22";
        if (!input(hwnd, "VBI lines", "Lines of each field to capture, first-last (field 2 is the same + 313).\n"
                   "16 lines (as the card gives, e.g. 6-21) or 18 lines (e.g. 5-22, 6-23).\nEmpty = the card's own.", v)) return;
        int a = 0, b = 0;
        if (strip(v).empty()) a = b = 0;
        else if (sscanf(v.c_str(), "%d-%d", &a, &b) != 2 || a < 2 || b > 30 || (b - a + 1 != 16 && b - a + 1 != 18)) {
            note = "lines not changed: give first-last, 16 or 18 lines (e.g. 5-22)";
            return;
        }
        stop();
        cap.setVbiLines(a, b);
        linesByHand = true;
        vbiShift = numShift = 0; vbiRefused = false;
        start(false);
        auto now = cap.vbiLines();
        if (a && now.first != a) note = fmt("the driver keeps its lines %d-%d (asked %d-%d); the field sync renumbers them when it shows in the window", now.first, now.second, a, b);
        else note = now.first ? fmt("VBI lines %d-%d / %d-%d", now.first, now.second, now.first + 313, now.second + 313) : "the card's own lines";
        if (a) vbiOwn = {a, b};
    }
    std::string sync_text()
    {
        int t = vbiShift + numShift;
        if (!t) return "";
        std::string s = fmt("sync moved %d line%s %s", std::abs(t), std::abs(t) == 1 ? "" : "s", t > 0 ? "down" : "up");
        if (numShift) s += vbiShift ? fmt(" (the card %+d, the screen %+d)", vbiShift, numShift) : std::string(" (on the screen: the driver keeps its window)");
        return s;
    }
    void vbi_note()
    {
        auto got = cap.vbiLines();
        if (!vbiShift) { if (got.first && !numShift) vbiOwn = got; note = sync_text(); return; }
        int want = vbiOwn.first + vbiShift;
        if (got.first != want) {   // the driver kept its lines: the rest of the shift is the numbering's
            vbiRefused = true;
            numShift += vbiShift; vbiShift = 0;
            cap.setVbiLines(0, 0);
        }
        note = sync_text();
    }

    // capture thread: YUY2 -> RGB
    void picture(const uint8_t *p, int w, int h, int stride)
    {
        Image im(w / 2 * 2, h);
        for (int y = 0; y < h; y++) {
            const uint8_t *s = p + (size_t)y * stride;
            uint32_t *d = &im.px[(size_t)y * im.w];
            for (int x = 0; x + 1 < w; x += 2, s += 4) {
                int u = s[1] - 128, v = s[3] - 128;
                int rv = (409 * v) >> 8, gu = (100 * u + 208 * v) >> 8, bu = (516 * u) >> 8;
                for (int k = 0; k < 2; k++) {
                    int Y = ((s[k * 2] - 16) * 298) >> 8;
                    d[x + k] = (uint32_t)clamp8(Y + rv) << 16 | (uint32_t)clamp8(Y - gu) << 8 | clamp8(Y + bu);
                }
            }
        }
        // line 23 / 336 from the picture (luma) for the decoder - WSS sits there, just below the VBI window of cards
        // that stop at line 22 (Behold H8); the row is looked for, the card's sync may have moved it down;
        // 720 samples a row = 13.5 MHz
        if (h >= 2) dec.pictureFrame(p, w, h, stride, 13.5e6 * w / 720);
        if (h <= 300) {   // one field per sample (bt8x8 at 384x288): nothing to split
            std::lock_guard<std::mutex> lk(vmu);
            fieldFirst = im;
            fieldSecond = std::move(im);
            vseq++;
            vcv.notify_one();
            return;
        }
        // which field comes first: the field sent between two others looks like both of them. Bottom of the
        // last frame against top of this one, and top of the last against bottom of this one: the pair that
        // follows each other directly differs less
        int H = h / 2 * 2;
        {
            std::vector<uint8_t> lum((size_t)(H / 2) * (w / 8));
            for (int y = 0; y < H; y += 2)
                for (int x = 0; x < w / 8; x++) lum[(size_t)(y / 2) * (w / 8) + x] = p[(size_t)y * stride + x * 16];
            std::vector<uint8_t> lumB(lum.size());
            for (int y = 1; y < H; y += 2)
                for (int x = 0; x < w / 8; x++) lumB[(size_t)(y / 2) * (w / 8) + x] = p[(size_t)y * stride + x * 16];
            if (prevLuma.size() == lum.size() * 2) {
                const uint8_t *pT = prevLuma.data(), *pB = prevLuma.data() + lum.size();
                double d1 = 0, d2 = 0;
                for (size_t i = 0; i < lum.size(); i++) { d1 += std::abs(pB[i] - lum[i]); d2 += std::abs(pT[i] - lumB[i]); }
                if (d1 + d2 > lum.size() * 2.0) orderScore = 0.97 * orderScore + 0.03 * (d2 - d1) / (d1 + d2);   // only while there is motion
            }
            prevLuma = lum;
            prevLuma.insert(prevLuma.end(), lumB.begin(), lumB.end());
        }
        auto avg = [](uint32_t a, uint32_t b) { return ((a & 0xFEFEFE) >> 1) + ((b & 0xFEFEFE) >> 1); };
        Image top(im.w, H), bot(im.w, H);
        for (int y = 0; y < H; y++) {
            uint32_t *t = &top.px[(size_t)y * im.w], *b = &bot.px[(size_t)y * im.w];
            const uint32_t *s = &im.px[(size_t)y * im.w];
            const uint32_t *up = &im.px[(size_t)std::max(0, y - 1) * im.w], *dn = &im.px[(size_t)std::min(H - 1, y + 1) * im.w];
            if (y % 2 == 0) {   // a line of the top field
                std::copy(s, s + im.w, t);
                if (y == 0) std::copy(dn, dn + im.w, b);
                else for (int x = 0; x < im.w; x++) b[x] = avg(up[x], dn[x]);
            } else {
                std::copy(s, s + im.w, b);
                if (y == H - 1) std::copy(up, up + im.w, t);
                else for (int x = 0; x < im.w; x++) t[x] = avg(up[x], dn[x]);
            }
        }
        std::lock_guard<std::mutex> lk(vmu);
        bool topFirst = orderScore >= 0;
        fieldFirst = std::move(topFirst ? top : bot);
        fieldSecond = std::move(topFirst ? bot : top);
        vseq++;
        vcv.notify_one();
    }
    void start_presenter()
    {
        stop_presenter();
        vquit = false;
        presenter = std::thread([this] {
            timeBeginPeriod(1);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            uint64_t seen = 0;
            std::unique_lock<std::mutex> lk(vmu);
            for (;;) {
                vcv.wait(lk, [&] { return vquit || vseq != seen; });
                if (vquit) break;
                seen = vseq;
                auto t0 = std::chrono::steady_clock::now();
                video = std::move(fieldFirst);
                videoNew = true;
                if (!videoPosted.exchange(true)) PostMessageW(hwnd, WM_VIDEO, 0, 0);
                // the second field half a frame later, unless the next frame is already here
                if (vcv.wait_until(lk, t0 + std::chrono::milliseconds(20), [&] { return vquit; })) break;
                if (seen == vseq && fieldSecond.w > 0) {
                    video = std::move(fieldSecond);
                    if (!videoPosted.exchange(true)) PostMessageW(hwnd, WM_VIDEO, 0, 0);
                }
            }
            timeEndPeriod(1);
        });
    }
    void stop_presenter()
    {
        { std::lock_guard<std::mutex> lk(vmu); vquit = true; }
        vcv.notify_all();
        if (presenter.joinable()) presenter.join();
    }

    void set_status(const std::string &s) { set_text(status, s); }

    // the time the station sends: the clock (and date) at the end of the teletext page header
    std::string on_air()
    {
        if (!snap.hasHeader) return "";
        std::string t;
        for (int k = 8; k < 40; k++) { int c = snap.header[k] & 0x7F; t += c >= 0x20 && c < 0x7F ? (char)c : ' '; }
        auto dg = [&](size_t i) { return i < t.size() && t[i] >= '0' && t[i] <= '9'; };
        std::string tm, dt;
        for (size_t i = t.size(); i-- > 0;)   // the last hh:mm(:ss)
            if (tm.empty() && dg(i) && dg(i + 1) && i + 4 < t.size() + 1 && (t[i + 2] == ':' || t[i + 2] == '.') && dg(i + 3) && dg(i + 4) &&
                (i == 0 || !dg(i - 1)) && !(i >= 2 && (t[i - 1] == ':' || t[i - 1] == '.') && dg(i - 2))) {
                tm = t.substr(i, 5);
                if ((t[i + 5] == ':' || t[i + 5] == '.') && dg(i + 6) && dg(i + 7)) tm = t.substr(i, 8);
                for (size_t j = 0; j + 7 < i; j++)   // a date before it: dd/mm/yy, dd.mm.yy
                    if (dg(j) && dg(j + 1) && (t[j + 2] == '/' || t[j + 2] == '.' || t[j + 2] == '-') && dg(j + 3) && dg(j + 4) && t[j + 5] == t[j + 2] && dg(j + 6) && dg(j + 7)) {
                        dt = t.substr(j, 8);
                        break;
                    }
            }
        if (tm.empty()) return "";
        return dt.empty() ? tm : dt + " " + tm;
    }
    static std::string clock(ULONGLONG ms)
    {
        int s = (int)(ms / 1000);
        return fmt("%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    }
    // the picture with the running time over it (REC while the file is written)
    // The nl TV lines just above the picture, as a TV with its vertical hold moved shows them: the
    // field 1 VBI lines of the last frame (bottom row = the line right above the picture, the first
    // picture line taken as 23 in 625 / 22 in 525 lines), decoded in colour - SECAM FM (the green
    // identification "bottles" of lines 7-15) when the lines carry it, else PAL / NTSC from the burst.
    // Lines the card does not give stay black.
    Image vbi_strip(int nl)
    {
        const VbiGeometry &g = snap.geom;
        int n = g.validSamples, half = g.linesPerFrame / 2;
        if (nl <= 0 || n <= 0 || half <= 0 || snap.frame.empty() || (int)snap.frame.size() < g.frameBytes()) return Image();
        double fs = g.lineSamples * g.lineRate;
        auto samples = [&](int idx) {
            std::vector<float> x(n);
            const uint8_t *p = &snap.frame[(size_t)idx * g.lineBytes];
            for (int i = 0; i < n; i++) x[i] = g.fmt == 1 ? (p[2 * i] | p[2 * i + 1] << 8) / 256.0f : p[i];
            return x;
        };
        std::map<int, int> byLine;   // TV line of field 1 -> stored index
        for (int i = 0; i < half; i++) { int no, f; lineOf(g, i, no, f); if (f == 1) byLine[no] = i; }
        int first = g.pal ? 23 : 22;
        std::vector<int> idx(nl, -1);   // strip row -> stored index
        for (int r = 0; r < nl; r++) { auto it = byLine.find(first - nl + r); if (it != byLine.end()) idx[r] = it->second; }
        std::vector<float> all;
        for (int r = 0; r < nl; r++) if (idx[r] >= 0) { auto x = samples(idx[r]); for (int i = 0; i < n; i += 7) all.push_back(x[i]); }
        if (all.empty()) return Image();
        std::sort(all.begin(), all.end());
        float white = all[all.size() * 995 / 1000];
        // SECAM when the lines carry its FM subcarrier
        std::map<int, SecamLine> sl;
        int secam = 0;
        for (auto &kv : byLine) { sl[kv.second] = secam_line(samples(kv.second), fs); secam += sl[kv.second].type != 0; }
        Image im(n, nl, 0);
        if (secam >= 2) {
            float black = all[all.size() * 5 / 100], scale = std::max(20.0f, white - black);
            for (int r = 0; r < nl; r++) {
                if (idx[r] < 0) continue;
                const SecamLine &a = sl[idx[r]];
                const SecamLine *nb = nullptr;
                for (int d : {-1, 1}) { auto it = sl.find(idx[r] + d); if (it != sl.end() && it->second.type && it->second.type != a.type) { nb = &it->second; break; } }
                secam_rgb(a, nb, black, scale, &im.px[(size_t)r * n]);
            }
        } else {
            int hist[256] = {0};
            for (float v : all) hist[std::clamp((int)v, 0, 255)]++;
            int mode = 0; for (int v = 1; v < 256; v++) if (hist[v] > hist[mode]) mode = v;
            float scale = std::max(20.0f, white - mode);
            double fsc = g.pal ? 4.43361875e6 : 3579545.4545;
            for (int r = 0; r < nl; r++) {
                if (idx[r] < 0) continue;
                QamLine a = qam_line(samples(idx[r]), fs, fsc);
                qam_rgb(a, g.pal, (idx[r] & 1) ? -1 : 1, (float)mode, scale, &im.px[(size_t)r * n]);
            }
        }
        return im;
    }
    void draw_video(HDC dc)
    {
        HFONT f = font(hwnd), fc = font(hwnd, 14, true, L"Consolas");
        fill(dc, rVideo, RGB(0, 0, 0));
        {
            std::lock_guard<std::mutex> lk(vmu);
            if (video.w > 0) {
                int vh = rVideo.bottom - rVideo.top, off = 0;
                HRGN clip = CreateRectRgn(rVideo.left, rVideo.top, rVideo.right, rVideo.bottom);
                SelectClipRgn(dc, clip);
                blit(dc, video, rVideo.left, rVideo.top + off, rVideo.right - rVideo.left, vh, false);
                if (off > 0) {   // above the picture now: the VBI lines the card gives, in colour as a TV shows them
                    int lpx = std::max(1, vh * (video.h > 400 ? 2 : 1) / std::max(1, video.h));
                    int nl = (off + lpx - 1) / lpx;
                    Image st = vbi_strip(nl);
                    if (st.w > 0) blit(dc, st, rVideo.left, rVideo.top + off - nl * lpx, rVideo.right - rVideo.left, nl * lpx, false);
                }
                SelectClipRgn(dc, nullptr);
                DeleteObject(clip);
            }
            else text_out(dc, (rVideo.left + rVideo.right) / 2, (rVideo.top + rVideo.bottom) / 2 - S(hwnd, 8), cap.running() ? "no picture" : "the picture appears after Start", MUTED, f, TA_CENTER);
            videoNew = false;
        }
        if (!cap.running()) return;
        ULONGLONG now = GetTickCount64();
        bool rec = dec.recording();
        std::string t = rec ? "\xE2\x97\x8F REC  " + clock(now - recAt) : clock(now - startedAt);
        if (rec) t += fmt("  %.0f MB", dec.recordedFrames() * (double)snap.geom.frameBytes() / 1e6);
        int x = rVideo.left + S(hwnd, 10), y = rVideo.top + S(hwnd, 8);
        RECT bg{x - S(hwnd, 6), y - S(hwnd, 3), x + S(hwnd, rec ? 250 : 100), y + S(hwnd, 24)};
        fill(dc, bg, RGB(0, 0, 0));
        bool blink = rec && (now / 500) % 2;
        text_out(dc, x, y, t, rec ? (blink ? RGB(255, 70, 70) : RGB(200, 40, 40)) : FG, fc);
        std::string air = on_air();
        if (!air.empty()) {
            air = "on air " + air;
            int ax = rVideo.right - S(hwnd, 10), ay = rVideo.bottom - S(hwnd, 30);
            RECT ab{rVideo.right - S(hwnd, 300), ay - S(hwnd, 3), rVideo.right, ay + S(hwnd, 24)};
            fill(dc, ab, RGB(0, 0, 0));
            text_out(dc, ax, ay, air, RGB(255, 220, 90), fc, TA_RIGHT);
        }
    }
    void paint_video()
    {
        HDC dc = GetDC(hwnd);
        int w = std::max(1L, rVideo.right - rVideo.left), h = std::max(1L, rVideo.bottom - rVideo.top);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, std::max(1L, rVideo.right), std::max(1L, rVideo.bottom));
        HGDIOBJ ob = SelectObject(mem, bm);
        draw_video(mem);
        BitBlt(dc, rVideo.left, rVideo.top, w, h, mem, rVideo.left, rVideo.top, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); ReleaseDC(hwnd, dc);
    }

    // ------------------------------------------------------------ teletext
    std::string charset_for(const LivePage &pg)
    {
        int i = combo_sel(c_cs);
        if (i > 0 && i < (int)kCharsets.size()) return kCharsets[i].first;
        if (!snap.charset.empty()) return snap.charset;
        std::vector<const u8 *> rows;
        for (auto &r : pg.rows) rows.push_back(r.second.data());
        return guess_charset(rows, pg.national);
    }
    // the subpage shown: chosen by hand, else the one received whole last; the transmission still
    // coming in is shown only while there is nothing else (a TV waits for its page too), its header
    // row goes over the page shown (the rolling header with the clock)
    LivePage shownTmp;
    int shownKey = kAutoSub;
    const LivePage *shown_page()
    {
        shownKey = kAutoSub;
        auto it = snap.pages.find(page);
        if (it == snap.pages.end() || it->second.empty()) return nullptr;
        auto &subs = it->second;
        if (subShown != kAutoSub && subs.count(subShown)) { shownKey = subShown; return &subs.at(subShown); }
        const LivePage *best = nullptr;
        for (auto &kv : subs)
            if (kv.first != kLiveReceiving && (!best || kv.second.frame > best->frame)) best = &kv.second, shownKey = kv.first;
        auto rc = subs.find(kLiveReceiving);
        if (!best) { if (rc != subs.end()) shownKey = kLiveReceiving; return rc != subs.end() ? &rc->second : nullptr; }
        if (rc == subs.end() || !rc->second.rows.count(0)) return best;
        shownTmp = *best;
        shownTmp.rows[0] = rc->second.rows.at(0);
        shownTmp.frame = rc->second.frame;
        return &shownTmp;
    }
    void set_page(int p, bool edit = true)
    {
        if (p < 0x100) p = 0x8FF; if (p > 0x8FF) p = 0x100;
        page = p; subShown = kAutoSub;
        if (edit) set_text(e_page, fmt("%03X", page));
        refresh();
    }
    void step_page(int d)
    {
        // to the next page that was received; any page if none
        if (!snap.list.empty()) {
            int best = -1;
            for (auto &kv : snap.list) {
                if (d > 0 && kv.first > page && (best < 0 || kv.first < best)) best = kv.first;
                if (d < 0 && kv.first < page && (best < 0 || kv.first > best)) best = kv.first;
            }
            if (best < 0) best = d > 0 ? snap.list.front().first : snap.list.back().first;
            set_page(best);
        } else set_page(page + d);
    }
    void step_sub(int d)
    {
        auto it = snap.pages.find(page);
        if (it == snap.pages.end()) return;
        std::vector<int> keys;
        for (auto &kv : it->second) if (kv.first != kLiveReceiving) keys.push_back(kv.first);
        if (keys.size() < 2) return;
        shown_page();
        size_t k = std::find(keys.begin(), keys.end(), shownKey) - keys.begin();
        if (k >= keys.size()) k = 0;
        else k = (k + keys.size() + (d > 0 ? 1 : -1)) % keys.size();
        subShown = keys[k];
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void refresh()
    {
        dec.snapshot(snap, page);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // ------------------------------------------------------------ drawing
    void paint(HDC dc, RECT rc)
    {
        HFONT f = font(hwnd), fb = font(hwnd, 10, true), fm = font(hwnd, 9, false, L"Consolas");
        draw_video(dc);
        // VBI lines: every line a band of its own next to its label, sharp (each pixel is the mean of
        // its samples, no stretching between lines), levels stretched to the frame's range
        const VbiGeometry &g = snap.geom;
        int L = g.linesPerFrame, nData = 0;
        for (auto &l : snap.lines) if (l.kind == "WST teletext" || l.kind == "CC 608") nData++;
        std::string info = L ? cap.vbiInfo() : std::string();
        std::string head = L ? fmt("%d VBI lines", L) + (info.empty() ? std::string() : "  \xC2\xB7  " + info) : "VBI lines";
        if (L) head += nData ? fmt("  \xC2\xB7  data read live on %d of them", nData) : std::string("  \xC2\xB7  no data line read yet");
        text_out(dc, rLines.left, rLines.top, head, FG, fb);
        int top = rLines.top + S(hwnd, 22), labW = std::min(S(hwnd, 380), (int)(rLines.right - rLines.left) / 2);
        RECT ras{rLines.left, top, rLines.right - labW, rLines.bottom};
        if (L > 0 && g.lineBytes > 0 && g.validSamples > 0 && !snap.frame.empty() && (int)snap.frame.size() >= g.frameBytes()) {
            int n = g.validSamples, W = std::max(1, (int)(ras.right - ras.left));
            // lines 23 / 336 from the picture are not shown (WSS is still read from them)
            int extra = 0;
            int sh = 0, rowsAll = L + extra;
            int lh = std::max(1, (int)(ras.bottom - ras.top) / rowsAll), gap = lh >= 6 ? 1 : 0;
            int y0 = ras.top + sh * lh;
            int saved = SaveDC(dc);
            IntersectClipRect(dc, ras.left, ras.top, rLines.right, ras.top + rowsAll * lh);
            fill(dc, RECT{ras.left, ras.top, rLines.right, ras.top + rowsAll * lh}, BG);
            std::vector<int> hist(256, 0);
            for (int l = 0; l < L; l++) for (int i = 0; i < n; i += 4) hist[snap.frame[(size_t)l * g.lineBytes + i]]++;
            int tot = 0; for (int h : hist) tot += h;
            int lo = 0, hi = 255, acc = 0;
            for (int v = 0; v < 256; v++) { acc += hist[v]; if (acc <= tot / 100) lo = v; if (acc <= tot - tot / 400) hi = v; }
            if (hi <= lo + 8) hi = lo + 8;
            Image im(W, rowsAll * lh, 0x24262b);
            for (int l = 0; l < L; l++) {
                const uint8_t *src = &snap.frame[(size_t)l * g.lineBytes];
                for (int x = 0; x < W; x++) {
                    int a0 = (int)((int64_t)x * n / W), a1 = std::max(a0 + 1, (int)((int64_t)(x + 1) * n / W));
                    int s = 0; for (int i = a0; i < a1; i++) s += src[i];
                    int v = std::clamp((s / (a1 - a0) - lo) * 255 / (hi - lo), 0, 255);
                    uint32_t c = (uint32_t)v << 16 | v << 8 | v;
                    for (int r = 0; r < lh - gap; r++) im.px[(size_t)(l * lh + r) * W + x] = c;
                }
            }
            for (int k = 0; k < extra; k++) {   // the picture's rows, levels stretched to their own range
                const auto &pr = snap.picRow[k];
                if (pr.empty()) continue;
                int pn = (int)pr.size();
                int plo = *std::min_element(pr.begin(), pr.end()), phi = std::max(plo + 8, (int)*std::max_element(pr.begin(), pr.end()));
                for (int x = 0; x < W; x++) {
                    int a0 = (int)((int64_t)x * pn / W), a1 = std::max(a0 + 1, (int)((int64_t)(x + 1) * pn / W));
                    int s = 0; for (int i = a0; i < a1 && i < pn; i++) s += pr[i];
                    int v = std::clamp((s / (a1 - a0) - plo) * 255 / (phi - plo), 0, 255);
                    uint32_t c = (uint32_t)v << 16 | v << 8 | v;
                    for (int r = 0; r < lh - gap; r++) im.px[(size_t)((L + k) * lh + r) * W + x] = c;
                }
            }
            blit(dc, im, ras.left, y0, W, rowsAll * lh, false);
            HFONT fs = font(hwnd, lh >= S(hwnd, 17) ? 9 : lh >= S(hwnd, 13) ? 8 : 7);
            for (int k = 0; k < extra; k++) {
                int y = y0 + (L + k) * lh;
                bool wss = snap.picWss[k];
                RECT sw{ras.right + S(hwnd, 6), y, ras.right + S(hwnd, 12), y + std::max(2, lh - gap)};
                fill(dc, sw, wss ? service_colour("WSS") : PANEL);
                std::string t = std::string(k ? "336" : "23") + "  from the picture" + (wss ? "  WSS read" : "  (no WSS on it yet)");
                if (lh >= S(hwnd, 10)) text_out(dc, ras.right + S(hwnd, 16), y + lh / 2 - S(hwnd, 7), t, wss ? ACCENT2 : DIM, fs);
            }
            lineHits.clear();
            for (auto &l : snap.lines) {
                int y = y0 + l.index * lh;
                if (y >= ras.top && y + lh <= ras.top + rowsAll * lh) lineHits.push_back({RECT{ras.left, y, rLines.right, y + lh}, l.index});
                // decoded live (teletext, captions) first, then the full decoder's line map, then the clock rate
                bool live = l.kind == "WST teletext" || l.kind == "CC 608" || l.kind == "VPS" || l.kind == "WSS" || l.kind == "VITC";
                std::string k = live ? l.kind : !l.probe.empty() ? l.probe : l.kind.empty() ? "empty" : l.kind == "signal" ? "picture or unknown signal" : l.kind;
                RECT sw{ras.right + S(hwnd, 6), y, ras.right + S(hwnd, 12), y + std::max(2, lh - gap)};
                fill(dc, sw, service_colour(k));
                if (l.mode == kLiveLineOff) k = "off (click: automatic)";
                std::string t = fmt("%d", l.lineNo) + "  " + k;
                if (live) t += fmt("  %d%%", (int)std::lround(l.good * 100));
                if (!l.vitc.empty()) t += "  VITC " + l.vitc;
                if (l.force && l.force != (int)LineForce::None) t += std::string("  [by hand: ") + lineForceKey((LineForce)l.force) + "]";
                else if (l.mode == kLiveLineTeletext) t += "  [teletext by hand]";
                bool dim = k == "empty" || k.rfind("picture", 0) == 0;
                if (lh >= S(hwnd, 10)) text_out(dc, ras.right + S(hwnd, 16), y + lh / 2 - S(hwnd, 7), t, dim ? DIM : live ? ACCENT2 : FG, fs);
            }
            RestoreDC(dc, saved);
        } else fill(dc, ras, PANEL);
        // teletext page
        const LivePage *pg = shown_page();
        Rows rows;
        std::string cs = "latin";
        int national = 0;
        if (pg) { rows = pg->rows; cs = charset_for(*pg); national = pg->national; }
        else if (snap.hasHeader) {   // as a TV while it searches: the rolling header
            Row r = snap.header;
            std::string tag = fmt("P%03X    ", page);
            for (int k = 0; k < 8; k++) r[k] = (u8)tag[k];
            rows[0] = r;
            std::vector<const u8 *> one{rows[0].data()};
            int i = combo_sel(c_cs);
            cs = i > 0 ? kCharsets[i].first : !snap.charset.empty() ? snap.charset : guess_charset(one, 0);
        }
        RenderOpts o; o.reveal = reveal; o.flash_on = (GetTickCount() / 500) % 3 != 2;
        Image ti = render_teletext(rows, charset_table(cs, national), nullptr, nullptr, o);
        int pw = rPage.right - rPage.left, ph = rPage.bottom - rPage.top;
        double sc = std::min((double)pw / ti.w, (double)ph / ti.h);
        int dw = (int)(ti.w * sc), dh = (int)(ti.h * sc);
        fill(dc, rPage, RGB(0, 0, 0));
        blit(dc, ti, rPage.left + (pw - dw) / 2, rPage.top + (ph - dh) / 2, dw, dh, sc < 1);
        std::string pinfo;
        if (pg) {
            size_t nsub = snap.pages.count(page) ? snap.pages[page].size() : 0;
            if (snap.pages.count(page) && snap.pages[page].count(kLiveReceiving) && nsub > 1) nsub--;
            std::string sp = pg->counter ? fmt("  subpage %02d/%02d", pg->counter, pg->of) : pg->sub ? fmt("  subpage %04X", pg->sub) : std::string("  subpage");
            pinfo = fmt("page %03X", page) + (nsub > 1 ? sp + fmt("  (%zu)", nsub) : std::string()) + (shownKey == kLiveReceiving ? "  receiving" : "") +
                    fmt("  \xC2\xB7  %.0f s ago", std::max(0.0, (snap.frames - pg->frame) / (g.pal ? 25.0 : 29.97)));
        } else pinfo = cap.running() ? fmt("waiting for page %03X\xE2\x80\xA6", page) : "";
        text_out(dc, rPage.left, rPage.bottom + S(hwnd, 6), pinfo, MUTED, f);
        // list of pages
        listHits.clear();
        text_out(dc, rList.left, rList.top, fmt("Pages received: %zu", snap.list.size()) + (snap.service.empty() ? "" : "   \xC2\xB7   " + snap.service), FG, fb);
        int x = rList.left, y = rList.top + S(hwnd, 22), cw = S(hwnd, 44), chh = S(hwnd, 18);
        for (auto &kv : snap.list) {
            if (x + cw > rList.right) { x = rList.left; y += chh; }
            if (y + chh > rList.bottom) break;
            RECT r{x, y, x + cw - S(hwnd, 4), y + chh - 2};
            bool cur = kv.first == page;
            if (cur) fill(dc, r, SEL);
            text_out(dc, x + S(hwnd, 3), y + 1, fmt("%03X", kv.first) + (kv.second > 1 ? "+" : ""), cur ? ACCENT2 : FG, fm);
            listHits.push_back({r, kv.first});
            x += cw;
        }
        // captions
        if (!snap.captions.empty()) {
            text_out(dc, rText.left, rText.top, "Captions (CC1)", FG, fb);
            std::string c = snap.captions;
            std::vector<std::string> ls;
            size_t a = 0;
            for (size_t i = 0; i <= c.size(); i++) if (i == c.size() || c[i] == '\n') { ls.push_back(c.substr(a, i - a)); a = i + 1; }
            int maxl = std::max(1, (int)((rText.bottom - rText.top - S(hwnd, 22)) / S(hwnd, 18)));
            int y2 = rText.top + S(hwnd, 22);
            for (size_t i = ls.size() > (size_t)maxl ? ls.size() - maxl : 0; i < ls.size(); i++, y2 += S(hwnd, 18)) text_out(dc, rText.left, y2, ls[i], FG, f);
        }
    }
    void update_status()
    {
        if (!cap.running()) return;
        std::string s = cap.isFile() ? "\xE2\x96\xB6 test file " + cap.vbiInfo() + fmt(", played through %lld time%s", (long long)cap.loops(), cap.loops() == 1 ? "" : "s")
                                     : cap.locked() ? "\xE2\x97\x8F signal" : "\xE2\x97\x8B no signal";
        s += "   \xC2\xB7   " + clock(GetTickCount64() - startedAt);
        if (!on_air().empty()) s += "   \xC2\xB7   on air " + on_air();
        s += fmt("   \xC2\xB7   %lld frames", (long long)snap.frames);
        s += fmt("   \xC2\xB7   teletext %.0f packets/s", snap.packetsPerSecond);
        std::string ch = !snap.service.empty() ? snap.service : snap.channel;
        if (ch.empty() && !snap.vps.empty()) ch = snap.vps.substr(0, snap.vps.find(" \xC2\xB7 ")) + " (VPS)";
        if (ch.empty() && snap.ni < 0 && snap.list.size() >= 20) s += "   \xC2\xB7   channel: not sent in the teletext (no 8/30, no name in the headers)";
        else if (!ch.empty() || snap.ni >= 0) s +="   \xC2\xB7   channel: " + (ch.empty() ? std::string("?") : ch) + (snap.ni >= 0 ? fmt(" (NI %04X)", snap.ni) : std::string());
        {   // fields the card gave against the time: drivers skip VBI fields on unstable tape sync / copy protection
            double want = (GetTickCount64() - startedAt) / (snap.geom.pal ? 20.0 : 1001.0 / 60);
            double got = want > 100 ? cap.fields() / want : 1;
            if (got < 0.97 && !cap.isFile()) s += fmt("   \xC2\xB7   the card gives %d%% of the VBI fields", (int)std::lround(got * 100));
        }
        if (snap.dropped) s += fmt("   \xC2\xB7   %lld frames not decoded live (all recorded)", (long long)snap.dropped);
        if (dec.recording()) {
            int64_t fr = dec.recordedFrames();
            s += "   \xC2\xB7   recording " + clock(GetTickCount64() - recAt) + fmt(", %.0f MB \xE2\x86\x92 ", fr * (double)snap.geom.frameBytes() / 1e6) + dec.recordPath();
            s += fmt(" + .t42 (%zu packets)", snap.t42Packets);
        }
        std::string bt = dec.btStatus(true);
        if (!bt.empty()) s += "   \xC2\xB7   " + bt;
        if (!note.empty()) s += "   \xC2\xB7   " + note;
        if (s != lastStatus) { set_status(s); lastStatus = s; }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override
    {
        switch (m) {
        case WM_CREATE: {
            dark_window(hwnd);
            hd.create(hwnd, "Capture card", "the picture and the VBI decoded live; record to decode everything afterwards");
            // the main actions in the header; pages, finds and the services below the picture, by the VBI lines
            b_decode = hd.add(hwnd, ID_DECODE, "Decode the recording", 170);
            b_rec = hd.add(hwnd, ID_RECORD, "\xE2\x97\x8F  Record", 150);
            b_watch = hd.add(hwnd, ID_WATCH, "\xF0\x9F\x91\x81  Just watch", 130);
            hd.add(hwnd, ID_TESTFILE, "Test file\xE2\x80\xA6", 110);
            b_start = hd.add(hwnd, ID_START, "\xE2\x96\xB6  Start", 110);
            DragAcceptFiles(hwnd, TRUE);
            set_button_style(b_start, BTN_ACCENT);
            EnableWindow(b_rec, FALSE); EnableWindow(b_decode, FALSE);
            c_dev = combo(hwnd, ID_DEVICE);
            c_input = combo(hwnd, ID_INPUT);
            c_std = combo(hwnd, ID_STD);
            c_pic = combo(hwnd, ID_PICTURE);
            c_lay = combo(hwnd, ID_LAYOUT);
            c_snd = combo(hwnd, ID_SOUND);
            b_mute = button(hwnd, ID_MUTE, "\xF0\x9F\x94\x87  Mute", BTN_CHECK);
            l_chan = label(hwnd, "Channel"); set_label_colour(l_chan, MUTED);
            e_chan = edit(hwnd, ID_CHANNEL, "", ES_NUMBER);
            b_tune = button(hwnd, ID_TUNE, "Tune");
            c_cs = combo(hwnd, ID_CHARSET);
            std::vector<std::string> cn; for (auto &c : kCharsets) cn.push_back(c.second);
            combo_fill(c_cs, cn, 0);
            e_page = edit(hwnd, ID_PAGE, "100", ES_UPPERCASE);
            SendMessageW(e_page, EM_LIMITTEXT, 3, 0);
            SendMessageW(e_page, WM_SETFONT, (WPARAM)font(hwnd, 12, true, L"Consolas"), TRUE);
            b_prev = button(hwnd, ID_PREV, "\xE2\x97\x80");
            b_next = button(hwnd, ID_NEXT, "\xE2\x96\xB6");
            b_sp = button(hwnd, ID_SUBPREV, "sub \xE2\x97\x80");
            b_sn = button(hwnd, ID_SUBNEXT, "sub \xE2\x96\xB6");
            b_reveal = button(hwnd, ID_REVEAL, "Reveal", BTN_CHECK);
            status = label(hwnd, ""); set_label_colour(status, MUTED);
            b_lines = button(hwnd, ID_LINES, "Lines\xE2\x80\xA6");
            b_save = button(hwnd, ID_SAVE, "\xF0\x9F\x92\xBE  Save pages");
            b_finds = button(hwnd, ID_FINDS, "\xF0\x9F\x93\x81  Found (0)");
            b_services = button(hwnd, ID_SERVICES, "Services \xE2\x96\xBE");
            c_hsync = combo(hwnd, ID_HSYNC); c_vsync = combo(hwnd, ID_VSYNC);
            ShowWindow(c_hsync, SW_HIDE); ShowWindow(c_vsync, SW_HIDE);
            fill_devices();
            open_device();
            return 0;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int p = S(hwnd, 14), y = hd.layout(hwnd), h = S(hwnd, 28);
            int x = p;
            MoveWindow(c_dev, x, y, S(hwnd, 300), S(hwnd, 300), TRUE); x += S(hwnd, 308);
            MoveWindow(c_input, x, y, S(hwnd, 130), S(hwnd, 300), TRUE); x += S(hwnd, 138);
            MoveWindow(c_std, x, y, S(hwnd, 120), S(hwnd, 300), TRUE); x += S(hwnd, 128);
            MoveWindow(c_pic, x, y, S(hwnd, 200), S(hwnd, 300), TRUE); x += S(hwnd, 208);
            MoveWindow(c_lay, x, y, S(hwnd, 230), S(hwnd, 360), TRUE); x += S(hwnd, 238);
            MoveWindow(l_chan, x, y + S(hwnd, 5), S(hwnd, 56), S(hwnd, 20), TRUE); x += S(hwnd, 58);
            MoveWindow(e_chan, x, y + 1, S(hwnd, 50), h - 2, TRUE); x += S(hwnd, 56);
            MoveWindow(b_tune, x, y, S(hwnd, 56), h, TRUE); x += S(hwnd, 64);
            MoveWindow(c_snd, x, y, S(hwnd, 260), S(hwnd, 300), TRUE); x += S(hwnd, 268);
            MoveWindow(b_mute, x, y, S(hwnd, 90), h, TRUE);
            y += h + S(hwnd, 8);
            MoveWindow(status, p, y, rc.right - 2 * p, S(hwnd, 20), TRUE);
            y += S(hwnd, 28);
            int lw = (rc.right - 3 * p) * 52 / 100, bottom = rc.bottom - p;
            int vh = std::min((bottom - y) * 45 / 100, lw * 3 / 4), vw = vh * 4 / 3;
            rVideo = {p, y, p + vw, y + vh};
            int ry = y + vh + S(hwnd, 10), gap = S(hwnd, 8), ch = h;
            {   // one row under the picture: the VBI lines and the card's sync on the left, pages / finds / services on
                // the right; buttons as tall as the drop-down lists, the same gap everywhere
                RECT cr{};
                MoveWindow(c_hsync, 0, ry, S(hwnd, 10), S(hwnd, 300), TRUE);
                GetWindowRect(c_hsync, &cr);
                ch = std::max<int>(cr.bottom - cr.top, h);   // the closed list's height
                int bx2 = p + lw;
                bx2 -= S(hwnd, 140); MoveWindow(b_services, bx2, ry, S(hwnd, 140), ch, TRUE);
                bx2 -= S(hwnd, 128) + gap; MoveWindow(b_finds, bx2, ry, S(hwnd, 128), ch, TRUE);
                bx2 -= S(hwnd, 136) + gap; MoveWindow(b_save, bx2, ry, S(hwnd, 136), ch, TRUE);
                MoveWindow(b_lines, p, ry, S(hwnd, 90), ch, TRUE);
                int sx = p + S(hwnd, 90) + gap, room = std::clamp((bx2 - S(hwnd, 24) - sx - gap) / 2, S(hwnd, 120), S(hwnd, 200));
                MoveWindow(c_hsync, sx, ry, room, S(hwnd, 300), TRUE);
                MoveWindow(c_vsync, sx + room + gap, ry, room, S(hwnd, 300), TRUE);
            }
            int rx = p + lw + p, rw = rc.right - p - rx;
            // page controls
            int bx = rx;
            MoveWindow(b_prev, bx, y, S(hwnd, 34), h, TRUE); bx += S(hwnd, 38);
            MoveWindow(e_page, bx, y, S(hwnd, 56), h, TRUE); bx += S(hwnd, 60);
            MoveWindow(b_next, bx, y, S(hwnd, 34), h, TRUE); bx += S(hwnd, 44);
            MoveWindow(b_sp, bx, y, S(hwnd, 64), h, TRUE); bx += S(hwnd, 68);
            MoveWindow(b_sn, bx, y, S(hwnd, 64), h, TRUE); bx += S(hwnd, 74);
            MoveWindow(b_reveal, bx, y, S(hwnd, 84), h, TRUE); bx += S(hwnd, 92);
            MoveWindow(c_cs, bx, y, std::max(S(hwnd, 120), rx + rw - bx), S(hwnd, 300), TRUE);
            int py = y + h + S(hwnd, 8);
            int ph = std::min((bottom - py) * 62 / 100, rw * 500 / 480);
            rLines = {p, ry + ch + S(hwnd, 6), p + lw, bottom};
            rPage = {rx, py, rx + std::min(rw, ph * 480 / 500), py + ph};
            rList = {rx, rPage.bottom + S(hwnd, 30), rc.right - p, bottom};
            rText = {p + vw + S(hwnd, 12), y, p + lw, y + vh};
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_VIDEO: videoPosted = false; paint_video(); return 0;
        case WM_DROPFILES: {
            HDROP dr = (HDROP)w;
            wchar_t p[MAX_PATH * 2] = {};
            if (DragQueryFileW(dr, 0, p, MAX_PATH * 2)) test_file(narrow(p));
            DragFinish(dr);
            return 0;
        }
        case WM_SAVED: saved(w != 0, (std::string *)l); lastStatus.clear(); if (cap.running()) update_status(); else set_status(note); return 0;
        case WM_PAINT: paint_buffered(hwnd, [this](HDC dc, RECT rc) { paint(dc, rc); }); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return ctl_colour(m, w, l);
        case WM_DRAWITEM: if (draw_item(l)) return TRUE; break;
        case WM_TIMER:
            // pages, lines and the status 5 times a second; the picture comes with WM_VIDEO
            if (w == 1) { refresh(); update_status(); check_finds(); check_widen(); }
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (auto &h : lineHits) if (PtInRect(&h.first, pt)) { line_menu(h.second, pt); break; }
            return 0;
        }
        case WM_MOUSEWHEEL: step_page(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (auto &h : listHits) if (PtInRect(&h.first, pt)) { set_page(h.second); break; }
            // a VBI line: automatic -> read as teletext -> off -> automatic
            for (auto &h : lineHits)
                if (PtInRect(&h.first, pt)) {
                    int m = (dec.lineMode(h.second) + 1) % 3;
                    set_line_force(h.second, m == kLiveLineTeletext ? (int)LineForce::Teletext625 : m == kLiveLineOff ? (int)LineForce::None : 0);
                    break;
                }
            SetFocus(hwnd);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_PRIOR || w == VK_UP) step_page(-1);
            else if (w == VK_NEXT || w == VK_DOWN) step_page(1);
            else if (w == VK_LEFT) step_sub(-1);
            else if (w == VK_RIGHT) step_sub(1);
            return 0;
        case WM_CHAR:   // digits typed anywhere: the page number, as on a TV remote
            if (w >= '0' && w <= '9') { SetFocus(e_page); set_text(e_page, std::string(1, (char)w)); SendMessageW(e_page, EM_SETSEL, 1, 1); }
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            switch (id) {
            case ID_DEVICE: if (code == CBN_SELCHANGE) open_device(); break;
            case ID_INPUT:
                if (code == CBN_SELCHANGE) { cap.setInput(combo_sel(c_input)); update_tuner(); save_choice(); if (cap.running()) dec.reset(cap.geom()); }
                break;
            case ID_STD:
                if (code == CBN_SELCHANGE) {
                    int i = combo_sel(c_std);
                    if (i >= 0 && i < (int)stdIndex.size()) {
                        bool was = cap.running();
                        if (was) stop();
                        cap.setStandard(captureStandards()[stdIndex[i]].bit);
                        fill_pictures();
                        if (was) start();
                    }
                }
                break;
            case ID_TUNE: {
                long ch = atol(get_text(e_chan).c_str());
                if (!cap.setChannel(ch)) set_status("The tuner did not take channel " + std::to_string(ch) + ".");
                else if (cap.running()) dec.reset(cap.geom());
                break;
            }
            case ID_CHARSET: if (code == CBN_SELCHANGE) InvalidateRect(hwnd, nullptr, FALSE); break;
            case ID_HSYNC: case ID_VSYNC:
                if (code == CBN_SELCHANGE) {
                    int which = id == ID_VSYNC ? 1 : 0, m = combo_sel(which ? c_vsync : c_hsync);
                    if (which) m = m == 1 ? 3 : 0;
                    note = cap.setSyncMode(which, m) ? std::string(which ? "frame" : "line") + " sync of the card changed"
                                                     : "the card did not take the sync mode";
                    fill_sync();   // what the card holds now
                    lastStatus.clear();
                    if (cap.running()) update_status(); else set_status(note);
                }
                break;
            case ID_PICTURE:
                if (code == CBN_SELCHANGE) {
                    int i = combo_sel(c_pic);
                    bool was = cap.running();
                    if (was) stop();
                    if (i > 0 && i <= (int)picSizes.size()) cap.setPictureSize(picSizes[i - 1].first, picSizes[i - 1].second);
                    else cap.setPictureSize(0, 0);
                    Json st = load_state();
                    st["capture_picture"] = i > 0 && i <= (int)picSizes.size() ? fmt("%dx%d", picSizes[i - 1].first, picSizes[i - 1].second) : std::string();
                    save_state(st);
                    if (was) start(false);
                }
                break;
            case ID_LAYOUT:
                if (code == CBN_SELCHANGE) {
                    int i = combo_sel(c_lay);
                    VbiFormat f = i > 0 && i <= (int)layList.size() ? layList[i - 1] : VbiFormat::Unknown;
                    bool was = cap.running();
                    if (was) stop();
                    cap.setLayout(f);
                    Json st = load_state();
                    st["capture_layout"] = f == VbiFormat::Unknown ? std::string() : std::string(formatKey(f));
                    save_state(st);
                    if (was) start(false);
                }
                break;
            case ID_FINDS: open_finds(); break;
            case ID_NABTS: if (auto r = dec.pages525()) nabts_window(hwnd, r); break;
            case ID_JTES: if (auto r = dec.pages525()) jtes_window(hwnd, r); break;
            case ID_SSLIVE: if (auto r = dec.pages525()) starsight_window(hwnd, r); break;
            case ID_GEMLIVE: if (auto r = dec.pages525()) gemstar_window(hwnd, r); break;
            case ID_SERVICES: services_menu(); break;
            case ID_VPS: { LiveDecoder *d = &dec; HWND me = hwnd; vps_window_live(hwnd, [d, me] { return IsWindow(me) ? d->vpsInfo() : LiveVps(); }); break; }
            case ID_SAVE: save_pages(); lastStatus.clear(); if (cap.running()) update_status(); else set_status(note); break;
            case ID_LINES:
                choose_lines();
                lastStatus.clear();
                if (cap.running()) update_status(); else set_status(note);
                SetFocus(hwnd);
                break;
            case ID_SOUND: if (code == CBN_SELCHANGE) choose_sound(); break;
            case ID_MUTE: toggle_mute(); break;
            case ID_START: if (cap.running()) stop(); else start(); break;
            case ID_WATCH: if (!cap.running()) start(false); break;
            case ID_TESTFILE: test_file(open_file(hwnd, "VBI recording to play live", {{"VBI recording", "*.vbi;*.tbc"}, {"All files", "*.*"}})); break;
            case ID_RECORD: toggle_record(); break;
            case ID_DECODE: decode_recording(); break;
            case ID_PREV: step_page(-1); break;
            case ID_NEXT: step_page(1); break;
            case ID_SUBPREV: step_sub(-1); break;
            case ID_SUBNEXT: step_sub(1); break;
            case ID_REVEAL: reveal = !reveal; set_checked(b_reveal, reveal); InvalidateRect(hwnd, nullptr, FALSE); break;
            case ID_PAGE:
                if (code == EN_CHANGE) {
                    std::string t = get_text(e_page);
                    if (t.size() == 3) {
                        int v = (int)strtol(t.c_str(), nullptr, 16);
                        if (v >= 0x100 && v <= 0x8FF && v != page) { set_page(v, false); SetFocus(hwnd); }
                    }
                }
                break;
            }
            return 0;
        }
        case WM_CLOSE: stop(); cap.close(); DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, 1); stop(); cap.close(); stop_presenter(); break;
        case WM_NCDESTROY: { LRESULT r = Window::proc(m, w, l); delete this; return r; }
        }
        return Window::proc(m, w, l);
    }
};
}

void capture_window(HWND owner, bool start, const std::string& testFile)
{
    auto *w = new CaptureWin;
    w->create(L"TRCapture", "Capture card \xE2\x80\x94 live", WS_OVERLAPPEDWINDOW, S(owner, 1400), S(owner, 900), owner);
    if (!w->hwnd) return;
    fit_large(w->hwnd);
    ShowWindow(w->hwnd, SW_SHOW);
    if (!testFile.empty()) w->test_file(testFile);
    else if (start) w->start();
}
}
