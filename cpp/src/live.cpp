// Live decoding of captured VBI frames, see live.h.
//   Teletext: every line is searched for the clock run-in and framing code (55 55 27) at
//   6.9375 Mbit/s, the 42 bytes after it are read at the found phase, and the packets build
//   pages per magazine the way a TV decoder does (header opens a page, rows 1-24 fill it).
//   Line kinds: valid teletext packets or 608 pairs name a line; otherwise the clock rate the
//   zero crossings lock to (zc_coherence of the original program's probe).
#include "live.h"
#include "decoder.h"
#include "slicer.h"
#include "legacy/pre.h"
namespace legacy {
#include "legacy/src/decode_vbi.h"
#include "legacy/src/vbi_probe.h"
#include "legacy/src/nabts_slicer.h"
}
#include "tvinorpak.h"
#include "ui/util.h"
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <cmath>
#include <atomic>
#include <thread>

// MLSE of the original program for teletext that the plain slicer cannot read (tapes)
struct LivePrior {
    std::vector<double> prior;
    int n = 0;
    bool has_prior() const { return !prior.empty(); }
};
struct LiveMlse {
    legacy::MlseDecoder dec;
    bool tape = false;
    LiveMlse(double spb, bool tape_ = false) : dec(model(spb, tape_)), tape(tape_) {}
    // tape = the tape decoder of a recording (vhs_wst): wide channel, the address and header re-decided among
    // Hamming codewords and text among odd-parity bytes - but only after the free reading of the address or of
    // the text was clean (MlseResult::good), so a line without a packet is not made one. A channel that loses
    // 1010 (s_estr_8_c.vbi, SECAM: the run-in is nearly flat) reads 15 as 29 / 2A / 25 a bit off - rows 0 / 1
    // and the zeros of page numbers - while the text around is clean
    static legacy::MlseModel model(double spb, bool tape)
    {
        auto m = legacy::wst_mlse_model();
        m.spb = spb;
        if (tape) {
            m.L = 6; m.R = 5; m.ntop = 2; m.refits = 1;
            m.good = [](const uint8_t* p) {
                int bad = 0;
                for (int k = 2; k < 42; k++) bad += !(__builtin_popcount(p[k]) & 1);
                return (ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0) || bad <= 2;
            };
            m.code_kind = 1;
            m.code = [](const uint8_t* p, int i) -> int {
                if (i < 2) return 1;
                int a = ham::dec[p[0]], b = ham::dec[p[1]];
                if (a < 0 || b < 0) return 0;
                int row = (a >> 3) | (b << 1);
                if (row == 0) return i < 10 ? 1 : 2;
                // an 8-bit data row («Бизнес-Текст»: > 12 of 40 bytes fail parity in the free reading) keeps its bytes:
                // re-decided among odd-parity bytes it lost about half of them (BTEST: 73% of the even ones)
                if (row <= 25) {
                    int bad = 0;
                    for (int k = 2; k < 42; k++) bad += !(__builtin_popcount(p[k]) & 1);
                    return bad > 12 ? 0 : 2;
                }
                if (row == 27) return i < 40 ? 1 : 0;
                if (row >= 26 && row <= 29) return i == 2 ? 1 : 0;
                return 0;
            };
        }
        return m;
    }
    // The model the graphics card starts with, before this capture's own lines taught it anything: the channel of
    // a clean broadcast line, learnt by this same MLSE from packets made here (NRZ at spb samples a bit, edges as
    // soft as a 5 MHz channel leaves them). The card then learns the capture's own channel from the packets it
    // reads. Empty when the made lines did not read (then the card waits for the processor's model, as before)
    static void generic(double spb, int ns, bool tape, std::vector<double>& prior, int& n)
    {
        prior.clear(); n = 0;
        const int os = 8, nb = 45 * 8;
        const double off = std::min(24.0, ns - (nb + 1) * spb - 2);   // the packet nearly fills a line (1401 of 1440 samples at 27 MHz)
        if (off < 6) return;
        LiveMlse m(spb, tape);
        uint32_t rnd = 12345;
        auto next = [&] { rnd = rnd * 1664525u + 1013904223u; return rnd >> 16; };
        for (int k = 0; k < 16; k++) {
            uint8_t pk[45] = {0x55, 0x55, 0x27};
            pk[3] = ham::CODEWORDS[(next() & 7) | 8]; pk[4] = ham::CODEWORDS[next() & 15];   // rows 1 and up: text
            for (int i = 5; i < 45; i++) {
                int c = 0x20 + next() % 0x5F;
                pk[i] = (uint8_t)((__builtin_popcount(c) & 1) ? c : c | 0x80);
            }
            // the bits, low one first, at os points a sample; two box filters of 0.6 bit soften the edges
            std::vector<float> w((size_t)ns * os, 0.f), t;
            for (size_t i = 0; i < w.size(); i++) {
                double b = ((double)i / os - off) / spb;
                int q = (int)std::floor(b);
                if (q >= 0 && q < nb) w[i] = (pk[q / 8] >> (q % 8)) & 1 ? 1.f : 0.f;
            }
            const int box = std::max(1, (int)std::lround(0.6 * spb * os));
            for (int pass = 0; pass < 2; pass++) {
                t.assign(w.size(), 0.f);
                double s = 0;
                for (size_t i = 0; i < w.size(); i++) {
                    s += w[i];
                    if (i >= (size_t)box) s -= w[i - box];
                    t[i >= (size_t)box / 2 ? i - box / 2 : 0] = (float)(s / box);
                }
                w.swap(t);
            }
            std::vector<float> y((size_t)ns);
            for (int i = 0; i < ns; i++) y[i] = 60.f + 120.f * w[(size_t)i * os];
            m.dec.decode(y.data(), ns, off - 4, off + 4, 0.5);
        }
        if (m.dec.n >= 6 && m.dec.has_prior()) { prior = m.dec.prior; n = m.dec.n; }
    }
};

// the templates of the recording's decoder trained on this capture (LiveDecoder::tplStart)
struct LiveTpl {
    legacy::tpl::Decoder dec;
    double lo = 0, hi = 0;         // where the packet starts on the bt8x8 grid (the training's calibration)
    std::vector<double> src0;      // per line of the frame: where its packet starts in the capture, -1 = not trained
    explicit LiveTpl(const legacy::tpl::Templates& T) : dec(T) {}
};

namespace {
// a line of the capture on the bt8x8 grid (35.47 MHz, 2048 samples) the way the recording's decoder converts a
// 27 MHz one (wst_to_bt8x8): its sample src0 (where the packet starts) goes where bttv has the run-in
void toBt(const uint8_t* p, int fmt, int n, double fs, double src0, uint8_t* d)
{
    std::vector<float> y(n);
    for (int i = 0; i < n; i++) y[i] = fmt == 1 ? (p[2 * i] | (p[2 * i + 1] << 8)) / 256.0f : p[i];
    const double k = fs / 35468950.0;
    for (int j = 0; j < 2048; j++) {
        double x = src0 + (j - legacy::BT8X8_WST_START) * k, v;
        int i = (int)std::floor(x);
        if (i < 1 || i + 2 >= n) v = legacy::interp(x, y.data(), n);
        else {
            double t = x - i, p0 = y[i - 1], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2];
            v = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
        }
        d[j] = (uint8_t)std::clamp(std::lround(v), 0L, 255L);
    }
}
constexpr int kTplLines = 6000;   // teletext lines kept for the templates (a random sample of the session)
constexpr int kTplMin = 3000;     // lines before the first training (~10 s of 12 lines a frame)
constexpr int kTplParity = 4;     // text bytes still failing parity after the repair that a template packet may have

// decoding runs below the capture threads: when the processor is busy the card must not lose fields
void lowPriority(bool lowest)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), lowest ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)lowest;
#endif
}

// processor time of the whole process, in seconds (for the TR_LC_PROF / TR_GPUTIME studies)
static double processCpuSeconds()
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
#else
    return (double)std::clock() / CLOCKS_PER_SEC;
#endif
}

const double kWstRate = 6.9375e6;

struct Rate { const char* kind; double hz; };
const Rate kRates[] = {
    {"WST teletext rate", 6.9375e6}, {"NABTS / WST 525 rate", 8 * 315e6 / 88 / 5}, {"VPS", 5e6},
    {"TV-Inform (222 fH)", 222 * 15625.0}, {"TV-Inform (111 fH)", 111 * 15625.0}, {"WSS", 5e6 / 6}, {"CC rate", 32 * 15734.264},
};

inline float at(const float* y, int n, double t)
{
    if (t <= 0) return y[0];
    int i = (int)t;
    if (i >= n - 1) return y[n - 1];
    float f = (float)(t - i);
    return y[i] + f * (y[i + 1] - y[i]);
}

// 55 55 27, sent LSB first
const int kRunIn[24] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0};

// finds the clock run-in and framing code: centre of bit 0 (samples) and how well it matches (0..1)
double wstFind(const float* y, int n, double fs, double& bt)
{
    double spb = fs / kWstRate;
    int lo = 0, hi = std::min(n, (int)(n * 0.5));
    float mn = 1e9f, mx = -1e9f;
    for (int i = lo; i < std::min(n, (int)(hi + 50 * spb)); i++) { mn = std::min(mn, y[i]); mx = std::max(mx, y[i]); }
    if (mx - mn < 20) return 0;
    float mid = (mn + mx) / 2;
    double best = -1e30;
    bt = 0;
    double last = hi - 24 * spb;
    for (double t = lo + spb / 2; t < last; t += 0.5) {
        double s = 0;
        for (int k = 0; k < 24; k++) s += (kRunIn[k] ? 1 : -1) * (at(y, n, t + k * spb) - mid);
        if (s > best) { best = s; bt = t; }
    }
    double b0 = bt;
    for (double t = b0 - 0.5; t <= b0 + 0.5; t += 0.1) {
        double s = 0;
        for (int k = 0; k < 24; k++) s += (kRunIn[k] ? 1 : -1) * (at(y, n, t + k * spb) - mid);
        if (s > best) { best = s; bt = t; }
    }
    return best / (24 * (mx - mn) / 2);
}

// reads 42 bytes after the framing code at the found phase; false if the framing code is wrong
bool wstRead(const float* y, int n, double fs, double bt, uint8_t out[42])
{
    double spb = fs / kWstRate;
    double th = 0;   // threshold from the run-in itself (its mean is the middle of the eye)
    for (int k = 0; k < 16; k++) th += at(y, n, bt + k * spb);
    th /= 16;
    uint8_t b[45];
    for (int j = 0; j < 45; j++) {
        int v = 0;
        for (int k = 0; k < 8; k++) {
            double t = bt + (j * 8 + k) * spb;
            if (t >= n - 1) return false;
            if (at(y, n, t) > th) v |= 1 << k;
        }
        b[j] = (uint8_t)v;
    }
    if (__builtin_popcount(b[2] ^ 0x27) > 1) return false;
    std::copy(b + 3, b + 45, out);
    return true;
}

// where a run-in may start (sample of the start of bit 0): the best peaks of the normalised correlation
// with 55 55 27 over the first 30 % of the line; flat stretches (blanking) do not count
std::vector<double> runInCandidates(const float* y, int n, double spb)
{
    float mn = *std::min_element(y, y + n), mx = *std::max_element(y, y + n);
    double minSd = 0.12 * (mx - mn);
    std::vector<std::pair<double, double>> sc;
    double v[24];
    for (double t = spb / 2; t + 24 * spb < n * 0.3 + 24 * spb && t + 24 * spb < n - 1; t += 0.5) {
        double m = 0;
        for (int k = 0; k < 24; k++) { v[k] = at(y, n, t + k * spb); m += v[k]; }
        m /= 24;
        double e = 0, c = 0;
        for (int k = 0; k < 24; k++) { double d = v[k] - m; e += d * d; c += (kRunIn[k] ? 1 : -1) * d; }
        double sd = std::sqrt(e / 24);
        if (sd < minSd) continue;
        sc.push_back({c / (std::sqrt(e) * std::sqrt(24.0)), t - spb / 2});
    }
    std::sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<double> out;
    for (auto& s : sc) {
        if (s.first < 0.3) break;
        bool near = false;
        for (double o : out) near |= std::fabs(o - s.second) < 2 * spb;
        if (!near) out.push_back(s.second);
        if (out.size() >= 4) break;
    }
    return out;
}

bool usable(const uint8_t* p) { return ham::fix[p[0]] >= 0 && ham::fix[p[1]] >= 0; }
int parityBad(const uint8_t* p) { int n = 0; for (int k = 2; k < 42; k++) n += !(__builtin_popcount(p[k]) & 1); return n; }

// a header whose address the tape broke (tvinfoorm.vbi: most of the teletext lines the MLSE loses are
// headers with 2-6 wrong address bits and intact text): page number, subcode and control bytes are all
// Hamming code words, so the row is 0; the magazine is the nearest code word when it leads the next
// one by 3 bits or more (checked against another capture of the same broadcast: 169 right, 0 wrong)
bool mendHeaderAddress(uint8_t* p)
{
    for (int k = 2; k < 10; k++) if (ham::fix[p[k]] < 0) return false;
    if (parityBad(p) > 12) return false;
    int best = 99, second = 99, bm = -1;
    for (int m = 0; m < 8; m++) {
        int d = __builtin_popcount(ham::CODEWORDS[m] ^ p[0]) + __builtin_popcount(ham::CODEWORDS[0] ^ p[1]);
        if (d < best) { second = best; best = d; bm = m; } else if (d < second) second = d;
    }
    if (second - best < 3) return false;
    p[0] = ham::CODEWORDS[bm]; p[1] = ham::CODEWORDS[0];
    return true;
}

inline bool oddParity(uint8_t c) { return __builtin_popcount(c) & 1; }
}

void LiveDecoder::setLineForce(int index, int force)
{
    if (index < 0 || index >= 64) return;
    lineForce_[index] = force;
    lineMode_[index] = force == (int)LineForce::None ? kLiveLineOff : force == (int)LineForce::Teletext625 ? kLiveLineTeletext : kLiveLineAuto;
}

void LiveDecoder::joinProbe()
{
    if (probe_.joinable()) probe_.join();
}

// the line map of the full decoder (the original program's probe: test signals, SECAM identification,
// copy protection, VPS, WSS, data clocks...) on the last 5 s, written to a temporary recording
// a teletext line for the templates: on the bt8x8 grid from the common packet start, into the session's sample
// (reservoir: every line seen has the same chance to be in it)
void LiveDecoder::tplGather(int index, const uint8_t* p)
{
    const LineAcc& a = acc_[index];
    if ((int)tplLine_.size() < g_.linesPerFrame) tplLine_.resize(g_.linesPerFrame, 0);
    // the teletext lines are the ones named so before the first training: once the templates read, they would
    // make packets of any line they are let on (SECAM identification, test signals - s_estr_8_c.vbi lines 7-15)
    if (tplTries_ == 0) { if (a.kind != "WST teletext" || a.off < 0 || a.good < 0.2) return; tplLine_[index] = 1; }
    else if (!tplLine_[index]) return;
    tplSeen_++;
    size_t have = tplBuf_.size() / 2048, slot = have;
    if (have >= (size_t)kTplLines) {
        tplRng_ = tplRng_ * 6364136223846793005ull + 1442695040888963407ull;
        slot = (size_t)((tplRng_ >> 33) % tplSeen_);
        if (slot >= have) return;
    } else tplBuf_.resize((have + 1) * 2048);
    toBt(p, g_.fmt, g_.validSamples, g_.lineSamples * g_.lineRate, tplSrc_, tplBuf_.data() + slot * 2048);
}

// the sample as a recording of 2 lines a frame, trained as the recording's decoder trains (train_templates); the
// templates are taken when they read at least 30% of the sampled lines and more than the ones in use
void LiveDecoder::tplStart()
{
    if (tplThread_.joinable()) tplThread_.join();
    tplTries_++;
    tplAt_ = frames_;
    tplBusy_ = true;
    auto buf = std::make_shared<std::vector<uint8_t>>(tplBuf_);
    std::vector<double> src0(g_.linesPerFrame, -1);
    for (int l = 0; l < g_.linesPerFrame && l < (int)tplLine_.size(); l++) if (tplLine_[l]) src0[l] = tplSrc_;
    tplThread_ = std::thread([this, buf, src0] {
        lowPriority(true);
        legacy::parallel_low = true;
        auto path = (std::filesystem::temp_directory_path() / ("TeletextRescue_tpl_" + std::to_string((uintptr_t)this) + ".vbi")).u8string();
        try {
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fwrite(buf->data(), 1, buf->size(), f);
                fclose(f);
                legacy::Progress pr;
                auto stop = [this] { if (tplQuit_) throw std::runtime_error("stopped"); };
                pr.on_step = [stop](const std::string&) { stop(); };
                pr.on_progress = [stop](long long, long long, const std::string&) { stop(); };
                static const bool debug = getenv("TR_DEBUG") != nullptr;
                if (debug) pr.on_log = [](const std::string& s) { fprintf(stderr, "live templates: %s\n", s.c_str()); };
                std::shared_ptr<LiveTpl> t;
                double got, own = -1;
                {
                    legacy::VbiReader R(path, 2, false, pr);
                    got = legacy::train_templates(R, pr);
                    // templates only where they read more than the MLSE (on a clean channel the live slicer and
                    // the MLSE read better than templates: they would take its lines with wrong bytes)
                    own = R.rate_tpl;
                    if (own >= 0.3 && own > R.rate_mlse && own * 100 > tplGot_) {
                        t = std::make_shared<LiveTpl>(R.templates());
                        t->lo = R.win_lo; t->hi = R.win_hi; t->src0 = src0;
                    }
                }
                if (debug) fprintf(stderr, "live templates: %.0f%% of the sampled lines read, %.0f%% by the templates%s\n", got * 100, own * 100, t ? ", taken" : "");
                std::lock_guard<std::mutex> lk(mu_);
                if (t) { tpl_ = t; tplGot_ = (int)std::lround(own * 100); }
                else if (!tpl_ && got * 100 > tplGot_) tplGot_ = (int)std::lround(got * 100);   // read well without them: no more tries
            }
        } catch (std::exception& e) {
            if (getenv("TR_DEBUG")) fprintf(stderr, "live templates: %s\n", e.what());
        } catch (...) {}
        std::remove(path.c_str());
        tplBusy_ = false;
    });
}

