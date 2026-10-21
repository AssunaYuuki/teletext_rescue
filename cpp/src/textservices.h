#pragma once
#include "silentradio.h"
#include <cstdint>
#include <string>
#include <vector>

// a caption line with the frame it came on the screen and the frame it went (end = -1: not known yet)
struct CaptionCue {
    int64_t frame = 0, end = -1;
    std::string svc, text;   // "CC1" .. "CC4", "T1" .. "T4"
};

// EIA-608 closed captions (line 21 field 1) and XDS (line 21 field 2).
// bytes holds pairs as received, parity bit included.
// firstChannel: 1 for field 1 (CC1/CC2), 3 for field 2 (CC3/CC4).
// frames (one per byte pair): the frame each pair came in; with cues, every line also goes there with its frame.
void ccDecodeCaptions(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines, int firstChannel = 1,
                      const std::vector<int64_t>* frames = nullptr, std::vector<CaptionCue>* cues = nullptr);
void ccDecodeXds(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines);
// XDS packets with a good checksum (finds which field really is field 2).
int ccCountXds(const std::vector<uint8_t>& bytes);
double ccParityRatio(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& valid);

// Silent Radio: see silentradio.h

