#pragma once
// PCX (ZSoft, DOS) pictures received with bytes lost or wrong («Бизнес-Текст» TOWER350.PCX): one damaged byte of
// the RLE stream shifts everything after it, so the picture is put together again line by line (pcxfix.cpp).
#include "png.h"
#include "util.h"
#include <string>
#include <vector>

struct PcxRepair {
    int width = 0, height = 0;
    int lines = 0, linesGood = 0;   // plane lines (bytes-per-line units) in the picture / decoded whole
    int gaps = 0;                   // damaged places the line start was found again after
    bool repaired = false;          // false = the plain decoding was kept (no damage, or the repair read less)
    std::string note() const;       // "640x350, 16 colours; 980 of 1400 lines whole, 23 damaged places"
};

// the picture: 1 bit black and white, 2-4 bit or 4 planes with the header's 16 colours, 8 bit with the palette at
// the end, 8 bit x 3 planes RGB. unread = positions of the bytes that did not come or did not read (sorted; empty =
// not known: two literal 00 bytes, which the encoder never writes, are taken as lost). Lines lost are the line above
// (fill) or a dark checker (marked, to watch the picture fill up). false = not a PCX picture.
bool pcxDecode(const Bytes& d, const std::vector<long>& unread, Image& im, bool fill, PcxRepair* info = nullptr);
inline bool isPcx(const Bytes& d) { return d.size() >= 129 && d[0] == 0x0A && d[1] <= 5 && d[2] == 1; }
