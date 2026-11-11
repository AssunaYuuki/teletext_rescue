#pragma once
// RAR archives (format of RAR 2.9-4.x, "RAR3"): the files unpacked, written from the format (not from UnRAR's
// sources, whose licence does not fit GPL). Packing methods: stored, LZ with Huffman tables, PPMd (var. H) blocks,
// the standard filters of the RAR virtual machine (E8, E8E9, DELTA, RGB, AUDIO; recognised by the CRC of their code).
// Solid archives are read in order. Not read: encrypted files, other VM programs, the older RAR 1.5 / 2.0 methods.
#include <cstdint>
#include <string>
#include <vector>

struct RarEntry {
    std::string name;            // as UTF-8 (Unicode name when stored, else the OEM name as CP866)
    uint64_t size = 0;
    uint32_t crc = 0;
    int version = 0, method = 0;
    bool dir = false, encrypted = false, solid = false;
    bool ok = false;             // unpacked and the CRC matches
    std::string error;           // why not
    std::vector<uint8_t> data;
};

// false + err when it is not a RAR archive; entries that could not be unpacked carry their error
bool rarExtract(const std::vector<uint8_t>& archive, std::vector<RarEntry>& out, std::string& err);
