#pragma once
// Colour system of a 625-line recording, from the colour subcarrier left in the VBI lines:
//   SECAM: line identification signals ("bottles", D'R up to 4.75 MHz, D'B down to 3.90 MHz,
//          lines 7-15) or FM subcarriers that alternate line by line (D'B 4.250 / D'R 4.406 MHz
//          at rest), so one stored line shows two different frequencies frame after frame;
//   PAL:   one fixed subcarrier at 4.43361875 MHz.
#include "vbifile.h"
#include <string>

struct ColourInfo {
    std::string system;   // "SECAM", "PAL", "NTSC" or "" (not seen)
    std::string why;      // how it was found, for the report
    int secamLines = 0, palLines = 0;
};

ColourInfo detectColour(VbiFile& vf);
