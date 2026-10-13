#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vbifile.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <cstring>

namespace {

constexpr double kLineRate = 15734.264; // NTSC fH
constexpr double kLineRatePal = 15625.0;

struct FormatInfo {
    VbiFormat f;
    const char* key;
    const char* name;
    int fmt;               // 0 = u8, 1 = u16 LE
    int samples;           // stored per line
    double rate;           // sampling rate, Hz
    int linesPerField;
    int firstLine;
    bool pal = false;
    int storedPerField = 0; // full-field files: lines stored per field
    int skip = 0;           // stored lines before the first used one
};

const FormatInfo kFormats[] = {
    {VbiFormat::Tbc, "tbc", "TBC VBI, vhs-decode/ld-decode (4fsc, 910 x u16)", 1, 910, 14318181.8, 16, 9},
    {VbiFormat::Bt8x8, "bttv", "bttv Bt848/Bt878 (28.6 MHz, 2048 x u8, 16 lines)", 0, 2048, 28636363.6, 16, 10},
    {VbiFormat::Cx88, "cx88", "Conexant CX2388x (28.6 MHz, 2048 x u8, 12 lines)", 0, 2048, 28636363.6, 12, 10},
    {VbiFormat::Saa7134, "saa7134", "Philips SAA7134 (27 MHz, 2048 x u8, 12 lines)", 0, 2048, 27000000.0, 12, 10},
    {VbiFormat::Cx23885, "cx23885", "CX23885/CX231xx (27 MHz, 1440 x u8, 12 lines)", 0, 1440, 27000000.0, 12, 10},
    {VbiFormat::Em28xx, "em28xx", "Empia EM28xx (13.5 MHz, 720 x u8, 12 lines)", 0, 720, 13500000.0, 12, 10},
    {VbiFormat::Ivtv, "ivtv", "ivtv/cx18 CX2341x (27 MHz, 1444 x u8, 12 lines)", 0, 1444, 27000000.0, 12, 10},
    {VbiFormat::BttvPal, "bttv-pal", "bttv Bt848/Bt878 PAL (35.5 MHz, 2048 x u8, 16 lines)", 0, 2048, 35468950.0, 16, 7, true},
    {VbiFormat::Cx88Pal, "cx88-pal", "Conexant CX2388x PAL (35.5 MHz, 2048 x u8, 18 lines)", 0, 2048, 35468950.0, 18, 6, true},
    {VbiFormat::Cx23885Pal, "cx23885-pal", "CX23885/CX231xx PAL (27 MHz, 1440 x u8, 18 lines)", 0, 1440, 27000000.0, 18, 6, true},
    {VbiFormat::Em28xxPal, "em28xx-pal", "Empia EM28xx PAL (13.5 MHz, 720 x u8, 18 lines)", 0, 720, 13500000.0, 18, 6, true},
    {VbiFormat::Saa7134x17, "saa7134-17", "Philips SAA7134 (27 MHz, 2048 x u8, 17 lines)", 0, 2048, 27000000.0, 17, 10},
    {VbiFormat::Saa7134Pal, "saa7134-pal", "Philips SAA7134 PAL (27 MHz, 2048 x u8, 17 lines)", 0, 2048, 27000000.0, 17, 7, true},
    {VbiFormat::Saa713xPal, "saa713x-pal", "Philips SAA713x PAL (27 MHz, 1440 x u8, 16 lines)", 0, 1440, 27000000.0, 16, 7, true},
    {VbiFormat::IvtvPal, "ivtv-pal", "ivtv/cx18 CX2341x PAL (27 MHz, 1444 x u8, 18 lines)", 0, 1444, 27000000.0, 18, 6, true},
    {VbiFormat::TbcPal, "tbc-pal", "TBC VBI PAL, vhs-decode/ld-decode (4fsc, 1135 x u16)", 1, 1135, 17734475.0, 16, 7, true},
    {VbiFormat::TbcFull, "tbc-full", "TBC full fields NTSC, ld-decode (4fsc, 263 x 910 x u16)", 1, 910, 14318181.8, 16, 9, false, 263, 8},
    {VbiFormat::TbcFullPal, "tbc-full-pal", "TBC full fields PAL, ld-decode (4fsc, 313 x 1135 x u16)", 1, 1135, 17734475.0, 18, 6, true, 313, 5},
};

const FormatInfo* info(VbiFormat f)
{
    for (auto& i : kFormats)
        if (i.f == f) return &i;
    return nullptr;
}

} // namespace

const char* formatName(VbiFormat f)
{
    const FormatInfo* i = info(f);
    return i ? i->name : "unknown";
}

