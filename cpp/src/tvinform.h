#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ТВ-информ (TV-Inform): Russian VBI datacast, GOST R 50822-95. Carried on
// Channel 1 / ORT (Moscow, SECAM) 1993..mid-2000s. Three clocks fТ1/fТ2/fТ3
// = 111/222/444 x line rate (1.734375 / 3.46875 / 6.9375 MHz), NRZ, first data
// bit 10.6 us after the sync edge, a leading pilot pulse (first bit = 1).
// Only the physical layer is public; the packet format above it (subscriber
// address, rubric, error protection) was proprietary to AO "ТВ-информ", so
// the payload is extracted as raw bytes, not decoded.
//
// Our recordings use fТ2 = 3.46875 MHz, 180 bits (22 bytes + 4 spare) per line.
constexpr int kTviRateHz[3] = {1734375, 3468750, 6937500};
constexpr int kTviBits[3] = {90, 180, 360};

struct TviWord {
    int64_t frame = 0;
    int lineNo = 0, field = 1;
    std::vector<uint8_t> bytes;   // nbits/8 bytes, bit 0 sent first
    float quality = 0;
};

// Slices one line at clock index rate (0..2) with run-in clock recovery.
// samplesPerBit = capture sample rate / kTviRateHz[rate]. Returns false if the
// line carries no TV-Inform signal (no pilot / too few clean edges).
bool tviSlice(const uint8_t* line, int fmt, int samples, float samplesPerBit, int nbits, std::vector<uint8_t>& out, float& quality);

struct TviPage {              // a carousel page recovered by voting repeats
    int lineNo = 0, field = 1;
    int copies = 0;
    std::vector<uint8_t> bytes;
};

// fТ1 lines (ТВЦ, line 19): asynchronous serial frames as they leave the encoder's UART,
// 10 bits each (as sliced: start 1, 8 data bits LSB first, stop 0) at bit 30, 40 .. 80
// of the line: a header frame, then 5 characters. The line carries the INVERTED levels:
// inverted, these are plain UART frames of 7-bit KOI-7 with parity. Each 5-character
// chunk is sent on two lines (header 13/1C or 06/01).
// ORT 2000 adds a second kind on the same line: 1110 + a 16-bit header + 10 Hamming(7,4)
// codewords (sent d7 d6 d5 p4 d3 p2 p1, low nibble first) = 5 bytes, inverted = KOI8-R.
// Content seen so far = the inserter's idle test message: teleprinter "RYRY…RY 1234567890",
// "0000" and the name «ТВ-ИНФОРМ» (KOI8-R, in quotes) on the Hamming kind.
// chars below are stored already inverted (= real character codes).
struct TviChunk {
    int64_t frame = 0;
    uint16_t header = 0;
    uint8_t kind = 0;             // 0 = async serial (7-bit, parity stripped), 1 = Hamming(7,4) bytes
    uint8_t fixed = 0;            // Hamming: codewords with one bit corrected
    std::vector<uint8_t> chars;
};

// a file sent through the channel (records D5 F3 C9 07 = description, 03 = 256-byte blocks)
struct TviFile {
    uint32_t id = 0, size = 0, crc = 0;
    int blocks = 0, have = 0;                   // blocks in the file, blocks received intact
    int joined = 0;                             // of them: put together from damaged copies (CRC checked)
    std::string name, shortName, time, path;    // path = where it was written ("" = not written)
    std::string note;                           // what was made of it (a fax page as PNG, ...)
    std::vector<int> missing;                   // block numbers not received
    std::vector<int> damaged;                   // blocks taken from a damaged copy (its CRC does not match)
    std::vector<long> unread;                   // byte positions in the file not received or in a damaged packet
    std::vector<std::string> text;              // a text file: its lines as UTF-8 ("[…]" where bytes did not come)
};

struct TviStream {
    int rate = 1;             // clock index used (0..2)
    int nbits = 180;
    // 1 = an fТ1 Hamming(7,4) stream over several lines (tviJoin): words = the lines read on a fixed bit grid,
    // serial = one record per frame (kind 3), texts = its text lines
    int joined = 0;
    std::vector<TviWord> words;      // every sliced line, in order
    std::vector<TviPage> pages;      // stable carousel pages (voted)
    std::vector<TviChunk> serial;    // fТ1 serial channel, repeats dropped
    double entropy = 0;              // bits/byte of the payload (8 = random)
    std::vector<std::string> report;
    std::vector<std::string> files;  // packet lines (Norpak): files received, as written to disk
    std::vector<TviFile> fileInfo;   // the same files: name, size, blocks received, time
    std::vector<std::string> beacons;   // Norpak: source beacon lines ("name", updated, version, times)
    std::vector<std::string> texts;     // fТ3: readable text (WMO bulletins), pieces joined where they overlap
    std::vector<std::string> meteo;     // fТ3: the bulletins and their METAR reports in Russian (lines)
    std::vector<std::string> catalog;   // fТ3: files of the inserter (folder, name, size), as the descriptors name them
};

