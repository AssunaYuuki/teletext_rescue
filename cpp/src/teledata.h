#pragma once
// TeletextData Petersburg / ТхТ 2/200 (5 канал): files on magazine-8 pages whose subcode is 10xx.
// Rows 1-23 carry eight 7-bit bytes for every seven file bytes, by bit planes:
//   out[j] = sum(((input[i] >> j) & 1) << i), j=0..6, i=0..7.
// The first 18 rows are 630 file bytes; rows 19-23 are error-correction and service data. A catalogue
// on page 800 names each file, its exact size and its first page / number of pages. Blocks are ordered
// by subpage, then by page; a block beginning "N(DD" is a service record, not file contents.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

constexpr size_t kTeledataBlockBytes = 630;

struct TeledataBlock {
    int page = 0, sub = 0, copies = 0, seedRows = 0;
    std::array<uint8_t, kTeledataBlockBytes> data{};
    std::array<char, kTeledataBlockBytes> known{};
    std::array<std::vector<uint8_t>, kTeledataBlockBytes> alts;
    bool control = false, mixed = false;
    double entropy = 0;
    bool text = false;
};

struct TeledataEntry {
    std::string name;
    uint64_t size = 0;
    int firstPage = 0;       // low two BCD digits, as on the teletext page
    int pageCount = 0;
};

struct TeledataCatalog {
    std::vector<uint8_t> bytes;
    std::vector<TeledataEntry> files;
    int page = 0, sub = 0, copies = 0;
};

class TeledataPages {
public:
    void packet(const uint8_t* p);
    bool empty() const { return tx_.empty(); }
    size_t transmissions() const { return tx_.size(); }
    std::vector<TeledataBlock> blocks() const;
private:
    struct Tx {
        int page = 0, sub = 0;
        std::map<int, std::array<uint8_t, 40>> rows;
    };
    std::vector<Tx> tx_;
    int open_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

TeledataCatalog teledataCatalogue(const std::vector<TeledataBlock>& blocks);
TeledataPages teledataPagesOf(const std::string& t42);
int teledataNextBcd(int v);