void LiveDecoder::startProbe()
{
    const char* fmtName = g_.pal && g_.lineBytes == 1440 && g_.linesPerFrame == 32 ? "saa7131"
                        : !g_.pal && g_.lineBytes == 1440 && g_.linesPerFrame == 24 ? "cx23885"
                        : g_.pal && g_.lineBytes == 2048 && g_.linesPerFrame == 32 ? "bt8x8-pal"
                        // 1440 x 18 lines a field (CX23885 / CX231xx, AVerTV 307 SAA7134 with its WDM driver)
                        : g_.pal && g_.lineBytes == 1440 && g_.linesPerFrame == 36 ? "d27-pal" : nullptr;
    if (!fmtName && g_.pal) return;   // 525 lines of any layout: NABTS / JTES are read without the old engine's probe
    if (probe_.joinable()) probe_.join();   // finished (probing_ is false)
    probeAt_ = frames_;
    probing_ = true;
    const int64_t firstFrame = frames_ + 1 - (int64_t)ring_.size();   // the session frame the chunk starts at (the newest is frames_)
    auto frames = std::make_shared<std::vector<uint8_t>>();
    for (auto& f : ring_) frames->insert(frames->end(), f.begin(), f.end());
    std::string fmtS = fmtName ? fmtName : "";
    bool fieldUnits = fmtS == "cx23885";
    bool byRow = fmtS == "d27-pal";   // the old engine numbers field 2 of this layout one line lower: named by row
    int off2 = g_.pal ? 313 : 263;
    std::set<int> ttLines;
    for (size_t i = 0; i < acc_.size(); i++) if (acc_[i].kind == "WST teletext" && acc_[i].good > 0.2) ttLines.insert((int)i);
    for (int i = 0; i < 64; i++) probeForce_[i] = lineForce_[i];
    const int lineOff = g_.firstLine - baseFirstLine_;   // Sync: the lines numbered otherwise than the card does
    probe_ = std::thread([this, frames, fmtS, fieldUnits, byRow, off2, ttLines, firstFrame, lineOff] {
        lowPriority(true);
        std::map<int, std::string> kinds;
        try {
            auto path = (std::filesystem::temp_directory_path() / ("TeletextRescue_live_" + std::to_string((uintptr_t)this) + ".vbi")).u8string();
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fwrite(frames->data(), 1, frames->size(), f);
                fclose(f);
                std::map<int, std::string> names;
                static const bool timing = getenv("TR_TIMING") != nullptr;
                auto t0 = std::chrono::steady_clock::now();
                auto lap = [&](const char* what) {
                    if (!timing) return;
                    auto t = std::chrono::steady_clock::now();
                    fprintf(stderr, "probe: %s %.2f s\n", what, std::chrono::duration<double>(t - t0).count());
                    t0 = t;
                };
                if (!fmtS.empty() && g_.pal) {
                legacy::Progress pr;
                legacy::Json r = legacy::vbi_probe(path, pr, 100, fmtS);
                lap("line probe");
                for (auto& L : r["lines"].a) {
                    int tv = (int)L["tv_line"].integer() + lineOff;   // the probe numbers as the card does
                    if (fieldUnits && !L["parity"].is_null() && L["parity"].integer() == 1) tv += off2;
                    if (byRow && !L["row"].is_null()) tv = tvLineOf(g_, (int)L["row"].integer());
                    kinds[tv] = L.get_str("kind");
                }
                // the window names ТВ-информ lines as the recording's decoder does (the probe alone calls fТ1 a
                // test signal and Norpak packets NABTS)
                try { findFiles(path, fmtS, *frames, kinds, ttLines, firstFrame, names); } catch (std::exception& e) { if (getenv("TR_DEBUG")) fprintf(stderr, "findFiles: %s\n", e.what()); } catch (...) { if (getenv("TR_DEBUG")) fprintf(stderr, "findFiles: exception\n"); }
                lap("files");
                }
                if (!g_.pal) {
                    const int64_t fb = g_.frameBytes();
                    try { find525(path, fb ? (int64_t)frames->size() / fb : 0, firstFrame, names); } catch (...) { if (getenv("TR_DEBUG")) fprintf(stderr, "find525: exception\n"); }
                    lap("525-line services");
                }
                for (auto& kv : names) kinds[kv.first] = kv.second;
                std::remove(path.c_str());
            }
        } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!kinds.empty()) {
                probeKind_ = kinds;
                for (int i = 0; i < 64 && i < g_.linesPerFrame; i++) {
                    auto it = kinds.find(tvLineOf(g_, i));
                    if (it != kinds.end() && it->second.find("teletext") != std::string::npos) probeTT_[i] = true;
                }
            }
        }
        probing_ = false;
    });
}

void LiveDecoder::stopWorker()
{
    {
        std::lock_guard<std::mutex> q(qmu_);
        quit_ = true;
        queue_.clear();
    }
    qcv_.notify_all();
    if (worker_.joinable()) worker_.join();
    quit_ = false;
}

void LiveDecoder::push(const uint8_t* frame, int bytes)
{
    std::unique_lock<std::mutex> q(qmu_);
    if (bytes != frameBytes_ || frameBytes_ <= 0) return;   // a frame of the layout before a reset
    if (rec_ && fwrite(frame, 1, bytes, rec_) == (size_t)bytes) recFrames_++;
    if (!worker_.joinable()) {
        worker_ = std::thread([this] {
            lowPriority(false);
            for (;;) {
                std::vector<uint8_t> f;
                {
                    std::unique_lock<std::mutex> q(qmu_);
                    qcv_.wait(q, [this] { return quit_ || !queue_.empty(); });
                    if (quit_) return;
                    f = std::move(queue_.front());
                    queue_.pop_front();
                }
                feed(f.data());
            }
        });
    }
    if (queue_.size() >= 25) { queue_.pop_front(); dropped_++; }   // a second behind: skip the oldest
    queue_.emplace_back(frame, frame + bytes);
    q.unlock();
    qcv_.notify_one();
}

