// VideoCrypt I / II data lines (the satellite scrambling system of the 1990s: Sky, MTV Europe ...): 40 bits a line,
// 1.286 us a bit (18 periods of 14 MHz), LSB first; VideoCrypt I on lines 12-15 / 325-328, VideoCrypt II on lines
// 8-11 / 321-324. A frame's 8 lines = 40 bytes, interleaved by 8x8 bit transposes, then 40 Hamming 8/4 codewords =
// 20 bytes [header, 8 data, sum, counter or mode, 8 data, sum]; a 32-byte message is sent in two halves (the second
// half's header has the nibbles of the first's swapped) and sums to 0. The messages are only read and counted here:
// what they carry is the scrambling system's own business (ECM / EMM), nothing of the picture is recovered.
#pragma once
#include "vbifile.h"
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct VcMessage {
    int64_t frame = 0;      // frame of its first half
    int sequence = 0;       // header of the first half
    int counter = 0, mode = 0;
    uint8_t bytes[32] = {};
};

struct VcStream {
    int system = 0;               // 1 = VideoCrypt I, 2 = VideoCrypt II
    std::vector<int> rows;        // stored lines it is on (4 of field 1, 4 of field 2)
    int64_t frames = 0;           // frames looked at
    int64_t halves = 0;           // frames whose 40 bytes decoded and both sums held
    int64_t messages = 0;         // whole messages (two halves) whose sum held
    int64_t badMessages = 0;      // two halves found whose message sum did not hold
    int distinct = 0;
    std::map<int, int> kinds;     // first byte of the message -> messages
    std::vector<VcMessage> list;  // every whole message, in order
};

// the VideoCrypt lines of a 625-line recording (frames read through vf): empty when there are none.
// progress(fraction) is called now and then; cancel stops it
std::vector<VcStream> videocryptPass(VbiFile& vf, const VbiGeometry& g, int64_t frames, const std::function<void(double)>& progress,
                                     const std::atomic<bool>* cancel);
// report lines (a headline first) and the messages as text, one a line
std::vector<std::string> videocryptReport(const std::vector<VcStream>& s);
std::string videocryptMessagesText(const std::vector<VcStream>& s);
