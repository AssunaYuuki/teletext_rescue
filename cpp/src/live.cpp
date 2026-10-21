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
    // Hamming codewords and text among odd-parity bytes - but only after the free reading of the address was
    // clean (MlseResult::good), so a line without a packet is not made one
    static legacy::MlseModel model(double spb, bool tape)
    {
        auto m = legacy::wst_mlse_model();
        m.spb = spb;
        if (tape) {
            m.L = 6; m.R = 5; m.ntop = 2; m.refits = 1;
            m.good = [](const uint8_t* p) { return ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0; };
            m.code_kind = 1;
            m.code = [](const uint8_t* p, int i) -> int {
                if (i < 2) return 1;
                int a = ham::dec[p[0]], b = ham::dec[p[1]];
                if (a < 0 || b < 0) return 0;
                int row = (a >> 3) | (b << 1);
                if (row == 0) return i < 10 ? 1 : 2;
                if (row <= 25) return 2;
                if (row == 27) return i < 40 ? 1 : 0;
                if (row >= 26 && row <= 29) return i == 2 ? 1 : 0;
                return 0;
            };
        }
        return m;
    }
};

namespace {
// decoding runs below the capture threads: when the processor is busy the card must not lose fields
void lowPriority(bool lowest)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), lowest ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)lowest;
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
    {
        std::lock_guard<std::mutex> q(qmu_);
        frameBytes_ = g.frameBytes();
        dropped_ = 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    g_ = g;
    baseFirstLine_ = g.firstLine;
    frames_ = 0; packets_ = bad_ = 0;
    acc_.assign(g.linesPerFrame, {});
    lineStats_.clear();
    last_.clear();
    pages_.clear();
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
    vpsLast_.clear(); wssLast_.clear(); vpsFrame_ = wssFrame_ = -1;
    vps_ = LiveVps();
    btWritten_.clear(); btNote_.clear(); btFrames_ = -1; btRows_ = 0;
    td_ = TtDataPages();
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
    std::vector<LineOut> out(L);
    std::atomic<int> next{0};
    auto work = [&] { lowPriority(false); for (int l; (l = next++) < L;) out[l] = line(l, frame + (size_t)l * g_.lineBytes); };
    int T = std::min(L, std::max(1, (int)std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (int t = 1; t < T; t++) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
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
        } else if (out[l].raw) {
            if (raw_) fwrite(out[l].rpk, 1, 42, raw_);
            bt_.packet(out[l].rpk, frames_);
        }
        if (out[l].cc && l < L / 2) cc(out[l].ccv[0], out[l].ccv[1]);
        if (out[l].vps) {
            vpsLast_.push_back(std::string((const char*)out[l].vpsb, 13));
            if (vpsLast_.size() > 250) vpsLast_.pop_front();
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
    if (g_.pal && !noTT && !ok && ((tv >= 22 && tv <= 24) || a.kind == "WSS") && wssRead(raw.data(), n, fs, res.wssv)) res.wss = ok = true;
    a.vpsGood = 0.95 * a.vpsGood + 0.05 * res.vps;
    a.wssGood = 0.95 * a.wssGood + 0.05 * res.wss;
    if (ok) {
    } else if (g_.pal && mx - mn > 20 && a.kind != "CC 608" && !noTT) {
        double spb = fs / kWstRate;
        bool got = false;
        double bt = 0;
        // broadcast quality: run-in by correlation, sliced at its phase
        if (a.off < 0 ? wstFind(y.data(), n, fs, bt) > 0.3 : (bt = a.off + spb / 2, true)) {
            for (double d : {0.0, -1.0, 1.0, -2.0, 2.0}) {
                if (wstRead(y.data(), n, fs, bt + d, pk) && usable(pk) && parityBad(pk) <= 2) { got = true; bt += d; break; }
                if (a.off < 0) break;
            }
            if (got) {
                a.off = a.off < 0 ? bt - spb / 2 : 0.9 * a.off + 0.1 * (bt - spb / 2);
                res.off = a.off;
            }
        }
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
                std::copy(r.data.begin(), r.data.end(), pk);
                got = true; a.mlsePackets++; a.tapePackets++;
                a.off = a.off < 0 ? r.off : 0.9 * a.off + 0.1 * r.off;
                a.misses = 0;
                res.off = r.off;
            };
            if (tapeFirst) tape();
            std::vector<double> starts;
            if (known >= 0) starts.push_back(known);
            if (search)
                for (double c : runInCandidates(y.data(), n, spb))
                    if (known < 0 || std::fabs(c - known) > 3) starts.push_back(c);
            for (double o : got ? std::vector<double>{} : starts) {
                auto r = a.mlse->dec.decode(raw.data(), n, std::max(0.0, o - 4), o + 4, 0.5);
                if (r.data.size() == 42 && !usable(r.data.data()) && a.off >= 0) mendHeaderAddress(r.data.data());
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
            if (!got && !res.raw && tapeLine && known >= 0 && !tapeFirst) tape();
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
        if (ls.kind == "VPS") { v.vpsLine = ls.lineNo; v.vpsRate = ls.good; }
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
            if (oddParity(c)) r[k] = c & 0x7F; else { if (!r[k]) r[k] = 0x20; pg.errors++; }
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
        return;
    }
    if (row <= 24) {
        if (open_[m] < 0) return;
        LivePage& pg = pages_[open_[m]][openSub_[m]];
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
            if ((it == pg.rows.end() && !onlyLow) || d > 4) { closePage(m); return; }
        }
        openRow_[m] = row;
        Row& r = it == pg.rows.end() ? (pg.rows[row] = Row{}) : it->second;
        if (it == pg.rows.end()) r.fill(0x20);
        for (int k = 0; k < 40; k++) {
            uint8_t c = p[2 + k];
            if (oddParity(c)) r[k] = c & 0x7F; else pg.errors++;
        }
        pg.frame = frames_;
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
    auto ex = subs.find(key);
    if (ex == subs.end() || openErase_[m]) { subs[key] = t; return; }
    LivePage& p = ex->second;   // rows sent again replace those of the earlier transmission
    for (auto& rb : t.rows) p.rows[rb.first] = rb.second;
    p.frame = t.frame; p.errors = t.errors; p.national = t.national;
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
    // VPS / WSS (voted in voteCodes), shown while they come
    s.vps = vps_.have && frames_ - vpsFrame_ < 125 ? vpsText(vps_.bytes) : "";
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        out = tviFinds_;
        bt = bt_;
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
    addBtFinds(out, bt, "teletext lines", written, note);
    addTtDataFinds(out, td, "teletext lines");
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