void tviBuild(TviStream& s);  // fills pages, serial, entropy, report from words
std::string tviSerialText(const TviStream& s, double fps);   // the serial channel, one chunk per line
std::string tviText(const TviStream& s);                     // the serial channel as text (UTF-8)

// what one ТВ-информ line carries, for the window
struct TviSummary {
    enum Kind { Idle, Test, Data, Packets } kind = Idle;
    std::string what;                  // one line: "test message of the inserter", "document, CP866, 103 lines", ...
    std::string charset;               // KOI-7 / KOI8-R / CP866 ("" = none)
    int lines = 0, dataLines = 0, chunks = 0, fixed = 0, ecTest = 0;
    std::vector<std::string> text;     // the text (a document in order; a test message as its distinct lines)
};
TviSummary tviSummarize(const TviStream& s);

// is a line ТВ-информ? Tried on probeN frames (frameBytes apart, the line at lineOff in each) at fТ1, then
// fТ2; fs = sample rate. force = 0 / 1: take that clock (line type set by hand), -1 = find it. capJob = the
// line also passed the caption-rate test (the fТ1 idle carrier does). rate = -1: not ТВ-информ;
// pairs: an fТ1 line read at fТ2 (every bit doubled) - its words go through tviCollapse.
struct TviLineTest { int rate = -1, hits = 0; bool pairs = false; };
TviLineTest tviTestLine(const uint8_t* probe, int probeN, size_t frameBytes, size_t lineOff, int fmt, int samples, float fs, int force, bool capJob);
double tviPairShare(const std::vector<uint8_t>& w, int nbits, int* phase = nullptr);   // share of equal bit pairs
std::vector<uint8_t> tviCollapse(const std::vector<uint8_t>& w, int nbits);            // fТ2 bytes -> fТ1 bytes

// fТ1 Hamming(7,4) stream over several lines (ORT «I программа - Европа», 07.03.1999: TV lines 317 - two halves
// either side of the equalizing pulse - 318 and 319). Each line carries Hamming(7,4) codewords (sent d0 d1 d2 p d3
// p p, levels inverted, low nibble first), and the lines of a frame are ONE stream of nibbles: the bytes run on
// over the line ends (here 5 + 5 + 13 + 13 = 36 nibbles = 18 bytes a frame). The first codeword of every segment
// starts in the blanking, its first bits are lost: it is rebuilt from the bits that are seen (where they fit two
// codewords, the one that makes text is taken). tviSlice reads from the first rising edge, so a line whose first
// seen bits are 0 comes out shifted or not at all - these lines are read on a fixed bit grid instead.
class TviGridSlicer {
public:
    explicit TviGridSlicer(float samplesPerBit) : T_(samplesPerBit) {}
    // lines without a signal are left out; the grid (threshold, phase) is learnt on the first 48 lines with one
    void add(const uint8_t* line, int fmt, int samples, int64_t frame, int lineNo, int field);
    std::vector<TviWord> take();         // the lines read so far (bit k = bytes[k/8] >> k%8, 1 = white), then forgotten
    std::vector<TviWord> finish();       // the same, the lines still waiting for the grid included
    int nbits() const { return nbits_; }
private:
    struct Pending { int64_t frame; int lineNo, field; std::vector<float> s; };
    float T_, thr_ = 0, ph_ = 0;
    int nbits_ = 0;
    bool trained_ = false;
    std::vector<Pending> buf_;
    std::vector<TviWord> out_;
    void train();
    void emit(const Pending& p);
};
// the lines read by TviGridSlicer (one vector per line) -> the Hamming streams among them (joined = 1, rate 0):
// lines of one field next to each other whose words carry Hamming(7,4) codewords in fixed places. tviBuild
// decodes a joined stream (records, text, clock)
std::vector<TviStream> tviJoin(const std::vector<std::vector<TviWord>>& lines, int nbits);