void LiveDecoder::reset(const VbiGeometry& g)
{
    stopWorker();
    joinProbe();
    tplJoin();
    {
        std::lock_guard<std::mutex> q(qmu_);
        frameBytes_ = g.frameBytes();
        dropped_ = 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    tpl_.reset(); tplNow_.reset(); tplBuf_.clear(); tplSeen_ = 0; tplLine_.clear(); tplSrc_ = -1; tplAt_ = 0; tplGot_ = -1; tplTries_ = 0;
    g_ = g;
    baseFirstLine_ = g.firstLine;
    frames_ = 0; packets_ = bad_ = 0;
    acc_.assign(g.linesPerFrame, {});
    lineStats_.clear();
    last_.clear();
    pages_.clear();
    pageTx_.clear();
    pending_.clear();
    session_.clear();
    for (auto& m : lineMode_) m = kLiveLineAuto;
    for (auto& m : lineForce_) m = 0;
    for (int& o : open_) o = -1;
    hasHeader_ = false; service_.clear(); cc_.clear(); charset_.clear(); charsetAt_ = -1000;
    ni_ = -1; headerWords_.clear(); headerPages_.clear(); lastHeaderText_.clear();
    ccLast_[0] = ccLast_[1] = -1;
    rate_.clear();
    off_ = -1;
    ring_.clear(); probeKind_.clear(); probeAt_ = 0;
    for (auto& t : probeTT_) t = false;
    master_.reset();
    bt_ = BtCatalog();
    vpsLast_.clear(); vpsB4_.clear(); vpsData_.clear(); vpsDataAt_.clear(); vpsChosen_ = -1; wssLast_.clear(); vpsFrame_ = wssFrame_ = -1;
    vps_ = LiveVps();
    btWritten_.clear(); btNote_.clear(); btFrames_ = -1; btRows_ = 0;
    td_ = TtDataPages();
    ts_ = TsCollector(); tsSeen_ = 0; tsAt_ = -1000000; tsProgs_.clear();
    tviLive_.clear();
    tvi3Slicers_.clear(); tvi3Live_ = TviStream(); tvi3Lines_.clear();
    tviFinds_.clear();
    norpakFiles_.clear();
    norpakChunk_ = 0;
    norpakLines_.clear();
    norpakBeacons_.clear();
    norpakVotes_.clear();
    norpakProbes_ = 0;
    if (!t33_.empty()) { std::remove(t33_.c_str()); std::remove((t33_ + ".pos").c_str()); t33_.clear(); }
    fast525_.clear(); fast525Chunk_ = 0; fast525Made_ = 0;
    ss_ = SsDatabase(); ssMade_ = 0;
    gemWords_.clear();
    jtesHow_ = JtesRecRead(); jtesTries_ = 0;
    for (auto& t : vitcText_) t.clear();
    for (auto& a : vitcAt_) a = 0;
    res525_.reset();
}

void LiveDecoder::feed(const uint8_t* frame)
{
    if (repOn_) {   // VBI repair: the frame decoded is the repaired one
        std::lock_guard<std::mutex> lk(repMu_);
        repFrame_.assign(frame, frame + g_.frameBytes());
        repairer_.frame(repFrame_.data(), g_);
        frame = repFrame_.data();
    }
    // the lines are decoded without the lock (only this thread touches acc_ and the MLSE), so the window
    // never waits for a frame being decoded; the results go in under the lock at the end
    if (g_.linesPerFrame <= 0) return;
    // lines in parallel (the MLSE costs a few ms a line), then their packets in order. The MLSE channel
    // model is one for all lines: every line starts the frame from it, and it learns from the lines that
    // read a clean packet
    int L = g_.linesPerFrame;
    for (auto& a : acc_)
        if (a.mlse && master_ && master_->has_prior()) { a.mlse->dec.prior = master_->prior; a.mlse->dec.n = master_->n; }
    // the teletext lines of this frame on the graphics card, when it was switched on and the model is learnt: the
    // lines then skip their own slicing and take the packet the card read (one kernel call for the whole frame)
    // study (TR_LC_PROF=1): processor time of the process spent in the card's call and in the lines' own decoding
    static const bool prof = getenv("TR_LC_PROF") != nullptr;
    auto cpuNow = processCpuSeconds;
    static double pGpu = 0, pLines = 0, pRest = 0, pT = 0;
    double c0 = prof ? cpuNow() : 0;
    if (gpuWanted_ && !gpuDead_) { gpuStart(); if (gp_) gpuFrame(frame); }
    double c1 = prof ? cpuNow() : 0;
    { std::lock_guard<std::mutex> lk(mu_); tplNow_ = tpl_; }
    std::vector<LineOut> out(L);
    std::atomic<int> next{0};
    auto work = [&] { lowPriority(false); for (int l; (l = next++) < L;) out[l] = line(l, frame + (size_t)l * g_.lineBytes); };
    int T = std::min(L, std::max(1, (int)std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (int t = 1; t < T; t++) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
    if (prof) {
        double c2 = cpuNow();
        pGpu += c1 - c0; pLines += c2 - c1;
        if (pT == 0) pT = c0;
        if (frames_ % 250 == 0) fprintf(stderr, "cpu: card call %.1f s, lines %.1f s, all %.1f s (%lld frames)\n", pGpu, pLines, c2 - pT, (long long)frames_);
    }
    // lines for the templates (WST 625): a sample of the session, trained in their own thread when there are enough
    // and again every 20 s while the templates read under 60% of it
    // (not while the card reads: it does not use them)
    if (g_.pal && tplGot_ < 60 && tplTries_ < 30 && !gp_) {
        if (tplSrc_ < 0 && frames_ >= 100) {   // the packet start, once the lines have settled on theirs
            std::vector<double> offs;
            for (auto& a : acc_) if (a.kind == "WST teletext" && a.off >= 0) offs.push_back(a.off);
            if (!offs.empty()) { std::nth_element(offs.begin(), offs.begin() + offs.size() / 2, offs.end()); tplSrc_ = offs[offs.size() / 2]; }
        }
        if (tplSrc_ >= 0) for (int l = 0; l < L; l++) tplGather(l, frame + (size_t)l * g_.lineBytes);
        if (!tplBusy_ && tplBuf_.size() >= (size_t)kTplMin * 2048 && (tplTries_ == 0 || frames_ - tplAt_ >= 500)) tplStart();
    }
    for (auto& a : acc_) {
        if (!a.mlse || !a.mlse->dec.has_prior()) continue;
        if (!master_) master_ = std::make_shared<LivePrior>();
        if (!master_->has_prior()) { master_->prior = a.mlse->dec.prior; master_->n = a.mlse->dec.n; continue; }
        if (a.mlse->dec.n > master_->n) {   // this line learnt: take its step
            master_->prior = a.mlse->dec.prior;
            master_->n++;
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    last_.assign(frame, frame + g_.frameBytes());
    ring_.emplace_back(frame, frame + g_.frameBytes());
    if (ring_.size() > 125) ring_.pop_front();
    if (frames_ - probeAt_ >= 125 && ring_.size() >= 100 && !probing_) startProbe();
    for (int l = 0; l < L; l++) {
        if (out[l].wst) {
            packet(out[l].pk);
            session_.insert(session_.end(), out[l].pk, out[l].pk + 42);
            if (t42_ && fwrite(out[l].pk, 1, 42, t42_) == 42) t42Packets_++;
            if (raw_) fwrite(out[l].pk, 1, 42, raw_);
            bt_.packet(out[l].pk, frames_);
            td_.packet(out[l].pk);
            ts_.packet(out[l].pk);
        } else if (out[l].raw) {
            if (raw_) fwrite(out[l].rpk, 1, 42, raw_);
            bt_.packet(out[l].rpk, frames_);
        }
        if (out[l].cc && l < L / 2) cc(out[l].ccv[0], out[l].ccv[1]);
        // VPS from ONE line: ARD 23.11.95 sends a second VPS line on 329 (field 2) with other bytes (CNI 0C0, 10:00
        // against DC4, 10:03 on line 16) - voted together they gave a label neither line sends. The line read best
        // lately; of lines read as well the one nearest line 16 of its field (Eurosport 13.11: the ТВ-информ lines
        // 7, 8, 320, 321 pass the start-code test in every frame too, with a label of their own), field 1 before
        // field 2; the line chosen before stays while it reads about as well (no flapping between two)
        if (l == 0) {
            int prev = vpsChosen_;
            vpsChosen_ = -1;
            double best = 0;
            for (int q = 0; q < L && q < (int)acc_.size(); q++) {
                int tv = tvLineOf(g_, q), ln = tv > 313 ? tv - 313 : tv;
                double s = acc_[q].vpsGood - 0.02 * std::abs(ln - 16) + (tv <= 313 ? 0.005 : 0);
                if (acc_[q].vpsGood > 0.2 && s > best) { best = s; vpsChosen_ = q; }
            }
            if (prev >= 0 && prev < (int)acc_.size() && vpsChosen_ >= 0 && prev != vpsChosen_ && acc_[prev].vpsGood > 0.2) {
                int tv = tvLineOf(g_, prev), ln = tv > 313 ? tv - 313 : tv;
                if (acc_[prev].vpsGood - 0.02 * std::abs(ln - 16) + (tv <= 313 ? 0.005 : 0) >= best - 0.1) vpsChosen_ = prev;
            }
            vpsChosenTv_ = vpsChosen_ >= 0 ? tvLineOf(g_, vpsChosen_) : 0;
        }
        if (out[l].vps && vpsChosen_ >= 0 && l != vpsChosen_) {
            // not the VPS line: an uplink's clock or name line in the same format (Eurosport 20.01.98)
            VpsData d;
            int tv = tvLineOf(g_, l);
            auto at = vpsDataAt_.find(tv);
            bool isData = vpsDataOf(out[l].vpsb, d, at == vpsDataAt_.end() ? -1 : at->second);
            const std::string reg = vpsCniName(vpsLabel(out[l].vpsb).cni);
            if (isData && d.clock) { vpsDataAt_[tv] = d.at; vpsData_[tv] = {vpsDataText(d), frames_}; }
            else if (!reg.empty()) {   // a second VPS label (ARD 23.11.95 line 329: WDR)
                std::string t = vpsText(out[l].vpsb);
                vpsData_[tv] = {"second VPS label: " + t.substr(0, t.find(" \xC2\xB7 source data")), frames_};
            } else if (isData && d.text.size() >= 5) vpsData_[tv] = {vpsDataText(d), frames_};
            out[l].vps = false;
        }
        if (out[l].vps) {
            vpsLast_.push_back(std::string((const char*)out[l].vpsb, 13));
            if (vpsLast_.size() > 250) vpsLast_.pop_front();
            if (vpsB4_.empty() || vpsB4_.back().first != frames_) vpsB4_.push_back({frames_, out[l].vpsb[1]});
            if (vpsB4_.size() > 400) vpsB4_.pop_front();
            vpsFrame_ = frames_;
        }
        if (out[l].wss) {
            wssLast_.push_back(out[l].wssv);
            if (wssLast_.size() > 250) wssLast_.pop_front();
            wssFrame_ = frames_;
        }
        if (out[l].vitc && l < 64) { vitcText_[l] = out[l].vitcText; vitcAt_[l] = frames_; }
        if (out[l].vps) vps_.reads++;
        if (out[l].wss) vps_.wssReads++;
        if (out[l].off >= 0) off_ = off_ < 0 ? out[l].off : 0.95 * off_ + 0.05 * out[l].off;
    }
    // a card left free running (no vertical lock) rolls its window through the frame: whenever the field sync
    // is in the window, the frame itself says which TV lines the window holds
    {
        const int half = L / 2, off2 = g_.pal ? 313 : 263, tot = g_.pal ? 625 : 525;
        int first = 0;
        if (int t0 = fieldSyncFirst(frame, half, g_.lineBytes, g_.validSamples, g_.fmt, g_.pal))
            first = t0 > tot - 30 ? t0 - tot : t0 > off2 ? t0 - off2 : t0;
        else if (int t1 = fieldSyncFirst(frame + (size_t)half * g_.lineBytes, half, g_.lineBytes, g_.validSamples, g_.fmt, g_.pal))
            first = t1 - off2;
        // the same numbering in 3 frames in a row: a tape dropout or a head switch can look like the field sync once
        // (capture_2026-10-23_233506: the whole recording numbered from line 622)
        if (first && first == syncCand_) syncCandN_++;
        else { syncCand_ = first; syncCandN_ = first ? 1 : 0; }
        if (first && syncCandN_ < 3) first = 0;
        if (first && first != g_.firstLine) {
            g_.firstLine = first;
            if (++syncMoves_ % 25 == 1) probeKind_.clear();   // named by the old numbering
        }
        if (first) syncSeen_ = frames_;
    }
    frames_++;
    if (frames_ % 25 == 0) voteCodes();
    rate_.push_back({frames_, packets_});
    if (rate_.size() > 50) rate_.erase(rate_.begin());
    lineStats_.resize(L);
    for (int l = 0; l < L; l++) {
        LiveLineStat& st = lineStats_[l];
        int half = L / 2;
        st.index = l; st.field = l < half ? 1 : 2;
        st.lineNo = g_.firstLine + l % half + (st.field == 2 ? (g_.pal ? 313 : 263) : 0);
        if (st.lineNo <= 0) st.lineNo += g_.pal ? 625 : 525;   // the window starts before line 1 (field sync seen)
        st.kind = acc_[l].kind; st.level = acc_[l].level; st.good = acc_[l].good; st.packets = acc_[l].packets; st.mlse = acc_[l].mlsePackets;
        st.mode = lineMode_[l];
        st.force = lineForce_[l];
        st.vitc = l < 64 && vitcAt_[l] > 0 && frames_ - vitcAt_[l] < 25 ? vitcText_[l] : std::string();
    }
    if (frames_ - charsetAt_ >= 50 && !pages_.empty()) {   // every 2 s: words of all pages decide Latin / Cyrillic
        charsetAt_ = frames_;
        std::vector<const u8*> rows;
        for (auto& kv : pages_)
            for (auto& sp : kv.second)
                for (auto& r : sp.second.rows) if (r.first > 0) rows.push_back(r.second.data());
        charset_ = guess_charset(rows, national_);
        if (charset_ == "latin") {
            // few dictionary words (titles in capitals, figures, tape errors): Russian read in the Latin set has
            // w (в) q (я) x (ь) v (ж) and { | } ~ (ш э щ ч) often, q and x are rare in Western languages
            long letters = 0, typical = 0, qx = 0;
            for (const u8* r : rows)
                for (int k = 0; k < 40; k++) {
                    int c = r[k] & 0x7F;
                    if (c < 0x40 || c == 0x7F) continue;
                    letters++;
                    int lc = c | 0x20;
                    if (lc == 'q' || lc == 'x') { qx++; typical++; }
                    else if (lc == 'w' || lc == 'v' || c >= 0x7B) typical++;
                }
            if (letters >= 300 && qx * 100 >= letters * 2 && typical * 100 >= letters * 9) charset_ = "cyr2";
        }
    }
}

LiveDecoder::LineOut LiveDecoder::line(int index, const uint8_t* p)
{
    LineOut res;
    if (index < 64 && lineMode_[index] == kLiveLineOff) return res;   // switched off by hand
    const int fc = index < 64 ? lineForce_[index].load() : 0;
    const bool forceCC = fc == (int)LineForce::Captions;
    const bool noTT = fc != 0 && fc != (int)LineForce::Teletext625;   // set by hand to another signal
    if (manual_ && !fc) return res;   // only the lines chosen by hand
    int n = g_.validSamples;
    double fs = g_.lineSamples * g_.lineRate;
    std::vector<float> y(n);
    for (int i = 0; i < n; i++) y[i] = g_.fmt == 1 ? (p[2 * i] | (p[2 * i + 1] << 8)) / 256.0f : p[i];
    std::vector<float> raw = y;
    // 3-tap smoothing against sampling noise
    for (int i = n - 2; i > 0; i--) y[i] = 0.25f * y[i - 1] + 0.5f * y[i] + 0.25f * y[i + 1];
    LineAcc& a = acc_[index];
    float mn = *std::min_element(y.begin(), y.end()), mx = *std::max_element(y.begin(), y.end());
    a.level = 0.9 * a.level + 0.1 * (mx - mn);
    bool ok = false;
    uint8_t* pk = res.pk;
    // VPS on line 16 of field 1, WSS on line 23 (or wherever the rate named them): fixed formats with a start
    // code, read before teletext is tried; a line next to them too (a tape or a processor moves the picture by one)
    const int tv = tvLineOf(g_, index);
    if (g_.pal && !noTT && ((tv >= 15 && tv <= 17) || a.kind == "VPS") && vpsRead(raw.data(), n, fs, res.vpsb)) res.vps = ok = true;
    // a line that reads as VPS in most frames: the frames the tape spoiled, by the more tolerant reading
    if (g_.pal && !noTT && !ok && a.kind == "VPS" && a.vpsGood > 0.5 && vpsReadKnown(raw.data(), n, fs, res.vpsb)) res.vps = ok = true;
    if (g_.pal && !noTT && !ok && ((tv >= 22 && tv <= 24) || a.kind == "WSS") && wssRead(raw.data(), n, fs, res.wssv)) res.wss = ok = true;
    a.vpsGood = 0.95 * a.vpsGood + 0.05 * res.vps;
    a.wssGood = 0.95 * a.wssGood + 0.05 * res.wss;
    // the packet the graphics card read for this line of the frame (gpuFrame): taken as it is, no slicing here
    auto takeGpu = [&]() -> bool {
        if (index >= (int)gpuHave_.size() || !gpuHave_[(size_t)index]) return false;
        gpuHave_[(size_t)index] = 0;
        std::copy(gpuLine_[(size_t)index].begin(), gpuLine_[(size_t)index].end(), res.pk);
        res.wst = ok = true;
        a.packets++; a.mlsePackets++; a.misses = 0;
        // a line the card found by searching takes the place where it read the packet
        const double go = index < (int)gpuOff_.size() ? gpuOff_[(size_t)index] : -1;
        if (go >= 0) a.off = a.off < 0 ? go : 0.9 * a.off + 0.1 * go;
        res.off = a.off;
        return true;
    };
    // the card is reading (gpuFrame): the teletext of this frame is its packet, or the 8-bit data row it read, or
    // nothing - not read again on the processor (the user: "only the graphics card")
    const bool cardReads = gp_ && g_.pal && !noTT && !forceCC && a.kind != "CC 608" && a.kind != "CC rate";
    if (ok) {
    } else if (cardReads) {
        if (!takeGpu()) {
            if (index < (int)gpuRawHave_.size() && gpuRawHave_[(size_t)index]) {
                std::copy(gpuRaw_[(size_t)index].begin(), gpuRaw_[(size_t)index].end(), res.rpk);
                res.raw = true;
            }
            if (a.off >= 0 && index < (int)gpuTried_.size() && gpuTried_[(size_t)index] && ++a.misses > 250) { a.off = -1; a.misses = 0; }   // lost: searched again
        }
    } else if (g_.pal && mx - mn > 20 && a.kind != "CC 608" && !noTT && !takeGpu()) {
        double spb = fs / kWstRate;
        bool got = false;
        double bt = 0;
        // the templates trained on this capture (tplStart), as the recording's decoder reads: the packet found in
        // the window of the training, damaged bytes repaired; taken when the address is right and the text nearly
        // clean, else the line goes on as before
        // exact: the address as sent; else a bit of it may be corrected (the packets then often go to another row and
        // break the pages: 1212\s_estr_8_c.vbi 113 subpages of 20+ rows against 192), so only for a line nothing read
        // The line is read once; the second call only looks again at an address the first found broken
        std::vector<float> yt;
        legacy::tpl::Result r;
        int tplState = 0;   // 0 not read, 1 read (address broken), 2 done
        auto tplRead = [&](bool exact) {
            const LiveTpl* T = tplNow_.get();
            if (tplState == 2 || !T || index >= (int)T->src0.size() || T->src0[index] < 0) return;
            if (tplState == 0) {
                uint8_t d[2048];
                toBt(p, g_.fmt, n, fs, T->src0[index], d);
                yt = legacy::tpl::norm(d, 2048);
                r = T->dec.decode_fast(yt, T->lo, T->hi);
                tplState = 1;
            }
            {
                const auto* hx = exact ? ham::dec : ham::fix;
                int ad = hx[r.data[0]], c = hx[r.data[1]];
                if (ad >= 0 && c >= 0) {
                    tplState = 2;
                    int row = (ad >> 3) | (c << 1), k0 = row == 0 ? 10 : 2, bad = 0;
                    bool hamOk = true;
                    for (int k = 2; k < k0; k++) hamOk &= ham::fix[r.data[k]] >= 0;
                    if (row <= 25) {
                        for (int k = k0; k < 42; k++) bad += !oddParity(r.data[k]);
                        // the repair makes any bytes odd: only a packet the templates read nearly (as the MLSE takes)
                        if (bad > 12) hamOk = false;
                        else if (bad) { T->dec.repair(yt, r.off, r.data); bad = 0; for (int k = k0; k < 42; k++) bad += !oddParity(r.data[k]); }
                    }
                    bool take = row <= 25 ? hamOk && bad <= kTplParity : ham::dec[r.data[0]] >= 0 && ham::dec[r.data[1]] >= 0;
                    if (take) {
                        std::copy(r.data.begin(), r.data.end(), pk);
                        got = true; a.tplPackets++; a.misses = 0;
                        res.off = a.off;
                    }
                }
            }
        };
        // broadcast quality: run-in by correlation, sliced at its phase
        if ((a.off < 0 ? wstFind(y.data(), n, fs, bt) > 0.3 : (bt = a.off + spb / 2, true))) {
            for (double d : {0.0, -1.0, 1.0, -2.0, 2.0}) {
                if (wstRead(y.data(), n, fs, bt + d, pk) && usable(pk) && parityBad(pk) <= 2) { got = true; bt += d; break; }
                if (a.off < 0) break;
            }
            if (got) {
                a.off = a.off < 0 ? bt - spb / 2 : 0.9 * a.off + 0.1 * (bt - spb / 2);
                res.off = a.off;
            }
        }
        if (!got) tplRead(true);
        // tape: the channel-fitting MLSE of the original program, in a narrow window where the packet starts.
        // Teletext lines of one capture share the place; while a line has none of its own, once a second the
        // places that look like a run-in (normalised correlation) are tried as well
        double known = a.off >= 0 ? a.off : off_;
        bool forced = lineMode_[index] == kLiveLineTeletext || ((readAll_ || probeTT_[index]) && lineMode_[index] != kLiveLineOff);
        bool search = a.off < 0 && (forced || (frames_ + index) % 25 == 0);
        if (!got && (known >= 0 || search)) {
            if (!a.mlse) a.mlse = std::make_shared<LiveMlse>(spb);
            // a teletext line (named so by the probe or by hand): the tape decoder of a recording where the
            // packets of the line start - first when the narrow MLSE reads its text with parity errors (a worn
            // tape: the address then often reads clean but wrong), else when the narrow one read nothing.
            // Not on 8-bit data rows (res.raw), their bytes have no parity
            bool tapeLine = lineMode_[index] == kLiveLineTeletext || (probeTT_[index] && lineMode_[index] != kLiveLineOff);
            bool tapeFirst = tapeLine && known >= 0 && (a.parityAvg > 1.0 || a.addrDiff > 0.3);
            const double tapeWin = 8 * spb;   // the narrow MLSE may hold a place a bit or two off on a worn tape
            auto tape = [&] {
                if (!a.tape) a.tape = std::make_shared<LiveMlse>(spb, true);
                auto r = a.tape->dec.decode(raw.data(), n, std::max(0.0, known - tapeWin), known + tapeWin, 0.5);
                if (r.data.size() != 42 || !r.good || !usable(r.data.data())) return;
                // an 8-bit data row (its text was not re-decided): for the .raw.t42 and the files, not a text packet
                if (ham::dec[r.data[0]] >= 0 && ham::dec[r.data[1]] >= 0 && parityBad(r.data.data()) > 12) {
                    if (!res.raw) { std::copy(r.data.begin(), r.data.end(), res.rpk); res.raw = true; }
                    return;
                }
                std::copy(r.data.begin(), r.data.end(), pk);
                got = true; a.mlsePackets++; a.tapePackets++;
                a.off = a.off < 0 ? r.off : 0.9 * a.off + 0.1 * r.off;
                a.misses = 0;
                res.off = r.off;
            };
            if (tapeFirst) tape();
            std::vector<double> starts;
            bool cleanText = false;
            if (known >= 0) starts.push_back(known);
            if (search)
                for (double c : runInCandidates(y.data(), n, spb))
                    if (known < 0 || std::fabs(c - known) > 3) starts.push_back(c);
            for (double o : got ? std::vector<double>{} : starts) {
                auto r = a.mlse->dec.decode(raw.data(), n, std::max(0.0, o - 4), o + 4, 0.5);
                if (r.data.size() == 42 && !usable(r.data.data()) && a.off >= 0) mendHeaderAddress(r.data.data());
                // clean text under a broken address: the tape decoder decides the address below
                if (r.data.size() == 42 && !usable(r.data.data()) && a.off >= 0 && parityBad(r.data.data()) <= 2) cleanText = true;
                if (r.data.size() != 42 || !usable(r.data.data())) continue;
                // a line not yet known to carry teletext must read a clean packet first (test lines, noise)
                bool clean = ham::dec[r.data[0]] >= 0 && ham::dec[r.data[1]] >= 0 && parityBad(r.data.data()) <= 6;
                if (a.off >= 0 && parityBad(r.data.data()) > 12 && ham::dec[r.data[0]] >= 0 && ham::dec[r.data[1]] >= 0 && !res.raw) {
                    // 8-bit data row (a file sent in teletext, «Бизнес-Текст»): read by the MLSE as it is,
                    // kept for the .raw.t42 if nothing better comes
                    std::copy(r.data.begin(), r.data.end(), res.rpk);
                    res.raw = true;
                }
                if (a.off < 0 && !forced ? !clean : parityBad(r.data.data()) > 12) continue;
                // the tape decoder holds this line and failed on it: only a nearly clean packet (on a worn tape the
                // narrow MLSE reads a clean but wrong address with the text full of parity errors)
                if (tapeFirst && parityBad(r.data.data()) > 1) continue;
                std::copy(r.data.begin(), r.data.end(), pk);
                got = true; a.mlsePackets++;
                a.parityAvg = 0.95 * a.parityAvg + 0.05 * parityBad(r.data.data());
                a.off = a.off < 0 ? r.off : 0.9 * a.off + 0.1 * r.off;
                a.misses = 0;
                res.off = r.off;
                break;
            }
            if (!got && ((!res.raw && tapeLine) || cleanText) && known >= 0 && !tapeFirst) {
                tape();
                // a header whose address only the tape decoder read (its text clean) is left out: its magazine or
                // subcode is often another page's and it closes the page being received (БТ Оскар 1996.vbi: 30
                // subpages fewer, even with its page number checked against the pages seen)
                if (got && cleanText && ham::dec[pk[0]] >= 0 && !(ham::dec[pk[0]] >> 3) && ham::dec[pk[1]] == 0) got = false;
            }
            else if (got && tapeLine && !tapeFirst && (frames_ + index) % 25 == 0) {
                // once a second the tape decoder reads the line too: where the two often disagree on the address
                // (and the page number of a header) the narrow one is reading it wrong - the tape one takes over
                if (!a.tape) a.tape = std::make_shared<LiveMlse>(spb, true);
                auto r = a.tape->dec.decode(raw.data(), n, std::max(0.0, known - tapeWin), known + tapeWin, 0.5);
                if (r.data.size() == 42 && r.good) {
                    bool hdr = ham::dec[pk[0]] >= 0 && ham::dec[pk[1]] >= 0 && (ham::dec[pk[0]] >> 3 | ham::dec[pk[1]] << 1) == 0;
                    bool differ = r.data[0] != pk[0] || r.data[1] != pk[1] || (hdr && (r.data[2] != pk[2] || r.data[3] != pk[3]));
                    a.addrDiff = 0.8 * a.addrDiff + 0.2 * differ;
                }
            }
            if (!got && a.off >= 0 && ++a.misses > 250) { a.off = -1; a.misses = 0; }   // nothing for 10 s: search again
        }
        if (!got) tplRead(false);
        if (got) { ok = true; a.packets++; res.wst = true; }
        // a teletext line whose packet failed the parity test: sliced as is when its address is good
        // (8-bit binary rows of files sent in teletext; they go only to the .raw.t42)
        // A line with only binary rows never reads a clean packet: it is tried at the place shared by the
        // teletext lines (wstRead wants the framing code 27)
        else if (!res.raw && (a.off >= 0 || off_ >= 0)) {
            double o = a.off >= 0 ? a.off : off_;
            for (double d : {0.0, -1.0, 1.0}) {
                if (!wstRead(y.data(), n, fs, o + spb / 2 + d, res.rpk)) continue;
                if (ham::dec[res.rpk[0]] >= 0 && ham::dec[res.rpk[1]] >= 0) { res.raw = true; break; }
            }
        }
    }
    if (ok) {
    } else if (mx - mn > 20 && (forceCC || (!noTT && (a.kind == "CC rate" || a.kind == "CC 608")))) {   // 608 only where the clock says so
        if (legacy::cc_slice(y, fs, res.ccv, true)) { ok = true; a.ccOk++; res.cc = true; }
    }
    a.good = 0.95 * a.good + 0.05 * (ok ? 1 : 0);
    // VITC (SMPTE 12M / EBU): 90 bits a line, read where nothing else was
    if (!ok && mx - mn > 20 && !noTT) {
        VitcCode vc;
        float q = 0;
        if (vitcSlice(p, g_.fmt, n, (float)g_.lineSamples, g_.pal, vc, q)) { res.vitc = true; res.vitcText = vc.text(); }
    }
    a.vitcGood = 0.95 * a.vitcGood + 0.05 * res.vitc;
    // name of the line, looked at again every 25 frames
    if (--a.kindAge > 0) return res;
    a.kindAge = 25;
    // a few stray packets (2% of frames on lines 22 / 335 of a SECAM tape) do not make a teletext line
    if (a.vpsGood > 0.1) { a.kind = "VPS"; return res; }
    if (a.wssGood > 0.1) { a.kind = "WSS"; return res; }
    if (a.vitcGood > 0.1) { a.kind = "VITC"; return res; }
    if (a.good > (a.kind == "WST teletext" ? 0.1 : 0.2) && a.packets > 0) { a.kind = "WST teletext"; return res; }
    if (a.good > 0.3 && a.ccOk > 0) { a.kind = "CC 608"; return res; }
    if (a.level < 25) { a.kind.clear(); return res; }
    double best = 0; const char* bk = nullptr;
    for (auto& r : kRates) {
        // 525 lines: no WST 625, VPS, WSS or ТВ-информ (a Russian SECAM service)
        if (!g_.pal && (r.hz == kWstRate || r.hz == 5e6 || r.hz == 5e6 / 6 || (r.hz < 2e6 && r.hz > 1e6) || r.hz == 222 * 15625.0)) continue;
        double c = legacy::zc_coherence(y.data(), fs, r.hz, (int)(n * 0.08), (int)(n * 0.92));
        if (c > best) { best = c; bk = r.kind; }
    }
    a.kind = best > 0.55 && bk ? bk : "signal";
    return res;
}

// VPS / WSS once a second (under mu_): every byte voted over the readings of the last 2 s (a tape spoils single
// bits, a change of label shows within seconds); a label that differs from the last one in the history and
// holds for two votes goes to the history
void LiveDecoder::voteCodes()
{
    auto now = [] { time_t t = time(nullptr); char b[16]; strftime(b, sizeof b, "%H:%M:%S", localtime(&t)); return std::string(b); };
    vps_.frames = frames_;
    if (!vpsLast_.empty() && frames_ - vpsFrame_ < 125) {
        size_t from = vpsLast_.size() > 50 ? vpsLast_.size() - 50 : 0;
        uint8_t b[13];
        for (int k = 0; k < 13; k++) {
            int cnt[256] = {0}, bi = 0;
            for (size_t j = from; j < vpsLast_.size(); j++) cnt[(uint8_t)vpsLast_[j][k]]++;
            for (int i = 1; i < 256; i++) if (cnt[i] > cnt[bi]) bi = i;
            b[k] = (uint8_t)bi;
        }
        // the label part: sound (byte 5) .. programme type (15); bytes 3-4 are the broadcasters' own data
        std::string key((const char*)b + 2, 11);
        bool same = vps_.have && std::equal(b + 2, b + 13, vps_.bytes + 2);
        std::copy(b, b + 13, vps_.bytes);
        vps_.have = true;
        // the station's text of byte 4 (a character a frame): over the last 16 s of readings
        while (!vpsB4_.empty() && frames_ - vpsB4_.front().first > 400) vpsB4_.pop_front();
        // (read through frames the capture lost: a turn has one marked character; how many were lost is told too)
        VpsTextStats st;
        vps_.station = vpsStationText(std::vector<std::pair<int64_t, uint8_t>>(vpsB4_.begin(), vpsB4_.end()), &vps_.stationPeriod, &st);
        vps_.stretches = st.stretches; vps_.lossy = st.lossy; vps_.keepNum = st.keepNum; vps_.keepDen = st.keepDen;
        vps_.name = vpsDisplayName(vps_.station, vpsLabel(b).cni);
        // the time on the air: the teletext headers' date (dd.mm.yy with its weekday) and clock
        vps_.onAir.clear();
        if (hasClock_) {
            std::string clk;
            for (int k = 0; k < 8; k++) clk += clock_[k] >= 0x20 && clock_[k] < 0x7F ? (char)clock_[k] : ' ';
            std::string date;
            if (hasHeader_) {
                std::string t;
                for (int k = 8; k < 32; k++) t += header_[k] >= 0x20 && header_[k] < 0x7F ? (char)header_[k] : ' ';
                auto dg = [](char c) { return c >= '0' && c <= '9'; };
                for (size_t k = 0; k + 5 <= t.size(); k++) {
                    if (!(dg(t[k]) && dg(t[k + 1]) && t[k + 2] == '.' && dg(t[k + 3]) && dg(t[k + 4])) || (k && dg(t[k - 1]))) continue;
                    size_t e = k + 5, s0 = k;
                    if (e < t.size() && t[e] == '.') { e++; while (e < t.size() && dg(t[e]) && e < k + 10) e++; }
                    if (s0 >= 2 && t[s0 - 1] == ' ' && isalpha((unsigned char)t[s0 - 2])) { s0 -= 2; while (s0 && isalpha((unsigned char)t[s0 - 1])) s0--; }
                    date = t.substr(s0, e - s0);
                    while (!date.empty() && date.back() == '.') date.pop_back();
                    break;
                }
            }
            while (!clk.empty() && clk.front() == ' ') clk.erase(0, 1);
            vps_.onAir = (date.empty() ? "" : date + "  ") + clk;
        }
        // the other VPS-format lines read in the last 5 s; a clock line gives the time on the air when teletext does not
        vps_.dataLines.clear();
        for (auto& kv : vpsData_)
            if (frames_ - kv.second.second < 125 && !kv.second.first.empty()) {
                vps_.dataLines.push_back({kv.first, kv.second.first});
                if (vps_.onAir.empty() && isdigit((unsigned char)kv.second.first[0])) vps_.onAir = kv.second.first + "  (clock line " + std::to_string(kv.first) + ")";
            }
        if (!same) vps_.pending = 0;
        else if (++vps_.pending == 1 && (vps_.history.empty() || vps_.history.back().key != key))
            vps_.history.push_back({now(), frames_, key, vpsText(b)});
    }
    if (!wssLast_.empty() && frames_ - wssFrame_ < 125) {
        // bit by bit over the last readings: a reading with one bit wrong still counts for the other 13
        size_t from = wssLast_.size() > 50 ? wssLast_.size() - 50 : 0;
        int ones[14] = {}, n = (int)(wssLast_.size() - from), bv = 0;
        for (size_t j = from; j < wssLast_.size(); j++) for (int i = 0; i < 14; i++) ones[i] += wssLast_[j] >> i & 1;
        for (int i = 0; i < 14; i++) if (2 * ones[i] > n) bv |= 1 << i;
        bool same = vps_.wssHave && bv == vps_.wssBits;
        vps_.wssBits = bv;
        vps_.wssHave = true;
        if (!same) vps_.wssPending = 0;
        else if (++vps_.wssPending == 1 && (vps_.wssHistory.empty() || vps_.wssHistory.back().key != std::to_string(bv)))
            vps_.wssHistory.push_back({now(), frames_, std::to_string(bv), wssText(bv)});
    }
}

LiveVps LiveDecoder::vpsInfo()
{
    std::lock_guard<std::mutex> lk(mu_);
    LiveVps v = vps_;
    v.have = vps_.have && frames_ - vpsFrame_ < 125;
    v.wssHave = vps_.wssHave && frames_ - wssFrame_ < 125;
    if (frames_ - picWssAt_[0] < 50) v.wssLine = 23;   // read on the picture's top row (the VBI window ends at 22)
    v.everVps = vps_.have; v.everWss = vps_.wssHave;
    for (auto& ls : lineStats_) {
        if (ls.kind == "VPS" && (!v.vpsLine || ls.lineNo == vpsChosenTv_)) { v.vpsLine = ls.lineNo; v.vpsRate = ls.good; }
        if (ls.kind == "WSS") { v.wssLine = ls.lineNo; v.wssRate = ls.good; }
    }
    return v;
}

void LiveDecoder::packet(const uint8_t* p)
{
    using ham::fix;
    int a = fix[p[0]], b = fix[p[1]];
    if (a < 0 || b < 0) { bad_++; return; }
    packets_++;
    int m = a & 7, row = (a >> 3) | (b << 1);
    if (ham::dec[p[0]] < 0 || ham::dec[p[1]] < 0) {
        // one wrong bit in the address: on tape a bit slip turns 0x15 (0) into 0x2B / 0x2D, which
        // "correct" to 7 - so only a header whose page number and subcode read exactly is taken (as pagebuild)
        bool hdr = row == 0;
        for (int k = 2; k < 8 && hdr; k++) hdr = ham::dec[p[k]] >= 0;
        if (!hdr) return;
    }
    if (row == 0) {
        int h[8];
        // page number and subcode exactly: a corrected digit on tape is often another page's
        for (int k = 0; k < 8; k++) h[k] = k < 6 ? ham::dec[p[2 + k]] : fix[p[2 + k]];
        bool serial = h[7] >= 0 && (h[7] & 1);
        if (serial) for (int k = 0; k < 8; k++) closePage(k);
        else closePage(m);
        // the clock goes on with every header (also time filling ones), as on a TV: each character from its last clean reading
        for (int k = 32; k < 40; k++) if (oddParity(p[2 + k])) { clock_[k - 32] = p[2 + k] & 0x7F; hasClock_ = true; }
        if (h[0] < 0 || h[1] < 0 || h[0] > 9 || h[1] > 9) return;   // time filling / hex pages
        int pn = (m ? m : 8) * 0x100 + (h[1] << 4) + h[0];
        int sub = 0;
        if (h[2] >= 0 && h[3] >= 0 && h[4] >= 0 && h[5] >= 0) sub = h[2] | (h[3] & 7) << 4 | h[4] << 8 | (h[5] & 3) << 12;
        bool c4 = h[3] >= 0 && (h[3] & 8);
        // every transmission is received on its own until the page closes; then closePage puts it in its
        // subpage (by subcode, else by its "NN/MM" counter or text) - the window keeps showing the subpage
        // received before until the new one is whole, as a TV that waits for its page
        int key = kLiveReceiving;
        pages_[pn].erase(key);
        LivePage& pg = pages_[pn][key];
        pg.number = pn; pg.sub = sub; pg.national = national_ = national_of(h[7] >= 0 ? h[7] : 0); pg.frame = frames_;
        openErase_[m] = c4;
        pg.errors = 0;
        Row& r = pg.rows[0];
        const char* num = nullptr;
        char tag[9]; snprintf(tag, sizeof tag, "P%03X    ", pn); num = tag;   // 8 characters: a 0 would be "alpha black"
        for (int k = 0; k < 8; k++) r[k] = (uint8_t)num[k];
        for (int k = 8; k < 40; k++) {
            uint8_t c = p[2 + k];
            if (oddParity(c)) r[k] = c & 0x7F; else { if (!r[k]) r[k] = 0x20; pg.errors++; pg.bad[0] |= 1ull << k; }
        }
        header_ = r; hasHeader_ = true;
        // the channel name stays the same in every header while titles, numbers and the clock change
        {
            std::string text(r.begin() + 8, r.end()), w;
            lastHeaderText_ = text;
            headerPages_.insert(pn);
            auto flush = [&] {
                bool digit = false;
                for (char c : w) digit |= c >= '0' && c <= '9';
                if (w.size() >= 2 && !digit && headerWords_.size() < 2000) headerWords_[w].insert(pn);
                w.clear();
            };
            for (char c : text) { if ((uint8_t)c > 0x20 && (uint8_t)c < 0x7F) w += c; else flush(); }
            flush();
        }
        open_[m] = pn; openSub_[m] = key; openRow_[m] = 0;
        prevRow_[m] = 0; lastNew_[m] = hasSkip_[m] = false; prevAt_[m] = lastAt_[m] = packets_;
        return;
    }
    if (row <= 24) {
        if (open_[m] < 0) return;
        LivePage& pg = pages_[open_[m]][openSub_[m]];
        auto put = [&](int rw, const uint8_t* q) {
            auto at = pg.rows.find(rw);
            Row& r = at == pg.rows.end() ? (pg.rows[rw] = Row{}) : at->second;
            if (at == pg.rows.end()) r.fill(0x20);
            for (int k = 0; k < 40; k++) {
                uint8_t c = q[2 + k];
                if (oddParity(c)) { r[k] = c & 0x7F; pg.bad[rw] &= ~(1ull << k); } else { pg.errors++; if (at == pg.rows.end()) pg.bad[rw] |= 1ull << k; }
            }
            pg.frame = frames_;
        };
        // a packet between rows a and a+2 of its magazine (within 6 packets) is row a+1: the repair of a damaged
        // address picks a wrong Hamming code word (12, 5, 14) - as mend_rows of the recording's page builder
        auto mend = [&](const uint8_t* q, int ra, size_t since) {
            if (ra < 23 && !pg.rows.count(ra + 1) && packets_ - since <= 6) put(ra + 1, q);
        };
        auto it = pg.rows.find(row);
        if (row <= openRow_[m]) {
            // rows go up within a page: a lower row is either a repeat of one already received
            // or the next page whose header was lost - then its rows must not land here
            // - except the bottom rows sent first (Polsat 1996: header, 22, 23, 24, 1, 2, ...): a row not yet
            // received while the page has only rows 20+ goes on with it
            bool onlyLow = true;
            for (auto& rb : pg.rows) if (rb.first >= 1 && rb.first < 20) onlyLow = false;
            int d = 0;
            if (it != pg.rows.end())
                for (int k = 0; k < 40; k++) d += oddParity(p[2 + k]) && it->second[k] != (p[2 + k] & 0x7F);
            if ((it == pg.rows.end() && !onlyLow) || d > 4) {
                // one row out of order is a broken row number, not yet the next page (as export_pages):
                // the row before that jumped ahead (7, 20, 9) goes, else this one waits for the next row
                const int last = openRow_[m], before = prevRow_[m];
                if (row > before && last > before + 1 && lastNew_[m]) {
                    pg.rows.erase(last); pg.bad.erase(last);
                    openRow_[m] = before; lastAt_[m] = prevAt_[m];
                    if (row == before + 2) mend(lastPk_[m], before, prevAt_[m]);
                    hasSkip_[m] = false;
                } else if (!hasSkip_[m]) {
                    std::copy(p, p + 42, skip_[m]); hasSkip_[m] = true;
                    return;
                } else { closePage(m); return; }
            }
        }
        if (hasSkip_[m] && row == openRow_[m] + 2) mend(skip_[m], openRow_[m], lastAt_[m]);
        hasSkip_[m] = false;
        prevRow_[m] = openRow_[m]; prevAt_[m] = lastAt_[m];
        lastNew_[m] = !pg.rows.count(row); lastAt_[m] = packets_;
        std::copy(p, p + 42, lastPk_[m]);
        openRow_[m] = row;
        put(row, p);
        return;
    }
    if (m == 0 && row == 30) {   // 8/30 format 1: status display in bytes 22..41
        int dc = fix[p[2]];
        if (dc < 0 || dc > 1) return;
        ni_ = p[9] << 8 | p[10];
        std::string s;
        for (int k = 22; k < 42; k++) { uint8_t c = p[k]; s += oddParity(c) && (c & 0x7F) >= 0x20 ? (char)(c & 0x7F) : ' '; }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (!s.empty()) service_ = s;
    }
}

namespace {
// the page's own "NN/MM" on rows 1-24 (as pagebuild / Clean: «Самбия» 676 "06/16"), -1 = none
int pageCounter(const Rows& rows, int& mm)
{
    for (auto& rb : rows) {
        if (rb.first < 1 || rb.first > 24) continue;
        const Row& t = rb.second;
        auto dg = [&](int j) { return j >= 0 && j < 40 && t[j] >= '0' && t[j] <= '9'; };
        for (int i = 0; i + 5 <= 40; i++) {
            if (!dg(i) || !dg(i + 1) || t[i + 2] != '/' || !dg(i + 3) || !dg(i + 4)) continue;
            if ((i > 0 && (dg(i - 1) || t[i - 1] == '/')) || dg(i + 5) || (i + 5 < 40 && t[i + 5] == '/')) continue;
            int a = (t[i] - '0') * 10 + t[i + 1] - '0', b = (t[i + 3] - '0') * 10 + t[i + 4] - '0';
            if (a >= 1 && a <= b && b >= 2 && b <= 99) { mm = b; return a; }
        }
    }
    return -1;
}

int textRows(const LivePage& p)   // rows 1-24 with something on them
{
    int n = 0;
    for (auto& rb : p.rows) {
        if (rb.first < 1 || rb.first > 24) continue;
        int c = 0;
        for (uint8_t x : rb.second) c += x > 0x20;
        n += c >= 3;
    }
    return n;
}

// share of the text rows both pages have that read the same (a tape copy may differ in a few characters)
double sameRows(const LivePage& a, const LivePage& b, int& both)
{
    int same = 0;
    both = 0;
    for (auto& rb : a.rows) {
        if (rb.first < 1 || rb.first > 24) continue;
        auto it = b.rows.find(rb.first);
        if (it == b.rows.end()) continue;
        int ca = 0, cb = 0, d = 0;
        for (int k = 0; k < 40; k++) { ca += rb.second[k] > 0x20; cb += it->second[k] > 0x20; d += rb.second[k] != it->second[k]; }
        if (ca < 3 && cb < 3) continue;
        both++;
        same += d <= 4;
    }
    return both ? (double)same / both : 0;
}

bool mosaicRow(const Row& r)
{
    for (uint8_t c : r) { c &= 0x7F; if (c >= 0x10 && c <= 0x17) return true; }
    return false;
}
// characters two readings of a row differ in, where both read a sign (a space that did not come is no difference)
int textDiff(const Row& a, const Row& b)
{
    int d = 0;
    for (int k = 0; k < 40; k++) { uint8_t x = a[k] & 0x7F, y = b[k] & 0x7F; d += x != y && x > 0x20 && y > 0x20; }
    return d;
}
// rows of text that are another text (more than 10 characters differ): another page of a carousel or a new edition,
// not tape errors (ТЕЛЕИНФ 100: four announcements in rows 11-14 under 0000, the other 20 rows the same)
// bytes of a row of the transmission t that failed parity (tape damage)
int badIn(const LivePage& t, int r)
{
    auto b = t.bad.find(r);
    return b == t.bad.end() ? 0 : __builtin_popcountll(b->second);
}
// only a row t read cleanly tells: a damaged copy is not another page (ОРТ 2002 151: row 18 "чейЙИбРсиЙ ро3 ййс")
int otherRows(const LivePage& t, const LivePage& p)
{
    int n = 0;
    for (auto& rb : t.rows) {
        if (rb.first < 1 || rb.first > 23 || badIn(t, rb.first) > 3) continue;
        auto it = p.rows.find(rb.first);
        if (it == p.rows.end() || mosaicRow(rb.second) || mosaicRow(it->second)) continue;
        int ca = 0, cb = 0;
        for (int k = 0; k < 40; k++) { ca += rb.second[k] > 0x20; cb += it->second[k] > 0x20; }
        if (ca < 3 || cb < 3) continue;
        n += textDiff(rb.second, it->second) > 10;
    }
    return n;
}
}

// Pages with subcode 0000 that rotate several pages under it (no subcodes of their own) are told apart
// the way the recording's page builder does: by the page's own counter "NN/MM", else a whole
// transmission (8+ text rows) that shares less than half its rows with every subpage seen is a new one.
// Anything else updates the subpage it matches best (or the only one).
void LiveDecoder::closePage(int m)
{
    int pn = open_[m];
    open_[m] = -1;
    if (pn < 0) return;
    auto& subs = pages_[pn];
    auto it = subs.find(kLiveReceiving);
    if (it == subs.end()) return;
    LivePage t = it->second;
    subs.erase(it);
    int rows = textRows(t);
    // a header with no text after it (the next page's header came at once) is no transmission
    if (!rows && !subs.empty()) return;
    int mm = 0, nn = t.sub ? -1 : pageCounter(t.rows, mm);
    int key = 0;
    if (t.sub) key = t.sub;   // its own subcode
    else if (nn > 0) {
        key = 0x10000 + nn;
        t.counter = nn; t.of = mm;
    } else {
        // no counter read (a lost row, or a page without one): the subpage it shares its text with;
        // a piece that matches none of several subpages is dropped - it would mix two of them
        bool counted = false;
        for (auto& kv : subs) counted |= kv.first >= 0 && (kv.first & 0x10000);
        double best = -1;
        int bestKey = -1, whole = 0, cands = 0;
        for (auto& kv : subs) {
            if (kv.first < 0 || kv.second.sub) continue;   // own subcodes apart
            if (counted != ((kv.first & 0x10000) != 0)) continue;   // counted pages among themselves
            cands++;
            int both = 0;
            double s = sameRows(t, kv.second, both);
            if (both && s > best) { best = s; bestKey = kv.first; }
            whole += textRows(kv.second) >= 8;
        }
        if (counted) {
            if (bestKey < 0 || best < 0.5) return;
            key = bestKey;
        } else if (bestKey < 0) key = 0;
        else if (best >= 0.5 || (cands == 1 && (rows < 8 || !whole))) key = bestKey;
        else if (rows < 8) return;
        else if (!whole) key = bestKey;
        else {
            key = 0x20000;
            while (subs.count(key) && key < 0x20063) key++;
        }
    }
    // a page a station rotates under 0000 with only a few rows changing (ТЕЛЕИНФ 100): the transmission shares most
    // rows with the subpage chosen, but a row of text is another text - it goes to the subpage where nothing
    // differs, or is a new one; character votes would mix the pages ("НАСТАБИДЬНОС БАПНАИЛ")
    auto isAuto = [](int k, const LivePage& p) { return k >= 0 && !p.sub && !(k & 0x10000); };
    if (!t.sub && nn <= 0) {
        auto cur = subs.find(key);
        if (cur != subs.end() && otherRows(t, cur->second)) {
            int alt = -1;
            for (auto& kv : subs) {
                if (kv.first == key || !isAuto(kv.first, kv.second)) continue;
                int both = 0;
                if (sameRows(t, kv.second, both) >= 0.5 && both && !otherRows(t, kv.second)) { alt = kv.first; break; }
            }
            if (alt >= 0) key = alt;
            else if (rows < 8) return;   // a piece of another page of the carousel: it would mix them
            else {
                // a new page of the rotation once its text came twice; the first time it waits (not mixed in)
                auto& pend = pending_[pn];
                int seen = -1;
                for (size_t i = 0; i < pend.size(); i++) {
                    int both = 0;
                    if (sameRows(t, pend[i], both) >= 0.5 && both && !otherRows(t, pend[i]) && !otherRows(pend[i], t)) { seen = (int)i; break; }
                }
                if (seen < 0) {
                    pend.push_back(t);
                    if (pend.size() > 8) pend.erase(pend.begin());
                    ++pageTx_[pn];
                    return;
                }
                pend.erase(pend.begin() + seen);
                key = 0x20000;
                while (subs.count(key) && key < 0x20063) key++;
            }
        }
    }
    int64_t txNow = ++pageTx_[pn];
    // a subpage of the carousel not sent for a long while (four rounds of all of them and 40 transmissions more: a
    // rotation is still being gathered while its count grows, РТР 1995 205 has 27) was an old edition of an updated
    // page, not a page of the rotation: it goes
    {
        int autos = 0;
        for (auto& kv : subs) autos += isAuto(kv.first, kv.second);
        if (autos >= 2)
            for (auto it2 = subs.begin(); it2 != subs.end();) {
                if (it2->first != key && isAuto(it2->first, it2->second) && txNow - it2->second.tx > 4 * autos + 40) it2 = subs.erase(it2);
                else ++it2;
            }
    }
    auto ex = subs.find(key);
    if (ex != subs.end() && openErase_[m]) {
        // C4 (erase page): a new edition replaces the page; the same text sent again with C4 set
        // (many services set it always) is voted on like any repeat
        int both = 0;
        if (sameRows(t, ex->second, both) < 0.5 || !both) { subs.erase(ex); ex = subs.end(); }
    }
    bool fresh = ex == subs.end();
    LivePage& p = fresh ? subs[key] : ex->second;
    if (fresh) { p = t; p.rows.clear(); p.votes.clear(); }
    p.bad.clear();
    // every character read cleanly votes; the reading with the most votes is shown. A new reading pushes out
    // the weakest and weakens the others, so a changed page (a new edition) wins after 2 transmissions, a tape
    // error seen once does not
    for (auto& rb : t.rows) {
        int r = rb.first;
        auto bi = t.bad.find(r);
        uint64_t bad = bi == t.bad.end() ? 0 : bi->second;
        bool newRow = !p.rows.count(r);
        // a row of text sent as another text (a new edition of the row) replaces it whole: character votes would
        // keep letters of the old text among the new
        if (!newRow && r >= 1 && r <= 23 && __builtin_popcountll(bad) <= 3 && !mosaicRow(rb.second) && !mosaicRow(p.rows[r]) &&
            textDiff(p.rows[r], rb.second) > 10)
            newRow = true;
        Row& out = p.rows[r];
        if (newRow) out = rb.second;
        auto& vr = p.votes[r];
        if (newRow) for (auto& v : vr) v.fill(0);
        for (int k = 0; k < 40; k++) {
            if ((r == 0 && k < 8) || (bad >> k & 1)) continue;
            uint8_t c = rb.second[k];
            if (r == 0 && k >= 32) { out[k] = c; continue; }   // the clock changes every second
            auto& v = vr[k];   // val0 cnt0 val1 cnt1 val2 cnt2
            int s = -1;
            for (int i = 0; i < 3; i++) if (v[2 * i + 1] && v[2 * i] == c) s = i;
            if (s < 0) { s = 0; for (int i = 1; i < 3; i++) if (v[2 * i + 1] < v[2 * s + 1]) s = i; v[2 * s] = c; v[2 * s + 1] = 0; }
            if (v[2 * s + 1] < 6) v[2 * s + 1] += 2;
            for (int i = 0; i < 3; i++) if (i != s && v[2 * i + 1]) v[2 * i + 1]--;
            int best = s;
            for (int i = 0; i < 3; i++) if (v[2 * i + 1] > v[2 * best + 1]) best = i;
            out[k] = v[2 * best];
        }
    }
    p.frame = t.frame; p.errors = t.errors; p.national = t.national; p.tx = txNow;
    if (nn > 0) { p.counter = nn; p.of = mm; }
}

std::vector<uint8_t> liveReadTeletext(const std::string& path, const VbiGeometry& g, int64_t frames, bool gpu,
                                      const std::function<void(double, const std::string&)>& progress,
                                      const std::atomic<bool>* cancel, LiveReadStats& st, int64_t first)
{
    std::vector<uint8_t> out;
    st = LiveReadStats();
    FILE* fi = _wfopen(widen(path).c_str(), L"rb");
    int64_t fb = g.fileFrameBytes();
    if (!fi || fb <= 0 || g.linesPerFrame <= 0) { if (fi) fclose(fi); return out; }
    int L = g.linesPerFrame, n = g.validSamples, bps = g.fmt == 1 ? 2 : 1;
    std::vector<uint8_t> raw((size_t)n * bps);
    auto readLine = [&](int64_t f, int l, float* y) {
        _fseeki64(fi, (first + f) * fb + (int64_t)l * g.lineBytes, SEEK_SET);
        size_t got = fread(raw.data(), 1, raw.size(), fi);
        for (int i = 0; i < n; i++) {
            size_t k = (size_t)i * bps;
            y[i] = k + bps > got ? 0.0f : bps == 2 ? (raw[k] | (raw[k + 1] << 8)) / 256.0f : raw[k];
        }
    };
    double fs = g.lineSamples * g.lineRate, spb = fs / kWstRate;
    auto model = LiveMlse::model(spb, false);
    // 1. which lines carry teletext, and where the packet starts: a clean packet in a wide window
    legacy::MlseDecoder learn(model);
    double far = std::max(30.0, std::min(260.0, n - spb * 364));
    const int S = (int)std::min<int64_t>(frames, 160);
    std::vector<std::vector<double>> offs(L);
    std::vector<float> y(n);
    for (int i = 0; i < S; i++) {
        int64_t f = frames * i / S;
        for (int l = 0; l < L; l++) {
            readLine(f, l, y.data());
            auto r = learn.decode(y.data(), n, 0, far, 0.5);
            if (r.data.size() == 42 && ham::dec[r.data[0]] >= 0 && ham::dec[r.data[1]] >= 0 && parityBad(r.data.data()) <= 6) offs[l].push_back(r.off);
        }
        if (progress) progress(0.1 * i / S, "Teletext: finding the lines");
        if (cancel && *cancel) { fclose(fi); return out; }
    }
    std::vector<double> start(L, -1);
    for (int l = 0; l < L; l++)
        if ((int)offs[l].size() >= std::max(3, S / 50)) {
            std::sort(offs[l].begin(), offs[l].end());
            start[l] = offs[l][offs[l].size() / 2];
            st.lines.push_back(l);
        }
    if (st.lines.empty() || !learn.has_prior()) { fclose(fi); return out; }
    // 2. every frame of those lines in batches
    std::vector<std::vector<std::pair<int, std::array<uint8_t, 42>>>> byFrame((size_t)frames);
    legacy::Progress lp;
    size_t total = st.lines.size() * (size_t)frames, done = 0;
    for (int l : st.lines) {
        legacy::MlseBatch bd(model, learn.prior, learn.n, gpu, lp);
        st.onGpu |= bd.on_gpu();
        std::vector<float> buf;
        for (int64_t b = 0; b < frames; b += (int64_t)bd.batch()) {
            int64_t m = std::min<int64_t>((int64_t)bd.batch(), frames - b);
            buf.resize((size_t)m * n);
            std::vector<const float*> rp((size_t)m);
            for (int64_t i = 0; i < m; i++) { readLine(b + i, l, &buf[(size_t)i * n]); rp[(size_t)i] = &buf[(size_t)i * n]; }
            auto res = bd.decode(rp, n, std::max(0.0, start[l] - 4), start[l] + 4, nullptr, 0.5);
            for (int64_t i = 0; i < m && i < (int64_t)res.size(); i++) {
                auto& d = res[(size_t)i].data;
                if (d.size() != 42) continue;
                if (!usable(d.data()) && mendHeaderAddress(d.data())) st.mended++;
                if (!usable(d.data()) || parityBad(d.data()) > 12) continue;
                std::array<uint8_t, 42> a;
                std::copy(d.begin(), d.end(), a.begin());
                byFrame[(size_t)(b + i)].push_back({l, a});
            }
            done += (size_t)m;
            if (progress) progress(0.1 + 0.9 * done / std::max<size_t>(1, total), "Teletext: reading the lines");
            if (cancel && *cancel) { fclose(fi); return out; }
        }
    }
    fclose(fi);
    for (auto& fr : byFrame) {
        std::sort(fr.begin(), fr.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& p : fr) out.insert(out.end(), p.second.begin(), p.second.end());
    }
    st.packets = out.size() / 42;
    return out;
}

// the teletext lines of one frame read on the graphics card (OpenCL): the lines whose packet starts at the same
// sample go in one call of the MLSE kernel, so the card does the work a frame at a time, not a line at a time
struct LiveGpuBatch {
    std::unique_ptr<legacy::MlseBatch> bd;       // the narrow model (as the processor's MLSE of a line)
    std::unique_ptr<legacy::MlseBatch> bdTape;   // the tape model, for the lines the narrow one could not read
};

void LiveDecoder::setGpu(bool on)
{
    gpuWanted_ = on;
    if (!on) { gp_.reset(); gpuDead_ = false; gpuGenericTried_ = false; }
    else gpuStart();                            // nothing happens before the processor has learnt the model
}
bool LiveDecoder::gpuActive()
{
    return gp_ && gp_->bd && gp_->bd->on_gpu();
}

void LiveDecoder::gpuStart()
{
    if (!gpuWanted_ || gpuDead_ || g_.linesPerFrame <= 0 || !g_.pal) return;   // the WST 625 models
    if (gp_ && gp_->bdTape) return;             // both models on the card
    const double spb = (g_.lineSamples * g_.lineRate) / kWstRate;
    // the models learnt for the card in their own thread (below): the narrow one reads, the tape one (wide channel,
    // codewords) reads again the lines the narrow one could not, as the processor's tape decoder did
    std::vector<double> prior, tapePrior;
    int n = 0, tapeN = 0;
    bool learnt = false;
    if (gpuLearn_) {
        std::lock_guard<std::mutex> lk(gpuLearn_->m);
        if (gpuLearn_->done) {
            learnt = true;
            if (gpuLearn_->n >= 6) { prior = gpuLearn_->prior; n = gpuLearn_->n; }
            if (gpuLearn_->tapeN >= 6) { tapePrior = gpuLearn_->tapePrior; tapeN = gpuLearn_->tapeN; }
        }
    }
    if (prior.empty() && master_ && master_->has_prior() && master_->n >= 6) { prior = master_->prior; n = (int)master_->n; }
    // (tried and dropped: putting the models learnt by the processor from this capture's lines in place of the
    // start-up ones once they are ready - ntv.vbi from frame 0: 5002 packets against 5325 without; the card's own
    // learning from the packets it reads does better)
    // nothing learnt yet (the first frames, or a capture with no teletext at all): the card starts at once with the
    // model of a clean line and learns this capture's channel from what it reads - the processor is not kept
    // reading and searching until there is teletext (the user: "it still decodes on the processor")
    if (!gp_ && prior.empty() && !gpuGenericTried_) {
        gpuGenericTried_ = true;
        LiveMlse::generic(spb, g_.validSamples, false, prior, n);
        if (!prior.empty()) LiveMlse::generic(spb, g_.validSamples, true, tapePrior, tapeN);
    }
    if (!gp_ && !prior.empty()) {
        try {
            auto g = std::make_shared<LiveGpuBatch>();
            legacy::Progress lp;
            g->bd = std::make_unique<legacy::MlseBatch>(LiveMlse::model(spb, false), prior, n, true, lp);
            if (!g->bd || !g->bd->on_gpu()) { gpuDead_ = true; return; }   // no OpenCL here: the processor as before
            gp_ = g;
        } catch (std::exception&) {
            gpuDead_ = true;                    // a broken driver must not take the capture down with it
            return;
        }
    }
    if (gp_ && !gp_->bdTape && !tapePrior.empty()) {
        try {
            legacy::Progress lp;
            auto t = std::make_unique<legacy::MlseBatch>(LiveMlse::model(spb, true), tapePrior, tapeN, true, lp);
            if (t && t->on_gpu()) gp_->bdTape = std::move(t);
        } catch (std::exception&) {}
    }
    if (gp_ && (gp_->bdTape || (learnt && frames_ - gpuLearnAt_ < 1500))) return;
    // not learnt yet: the MLSE of the processor learns the models from the teletext lines of the last frames (the
    // lines found so far), in a thread, so the capture does not wait; again every 10 s until it has (the tape model
    // every 60 s when its lines did not read)
    bool busy = false;
    if (gpuLearn_) { std::lock_guard<std::mutex> lk(gpuLearn_->m); busy = !gpuLearn_->done; }
    if (busy || frames_ - gpuLearnAt_ < 250) return;
    const int L = g_.linesPerFrame, ns = g_.validSamples, bps = g_.fmt == 1 ? 2 : 1;
    std::vector<std::pair<std::vector<float>, double>> lines;   // samples, where the packet starts
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = ring_.rbegin(); it != ring_.rend() && lines.size() < 200; ++it)
            for (int l = 0; l < L && l < (int)acc_.size() && lines.size() < 200; l++) {
                if (lineMode_[l] == kLiveLineOff || acc_[l].kind != "WST teletext") continue;
                double off = acc_[l].off >= 0 ? acc_[l].off : off_;
                if (off < 0 || it->size() < (size_t)(l + 1) * g_.lineBytes) continue;
                const uint8_t* s = it->data() + (size_t)l * g_.lineBytes;
                std::vector<float> y((size_t)ns);
                for (int i = 0; i < ns; i++) { size_t q = (size_t)i * bps; y[i] = bps == 2 ? (float)(s[q] | (s[q + 1] << 8)) / 256.0f : (float)s[q]; }
                lines.push_back({std::move(y), off});
            }
    }
    if (lines.size() < 20) return;               // no teletext lines found yet
    gpuLearnAt_ = frames_;
    auto job = std::make_shared<GpuLearn>();
    gpuLearn_ = job;
    std::thread([job, lines = std::move(lines), spb, ns] {
        lowPriority(true);
        LiveMlse m(spb), t(spb, true);
        for (auto& x : lines) m.dec.decode(x.first.data(), ns, std::max(0.0, x.second - 4), x.second + 4, 0.5);
        for (auto& x : lines) t.dec.decode(x.first.data(), ns, std::max(0.0, x.second - 8 * spb), x.second + 8 * spb, 0.5);
        std::lock_guard<std::mutex> lk(job->m);
        job->prior = m.dec.prior; job->n = m.dec.n;
        job->tapePrior = t.dec.prior; job->tapeN = t.dec.n;
        job->done = true;
    }).detach();
}

// the card reads every teletext line of the frame, in as few calls as the places of the packets allow; the lines it
// could not read go once more to the card's tape model; lines with no teletext found yet are searched on the card
// once a second. Nothing of it is done on the processor while the card works (the processor's MLSE on the 26
// other lines of a SECAM capture took 1.3 s of processor time a second of video)
namespace legacy { extern thread_local bool parallel_serial; }   // legacy/src/vbidecode.cpp

void LiveDecoder::gpuFrame(const uint8_t* frame)
{
    // the card's calls are small (a frame's lines): their processor parts run in this thread, not in new threads
    struct Serial { Serial() { legacy::parallel_serial = true; } ~Serial() { legacy::parallel_serial = false; } } serial;
    const int L = std::max(0, g_.linesPerFrame), n = g_.validSamples, bps = g_.fmt == 1 ? 2 : 1;
    if ((int)gpuHave_.size() != L) { gpuLine_.resize((size_t)L); gpuRaw_.resize((size_t)L); }
    gpuHave_.assign((size_t)L, 0);
    gpuTried_.assign((size_t)L, 0);
    gpuRawHave_.assign((size_t)L, 0);
    gpuOff_.assign((size_t)L, -1.0);
    if (!gp_ || !gp_->bd || n <= 0) return;
    const double spb = (g_.lineSamples * g_.lineRate) / kWstRate;
    std::vector<float> buf;
    // one call of a model for these lines, the window lo..hi (samples where the packet may start)
    auto run = [&](legacy::MlseBatch& bd, const std::vector<int>& idx, double lo, double hi, std::vector<legacy::MlseResult>& res) {
        buf.resize(idx.size() * (size_t)n);
        std::vector<const float*> rows(idx.size());
        for (size_t k = 0; k < idx.size(); k++) {
            const uint8_t* s = frame + (size_t)idx[k] * g_.lineBytes;
            float* y = &buf[k * (size_t)n];
            for (int i = 0; i < n; i++) {
                size_t q = (size_t)i * bps;
                y[i] = bps == 2 ? (float)(s[q] | (s[q + 1] << 8)) / 256.0f : (float)s[q];
            }
            rows[k] = y;
        }
        try {
            static const bool timing = getenv("TR_GPUTIME") != nullptr;   // study: time of the card's calls
            auto t0 = std::chrono::steady_clock::now();
            auto cpu = processCpuSeconds;
            const double c0 = timing ? cpu() : 0;
            res = bd.decode(rows, n, std::max(0.0, lo), std::max(lo, hi), nullptr, 0.5);
            if (timing) {
                const int tape = &bd == gp_->bdTape.get();
                static double sum[2] = {0, 0}, cs[2] = {0, 0}; static int calls[2] = {0, 0};
                sum[tape] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); cs[tape] += cpu() - c0; calls[tape]++;
                if (calls[tape] % 250 == 0)
                    fprintf(stderr, "gpu decode %s: %d calls, %.1f ms a call, processor %.1f ms a call, %zu lines\n", tape ? "tape  " : "narrow", calls[tape],
                            1000 * sum[tape] / calls[tape], 1000 * cs[tape] / calls[tape], idx.size());
            }
            return true;
        } catch (std::exception&) {
            gpuDead_ = true; gp_.reset();       // the card went away: the processor reads the next frame
            return false;
        }
    };
    // lines by where their packet starts, those within 'gap' samples in one call
    struct Group { double lo, hi; std::vector<int> lines; };
    auto group = [](std::vector<std::pair<double, int>> at, double gap) {
        std::sort(at.begin(), at.end());
        std::vector<Group> g;
        for (auto& x : at) {
            if (g.empty() || x.first - g.back().lo > gap) g.push_back({x.first, x.first, {}});
            g.back().hi = x.first;
            g.back().lines.push_back(x.second);
        }
        return g;
    };
    // a line with no place of its own yet (not teletext, or lost) is searched once a second (every frame when set to
    // teletext by hand or when the probe named it so)
    auto searchTurn = [&](int l) {
        const LineAcc& a = acc_[l];
        if (lineMode_[l] == kLiveLineOff || a.off >= 0 || a.level < 20) return false;
        if (a.kind == "CC 608" || a.kind == "VPS" || a.kind == "WSS" || a.kind == "VITC") return false;
        const bool forced = lineMode_[l] == kLiveLineTeletext || ((readAll_ || probeTT_[l]) && lineMode_[l] != kLiveLineOff);
        return forced || (frames_ + l) % 25 == 0;
    };
    // 1. the teletext lines, and the lines searched where this capture's teletext starts (one call for them all)
    std::vector<std::pair<double, int>> at, failed;
    std::vector<char> searched((size_t)L, 0);
    for (int l = 0; l < L && l < (int)acc_.size(); l++) {
        // a teletext line, or a line the card found a packet on (its name comes within a second)
        if (lineMode_[l] == kLiveLineOff) continue;
        if (acc_[l].kind != "WST teletext" && acc_[l].off < 0) {
            if (off_ >= 0 && searchTurn(l)) { at.push_back({off_, l}); searched[(size_t)l] = 1; }
            continue;
        }
        double off = acc_[l].off >= 0 ? acc_[l].off : off_;
        if (off >= 0) at.push_back({off, l});
    }
    for (auto& gr : group(at, 8)) {
        std::vector<legacy::MlseResult> res;
        if (!run(*gp_->bd, gr.lines, gr.lo - 4.0, gr.hi + 4.0, res)) return;
        for (size_t k = 0; k < gr.lines.size() && k < res.size(); k++) {
            const int l = gr.lines[k];
            auto& d = res[k].data;
            gpuTried_[(size_t)l] = 1;
            if (d.size() != 42) continue;
            if (!usable(d.data())) mendHeaderAddress(d.data());
            const int bad = parityBad(d.data());
            if (searched[(size_t)l]) {   // a line searched: only a clean packet makes it teletext
                if (!usable(d.data()) || ham::dec[d[0]] < 0 || ham::dec[d[1]] < 0 || bad > 6) continue;
            }
            if (usable(d.data()) && bad <= 12) {
                std::copy(d.begin(), d.end(), gpuLine_[(size_t)l].begin());
                gpuHave_[(size_t)l] = 1;
                gpuOff_[(size_t)l] = res[k].off;
                continue;
            }
            // 8-bit data row (a file sent in teletext, «Бизнес-Текст»): kept for the .raw.t42 if nothing better comes
            if (ham::dec[d[0]] >= 0 && ham::dec[d[1]] >= 0 && bad > 12) {
                std::copy(d.begin(), d.end(), gpuRaw_[(size_t)l].begin());
                gpuRawHave_[(size_t)l] = 1;
            }
            if (!searched[(size_t)l]) failed.push_back({acc_[l].off >= 0 ? acc_[l].off : off_, l});
        }
    }
    // 2. the card's tape model on the lines the narrow one could not read (a worn tape), its window wider; taken
    // when its reading is good (the address and text re-decided among codewords), as the processor's tape decoder.
    // Not on the 8-bit data rows: their bytes have no parity
    if (gp_ && gp_->bdTape && !failed.empty()) {
        std::vector<std::pair<double, int>> tf;
        for (auto& x : failed) if (!gpuRawHave_[(size_t)x.second]) tf.push_back(x);
        for (auto& gr : group(tf, 8)) {
            std::vector<legacy::MlseResult> res;
            if (!run(*gp_->bdTape, gr.lines, gr.lo - 8 * spb, gr.hi + 8 * spb, res)) return;
            for (size_t k = 0; k < gr.lines.size() && k < res.size(); k++) {
                const int l = gr.lines[k];
                auto& d = res[k].data;
                if (d.size() != 42 || !res[k].good || !usable(d.data())) continue;
                std::copy(d.begin(), d.end(), gpuLine_[(size_t)l].begin());
                gpuHave_[(size_t)l] = 1;
                gpuOff_[(size_t)l] = res[k].off;
            }
        }
    }
    if (!gp_) return;
    // 3. lines with no place of their own yet (not teletext, or lost): each once a second (every frame when set to
    // teletext by hand or when the probe named it so), where the teletext lines of this capture start, or anywhere
    // the packet fits when none is known yet; taken when the packet reads clean
    // (with the place known they went with the teletext lines above: here only while no teletext line is known)
    std::vector<int> search;
    for (int l = 0; l < L && l < (int)acc_.size(); l++)
        if (!gpuTried_[(size_t)l] && searchTurn(l)) search.push_back(l);
    if (!search.empty()) {
        // the packet fills the line: it starts in its first quarter (~115 samples of 1440 at 27 MHz)
        const double lo = off_ >= 0 ? off_ - 8 : 0, hi = off_ >= 0 ? off_ + 8 : n * 0.25;
        std::vector<legacy::MlseResult> res;
        if (!run(*gp_->bd, search, lo, hi, res)) return;
        for (size_t k = 0; k < search.size() && k < res.size(); k++) {
            const int l = search[k];
            auto& d = res[k].data;
            gpuTried_[(size_t)l] = 1;
            if (d.size() != 42 || !usable(d.data())) continue;
            if (ham::dec[d[0]] < 0 || ham::dec[d[1]] < 0 || parityBad(d.data()) > 6) continue;   // a clean packet first
            std::copy(d.begin(), d.end(), gpuLine_[(size_t)l].begin());
            gpuHave_[(size_t)l] = 1;
            gpuOff_[(size_t)l] = res[k].off;
        }
    }
    gpuFrames_++;
}

std::vector<uint8_t> LiveDecoder::sessionPackets()
{
    std::lock_guard<std::mutex> lk(mu_);
    return session_;
}

void LiveDecoder::cc(int a, int b)
{
    a &= 0x7F; b &= 0x7F;
    if (a == 0 && b == 0) return;
    auto newline = [&] { if (!cc_.empty() && cc_.back() != '\n') cc_ += '\n'; };
    if (a >= 0x10 && a <= 0x1F) {
        bool repeat = a == ccLast_[0] && b == ccLast_[1];
        ccLast_[0] = a; ccLast_[1] = b;
        if (repeat) return;   // control codes are sent twice
        if ((a & 0x77) == 0x14 && (b == 0x2C || b == 0x2F || b == 0x2D)) newline();
        else if (b >= 0x40) newline();   // preamble address code: a new row
        else if ((a & 0x77) == 0x11 && b >= 0x30 && b <= 0x3F) {
            char32_t c = cc_special(b);
            if (c) cc_ += from_u32(std::u32string(1, c));
        }
    } else {
        ccLast_[0] = ccLast_[1] = -1;
        for (int c : {a, b})
            if (c >= 0x20) cc_ += (char)c;
    }
    if (cc_.size() > 4000) cc_.erase(0, cc_.size() - 3000);
}

void LiveDecoder::snapshot(LiveSnapshot& s, int page)
{
    std::lock_guard<std::mutex> lk(mu_);
    s.geom = g_;
    s.frames = frames_;
    s.packets = packets_; s.badPackets = bad_;
    { std::lock_guard<std::mutex> q(qmu_); s.dropped = dropped_; }
    s.frame = last_;
    for (int k = 0; k < 2; k++) { s.picRow[k] = picRow_[k]; s.picWss[k] = frames_ - picWssAt_[k] < 50; }
    for (int k = 2; k < 8 && k < picRowsGot_; k++) s.picRow[k] = picRow_[k];
    s.picRows = picRowsGot_;
    s.lines = lineStats_;
    for (auto& st : s.lines) { auto it = probeKind_.find(st.lineNo); if (it != probeKind_.end()) st.probe = it->second; }
    s.list.clear();
    for (auto& kv : pages_) {
        int n = (int)kv.second.size();
        if (n > 1 && kv.second.count(kLiveReceiving)) n--;
        s.list.push_back({kv.first, n});
    }
    s.pages.clear();
    if (page && pages_.count(page)) s.pages[page] = pages_[page];
    s.header = header_; s.hasHeader = hasHeader_;
    memcpy(s.clock, clock_, sizeof clock_); s.hasClock = hasClock_;
    // VPS / WSS (voted in voteCodes), shown while they come
    // one name first (the register's when the station text is that name, else the station's own: "NOVY.TV. (CNI 7C5) · ...")
    s.vps.clear();
    if (vps_.have && frames_ - vpsFrame_ < 125) {
        std::string t = vpsText(vps_.bytes);
        size_t c = t.find(" (CNI ");
        s.vps = vps_.name.empty() || c == std::string::npos ? t : vps_.name + t.substr(c);
    }
    s.wss = vps_.wssHave && frames_ - wssFrame_ < 125 ? wssText(vps_.wssBits) : "";
    s.service = service_;
    s.charset = charset_;
    s.ni = ni_;
    s.t42Packets = t42Packets_;
    s.channel.clear();
    if (headerPages_.size() >= 5) {   // words in at least 60 % of the pages, in the order of the last header
        const Charset& t = charset_table(charset_.empty() ? "latin" : charset_, national_);
        std::string w;
        auto take = [&] {
            auto it = headerWords_.find(w);
            if (!w.empty() && it != headerWords_.end() && it->second.size() * 10 >= headerPages_.size() * 6) {
                std::u32string u;
                for (char c : w) u += t[(uint8_t)c - 0x20];
                if (!s.channel.empty()) s.channel += " ";
                s.channel += from_u32(u);
            }
            w.clear();
        };
        for (char c : lastHeaderText_) { if ((uint8_t)c > 0x20 && (uint8_t)c < 0x7F) w += c; else take(); }
        take();
    }
    s.captions = cc_;
    s.packetsPerSecond = 0;
    if (rate_.size() > 1) {
        double fr = (double)(rate_.back().first - rate_.front().first);
        double fps = g_.pal ? 25.0 : 30000.0 / 1001;
        if (fr > 0) s.packetsPerSecond = (rate_.back().second - rate_.front().second) / fr * fps;
    }
}

bool LiveDecoder::record(const std::string& path, std::string& err, bool writeVbi)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> q(qmu_);
    if (rec_) { fclose(rec_); rec_ = nullptr; }
    if (t42_) { fclose(t42_); t42_ = nullptr; }
    if (raw_) { fclose(raw_); raw_ = nullptr; }
    recOn_ = false;
    if (path.empty()) return true;
    std::string base = path;
    size_t dot = base.find_last_of("./\\");
    if (dot != std::string::npos && base[dot] == '.') base.resize(dot);
    auto open = [](const std::string& p) {
#ifdef _WIN32
        return _wfopen(widen(p).c_str(), L"wb");
#else
        return fopen(p.c_str(), "wb");
#endif
    };
    if (writeVbi) rec_ = open(path);
    t42_ = open(base + ".t42");
    raw_ = open(base + ".raw.t42");
    if (writeVbi ? !rec_ : !t42_) {
        if (t42_) fclose(t42_);
        if (raw_) fclose(raw_);
        t42_ = raw_ = nullptr;
        err = "cannot create " + path;
        return false;
    }
    recFrames_ = 0; recPath_ = path; recOn_ = true; t42Packets_ = 0;
    // the first VBI line of the recording when it is not the layout's own (a window moved or widened)
    if (writeVbi && g_.firstLine != formatGeometry(g_.format).firstLine)
        if (FILE* f = open(path + ".lines")) { fprintf(f, "%d\n", g_.firstLine); fclose(f); }
    // the layout of the recording, so the decoder opens it so (a 1440 x 36 SECAM file is also 1440 x 24 NTSC by size)
    if (writeVbi)
        if (FILE* f = open(path + ".info")) {
            fprintf(f, "format %s, %dx%d@%dHz %s\n", formatKey(g_.format), g_.lineBytes, g_.linesPerFrame, g_.pal ? 25 : 30, g_.pal ? "PAL/SECAM" : "NTSC");
            fclose(f);
        }
    return true;
}

// ---------------------------------------------------------------- files found live
std::string LiveDecoder::filesDir()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!recPath_.empty() && recOn_) return recPath_.substr(0, recPath_.find_last_of('.')) + "_vbi";
    return (std::filesystem::temp_directory_path() / "TeletextRescue_live").u8string();
}

