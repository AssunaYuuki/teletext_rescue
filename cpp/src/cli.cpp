// trcli: command-line front end, same engine as the GUI.
//   trcli <capture.vbi> [--gpu] [--threads N] [--frames N] [--unverified]
//         [--import old.json] [--json out.json] [--text out.txt] [--export folder]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "decoder.h"
#include "ui/util.h"
#include "ssjson.h"
#include "export.h"
#include "finds.h"
#include "ttpages.h"
#include "vbirepair.h"
#include "rescache.h"
#include "version.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include "capture.h"
#include "live.h"

// trcli --replay <rec.vbi> [--format key]: a recording through the live decoder -> <rec>.t42 and
// <rec>.raw.t42 (every packet with a good address, binary rows included)
static int replayCli(const std::vector<std::string>& args)
{
    if (args.size() < 2) { fprintf(stderr, "trcli --replay <rec.vbi> [--format saa713x-pal] [--start F] [--frames N]\n"); return 1; }
    std::string in = args[1], key = "saa713x-pal";
    // the streams written next to the recording replace the ones there (a live capture's own .t42 / .raw.t42):
    // those are kept as .bak first (once - an older .bak is the original)
    for (const char* ext : {".t42", ".raw.t42"}) {
        std::string p = in.substr(0, in.find_last_of('.')) + ext;
        std::error_code ec;
        if (exists(p) && std::filesystem::file_size(std::filesystem::u8path(p), ec) > 0 && !exists(p + ".bak")) {
            std::filesystem::rename(std::filesystem::u8path(p), std::filesystem::u8path(p + ".bak"), ec);
            if (!ec) printf("kept %s as .bak\n", p.c_str());
        }
    }
    int64_t start = 0, limit = -1;
    bool manual = false;
    std::vector<std::pair<int, LineForce>> forces;   // --line <index in the frame>=<key>, as a click in the capture window
    for (size_t i = 2; i < args.size(); i += 2) {
        if (args[i] == "--manual") { manual = true; i--; continue; }
        if (i + 1 >= args.size()) break;
        if (args[i] == "--line") { auto e = args[i + 1].find('='); if (e != std::string::npos) forces.push_back({atoi(args[i + 1].c_str()), lineForceFromKey(args[i + 1].substr(e + 1))}); }
        else if (args[i] == "--format") key = args[i + 1];
        else if (args[i] == "--start") start = atoll(args[i + 1].c_str());
        else if (args[i] == "--frames") limit = atoll(args[i + 1].c_str());
    }
    VbiGeometry g = formatGeometry(formatFromKey(key));
    if (g.format == VbiFormat::Unknown) { fprintf(stderr, "unknown format %s\n", key.c_str()); return 1; }
    int64_t fb = g.fileFrameBytes();
    FILE* fi = _wfopen(widen(in).c_str(), L"rb");
    if (!fi || fb <= 0) { fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    _fseeki64(fi, 0, SEEK_END);
    int64_t n = _ftelli64(fi) / fb;
    _fseeki64(fi, start * fb, SEEK_SET);
    n = std::max<int64_t>(0, n - start);
    if (limit >= 0) n = std::min(n, limit);
    LiveDecoder dec;
    dec.reset(g);
    dec.setManual(manual);
    for (auto& f : forces) dec.setLineForce(f.first, (int)f.second);
    std::string err;
    if (!dec.record(in, err, false)) { fprintf(stderr, "%s\n", err.c_str()); fclose(fi); return 1; }
    std::vector<uint8_t> frame((size_t)fb);
    for (int64_t f = 0; f < n; f++) {
        if (fread(frame.data(), 1, (size_t)fb, fi) != (size_t)fb) { n = f; break; }
        dec.feed(frame.data());
        if (f % 125 == 124) dec.joinProbe();   // every 5 s chunk goes through the probe (live it runs beside)
        if (f % 500 == 0) { fprintf(stderr, "\r%lld / %lld frames", (long long)f, (long long)n); }
    }
    fclose(fi);
    dec.record("", err);
    dec.joinProbe();
    LiveSnapshot sn;
    dec.snapshot(sn, 0);
    if (auto r = dec.pages525())
        printf("live 525 lines: Japanese %zu pages, NABTS %zu records, StarSight %zu programmes / %zu show lists / %zu channels\n",
               r->jtes.pages.size(), r->nabtsRecords.size(), r->db.programs.size(), r->db.showLists.size(), r->db.channels.size());
    for (auto& l : sn.lines)
        if (!l.vitc.empty()) printf("line %d: VITC %s\n", l.lineNo, l.vitc.c_str());
    // the line map as the capture window shows it: live name, then the probe's
    for (auto& l : sn.lines)
        if (!l.kind.empty() || !l.probe.empty())
            printf("       line %3d f%d  %-22s %-38s %3d%%  %d packets\n", l.lineNo, l.field, l.kind.c_str(), l.probe.c_str(), (int)std::lround(l.good * 100), l.packets);
    if (!sn.vps.empty()) printf("VPS: %s\n", sn.vps.c_str());
    if (!sn.wss.empty()) printf("WSS: %s\n", sn.wss.c_str());
    {
        LiveVps vi = dec.vpsInfo();
        for (auto& e : vi.history) printf("  VPS from frame %lld: %s\n", (long long)e.frame, e.text.c_str());
        for (auto& e : vi.wssHistory) printf("  WSS from frame %lld: %s\n", (long long)e.frame, e.text.c_str());
    }
    dec.finds();       // starts writing the «Бизнес-Текст» files (in a thread, as live)
    dec.waitFiles();
    printf("%s\n", dec.btStatus().c_str());
    printf("files: %s\n", dec.filesDir().c_str());
    for (auto& f : dec.finds())
        printf("  %-9s %s%s  [%s]  %s  %s, %s  %s\n", f.kind == Find::File ? "FILE" : f.kind == Find::Announced ? "ANNOUNCED" : f.kind == Find::Text ? "TEXT" : "TEST",
               f.name.c_str(), f.shortName.empty() ? "" : (" (" + f.shortName + ")").c_str(), f.type.c_str(), f.status.c_str(), f.service.c_str(), f.where.c_str(), f.when.c_str());
    printf("\n%lld frames, %zu packets, %zu pages\n", (long long)n, sn.packets, sn.list.size());
    return 0;
}

static int captureCli(const std::vector<std::string>& args)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto devs = captureDevices();
    if (args[0] == "--capture-list") {
        for (size_t i = 0; i < devs.size(); i++) printf("%zu  %s%s\n", i, devs[i].name.c_str(), devs[i].vbi ? "  (raw VBI)" : "");
        return 0;
    }
    std::string out = args.size() > 1 ? args[1] : "";
    int seconds = 60, device = -1, input = -1, videoTop = 0;
    long channel = -1;
    std::string standard;
    int picW = 0, picH = 0, hsync = -1, vsync = -1;
    for (size_t i = 2; i + 1 < args.size(); i += 2) {
        if (args[i] == "--hsync") hsync = atoi(args[i + 1].c_str());   // Behold line / frame sync mode (Capture::syncMode)
        if (args[i] == "--vsync") vsync = atoi(args[i + 1].c_str());
        if (args[i] == "--seconds") seconds = atoi(args[i + 1].c_str());
        else if (args[i] == "--device") device = atoi(args[i + 1].c_str());
        else if (args[i] == "--input") input = atoi(args[i + 1].c_str());
        else if (args[i] == "--channel") channel = atol(args[i + 1].c_str());
        else if (args[i] == "--video-top") videoTop = atoi(args[i + 1].c_str());
        else if (args[i] == "--standard") standard = args[i + 1];
        else if (args[i] == "--picture") sscanf(args[i + 1].c_str(), "%dx%d", &picW, &picH);
    }
    if (out.empty()) { fprintf(stderr, "trcli --capture <out.vbi> [--seconds N] [--device I] [--input I] [--channel N] [--standard S] [--picture WxH]\n"); return 1; }
    if (device < 0)
        for (size_t i = 0; i < devs.size(); i++)
            if (devs[i].vbi) { device = (int)i; break; }
    if (device < 0 || device >= (int)devs.size()) { fprintf(stderr, "no capture card with a raw VBI pin\n"); return 1; }
    Capture cap;
    LiveDecoder dec;
    std::string err;
    if (!cap.open(device, err)) { fprintf(stderr, "cannot open %s: %s\n", devs[device].name.c_str(), err.c_str()); return 1; }
    if (input >= 0) cap.setInput(input);
    if (picW > 0 && picH > 0) cap.setPictureSize(picW, picH);   // --picture WxH (bt8x8: larger than 384x288 loses VBI fields)
    if (channel >= 0 && cap.hasTuner()) cap.setChannel(channel);
    // --hsync / --vsync N: the card's sync modes for this capture (put back afterwards)
    int oldH = cap.syncMode(0), oldV = cap.syncMode(1);
    if (hsync >= 0) printf("line sync %d%s\n", hsync, cap.setSyncMode(0, hsync) ? "" : " (refused by the card)");
    if (vsync >= 0) printf("frame sync %d%s\n", vsync, cap.setSyncMode(1, vsync) ? "" : " (refused by the card)");
    // --standard "PAL D" / SECAM / NTSC ...: the first standard of the card whose name starts with it
    if (!standard.empty()) {
        bool found = false;
        for (auto& st : captureStandards())
            if (_strnicmp(st.name, standard.c_str(), standard.size()) == 0 && (cap.standards() & st.bit)) {
                printf("standard %s%s\n", st.name, cap.setStandard(st.bit) ? "" : " (refused by the card)");
                found = true;
                break;
            }
        if (!found) fprintf(stderr, "the card has no standard %s\n", standard.c_str());
    }
    auto in = cap.inputs();
    printf("%s, input %s%s\n", devs[device].name.c_str(), cap.input() >= 0 && cap.input() < (int)in.size() ? in[cap.input()].c_str() : "?",
           cap.hasTuner() ? (", channel " + std::to_string(cap.channel())).c_str() : "");
    cap.onFrame = [&](const uint8_t* f) { dec.push(f, cap.geom().frameBytes()); };
    // --video-top N: the luma of the top N rows of every picture frame -> <out>.top (u16 w, u16 N, then rows)
    FILE* top = nullptr;
    if (videoTop > 0) top = _wfopen(widen(out + ".top").c_str(), L"wb");
    std::mutex topMu;
    if (top)
        cap.onVideo = [&](const uint8_t* p, int w, int h, int stride) {
            std::lock_guard<std::mutex> lk(topMu);
            uint16_t hd[2] = {(uint16_t)w, (uint16_t)std::min(videoTop, h)};
            fwrite(hd, 2, 2, top);
            std::vector<uint8_t> y(w);
            for (int r = 0; r < hd[1]; r++) {
                for (int x = 0; x < w; x++) y[x] = p[(size_t)r * stride + 2 * x];
                fwrite(y.data(), 1, w, top);
            }
        };
    if (!cap.run(err)) { fprintf(stderr, "cannot start: %s\n", err.c_str()); return 1; }
    dec.reset(cap.geom());
    printf("VBI: %s\n", cap.vbiInfo().c_str());
    if (!dec.record(out, err)) { fprintf(stderr, "cannot record: %s\n", err.c_str()); cap.stop(); return 1; }
    for (int s = 1; s <= seconds; s++) {
        Sleep(1000);
        if (s % 5 && s != seconds) continue;
        LiveSnapshot sn;
        dec.snapshot(sn, 0);
        printf("%3d s  %lld frames, %zu packets, %zu pages%s%s\n", s, (long long)dec.recordedFrames(), sn.packets, sn.list.size(),
               sn.channel.empty() ? "" : (", " + sn.channel).c_str(), "");
        if (s == seconds)   // what each line carries, at the end
            for (auto& l : sn.lines)
            if (!l.kind.empty() || !l.probe.empty()) printf("       line %3d f%d  %-22s %s\n", l.lineNo, l.field, l.kind.c_str(), l.probe.c_str());
        fflush(stdout);
    }
    dec.record("", err);
    cap.stop();
    if (hsync >= 0 && oldH >= 0) cap.setSyncMode(0, oldH);
    if (vsync >= 0 && oldV >= 0) cap.setSyncMode(1, oldV);
    if (top) { std::lock_guard<std::mutex> lk(topMu); fclose(top); top = nullptr; }
    dec.stopWorker();
    dec.joinProbe();
    printf("written %s (+ .t42)\n", out.c_str());
    return 0;
}

