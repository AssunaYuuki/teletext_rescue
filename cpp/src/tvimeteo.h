#pragma once
// ТВ-информ fТ3 (RTR) texts: the file catalogue of the inserter, WMO bulletins put together from the pieces
// of the lines, and METAR reports decoded into Russian.
//   A capture keeps 1 of the ~11 lines a field the service sends on, so the stream comes as separate 41-byte
//   pieces. File descriptors often arrive without their record header (it went on a line not captured):
//   attributes (20), 5 bytes, size u32, name length, name - "UFAA026.ub" 4199 bytes; the inserter's folder
//   comes as "c:\files". Bulletins are sent again and again, so pieces of one bulletin overlap in time.
#include <cstdint>
#include <string>
#include <vector>

struct Ft3Catalog {
    struct File { std::string name; uint32_t size = 0; int copies = 0; };
    std::vector<File> files;            // by name
    std::vector<std::string> folders;   // "c:\files"
};
// slots: the 41 data bytes of every fТ3 line with data, in time order
Ft3Catalog ft3Catalog(const std::vector<std::vector<uint8_t>>& slots);

// readable runs (8+ characters, bulletin text or plain English) of the slots; byte 40 of a line (6 bits) left out
std::vector<std::string> ft3Pieces(const std::vector<std::vector<uint8_t>>& slots);
// pieces joined where they overlap (tape errors allowed, characters voted between the copies); the longest first
std::vector<std::string> ft3JoinTexts(const std::vector<std::string>& pieces);

// the bulletins in the joined texts: heading "TTAAii CCCC YYGGgg" and its text; then every METAR / SPECI
// report of them decoded into Russian. "" when there is none
std::string ft3MeteoText(const std::vector<std::string>& texts, int* reports = nullptr);

// one METAR report ("EGBB 141750Z 05007KT 6000 OVC024 15/10 Q1028=") in Russian, "" if it is no METAR
std::string metarRu(const std::string& report);
