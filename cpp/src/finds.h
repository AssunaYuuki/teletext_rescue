#pragma once
// Everything a recording (or a live capture) brought that is a file or a text of its own: files sent through
// ТВ-информ, files announced by «Бизнес-Текст», documents on ТВ-информ lines, data-channel messages - with
// where (service, lines) and when they were found. One list for the "Found files and texts" window.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct DecodeResult;
struct TviStream;
class BtCatalog;
class TtDataPages;

struct Find {
    enum Kind { File, Announced, Text, Test } kind = File;   // Announced = listed in a catalogue, data not received
    std::string name, shortName;     // long and 8.3 names
    std::string type;                // what the file is (by its contents or extension)
    std::string service;             // "ТВ-информ (Norpak packets)", "Бизнес-Текст (teletext, НТВ)", ...
    std::string where;               // lines
    std::string when;                // date of the file / the transmission
    std::string status;              // "complete", "386 of 393 blocks", ...
    uint64_t size = 0;
    int blocks = 0, have = 0;
    int blockBytes = 256;            // size of one block (Norpak 256, «Бизнес-Текст» 800)
    std::string path;                // on disk ("" = not written)
    std::string note;                // one line more (what was made of it, missing blocks)
    std::vector<std::string> text;   // a text: its lines
    std::vector<long> unread;        // positions in the file of the bytes not received / not readable (when known)
};

// a received file as Windows can open it: <file>.txt (DOS text as UTF-8), .png (PCX, fax TIFF), or a copy
// named by what it is (.arj .zip .rar .gif .jpg .doc .rtf ...); "" = nothing known to open it with
// refresh: made again (the file was written again with more blocks); unread = positions of the bytes that did not
// come or did not read (a PCX picture is put together again around them)
std::string openableCopy(const std::string& path, bool refresh = false, const std::vector<long>* unread = nullptr);
std::string fileTypeOf(const std::string& path, const std::string& name);   // "TIFF image, fax G4", "ARJ archive", ...
// what is being done now ("«Бизнес-Текст»: decrypting and writing AKC.C01 (3 of 19)"), for a progress window
using FindStatus = std::function<void(const std::string&)>;
// the finds of a recording: the ones prepareFinds made, else made now (and the «Бизнес-Текст» files written again)
std::vector<Find> findsOf(const DecodeResult& r);
// made once, after the decoding (in its task, telling what it does), and kept in r.finds
void prepareFinds(DecodeResult& r, const FindStatus& status);
std::vector<Find> findsCompute(const DecodeResult& r, const FindStatus& status);
// pieces shared with the live capture
void addTviFinds(std::vector<Find>& out, const TviStream& t, const std::string& where);
// dir: where the received files are written ("" = only the catalogue)
void addBtFinds(std::vector<Find>& out, const BtCatalog& c, const std::string& where, const std::string& dir = "",
                const FindStatus& status = nullptr);
// the same with files written earlier (live: written in a thread of their own)
struct BtWritten;
void addBtFinds(std::vector<Find>& out, const BtCatalog& c, const std::string& where, const std::vector<BtWritten>& written,
                const std::string& keyNote);
void addTtDataFinds(std::vector<Find>& out, const TtDataPages& t, const std::string& where);   // Hamming 8/4 data pages
