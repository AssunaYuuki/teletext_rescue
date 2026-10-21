// Japanese teletext receiver for 8-bit 27 MHz captures (GV-USB2 / EM28xx 1440 samples a line, Hauppauge
// cx23885 the same layout): the waveform recovery of JTES Decoder 0.9.50 (receiver.py, receiver_hauppauge.py,
// core.py) in C++. The heavy work (MLSE, code correction, start search; jtes_core.h) runs on the Slicer given:
// the processor or the GPU, as the user chose.
//   GV-USB2: level and edge of the run-in; MLSE over 2 samples a bit with a 7-tap channel per half bit (bundled
//   profile scaled to the line's contrast), run-in 55 55 A7 forced, 3 rates x 5 start offsets.
//   Hauppauge (tapes without time-base correction): the start searched by the run-in + service byte over -60..80
//   samples and 8 rates, two channel profiles.
//   Both: (272,190) code corrected by majority logic over its 273 cyclic checks (up to 8 bits); lines that fail are
//   tried again with the channel fitted to their own best guess, then with a channel learned from good packets;
//   single missing packets between two good ones of a stream get a wide timing search.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct FastPacket;
class VbiFile;
class Slicer;

struct JtesRxPacket {
    int64_t row = 0;          // line of the lines given: field * linesPerField + index
    double cost = 0, start = 0, spb = 0;
    int errors = 0;           // bits the code corrected
    uint8_t word[34] = {};    // the 272 bits after the run-in, first sent = bit 0 of byte 0
    bool refined = false;
};

struct JtesRxStats { int64_t scanned = 0, found = 0, refined = 0, wide = 0, idle = 0; int anchors = 0; };

// the GV-USB2 channel: 7 taps + level per half bit, measured at a run-in contrast, with the edge and start it was
// measured at (JTES Decoder 0.9.51 receiver.calibrate): the bundled VHS profile, or one fitted to the clearest idle
// packet of the recording, kept only when it reads clearly more of a sample of lines (damaged tapes, other decks)
struct JtesChannel {
    float h[16] = {};
    double contrast = 57, edge = 39, start = 40;
    bool fitted = false;
    int64_t sampleBundled = -1, sampleFitted = -1;   // packets each read on the sample (-1: not tried)
};

// lines: rows lines of n u8 samples, linesPerField a field; slots = indexes inside a field to read.
// hauppauge: start searched by the run-in, otherwise from the line's edge (GV-USB2) with chan (null: bundled).
// quick: the first reading only (no refining, no wide search). false + err when the Slicer failed.
bool jtesReceive(const uint8_t* lines, int64_t rows, int n, int linesPerField, const std::vector<int>& slots, bool hauppauge,
                 bool quick, Slicer& sl, std::vector<JtesRxPacket>& out, std::string& err, JtesRxStats* stats = nullptr,
                 const JtesChannel* chan = nullptr);

// a 27 MHz 8-bit 525-line recording (1440 samples, 12 lines a field): which lines carry Japanese teletext and
// which receiver reads them, from 3 pieces of 40 frames; false when there is none
struct JtesRecRead {
    bool hauppauge = false;
    std::vector<int> slots;        // line indexes within a field
    int64_t probePackets = 0;
    JtesChannel chan;              // GV-USB2: the channel chosen for the recording
    std::string why;
};
// only: line indexes within a field to look at (empty = all 12)
bool jtesProbeRecording(VbiFile& vf, Slicer& sl, std::atomic<bool>* cancel, JtesRecRead& out, const std::vector<int>& only = {});
// the whole recording (up to maxFrames, 0 = all) as 5.7 Mbit/s packets of kind 'E' (55 55 A7 + 34 bytes) in time order
bool jtesReadRecording(VbiFile& vf, const JtesRecRead& how, int64_t maxFrames, Slicer& sl, std::atomic<bool>* cancel,
                       const std::function<void(double)>& progress, std::vector<FastPacket>& out, std::string& err,
                       JtesRxStats* stats = nullptr);
