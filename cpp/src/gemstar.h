#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Gemstar (VCR Plus+ / Guide Plus+) data lines, seen on FOX Family 1999
// lines 17/18. Two framings share the lines:
//   2x: 10.5 us blank, 5 cycles of 503 kHz run-in, frame code 10011101101,
//       4 bytes NRZ at 64 x fH (1.007 Mbit/s).
//   1x: the line 21 caption framing (7 cycles run-in, 001 start, 2 bytes at
//       32 x fH); 0x80 0x80 = null.
// Bytes in both are sent LSB first. Control bytes are 7 bits + odd parity and
// form XDS-like packets (found on FOX Family 1999):
//   07 type data.. checksum 0F x   (sum of 07 .. checksum = 0 mod 128)
//   type 7A = clock: minute hour date month weekday year zone, each 0x40|value
//     (hour and month carry a flag in 0x20; weekday 1 = Sunday)
//   type 7C = text-like station data; the 2x idle words are an empty 7C
//     packet: 07 7C 80 FD | 8F 89 80 80.
// Between packets 2x carries binary download blocks of 80 bytes, 8-bit, no
// parity: 05 FC 4D 61, block number, 01 80, 73 bytes (scrambled guide data).
struct GemstarWord {
    int64_t frame = 0;
    bool x2 = true;        // 2x word (4 bytes) or 1x pair (2 bytes)
    uint8_t b[4] = {};     // bytes in sending order, bit 0 sent first
    bool ok = true;        // 1x: both parity bits right
    float quality = 0;
    std::string hex() const;
};

bool gemstarSlice2x(const uint8_t* line, int fmt, int samples, float lineSamples, uint8_t out[4], float& quality);
bool gemstarSlice1x(const uint8_t* line, int fmt, int samples, float lineSamples, uint8_t out[2], bool& parityOk);

struct GemstarMessage {
    int64_t frame = 0;         // first word
    bool x2 = true;
    std::vector<std::string> words; // hex per word, "????????" for a missed word
    int copies = 1;            // times the same message was received
    std::string hex() const;
};

struct GemstarPacket {         // 07 type .. checksum 0F, checksum right
    int64_t frame = 0;
    bool x2 = true;
    int type = 0;
    std::vector<uint8_t> data; // 7-bit values between the type and the checksum
    std::vector<uint8_t> raw;  // 7-bit values 07 .. checksum, nothing left out (Type A parsing)
    int lrc = -1;              // byte after 0F = XOR(07..checksum with parity) ^ 8F: 1 right, 0 wrong, -1 missing
    int copies = 1;
    std::string text() const;  // "clock: Fri 31 Dec 1999 17:40 (zone 5)", "7C: B00N01A@H.."
};

// One programme from a Show Information (SIP) message of the Type B guide data.
struct GemstarShow {
    int channel = 0;           // guide channel from the message PlusCode
    int year = 0, month = 0, day = 0;
    int start = -1;            // minutes after 0:00 UTC of that day (can pass 1440)
    int duration = 0;          // minutes
    int theme = 0, misc = 0;
    int64_t code = 0;          // the programme's own number (BCD in the record)
    int64_t frame = 0;
    std::string title, line1, line2;
};

struct GemstarClock {
    bool valid = false;
    int year = 0, month = 0, day = 0, weekday = 0, hour = 0, minute = 0, zone = 0;
    int64_t frame = 0;
};
GemstarClock gemstarClock(const GemstarPacket& p);

struct GemstarStream {
    int lineNo = 0, field = 1;
    int64_t words2x = 0, idle2x = 0, words1x = 0, null1x = 0, parityBad1x = 0;
    int64_t blocks = 0;                    // binary download blocks (05 FC 4D 61)
    std::vector<int> blockNumbers;         // distinct block numbers seen
    std::vector<std::string> idleWords;    // the 2x fill words
    std::vector<GemstarWord> words;        // in order
    std::vector<GemstarMessage> messages;  // in order, repeats folded into the first copy
    std::vector<GemstarPacket> packets;    // in order, repeats folded
    // Type A / Type B guide data (gemguide.cpp, as Gemstar's own monitor program FREQCCD_ does it)
    int64_t blocksGood = 0, blocksBad = 0; // Type B blocks whose chained checksum is right after decryption
    int keyYear = 0, keyMonth = 0;         // the date the Type B data was decrypted with
    std::vector<GemstarShow> shows;
    std::vector<std::string> guide;        // report lines: Type A headers, Type B messages, programmes
    std::vector<std::string> report;
};

// header of a Type A packet (07 7C / 7A), as FREQCCD_ reads it; payload still scrambled
struct GemstarTypeA {
    bool ok = false;
    std::string name, country;  // "Source Mapping", "USA"
    int block = 0, total = 0, version = 0, host = 0;
    std::vector<uint8_t> payload;
};
GemstarTypeA gemstarTypeA(const GemstarPacket& p);

void gemstarDecode(GemstarStream& s);      // words -> messages, packets
void gemstarGuide(std::vector<GemstarStream>& all); // Type A / Type B guide data; needs every stream for the clock
void gemstarBuildReport(GemstarStream& s); // call once the field is final
