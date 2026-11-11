#pragma once
// IBS data broadcasting of ITS ("IBDecoder", driver IBSD.vxd of TV-Inform St Petersburg's VBIplugin 1.04, 2001;
// the same system as ТЕЛЕИНФ on ОРТ): teletext packets X/31 (independent data lines, EN 300 708 format A).
// Taken from the driver (tools/ibsd) and checked on the ТЕЛЕИНФ tape:
//   packet: FT, IAL (Hamming 8/4), IAL address nibbles (SPA), RI byte if FT&2 (80/81/02 = the three copies),
//   CI byte if FT&4, DL byte if FT&8, data, CRC 2 bytes. CRC (per byte: c = T2[g(b ^ c&FF)] ^ c>>8, g = bit
//   reversal ^ bit 0, T2[v] = XOR of 8940h>>k over the bits k of v) runs from the byte after RI to the end:
//   with CI sent the remainder is 0, without it both remainder bytes equal the implicit CI. One wrong bit is
//   mended (the driver does it with a syndrome table). Channel = magazine (DC 8-B), service = SPA.
//   data of a channel in CI order = a byte stream: 10 10 = byte 10h, 10 X = a message starts, type X & C0:
//   00 open, 40 encrypted (2 bytes key number + seed, then "IBS" and the data XORed with an 8-byte keystream
//   from the key and the receiver's hardware key), 80 key (receiver number 3 bytes, key number, 8 key bytes),
//   FF break. The keys go to each subscriber's receiver by its number: without the hardware key the encrypted
//   data cannot be read.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct ItsChannel {
    int mag = 0, ft = 0;
    std::string spa;                       // address nibbles, hex
    int64_t packets = 0, good = 0, mended = 0, repeats = 0, lost = 0;
    int64_t first = -1, last = -1;          // frames
    std::map<std::string, int> types;       // message type -> count ("key", "open", "encrypted", "break", "broken")
    std::set<uint32_t> receivers;           // key messages: receiver numbers
    std::map<int, int> keyNumbers;          // key messages: key number -> count
    std::vector<std::vector<uint8_t>> open; // open messages (first ones)
    // stream state
    int prevCi = -1, candCi = -1;           // CI of the last packet taken / of a packet waiting to be confirmed
    bool esc = false, inMsg = false;
    int msgType = 0;
    std::vector<uint8_t> msg;
};

// InterTVNet (TV-Inform St Petersburg 2002-2003, WDM receiver VbiDump.ax of InterTVNet 7.30; channels РТР and
// «Петербург - 5 канал"): packets X/31 of magazine 8 in another format. The 40 bytes after the address are 5 BCH(63,51)
// codewords interleaved bit by bit (bit j + 5i, LSB first; parity bits 0-11, data 12-62; GF(64), p = 1 + x + x^6;
// up to 2 errors each) = 29 data bytes + CRC-16 (x^16+x^9+x^7+x^4+1 over bit-reversed bytes); the CI is not sent:
// received CRC (bytes bit-reversed, swapped) ^ computed CRC = an entry of a 256-word table. A file starts in a packet
// beginning 10 00 k 10 (k 1-3) and runs to the next start; a lone 10 is dropped (10 10 = 10). Then: 00, kind (1-2:
// C:\IBS\ROOT\S<n>\ - n is never set in VbiDump.ax, so S0 -, 3: the folder the access list gives the resource),
// flags, resource (LE16), name length, name, size (LE16 when flags & 10h, else LE32 two bytes on, the data two bytes
// earlier), data. Resource 0 "aclinfo.dat" is the access list: resources and groups open, the subscribers' records
// RC5-encrypted with the card's key. The files themselves are not encrypted.
struct ItvFile {
    int kind = 0, flags = 0, resource = 0;
    std::string name;
    uint32_t size = 0;
    std::vector<uint8_t> data;   // size bytes when complete, else what came
    bool complete = false;
    int copies = 0;              // complete copies received (the files repeat)
    int64_t first = -1;          // frame
};

// the InterTVNet access list (resource 0 "aclinfo.dat"; BDA_ACL_DecodeACL of acl_dll.dll, no encryption): AB BD,
// type (1 public, 2 personal), then records: 1 byte, kind (low 4 bits; bit 4 a flag of kind 1), count LE16, and
// count entries: kind 1 resource (number LE16, two lengths, two strings), kind 2 group (number LE16, count LE16, item
// size, the items: resource numbers), kind 3/4 card (code LE32, group LE16, 2 bytes). The receiver keeps the resources
// of the groups its card is in.
struct ItvAcl {
    int type = 0;
    // a: the folder under C:\IBS\ROOT\ (what BDA_ACL_GetResource gives VbiDump.ax), b: the second string
    struct Resource { std::string a, b; bool flag = false; };
    std::map<int, Resource> resources;
    std::map<int, std::vector<int>> groups;   // group 0: open to every receiver
    // subscribers' records (card -> group): 8 bytes each, RC5-32/12 encrypted with a key made from the card and
    // computer codes - without a card they cannot be read, so only counted
    int cards = 0;
    bool ok = false;               // the whole list read
    std::string folderOf(int resource) const;   // a folder name for the resource's files ("" = not known)
};
ItvAcl itvAclOf(const std::vector<uint8_t>& d);

class ItsIdl {
public:
    void packet(const uint8_t* p, int64_t frame);   // a teletext packet (42 bytes as sliced)
    void finish();                                   // closes the messages still open
    int64_t good() const;                            // packets with a right CRC
    bool found() const { return good() >= 20; }      // the service is there (not a few noisy packets)
    const std::map<std::string, ItsChannel>& channels() const { return ch_; }
    std::vector<std::string> report() const;         // lines for a report
    // InterTVNet
    bool itvFound() const { return itvGood_ >= 20; }
    int64_t itvPackets() const { return itvPackets_; }
    int64_t itvGood() const { return itvGood_; }
    int64_t itvCorrected() const { return itvCorrected_; }
    int64_t itvLost() const { return itvLost_; }
    const std::vector<ItvFile>& itvFiles() const { return itvFiles_; }

private:
    void message(ItsChannel& c, bool broken);
    bool itvPacket(const uint8_t* p, int64_t frame);
    void itvEnd();
    std::map<std::string, ItsChannel> ch_;
    int64_t itvPackets_ = 0, itvGood_ = 0, itvCorrected_ = 0, itvLost_ = 0, itvFrame_ = -1;
    int itvPrevCi_ = -1;
    bool itvIn_ = false;
    std::vector<uint8_t> itvMsg_;
    std::vector<ItvFile> itvFiles_;
};

ItsIdl itsIdlOf(const std::string& t42);   // a .t42 file through ItsIdl (frame = packet index / 20)
