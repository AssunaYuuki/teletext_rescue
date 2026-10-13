#pragma once
// Bridge to the engine of the original Teletext Rescue (src/legacy, compiled inside namespace legacy).
// Its 625-line machinery is used as is: format detection, the line map (WST PAL teletext, VPS, WSS,
// Nagravision/Syster, SECAM identification, test signals), the teletext tape decoder and the
// results folder "<recording>_vbi" (report.txt, report.json, teletext project, files per service).
#include "vbifile.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct LegacyLine {
    int row = 0, tvLine = 0;      // record index in the frame, TV line (field 2: 313 + n)
    std::string kind;             // "WST PAL teletext", "VPS", "Nagravision (Syster) access data", ...
    double signal = 0;
};
struct LegacyResult { std::string service, kind, path, note; };
struct LegacyReport {
    std::string format, dir;      // format name of the old program; results folder
    std::vector<LegacyLine> lines;
    std::vector<LegacyResult> results;
    std::vector<std::string> vits;
};

// format name the old program detects ("bt8x8-pal", ...) and ours for it (Unknown if none)
VbiFormat legacyDetectFormat(const std::string& path, std::string* name = nullptr);
// results folder of a recording: x.vbi -> x_vbi, x.vbi.flac -> x_vbi (as in the old program)
std::string legacyReportDir(const std::string& recording);
// runs the old engine on a recording (or its FLAC; the unpacked copy is shared). again = decode again
// even where results already exist. Returns false with err on failure.
bool legacyRun(const std::string& recording, bool again, const std::function<void(const std::string&)>& step,
               const std::function<void(const std::string&)>& log, std::atomic<bool>* cancel, LegacyReport& out, std::string& err);
