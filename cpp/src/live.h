#pragma once
// Live decoding of VBI frames as they come from a capture card (capture.cpp): what every line
// carries, WST 625-line teletext pages (as a TV builds them), EIA-608 captions, and the raw
// frames written to a .vbi recording that the full decoder opens afterwards.
#include "vbifile.h"
#include "nabts/teletext.h"
#include "biznestext.h"
#include "ttdata.h"
#include "finds.h"
#include "tvinform.h"
#include "tviftr3.h"
#include "fastdata.h"
#include "vbicodes.h"
#include "starsight.h"
#include "gemstar.h"
#include "jtesrx.h"
#include "vbirepair.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct LiveMlse;
struct LivePrior;
struct DecodeResult;


// Teletext of a whole recording read as the live decoder reads it (the place of the packet, the
// channel-fitting MLSE, headers whose address the tape broke mended), but each teletext line of all
// frames in batches: on the graphics card when gpu (if there is one), else on the processor.
// Returns the packets (42 bytes each) in frame / line order.
struct LiveReadStats {
    std::vector<int> lines;      // line indexes read as teletext
    size_t packets = 0, mended = 0;
    bool onGpu = false;
};
std::vector<uint8_t> liveReadTeletext(const std::string& path, const VbiGeometry& g, int64_t frames, bool gpu,
                                      const std::function<void(double, const std::string&)>& progress,
                                      const std::atomic<bool>* cancel, LiveReadStats& st, int64_t first = 0);   // from frame first

// what a VBI repair does to the teletext of frames [from, from + count) of a recording: the frames read by the live
// decoder (run-in, MLSE for tapes) as recorded and as repaired; the 50 frames before them let it find the lines first
bool liveRepairMeasure(const std::string& path, VbiFormat format, const VbiRepair& r, int64_t from, int64_t count,
                       VbiRepairMeasure& m, std::string& err);

struct LiveLineStat {
    int index = 0, lineNo = 0, field = 1;
    std::string kind;        // "WST teletext", "CC 608", "data 5.00 Mbit/s", "signal", "" (empty)
    std::string probe;       // what the line probe of the full decoder says (every few seconds)
    double level = 0;        // peak-to-peak of the line, in sample units
    double good = 0;         // share of the last frames with a valid packet / pair
    int packets = 0;         // teletext packets read on this line, in all
    int mlse = 0;            // of them read by the MLSE (tape) decoder
    int mode = 0;            // set by hand: 0 automatic, 1 read as teletext, 2 not read
    int force = 0;           // signal set by hand (LineForce of decoder.h), 0 = automatic
    std::string vitc;        // VITC time code read on this line in the last second ("" = none)
};

constexpr int kLiveLineAuto = 0, kLiveLineTeletext = 1, kLiveLineOff = 2;

struct LivePage {
    int number = 0;          // magazine page, hex (0x100..0x8FF)
    int sub = 0;             // subcode S4 S3 S2 S1 (14 bits)
    int national = 0;        // C12..C14
    Rows rows;               // row 0 = 8 spaces + 32 header characters
    int64_t frame = 0;       // last frame a row of it arrived
    int errors = 0;          // bytes that failed parity in the last transmission
    int counter = 0, of = 0; // the page's own "NN/MM" (a carousel under one subcode), 0 = none
};

// key of a page's transmission while it is being received (subcode 0000: which subpage it is
// is decided when the page closes, by its "NN/MM" counter or by its text)
constexpr int kLiveReceiving = -1;

struct LiveSnapshot {
    VbiGeometry geom;
    int64_t frames = 0;
    size_t packets = 0, badPackets = 0;
    int64_t dropped = 0;                        // frames the decoder had no time for (still recorded)
    std::vector<LiveLineStat> lines;
    std::vector<uint8_t> frame;                 // the last frame (for the raster)
    std::map<int, std::map<int, LivePage>> pages; // page -> subcode -> page (filled only when asked)
    std::vector<std::pair<int, int>> list;      // page, subpages (all pages seen)
    Row header{};                               // last header row (rolling header, clock)
    bool hasHeader = false;
    std::string service;                        // packet 8/30 status display
    std::string charset;                        // character set of the service, guessed from all its pages
    std::string channel;                        // name in the page headers (words every header carries)
    int ni = -1;                                // network identification code from packet 8/30 format 1
    size_t t42Packets = 0;                      // packets written to the .t42 of the recording
    std::string captions;                       // CC1 text, last lines
    std::string vps, wss;                       // the VPS label (line 16) and WSS (line 23) read, "" = none
    // the two top rows of the picture (line 23 of field 1 and 336 of field 2, luma): the line after the VBI window
    // of cards that stop at 22 / 335 (Behold, SAA7231); picWss = WSS read on it in the last 2 s
    std::vector<uint8_t> picRow[2];
    bool picWss[2] = {};
    double packetsPerSecond = 0;
};

