// Japanese teletext (文字多重放送, JTES, ITU-R BT.653 system D) on 525-line VBI lines.
//   line: clock run-in, framing code, 272 bits = a (272,190) difference-set cyclic codeword
//   (generator x^82+x^77+x^76+x^71+x^67+x^66+x^56+x^52+x^48+x^40+x^36+x^34+x^24+x^22+x^18+x^10+x^4+1, as in DARC),
//   bits sent LSB first, the polynomial highest degree first.
//   190 data bits: prefix 14 (byte 4 = data channel, byte 5 bits 1-4 continuity, bit 6 = data group header)
//   + data block of 22 bytes. A data channel runs on each line separately (channel + line = one stream).
//   Data group: D-byte 1 = 01, D-bytes 2-7 header, then blocks; D-byte 20 = 17/03/04 ends a block, 21-22 = CRC.
//   First block = record header 1E 2x: programme number (000-999) and page (00-99); then blocks
//   STX 1F type length(2) data: 24 = header row, 20 = text, 30 = DRCS, 38 = picture.
//   Text: the 8-unit code that ARIB STD-B24 took over (G0-G3, LS/SS, default macros 60-6F, C1 colours and
//   sizes, APS row/column, active position = lower left corner of the character), screen 248 x 204 dots
//   under a header row, drawn with the fonts of the Panasonic TU-TX100 teletext tuner.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct FastPacket;

struct JtesUnit {
    int type = 0;
    std::vector<uint8_t> data;
};
struct JtesPage {
    int channel = 0, line = 0;        // data channel (prefix byte 4) and the line it came on
    int programme = 0, page = 0, hi = 0;
    int64_t frame = 0;                // first frame of the group kept
    int seen = 1;                     // times received
    bool gaps = false;                // a packet of the group was missing
    int raster = -1;                  // record header: raster colours, header row (high nibble) and page (low)
    std::vector<uint8_t> header;      // record header (1E ..)
    std::vector<JtesUnit> units;
    std::string title;                // the header row as text (UTF-8)
};
struct JtesResult {
    int64_t packets = 0, clean = 0, fixed = 0, failed = 0, groups = 0;
    std::map<int, int64_t> channels;  // data channel -> packets
    std::vector<JtesPage> pages;      // one per data channel, programme and page: the best copy
    std::vector<std::string> report;
};

// packets: 5.7 Mbit/s lines with the NABTS-like sync (kind 'E'), 37 bytes from the run-in;
// false when they are not Japanese teletext
bool jtesDecode(const std::vector<FastPacket>& packets, JtesResult& out);

struct JtesImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;         // 0xRRGGBB
};
// the page as the tuner shows it: header row (24 dots) over the 248 x 204 page; transparent = checker board
JtesImage jtesRender(const JtesPage& p);
// an animated page: the screen at each TIME code and how long it stays (seconds), then the screen after the
// last TIME (0); empty when the page does not animate
struct JtesFrame { JtesImage img; double seconds = 0; };
std::vector<JtesFrame> jtesFrames(const JtesPage& p);
// the text of the page (UTF-8), row by row
std::string jtesText(const JtesPage& p);
std::string jtesPageName(const JtesPage& p);   // "131-05"

// Sound (unit 2C): 80 (0;tempo;) .. 84 = a tune; voices 81 (n;) .. 85 play together; 90 (n;) = volume;
// notes are pairs: pitch (semitones, 48 = C4; 20 = rest), length (40 + ticks, 50 = quarter note).
// Worked out on the recording, not from the standard: 88-8C and 98 are skipped.
struct JtesNote { int voice = 0, midi = 0, volume = 7; double start = 0, length = 0; };
std::vector<JtesNote> jtesTune(const JtesPage& p);          // empty: no sound (or 80 (0;000;) = silence)
std::vector<uint8_t> jtesWave(const std::vector<JtesNote>& tune, int rate = 22050);   // a .wav file in memory

// every page into dir: <prog-page>.png (x3), .txt, .wav when it plays a tune, index.txt; returns the pages written
int jtesExport(const JtesResult& r, const std::string& dir);