std::vector<Find> LiveDecoder::finds()
{
    std::vector<Find> out;
    BtCatalog bt;
    TtDataPages td;
    std::vector<BtWritten> written;
    std::string note;
    bool write = false;
    int64_t now = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        out = tviFinds_;
        bt = bt_;
        now = frames_;
        td = td_;
        written = btWritten_;
        note = btNote_;
        // the «Бизнес-Текст» files: again when 30 s passed and new data rows came
        long rows = bt_.dataRows();
        if (rows > btRows_ && (btFrames_ < 0 || frames_ - btFrames_ >= 25 * 30) && !btBusy_) {
            write = true;
            btFrames_ = frames_;
            btRows_ = rows;
        }
    }
    if (write) {
        if (btThread_.joinable()) btThread_.join();
        btBusy_ = true;
        std::string dir = path_join(filesDir(), "biznestext_files");
        btThread_ = std::thread([this, bt, dir] {
            std::string n;
            std::vector<BtWritten> w;
            try { w = bt.writeFiles(dir, n); } catch (...) {}
            {
                std::lock_guard<std::mutex> lk(mu_);
                btWritten_ = w;
                btNote_ = n;
            }
            btBusy_ = false;
        });
    }
    addBtFinds(out, bt, "teletext lines", written, note, now);
    addTtDataFinds(out, td, "teletext lines");
    // telesoftware: made again when new blocks came (at most every 5 s), the files written then
    std::vector<TsProgram> ts;
    bool tsWrite = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (ts_.blocksSeen() != tsSeen_ && frames_ - tsAt_ >= 25 * 5) {
            tsProgs_ = ts_.programs();
            tsSeen_ = ts_.blocksSeen();
            tsAt_ = frames_;
            tsWrite = true;
        }
        ts = tsProgs_;
    }
    if (!ts.empty()) addTelesoftFinds(out, ts, "teletext lines", path_join(filesDir(), "telesoftware"), tsWrite);
    return out;
}

