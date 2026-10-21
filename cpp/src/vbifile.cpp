#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vbifile.h"
#include "ui/util.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <map>
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
    // <recording>.lines: the first VBI line when the capture asked the card for other lines (live: Lines...)
    if (FILE* lf = _wfopen(widen(path + ".lines").c_str(), L"rb")) {
        int first = 0;
        if (fscanf(lf, "%d", &first) == 1 && first > 0 && first < 40) g_.firstLine = first;
        fclose(lf);
    }
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
    numberBySync();
    return true;
}

// 625 lines: when the file holds the field sync, the lines are numbered from it. Broad pulses (sync level
// over most of a half line): line 1, 2 = both halves, 3 = first half; 313 = second half, 314, 315 = both
void VbiFile::numberBySync()
{
    if (!g_.pal || g_.storedPerField || g_.frames < 20) return;
    const int L = g_.linesPerFrame, half = L / 2, n = std::max(8, g_.validSamples);
    std::vector<uint8_t> buf;
    std::vector<int> levels;
    const int frames = 24;
    int got = 0;
    for (int k = 0; k < frames; k++) {
        if (read(g_.frames * (k + 1) / (frames + 2), 1, buf) < 1) continue;
        got++;
        auto at = [&](int l, int i) { const uint8_t* p = &buf[(size_t)l * g_.lineBytes]; return g_.fmt ? (p[2 * i] | p[2 * i + 1] << 8) >> 8 : p[i]; };
        for (int l = 0; l < L; l++)
            for (int i = 0; i < n; i += 4) levels.push_back(at(l, i));
    }
    if (got < frames / 2 || levels.empty()) return;
    std::vector<int> s = levels;
    std::sort(s.begin(), s.end());
    int syncLv = s[s.size() / 100], blank = s[s.size() / 2];
    if (blank - syncLv < 20) return;
    const int thr = syncLv + (blank - syncLv) * 3 / 10;
    // each frame votes (tvinfoorm.vbi swaps its fields in the middle of the recording): the share of
    // sync-level samples in the first and the second half of each stored line
    auto broad = [&](double v) { return v > 0.5; };
    auto none = [&](double v) { return v < 0.25; };
    std::map<std::pair<int, int>, int> votes;
    for (int k = 0; k < frames; k++) {
        if (read(g_.frames * (k + 1) / (frames + 2), 1, buf) < 1) continue;
        std::vector<double> A(L), B(L);
        for (int l = 0; l < L; l++) {
            const uint8_t* p = &buf[(size_t)l * g_.lineBytes];
            int a = 0, b = 0;
            for (int i = 0; i < n; i++) {
                int v = g_.fmt ? (p[2 * i] | p[2 * i + 1] << 8) >> 8 : p[i];
                if (v <= thr) (i < n / 2 ? a : b)++;
            }
            A[l] = (double)a / (n / 2);
            B[l] = (double)b / (n - n / 2);
        }
        int tv[2] = {0, 0};
        for (int h = 0; h < 2; h++)
            for (int k2 = 0; k2 + 2 < half && !tv[h]; k2++) {
                int l = h * half + k2;
                // lines 1, 2, 3
                if (broad(A[l]) && broad(B[l]) && broad(A[l + 1]) && broad(B[l + 1]) && broad(A[l + 2]) && none(B[l + 2]))
                    tv[h] = (1 - k2 + 625 - 1) % 625 + 1;
                // lines 313, 314, 315
                else if (none(A[l]) && broad(B[l]) && broad(A[l + 1]) && broad(B[l + 1]) && broad(A[l + 2]) && broad(B[l + 2]))
                    tv[h] = (313 - k2 + 625 - 1) % 625 + 1;
            }
        // both halves, one of each field
        if (tv[0] && tv[1] && (tv[0] > 313) != (tv[1] > 313)) votes[{tv[0], tv[1]}]++;
    }
    std::pair<int, int> best{0, 0};
    int bestN = 0;
    for (auto& kv : votes) if (kv.second > bestN) { bestN = kv.second; best = kv.first; }
    // the same place in (nearly) every frame: a tape whose sync moves (tvinfoorm.vbi: a VCR in pause makes
    // its own field sync, 3 places in one recording) says nothing about the lines of the broadcast
    if (bestN * 10 < got * 9) return;
    int tv[2] = {best.first, best.second};
    g_.tvFirst[0] = tv[0];
    g_.tvFirst[1] = tv[1];
}

int fieldSyncFirst(const uint8_t* lines, int count, int lineBytes, int samples, int fmt, bool pal)
{
    const int n = std::max(8, samples);
    if (count < 4) return 0;
    auto at = [&](int l, int i) { const uint8_t* p = lines + (size_t)l * lineBytes; return fmt ? (p[2 * i] | p[2 * i + 1] << 8) >> 8 : p[i]; };
    std::vector<int> s;
    for (int l = 0; l < count; l++)
        for (int i = 0; i < n; i += 4) s.push_back(at(l, i));
    std::sort(s.begin(), s.end());
    int syncLv = s[s.size() / 100], blank = s[s.size() / 2];
    if (blank - syncLv < 20) return 0;
    const int thr = syncLv + (blank - syncLv) * 3 / 10;
    std::vector<double> A(count), B(count);
    for (int l = 0; l < count; l++) {
        int a = 0, b = 0;
        for (int i = 0; i < n; i++)
            if (at(l, i) <= thr) (i < n / 2 ? a : b)++;
        A[l] = (double)a / (n / 2);
        B[l] = (double)b / (n - n / 2);
    }
    auto broad = [](double v) { return v > 0.5; };
    auto none = [](double v) { return v < 0.25; };
    const int total = pal ? 625 : 525;
    auto tvOf = [&](int line, int k) { return ((line - k - 1) % total + total) % total + 1; };
    for (int k = 0; k + 3 < count; k++) {
        bool full0 = broad(A[k]) && broad(B[k]), full1 = broad(A[k + 1]) && broad(B[k + 1]), full2 = broad(A[k + 2]) && broad(B[k + 2]);
        if (pal) {
            if (full0 && full1 && broad(A[k + 2]) && none(B[k + 2])) return tvOf(1, k);                   // 1, 2, 3
            if (none(A[k]) && broad(B[k]) && full1 && full2) return tvOf(313, k);                          // 313, 314, 315
        } else {
            if (full0 && full1 && full2 && none(A[k + 3]) && (k == 0 || none(B[k - 1]))) return tvOf(4, k); // 4, 5, 6
            if (none(A[k]) && broad(B[k]) && full1 && full2 && broad(A[k + 3]) && none(B[k + 3])) return tvOf(266, k);   // 266-269
        }
    }
    return 0;
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
