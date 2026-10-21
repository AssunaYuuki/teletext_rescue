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
// Stream records: D5 F3 C9, type, file id (u32), ?, block (u16), ?, length (u16), CRC-16/ARC of the data,
// CRC-16/ARC of bytes 3..16 (type .. data CRC), data.
// Type 6 = source beacon, 7 = file description (8.3 name, attributes, FILETIME created/written, size,
// u32, block size 256, block count, long name), 3 = file block (256 bytes, the last one the rest of the file), 1 = index (sent before the description, same
// file id; 27 x 6 bytes: 00 00, 01 AE / 01 AF, u16 - not sums or CRCs of block groups, not times; not decoded).
// Every slot (frame x line 7, 8, 9, 320, 321, 322) carries the next packet and the CI counts the slots, so
// with the .pos file the CI and bundle come from the slot (headers that lost bits still count). Bundles
// with more than 2 bad packets go through iterative row / column decoding; a file block no copy of which
// came intact is put together from its damaged copies (found 275 / 550 bytes from its neighbours) when the
// block CRC matches (rows of one copy mended from another under their own check bytes; a copy with only its
// marker hit is taken by its CRC, the data CRC also found from the header CRC).
// Content seen: ORT 2002 - the file PPUE98_RUMS.tif (a fax-coded newspaper page) and the source beacon (record d5 f3 c9 06, type 5: version word,
// time of the last update, source name "ОРТ, Москва и Область"), every 275 bytes, zeros between;
// ORT 1999 - the test transmission: a 16-packet carousel (PS 4) of 7-bit + odd parity bytes that
// are a sliding window over the PRBS s[k+8] = s[k] ^ s[k+3] (the receiver's "TEST" mode).
#include "tvinform.h"
#include <cstdint>
#include <string>
#include <vector>

struct TviNorpak {
    int64_t packets = 0, bundles = 0, bundlesOk = 0, rowsFixed = 0, rowsErased = 0, testPackets = 0, rowsKept = 0;
    std::vector<uint8_t> stream;               // data of every bundle, in order (364 bytes each)
    std::vector<uint8_t> valid;                // per byte of stream: 1 = intact (row check passed / bundle corrected)
    std::vector<uint8_t> checks;               // per 26-byte row of stream: its packet's 2 check bytes, 1 if the packet was read
    std::vector<TviFile> files;
    std::vector<std::string> beacons;          // source beacon: "name"  updated <time>  version <hex>  - <n> times
    std::vector<std::string> report;
};

// Reads <t33> (+ .pos), decodes, writes <outDir>/tvinform_stream.bin and tvinform_norpak.txt.
bool tviNorpakDecode(const std::string& t33, const std::string& outDir, TviNorpak& out);
