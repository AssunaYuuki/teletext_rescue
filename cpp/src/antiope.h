// Antiope (teletext system A of ITU-R BT.653, the French service of the 1980s: SAVOIR/A2 on Antenne 2, TF1, FR3),
// carried by Didon packets: 6.203125 Mbit/s (397 x fH), 320 bits a line = run-in 55 55, framing code E7, a 5-byte
// prefix in Hamming 8/4 (channel address 3 nibbles, continuity index, format) and 32 data bytes with odd parity.
// A page is a data group of its channel: SOH RS, its number and flags in Hamming nibbles, then the page itself in the
// videotex coding (US row column positions, SS2 accent + letter, SO / SI mosaics, ESC attributes, REP), ETX EOT.
// The lines are read with the per-line channel MLSE (mlse.h); every page is voted over its copies, the texts a page
// turns through kept apart.
//
// The page as the receiver showed it (checked against screen photos of SAVOIR/A2, 30.01.1986):
//   - header nibble 8 = 1ccc: the colour of the whole screen (rows 1-24), ccc in the usual order (E = cyan, B = yellow,
//     F = white, A = green); text on a light screen is black, on a dark one white;
//   - every row (US, a row change) starts with those colours, normal size, no mosaics;
//   - ESC 4/x foreground, ESC 5/x background: the background, lining and concealing take effect at the next space
//     (a delimiter) in text, at once in mosaics; ESC 4/C-4/F normal / double height / width / size (a double height
//     character grows up into the row above); ESC 5/D 5/C inverted / normal; ESC 5/8 conceal, 5/F reveal;
//   - mosaics 2 x 3 as in teletext (5/F = the full block, 5/A lining = separated), APB / APF / APD / APU move the cursor.
#pragma once
#include "vbifile.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AntiopeCell {
    enum : uint8_t { DoubleHeight = 1, DoubleWidth = 2, Separated = 4, Underline = 8, Conceal = 16, Flash = 32,
                     CoveredLeft = 64,     // the right half of a double width character
                     CoveredBelow = 128 }; // the upper half of a double height character
    std::string ch = " ";    // UTF-8 (text cells)
    uint8_t mosaic = 0;      // a mosaic cell: its code (0x20-0x7F, sextants as in teletext), else 0
    uint8_t fg = 7, bg = 0;  // colours 0-7: black red green yellow blue magenta cyan white
    uint8_t flags = 0;
};

struct AntiopePage {
    std::string number;              // "083" (the page number the header gives)
    int copies = 0;                  // transmissions voted
    std::vector<std::string> rows;   // 25 rows of 40 characters, UTF-8 (mosaic cells as a block)
    std::vector<AntiopeCell> cells;  // 25 x 40, row 0 first: the page with its colours, sizes and mosaics
    int screen = 0;                  // the colour of the screen (rows 1-24)
};

struct AntiopeResult {
    std::vector<int> rows;           // stored lines carrying Didon packets
    std::vector<int> tvLines;        // their TV lines (625: 19-21, 332-334 on Antenne 2 1986)
    std::vector<double> rates;       // share of frames whose prefix read, per line
    long long packets = 0;
    std::string channel;             // the channel the pages came from ("07B")
    long long groups = 0;            // whole data groups (no packet lost)
    std::vector<AntiopePage> pages;  // in page order; a page turning through texts gives several
    std::string service;             // the service name if a page shows one in its first rows ("SAVOIR/A2")
};

// the Didon lines of a 625-line recording and the pages they carry; false when there are none
bool antiopeDecode(VbiFile& vf, const VbiGeometry& g, int64_t frames, int threads, const std::function<void(double)>& progress,
                   const std::atomic<bool>* cancel, AntiopeResult& out);
std::vector<std::string> antiopeReport(const AntiopeResult& r);   // [0] = the headline, then the report
// the file name of each page ("083", "083_2" when the page turned through several texts), in r.pages order
std::vector<std::string> antiopeNames(const AntiopeResult& r);
// the page's title for lists: its first row of real text below the header
std::string antiopeTitle(const AntiopePage& p);
// the page drawn as the receiver showed it: 480 x 500 (cells of 12 x 20), 0xRRGGBB; concealed text only when reveal
struct Image;
Image antiopeImage(const AntiopePage& p, bool reveal = false);
// writes <dir>/<page>.txt and .png, all.txt and index.html (a page viewer); returns the path of all.txt ("" when
// nothing was written)
std::string antiopeWrite(const AntiopeResult& r, const std::string& dir);