int wmain(int argc, wchar_t** wargv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(narrow(wargv[i]));
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
#endif
    if (args.empty()) {
        printf("Teletext Rescue " TR_VERSION " - trcli\n"
               "usage: trcli <capture.vbi> [--gpu] [--format tbc|bttv|cx88|saa7134|cx23885|em28xx|ivtv]\n"
               "             [--threads N] [--frames N] [--unverified] [--again] [--reunpack] [--clean] [--squash v1|v3|auto|aggressive|...]\n"
               "             [--repair noise=30,dropout=50,align=40,...]\n"
               "  .flac recordings are unpacked once to x_vbi/x.vbi next to them; --reunpack does it again\n"
               "  a recording decoded before opens from <recording>_vbi/results.trc; --again decodes it again\n"
               "             [--import old.json] [--json out.json] [--text out.txt]\n");
        return 1;
    }
#ifdef _WIN32
    // trcli --capture <out.vbi> [--seconds N] [--device I] [--input I] [--channel N] [--standard S]: record from a capture
    // card without the window (the live decoder writes <out>.vbi and <out>.t42); --capture-list shows cards
    if (!args.empty() && (args[0] == "--capture" || args[0] == "--capture-list")) return captureCli(args);
    if (!args.empty() && args[0] == "--replay") return replayCli(args);
#endif
    // trcli --repair-measure <rec.vbi> <spec> [first frame] [frames] [--format key]: what a VBI repair does to the
    // teletext, per line (as the repair window's Measure)
    if (!args.empty() && args[0] == "--repair-measure") {
        if (args.size() < 3) { fprintf(stderr, "trcli --repair-measure <rec.vbi> <noise=30,...> [first] [frames] [--format key]\n"); return 1; }
        VbiRepair rp;
        std::string err;
        if (!vbiRepairParse(args[2], rp, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        int64_t from = args.size() > 3 && args[3][0] != '-' ? atoll(args[3].c_str()) : 0;
        int64_t count = args.size() > 4 && args[4][0] != '-' ? atoll(args[4].c_str()) : 100;
        VbiFormat f = VbiFormat::Unknown;
        for (size_t i = 3; i + 1 < args.size(); i++) if (args[i] == "--format") f = formatFromKey(args[i + 1]);
        VbiRepairMeasure ms;
        if (!liveRepairMeasure(args[1], f, rp, from, count, ms, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        for (auto& s : ms.lines)
            if (s.packets[0] || s.packets[1]) printf("line %2d  packets %4d -> %4d\n", s.index, s.packets[0], s.packets[1]);
        printf("all lines: packets %ld -> %ld, parity errors %.2f%% -> %.2f%% (%s)\n", ms.packets[0], ms.packets[1],
               ms.bytes[0] ? 100.0 * ms.bad[0] / ms.bytes[0] : 0.0, ms.bytes[1] ? 100.0 * ms.bad[1] / ms.bytes[1] : 0.0, rp.text().c_str());
        return 0;
    }
    DecodeOptions opt;
    std::string in, jsonOut, textOut, importJson, exportDir;
    bool cleanPages = false;
    std::string squashMode;   // --squash v1|v3|auto|<profile>: Clean pages by the VHSTTX squash
    std::string repairSpec;   // --repair noise=30,dropout=50: VBI repair (VHSTTX) before decoding
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--gpu") opt.gpu = true;
        else if (a == "--cpu") opt.gpu = false;
        else if (a == "--threads") opt.threads = atoi(next().c_str());
        else if (a == "--frames") opt.maxFrames = atoll(next().c_str());
        else if (a == "--unverified") opt.keepUnverified = true;
        else if (a == "--reunpack") opt.reunpack = opt.again = true;
        else if (a == "--again") opt.again = true;
        else if (a == "--clean") cleanPages = true;
        else if (a == "--squash") { cleanPages = true; squashMode = next(); }
        else if (a == "--json") jsonOut = next();
        else if (a == "--text") textOut = next();
        else if (a == "--export") exportDir = next();
        else if (a == "--import") importJson = next();
        else if (a == "--dump") opt.dumpDir = next();
        else if (a == "--line") {   // --line <stored line index>=<cc|sr|amol|gemstar|tvi1|tvi2|fast|wst|vitc|test|norpak|none>
            std::string v = next();
            size_t e = v.find('=');
            LineForce f = e == std::string::npos ? LineForce::Count : lineForceFromKey(v.substr(e + 1));
            if (f == LineForce::Count) { fprintf(stderr, "--line wants <index>=<auto|none|cc|sr|amol|gemstar|tvi1|tvi2|fast|wst|vitc|test|norpak>\n"); return 1; }
            opt.lineForce[atoi(v.substr(0, e).c_str())] = f;
            opt.again = true;
        }
        else if (a == "--format") {
            std::string k = next();
            opt.force = formatFromKey(k);
            if (opt.force == VbiFormat::Unknown && k != "auto") {
                fprintf(stderr, "unknown format '%s' (tbc bttv cx88 saa7134 cx23885 em28xx ivtv bttv-pal cx88-pal cx23885-pal em28xx-pal auto)\n", k.c_str());
                return 1;
            }
        }
        else if (a == "--repair") repairSpec = next();
        else in = a;
    }
    int lastPct = -1;
    opt.progress = [&](double f, const std::string&) {
        int p = (int)(f * 100);
        if (p / 10 != lastPct / 10) { fprintf(stderr, "%d%% ", p); lastPct = p; }
    };
    if (!repairSpec.empty()) {   // VBI repair (VHSTTX): a repaired copy next to the recording, decoded instead of it
        VbiRepair rp;
        std::string rerr;
        if (!vbiRepairParse(repairSpec, rp, rerr)) { fprintf(stderr, "%s\n", rerr.c_str()); return 1; }
        std::string out = in.substr(0, in.find_last_of('.')) + "_repaired.vbi";
        fprintf(stderr, "VBI repair (%s) -> %s\n", rp.text().c_str(), out.c_str());
        if (!vbiRepairFile(in, out, opt.force, rp, [&](double x) { opt.progress(x, ""); }, nullptr, rerr)) { fprintf(stderr, "repair: %s\n", rerr.c_str()); return 1; }
        fprintf(stderr, "\n");
        in = out;
        opt.again = true;
    }
    DecodeResult r;
    std::string err;
    bool cached = !opt.again && opt.lineForce.empty() && opt.maxFrames == 0 && opt.force == VbiFormat::Unknown && resultsCached(in) && loadResults(in, r, err);
    if (cached) fprintf(stderr, "saved results: %s (--again decodes again)\n", resultsCachePath(in).c_str());
    if (!cached && !decodeFile(in, opt, r, err)) {
        for (auto& l : r.log) fprintf(stderr, "%s\n", l.c_str());
        fprintf(stderr, "\nerror: %s\n", err.c_str());
        return 2;
    }
    fprintf(stderr, "\n");
    if (!importJson.empty()) {
        SsDatabase old;
        if (!ssImportJson(importJson, old, err)) fprintf(stderr, "import: %s\n", err.c_str());
        // old data first, then the freshly decoded packets update it
        SsStats fresh = r.db.stats;
        old.merge(r.db);
        r.db = old;
        r.db.stats.commands = fresh.commands;
    }
    std::string sum = summarize(r);
    for (size_t p; (p = sum.find('\r')) != std::string::npos;) sum.erase(p, 1);
    printf("%s", sum.c_str());
    // files and texts of their own, as the window "Found files and texts" lists them
    prepareFinds(r, [](const std::string& s) { fprintf(stderr, "%s\n", s.c_str()); });
    auto finds = findsOf(r);
    if (!finds.empty()) printf("\nFound files and texts:\n");
    for (auto& f : finds)
        printf("  %-9s %s%s  [%s]  %s  %s, %s  %s\n", f.kind == Find::File ? "FILE" : f.kind == Find::Announced ? "ANNOUNCED" : f.kind == Find::Text ? "TEXT" : "TEST",
               f.name.c_str(), f.shortName.empty() ? "" : (" (" + f.shortName + ")").c_str(), f.type.c_str(), f.status.c_str(), f.service.c_str(), f.where.c_str(), f.when.c_str());
    if (!jsonOut.empty() && !ssExportJson(jsonOut, r.db, &r, err)) fprintf(stderr, "%s\n", err.c_str());
    if (!textOut.empty()) {
        FILE* f = _wfopen(widen(textOut).c_str(), L"wb");
        if (f) {
            fprintf(f, "%s\n", sum.c_str());
            for (auto& l : captionsTimed(r)) fprintf(f, "%s\n", l.c_str());
            for (auto& l : r.xds) fprintf(f, "%s\n", l.c_str());
            for (auto& m : r.silentRadio) fprintf(f, "[SR frame %lld]%s%s %s\n", (long long)m.frame, m.zone.empty() ? "" : " zone ", m.zone.c_str(), m.text.c_str());
            for (auto& a : r.amol)
                for (auto& t : a.stamps)
                    fprintf(f, "[AMOL frame %lld] %s%s\n", (long long)t.frame, t.text().c_str(), t.parityOk ? "" : "  (parity error)");
            for (auto& g : r.gemstar)
                for (auto& m : g.messages)
                    fprintf(f, "[Gemstar frame %lld L%d F%d] %s %s%s\n", (long long)m.frame, g.lineNo, g.field, m.x2 ? "2x" : "1x", m.hex().c_str(),
                            m.copies > 1 ? (" (x" + std::to_string(m.copies) + ")").c_str() : "");
            for (auto& l : r.fastText) fprintf(f, "%s\n", l.c_str());
            for (auto& l : r.db.log) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
    }
    if (cleanPages && !r.teletext.project.empty()) {
        // teletext pages cleaned as in the GUI's "Clean pages": <project>/clean
        try {
            std::string note, out = ttClean(r.teletext.project, console_progress(), note, "", squashMode);
            printf("\nClean pages: %s\ncleaned project: %s\n", note.c_str(), out.c_str());
        } catch (std::exception& e) { fprintf(stderr, "clean: %s\n", e.what()); }
    }
    if (!exportDir.empty()) {
        try {
            printf("%s", exportResult(r, exportDir, console_progress()).c_str());
        } catch (std::exception& e) {
            fprintf(stderr, "export: %s\n", e.what());
            return 1;
        }
    }
    return 0;
}
