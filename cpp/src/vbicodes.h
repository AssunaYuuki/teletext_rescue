#pragma once
// Fixed service lines of 625-line television:
//   VPS (EN 300 231, line 16 of field 1): 15 bytes, biphase at 2.5 Mbit/s (5 MHz half-bits): run-in, start code,
//       then bytes 3..15 - source/control data, sound, the label (CNI of the network, PIL = announced start
//       of the programme or a special code) and the programme type
//   WSS (EN 300 294, line 23 of field 1): 14 bits, each sent as 6 elements at 5 MHz (biphase) - aspect ratio,
//       film/camera mode, subtitles, surround sound, copy protection
#include <cstdint>
#include <string>
#include <vector>

// y = one line's samples (any scale), fs = samples per second. b = bytes 3..15 of the line (13)
bool vpsRead(const float* y, int n, double fs, uint8_t b[13]);
// the same for a line already known to carry VPS: reads the frames vpsRead leaves out (not for finding VPS)
bool vpsReadKnown(const float* y, int n, double fs, uint8_t b[13]);
struct VpsLabel {
    int cni = 0;          // 12 bits, e.g. 0xD88
    int day = 0, month = 0, hour = 0, minute = 0;
    int pty = 0;          // programme type (byte 15)
    int sound = 0;        // byte 5 bits 7-6: 0 unknown, 1 mono, 2 stereo, 3 two channels
};
VpsLabel vpsLabel(const uint8_t b[13]);
std::string vpsCniName(int cni);         // "VIVA", "" if not in the register
std::string vpsText(const uint8_t b[13]);   // one line for people

// The station's text in byte 4 (b[1]), one character a frame: b4 = (frame, byte 4 as read) in frame order.
// "" when byte 4 is no repeating text; period = frames a turn. VIVA: "V I V A", Novy Kanal: "NOVY.TV.",
// Eurosport "Eurospor". Read also where the capture lost frames (a card that keeps 2 frames of 3 shows the text
// in another order: "VI  AV  VA  IV"): bit 7 marks the first character, so a turn has exactly one marked one.
struct VpsTextStats {
    int stretches = 0;       // stretches of ~4 s the text was read on
    int lossy = 0;           // of them, stretches where the capture had lost frames
    int keepNum = 0, keepDen = 0;   // the commonest loss there: keepNum frames of every keepDen kept (2 of 3)
};
std::string vpsStationText(const std::vector<std::pair<int64_t, uint8_t>>& b4, int* period = nullptr, VpsTextStats* stats = nullptr);
// one name for people: the register's name when the station text is that name (VIVA, "Eurospor" = Eurosport),
// else the station's own text ("NOVY.TV", "DBP-2"), else the register's name, else a near register entry
// ("Studio 1+1?" for 7C1), else ""
std::string vpsDisplayName(const std::string& station, int cni);
std::string vpsCniGuess(int cni);        // a register entry of the same country one or two codes off: "Studio 1+1 (7C0)"

// A whole recording's VPS line: everything it tells. One reading = the 13 bytes of one frame.
struct VpsReading { int64_t frame = 0; uint8_t b[13] = {}; bool second = false; };   // second = read by vpsReadKnown
struct VpsFile {
    int cni = 0;
    std::string head;                  // one line: "VPS: NOVY.TV. · M1 (CNI 7C5) · ..."
    std::string name;                  // short name for the line map: station text, else the register name, else "CNI 7C5"
    std::string station;               // the text sent in byte 4, a character a frame ("" = none)
    std::vector<std::string> report;   // the full text
    std::string csv;                   // every frame read: frame; time; 13 bytes; the character of byte 4
    // what it was made from (to make it again with the clocks of the recording once the teletext is read)
    std::vector<VpsReading> readings;
    int64_t frames = 0;
    double fps = 25;
    int line = 0, field = 0;
};
// Clocks of the same recording that tell the real time (filled by the decoder when it has them)
struct VpsClocks {
    std::string ttDate;              // date in the teletext page headers ("Do 23.11.95"), "" = none
    std::string ttFrom, ttTo;        // first and last clock of the headers ("10:15:27")
    double ttSeconds = -1;           // broadcast seconds between them (-1 = not known)
    std::string vitcFrom, vitcTo;    // VITC time code first / last
    double vitcSeconds = -1;         // broadcast seconds VITC ran
    int64_t vitcFrames = 0;          // recording frames between those two VITC readings
};
// frames = frames in the recording, fps = 25, line / field = where it was read
VpsFile vpsAnalyse(const std::vector<VpsReading>& r, int64_t frames, double fps, int line, int field, const VpsClocks* clocks = nullptr);

// A line in the VPS format (biphase 2.5 Mbit/s, the VPS start code) that is not the VPS label: the uplink's or the
// station's own data. Eurosport 20.01.98 (satellite): lines 7 / 320 the date and clock in BCD (bits as VPS sends
// them: reversed), "20 01 98 18 22 46"; lines 8 / 321 the text " EUROSPORT ". One frame's bytes:
struct VpsData { bool clock = false; int day = 0, month = 0, year = 0, hour = 0, minute = 0, second = 0, at = -1; std::string text; };
bool vpsDataOf(const uint8_t b[13], VpsData& out, int clockAt = -1);   // clockAt: the byte the date starts at (-1 = look)
std::string vpsDataText(const VpsData& d);                            // "20.01.98 18:22:46" / "\"EUROSPORT\""
// a whole recording's such line: what it carries, voted (one line for the line map) and the report lines;
// the clock's first and last reading for the time of the broadcast
struct VpsDataFile { std::string kind; std::vector<std::string> report; std::string clockFrom, clockTo, date; double seconds = -1; };
VpsDataFile vpsDataAnalyse(const std::vector<VpsReading>& r, int64_t frames, double fps, int line, int field);

bool wssRead(const float* y, int n, double fs, int& bits);   // bits b0..b13 (b0 = bit 0)
std::string wssText(int bits);