std::string LiveDecoder::btStatus(bool shortForm)
{
    std::string dir = path_join(filesDir(), "biznestext_files");
    std::lock_guard<std::mutex> lk(mu_);
    if (bt_.empty()) return "";
    int64_t at = -1;
    std::string now = bt_.nowSent(&at);
    auto fl = bt_.files(1);
    long rows = bt_.dataRows();
    int dec = 0;
    for (auto& w : btWritten_) dec += w.decrypted;
    std::string s = "\xD0\x91\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB5\xD1\x81-\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82: ";
    if (shortForm) {
        s += fmt("%zu files", fl.size());
        if (!now.empty() && frames_ - at < 25 * 10) s += ", now " + now.substr(0, now.find(" (page"));
        s += btBusy_ ? ", decrypting\xE2\x80\xA6" : !btWritten_.empty() ? fmt(", %d decrypted", dec) : "";
        return s;
    }
    s += fmt("catalogue of %zu files, blocks of %d of them received (%ld rows)", fl.size(), bt_.filesWithData(), rows);
    if (long m = bt_.mendedRows()) s += fmt(", %ld rows sent apart from their blocks put back", m);
    if (!now.empty()) s += "  \xC2\xB7  " + std::string(frames_ - at < 25 * 10 ? "on the air now: " : "last sent: ") + now;
    if (btBusy_) s += "  \xC2\xB7  finding the key and writing the files\xE2\x80\xA6";
    else if (!btWritten_.empty()) s += fmt("  \xC2\xB7  %zu files written, %d decrypted \xE2\x86\x92 ", btWritten_.size(), dec) + dir;
    else if (rows) s += "  \xC2\xB7  the files are written every 30 s while new blocks come";
    else s += "  \xC2\xB7  waiting for the blocks (rows 1-20 of the catalogue pages)";
    if (!btNote_.empty() && !btBusy_) s += "  \xC2\xB7  " + btNote_;
    return s;
}

