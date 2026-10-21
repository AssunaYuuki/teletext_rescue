#pragma once
// VBI repair of VHSTTX 2.7 (teletext/vbi/line.py apply_signal_controls, repair_dropouts, adaptive_normalise,
// apply_auto_line_align / wow-flutter; gui/vbirepair.py): the samples of every VBI line cleaned before decoding.
// Values as in VHSTTX: brightness / sharpness / gain / contrast 0-100 with 50 = unchanged; the others 0-100, 0 = off.
//   impulse      single-sample spikes pulled to the 3-sample median
//   temporal     each line blended with the same line of the frames before
//   noise        gaussian smoothing
//   hum          slow baseline (hum, tilt) taken out
//   blackLevel   the level before the data (20th percentile) taken as black
//   headSwitch   the end of the line ramped to the level before it (VHS head switching)
//   stabilize    the line's mean and spread held to those of the line just above (line-to-line)
//   autoGain     10th-90th percentile stretched to 18-236
//   dropout      samples far from the 5-sample median (dropouts) pulled to it
//   adaptive     the slow baseline under the data taken out for slicing (adaptive threshold)
//   wow          the start of the data on each line held near its start a frame before (wow / flutter)
//   align        the start of the data held near the median start of the recent lines (auto line align)
#include "vbifile.h"
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct VbiRepair {
    int brightness = 50, sharpness = 50, gain = 50, contrast = 50;
    int impulse = 0, temporal = 0, noise = 0, hum = 0, blackLevel = 0, headSwitch = 0, stabilize = 0, autoGain = 0;
    int dropout = 0, adaptive = 0, wow = 0, align = 0;
    // Stabilize VBI by hand (VHSTTX per_line_shift): stored line index -> samples the line is moved by (+ = later)
    std::map<int, int> lineShift;
    bool active() const;
    std::string text() const;     // "noise 30, dropout 50, line 7 +3" ("" = off)
    // the controls by name, for settings files and the window: name, pointer, neutral value
    struct Control { const char* key; const char* label; int VbiRepair::*v; int neutral; };
    static const std::vector<Control>& controls();
};

// keeps what the time-based controls need (the lines of the frames before)
class VbiRepairer {
public:
    explicit VbiRepairer(const VbiRepair& r = VbiRepair()) : r_(r) {}
    void set(const VbiRepair& r) { r_ = r; }
    const VbiRepair& get() const { return r_; }
    void reset() { hist_.clear(); stab_.clear(); align_.clear(); }
    // one frame of g.linesPerFrame lines, in place
    void frame(uint8_t* frame, const VbiGeometry& g);
    void line(float* y, int n, int key);   // one line of samples on the 8-bit scale
private:
    VbiRepair r_;
    std::map<int, std::vector<float>> hist_;    // temporal: per line
    std::map<int, std::vector<float>> stab_;    // stabilize: per line
    std::map<int, std::vector<double>> align_;  // align: starts of the last frames per line
};

// frames [from, to] of a recording repaired with settings of their own (VHSTTX "tuning ranges")
struct VbiRepairRange {
    int64_t from = 0, to = 0;
    VbiRepair r;
};

// a repaired copy of a recording (same layout); progress 0..1
// "noise=30,dropout=50,shift7=3" (keys of controls(), shiftN = line N moved); false + err for an unknown key
bool vbiRepairParse(const std::string& spec, VbiRepair& r, std::string& err);
bool vbiRepairFile(const std::string& in, const std::string& out, VbiFormat format, const VbiRepair& r,
                   const std::function<void(double)>& progress, std::atomic<bool>* cancel, std::string& err);
// the same with ranges (a frame in a range takes its settings, the others r) and only frames [from, to]
// (to < 0: to the end)
bool vbiRepairFile(const std::string& in, const std::string& out, VbiFormat format, const VbiRepair& r,
                   const std::vector<VbiRepairRange>& ranges, int64_t from, int64_t to,
                   const std::function<void(double)>& progress, std::atomic<bool>* cancel, std::string& err);

// what the repair does to the teletext of some frames (liveRepairMeasure of live.h): [0] as recorded, [1] repaired
struct VbiRepairMeasure {
    struct Line { int index = 0; int packets[2] = {0, 0}; };   // teletext packets read on a stored line
    std::vector<Line> lines;
    long packets[2] = {0, 0};
    long bytes[2] = {0, 0}, bad[2] = {0, 0};   // text bytes of the packets (rows 1-23) / with a parity error
};
