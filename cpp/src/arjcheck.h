// ARJ archives checked as "arj t" / "7z t" does: every header by its CRC-32, every file unpacked in memory (methods
// 0 stored, 1-3 LZ + Huffman, 4 fast) and compared with its CRC-32. For files put together from a broadcast
// (Бизнес-Текст): whether an archive came through whole, and which files inside did.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ArjEntry {
    std::string name;
    uint32_t size = 0, packed = 0;
    int method = 0;
    bool ok = false;          // unpacked and its CRC-32 is right
    std::string why;          // when not: "header CRC", "data CRC", "encrypted", "cut off", ...
    uint32_t got = 0;         // bytes unpacked (all of size when whole; up to where the damage stopped it)
    std::vector<uint8_t> data;   // what unpacked, also from a damaged archive
};
struct ArjCheck {
    bool archive = false;     // starts as an ARJ archive (main header found)
    bool headerOk = false;    // the main header's CRC is right
    bool complete = false;    // the end of the archive reached
    std::vector<ArjEntry> files;
    int good() const { int n = 0; for (auto& f : files) n += f.ok; return n; }
    std::string summary() const;   // "ARJ: 5 of 5 files right (CRC)" / "ARJ: header damaged" / ...
};
ArjCheck arjCheck(const std::vector<uint8_t>& data);
// every file of the archive, whole or as far as it unpacks, into dir (made); names made safe; returns a list
// "NAME  size  ok / CRC wrong / N of M bytes" for the report
std::string arjExtract(const ArjCheck& c, const std::string& dir);
uint32_t crc32Arj(const uint8_t* p, size_t n, uint32_t crc = 0);