// VPS / WSS of the live capture for their window: the label now (voted over the last 2 s) and its history
struct LiveVpsEvent {
    std::string time;      // clock time it came
    int64_t frame = 0;
    std::string key;       // what changed (label bytes / WSS bits)
    std::string text;
};
struct LiveVps {
    bool have = false, wssHave = false;       // read in the last 5 s
    bool everVps = false, everWss = false;    // read at all in this session
    uint8_t bytes[13] = {};                   // VPS bytes 3..15
    int wssBits = 0;
    int64_t reads = 0, wssReads = 0, frames = 0;
    int vpsLine = 0, wssLine = 0;             // TV line it is read on
    double vpsRate = 0, wssRate = 0;          // share of frames read lately
    int pending = 0, wssPending = 0;          // votes the label held
    std::vector<LiveVpsEvent> history, wssHistory;
};

class LiveDecoder {
public:
    LiveVps vpsInfo();
    void reset(const VbiGeometry& g);
    // one frame: field 1 lines then field 2 lines (geometry of reset()); decoded at once
    void feed(const uint8_t* frame);
    // the same from the capture thread: written to the recording at once, decoded by a worker
    // thread (frames it has no time for are skipped, never the recording); bytes = size of the frame
    void push(const uint8_t* frame, int bytes);
    void stopWorker();
    void joinProbe();
    // the top two rows of the picture (luma, w samples at fs): line 23 / 336, read for WSS and shown with the VBI
    // lines (from the capture thread, every picture frame)
    void pictureRows(const uint8_t* row0, const uint8_t* row1, int w, double fs);
    // the whole picture frame (YUY2): the row carrying line 23 / 336 is found (wherever the card's sync put it)
    // and read as above
    void pictureFrame(const uint8_t* p, int w, int h, int stride, double fs);
    // the lines are numbered d lines lower (+) or higher (-) than the card says: the picture was shifted on the way
    // (VCR, processor) and the driver keeps its window (Sync in the capture window); 0 = the card's numbering.
    // Kept until reset() (the geometry's own numbering again)
    void setLineOffset(int d);
    // VBI repair (VHSTTX controls) on every frame before it is decoded; the recording keeps the frames as they came
    void setRepair(const VbiRepair& r);
    // copy for the window; page = which page to copy in full (0 = none)
    void snapshot(LiveSnapshot& s, int page);
    // .vbi recording of the frames as they come, and next to it <name>.t42 with the teletext packets
    // decoded live (42 bytes each, ready for any teletext tool); empty path stops it
    // <name>.raw.t42: every packet with a good address, in order, data bytes as sliced - also the 8-bit
    // binary rows that fail the parity check (files sent in teletext, e.g. "Бизнес-Текст" 1996).
    // writeVbi = false: only the .t42 files (replaying a recording)
    bool record(const std::string& path, std::string& err, bool writeVbi = true);
    bool recording() const { return recOn_; }
    int64_t recordedFrames() const { return recFrames_; }
    std::string recordPath() const { return recPath_; }
    // files and texts found so far (ТВ-информ lines and Norpak packets, «Бизнес-Текст» catalogue and
    // data channel); files are written to filesDir() (next to the recording, else a temporary folder)
    std::vector<Find> finds();
    // what the «Бизнес-Текст» part is doing now: catalogue, the file and block on the air, rows, decrypting, written
    // ("" = no Бизнес-Текст heard)
    std::string btStatus(bool shortForm = false);
    std::string filesDir();
    // 525 lines: NABTS (CEA-516, NAPLPS pages) and Japanese teletext (JTES) gathered so far, as a decoded recording
    // for their windows (nullptr = none heard); remade every 4th probe chunk from all the session's packets
    std::shared_ptr<DecodeResult> pages525();
    void waitFiles() { if (btThread_.joinable()) btThread_.join(); }   // the «Бизнес-Текст» files being written
    // every teletext packet of the session so far (.t42 bytes), for saving at any moment
    std::vector<uint8_t> sessionPackets();
    // a line set by hand (kLiveLine*): teletext = searched for packets on every frame and taken without the
    // clean first packet a line not yet known must read; off = not read at all. Kept until reset().
    void setLineMode(int index, int mode) { if (index >= 0 && index < 64) lineMode_[index] = mode; }
    int lineMode(int index) const { return index >= 0 && index < 64 ? lineMode_[index].load() : 0; }
    // the signal of a line set by hand, as in the VBI lines window (LineForce of decoder.h, 0 = automatic):
    // teletext / nothing as above; captions read as 608 there; ТВ-информ fТ1 / fТ2 and Norpak packets read
    // by the probe thread on that line; the other kinds only name the line (and go to "Decode the recording")
    void setLineForce(int index, int force);
    int lineForce(int index) const { return index >= 0 && index < 64 ? lineForce_[index].load() : 0; }
    // only the lines set by hand are decoded (a test file played in the capture window); the line map
    // (the probe's names) still comes, so the user sees what to choose
    void setManual(bool m) { manual_ = m; }
    // every line read as if set by hand to teletext (a run-in looked for on every frame, packets that are
    // not clean kept): for a poor signal, so nothing is left out
    void setReadAll(bool a) { readAll_ = a; }
    bool readAll() const { return readAll_; }
    bool manual() const { return manual_; }
    ~LiveDecoder() { stopWorker(); joinProbe(); if (btThread_.joinable()) btThread_.join(); std::string e; record("", e); }

private:
    struct LineOut {
        bool wst = false, cc = false;
        uint8_t pk[42];
        int ccv[2] = {};
        double off = -1;     // where a packet was found (samples)
        bool raw = false;    // rpk: a packet with a good address that failed the parity test (binary data)
        uint8_t rpk[42];
        bool vps = false, wss = false;   // VPS bytes 3..15 / WSS bits read on this line
        bool vitc = false;               // VITC time code read on this line
        std::string vitcText;
        uint8_t vpsb[13];
        int wssv = 0;
    };
    LineOut line(int index, const uint8_t* p);   // runs in parallel: touches only its own LineAcc
    void packet(const uint8_t* p);   // 42 bytes after the framing code
    void closePage(int m);           // the page being received in magazine m is complete
    std::vector<uint8_t> session_;   // all packets of the session (under mu_)
    std::atomic<int> lineMode_[64] = {};
    std::atomic<int> lineForce_[64] = {};
    std::atomic<bool> manual_{false};
    std::atomic<bool> readAll_{false};
    int probeForce_[64] = {};        // lineForce_ when the probe chunk was taken (probe thread)
    // lines the probe of the full decoder names teletext: read as if set by hand to teletext (a run-in looked
    // for on every frame, the first packet need not be clean) - a weak tape line is caught without a switch
    std::atomic<bool> probeTT_[64] = {};
    void cc(int a, int b);

