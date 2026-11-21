// Silent Radio (WTTW Chicago, US4620227 / US4551720): an LED news sign fed
// from one VBI line, 16 bits per line at 80 x fH, sent MSB first.
// The byte stream carries packets  FF seq(2) len(2) 00 03 ?? ?? C1 zone(2)
// + body; the body holds sign scripts (text, bitmap columns, holds, wipes)
// and filled-in templates (headlines, sports scores) for zones A, B and C.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

constexpr int SR_W = 112, SR_H = 15;   // the sign: 112 x 15 LEDs

// How a still picture replaces the one before it. The script sends a code
// (1F xx) per picture; srEffect() maps the codes to these.
enum SrFx : int {
    SR_CUT = 0,
    SR_WIPE_RIGHT,    // revealed from the left edge to the right
    SR_WIPE_LEFT,     // from the right edge to the left
    SR_WIPE_OUT,      // from the middle outwards
    SR_WIPE_IN,       // from both edges to the middle
    SR_SLIDE_LEFT,    // pushed in from the right, the old picture leaves to the left
    SR_SLIDE_RIGHT,   // pushed in from the left
    SR_SCROLL_UP,     // rolls up from the bottom
    SR_SCROLL_DOWN,   // rolls down from the top
    SR_DISSOLVE,      // LEDs change one by one in random order
    SR_SPLIT,         // top line slides in from the left, bottom line from the right
    SR_PAN,           // the sign's window moves fxArg columns over its wider memory
    SR_FX_COUNT
};
const char* srFxName(int fx);

struct SrFrame {
    // a still picture (cols, one 15-bit column per LED column, bit 0 = top)
    // or text that scrolls in from the right (lines: columns + top row)
    bool scroll = false;
    std::vector<uint16_t> cols;
    std::vector<std::pair<std::vector<uint16_t>, int>> lines;
    double hold = 2.0;           // seconds a still frame stays up (the transition included)
    double speed = 36;           // scrolling, columns per second
    int fx = SR_CUT;             // transition from the picture before (SrFx)
    double fxTime = 0;           // seconds the transition takes
    int fxArg = 0;               // SR_PAN: columns the window moves (> 0 = to the right)
    int fx2 = -1;                // the bottom text line (rows 8-14) comes in this way; -1 = as fx
    std::string what;            // the text shown, for captions
    double duration() const;
    std::vector<uint16_t> at(double t, const std::vector<uint16_t>& prev) const;
};

struct SilentRadioMessage {
    int64_t frame = 0;           // recording frame where the packet started
    std::string text;            // all text of the item, " / " between parts
    std::string zone;            // A, B, C (or the raw zone bytes)
    int seq = -1;                // packet sequence number, -1 = loose text
    int copies = 1, intact = 0;  // repeats received / repeats read without a lost line
    std::vector<std::string> texts;
    std::vector<SrFrame> frames; // what the sign shows for this item
};

// bytes: the line's byte stream, 2 per field; frames: the recording frame per byte pair.
// valid (optional): per byte, the line had the preamble; erasures keep the copies of a packet
// aligned. soft (optional): 8 signed values per byte (MSB first, > 0 = 1) for voting bit by bit.
void srDecode(const std::vector<uint8_t>& bytes, const std::vector<int64_t>& frames,
              std::vector<SilentRadioMessage>& out, const std::vector<uint8_t>* valid = nullptr,
              const std::vector<float>* soft = nullptr);
// 5 x 7 sign font: columns, bit 0 = top row
std::vector<int> srGlyph(char32_t ch);
std::vector<uint16_t> srTextColumns(const std::string& s);
