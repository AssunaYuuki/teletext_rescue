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
#include "ttpages.h"
#include "rescache.h"
#include "version.h"
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>

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
               "             [--threads N] [--frames N] [--unverified] [--again] [--reunpack] [--clean]\n"
               "  .flac recordings are unpacked once to x_vbi/x.vbi next to them; --reunpack does it again\n"
               "  a recording decoded before opens from <recording>_vbi/results.trc; --again decodes it again\n"
               "             [--import old.json] [--json out.json] [--text out.txt]\n");
        return 1;
    }
    DecodeOptions opt;
    std::string in, jsonOut, textOut, importJson, exportDir;
    bool cleanPages = false;
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
        else if (a == "--json") jsonOut = next();
        else if (a == "--text") textOut = next();
        else if (a == "--export") exportDir = next();
        else if (a == "--import") importJson = next();
        else if (a == "--dump") opt.dumpDir = next();
        else if (a == "--format") {
            std::string k = next();
            opt.force = formatFromKey(k);
            if (opt.force == VbiFormat::Unknown && k != "auto") {
                fprintf(stderr, "unknown format '%s' (tbc bttv cx88 saa7134 cx23885 em28xx ivtv bttv-pal cx88-pal cx23885-pal em28xx-pal auto)\n", k.c_str());
                return 1;
            }
        }
        else in = a;
    }
    int lastPct = -1;
    opt.progress = [&](double f, const std::string&) {
        int p = (int)(f * 100);
        if (p / 10 != lastPct / 10) { fprintf(stderr, "%d%% ", p); lastPct = p; }
    };
    DecodeResult r;
    std::string err;
    bool cached = !opt.again && opt.maxFrames == 0 && opt.force == VbiFormat::Unknown && resultsCached(in) && loadResults(in, r, err);
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
    if (!jsonOut.empty() && !ssExportJson(jsonOut, r.db, &r, err)) fprintf(stderr, "%s\n", err.c_str());
    if (!textOut.empty()) {
        FILE* f = _wfopen(widen(textOut).c_str(), L"wb");
        if (f) {
            fprintf(f, "%s\n", sum.c_str());
            for (auto& l : r.captions) fprintf(f, "%s\n", l.c_str());
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
            std::string note, out = ttClean(r.teletext.project, console_progress(), note);
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
