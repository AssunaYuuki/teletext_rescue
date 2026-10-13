#pragma once
// ТВ-информ in packet form (late 1990s on, equipment by Norpak): NABTS-format packets
// 55 55 E7 + 33 bytes (3 Hamming 8/4 address nibbles, continuity index CI, packet structure PS,
// 28 bytes) at the NTSC NABTS rate 5.727 MHz on GOST lines 7-9 / 320-322 of ORT (SECAM).
//
// Error protection (found on the recordings): a product code over GF(256), polynomial 0x11D,
//   rows    = one packet: 2 check bytes (bytes 26, 27) + 26 data bytes,
//   columns = a bundle of 16 packets: 2 FEC packets (CI 14, 15, PS 12) + 14 data packets (CI 0..13),
// both with s0 = sum v[k] b^k = 0 and s1 = sum v[k] b^3k = 0, b = 0x1D, check symbols at k = 0, 1
// (the same code as Norpak's WaveTop). A bundle carries 14 x 26 = 364 bytes of the data stream.
//
// Content seen: ORT 2002 - only the source beacon (record d5 f3 c9 06, type 5: version word,
// time of the last update, source name "ОРТ, Москва и Область"), every 275 bytes, zeros between;
// ORT 1999 - the test transmission: a 16-packet carousel (PS 4) of 7-bit + odd parity bytes that
// are a sliding window over the PRBS s[k+8] = s[k] ^ s[k+3] (the receiver's "TEST" mode).
#include <cstdint>
#include <string>
#include <vector>

struct TviNorpak {
    int64_t packets = 0, bundles = 0, bundlesOk = 0, rowsFixed = 0, rowsErased = 0, testPackets = 0;
    std::vector<uint8_t> stream;               // data of the corrected bundles, in order
    std::vector<std::string> report;
};

// Reads <t33> (+ .pos), decodes, writes <outDir>/tvinform_stream.bin and tvinform_norpak.txt.
bool tviNorpakDecode(const std::string& t33, const std::string& outDir, TviNorpak& out);
