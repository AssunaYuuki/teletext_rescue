#pragma once
#include "silentradio.h"
#include <cstdint>
#include <string>
#include <vector>

// EIA-608 closed captions (line 21 field 1) and XDS (line 21 field 2).
// bytes holds pairs as received, parity bit included.
// firstChannel: 1 for field 1 (CC1/CC2), 3 for field 2 (CC3/CC4).
void ccDecodeCaptions(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines, int firstChannel = 1);
void ccDecodeXds(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines);
// XDS packets with a good checksum (finds which field really is field 2).
int ccCountXds(const std::vector<uint8_t>& bytes);
double ccParityRatio(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& valid);

// Silent Radio: see silentradio.h