// runs in the probe thread on one chunk of frames (the last 5 s) written to vbiPath:
// ТВ-информ lines (fТ1 / fТ2) are found and sliced, Norpak packets (55 55 E7 on lines 7-9 / 320-322, or any
// line the probe calls NABTS: ORT 2001 17-19 / 331-333) go to the session's .t33 with their places (.pos,
// frames counted in the session, so the packet counter goes by slot as in a recording) and through the
// error correction; the files they carry are written out
void LiveDecoder::findFiles(const std::string& vbiPath, const std::string& fmt, const std::vector<uint8_t>& frames,
                            const std::map<int, std::string>& kinds, const std::set<int>& teletextLines, int64_t firstFrame,
                            std::map<int, std::string>& names)
{
    const VbiGeometry g = g_;
    const size_t fb = (size_t)g.frameBytes();
    const int n = fb ? (int)(frames.size() / fb) : 0;
    if (n < 10 || !g.pal) return;
    const int half = g.linesPerFrame / 2;
    auto tvOf = [&](int l) { return g.firstLine + l % half + (l >= half ? 313 : 0); };
    const float fs = g.lineSamples * (float)g.lineRate;
    static int64_t chunk = 0;
    const int64_t base = chunk++ * n;
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    int tLines = 0, tSlice = 0, tFec = 0;
    // ТВ-информ fТ3 (RTR 2001): the header FB AC on the line; set by hand or found (and then kept)
    for (int l = 0; l < g.linesPerFrame; l++) {
        const int pf = l < 64 ? probeForce_[l] : 0;
        const bool hand3 = pf == (int)LineForce::TviT3;
        if ((pf && !hand3) || (manual_ && !hand3)) continue;
        // a line read as teletext is not fТ3 (both have the 444 x fH run-in; a teletext line gives the header test
        // a share of hits on a weak tape, 2с.vbi lines 21 / 334)
        if (!hand3 && teletextLines.count(l)) { tvi3Lines_.erase(l); continue; }
        if (!hand3 && !tvi3Lines_.count(l)) {
            const int pn = std::min(n, 50);
            int h = tvi3TestLine(frames.data(), pn, fb, (size_t)l * g.lineBytes, g.fmt, g.validSamples, fs);
            if (h * 10 < pn * 3) continue;
        }
        tvi3Lines_.insert(l);
        auto& sl = tvi3Slicers_[l];
        if (!sl) sl = std::make_unique<Tvi3Slicer>(fs, true);
        for (int f = 0; f < n; f++)
            sl->add(&frames[(size_t)f * fb + (size_t)l * g.lineBytes], g.fmt, g.validSamples, base + f, g.firstLine + l % half, l < half ? 1 : 2);
        auto w = sl->take();
        tvi3Live_.words.insert(tvi3Live_.words.end(), w.begin(), w.end());
        names[tvOf(l)] = "TV-Inform fТ3, RTR (file transfer)";
    }
    if (tvi3Live_.words.size() > 400000) tvi3Live_.words.erase(tvi3Live_.words.begin(), tvi3Live_.words.begin() + 100000);
    // ТВ-информ lines
    for (int l = 0; l < g.linesPerFrame; l++) {
        if (tvi3Lines_.count(l)) continue;
        const int pf = l < 64 ? probeForce_[l] : 0;
        const bool tviHand = pf == (int)LineForce::TviT1 || pf == (int)LineForce::TviT2;
        if (pf && !tviHand) continue;   // set by hand to another signal
        if (manual_ && !tviHand) continue;
        if (teletextLines.count(l) && !tviLive_.count(l) && !tviHand) continue;
        auto k = kinds.find(tvOf(l));
        std::string kind = k == kinds.end() ? "" : k->second;
        bool capJob = kind.rfind("CC", 0) == 0 || kind.find("0.5 Mbit") != std::string::npos;
        TviLineTest t = tviTestLine(frames.data(), std::min(n, 50), fb, (size_t)l * g.lineBytes, g.fmt, g.validSamples, fs,
                                    tviHand ? (pf == (int)LineForce::TviT1 ? 0 : 1) : -1, capJob && !tviHand);
        if (t.rate < 0) continue;
        TviStream& st = tviLive_[l];
        st.rate = t.pairs ? 0 : t.rate;
        st.nbits = kTviBits[st.rate];
        for (int f = 0; f < n; f++) {
            TviWord w;
            if (!tviSlice(&frames[(size_t)f * fb + (size_t)l * g.lineBytes], g.fmt, g.validSamples, fs / kTviRateHz[t.rate], kTviBits[t.rate], w.bytes, w.quality))
                continue;
            if (t.pairs) w.bytes = tviCollapse(w.bytes, kTviBits[1]);
            w.frame = base + f;
            w.lineNo = g.firstLine + l % half;
            w.field = l < half ? 1 : 2;
            st.words.push_back(std::move(w));
        }
        if (st.words.size() > 60000) st.words.erase(st.words.begin(), st.words.begin() + 20000);
    }
    // a line found once stays ТВ-информ: fТ1 with the idle carrier is not seen in every 5 s chunk
    for (auto& kv : tviLive_)
        if (!kv.second.words.empty() && (!teletextLines.count(kv.first) || (kv.first < 64 && probeForce_[kv.first])))
            names[tvOf(kv.first)] = "TV-Inform";
    // lines set by hand to a signal read only when the recording is decoded: named so
    for (int l = 0; l < g.linesPerFrame && l < 64; l++) {
        LineForce f = (LineForce)probeForce_[l];
        if (f == LineForce::SilentRadio || f == LineForce::Amol || f == LineForce::Gemstar || f == LineForce::Vitc || f == LineForce::Test)
            names[tvOf(l)] = std::string(lineForceName(f)) + " (set by hand)";
    }
    tLines = ms();
    // Norpak packets
    std::string dir = filesDir();
    std::vector<TviFile> files;
    bool norpak = false;
    auto gostPk = [](int tv) { return (tv >= 7 && tv <= 9) || (tv >= 320 && tv <= 322); };
    std::set<int> pkLines;   // 625 lines: 55 55 E7 packets on any line are ТВ-информ (NABTS never ran on 625-line TV)
    std::set<int> pkHand;    // set by hand to Norpak packets
    for (int l = 0; l < g.linesPerFrame && l < 64; l++)
        if (probeForce_[l] == (int)LineForce::Norpak || probeForce_[l] == (int)LineForce::Fast) { pkHand.insert(tvOf(l)); pkLines.insert(tvOf(l)); norpak = true; }
    for (auto& kv : kinds) {
        if (manual_) break;   // only the lines set by hand
        bool nabts = kv.second.find("NABTS") != std::string::npos;
        if (nabts || (gostPk(kv.first) && (kv.second.find("Didon") != std::string::npos || kv.second.find("TV-Inform") != std::string::npos))) {
            norpak = true;
            pkLines.insert(kv.first);
        }
    }
    if (norpak || !t33_.empty()) {
        // the lines of the service: those the probe called packet lines in most of the chunks since it first
        // saw them. Only these are sliced - every other line read as packets (test signals on 7-9 / 320-322
        // of ORT 2001) would be slots of the packet counter too
        if (norpak) norpakProbes_++;
        for (int tv : pkLines) norpakVotes_[tv]++;
        norpakLines_.clear();
        for (auto& kv : norpakVotes_) if (kv.second * 2 >= norpakProbes_) norpakLines_.insert(kv.first);
        for (int tv : pkHand) norpakLines_.insert(tv);
        for (int tv : norpakLines_) names[tv] = "TV-Inform packets (Norpak, 55 55 E7)";
        legacy::Rec R(vbiPath, fmt);
        std::vector<int> rows;
        for (int r : R.rows) {
            int tv = R.tv.count(r) ? R.tv.at(r) : 0;
            if (norpakLines_.count(tv)) rows.push_back(r);
        }
        std::string tmp = vbiPath + ".t33";
        if (!rows.empty()) {
            legacy::Progress pr;
            try { legacy::slice525(R, rows, legacy::Svc525::NABTS, tmp, pr); } catch (...) {}
        }
        if (exists(tmp)) {
            Bytes b = read_file(tmp), pb;
            if (exists(tmp + ".pos")) pb = read_file(tmp + ".pos");
            std::remove(tmp.c_str());
            std::remove((tmp + ".pos").c_str());
            if (!b.empty()) {
                if (t33_.empty()) t33_ = (std::filesystem::temp_directory_path() / ("TeletextRescue_live_" + std::to_string((uintptr_t)this) + ".t33")).u8string();
                // places: frame of the chunk -> frame of the session; kept only while every packet has one
                if (pb.size() == b.size() / 33 * 8 && (!exists(t33_) || exists(t33_ + ".pos"))) {
                    for (size_t i = 0; i + 8 <= pb.size(); i += 8) {
                        uint32_t fr = (uint32_t)(pb[i] | pb[i + 1] << 8 | pb[i + 2] << 16 | pb[i + 3] << 24) + (uint32_t)firstFrame;
                        for (int k = 0; k < 4; k++) pb[i + k] = (uint8_t)(fr >> (8 * k));
                    }
                    if (FILE* f = fopen((t33_ + ".pos").c_str(), "ab")) { fwrite(pb.data(), 1, pb.size(), f); fclose(f); }
                } else
                    std::remove((t33_ + ".pos").c_str());
                if (FILE* f = fopen(t33_.c_str(), "ab")) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
            }
        }
        tSlice = ms();
        // the whole session's packets go through the error correction again (the file blocks of a carousel
        // come in any order): every 4th chunk = 20 s, so the probe keeps up with the capture
        if (!t33_.empty() && exists(t33_) && (++norpakChunk_ >= 4 || norpakFiles_.empty())) {
            norpakChunk_ = 0;
            TviNorpak tn;
            if (tviNorpakDecode(t33_, dir, tn)) { norpakFiles_ = tn.files; norpakBeacons_ = tn.beacons; }
            if (getenv("TR_DEBUG")) {
                std::string ls;
                for (int tv : norpakLines_) ls += ::fmt(" %d", tv);
                fprintf(stderr, "norpak: lines%s, rows %zu, %lld packets, %lld bundles, %lld corrected, %zu beacons\n", ls.c_str(), rows.size(),
                        (long long)tn.packets, (long long)tn.bundles, (long long)tn.bundlesOk, tn.beacons.size());
            }
        }
        files = norpakFiles_;
        tFec = ms();
    }
    std::vector<Find> out;
    if (!files.empty() || !norpakBeacons_.empty()) {
        TviStream ts;
        ts.rate = -1;
        ts.fileInfo = files;
        ts.beacons = norpakBeacons_;
        std::string where;
        for (int tv : norpakLines_) where += (where.empty() ? "lines " : ", ") + ::fmt("%d", tv);
        addTviFinds(out, ts, where.empty() ? "lines 7-9 / 320-322" : where);
    }
    if (!tvi3Live_.words.empty()) {
        TviStream c = tvi3Live_;
        c.rate = 2; c.nbits = kTviBits[2];
        std::stable_sort(c.words.begin(), c.words.end(), [](const TviWord& x, const TviWord& y) { return x.frame != y.frame ? x.frame < y.frame : x.field < y.field; });
        tvi3Build(c, dir, 25.0);
        std::string where;
        for (int l : tvi3Lines_) where += (where.empty() ? "lines " : ", ") + ::fmt("%d", tvOf(l));
        addTviFinds(out, c, where);
    } else if (!tvi3Lines_.empty()) {
        // the lines are found but their MLSE is still learning the channel: say so, the words follow
        Find x;
        x.kind = Find::Text;
        int got = 0, need = 0;
        std::string where;
        for (int l : tvi3Lines_) {
            where += (where.empty() ? "lines " : ", ") + ::fmt("%d", tvOf(l));
            auto it = tvi3Slicers_.find(l);
            if (it != tvi3Slicers_.end() && it->second) got += it->second->learnt(), need += it->second->toLearn();
        }
        x.name = "file transfer stream (RTR): lines found, learning the channel";
        x.type = "records 7E 80 80 80 80, CRC-32";
        x.service = "\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC (f\xD0\xA2" "3, 6.94 Mbit/s)";   // ТВ-информ, as addTviFinds
        x.where = where;
        x.status = ::fmt("learning: %d of %d lines with data", got, need);
        x.note = "the data lines come out after the learning (up to ~30 s of data on the line)";
        out.push_back(x);
    }
    for (auto& kv : tviLive_) {
        if (kv.second.words.empty()) continue;
        TviStream c = kv.second;
        tviBuild(c);
        addTviFinds(out, c, ::fmt("line %d field %d", c.words[0].lineNo, c.words[0].field));
    }
    if (getenv("TR_DEBUG")) {
        fprintf(stderr, "findFiles chunk %lld (ms: lines %d, slice %d, fec %d, all %d): norpak %d, %zu files, finds %zu;", (long long)base / std::max(1, n),
                tLines, tSlice, tFec, ms(), (int)norpak, files.size(), out.size());
        for (auto& kv : tviLive_) fprintf(stderr, " line %d: %zu words", kv.first, kv.second.words.size());
        fprintf(stderr, "\n");
    }
    std::lock_guard<std::mutex> lk(mu_);
    tviFinds_ = std::move(out);
    filesDir_ = dir;
}

