#pragma once
#include "amol.h"
#include "gemstar.h"
#include "jtes.h"
#include "tvinform.h"
#include "fastdata.h"
#include "nabts.h"
#include "ttpages.h"
#include "starsight.h"
#include "textservices.h"
#include "vbifile.h"
#include "legacy_bridge.h"
#include "vitc.h"
#include "finds.h"
#include <map>
#include <memory>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// a line's signal set by hand before decoding (VBI lines window); Auto = let the decoder find it
enum class LineForce : uint8_t { Auto = 0, None, Captions, SilentRadio, Amol, Gemstar, TviT1, TviT2, Fast, Teletext625, Vitc, Test, Norpak, TviT3, Count };
const char* lineForceName(LineForce f);   // "Captions / XDS / StarSight (32 x fH)" ...
const char* lineForceKey(LineForce f);    // "cc", "sr", ... (trcli --line 21f1=cc)
LineForce lineForceFromKey(const std::string& k);   // Count if unknown

struct JtesRecRead;

struct DecodeOptions {
    bool gpu = false;
    int threads = 0;              // 0 = all cores
    bool keepUnverified = false;  // also use schedule data from packets that failed CRC
    int64_t maxFrames = 0;        // 0 = whole file
    VbiFormat force = VbiFormat::Unknown;
    std::string dumpDir;          // write raw per-line byte streams here
    std::function<void(double, const std::string&)> progress;
    std::function<void(const std::string&)> log; // each report line as soon as it is known
    std::function<void(int64_t, int64_t)> count;  // frames decoded so far, frames in all
    std::atomic<bool>* cancel = nullptr;
    bool reunpack = false;        // FLAC: unpack again even if x_vbi/x.vbi is ready
    bool again = false;           // results folder x_vbi: decode again even where results exist
    std::map<int, LineForce> lineForce;   // stored line index -> signal set by hand
    bool probeOnly = false;       // open the recording (format, geometry) and decode nothing
    std::string sourcePath;       // internal: the file the user opened (a FLAC), for the results folder
    // Japanese teletext receiver (27 MHz 8-bit 525 lines): how a recording is read, when already known (live chunks);
    // jtesFound gets what the probe found; jtesProbe false = no probe (live gave up after a few chunks)
    const JtesRecRead* jtesHow = nullptr;
    JtesRecRead* jtesFound = nullptr;
    bool jtesProbe = true;
};

struct LineInfo {
    int index = 0;      // line index in the frame
    int lineNo = 0;     // NTSC line number
    int field = 1;
    int mode = 0;
    double detectRate = 0;
    std::string kind;   // "StarSight", "Captions", "XDS", "Silent Radio", ...
    int tviRate = 1;    // ТВ-информ clock index (0 = fТ1 111 x fH, 1 = fТ2 222 x fH)
};

struct DecodeResult {
    // files and texts found (prepareFinds: made once, with the «Бизнес-Текст» files written); null = not yet
    std::shared_ptr<const std::vector<Find>> finds;
    std::string path;
    std::string source;           // the FLAC file this recording was unpacked from (empty = path itself)
    std::string colour, colourWhy;   // "SECAM" / "PAL" / "NTSC" / "" and how it was found
    std::string captureNotes;     // notes saved with the capture (FLAC comment, e.g. "2048x32@25Hz")
    VbiGeometry geom;
    std::string slicerName;
    double sliceSeconds = 0, totalSeconds = 0;
    std::vector<LineInfo> lines;
    SsDatabase db;
    std::vector<std::string> captions;
    std::vector<CaptionCue> captionCues;   // the same caption lines with their frames (times: VITC or recording)
    std::vector<VitcCode> vitc;            // VITC time code, one per frame that has it
    std::vector<std::string> vitcReport;
    std::vector<std::string> timeCode;     // the time code line of ORT (line 6: 1 MHz burst + 25 symbols a second)
    std::map<int, LineForce> lineForce;    // line types the user set by hand for this decoding
    std::vector<std::string> xds;
    std::vector<SilentRadioMessage> silentRadio;
    std::vector<AmolStream> amol;          // Nielsen AMOL I lines
    std::vector<GemstarStream> gemstar;    // Gemstar 1x/2x lines (VCR Plus+ / Guide Plus+)
    std::vector<TviStream> tvinform;       // ТВ-информ datacast lines (GOST R 50822-95)
    bool fieldsSwapped = false;            // the capture stores field 2 first (found from XDS)
    std::vector<FastPacket> fastPackets;   // 5.7 Mbit/s lines (NABTS, WST, framing 2D)
    std::vector<std::string> fastReport, fastText;
    Bytes nabtsT33;                        // NABTS packets as a .t33 stream (33 bytes each, in order)
    std::vector<int> nabtsField, nabtsLine; // per packet: field count (frame * 2 + field - 1) and line
    std::vector<nabts::Record> nabtsRecords; // CEA-516 records, pages drawn by the NAPLPS interpreter
    nabts::Summary nabtsSummary;
    LegacyReport legacy;                   // 625-line recordings: the original Teletext Rescue engine (x_vbi folder)
    TtResult teletext;                     // WST pages (VBI 525-line or DVB .ts), project folder
    JtesResult jtes;                       // Japanese teletext (BT.653 system D) pages
    std::vector<std::string> log;
};

bool decodeFile(const std::string& path, const DecodeOptions& opt, DecodeResult& res, std::string& err);
std::string summarize(const DecodeResult& r);
// the first lines of the report: colour system, ТВ-информ yes / no, teletext, other services
std::vector<std::string> headline(const DecodeResult& r);
// programme names and clocks from XDS, StarSight, AMOL and teletext (onair.cpp)
std::vector<std::string> onAirSummary(const DecodeResult& r);
