#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Nielsen AMOL I (Automated Measurement Of Lineups): 48 bits per line at
// 64 x fH (1.007 Mbit/s), NRZ, usually line 20 (or 22). Word layout, first
// bit sent first, fields MSB first:
//   phase reference 101 | start of message 0110 | frame address (5) |
//   payload 36 bits | parity (odd over frame address .. parity)
// The frame address counts 0..29, one cycle per second. Frame addresses 2
// and 3 carry the same source ID and timestamp:
//   source ID (6) month (4) day (5) hour (4) minute (6) second (6) PM (1) spare (3)
// (layout confirmed on WCAU "GL Demo" 1992; same on NBC 1983 and TNBC 1999).
// Some feeds send the stamp at 26/27 instead, 27 with the bits inverted, and
// text in frame addresses 20, 22, 24 (copies in 21, 23, 25): 5 characters of
// 7 bits, MSB first, no parity (from the original Teletext Rescue).
constexpr int kAmolBits = 48;

struct AmolWord {
    int64_t frame = 0;
    uint64_t bits = 0;   // bit 47 = first bit sent
    float quality = 0;   // smallest margin of the bit decisions, 0..1
};

// Slices one line. Returns true when the AMOL preamble is there.
bool amolSlice(const uint8_t* line, int fmt, int samples, float lineSamples, uint64_t& bits, float& quality);

inline int amolField(uint64_t w, int first, int len) { return (int)((w >> (kAmolBits - first - len)) & ((1u << len) - 1)); }
inline int amolFrameAddress(uint64_t w) { return amolField(w, 7, 5); }
inline uint64_t amolPayload(uint64_t w) { return (w >> 1) & ((1ULL << 35) - 1); } // 35 data bits
bool amolParityOk(uint64_t w);

struct AmolStamp {
    int64_t frame = 0;
    int sid = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    bool pm = false;
    bool parityOk = false; // the copy used passed the parity check
    uint64_t bits = 0;     // the word it was read from
    int fa = 0;            // frame address it came in
    bool inverted = false; // sent with the message bits inverted (frame address 27 on some feeds)
    std::string text() const; // "SID 18  4/30  3:28:12 PM"
};

struct AmolRun {               // seconds that follow each other
    AmolStamp first, last;
    int seconds = 0, bad = 0;
};

struct AmolStream {
    int lineNo = 0, field = 1;
    int64_t words = 0, parityOk = 0, parityBad = 0, idle = 0, faFixed = 0;
    std::vector<AmolStamp> stamps;
    std::vector<AmolRun> runs;
    struct Other { int fa = 0; uint64_t payload = 0; int count = 0; int64_t firstFrame = 0; };
    std::vector<Other> other; // payloads of the other frame addresses, distinct
    struct Text { int64_t frame = 0; std::string text; }; // frame addresses 20..25: 5 ASCII characters of 7 bits each
    std::vector<Text> texts;
    std::vector<std::string> report;
};

void amolDecode(const std::vector<AmolWord>& words, AmolStream& s);
void amolBuildReport(AmolStream& s); // fills s.report; call once the field is final
std::string amolSourceName(int sid); // known source IDs, empty when unknown
