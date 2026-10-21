#pragma once
#include "tvinform.h"
#include <cstdint>
#include <string>
#include <vector>

// ТВ-информ at fТ3 = 6.9375 MHz (444 x fH), GOST R 50822-95 (RTR / ВГТРК, 23.12.2001, lines 19 and 332).
// A line is 360 bits = 45 bytes, bit 0 first: a run-in (a 2.8 MHz tone, not on the data clock), the
// header FB AC, then 41 data bytes. The levels are not inverted; the eye is narrow (the 2T pulse of the
// recording is 296 ns), so the bits come from an MLSE (16 states, the observation of a bit depends on
// two bits either side; the channel is learnt from the recording itself). A 27 MHz capture window ends
// before the last 2 bits of the line: byte 40 has only its low 6 bits.
//
// The data stream runs over the lines in time order (field 1, field 2, field 1 ...): the next slot of
// the stream repeats the last 2d bytes of the slot d lines before it (d = 1 for the next line). Records:
//   7E 80 80 80 80 | seq u16 | id u32 | type | len u16 | 00 | CRC-32 of the 10 header bytes |
//   CRC-32 of the body | body (len bytes)          (CRC-32 as zlib, stored low byte first)
// types: 05 = a file (attributes, size u32, name), 06 = a block (number u32, offset = (number-1) x 1100,
// 1100 bytes), 03 = before a file ("GRMC0000 .. CNT0000A"), 09 = a block with no length/CRC (FF FF FF FF),
// 00 = beacon (DOS date and time of the inserter + test text "TV-INFORM / THE RED FOX ...").
// The service sends on more VBI lines than a capture card keeps (11 of the ГОСТ lines a field): of the
// record numbers only the share on the captured lines is seen, so files are rarely complete.
constexpr int kTvi3Bytes = 41;          // data bytes of a line (after 55 55 FB AC)
// a line with an equalizing pulse mid-line (lines of the field sync): bytes 13..23 are the pulse and the blanking
// after it, the data go on after them - the word is 30 bytes
constexpr int kTvi3GapFrom = 13, kTvi3GapTo = 24;
constexpr int kTvi3GapBytes = kTvi3Bytes - (kTvi3GapTo - kTvi3GapFrom);

// share of probe frames whose line carries the fТ3 header (0 = none)
int tvi3TestLine(const uint8_t* probe, int probeN, size_t frameBytes, size_t lineOff, int fmt, int samples, float fs);

// slices the lines of one VBI line through the recording; the channel is learnt on the first lines
// with data, then every line goes through the MLSE
class Tvi3Slicer {
public:
    // live: learns on the first 150 lines with data (or 750 lines), so the words come within ~30 s, and
    // learns once more when 1500 lines with data have come
    explicit Tvi3Slicer(float fs, bool live = false);
    void add(const uint8_t* line, int fmt, int samples, int64_t frame, int lineNo, int field);
    std::vector<TviWord> finish();      // one word per line with the header: 41 bytes
    std::vector<TviWord> take() { std::vector<TviWord> o; o.swap(out_); return o; }   // the words so far (live)
    bool trained() const { return trained_; }
    int learnt() const { return varied_; }               // lines with data gathered for the learning
    int toLearn() const { return needVaried_; }
    struct Model { float mu[32][2]; float var[2]; };
private:
    struct Pending { int64_t frame; int lineNo, field; std::vector<float> raw; bool varied; };
    float fs_, T_;
    bool trained_ = false;
    int needVaried_ = 1500, needLines_ = 8000;
    int retrainAt_ = 0;         // live: lines with data for the second learning (0 = none)
    int varied_ = 0;            // buffered lines with data (enough bit changes to learn the channel)
    Model m_{};
    std::vector<Pending> buf_;
    std::vector<TviWord> out_;
    void train();
    void flush();
    void emit(int64_t frame, int lineNo, int field, const std::vector<float>& y, const std::vector<float>& raw);
};

// the stream of all fТ3 lines (words of both fields in s.words): records, files, beacons, texts.
// dir = results folder (files and texts are written there); fps = frame rate (recording times)
void tvi3Build(TviStream& s, const std::string& dir, double fps);
