#pragma once
// 5.727 Mbit/s VBI data (364 x fH): NABTS (framing E7), NTSC WST teletext
// (framing 27) and the unidentified KCET 1994 service (framing 2D, payload
// sent MSB first). Decoded with MLSE, see fastdata_core.h.
#include "vbifile.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

constexpr int kFastBits = 400; // bits decoded per line (WST 625 needs 360 + slack)

struct FastJob {
    int line = 0;
    float phi = 0;
    float spb = 0;      // samples per bit
    float H[16] = {};
};

struct FastCalib {
    bool ok = false;
    char kind = 0;      // 'N' NABTS, 'W' WST, 'X' framing 2D
    float phi = 0;
    float spb = 0;
    double rate = 364;   // bit rate in multiples of fH
    float H[16] = {};
    double syncRate = 0; // share of calibration lines with a clean sync
    double headerOk = 0; // NABTS: share of Hamming-valid header bytes
};

struct FastPacket {
    int64_t frame = 0;
    int lineNo = 0, field = 1;
    char kind = 0;
    int syncErr = 0;
    int crc = -1;               // 2D: -1 unchecked, 0 CRC ok, 1/2 bits repaired, 3 failed
    std::vector<uint8_t> bytes; // from the run-in on; LSB-first for N/W, MSB-first for X
};

// Framing 2D (Interactive Network): AA AA B4 | service | counter | 27 data |
// 4 check bytes | 00. The check is a CRC-32, generator x^32 + 0xBE340351, over
// the 27 data bytes taken LSB first, XOR a term that depends on the header
// (learned from the recording). Repairs 1 and 2 bit errors in place, sets
// FastPacket::crc; returns report lines.
std::vector<std::string> fd2dCrc(std::vector<FastPacket>& pk);

// Calibrates the channel model on sample lines (pointers to raw line data).
// forceRate > 0 fixes the bit rate (multiples of fH) instead of searching.
FastCalib fdCalibrate(const std::vector<const uint8_t*>& lines, const VbiGeometry& g, double forceRate = 0);

// Finds the sync in decoded bits; returns false if none.
bool fdFindSync(const uint8_t* bits, int nb, char& kind, int& pos, int& err);
FastPacket fdMakePacket(const uint8_t* bits, int nb, char kind, int pos, int err);

int hamming84(uint8_t b); // -1 if uncorrectable

// One 5.7 Mbit/s line whose kind is known (N NABTS, W WST, X framing 2D,
// E NABTS sync without a NABTS header).
struct FastLineJob {
    int index = 0, lineNo = 0, field = 1;
    char kind = 0;
    std::vector<int> services; // 2D: service bytes seen on the line
    bool cycle8 = false;       // 2D: the counter byte runs the 8-step cycle 8B 9C 4D C6 17 E8 39 5A
};
// Reads the lines again with the per-line channel MLSE (fastmlse.cpp);
// packets in time order. Lines it cannot learn are left out.
std::vector<FastPacket> fdSecondPass(VbiFile& vf, const std::vector<FastLineJob>& jobs, int threads, std::atomic<bool>* cancel,
                                     const std::function<void(double)>& progress, const std::function<void(const std::string&)>& note,
                                     std::vector<std::pair<int, int>>* rejected = nullptr, // (line, field) found not to be the kind
                                     bool gpu = false);   // the Viterbi of the original engine on the GPU (MlseBatch)

struct FastReport {
    std::vector<std::string> lines;  // human readable summary
    std::vector<std::string> text;   // recovered text messages
};
FastReport fdReport(const std::vector<FastPacket>& pk);