const char* formatKey(VbiFormat f)
{
    const FormatInfo* i = info(f);
    return i ? i->key : "auto";
}

VbiFormat formatFromKey(const std::string& k)
{
    for (auto& i : kFormats)
        if (k == i.key) return i.f;
    return VbiFormat::Unknown;
}

VbiGeometry formatGeometry(VbiFormat f)
{
    VbiGeometry g;
    const FormatInfo* i = info(f);
    if (!i) return g;
    g.format = f;
    g.fmt = i->fmt;
    g.samplesPerLine = i->samples;
    g.validSamples = i->samples;
    g.lineBytes = i->samples * (i->fmt ? 2 : 1);
    g.linesPerFrame = i->linesPerField * 2;
    g.pal = i->pal;
    g.lineRate = i->pal ? kLineRatePal : kLineRate;
    g.fastRate = i->pal ? 444 : 364;
    g.lineSamples = (float)(i->rate / g.lineRate);
    g.firstLine = i->firstLine;
    g.storedPerField = i->storedPerField;
    g.skipLines = i->skip;
    return g;
}

std::vector<VbiFormat> candidateFormats(int64_t size)
{
    std::vector<VbiFormat> out;
    for (auto& i : kFormats) {
        VbiGeometry g = formatGeometry(i.f);
        // allow a trailing partial frame (captures stopped mid-frame)
        if (size >= g.fileFrameBytes() && (size % g.fileFrameBytes()) % g.lineBytes == 0) out.push_back(i.f);
    }
    return out;
}

// widen() and narrow() live in ui/util.cpp

VbiFile::~VbiFile() { close(); }

void VbiFile::close()
{
    if (f_) fclose(f_);
    f_ = nullptr;
}

bool VbiFile::open(const std::string& path, std::string& err, VbiFormat force)
{
    close();
    path_ = path;
    f_ = _wfopen(widen(path).c_str(), L"rb");
    if (!f_) { err = "cannot open file"; return false; }
    _fseeki64(f_, 0, SEEK_END);
    size_ = _ftelli64(f_);
    _fseeki64(f_, 0, SEEK_SET);

    VbiFormat fmt = force;
    if (fmt == VbiFormat::Unknown) {
        auto c = candidateFormats(size_);
        fmt = c.empty() ? VbiFormat::Tbc : c[0];
    }
    g_ = formatGeometry(fmt);
    if (g_.fmt == 0) {
        // 8-bit chips pad each line with zeros after the sampled window; use
        // the median per-line length so padding stays out of level estimates
        std::vector<uint8_t> fr(g_.frameBytes());
        if (fread(fr.data(), 1, fr.size(), f_) == fr.size()) {
            std::vector<int> last;
            for (int l = 0; l < g_.linesPerFrame; l++) {
                int i = g_.samplesPerLine - 1;
                while (i > 0 && !fr[(size_t)l * g_.lineBytes + i]) i--;
                if (i > g_.samplesPerLine / 2) last.push_back(i);
            }
            if (!last.empty()) {
                std::sort(last.begin(), last.end());
                int v = last[last.size() / 2];
                if (v < g_.samplesPerLine - 16) g_.validSamples = v - 8;
            }
        }
        _fseeki64(f_, 0, SEEK_SET);
    }
    g_.frames = size_ / g_.fileFrameBytes();
    if (g_.frames <= 0) { err = "file too small"; close(); return false; }
    return true;
}

int64_t VbiFile::read(int64_t first, int64_t count, std::vector<uint8_t>& buf)
{
    if (!f_ || first >= g_.frames) return 0;
    if (first + count > g_.frames) count = g_.frames - first;
    buf.resize((size_t)(count * g_.frameBytes()));
    _fseeki64(f_, first * g_.fileFrameBytes(), SEEK_SET);
    if (!g_.storedPerField) {
        size_t got = fread(buf.data(), 1, buf.size(), f_);
        return (int64_t)(got / g_.frameBytes());
    }
    // full fields: keep the VBI lines of each field
    std::vector<uint8_t> fr((size_t)g_.fileFrameBytes());
    const int used = g_.linesPerFrame / 2;
    int64_t n = 0;
    for (; n < count; n++) {
        if (fread(fr.data(), 1, fr.size(), f_) != fr.size()) break;
        for (int fld = 0; fld < 2; fld++)
            memcpy(&buf[(size_t)n * g_.frameBytes() + (size_t)fld * used * g_.lineBytes],
                   &fr[((size_t)fld * g_.storedPerField + g_.skipLines) * g_.lineBytes], (size_t)used * g_.lineBytes);
    }
    return n;
}
