#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ТВ-информ (TV-Inform): Russian VBI datacast, GOST R 50822-95. Carried on
// Channel 1 / ORT (Moscow, SECAM) 1993..mid-2000s. Three clocks fТ1/fТ2/fТ3
// = 111/222/444 x line rate (1.734375 / 3.46875 / 6.9375 MHz), NRZ, first data
// bit 10.6 us after the sync edge, a leading pilot pulse (first bit = 1).
// Only the physical layer is public; the packet format above it (subscriber
// address, rubric, error protection) was proprietary to AO "ТВ-информ", so
// the payload is extracted as raw bytes, not decoded.
//
// Our recordings use fТ2 = 3.46875 MHz, 180 bits (22 bytes + 4 spare) per line.
constexpr int kTviRateHz[3] = {1734375, 3468750, 6937500};
constexpr int kTviBits[3] = {90, 180, 360};

struct TviWord {
    int64_t frame = 0;
    int lineNo = 0, field = 1;
    std::vector<uint8_t> bytes;   // nbits/8 bytes, bit 0 sent first
    float quality = 0;
};

// Slices one line at clock index rate (0..2) with run-in clock recovery.
// samplesPerBit = capture sample rate / kTviRateHz[rate]. Returns false if the
// line carries no TV-Inform signal (no pilot / too few clean edges).
bool tviSlice(const uint8_t* line, int fmt, int samples, float samplesPerBit, int nbits, std::vector<uint8_t>& out, float& quality);

struct TviPage {              // a carousel page recovered by voting repeats
    int lineNo = 0, field = 1;
    int copies = 0;
    std::vector<uint8_t> bytes;
};

struct TviStream {
    int rate = 1;             // clock index used (0..2)
    int nbits = 180;
    std::vector<TviWord> words;      // every sliced line, in order
    std::vector<TviPage> pages;      // stable carousel pages (voted)
    double entropy = 0;              // bits/byte of the payload (8 = random)
    std::vector<std::string> report;
};

void tviBuild(TviStream& s);  // fills pages, entropy, report from words
