// CCITT Group 4 (T.6) fax decoding, and repair of a fax TIFF whose directory was lost in transmission.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Decodes one G4 strip of `width` pixels into rows of 0/1 (1 = black), appended to `out`.
// lsbFirst = FillOrder 2. Returns the number of rows decoded (stops at EOFB, at `maxRows` or at a bad code;
// a row cut short by a bad code is still returned).
int g4Decode(const uint8_t* p, size_t n, int width, int maxRows, bool lsbFirst, std::vector<uint8_t>& out);

// A fax TIFF (G4 strips each ending in EOFB). When its directory is incomplete, the strips are found by
// their EOFB marks. Width/height are taken from what is left of the directory (defaults 1728 / 52 rows
// per strip). Writes a PNG of the page; the TIFF itself is rewritten with a complete directory to
// `fixedTiff` when that is not empty. Returns false when the data is not a G4 fax.
bool faxTiffToPng(const std::vector<uint8_t>& tiff, const std::string& png, const std::string& fixedTiff, std::string& note);

// The page itself: width x height pixels, 1 = black. False when the data is not a G4 fax TIFF.
bool faxTiffDecode(const std::vector<uint8_t>& tiff, int& width, int& height, std::vector<uint8_t>& px, std::string& note);
