#pragma once
// Time code on a VBI line of ORT (Channel 1, Moscow; sample.vbi 2001, line 6), found on the recording:
//   at the start of the line ~14 periods of 1.000 MHz (a reference frequency), then a flat part, then one
//   symbol per frame at the end of the line: 2.5 MHz = 1, 1.67 MHz = 0, 1.0 MHz = second mark, nothing = gap
//   (all from a 5 MHz clock). 25 symbols = one second, sent the second it names:
//     [mark][gap][1][1][hours tens 2][hours units 4][minutes tens 3][minutes units 4][parity][seconds tens 3][seconds units 4]
//   BCD, most significant bit first; the parity bit makes the seconds bits even. The two ones after the gap
//   may be the parities of the hours and the minutes (the recordings so far cannot tell).
#include "vbifile.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct TvTimeSecond {
    int64_t frame = 0;      // frame of the second mark
    int h = 0, m = 0, s = 0;
    bool parityOk = false;
};

struct TvTimeCode {
    int line = -1;                       // stored line index
    int64_t frames = 0, symbols = 0;     // frames looked at, frames with a symbol
    std::vector<TvTimeSecond> seconds;   // decoded seconds (all fields in range, parity good)
    std::vector<std::string> report;
};

// 1 MHz burst at the start and a symbol at the end in most of the first frames
bool tvTimeProbe(VbiFile& vf, int line, int frames = 200);
// reads the whole recording on that line
bool tvTimeDecode(VbiFile& vf, int line, int64_t maxFrames, TvTimeCode& out, std::atomic<bool>* cancel = nullptr);
