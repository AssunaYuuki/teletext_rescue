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

struct SrFrame {
    // a still picture (cols, one 15-bit column per LED column, bit 0 = top)
    // or text that scrolls in from the right (lines: columns + top row)
    bool scroll = false;
    std::vector<uint16_t> cols;
    std::vector<std::pair<std::vector<uint16_t>, int>> lines;
    double hold = 2.0;           // seconds a still frame stays up
    double speed = 40;           // scrolling, columns per second
    bool wipe = false;           // a still frame wipes in from the left
    std::string what;            // the text shown, for captions
    double duration() const;
    std::vector<uint16_t> at(double t, const std::vector<uint16_t>& prev) const;
};

struct SilentRadioMessage {
    int64_t frame = 0;           // recording frame where the packet started
    std::string text;            // all text of the item, " / " between parts
    std::string zone;            // A, B, C (or the raw zone bytes)
    int seq = -1;                // packet sequence number, -1 = loose text
    int copies = 1, intact = 0;  // repeats received / repeats with a clean end
    std::vector<std::string> texts;
    std::vector<SrFrame> frames; // what the sign shows for this item
};

void srDecode(const std::vector<uint8_t>& bytes, const std::vector<int64_t>& frames,
              std::vector<SilentRadioMessage>& out);
// 5 x 7 sign font: columns, bit 0 = top row
std::vector<int> srGlyph(char32_t ch);
std::vector<uint16_t> srTextColumns(const std::string& s);