// 525 lines (a probe chunk of the last ~4 s written to vbiPath): the chunk is read as a recording of the capture's
// layout, its NABTS packets (55 55 E7, CEA-516 records) and JTES lines (NABTS sync, (272,190) codewords) join the
// session's; every 4th chunk the pages are made again from all of them (a carousel's groups come in any order)
void LiveDecoder::find525(const std::string& vbiPath, int64_t frames, int64_t firstFrame, std::map<int, std::string>& names)
{
    if (frames < 10) return;
    DecodeOptions o;
    o.force = g_.format;
    o.threads = 2;
    o.maxFrames = frames;   // a part of a recording: no results.trc kept
    JtesRecRead found;
    o.jtesHow = jtesHow_.slots.empty() ? nullptr : &jtesHow_;
    o.jtesFound = &found;
    o.jtesProbe = jtesTries_ < 3;
    DecodeResult r;
    std::string err;
    bool ok = decodeFile(vbiPath, o, r, err);
    if (getenv("TR_DEBUG")) fprintf(stderr, "find525: %lld frames of %s: %s, %zu lines, %zu 5.7 Mbit/s packets\n", (long long)frames, formatKey(o.force), ok ? "decoded" : err.c_str(), r.lines.size(), r.fastPackets.size());
    if (jtesHow_.slots.empty()) {
        if (!found.slots.empty()) jtesHow_ = found;
        else if (o.jtesProbe) jtesTries_++;
    }
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(legacyReportDir(vbiPath)), ec);
    std::filesystem::remove_all(std::filesystem::u8path(ttWorkDir(vbiPath)), ec);
    if (!ok) return;
    bool jtes = false;
    for (auto& li : r.lines)   // every line as the decoder names it
        if (!li.kind.empty()) names[li.lineNo + (li.field == 2 ? 263 : 0)] = li.kind;
    for (auto& li : r.lines)
        if (li.mode == MODE_FAST && (li.kind == "NABTS" || li.kind.find("JTES") != std::string::npos)) {
            jtes |= li.kind.find("JTES") != std::string::npos;
        }
    if (!r.db.empty()) ss_.merge(r.db);
    size_t gemWords = 0;
    for (auto& gs : r.gemstar) {
        auto& acc = gemWords_[{gs.lineNo, gs.field}];
        for (auto w : gs.words) { w.frame += firstFrame; acc.push_back(w); }
        gemWords += gs.words.size();
    }
    const size_t ssNow = ss_.programs.size() + ss_.showLists.size() + ss_.channels.size();
    for (auto& p : r.fastPackets)
        if (p.kind == 'N' || (p.kind == 'E' && jtes)) {
            p.frame += firstFrame;
            fast525_.push_back(std::move(p));
        }
    if (fast525_.size() > 3000000) fast525_.erase(fast525_.begin(), fast525_.begin() + 1000000);   // ~1 hour of 8 lines
    const bool ssNew = ssNow != ssMade_ || gemWords > 0;
    if (!ssNew && (fast525_.size() == fast525Made_ || (++fast525Chunk_ < 4 && fast525Made_))) return;
    ssMade_ = ssNow;
    fast525Chunk_ = 0;
    fast525Made_ = fast525_.size();
    auto R = std::make_shared<DecodeResult>();
    R->path = recPath_.empty() ? "live capture" : recPath_;
    R->geom = g_;
    for (auto& li : r.lines) if (li.mode == MODE_FAST || li.kind.find("StarSight") != std::string::npos) R->lines.push_back(li);
    R->db = ss_;
    for (auto& kv : gemWords_) {
        GemstarStream s;
        s.lineNo = kv.first.first;
        s.field = kv.first.second;
        s.words = kv.second;
        gemstarDecode(s);
        R->gemstar.push_back(std::move(s));
    }
    if (!R->gemstar.empty()) {
        gemstarGuide(R->gemstar);
        for (auto& s : R->gemstar) gemstarBuildReport(s);
    }
    for (auto& li : r.lines) if (li.mode == MODE_GEMSTAR) R->lines.push_back(li);
    jtesDecode(fast525_, R->jtes);
    for (auto& pk : fast525_)
        if (pk.kind == 'N' && pk.bytes.size() >= 36) {
            R->nabtsT33.insert(R->nabtsT33.end(), pk.bytes.begin() + 3, pk.bytes.begin() + 36);
            R->nabtsField.push_back((int)(pk.frame * 2 + pk.field - 1));
            R->nabtsLine.push_back(pk.lineNo);
        }
    if (!R->nabtsT33.empty()) {
        try {
            R->nabtsRecords = nabts::read_packets(R->nabtsT33, R->nabtsSummary);
            nabts::interpret(R->nabtsRecords, 256, 200);
        } catch (...) {}
    }
    if (getenv("TR_DEBUG")) fprintf(stderr, "find525: %zu packets, JTES %zu pages, NABTS %zu records\n", fast525_.size(), R->jtes.pages.size(), R->nabtsRecords.size());
    std::lock_guard<std::mutex> lk(mu_);
    res525_ = R;
}

