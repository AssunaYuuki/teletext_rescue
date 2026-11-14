#pragma once
// BBC Ceefax telesoftware (1983-89): BBC BASIC programs sent as teletext pages. A block = one page: row 1 starts
// |B<type 2><block, hex 2><?><blocks, hex 2>|a1<length><name>|i..|s~..|s~Z, then the program text for *EXEC
// (keywords abbreviated, 0x07 ends a line) to |c, the rest padded with 02. Escapes: || = |, |` = code 96,
// |@ .. |_ = control codes.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Find;

struct TsProgram {
    std::string name;
    int blocks = 0;                  // in the program
    std::vector<int> copies;         // copies received of each block (index = block - 1)
    std::vector<int> page;           // the page each block came on (hex as decimal 7xx)
    std::string exec;                // the text for BASIC: lines ended by CR, escapes resolved
    std::string listing;             // readable: CR LF, missing blocks marked
    int have() const { int n = 0; for (int c : copies) n += c > 0; return n; }
    bool complete() const { return blocks > 0 && have() == blocks; }
};

// packets one by one (a recording's stream, the live decoder): the blocks of telesoftware kept, programs made of them
class TsCollector {
public:
    TsCollector();
    void packet(const uint8_t* p);           // 42 bytes: MRAG + 40
    size_t blocksSeen() const;               // copies of blocks kept so far (grows as they come)
    std::vector<TsProgram> programs() const; // copies of a block voted byte by byte
    bool empty() const { return blocksSeen() == 0; }
private:
    struct Impl;
    std::shared_ptr<Impl> d_;
};
// the telesoftware programs in a teletext packet stream (.t42)
std::vector<TsProgram> telesoftOf(const std::string& t42);
// telesoftware escapes resolved (|| -> |, |` -> `, |@.._ -> 0..31)
std::string telesoftUnescape(const std::string& s);
// a program file for the BBC window: <dir>/<name>.bbc (*EXEC text) and <name>.txt (listing); finds for them
// write = false: the files are not written again (live, between new blocks), only pointed to when they are there
void addTelesoftFinds(std::vector<Find>& out, const std::vector<TsProgram>& progs, const std::string& where, const std::string& dir,
                      bool write = true);
