#pragma once
// VITC (vertical interval time code, SMPTE 12M / EBU): 90 bits per line at 115 x fH (525 lines,
// 1.7898 Mbit/s) or 116 x fH (625 lines, 1.8125 Mbit/s), NRZ. Nine groups of 10 bits, each starting
// with the sync pair 1 0; bits sent LSB first:
//   2-5 frame units      6-9 user bits 1     12-13 frame tens  14 drop frame  15 colour frame  16-19 UB2
//   22-25 second units   26-29 UB3           32-34 second tens 35 field mark (525)              36-39 UB4
//   42-45 minute units   46-49 UB5           52-54 minute tens 55 binary group flag             56-59 UB6
//   62-65 hour units     66-69 UB7           72-73 hour tens   74, 75 flags (625: 75 = field)   76-79 UB8
//   82-89 CRC, generator x^8 + 1 over bits 0-81 (so every 8th bit of the 90 sums to 0).
// Usually two lines a field, lines 10-20 (525) or 6-22 (625); studio and camera tapes carry it.
#include <cstdint>
#include <string>
#include <vector>

struct DecodeResult;

constexpr int kVitcBits = 90;

struct VitcCode {
    int64_t frame = 0;
    int hour = 0, minute = 0, second = 0, fr = 0;
    bool drop = false, colourFrame = false, fieldMark = false;
    uint32_t user = 0;          // user bits, UB1 in the low nibble
    int lineNo = 0, field = 1;  // where it was read
    std::string text() const;   // 01:02:03:04 (01:02:03;04 drop frame)
};

// Slices one line; true when the 9 sync pairs, the CRC and the digit ranges are right.
bool vitcSlice(const uint8_t* line, int fmt, int samples, float lineSamples, bool pal, VitcCode& out, float& quality);

// frames since 00:00:00:00 and back (drop-frame counting for 30 frames/s with the drop flag)
int64_t vitcToCount(const VitcCode& c, bool pal);
VitcCode vitcFromCount(int64_t n, bool pal, bool drop);

// the time code at a frame of the recording: the nearest code read, counted on by the frames in
// between (codes are on every frame of a tape that has them). false if the recording has none near.
bool vitcAt(const std::vector<VitcCode>& codes, int64_t frame, bool pal, VitcCode& out);

// report lines: lines carrying VITC, first / last code, breaks (edits), user bits
std::vector<std::string> vitcReport(const std::vector<VitcCode>& codes, bool pal, double fps);

// caption times: VITC time code when the recording has it, otherwise the place in the recording
std::string captionTime(const DecodeResult& r, int64_t frame);
// "captions.txt" with a time in front of every line; one .srt per caption service (CC1, T1 ...)
std::vector<std::string> captionsTimed(const DecodeResult& r);
std::string captionsSrt(const DecodeResult& r, const std::string& svc);
bool captionsUseVitc(const DecodeResult& r);
