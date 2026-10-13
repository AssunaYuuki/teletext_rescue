#pragma once
// StarSight Telecast data transmission network (US5619274 / WO9531069).
//
// Transport verified on the KCET 1994 and 2000 recordings: line 16 of both
// fields, EIA-608 waveform but 8-bit binary bytes (no parity), 2 bytes per
// field = 960 bit/s. Packets:
//   0     sync 0x2C
//   1-2   size (whole packet, big endian)
//   3-6   timestamp, minutes since 1992-01-01 00:00 GMT
//   7-8   VBI stream id
//   9-10  CRC1 = low 16 bits of CRC over bytes 0-8, little endian
//   11..  commands
//   last4 CRC32 over everything before it, little endian
// CRC: poly 0x04C11DB7 reflected, init 0xFFFFFFFF, no final xor.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Minutes since 1992-01-01 00:00 GMT, the StarSight epoch.
using SsTime = int64_t;
std::string ssIso(SsTime t);                      // 1994-05-03T14:58:00Z
bool ssParseIso(const std::string& s, SsTime& t, int* sec = nullptr);
void ssCivil(SsTime t, int& y, int& mo, int& d, int& h, int& mi, int& wday);

struct SsChannel {
    int id = 0;
    std::string name;      // name-affiliation string (up to 8 chars)
    int native = 0;        // native channel number
    bool nameFlag = false; // display name instead of number
    int abbrevBits = 0;    // which name characters make the call letters
    bool verified = false;
    std::string callLetters() const;
};

// Built-in channel names for schedules whose channel packets were not received. false if unknown.
bool ssKnownChannel(int id, std::string& label, std::string& call);
// Huffman-compressed StarSight text (titles, descriptions). complete is false
// when a codeword is not in the table; the text then ends where it stopped.
std::string ssHuffmanDecode(const uint8_t* p, size_t n, bool* complete);

// Short name of a StarSight command type (0..63), from the StarSight patent
// US 7,210,159 Table VII. "" for a reserved/undefined type.
const char* ssCommandName(int type);

struct SsProgram {
    int id = 0;
    std::string title;
    int theme = 0;
    bool cc = false, stereo = false, bw = false;
    bool compressed = false;        // sent Huffman-compressed
    bool partial = false;           // the code table stopped short: text ends in ...
    std::vector<uint8_t> raw;       // payload as sent
};

struct SsDescription {
    int id = 0;
    std::string text;
    int theme = 0;
    int critic = 0;                 // 0 none, 1 poor .. 4 excellent
    std::string ratingSystem, rating;
    int year = 0;
    std::vector<std::string> advisories;
    bool cc = false, stereo = false, bw = false;
    bool compressed = false;
    bool partial = false;
    std::vector<uint8_t> raw;
};

struct SsSlot {
    SsTime start = 0;
    int duration = 0;
    int program = 0;
    int description = 0;            // 0 = none
    int flags = 0;                  // 0x80 has DID, 0x40 pay-per-view, 0x01 joined in progress
};

struct SsShowList {
    int channel = 0;
    SsTime start = 0;
    int version = -1;               // -1 = imported from JSON
    bool verified = false;
    std::vector<SsSlot> slots;
};

struct SsClock {
    SsTime time = 0;
    int seconds = 0;
    int utcOffsetMinutes = 0;
    bool dst = false;
};

struct SsTheme {
    int id = 0;
    std::string name;
};

struct SsStats {
    int64_t streamBytes = 0;
    int packets = 0;
    int crcOk = 0;
    int fixedSingle = 0;            // one bit repaired by CRC syndrome
    int fixedChase = 0;             // repaired by flipping weak (soft) bits
    int fixedCombine = 0;           // repaired by combining repeated copies
    int failed = 0;
    int headerFixed = 0;
    std::map<int, int> commands;    // command type -> count
    int encrypted = 0;
    std::map<int, int> encryptedByType; // command type -> count, flag 0x80 on the type byte
    std::set<uint32_t> timeKeys;    // 4 extra bytes of the time command (they change every minute)
    // incremental update, like Schedules Direct's "only what changed"
    int listsAdded = 0, listsReplaced = 0, listsUnchanged = 0;
    int titlesAdded = 0, descriptionsAdded = 0, channelsAdded = 0;
};

struct SsDatabase {
    std::map<int, SsChannel> channels;
    std::map<int, SsProgram> programs;
    std::map<int, SsDescription> descriptions;
    std::map<std::pair<int, SsTime>, SsShowList> showLists;
    std::map<int, SsTheme> themes;
    std::vector<SsClock> clock;
    std::vector<std::pair<SsTime, SsTime>> dstChanges;
    std::vector<uint32_t> sequenceNumbers;
    SsStats stats;
    std::vector<std::string> log;

    void merge(const SsDatabase& other);
    bool empty() const { return showLists.empty() && programs.empty() && channels.empty(); }
    // Flattened, time-sorted slots of a channel.
    std::vector<SsSlot> slotsFor(int channel) const;
    std::vector<int> channelIds() const;
    SsTime firstTime() const;
    SsTime nowTime() const;         // latest Time command, or first slot
    int utcOffset() const;          // minutes, from the latest Time command
    std::string titleOf(int program) const;
    // title for a schedule slot (only what the recording sent)
    std::string titleOf(const SsSlot& s, bool* guessed = nullptr) const;
};

// One decoded byte stream with per-bit confidence (|soft value|), as produced
// by the slicer for one VBI line pair (field 1 + field 2).
struct SoftStream {
    std::vector<uint8_t> bytes;
    std::vector<float> conf; // 8 per byte, bit i = value 1<<i
};

// Finds packets, repairs them, parses commands into db.
void ssDecodeStream(const SoftStream& s, SsDatabase& db, bool keepUnverified);

// Packets whose CRC32 checks, allowing one repaired bit (used for calibration).
int ssCountGoodPackets(const std::vector<uint8_t>& bytes);

uint32_t ssCrc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu);
