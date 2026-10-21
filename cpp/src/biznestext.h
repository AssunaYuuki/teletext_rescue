#pragma once
// «Бизнес-Текст» (ГЦРТ, 8th channel Moscow / НТВ, 1996, decoder "Dekoder TT50x"): files for PCs announced
// in teletext. The catalogue goes round on pages 600-602 (and 6B0/6B2; on 9.12.94 pages 500-509 and
// 600-6FF, magazines 5 and 6 sent in parallel; on 9.07.94 also 7xx with blocks 101+): each transmission is a header + rows 21-23 in Hamming 8/4,
// nibble pairs, low nibble first:
//   row 21: AA AA AA AA (FF FF FF FF = file sent open), 8.3 file name (11 characters, no dot), 00,
//           file size as a*10000 + b*100 + c (three bytes 0-99), 01
//   row 22: 00 <blocks hi> <blocks lo> 00 00 <block being sent now>, date DDMMYY + time HHMMSS as digits,
//           file number, type (01 binary, 04 open test, 05 text, 13 .COD)
//   row 23: 4 bytes: in 1994 the sum of the 800 plain bytes of this block (00, a, b, c = a*10000+b*100+c),
//           in 1996 the same for every block of a file; then 16 bytes the same for every file
// Packets 8/31 (independent data lines) carry the data channel; in the 1996 tape only its service descriptor
// "Test 3x Packet F. CORO Ltd. 1995,96" (service information sent in the signal, not receiver firmware).
// The file blocks themselves are rows 1-20 of the catalogue transmission (8-bit bytes, 800 a block, the
// block number is bytes 3-5 of row 22 in decimal: 00 01 00 = 100). They are encrypted with a key of one byte
// per position in the block (9.07.94: the same 800 bytes for every block and file), found from the recording
// itself by the statistics of the text (writeFiles). 1996: every file byte is two Hamming 8/4 codewords spread
// down the columns of the block (400 bytes a block), each codeword XORed with its own key byte (biznestext.cpp).
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct BtFile {
    std::string name;          // 8.3, upper case
    std::string page;          // catalogue page it was announced on (most often)
    std::string date;          // DD.MM.YY hh:mm:ss of the file
    std::string check;         // the 4 bytes of row 23, hex
    long size = -1;            // file size from the catalogue (bytes)
    int copies = 0, blocks = -1;
    std::set<int> sent;        // block numbers the catalogue said were being sent
    int64_t first = -1, last = -1;   // frames
};

std::string utf8Of866(uint8_t x);   // one byte of DOS text (CP866) as UTF-8

struct BtWritten {
    std::string name, path, text;      // the file and its text as UTF-8 ("" if not text)
    int blocks = -1, blocksHave = 0;   // in the catalogue / with at least one row received
    int rows = 0, rowsHave = 0;        // rows of the blocks up to the last one received
    int rowsBad = 0;                   // rows of a text file that did not read as text (left out)
    int blockBytes = 800;              // file bytes a block (1996: 400, each byte two codewords)
    long bytesBad = 0;                 // bytes of the blocks received that did not come or did not read
    std::vector<long> unread;          // their positions in the file (with the blocks not received)
    // 1994: blocks whose bytes as sent add up to the catalogue's sum (row 23) / made so by taking another copy's
    // byte where the copies differ / that do not
    int sumRight = 0, sumMended = 0, sumWrong = 0;
    bool decrypted = false;
    // how much of the file is there and whether it checks: ARJ unpacked and every CRC-32 compared (arjcheck.cpp),
    // PCX decoded line by line, a text by the bytes that read
    double readPct = 0;                // bytes of the file received and readable, %
    std::string check;                 // "ARJ: 1 of 3 files right (CRC)", "PCX 640x350: 980 of 1400 lines whole", ...
    bool checkOk = false;              // the file checks whole
    // what the received blocks look like: "compressed" = the service's own data (packed, bytes nearly
    // uniform); "disk leftovers" = old contents of the sender's disk inside a file made at its size
    // (9.07.94: dictionaries, RemoteAccess BBS, Windows - its directory lists these very files)
    std::string content;
};

class BtCatalog {
public:
    // every teletext packet (42 bytes, as sliced, parity not checked) in order; frame = when
    void packet(const uint8_t* p, int64_t frame);
    // how much the next packets' data rows are trusted when copies differ: a live capture's 8-bit packets (.raw.t42)
    // more than the file decoder's stream, whose parity repair spoils 8-bit bytes
    void setWeight(int w) { weight_ = w; }
    std::vector<BtFile> files(int minCopies = 3) const;   // most announced first
    int64_t dataPackets() const { return idl_; }          // packets 8/31
    std::string dataText() const;                         // their text, character by character voted
    int64_t firstData() const { return idlFirst_; }
    bool empty() const { return entries_.empty() && !idl_; }
    bool hasData() const { return !data_.empty(); }
    long dataRows() const;   // data rows received (copies counted)
    int filesWithData() const { return (int)data_.size(); }
    // the transmission heard last: "AKC.C01 block 57 (page 602)"; frame = when ("" = none yet)
    std::string nowSent(int64_t* frame = nullptr) const { if (frame) *frame = nowFrame_; return nowSent_; }
    // another recording of the same transmission (a file of the same name and date in both catalogues)
    bool sameFiles(const BtCatalog& o) const;
    void merge(const BtCatalog& o);   // its catalogue entries and block copies join these
    // assembles the files from their blocks (each byte voted over the copies), finds the key and writes
    // <dir>/<name> (+ <name>.txt when it is text); note = how the key was found
    // status: what is being done now, for a progress window ("decrypting and writing AKC.C01 (3 of 19)")
    std::vector<BtWritten> writeFiles(const std::string& dir, std::string& note,
                                      const std::function<void(const std::string&)>& status = nullptr) const;

private:
    struct Entry {
        int n = 0;
        std::map<int, int> blocks;
        std::map<std::string, int> date, check, page;
        std::map<long, int> size;
        std::map<int, std::map<long, int>> blockSum;   // 1994: block -> the sum of its 800 bytes as sent (row 23)
        std::set<int> sent;
        int64_t first = -1, last = -1;
    };
    struct Cur { std::string page; std::map<int, std::vector<uint8_t>> rows, data; int last = 0; bool on = false; };
    using Row40 = std::array<uint8_t, 40>;
    struct Copy { Row40 b; int w; };
    int weight_ = 1;
    static constexpr int kEnoughCopies = 5;   // copies of a data row kept (more only repeat it)
    // files received whole (every block of the catalogue's count, every row in two agreeing copies): their
    // further transmissions are not looked at any more
    std::set<std::string> done_;
    bool whole(const std::string& name) const;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::map<int, std::map<int, std::vector<Copy>>>> data_;   // file -> block -> row 1-20 -> copies
    Cur cur_[9];   // per magazine (5 and 6 carry the catalogue, sent in parallel)
    int64_t idl_ = 0, idlFirst_ = -1;
    std::string nowSent_;
    int64_t nowFrame_ = -1;
    std::vector<std::map<uint8_t, int>> idlVote_;
};

// a .t42 file through BtCatalog (frame = packet index / 20)
BtCatalog btCatalogOf(const std::string& t42, int weight = 1);
