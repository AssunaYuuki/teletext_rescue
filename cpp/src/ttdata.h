#pragma once
// Teletext pages that carry data, not text: every byte of their rows is a Hamming 8/4 codeword, two of them
// make one data byte (low nibble first). ТЕЛЕИНФ (ORT, 1995 tape and capture_2026-10-15_003002) sends two
// such pages with the display inhibited (C10): 790 (subcode 2000) - header A1 A1 05 95 07, then zero
// bytes; 795 (subcode 3000) - header 05 C0 16 50 00 00 2E 34, then text lines "\P 5 811121988", a table
// "0 123 ... 5 128" and "66666666", lines ending in CR LF (a receiver test table).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct TtDataPage {
    int page = 0, sub = 0;               // 0x795, 0x3000
    int copies = 0;
    bool inhibit = false;                // C10: not for display
    std::string headerText;              // the header row as sent ("Teleinf")
    std::vector<uint8_t> head;           // the bytes of row 1
    std::vector<uint8_t> data;           // rows 2.. one after another (padding left out)
    int unread = 0;                      // bytes no copy read (shown as '?')
    std::vector<std::string> text;       // data as text lines, when it is text
    bool zeros = false;                  // data all zero bytes
};

class TtDataPages {
public:
    void packet(const uint8_t* p);       // every teletext packet (42 bytes) in order
    std::vector<TtDataPage> pages() const;
    bool empty() const { return tx_.empty(); }
private:
    struct Tx { int page, sub; bool inhibit; std::string header; std::map<int, std::vector<uint8_t>> rows; int sent = 0; };
    std::vector<Tx> tx_;
    int open_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // per magazine: index in tx_
};

TtDataPages ttDataPagesOf(const std::string& t42);
