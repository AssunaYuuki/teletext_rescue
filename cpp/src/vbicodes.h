#pragma once
// Fixed service lines of 625-line television:
//   VPS (EN 300 231, line 16 of field 1): 15 bytes, biphase at 2.5 Mbit/s (5 MHz half-bits): run-in, start code,
//       then bytes 3..15 - source/control data, sound, the label (CNI of the network, PIL = announced start
//       of the programme or a special code) and the programme type
//   WSS (EN 300 294, line 23 of field 1): 14 bits, each sent as 6 elements at 5 MHz (biphase) - aspect ratio,
//       film/camera mode, subtitles, surround sound, copy protection
#include <cstdint>
#include <string>

// y = one line's samples (any scale), fs = samples per second. b = bytes 3..15 of the line (13)
bool vpsRead(const float* y, int n, double fs, uint8_t b[13]);
struct VpsLabel {
    int cni = 0;          // 12 bits, e.g. 0xD88
    int day = 0, month = 0, hour = 0, minute = 0;
    int pty = 0;          // programme type (byte 15)
    int sound = 0;        // byte 5 bits 7-6: 0 unknown, 1 mono, 2 stereo, 3 two channels
};
VpsLabel vpsLabel(const uint8_t b[13]);
std::string vpsCniName(int cni);         // "VIVA", "" if not in the register
std::string vpsText(const uint8_t b[13]);   // one line for people

bool wssRead(const float* y, int n, double fs, int& bits);   // bits b0..b13 (b0 = bit 0)
std::string wssText(int bits);