    std::mutex mu_;
    VbiGeometry g_;
    // capture thread -> worker
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::vector<uint8_t>> queue_;
    std::thread worker_;
    bool quit_ = false;
    int64_t dropped_ = 0;
    int frameBytes_ = 0;
    int64_t frames_ = 0;
    size_t packets_ = 0, bad_ = 0;
    std::vector<uint8_t> last_;
    struct LineAcc {
        double level = 0, good = 0;
        int packets = 0, ccOk = 0, mlsePackets = 0;
        std::shared_ptr<LiveMlse> mlse;   // its own channel model
        std::shared_ptr<LiveMlse> tape;   // the tape decoder (wide channel, codewords), for a weak teletext line
        int tapePackets = 0;
        double addrDiff = 0;              // share of the once-a-second checks where the tape decoder read another address
        double parityAvg = 0;             // parity errors in the text of the packets the narrow MLSE read (mean)
        double off = -1;     // where the teletext packet starts in this line (samples), -1 = not known
        int misses = 0;
        std::string kind;
        int kindAge = 0;
        double vpsGood = 0, wssGood = 0;   // share of frames with a VPS / WSS line read
        double vitcGood = 0;               // the same for VITC
    };
    std::vector<LineAcc> acc_;              // the worker's own, outside the lock
    std::vector<LiveLineStat> lineStats_;   // their state for the window, under the lock
    std::map<int, std::map<int, LivePage>> pages_;
    int open_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // page being received per magazine
    int openSub_[8] = {};
    int openRow_[8] = {};   // last row received of that page in this transmission
    bool openErase_[8] = {};   // its header had C4 (erase page)
    Row header_{};
    bool hasHeader_ = false;
    std::string service_;
    std::string charset_;
    int64_t charsetAt_ = -1000;
    int national_ = 0;
    std::string cc_;
    int ccLast_[2] = {-1, -1};
    std::vector<std::pair<int64_t, size_t>> rate_;   // frame, packets so far (for packets per second)
    std::shared_ptr<LivePrior> master_;               // the MLSE channel model shared by the lines
    double off_ = -1;                                 // packet start shared by the teletext lines
    FILE* rec_ = nullptr;
    std::atomic<int64_t> recFrames_{0};
    std::atomic<bool> recOn_{false};
    std::string recPath_;
    FILE* t42_ = nullptr;
    FILE* raw_ = nullptr;
    size_t t42Packets_ = 0;
    int ni_ = -1;
    std::map<std::string, std::set<int>> headerWords_;   // header word (raw codes) -> pages that carry it
    std::set<int> headerPages_;
    std::string lastHeaderText_;
    // line probe of the full decoder on the last frames, in its own thread
    std::deque<std::vector<uint8_t>> ring_;
    std::thread probe_;
    std::atomic<bool> probing_{false};
    int64_t probeAt_ = 0;
    std::map<int, std::string> probeKind_;   // TV line -> kind
    void startProbe();
    // found files: the probe thread reads ТВ-информ on the frames it was given (contiguous 5 s chunks)
    BtCatalog bt_;                           // under mu_, fed by the worker
    // VPS / WSS: the last 250 readings voted (under mu_)
    LiveVps vps_;
    void voteCodes();
    std::deque<std::string> vpsLast_;
    std::deque<int> wssLast_;
    int64_t vpsFrame_ = -1, wssFrame_ = -1;
    std::vector<uint8_t> picRow_[2];                // under mu_: the picture's rows 23 / 336
    int baseFirstLine_ = 0;                         // g_.firstLine as reset() got it (setLineOffset moves g_'s)
    std::mutex repMu_;
    VbiRepairer repairer_;
    std::atomic<bool> repOn_{false};
    std::vector<uint8_t> repFrame_;
    int64_t syncSeen_ = -1, syncMoves_ = 0;         // frame the field sync was last in the window; numbering changes
    std::string vitcText_[64];                      // under mu_: the last VITC of each line and its frame
    int64_t vitcAt_[64] = {};
    SsDatabase ss_;                                 // StarSight (525) from the probe chunks, merged (probe thread)
    std::map<std::pair<int, int>, std::vector<GemstarWord>> gemWords_;   // Gemstar words of every chunk (probe thread)
    size_t ssMade_ = 0;
    int64_t picWssAt_[2] = {-1000, -1000};          // frame WSS was last read on them
    // pictureFrame (the capture thread only): the picture row of line 23 / 336 found, when it was looked for / last read
    int picRowIdx_[2] = {-1, -1};
    int64_t picRowCheck_[2] = {-1000, -1000}, picRowSeen_[2] = {-1000, -1000};
    // «Бизнес-Текст» files assembled live: in a thread of their own (the key is found by a search over the
    // text, seconds), at most every 30 s and when new data rows came; the last result is shown meanwhile
    std::thread btThread_;
    std::atomic<bool> btBusy_{false};
    int64_t btFrames_ = -1, btRows_ = 0;     // when / with how many data rows they were last written (under mu_)
    std::vector<BtWritten> btWritten_;       // under mu_
    std::string btNote_;                     // under mu_
    TtDataPages td_;                         // Hamming 8/4 data pages (ТЕЛЕИНФ 790 / 795), the same
    std::map<int, TviStream> tviLive_;       // line index -> words so far (probe thread only)
    std::map<int, std::unique_ptr<Tvi3Slicer>> tvi3Slicers_;   // ТВ-информ fТ3 lines: their MLSE (probe thread only)
    TviStream tvi3Live_;                     // the fТ3 words of all its lines so far
    std::set<int> tvi3Lines_;                // line indices found carrying fТ3 (stay so)
    std::string t33_;                        // Norpak packets of the session (probe thread only)
    std::string filesDir_;                   // under mu_
    std::vector<Find> tviFinds_;             // under mu_
    std::vector<TviFile> norpakFiles_;       // the files of the last Norpak pass (probe thread only)
    std::vector<std::string> norpakBeacons_; // its source beacon lines
    int norpakChunk_ = 0;                    // chunks since that pass
    std::set<int> norpakLines_;              // TV lines the probe found Norpak packets on (probe thread only)
    std::map<int, int> norpakVotes_;         // TV line -> chunks the probe called it a packet line
    int norpakProbes_ = 0;                   // chunks with packet lines
    // 525 lines: the 5.7 Mbit/s packets of a probe chunk (read as a recording is) join fast525_; NABTS / JTES
    // lines named for the window (probe thread)
    void find525(const std::string& vbiPath, int64_t frames, int64_t firstFrame, std::map<int, std::string>& names);
    std::vector<FastPacket> fast525_;        // NABTS and JTES packets of the session (probe thread only)
    int fast525Chunk_ = 0;                   // chunks since the pages were made
    JtesRecRead jtesHow_;                    // the Japanese teletext receiver's reading, once a chunk found it
    int jtesTries_ = 0;                      // chunks probed for it without finding it (3 = no more)
    size_t fast525Made_ = 0;                 // packets they were made from
    std::shared_ptr<DecodeResult> res525_;   // under mu_
    void findFiles(const std::string& vbiPath, const std::string& fmt, const std::vector<uint8_t>& frames, const std::map<int, std::string>& kinds,
                   const std::set<int>& teletextLines, int64_t firstFrame, std::map<int, std::string>& names);
};
