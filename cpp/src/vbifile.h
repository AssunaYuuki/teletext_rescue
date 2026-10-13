#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Raw VBI capture layouts (NTSC). Parameters follow the Linux V4L2 drivers
// (drivers/media/...: bttv-vbi.c, cx88-vbi.c, saa7134-video.c,
// cx23885-vbi.c, cx231xx-vbi.h, em28xx-video.c, ivtv-ioctl.c, cx18-ioctl.c)
// and vhs-decode / ld-decode for TBC exports.
enum class VbiFormat {
    Unknown,
    Tbc,      // vhs-decode/ld-decode TBC VBI: 4fsc 14.318 MHz, 910 x u16 LE, 16 lines/field
    Bt8x8,    // bttv Bt848/Bt878: 8fsc 28.636 MHz, 2048 x u8, 16 lines/field
    Cx88,     // Conexant CX2388x: 28.636 MHz, 2048 x u8, 12 lines/field
    Saa7134,  // Philips SAA7130/7134: 27 MHz, 2048 x u8, 12 lines/field
    Cx23885,  // CX23885/CX231xx: 27 MHz, 1440 x u8, 12 lines/field
    Em28xx,   // Empia EM28xx: 13.5 MHz, 720 x u8, 12 lines/field
    Ivtv,     // CX23415/16 (ivtv), CX23418 (cx18): 27 MHz, 1444 x u8 (4 byte SAV), 12 lines/field
    // 625-line (PAL/SECAM) captures
    BttvPal,    // bttv: 35.47 MHz, 2048 x u8, 16 lines/field from line 7
    Cx88Pal,    // CX2388x: 35.47 MHz, 2048 x u8, 18 lines/field from line 6
    Cx23885Pal, // CX23885/CX231xx: 27 MHz, 1440 x u8, 18 lines/field from line 6
    Em28xxPal,  // EM28xx: 13.5 MHz, 720 x u8, 18 lines/field from line 6
    // added 2026-10-08 (Linux drivers and the original Teletext Rescue table)
    Saa7134x17,    // saa7134.h VBI_LINE_COUNT 17 x VBI_LINE_LENGTH 2048, 27 MHz, NTSC from line 10
    Saa7134Pal,    // the same, PAL from line 7
    Saa713xPal,    // saa713x PAL: 1440 x u8 at 27 MHz, 16 lines/field from line 7
    IvtvPal,       // ivtv/cx18 PAL: 1444 x u8 at 27 MHz (4 byte SAV), 18 lines/field from line 6
    TbcPal,        // vhs-decode/ld-decode TBC VBI crop PAL: 4fsc 17.73 MHz, 1135 x u16, 16 lines from line 7
    TbcFull,       // ld-decode .tbc full fields NTSC: 263 lines x 910 u16 per field (VBI lines 9-24 used)
    TbcFullPal,    // ld-decode .tbc full fields PAL: 313 lines x 1135 u16 per field (lines 6-23 used)
    Count
};

struct VbiGeometry {
    VbiFormat format = VbiFormat::Unknown;
    int fmt = 0;            // 0 = u8, 1 = u16 LE (slicer argument)
    int samplesPerLine = 0; // stored samples per line
    int validSamples = 0;   // samples carrying signal
    int lineBytes = 0;
    int linesPerFrame = 32;
    float lineSamples = 0;  // samples per full line period (63.556 us)
    int firstLine = 9;      // line number of index 0 in field 1
    bool pal = false;       // 625-line system
    double lineRate = 15734.264; // fH, Hz
    double fastRate = 364;  // teletext/NABTS bit rate in multiples of fH (WST 625: 444)
    int64_t frames = 0;
    int storedPerField = 0;  // lines stored per field in the file (0 = linesPerFrame / 2)
    int skipLines = 0;       // stored lines before the first used one, per field
    int frameBytes() const { return lineBytes * linesPerFrame; }   // a frame in memory (used lines)
    int64_t fileFrameBytes() const { return (int64_t)lineBytes * (storedPerField ? 2 * storedPerField : linesPerFrame); }
};

const char* formatName(VbiFormat f);
const char* formatKey(VbiFormat f);        // short name for --format
VbiFormat formatFromKey(const std::string& k);
VbiGeometry formatGeometry(VbiFormat f);
// Formats whose frame size divides the file size (best guesses first).
std::vector<VbiFormat> candidateFormats(int64_t fileSize);

class VbiFile {
public:
    ~VbiFile();
    bool open(const std::string& path, std::string& err, VbiFormat force = VbiFormat::Unknown);
    void close();
    const VbiGeometry& geom() const { return g_; }
    int64_t size() const { return size_; }
    // Reads frames [first, first+count) into buf; returns frames read.
    int64_t read(int64_t first, int64_t count, std::vector<uint8_t>& buf);
    const std::string& path() const { return path_; }

private:
    FILE* f_ = nullptr;
    VbiGeometry g_;
    std::string path_;
    int64_t size_ = 0;
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
