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
    // TV line (625: 1..625) of stored index 0 of each half of the frame, found from the field sync pulses
    // the file holds (sample.vbi: 622 / 309, the capture started before line 1); 0 = the format's numbering
    int tvFirst[2] = {0, 0};
    int frameBytes() const { return lineBytes * linesPerFrame; }   // a frame in memory (used lines)
    int64_t fileFrameBytes() const { return (int64_t)lineBytes * (storedPerField ? 2 * storedPerField : linesPerFrame); }
};

// TV line of stored line l (625 lines: 1..625; 525: 1..525)
inline int tvLineOf(const VbiGeometry& g, int l)
{
    int half = g.linesPerFrame / 2, h = l < half ? 0 : 1, total = g.pal ? 625 : 525;
    if (g.tvFirst[h] > 0) return (g.tvFirst[h] - 1 + l % half) % total + 1;
    return g.firstLine + l % half + (h ? (g.pal ? 313 : 263) : 0);
}
// field (1 / 2) and line number within it (field 2: TV line - 313 / 263) of stored line l
inline void lineOf(const VbiGeometry& g, int l, int& lineNo, int& field)
{
    int tv = tvLineOf(g, l), half2 = g.pal ? 313 : 263;
    field = tv > half2 ? 2 : 1;
    lineNo = tv > half2 ? tv - half2 : tv;
}
inline int lineNoOf(const VbiGeometry& g, int l) { int n, f; lineOf(g, l, n, f); return n; }
inline int fieldOf(const VbiGeometry& g, int l) { int n, f; lineOf(g, l, n, f); return f; }
// a stored line inside the field sync (625: 623-625, 1-5, 311-318) or in the picture (23-310, 336-622)
inline bool tvNotVbi(const VbiGeometry& g, int l)
{
    if (!g.pal || !g.tvFirst[0]) return false;
    int tv = tvLineOf(g, l);
    return tv >= 623 || tv <= 5 || (tv >= 311 && tv <= 318) || (tv >= 23 && tv <= 310) || (tv >= 336 && tv <= 622);
}

// one field's stored lines (count lines of lineBytes, samples used): the TV line of the first of them, found from
// the broad pulses of the field sync among them (625: lines 1-3 / 313-315, 525: 4-6 / 266-269); 0 = no field sync
// there. For a card left free running (no vertical lock) the window rolls through the frame.
int fieldSyncFirst(const uint8_t* lines, int count, int lineBytes, int samples, int fmt, bool pal);

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
    void numberBySync();
    FILE* f_ = nullptr;
    VbiGeometry g_;
    std::string path_;
    int64_t size_ = 0;
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