std::shared_ptr<DecodeResult> LiveDecoder::pages525()
{
    std::lock_guard<std::mutex> lk(mu_);
    return res525_;
}

// a whole picture frame (YUY2, interlaced: even rows field 1, odd rows field 2): the row of each field that
// carries WSS (line 23 / 336) is looked for in the top 160 rows - the card's sync (Behold «Frame sync» off) moves
// the picture so that line 23 lands lower than its first row - once a second and while none is known; that row is
// then read as line 23 / 336 on every frame
void LiveDecoder::pictureFrame(const uint8_t* p, int w, int h, int stride, double fs)
{
    if (w <= 0 || h < 2) return;
    bool interlaced = h > 300;
    auto luma = [&](int row, std::vector<uint8_t>& out) {
        out.resize(w);
        const uint8_t* s = p + (size_t)row * stride;
        for (int x = 0; x < w; x++) out[x] = s[x * 2];
    };
    int64_t fr;
    { std::lock_guard<std::mutex> lk(mu_); fr = frames_; }
    for (int k = 0; k < 2; k++) {
        bool search = picRowIdx_[k] < 0 || fr - picRowCheck_[k] >= 25;
        if (!search) continue;
        picRowCheck_[k] = fr;
        std::vector<uint8_t> r;
        int found = -1;
        for (int row = interlaced ? k : 0; row < std::min(h, 160); row += interlaced ? 2 : 1) {
            luma(row, r);
            std::vector<float> y(r.begin(), r.end());
            int bits = 0;
            if (wssRead(y.data(), w, fs, bits)) { found = row; break; }
        }
        if (found >= 0) picRowIdx_[k] = found;
        else if (picRowIdx_[k] >= 0 && fr - picRowSeen_[k] > 250) picRowIdx_[k] = -1;   // not seen for 10 s: search again
    }
    int r0 = picRowIdx_[0] >= 0 ? picRowIdx_[0] : 0, r1 = picRowIdx_[1] >= 0 ? picRowIdx_[1] : (interlaced ? 1 : 0);
    std::vector<uint8_t> a, b;
    luma(r0, a);
    luma(r1, b);
    pictureRows(a.data(), b.data(), w, fs);
    // the lines that follow 23 / 336 (24, 337, 25 ...) as well, as many as the capture window asks for: a VCR writes
    // its own text or a data line right there, above or below the driver's window
    const int want = std::clamp(picRowsWanted_, 2, 8);
    if (want > 2) {
        std::vector<uint8_t> r;
        std::lock_guard<std::mutex> lk(mu_);
        for (int k = 2; k < want; k++) {
            luma(std::min(h - 1, k), r);
            picRow_[k] = r;
        }
        picRowsGot_ = want;
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        picRowsGot_ = 2;
    }
}

void LiveDecoder::setPictureRows(int n)
{
    std::lock_guard<std::mutex> lk(mu_);
    picRowsWanted_ = std::clamp(n, 1, 8);
}
int LiveDecoder::pictureRowsNow()
{
    std::lock_guard<std::mutex> lk(mu_);
    return picRowsWanted_;
}

// line 23 / 336 from the picture: WSS (EN 300 294) on the row of either field (pictureFrame finds it); read on every
// picture frame, voted with the VBI lines' readings
void LiveDecoder::pictureRows(const uint8_t* row0, const uint8_t* row1, int w, double fs)
{
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t* rows[2] = {row0, row1};
    for (int k = 0; k < 2; k++) {
        picRow_[k].assign(rows[k], rows[k] + w);
        if (!g_.pal) continue;
        std::vector<float> y(rows[k], rows[k] + w);
        int bits = 0;
        if (!wssRead(y.data(), w, fs, bits)) continue;
        picWssAt_[k] = frames_;
        picRowSeen_[k] = frames_;
        wssLast_.push_back(bits);
        if (wssLast_.size() > 250) wssLast_.pop_front();
        wssFrame_ = frames_;
        vps_.wssReads++;
    }
}

bool liveRepairMeasure(const std::string& path, VbiFormat format, const VbiRepair& r, int64_t from, int64_t count,
                       VbiRepairMeasure& m, std::string& err)
{
    VbiFile vf;
    if (!vf.open(path, err, format)) return false;
    const VbiGeometry g = vf.geom();
    if (g.storedPerField) { err = "full-field .tbc files are not measured"; return false; }
    from = std::clamp<int64_t>(from, 0, std::max<int64_t>(g.frames - 1, 0));
    count = std::min(count, g.frames - from);
    int64_t warm = std::min<int64_t>(from, 50);
    std::vector<uint8_t> buf;
    if (count <= 0 || vf.read(from - warm, warm + count, buf) < warm + count) { err = "no frames"; return false; }
    m = VbiRepairMeasure();
    m.lines.resize(g.linesPerFrame);
    for (int l = 0; l < g.linesPerFrame; l++) m.lines[l].index = l;
    for (int pass = 0; pass < 2; pass++) {
        LiveDecoder d;
        d.reset(g);
        if (pass) d.setRepair(r);
        for (int64_t k = 0; k < warm; k++) d.feed(&buf[(size_t)k * g.frameBytes()]);
        LiveSnapshot s0;
        d.snapshot(s0, 0);
        size_t p0 = d.sessionPackets().size();
        for (int64_t k = warm; k < warm + count; k++) d.feed(&buf[(size_t)k * g.frameBytes()]);
        LiveSnapshot s1;
        d.snapshot(s1, 0);
        for (auto& ls : s1.lines) {
            if (ls.index < 0 || ls.index >= g.linesPerFrame) continue;
            int before = 0;
            for (auto& o : s0.lines) if (o.index == ls.index) before = o.packets;
            m.lines[ls.index].packets[pass] = ls.packets - before;
        }
        auto pk = d.sessionPackets();
        for (size_t i = p0; i + 42 <= pk.size(); i += 42) {
            const uint8_t* p = &pk[i];
            m.packets[pass]++;
            int a0 = ham::dec[p[0]], a1 = ham::dec[p[1]];
            int row = a0 >= 0 && a1 >= 0 ? (a0 >> 3) | (a1 << 1) : -1;
            if (row < 1 || row > 23) continue;
            for (int k = 2; k < 42; k++) { m.bytes[pass]++; m.bad[pass] += !(__builtin_popcount(p[k]) & 1); }
        }
        d.stopWorker();
        d.joinProbe();
    }
    return true;
}

void LiveDecoder::setRepair(const VbiRepair& r)
{
    std::lock_guard<std::mutex> lk(repMu_);
    repairer_.set(r);
    repairer_.reset();
    repOn_ = r.active();
}

void LiveDecoder::setLineOffset(int d)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (g_.firstLine == baseFirstLine_ + d) return;
    g_.firstLine = baseFirstLine_ + d;
    probeKind_.clear();   // named by the old numbering: the next probe chunk names them again
}
