#pragma once
// ТВ-информ 1998 (ИТАР-ТАСС, «Российская газета», «Сегодня», «Независимая газета», Moscow News ...): the files of the
// Norpak-26 container (tvinform_files/encoded/*.encoded.bin) deciphered.
//
// Format of a payload (found on the ORT recordings of 1998, see test\decoded\_README.txt):
//   cells of 26 bytes: 25 data bytes + a sum byte (sum of the 25 mod 256); a cell whose sum is wrong is dropped;
//   frames of 16 cells: header 01 01 <frame number, 2 bytes> <data length, 2 bytes> (0x188 = 392, less in the last
//   frame), a 2-byte trailer before the next header; frame 0 of a document (length 7C/17, then 37 0C) is markup;
//   2 cells of a frame are garbage (their sum fails) and 50 data bytes are cut out, once every 14 cells, at a phase
//   that follows the place of the file in the stream (the end of a Norpak bundle: 14 rows of 26 bytes) or, when the
//   stream place is not known or does not hold, is found from the text (words broken at the cut);
//   text in CP866 under a substitution: cipher = S[(byte + k) mod 256], S one table for the whole service
//   (recovered by cryptanalysis), k one per document (a file may hold several, the border being a frame 0);
//   ARJ archives sent the same way: k found by the CRC-32 of their headers; one file may carry the end of a volume
//   and the start of the next (each with its own k).
// The key, language, documents and phase of every file are found here (3-gram models of Russian and English and a
// dictionary taken from the texts deciphered in 2026, norpak26_model.inc), so a new recording needs no table.
#include <cstdint>
#include <string>
#include <vector>

struct Norpak26Input {
    std::string name;              // the file name as sent (e.g. 00017137.ATV, RG14S23, Z9803062.A0e1)
    std::vector<uint8_t> raw;      // the payload exactly as sent (.encoded.bin)
    int64_t streamOffset = -1;     // where it starts in the data stream (tvinform_stream.bin), -1 when not known
};

struct Norpak26Result {
    std::string name;
    std::string kind;              // "text", "arj", "raw" (not CP866 text: kept as bytes), "empty", "" (not deciphered)
    int k = -1;                    // key (of the main document)
    std::string lang;              // "ru" / "en"
    std::string segments;          // documents with their own key: "cell:k:lang ..." (empty for one document)
    int phase = -1;                // phase of the cut-out bytes (cell index mod 14), -1 not known
    std::string phaseHow;          // "text", "stream place", "duplicates"
    int gaps = 0, filled = 0;      // places of cut-out bytes marked "[…]" / filled from another file with the same text
    std::vector<std::string> lines;   // the text (UTF-8)
    std::string title, source;
    std::string path;              // what was written
    std::string note;
};

// Deciphers all files of one recording (or one folder), writes into outDir:
//   <name>.txt (UTF-8, CRLF), _archive_ARJ\<volume> (+ <volume>_files\ what unpacks), _raw\<name>.bin,
//   _ОГЛАВЛЕНИЕ.txt (table of contents), _norpak26_params.txt (key, language, phase of every file).
// Report lines are appended to report.
std::vector<Norpak26Result> norpak26DecodeAll(const std::vector<Norpak26Input>& in, const std::string& outDir,
                                              std::vector<std::string>& report);
