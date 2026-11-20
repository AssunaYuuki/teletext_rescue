// Antiope (teletext system A of ITU-R BT.653, the French service of the 1980s: SAVOIR/A2 on Antenne 2, TF1, FR3),
// carried by Didon packets: 6.203125 Mbit/s (397 x fH), 320 bits a line = run-in 55 55, framing code E7, a 5-byte
// prefix in Hamming 8/4 (channel address 3 nibbles, continuity index, format) and 32 data bytes with odd parity.
// A page is a data group of its channel: SOH RS, its number and flags in Hamming nibbles, then the page itself in the
// videotex coding (US row column positions, SS2 accent + letter, SO / SI mosaics, ESC attributes, REP), ETX EOT.
// The lines are read with the per-line channel MLSE (mlse.h); every page is voted over its copies, the texts a page
// turns through kept apart.
#pragma once
#include "vbifile.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct AntiopePage {
    std::string number;              // "083" (the page number the header gives)
    int copies = 0;                  // transmissions voted
    std::vector<std::string> rows;   // 25 rows of 40 characters, UTF-8 (mosaic cells as a block)
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
std::vector<std::string> antiopeReport(const AntiopeResult& r);   // [0] = the headline
// writes <dir>/<page>.txt and all.txt; returns the path of all.txt ("" when nothing was written)
std::string antiopeWrite(const AntiopeResult& r, const std::string& dir);
