#include "decoder.h"
#include "jtes.h"
#include "flac.h"
#include "colour.h"
#include "tvinorpak.h"
#include "rescache.h"
#include "slicer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include "fastdata_core.h"
#include "dvbts.h"

namespace {

constexpr float kCcScore = 1.5f; // run-in found and "0 0 1" before the data
constexpr float kSrScore = 2.2f; // Silent Radio preamble matched

// (92nd - 8th percentile) of a line as a share of full scale
static float lineSwing(const uint8_t* p, int fmt, int n)
{
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = fmt == 1 ? (float)(p[2 * i] | (p[2 * i + 1] << 8)) : (float)p[i];
    std::nth_element(v.begin(), v.begin() + n * 8 / 100, v.end());
    float lo = v[n * 8 / 100];
    std::nth_element(v.begin(), v.begin() + n * 92 / 100, v.end());
    return (v[n * 92 / 100] - lo) / (fmt == 1 ? 65535.0f : 255.0f);
}
bool detected(const float* o, int mode) { return o[0] > 0 && o[3] > (mode == MODE_CC ? kCcScore : kSrScore); }

struct Stream {
    int lineNo = 0, mode = 0;
    SoftStream soft;               // both fields interleaved, 2 bytes per field
    std::vector<uint8_t> valid;    // per byte
    std::vector<uint8_t> field;    // per byte, 1 or 2
    std::vector<int64_t> frame;    // per byte pair
};

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Test signals (VITS: white bar, multiburst, staircase) look the same in
// every frame and hold a level for several microseconds; data changes.
// Compared after a 1 us moving average, which also hides tape jitter on
// the multiburst packets.
bool looksLikeTestSignal(const std::vector<uint8_t>& probe, int probeN, const VbiGeometry& g, int line)
{
    const float usec = g.lineSamples / 63.556f;
    const int n = g.validSamples, box = std::max(1, (int)usec);
    int hits = 0, seen = 0;
    double diff = 0;
    std::vector<float> prev;
    for (int i = 0; i < probeN; i++) {
        const uint8_t* p = &probe[(size_t)i * g.frameBytes() + (size_t)line * g.lineBytes];
        std::vector<float> v(n), m(n, 0.0f);
        for (int k = 0; k < n; k++) v[k] = g.fmt == 1 ? (float)(p[2 * k] | (p[2 * k + 1] << 8)) : (float)p[k];
        std::vector<float> s(v);
        std::nth_element(s.begin(), s.begin() + n * 3 / 10, s.end());
        float lo = s[n * 3 / 10];
        std::nth_element(s.begin(), s.begin() + n * 98 / 100, s.end());
        float hi = s[n * 98 / 100];
        if (hi - lo < (g.fmt == 1 ? 65535.0f : 255.0f) * 0.15f) continue;
        seen++;
        float thr = lo + 0.6f * (hi - lo);
        int run = 0, best = 0;
        for (int k = 0; k < n; k++) { run = v[k] > thr ? run + 1 : 0; best = std::max(best, run); }
        hits += best > 4 * usec;
        double acc = 0;
        for (int k = 0; k < n; k++) acc += v[k] - (k >= box ? v[k - box] : 0.0f), m[k] = (float)(acc / box);
        if (!prev.empty()) {
            double d = 0;
            for (int k = box; k < n; k++) d += std::fabs(m[k] - prev[k]);
            diff += d / (n - box) / (hi - lo);
        }
        prev = m;
    }
    return seen * 2 >= probeN && seen > 1 && hits * 2 >= seen && diff / (seen - 1) < 0.08;
}

} // namespace

// How convincingly a geometry fits: the best line's share of frames where a
// 608-style run-in (captions are on nearly every NTSC recording) is found.
static double formatScore(const std::string& path, VbiFormat f, Slicer& cpu)
{
    VbiFile vf;
    std::string err;
    if (!vf.open(path, err, f)) return -1;
    const VbiGeometry& g = vf.geom();
    const int n = (int)std::min<int64_t>(g.frames, 60);
    std::vector<uint8_t> buf, one;
    for (int i = 0; i < n; i++) {
        vf.read(g.frames * i / n, 1, one);
        buf.insert(buf.end(), one.begin(), one.end());
    }
    std::vector<SliceJob> jobs;
    for (int l = 0; l < g.linesPerFrame; l++) jobs.push_back({l, MODE_CC});
    std::vector<float> out;
    cpu.run(buf.data(), n, g, jobs, out, err);
    double best = 0;
    for (int l = 0; l < g.linesPerFrame && !g.pal; l++) { // 608 captions only exist in 525-line video
        int hits = 0;
        for (int i = 0; i < n; i++) hits += detected(&out[((size_t)i * jobs.size() + l) * kSliceOut], MODE_CC);
        best = std::max(best, (double)hits / n);
    }
    if (best >= 0.8) return best;
    // no clear captions (625-line captures have none): try teletext-rate
    // sync on the busiest lines
    std::vector<std::pair<double, int>> busy;
    for (int l = 0; l < g.linesPerFrame; l++) {
        int sig = 0;
        for (int i = 0; i < n; i++) {
            float info[4];
            fd_prepare(&buf[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes], g.fmt, g.validSamples, g.lineSamples,
                       info);
            sig += info[3] > 0;
        }
        busy.push_back({(double)sig, l});
    }
    std::sort(busy.rbegin(), busy.rend());
    for (size_t k = 0; k < busy.size() && k < 3; k++) {
        std::vector<const uint8_t*> ptr;
        for (int i = 0; i < n; i++) ptr.push_back(&buf[(size_t)i * g.frameBytes() + (size_t)busy[k].second * g.lineBytes]);
        FastCalib c = fdCalibrate(ptr, g, g.fastRate);
        best = std::max(best, c.syncRate);
    }
    return best;
}

// Packet streams, opened directly as in the original program: .t42 (625-line WST, 42 bytes per
// packet), .t34 (525-line WST, 34 bytes), .t33 (NABTS, 33 bytes after 55 55 E7).
static bool decodeStream(const std::string& path, const DecodeOptions& opt, DecodeResult& res, std::string& err)
{
    double t0 = now();
    res = DecodeResult{};
    res.path = path;
    auto note = [&](const std::string& m) {
        res.log.push_back(m);
        if (opt.log) opt.log(m);
    };
    Bytes st;
    try { st = read_file(path); } catch (std::exception& e) { err = e.what(); return false; }
    LineInfo li;
    li.mode = MODE_TEST + 1;
    li.detectRate = 1;
    if (ends_with_i(path, ".t33")) {
        res.slicerName = "NABTS packet stream (.t33)";
        res.nabtsT33 = st;
        if (opt.progress) opt.progress(0, "Reading the NABTS records (CEA-516, NAPLPS)");
        res.nabtsRecords = nabts::read_packets(res.nabtsT33, res.nabtsSummary);
        nabts::interpret(res.nabtsRecords, 256, 200);
        int pages = 0;
        for (auto& r : res.nabtsRecords) pages += r.page != nullptr;
        note(fmt("NABTS: %zu packets, %zu records, %d pages", st.size() / 33, res.nabtsRecords.size(), pages));
        li.kind = "NABTS";
    } else {
        bool is525 = ends_with_i(path, ".t34");
        res.geom.pal = !is525;
        res.slicerName = is525 ? "525-line teletext stream (.t34)" : "625-line teletext stream (.t42)";
        if (opt.progress) opt.progress(0, "Building the teletext pages");
        if (!ttBuild(st, is525, ttWorkDir(path), path, res.teletext, note, err)) return false;
        res.teletext.system = is525 ? "525-line WST (.t34)" : "625-line WST (.t42)";
        note("teletext: " + std::to_string(res.teletext.pages) + " pages, " + std::to_string(res.teletext.subpages) + " versions");
        li.kind = "Teletext stream";
    }
    res.lines.push_back(li);
    res.totalSeconds = now() - t0;
    return true;
}

// DVB recording: teletext packets from the transport stream, then pages
static bool decodeTs(const std::string& path, const DecodeOptions& opt, DecodeResult& res, std::string& err)
{
    double t0 = now();
    res = DecodeResult{};
    res.path = path;
    res.geom.pal = true;
    res.slicerName = "DVB transport stream";
    auto note = [&](const std::string& m) {
        res.log.push_back(m);
        if (opt.log) opt.log(m);
    };
    if (opt.progress) opt.progress(0, "Reading teletext from the transport stream");
    Progress pr;
    pr.on_progress = [&](long long a, long long b, const std::string&) { if (opt.progress && b) opt.progress((double)a / b, "Reading teletext from the transport stream"); };
    std::vector<Packet42> pk;
    try {
        pk = ts_teletext_packets(path, pr);
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
    note("DVB teletext (EN 300 472): " + std::to_string(pk.size()) + " packets");
    if (pk.empty()) { err = "no DVB teletext in this transport stream"; return false; }
    Bytes st;
    for (auto& p : pk) st.insert(st.end(), p.begin(), p.end());
    if (opt.progress) opt.progress(1, "Building the teletext pages");
    if (!ttBuild(st, false, ttWorkDir(path), path, res.teletext, note, err)) return false;
    LineInfo li;
    li.kind = "DVB teletext";
    li.mode = MODE_TEST + 1;
    li.detectRate = 1;
    res.lines.push_back(li);
    note("teletext: " + std::to_string(res.teletext.pages) + " pages, " + std::to_string(res.teletext.subpages) + " versions");
    res.totalSeconds = now() - t0;
    return true;
}

// fТ1 bits read at fТ2 come in equal pairs. Share of equal pairs at the better of the two phases.
static double tviPairShare(const std::vector<uint8_t>& w, int nbits, int* phase = nullptr)
{
    auto bit = [&](int k) { return (w[k / 8] >> (k % 8)) & 1; };
    double best = 0;
    for (int p = 0; p < 2; p++) {
        int eq = 0, n = 0;
        for (int k = p; k + 1 < nbits; k += 2, n++) eq += bit(k) == bit(k + 1);
        double r = n ? (double)eq / n : 0;
        if (r > best) { best = r; if (phase) *phase = p; }
    }
    return best;
}
// one bit of each pair: fТ2 bytes -> fТ1 bytes (bit 0 sent first)
static std::vector<uint8_t> tviCollapse(const std::vector<uint8_t>& w, int nbits)
{
    int ph = 0;
    tviPairShare(w, nbits, &ph);
    std::vector<uint8_t> o((nbits / 2 + 7) / 8, 0);
    for (int k = 0; ph + 2 * k < nbits && k < nbits / 2; k++)
        if ((w[(ph + 2 * k) / 8] >> ((ph + 2 * k) % 8)) & 1) o[k / 8] |= 1 << (k % 8);
    return o;
}

// 625-line recordings: the engine of the original Teletext Rescue reads them as it always did
// (WST PAL teletext with its tape decoder, VPS, WSS, Nagravision/Syster, SECAM identification,
// test signals) and keeps its results in "<recording>_vbi". Its line map fills the lines ours left.
static bool runLegacy(DecodeResult& res, const DecodeOptions& opt, const std::string& path,
                      const std::function<void(const std::string&)>& note, std::string& err)
{
    std::string src = opt.sourcePath.empty() ? path : opt.sourcePath;
    if (opt.progress) opt.progress(0, "625 lines: teletext, VPS, Syster (Teletext Rescue engine)");
    LegacyReport L;
    std::string e;
    if (!legacyRun(src, opt.again, [&](const std::string& s) { if (opt.progress) opt.progress(0, s); },
                   [&](const std::string& s) { note("  " + s); }, opt.cancel, L, e)) {
        if (opt.cancel && *opt.cancel) { err = "cancelled"; return false; }
        note("Teletext Rescue engine: " + e);
        return true;
    }
    const VbiGeometry& g = res.geom;
    int per = g.linesPerFrame / 2;
    // Norpak packets (55 55 E7, 5.73 Mbit/s) on GOST lines 7-9 / 320-322: our fТ2 slicer also passes
    // them (pilot test), so where the old engine reads such packets, every fТ2 guess on these lines
    // is the same packet signal (ORT 2002: lines 7 and 321 too)
    auto gostPk = [](int tv) { return (tv >= 7 && tv <= 9) || (tv >= 320 && tv <= 322); };
    bool norpak = false;
    for (auto& l : L.lines) norpak |= l.kind == "NABTS" && gostPk(l.tvLine);
    if (norpak)
        for (size_t i = 0; i < res.lines.size();) {
            LineInfo& li = res.lines[i];
            int tv = li.field == 2 ? li.lineNo + 313 : li.lineNo;
            if (li.mode == MODE_TVINFORM && li.tviRate == 1 && gostPk(tv)) {
                int ln = li.lineNo, fl = li.field;
                res.tvinform.erase(std::remove_if(res.tvinform.begin(), res.tvinform.end(), [&](const TviStream& t) {
                                       return !t.words.empty() && t.words[0].lineNo == ln && t.words[0].field == fl;
                                   }), res.tvinform.end());
                li.mode = MODE_TEST + 1;
                li.kind = "TV-Inform packets (Norpak, 55 55 E7)";
                note(fmt("line %d field %d: Norpak packets at 5.73 Mbit/s (not fТ2)", ln, fl));
            }
            i++;
        }
    for (auto& l : L.lines) {
        if (l.kind == "empty" || l.kind.empty()) continue;
        // a service the old engine names for sure (teletext, VPS, WSS, Syster ...) beats our
        // ТВ-информ guess: NRZ teletext sliced at fТ2 also passes the pilot test (1.vbi, NTV)
        // The old engine does not know ТВ-информ and calls its lines "NABTS" or "datacast"
        // (ORT 1999/2002), so those never win. Teletext only beats an fТ2 line it reads more
        // surely than we do (fТ1 lines are too slow to be mistaken); VPS, WSS, Syster always win.
        bool wst = l.kind.find("WST") != std::string::npos;
        bool fixedSvc = l.kind.rfind("VPS", 0) == 0 || l.kind.rfind("WSS", 0) == 0 || l.kind.find("Syster") != std::string::npos;
        if (wst || fixedSvc)
            for (size_t i = 0; i < res.lines.size(); i++)
                if (res.lines[i].index == l.row && res.lines[i].mode == MODE_TVINFORM &&
                    (fixedSvc || (res.lines[i].tviRate == 1 && l.signal >= res.lines[i].detectRate))) {
                    int ln = res.lines[i].lineNo, fl = res.lines[i].field;
                    res.tvinform.erase(std::remove_if(res.tvinform.begin(), res.tvinform.end(), [&](const TviStream& t) {
                                           return !t.words.empty() && t.words[0].lineNo == ln && t.words[0].field == fl;
                                       }), res.tvinform.end());
                    res.lines.erase(res.lines.begin() + i);
                    note(fmt("line %d field %d: %s (not ТВ-информ)", ln, fl, l.kind.c_str()));
                    break;
                }
        bool have = false;
        for (auto& li : res.lines) have |= li.index == l.row;
        if (have) continue;
        LineInfo li;
        li.index = l.row;
        li.field = l.tvLine > 313 ? 2 : 1;
        li.lineNo = l.tvLine > 313 ? l.tvLine - 313 : l.tvLine;
        li.mode = MODE_TEST + 1;   // shown, decoded by the old engine
        li.detectRate = l.signal;
        li.kind = l.kind;
        if (li.kind.rfind("test signal", 0) == 0) { li.mode = MODE_TEST; li.kind = "Test signal (VITS)"; }
        // 625 lines: 55 55 E7 packets (NABTS packet format) on lines 7-9 / 320-322 are ТВ-информ,
        // sent with Norpak equipment (GOST R 50822-95 lines; NABTS as a teletext service was never
        // used on Russian TV): ORT 1999 test transmission, ORT 2002 beacon
        if ((li.kind.find("NABTS") != std::string::npos || li.kind.find("Didon") != std::string::npos || li.kind.find("TV-Inform") != std::string::npos) &&
            ((l.tvLine >= 7 && l.tvLine <= 9) || (l.tvLine >= 320 && l.tvLine <= 322)))
            li.kind = "TV-Inform packets (Norpak, 55 55 E7)";
        (void)per;
        res.lines.push_back(li);
    }
    std::sort(res.lines.begin(), res.lines.end(), [](const LineInfo& a, const LineInfo& b) { return a.index < b.index; });
    for (auto& r : L.results) {
        if (r.kind == "t33" && g.pal)
            for (auto& li : res.lines)
                if (li.kind.rfind("TV-Inform packets", 0) == 0 || norpak) {
                    r.service = "TV-Inform packets (" + std::string("\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC") + ", .t33)";
                    // Norpak product code, beacon, test transmission
                    note("TV-Inform packets: correcting errors (Norpak FEC) and reading the data stream");
                    TviNorpak tn;
                    if (tviNorpakDecode(r.path, L.dir, tn)) {
                        TviStream ts;
                        ts.rate = -1; ts.nbits = 0;
                        ts.report = tn.report;
                        res.tvinform.erase(std::remove_if(res.tvinform.begin(), res.tvinform.end(), [](const TviStream& t) { return t.rate < 0; }), res.tvinform.end());
                        res.tvinform.push_back(std::move(ts));
                        r.note = fmt("%lld bundles corrected", (long long)tn.bundlesOk) + (tn.testPackets ? fmt(", %lld test-pattern packets", (long long)tn.testPackets) : "");
                    }
                    break;
                }
        if (r.kind == "teletext_project") {
            res.teletext.project = r.path;
            res.teletext.system = "625-line WST";
        }
    }
    note("results folder: " + L.dir);
    res.legacy = std::move(L);
    return true;
}

// as in the original program: the report goes to "<recording>_vbi/report.txt" (the old engine's
// report.json and service files sit next to it)
static void saveReport(const DecodeResult& res, const DecodeOptions& opt, const std::string& path)
{
    try {
        std::string dir = legacyReportDir(opt.sourcePath.empty() ? path : opt.sourcePath);
        make_dirs(dir);
        write_text(path_join(dir, "report.txt"), summarize(res));
        saveResults(opt.sourcePath.empty() ? path : opt.sourcePath, res);
    } catch (...) {}
}

bool decodeFile(const std::string& path, const DecodeOptions& opt, DecodeResult& res, std::string& err)
{
    if (ends_with_i(path, ".ts") || ends_with_i(path, ".mts") || ends_with_i(path, ".m2ts")) return decodeTs(path, opt, res, err);
    if (ends_with_i(path, ".t42") || ends_with_i(path, ".t34") || ends_with_i(path, ".t33")) return decodeStream(path, opt, res, err);
    if (is_flac(path)) {
        // FLAC-compressed capture: unpacked once to x_vbi/x.vbi next to it, then read as a plain recording
        std::string raw;
        std::vector<std::string> flog;
        try {
            Progress pr;
            pr.on_step = [&](const std::string& s) { if (opt.progress) opt.progress(0, s); };
            pr.on_log = [&](const std::string& s) { flog.push_back(s); if (opt.log) opt.log(s); };
            pr.on_progress = [&](long long a, long long b, const std::string&) {
                if (opt.cancel && *opt.cancel) throw Cancelled();
                if (opt.progress && b) opt.progress((double)a / b, "Unpacking the FLAC-compressed recording");
            };
            raw = flac_unpack_cached(path, opt.reunpack, pr);
        } catch (std::exception& e) {
            err = std::string("FLAC: ") + e.what();
            return false;
        }
        DecodeOptions o2 = opt;
        o2.sourcePath = path;
        bool ok = decodeFile(raw, o2, res, err);
        res.source = path;
        res.log.insert(res.log.begin(), flog.begin(), flog.end());
        return ok;
    }
    double t0 = now();
    VbiFormat format = opt.force;
    std::string formatLog;
    const std::string notes = capture_notes(path);   // e.g. "8bit, little-endian, 2048x32@25Hz"
    if (format == VbiFormat::Unknown) {
        // several chips share frame sizes (e.g. 1440 x 24 and 720 x 48 bytes),
        // so let the signal decide
        FILE* f = _wfopen(widen(path).c_str(), L"rb");
        int64_t size = 0;
        if (f) { _fseeki64(f, 0, SEEK_END); size = _ftelli64(f); fclose(f); }
        auto cands = candidateFormats(size);
        auto probeCpu = makeCpuSlicer(opt.threads);
        // the capture notes name the field rate: 25 Hz = 625 lines, 30 Hz = 525 lines
        bool want625 = notes.find("@25") != std::string::npos || notes.find("PAL") != std::string::npos ||
                       notes.find("SECAM") != std::string::npos;
        bool want525 = notes.find("@30") != std::string::npos || notes.find("@29") != std::string::npos ||
                       notes.find("NTSC") != std::string::npos;
        if (want625 != want525) {
            std::vector<VbiFormat> keep;
            for (VbiFormat c : cands) if (formatGeometry(c).pal == want625) keep.push_back(c);
            if (!keep.empty()) { cands = keep; formatLog += std::string("capture notes: ") + (want625 ? "625 lines, 25 Hz" : "525 lines, 30 Hz") + "\n"; }
        }
        double best = -1, bestPal = -1;
        VbiFormat pal = VbiFormat::Unknown;
        for (VbiFormat c : cands) {
            double s = formatScore(path, c, *probeCpu);
            char b[160];
            snprintf(b, sizeof b, "format candidate %s: 608 run-in score %.2f", formatKey(c), s);
            formatLog += std::string(b) + "\n";
            if (s > best + 0.05) { best = s; format = c; }
            if (formatGeometry(c).pal && s > bestPal) { bestPal = s; pal = c; }
        }
        // The original Teletext Rescue's detector (line timing, sync, SECAM identification,
        // teletext coherence) knows 625-line captures well; our score above is a 608 run-in
        // count that random 625-line data can also reach (capture(1).vbi: 0.83). Its answer is
        // taken when it names a 625-line layout, or when we found no clear captions.
        bool legacyPick = false;
        {
            std::string ln;
            VbiFormat lf = legacyDetectFormat(path, &ln);
            if (lf != VbiFormat::Unknown && (formatGeometry(lf).pal || best < 0.8) &&
                std::find(cands.begin(), cands.end(), lf) != cands.end()) {
                formatLog += "format by the Teletext Rescue detector: " + ln + "\n";
                format = lf;
                legacyPick = true;
            }
        }
        if (!legacyPick && format != VbiFormat::Unknown && !formatGeometry(format).pal && pal != VbiFormat::Unknown && best < 0.5 && !want525) {
            formatLog += "no 608 captions: taking the 625-line layout " + std::string(formatKey(pal)) + "\n";
            format = pal;
        }
    }
    VbiFile vf;
    if (!vf.open(path, err, format)) return false;
    const VbiGeometry& g = vf.geom();
    ColourInfo colour = detectColour(vf);
    res = DecodeResult{};
    res.path = path;
    res.geom = g;
    res.colour = colour.system;
    res.colourWhy = colour.why;
    res.captureNotes = notes;
    // everything the decoder learns goes to the report and, live, to the GUI
    auto note = [&](const std::string& m) {
        res.log.push_back(m);
        if (opt.log) opt.log(m);
    };
    {
        char b[200];
        snprintf(b, sizeof b, "%s: %lld frames (%.0f s), %d lines of %d samples, %s",
                 path.substr(path.find_last_of("/\\") + 1).c_str(), (long long)g.frames,
                 g.frames / (g.pal ? 25.0 : 29.97), g.linesPerFrame, g.samplesPerLine, formatName(g.format));
        note(b);
    }
    for (size_t a = 0, b; a < formatLog.size(); a = b + 1) {
        b = formatLog.find('\n', a);
        if (b == std::string::npos) b = formatLog.size();
        if (b > a) note(formatLog.substr(a, b - a));
    }
    auto progress = [&](double f, const std::string& s) { if (opt.progress) opt.progress(f, s); };

    std::unique_ptr<Slicer> slicer;
    if (opt.gpu) {
        std::string gerr;
        slicer = makeGpuSlicer(gerr);
        if (!slicer) note("GPU unavailable, using CPU: " + gerr);
    }
    if (!slicer) slicer = makeCpuSlicer(opt.threads);
    res.slicerName = slicer->name();
    note("decoding on " + res.slicerName);

    int64_t frames = g.frames;
    if (opt.maxFrames > 0 && opt.maxFrames < frames) frames = opt.maxFrames;

    // --- probe: which lines carry which signal ---------------------------
    progress(0, "Looking at the recording: what each line carries");
    note("finding lines with a signal");
    std::vector<SliceJob> all;
    for (int l = 0; l < g.linesPerFrame; l++) {
        all.push_back({l, MODE_CC});
        all.push_back({l, MODE_SILENT_RADIO});
    }
    const int probeN = (int)std::min<int64_t>(frames, 240);
    std::vector<uint8_t> probe((size_t)probeN * g.frameBytes()), one;
    for (int i = 0; i < probeN; i++) {
        int64_t f = frames * i / probeN;
        vf.read(f, 1, one);
        std::copy(one.begin(), one.end(), probe.begin() + (size_t)i * g.frameBytes());
    }
    std::vector<float> out;
    auto cpu = makeCpuSlicer(opt.threads);
    cpu->run(probe.data(), probeN, g, all, out, err);
    std::vector<SliceJob> jobs;
    for (int l = 0; l < g.linesPerFrame; l++) {
        double rate[2] = {0, 0};
        for (int m = 0; m < (g.pal ? 1 : 2); m++) {   // 625 lines: 608-style captions on any line (no Silent Radio there)
            int hits = 0;
            for (int i = 0; i < probeN; i++) hits += detected(&out[((size_t)i * all.size() + l * 2 + m) * kSliceOut], m);
            rate[m] = (double)hits / probeN;
        }
        int m = rate[1] > rate[0] ? 1 : 0;
        if (rate[m] < 0.75) continue;
        // median start-bit position: the anchor that prevents bit slips
        std::vector<float> pos;
        for (int i = 0; i < probeN; i++) {
            const float* o = &out[((size_t)i * all.size() + l * 2 + m) * kSliceOut];
            if (detected(o, m)) pos.push_back(o[1]);
        }
        std::nth_element(pos.begin(), pos.begin() + pos.size() / 2, pos.end());
        float anchor = pos[pos.size() / 2];
        LineInfo li;
        li.index = l;
        li.field = l < g.linesPerFrame / 2 ? 1 : 2;
        li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
        li.mode = m;
        li.detectRate = rate[m];
        res.lines.push_back(li);
        jobs.push_back({l, m, 0.0f, anchor});
        char b[160];
        snprintf(b, sizeof b, "  line %d field %d: %s   signal %3.0f%%", li.lineNo, li.field,
                 m == MODE_CC ? "data at the caption rate (captions / XDS / StarSight)" : "Silent Radio (LED sign service)",
                 rate[m] * 100);
        note(b);
    }
    // --- AMOL I (Nielsen): 48 bits at 64 x fH ---------------------------
    std::vector<LineInfo> amolLines;
    for (int l = 0; l < g.linesPerFrame && !g.pal; l++) {
        bool used = false;
        for (auto& j : jobs) used |= j.line == l;
        if (used) continue;
        int hits = 0;
        for (int i = 0; i < probeN; i++) {
            uint64_t bits;
            float q;
            hits += amolSlice(&probe[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes], g.fmt, g.validSamples,
                              g.lineSamples, bits, q);
        }
        if (hits * 2 < probeN) continue;
        LineInfo li;
        li.index = l;
        li.field = l < g.linesPerFrame / 2 ? 1 : 2;
        li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
        li.mode = MODE_AMOL;
        li.detectRate = (double)hits / probeN;
        li.kind = "AMOL (Nielsen)";
        amolLines.push_back(li);
        char b[160];
        snprintf(b, sizeof b, "  line %d field %d: Nielsen AMOL I (48 bits at 64 x fH)   signal %3.0f%%", li.lineNo, li.field,
                 li.detectRate * 100);
        note(b);
    }
    // --- Gemstar 2x: frame code 10011101101 + 32 bits at 64 x fH ----------
    std::vector<LineInfo> gemLines;
    for (int l = 0; l < g.linesPerFrame && !g.pal; l++) {
        bool used = false;
        for (auto& j : jobs) used |= j.line == l;
        for (auto& a : amolLines) used |= a.index == l;
        if (used) continue;
        int hits = 0;
        for (int i = 0; i < probeN; i++) {
            uint8_t w[4];
            float q;
            hits += gemstarSlice2x(&probe[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes], g.fmt, g.validSamples,
                                   g.lineSamples, w, q) && q > 0.2f;
        }
        if (hits * 4 < probeN) continue;
        LineInfo li;
        li.index = l;
        li.field = l < g.linesPerFrame / 2 ? 1 : 2;
        li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
        li.mode = MODE_GEMSTAR;
        li.detectRate = (double)hits / probeN;
        li.kind = "Gemstar";
        gemLines.push_back(li);
        char b[160];
        snprintf(b, sizeof b, "  line %d field %d: Gemstar 2x/1x (VCR Plus+ / Guide Plus+ data)   signal %3.0f%%", li.lineNo, li.field,
                 li.detectRate * 100);
        note(b);
    }
    // --- ТВ-информ (GOST R 50822-95): NRZ at fТ2 = 3.46875 MHz, 180 bits,
    // a pilot pulse (first bit = 1) + run-in clock recovery. Russian datacast
    // (ORT/Channel 1, SECAM); claimed before the 5.7 Mbit/s pass so these
    // lines are not mistaken for NABTS/WST.
    std::vector<LineInfo> tviLines;
    std::vector<int> tviRates;              // per line: clock index (0 = fТ1, 1 = fТ2)
    std::vector<char> tviPairs;             // per line: sliced at fТ2, every bit doubled = really fТ1
    const float fs = g.lineSamples * (float)g.lineRate;
    auto tviT = [&](int rate) { return fs / kTviRateHz[rate]; };   // samples per bit
    // ГОСТ allows fТ1 (90 bits), fТ2 (180) and fТ3 (360); recordings show fТ1 and fТ2 lines side by
    // side (ORT 2002). fТ3 equals the teletext clock (444 x fH), so it is left to the teletext pass.
    // A line is tried at fТ1 first: an fТ2 line fails there (its common edge gap is half a bit),
    // while an fТ1 line would also pass at fТ2. A clock needs at least ~6 samples/bit; a lower
    // sample rate (e.g. a 910-sample TBC decode) cannot tell ТВ-информ from NTSC datacast.
    for (int l = 0; l < g.linesPerFrame; l++) {
        bool used = false;
        for (auto& j : jobs) used |= j.line == l;
        for (auto& a : amolLines) used |= a.index == l;
        for (auto& a : gemLines) used |= a.index == l;
        if (used) continue;
        int rate = -1, hits = 0;
        for (int r = 0; r < 2 && rate < 0; r++) {
            if (tviT(r) < 6.0f) continue;
            int h = 0;
            float q;
            std::vector<uint8_t> w;
            int sig = 0, pre = 0;
            for (int i = 0; i < probeN; i++) {
                const uint8_t* p = &probe[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes];
                sig += lineSwing(p, g.fmt, g.validSamples) >= 0.10f;
                if (!tviSlice(p, g.fmt, g.validSamples, tviT(r), kTviBits[r], w, q)) continue;
                h++;
                auto idle = [](uint8_t v) { return v == 0x55 || v == 0xAA; };
                int nIdle = 0;
                bool lead = false;
                for (size_t k = 0; k < w.size(); k++) {
                    nIdle += idle(w[k]);
                    if (k < 3 && k + 1 < w.size() && idle(w[k]) && idle(w[k + 1])) lead = true;
                }
                pre += lead || nIdle >= 4;
            }
            // >= 60% of the probes that carry a signal slice with a pilot (the service may be on for only
            // part of a recording), and real ТВ-информ lines start with the 0101 idle carrier / 55 55 sync
            // (ORT: 5F 55 55.., ТВЦ: 55 55 27); a tape's own line signal at a similar rate does not (Piglet's Big Movie, UK VHS)
            if (sig * 10 >= probeN && h * 10 >= sig * 6 && pre * 10 >= h) { rate = r; hits = h; }
        }
        if (rate < 0) continue;
        // an fТ1 line that the fТ1 pass missed (strong colour burst in the window, ТВЦ line 19)
        // reads at fТ2 with every bit doubled: then it is fТ1, read at fТ2 and halved
        bool pairs = false;
        if (rate == 1) {
            int dbl = 0, ok = 0;
            float q;
            std::vector<uint8_t> w;
            for (int i = 0; i < probeN; i++)
                if (tviSlice(&probe[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes], g.fmt, g.validSamples, tviT(1), kTviBits[1], w, q)) {
                    ok++;
                    dbl += tviPairShare(w, kTviBits[1]) >= 0.90;
                }
            pairs = ok && dbl >= 0.8 * ok;
        }
        LineInfo li;
        li.index = l;
        li.field = l < g.linesPerFrame / 2 ? 1 : 2;
        li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
        li.mode = MODE_TVINFORM;
        li.detectRate = (double)hits / probeN;
        li.kind = "TV-Inform";
        li.tviRate = pairs ? 0 : rate;
        tviLines.push_back(li);
        tviRates.push_back(rate);
        tviPairs.push_back(pairs);
        char b[200];
        snprintf(b, sizeof b, "  line %d field %d: ТВ-информ (GOST R 50822-95, fТ%d %.5f MHz, %d bits)   signal %3.0f%%", li.lineNo,
                 li.field, li.tviRate + 1, kTviRateHz[li.tviRate] / 1e6, kTviBits[li.tviRate], li.detectRate * 100);
        note(b);
    }
    // --- 5.7 Mbit/s lines: anything else with a signal is a candidate ------
    std::vector<FastJob> fastJobs;
    std::vector<size_t> fastLine; // index into res.lines
    std::vector<int> testLines;   // line indices that carry test signals
    {
        std::vector<int> cand;
        for (int l = 0; l < g.linesPerFrame; l++) {
            bool used = false;
            for (auto& j : jobs) used |= j.line == l;
            for (auto& a : amolLines) used |= a.index == l;
            for (auto& a : gemLines) used |= a.index == l;
            for (auto& a : tviLines) used |= a.index == l;
            if (used) continue;
            int sig = 0;
            for (int i = 0; i < probeN; i++) {
                float info[4];
                fd_prepare(&probe[(size_t)i * g.frameBytes() + (size_t)l * g.lineBytes], g.fmt, g.validSamples,
                           g.lineSamples, info);
                sig += info[3] > 0;
            }
            if (sig * 2 >= probeN && !g.pal) cand.push_back(l);   // 625 lines: teletext comes from the old engine
        }
        if (!cand.empty()) {
            progress(0, "Calibrating the 5.7 Mbit/s decoder (MLSE)");
            note("calibrating the 5.7 Mbit/s decoder on " + std::to_string(cand.size()) + " lines");
            const int calN = (int)std::min<int64_t>(frames, 300);
            std::vector<uint8_t> cbuf;
            int got = (int)vf.read(std::max<int64_t>(0, std::min<int64_t>(frames / 3, frames - calN)), calN, cbuf);
            for (int l : cand) {
                std::vector<const uint8_t*> ptr;
                for (int f = 0; f < got; f++) ptr.push_back(&cbuf[(size_t)f * g.frameBytes() + (size_t)l * g.lineBytes]);
                FastCalib c = fdCalibrate(ptr, g);
                if (!c.ok || c.syncRate < 0.35) {
                    char b[128];
                    snprintf(b, sizeof b, "line index %d: no 5.7 Mbit/s sync (best %.0f%%)", l, c.syncRate * 100);
                    note(b);
                    if (looksLikeTestSignal(probe, probeN, g, l)) testLines.push_back(l);
                    continue;
                }
                FastJob fj;
                fj.line = l;
                fj.phi = c.phi;
                fj.spb = c.spb;
                memcpy(fj.H, c.H, sizeof fj.H);
                fastJobs.push_back(fj);
                LineInfo li;
                li.index = l;
                li.field = l < g.linesPerFrame / 2 ? 1 : 2;
                li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
                li.mode = MODE_FAST;
                li.detectRate = c.syncRate;
                li.kind = c.kind == 'N' ? "NABTS" : c.kind == 'W' ? "WST teletext" : "Encrypted datacast (5.7 Mbit/s, framing 2D)";
                fastLine.push_back(res.lines.size());
                res.lines.push_back(li);
                char b[200];
                snprintf(b, sizeof b,
                         "line %d field %d: MLSE calibrated, %.1f x fH (%.3f Mbit/s), sync on %.0f%% of lines, kind %c, "
                         "NABTS header valid %.0f%%",
                         li.lineNo, li.field, c.rate, c.rate * g.lineRate / 1e6, c.syncRate * 100, c.kind, c.headerOk * 100);
                note(b);
            }
        }
    }
    std::vector<size_t> amolLine; // index into res.lines
    for (auto& a : amolLines) { amolLine.push_back(res.lines.size()); res.lines.push_back(a); }
    std::vector<size_t> gemLine; // index into res.lines
    for (auto& a : gemLines) { gemLine.push_back(res.lines.size()); res.lines.push_back(a); }
    std::vector<size_t> tviLine; // index into res.lines
    for (auto& a : tviLines) { tviLine.push_back(res.lines.size()); res.lines.push_back(a); }
    for (int l : testLines) {
        LineInfo li;
        li.index = l;
        li.field = l < g.linesPerFrame / 2 ? 1 : 2;
        li.lineNo = g.firstLine + l % (g.linesPerFrame / 2);
        li.mode = MODE_TEST;
        li.detectRate = 1;
        li.kind = "Test signal (VITS)";
        res.lines.push_back(li);
    }
    std::vector<std::vector<AmolWord>> amolWords(amolLine.size());
    std::vector<std::vector<GemstarWord>> gemWords(gemLine.size());
    std::vector<std::vector<TviWord>> tviWords(tviLine.size());
    if (jobs.empty() && fastJobs.empty() && amolLine.empty() && gemLine.empty() && tviLine.empty()) {
        if (g.pal) {   // the old engine may still find teletext, VPS, Syster ...
            if (!runLegacy(res, opt, path, note, err)) return false;
            res.totalSeconds = now() - t0;
            saveReport(res, opt, path);
            return true;
        }
        // nothing to decode: still a result, so the recording window and its VBI line viewer open
        note("no data lines found");
        res.totalSeconds = now() - t0;
        saveReport(res, opt, path);
        return true;
    }

    // --- phase calibration ----------------------------------------------
    // Tape group delay shifts the best sampling point. Try offsets on a
    // contiguous stretch and keep the one giving most CRC-valid StarSight
    // packets (or, for caption lines, the best parity ratio).
    {
        progress(0, "Calibrating the sampling phase");
        note("calibrating the sampling phase");
        const int calN = (int)std::min<int64_t>(frames, 2400);
        const int64_t calStart = std::max<int64_t>(0, std::min<int64_t>(frames / 3, frames - calN));
        std::vector<uint8_t> cbuf;
        int got = (int)vf.read(calStart, calN, cbuf);
        std::vector<float> phases;
        for (int k = -6; k <= 12; k++) phases.push_back(k * 0.05f);
        std::vector<SliceJob> cj;
        std::vector<int> cjJob;
        for (size_t j = 0; j < jobs.size(); j++)
            if (jobs[j].mode == MODE_CC)
                for (float ph : phases) { cj.push_back({jobs[j].line, MODE_CC, ph, jobs[j].anchor}); cjJob.push_back((int)j); }
        if (!cj.empty() && got > 0 && cpu->run(cbuf.data(), got, g, cj, out, err)) {
            std::map<int, std::vector<double>> score; // lineNo -> per phase
            for (size_t pi = 0; pi < phases.size(); pi++) {
                std::map<int, std::vector<uint8_t>> bytes;
                for (int f = 0; f < got; f++)
                    for (size_t c = pi; c < cj.size(); c += phases.size()) {
                        const float* o = &out[((size_t)f * cj.size() + c) * kSliceOut];
                        uint8_t by[2] = {0, 0};
                        for (int k = 0; k < 16; k++) by[k >> 3] |= (o[4 + k] > 0) << (k & 7);
                        auto& v = bytes[res.lines[cjJob[c]].lineNo];
                        v.push_back(by[0]);
                        v.push_back(by[1]);
                    }
                for (auto& kv : bytes) {
                    auto& sc = score[kv.first];
                    sc.resize(phases.size());
                    sc[pi] = ssCountGoodPackets(kv.second) + 0.5 * ccParityRatio(kv.second, {});
                }
            }
            for (auto& kv : score) {
                auto& sc = kv.second;
                size_t best = 6; // phase 0
                double bv = -1;
                for (size_t pi = 0; pi < sc.size(); pi++) {
                    double v = sc[pi] + 0.5 * ((pi ? sc[pi - 1] : sc[pi]) + (pi + 1 < sc.size() ? sc[pi + 1] : sc[pi]));
                    if (v > bv + 1e-9) { bv = v; best = pi; }
                }
                for (size_t j = 0; j < jobs.size(); j++)
                    if (jobs[j].mode == MODE_CC && res.lines[j].lineNo == kv.first) jobs[j].phase = phases[best];
                char b[128];
                snprintf(b, sizeof b, "line %d: sampling phase %+.2f bit (score %.1f)", kv.first, phases[best], sc[best]);
                note(b);
            }
        }
    }

    // --- main pass --------------------------------------------------------
    std::map<std::pair<int, int>, Stream> streams;
    for (auto& li : res.lines) {
        if (li.mode != MODE_CC && li.mode != MODE_SILENT_RADIO) continue;
        auto& s = streams[{li.lineNo, li.mode}];
        s.lineNo = li.lineNo;
        s.mode = li.mode;
    }
    const int chunk = (int)std::max<int64_t>(64, (64LL << 20) / g.frameBytes());
    const std::string mainMsg = "decoding " + std::to_string(frames) + " frames on " +
                                (opt.gpu && res.slicerName.rfind("GPU", 0) == 0 ? "the GPU" : "the processor");
    note(mainMsg);
    progress(0, mainMsg);
    std::vector<uint8_t> buf;
    double sliceTime = 0;
    for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
        if (opt.cancel && *opt.cancel) { err = "cancelled"; return false; }
        int64_t want = std::min<int64_t>(chunk, frames - f0);
        int got = (int)vf.read(f0, want, buf);
        if (got <= 0) break;
        double ts = now();
        if (!jobs.empty() && !slicer->run(buf.data(), got, g, jobs, out, err)) return false;
        if (!fastJobs.empty()) {
            std::vector<uint8_t> fbits;
            std::string ferr;
            if (!slicer->runFast(buf.data(), got, g, fastJobs, fbits, ferr)) {
                note("GPU MLSE failed, using CPU: " + ferr);
                cpu->runFast(buf.data(), got, g, fastJobs, fbits, ferr);
            }
            for (int f = 0; f < got; f++)
                for (size_t j = 0; j < fastJobs.size(); j++) {
                    const uint8_t* b = &fbits[((size_t)f * fastJobs.size() + j) * kFastBits];
                    char kind;
                    int pos, e;
                    if (!fdFindSync(b, kFastBits, kind, pos, e)) continue;
                    FastPacket pk = fdMakePacket(b, kFastBits, kind, pos, e);
                    const LineInfo& li = res.lines[fastLine[j]];
                    pk.frame = f0 + f;
                    pk.lineNo = li.lineNo;
                    pk.field = li.field;
                    res.fastPackets.push_back(std::move(pk));
                }
        }
        for (size_t a = 0; a < amolLine.size(); a++) {
            const LineInfo& li = res.lines[amolLine[a]];
            for (int f = 0; f < got; f++) {
                AmolWord w;
                if (!amolSlice(&buf[(size_t)f * g.frameBytes() + (size_t)li.index * g.lineBytes], g.fmt, g.validSamples,
                               g.lineSamples, w.bits, w.quality))
                    continue;
                w.frame = f0 + f;
                amolWords[a].push_back(w);
            }
        }
        for (size_t a = 0; a < gemLine.size(); a++) {
            const LineInfo& li = res.lines[gemLine[a]];
            for (int f = 0; f < got; f++) {
                const uint8_t* p = &buf[(size_t)f * g.frameBytes() + (size_t)li.index * g.lineBytes];
                GemstarWord w;
                w.frame = f0 + f;
                if (gemstarSlice2x(p, g.fmt, g.validSamples, g.lineSamples, w.b, w.quality)) w.x2 = true;
                else if (gemstarSlice1x(p, g.fmt, g.validSamples, g.lineSamples, w.b, w.ok)) w.x2 = false, w.quality = 1;
                else continue;
                gemWords[a].push_back(w);
            }
        }
        for (size_t a = 0; a < tviLine.size(); a++) {
            const LineInfo& li = res.lines[tviLine[a]];
            for (int f = 0; f < got; f++) {
                TviWord w;
                if (!tviSlice(&buf[(size_t)f * g.frameBytes() + (size_t)li.index * g.lineBytes], g.fmt, g.validSamples,
                              tviT(tviRates[a]), kTviBits[tviRates[a]], w.bytes, w.quality))
                    continue;
                if (tviPairs[a]) w.bytes = tviCollapse(w.bytes, kTviBits[1]);
                w.frame = f0 + f;
                w.lineNo = li.lineNo;
                w.field = li.field;
                tviWords[a].push_back(std::move(w));
            }
        }
        sliceTime += now() - ts;
        for (int f = 0; f < got; f++)
            for (size_t j = 0; j < jobs.size(); j++) {
                const LineInfo& li = res.lines[j];
                const float* o = &out[((size_t)f * jobs.size() + j) * kSliceOut];
                Stream& s = streams[{li.lineNo, li.mode}];
                // keep weak lines too: soft confidences let the CRC repair them
                bool ok = li.mode == MODE_CC ? o[0] > 0 : detected(o, li.mode);
                if (li.mode == MODE_SILENT_RADIO && !ok) continue;
                uint8_t by[2] = {0, 0};
                for (int k = 0; k < 16; k++) {
                    float v = ok ? o[4 + k] : 0.0f;
                    int bit = v > 0;
                    if (li.mode == MODE_CC) by[k >> 3] |= bit << (k & 7);
                    else by[k >> 3] |= bit << (7 - (k & 7));
                    if (li.mode == MODE_CC) s.soft.conf.push_back(std::fabs(v));
                }
                for (int k = 0; k < 2; k++) {
                    s.soft.bytes.push_back(by[k]);
                    s.valid.push_back(ok);
                    s.field.push_back((uint8_t)li.field);
                }
                s.frame.push_back(f0 + f);
            }
        progress((double)(f0 + got) / frames, mainMsg);
        if (opt.count) opt.count(f0 + got, frames);
    }
    res.sliceSeconds = sliceTime;

    // --- per stream decoding ------------------------------------------
    progress(1.0, "Decoding the services");
    note("decoding the services");
    // --- 5.7 Mbit/s lines: the kind is what the line actually delivered -----
    // Calibration looks at a few hundred lines; noise and teletext lines can
    // throw up stray syncs of another kind. Per line: the majority kind wins,
    // NABTS must have mostly valid Hamming headers, the rest is dropped.
    std::map<std::pair<int, int>, char> lineKind; // (line, field) -> N W X, E = NABTS sync with a foreign header, ? = unclear
    if (!res.fastPackets.empty()) {
        std::map<std::pair<int, int>, std::map<char, int>> kinds;   // (line, field) -> kind -> packets
        std::map<std::pair<int, int>, std::pair<int, int>> ham;     // NABTS header bytes valid / all
        std::map<std::pair<int, int>, int64_t> fill2d;               // AA / 55 bytes after the sync and header
        for (auto& p : res.fastPackets) {
            kinds[{p.lineNo, p.field}][p.kind]++;
            for (size_t b = 5; b < p.bytes.size() && b < 37; b++) fill2d[{p.lineNo, p.field}] += p.bytes[b] == 0xAA || p.bytes[b] == 0x55;
            if (p.kind == 'N') {   // the whole 5-byte prefix must be Hamming 8/4
                bool all = p.bytes.size() >= 8;
                for (int b = 3; b < 8 && all; b++) all = hamming84(p.bytes[b]) >= 0;
                auto& h = ham[{p.lineNo, p.field}];
                h.first += all;
                h.second++;
            }
        }
        // WST (55 55 27) and NABTS (55 55 E7) differ in the last sync bit only and
        // both start with Hamming bytes: on a noisy tape one passes for the other.
        // The recording decides, a line keeps its own kind only by a clear margin.
        int totN = 0, totW = 0;
        for (auto& kv : kinds) { totN += kv.second['N']; totW += kv.second['W']; }
        const char family = totW > totN ? 'W' : 'N';
        for (auto& kv : kinds) {
            int nN = kv.second['N'], nW = kv.second['W'], nX = kv.second['X'], all = nN + nW + nX;
            char best = nX > nN + nW ? 'X' : nN > 0.8 * (nN + nW) ? 'N' : nW > 0.8 * (nN + nW) ? 'W' : family;
            auto h = ham[kv.first];
            if (best == 'N' && h.second && (double)h.first / h.second < 0.3) best = 'E';
            if (best == 'X' ? nX * 2 < all : (nN + nW) * 2 < all) best = '?'; // no clear kind
            // a test signal with a chroma packet passes the AA AA B4 (or 55 55 27) sync: the 3.58 MHz subcarrier
            // sliced at 5.73 Mbit/s is 1010.., so its "payload" is half AA/55 (CBS 1997 line 18, WVIZ 1995 lines
            // 19/20: the same waveform); real 2D data has ~0.3 such bytes a packet, WST/NABTS bytes with odd parity none
            if (all && (double)fill2d[kv.first] / all >= 8) best = 'T';
            char b[220];
            snprintf(b, sizeof b, "5.7 Mbit/s line %d field %d: NABTS %d, WST %d, 2D %d packets; NABTS prefix valid in %.0f%%, AA/55 bytes %.1f a packet -> %c",
                     kv.first.first, kv.first.second, nN, nW, nX, h.second ? 100.0 * h.first / h.second : 0.0, all ? (double)fill2d[kv.first] / all : 0.0, best);
            note(b);
            lineKind[kv.first] = best;
            for (auto& li : res.lines)
                if (li.mode == MODE_FAST && li.lineNo == kv.first.first && li.field == kv.first.second)
                    li.kind = best == 'N' ? "NABTS" : best == 'W' ? "WST teletext" : best == 'X' ? "Encrypted datacast (5.7 Mbit/s, framing 2D)"
                            : best == 'T' ? "Test signal (VITS)" : best == 'E' ? "Encrypted datacast (5.7 Mbit/s, NABTS sync, no NABTS header)" : "5.7 Mbit/s data (unknown)";
        }
        // same bytes from the sync on: a WST/NABTS packet takes its line's kind
        for (auto& p : res.fastPackets) {
            char k = lineKind[{p.lineNo, p.field}];
            if ((k == 'N' || k == 'W') && (p.kind == 'N' || p.kind == 'W')) p.kind = k;
        }
        size_t before = res.fastPackets.size();
        res.fastPackets.erase(std::remove_if(res.fastPackets.begin(), res.fastPackets.end(),
                                             [&](const FastPacket& p) { return lineKind[{p.lineNo, p.field}] != p.kind; }),
                              res.fastPackets.end());
        if (before != res.fastPackets.size())
            note("5.7 Mbit/s: " + std::to_string(before - res.fastPackets.size()) + " packets of a kind foreign to their line dropped");
    }
    // --- 5.7 Mbit/s: second reading with a channel model fitted to every line
    if (!lineKind.empty() && !getenv("TR_FIRST_MLSE")) {
        std::vector<FastLineJob> sj;
        for (auto& fj : fastJobs) {
            FastLineJob j;
            j.index = fj.line;
            j.field = fj.line < g.linesPerFrame / 2 ? 1 : 2;
            j.lineNo = g.firstLine + fj.line % (g.linesPerFrame / 2);
            auto it = lineKind.find({j.lineNo, j.field});
            if (it == lineKind.end() || !strchr("NWXE", it->second)) continue;
            j.kind = it->second;
            if (j.kind == 'X') { // the line's service bytes (3 % of its packets or more)
                std::map<int, int> sv;
                int tot = 0;
                static const uint8_t cyc[8] = {0x8B, 0x9C, 0x4D, 0xC6, 0x17, 0xE8, 0x39, 0x5A};
                int inCycle = 0;
                for (auto& p : res.fastPackets)
                    if (p.lineNo == j.lineNo && p.field == j.field && p.bytes.size() > 4) {
                        sv[p.bytes[3]]++;
                        tot++;
                        inCycle += std::find(cyc, cyc + 8, p.bytes[4]) != cyc + 8;
                    }
                for (auto& kv : sv)
                    if (kv.second * 100 >= tot * 2) j.services.push_back(kv.first);
                j.cycle8 = inCycle * 2 > tot;
            }
            sj.push_back(j);
        }
        if (!sj.empty()) {
            const std::string msg = "5.7 Mbit/s: reading again with a per-line channel model (MLSE)";
            note(msg);
            double ts = now();
            std::vector<std::pair<int, int>> rejected;
            auto pk = fdSecondPass(vf, sj, opt.threads, opt.cancel, [&](double x) { progress(x, msg); }, note, &rejected);
            if (opt.cancel && *opt.cancel) { err = "cancelled"; return false; }
            std::set<std::pair<int, int>> redone(rejected.begin(), rejected.end());
            for (auto& p : pk) redone.insert({p.lineNo, p.field});
            for (auto& r : rejected) {
                lineKind[r] = '?';
                for (auto& li : res.lines)
                    if (li.mode == MODE_FAST && li.lineNo == r.first && li.field == r.second) li.kind = "5.7 Mbit/s data (unknown)";
            }
            res.fastPackets.erase(std::remove_if(res.fastPackets.begin(), res.fastPackets.end(),
                                                 [&](const FastPacket& p) { return redone.count({p.lineNo, p.field}) > 0; }),
                                  res.fastPackets.end());
            res.fastPackets.insert(res.fastPackets.end(), pk.begin(), pk.end());
            std::stable_sort(res.fastPackets.begin(), res.fastPackets.end(), [](const FastPacket& a, const FastPacket& b) {
                if (a.frame != b.frame) return a.frame < b.frame;
                if (a.field != b.field) return a.field < b.field;
                return a.lineNo < b.lineNo;
            });
            char b[120];
            snprintf(b, sizeof b, "5.7 Mbit/s second reading: %.1f s", now() - ts);
            note(b);
        }
    }
    if (!res.fastPackets.empty()) {
        note("5.7 Mbit/s data: " + std::to_string(res.fastPackets.size()) + " packets");
        auto crcLines = fd2dCrc(res.fastPackets);
        for (auto& l : crcLines) note(l);
        FastReport fr = fdReport(res.fastPackets);
        res.fastReport = fr.lines;
        res.fastReport.insert(res.fastReport.begin() + std::min<size_t>(1, res.fastReport.size()), crcLines.begin(), crcLines.end());
        res.fastText = fr.text;
        // Japanese teletext: the "NABTS sync, foreign header" lines carry (272,190) codewords
        if (jtesDecode(res.fastPackets, res.jtes)) {
            for (auto& l : res.jtes.report) note(l);
            std::set<std::pair<int, int>> jl;
            for (auto& pk : res.fastPackets)
                if (pk.kind == 'E') jl.insert({pk.lineNo, pk.field});
            for (auto& li : res.lines)
                if (li.mode == MODE_FAST && jl.count({li.lineNo, li.field})) li.kind = "Japanese teletext (JTES)";
            res.fastReport.insert(res.fastReport.end(), res.jtes.report.begin(), res.jtes.report.end());
        }
        // NABTS: the packets after 55 55 E7 are the 33-byte .t33 records of CEA-516
        for (auto& pk : res.fastPackets)
            if (pk.kind == 'N' && pk.bytes.size() >= 36 && lineKind[{pk.lineNo, pk.field}] == 'N') {
                res.nabtsT33.insert(res.nabtsT33.end(), pk.bytes.begin() + 3, pk.bytes.begin() + 36);
                res.nabtsField.push_back((int)(pk.frame * 2 + pk.field - 1));
                res.nabtsLine.push_back(pk.lineNo); // a WaveTop bundle runs on one line through both fields
            }
        // WST: 34 bytes after 55 55 27 (address 2 + 32 characters) make the .t34 stream
        Bytes t34;
        for (auto& pk : res.fastPackets)
            if (pk.kind == 'W' && pk.bytes.size() >= 37) t34.insert(t34.end(), pk.bytes.begin() + 3, pk.bytes.begin() + 37);
        if (t34.size() >= 34 * 50) {
            progress(1.0, "WST teletext: building pages");
            std::string terr;
            if (ttBuild(t34, true, ttWorkDir(path), path, res.teletext, note, terr))
                note("WST teletext: " + std::to_string(res.teletext.pages) + " pages, " + std::to_string(res.teletext.subpages) + " versions" +
                     (res.teletext.service.empty() ? "" : " (" + res.teletext.service + ")"));
            else
                note("WST teletext: no pages (" + terr + ")");
        }
        if (!res.nabtsT33.empty()) {
            progress(1.0, "NABTS: assembling groups and records, drawing NAPLPS pages");
            try {
                res.nabtsRecords = nabts::read_packets(res.nabtsT33, res.nabtsSummary);
                nabts::interpret(res.nabtsRecords, 256, 200);
            } catch (std::exception& e) {
                note(std::string("NABTS: ") + e.what());
            }
            int pages = 0;
            for (auto& r : res.nabtsRecords) pages += r.page != nullptr;
            char b[200];
            snprintf(b, sizeof b, "NABTS (CEA-516): %ld packets, %ld groups, %zu records, %d NAPLPS pages",
                     res.nabtsSummary.packets, res.nabtsSummary.groups["complete"], res.nabtsRecords.size(), pages);
            note(b);
        }
        if (!opt.dumpDir.empty())
            if (FILE* f = _wfopen(widen(opt.dumpDir + "/fast_packets.txt").c_str(), L"wb")) {
                for (auto& pk : res.fastPackets) {
                    fprintf(f, "%lld L%d F%d %c e%d ", (long long)pk.frame, pk.lineNo, pk.field, pk.kind, pk.syncErr);
                    for (uint8_t c : pk.bytes) fprintf(f, "%02x", c);
                    fprintf(f, "\n");
                }
                fclose(f);
            }
    }
    for (size_t a = 0; a < amolLine.size(); a++) {
        AmolStream as;
        as.lineNo = res.lines[amolLine[a]].lineNo;
        as.field = res.lines[amolLine[a]].field;
        amolDecode(amolWords[a], as);
        note("AMOL, line " + std::to_string(as.lineNo) + ": " + std::to_string(as.stamps.size()) + " timestamps");
        res.amol.push_back(std::move(as));
    }
    for (size_t a = 0; a < gemLine.size(); a++) {
        GemstarStream gs;
        gs.lineNo = res.lines[gemLine[a]].lineNo;
        gs.field = res.lines[gemLine[a]].field;
        gs.words = std::move(gemWords[a]);
        gemstarDecode(gs);
        note("Gemstar, line " + std::to_string(gs.lineNo) + " field " + std::to_string(gs.field) + ": " + std::to_string(gs.messages.size()) + " messages");
        res.gemstar.push_back(std::move(gs));
    }
    for (size_t a = 0; a < tviLine.size(); a++) {
        TviStream tvi;
        tvi.rate = tviPairs[a] ? 0 : tviRates[a];
        tvi.nbits = kTviBits[tvi.rate];
        tvi.words = std::move(tviWords[a]);
        note("ТВ-информ, line " + std::to_string(res.lines[tviLine[a]].lineNo) + " field " +
             std::to_string(res.lines[tviLine[a]].field) + ": " + std::to_string(tvi.words.size()) + " lines");
        res.tvinform.push_back(std::move(tvi));
    }
    for (auto& kv : streams) {
        Stream& s = kv.second;
        auto setKind = [&](int field, const std::string& k) {
            for (auto& li : res.lines)
                if (li.lineNo == s.lineNo && li.mode == s.mode && (field == 0 || li.field == field)) li.kind = k;
        };
        if (!opt.dumpDir.empty()) {
            std::string fn = opt.dumpDir + "/line" + std::to_string(s.lineNo) + (s.mode ? "_sr" : "_cc") + ".bin";
            if (FILE* f = _wfopen(widen(fn).c_str(), L"wb")) {
                fwrite(s.soft.bytes.data(), 1, s.soft.bytes.size(), f);
                fclose(f);
            }
        }
        if (s.mode == MODE_SILENT_RADIO) {
            size_t before = res.silentRadio.size(), nz = 0;
            for (uint8_t c : s.soft.bytes) nz += c != 0;
            srDecode(s.soft.bytes, s.frame, res.silentRadio);
            bool idle = res.silentRadio.size() == before && nz * 100 < s.soft.bytes.size();
            // the same 80 x fH preamble without any sign text is not taken for Silent Radio
            (void)idle;
            setKind(0, res.silentRadio.size() == before ? "data at 80 x fH (no text)" : "Silent Radio");
            note("Silent Radio, line " + std::to_string(s.lineNo) + ": " +
                 std::to_string(res.silentRadio.size() - before) + " messages");
            continue;
        }
        bool textField[3] = {false, false, false};
        std::vector<uint8_t> per[3], perValid[3];
        for (size_t i = 0; i < s.soft.bytes.size(); i++) {
            per[s.field[i]].push_back(s.soft.bytes[i]);
            perValid[s.field[i]].push_back(s.valid[i]);
        }
        // XDS lives in field 2 only; when it turns up in the first stored
        // field, the capture has the fields the other way round
        int xdsField = 2;
        if (s.lineNo == 21 && !per[1].empty()) {
            int x1 = ccCountXds(per[1]), x2 = per[2].empty() ? 0 : ccCountXds(per[2]);
            if (x1 >= 3 && x1 > 2 * x2) {
                xdsField = 1;
                res.fieldsSwapped = true;
                note("line 21: XDS found in the first stored field - the capture stores field 2 first");
            }
        }
        // captions only in the second stored field and no XDS there: captions
        // (CC1) live in field 1, so the capture has the fields the other way round
        if (s.lineNo == 21 && xdsField == 2 && !per[2].empty() && ccCountXds(per[2]) == 0 &&
            ccParityRatio(per[2], perValid[2]) >= 0.85 && (per[1].empty() || ccParityRatio(per[1], perValid[1]) < 0.85)) {
            xdsField = 1;
            res.fieldsSwapped = true;
            note("line 21: captions only in the second stored field, no XDS - the capture stores field 2 first");
        }
        for (int fl = 1; fl <= 2; fl++) {
            if (per[fl].empty()) continue;
            double pr = ccParityRatio(per[fl], perValid[fl]);
            if (pr < 0.85) continue;
            textField[fl] = true;
            char hdr[64];
            const int logical = fl == xdsField ? 2 : 1;
            if (logical == 2 && s.lineNo == 21) {
                snprintf(hdr, sizeof hdr, "--- line %d field 2: XDS ---", s.lineNo);
                res.xds.push_back(hdr);
                ccDecodeXds(per[fl], res.xds);
                ccDecodeCaptions(per[fl], res.captions, 3);
                setKind(fl, "XDS / CC3");
                note("XDS, line " + std::to_string(s.lineNo) + " field 2");
            } else {
                snprintf(hdr, sizeof hdr, "--- line %d field %d: captions ---", s.lineNo, logical);
                res.captions.push_back(hdr);
                ccDecodeCaptions(per[fl], res.captions, logical == 1 ? 1 : 3);
                setKind(fl, "Captions");
                note("captions, line " + std::to_string(s.lineNo) + " field " + std::to_string(logical));
            }
        }
        if (textField[1] && (textField[2] || per[2].empty())) continue;
        if (g.pal) {   // no StarSight in 625-line video: pulses at about the caption rate (Macrovision AGC, test bursts)
            for (int fl = 1; fl <= 2; fl++)
                if (!textField[fl]) setKind(fl, "Test signal / copy-protection pulses");
            continue;
        }
        SsDatabase db;
        ssDecodeStream(s.soft, db, opt.keepUnverified);
        char b[160];
        snprintf(b, sizeof b, "line %d: %d StarSight packets", s.lineNo, db.stats.packets);
        note(b);
        if (db.stats.packets > 0) {
            for (int fl = 1; fl <= 2; fl++)
                if (!textField[fl]) setKind(fl, "StarSight");
            res.db.merge(db);
        } else {
            for (int fl = 1; fl <= 2; fl++)
                if (!textField[fl]) {
                    bool vits = false;
                    for (auto& li : res.lines)
                        if (li.lineNo == s.lineNo && li.mode == s.mode && li.field == fl)
                            vits = looksLikeTestSignal(probe, probeN, g, li.index);
                    setKind(fl, vits ? "Test signal (VITS)" : "8-bit data (unknown)");
                }
        }
    }
    if (res.fieldsSwapped) {
        // report fields as broadcast
        for (auto& li : res.lines) li.field = 3 - li.field;
        for (auto& a : res.amol) a.field = 3 - a.field;
        for (auto& a : res.gemstar) a.field = 3 - a.field;
        for (auto& t : res.tvinform) for (auto& w : t.words) w.field = 3 - w.field;
        for (auto& p : res.fastPackets) p.field = 3 - p.field;
    }
    for (auto& a : res.amol) amolBuildReport(a);
    gemstarGuide(res.gemstar);
    for (auto& a : res.gemstar) gemstarBuildReport(a);
    for (auto& a : res.tvinform) tviBuild(a);
    if (g.pal && !runLegacy(res, opt, path, note, err)) return false;
    res.totalSeconds = now() - t0;
    saveReport(res, opt, path);
    return true;
}

std::vector<std::string> headline(const DecodeResult& r)
{
    std::vector<std::string> h;
    std::string sys = r.geom.pal ? "625 lines, 25 frames/s" : "525 lines, 30 frames/s";
    if (r.slicerName.find("stream") != std::string::npos || r.slicerName.find("DVB") != std::string::npos) sys.clear();
    if (!sys.empty())
        h.push_back("Picture: " + sys + (r.colour.empty() ? (r.geom.pal ? ", colour system not visible in the VBI (PAL or SECAM)" : "")
                                                         : ", " + r.colour) +
                    (r.colourWhy.empty() || r.colour.empty() ? "" : "  (" + r.colourWhy + ")"));
    auto linesOf = [&](auto pred) {
        std::string s;
        for (auto& li : r.lines)
            if (pred(li)) s += (s.empty() ? "" : ", ") + std::to_string(li.lineNo) + (li.field == 2 ? "/f2" : "");
        return s;
    };
    if (r.geom.pal) {
        std::string t = linesOf([](const LineInfo& li) { return li.mode == MODE_TVINFORM || li.kind.rfind("TV-Inform", 0) == 0; });
        std::string pk2 = linesOf([](const LineInfo& li) { return li.kind.rfind("TV-Inform packets", 0) == 0; });
        std::string pk = linesOf([](const LineInfo& li) { return li.kind.find("NABTS") != std::string::npos || li.kind.find("Didon") != std::string::npos; });
        if (t.empty() && !pk.empty())
            h.push_back("TV-Inform (\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC): no GOST R 50822-95 lines; "
                        "packet datacast with 55 55 E7 sync on lines " + pk + " (possibly \xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC in packet form)");
        else
        h.push_back(t.empty() ? "TV-Inform (\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC): none"
                              : "TV-Inform (\xD0\xA2\xD0\x92-\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC): YES, lines " + t +
                                    " (GOST R 50822-95 datacast; ORT / Channel 1)" + std::string(pk2.empty() ? "" : "; in packets 55 55 E7 on lines " + pk2));
    }
    std::string tt = linesOf([](const LineInfo& li) { return li.kind.find("WST") != std::string::npos || li.kind == "Teletext stream" || li.kind == "DVB teletext"; });
    if (!tt.empty() || !r.teletext.project.empty())
        h.push_back("Teletext: yes" + (tt.empty() ? std::string() : ", lines " + tt) +
                    (r.teletext.pages ? ", " + std::to_string(r.teletext.pages) + " pages" : ""));
    else
        h.push_back("Teletext: none");
    std::vector<std::string> other;
    for (auto& li : r.lines) {
        if (li.mode == MODE_TVINFORM || li.mode == MODE_TEST || li.kind.rfind("TV-Inform", 0) == 0 || li.kind.find("WST") != std::string::npos || li.kind == "Teletext stream" ||
            li.kind.rfind("picture", 0) == 0 || li.kind.rfind("Test", 0) == 0)
            continue;
        if (std::find(other.begin(), other.end(), li.kind) == other.end()) other.push_back(li.kind);
    }
    if (!other.empty()) {
        std::string s = "Also: ";
        for (size_t i = 0; i < other.size(); i++) s += (i ? "; " : "") + other[i];
        h.push_back(s);
    }
    return h;
}

std::string summarize(const DecodeResult& r)
{
    std::string s;
    char b[512];
    snprintf(b, sizeof b, "File: %s\r\nFormat: %s, %lld frames (%.1f min)\r\nDecoder: %s, slicing %.2f s, total %.2f s\r\n\r\n",
             r.path.c_str(), formatName(r.geom.format), (long long)r.geom.frames,
             r.geom.frames / (r.geom.pal ? 25.0 : 29.97) / 60.0,
             r.slicerName.c_str(), r.sliceSeconds, r.totalSeconds);
    s += b;
    if (!r.source.empty()) {
        // the report starts with where the unpacked copy came from
        size_t e = s.find("\r\n");
        s.insert(e + 2, "Unpacked from: " + r.source + "\r\n");
    }
    for (auto& l : headline(r)) s += l + "\r\n";
    s += "\r\n";
    auto air = onAirSummary(r);
    if (!air.empty()) {
        s += "On the air:\r\n";
        for (auto& l : air) s += "  " + l + "\r\n";
        s += "\r\n";
    }
    s += "Lines:\r\n";
    for (auto& li : r.lines) {
        snprintf(b, sizeof b, "  line %2d field %d  %-13s %3.0f%%  %s\r\n", li.lineNo, li.field,
                 li.mode == MODE_CC ? "32xfH (608)" : li.mode == MODE_FAST ? (r.geom.pal ? "444xfH (MLSE)" : "364xfH (MLSE)")
                 : li.mode == MODE_AMOL ? "64xfH (AMOL)" : li.mode == MODE_GEMSTAR ? "64/32xfH" : li.mode == MODE_TVINFORM ? (li.tviRate == 0 ? "111xfH" : "222xfH")
                 : li.mode == MODE_TEST ? "-"
                 : li.kind.rfind("TV-Inform packets", 0) == 0 ? "366xfH (5.73)"
                 : li.kind.find("WST") != std::string::npos ? "444xfH"
                 : li.mode == MODE_SILENT_RADIO ? "80xfH" : "-",
                 li.detectRate * 100, li.kind.c_str());
        s += b;
    }
    const SsStats& st = r.db.stats;
    if (st.packets) {
        int good = st.crcOk + st.fixedSingle + st.fixedChase + st.fixedCombine;
        snprintf(b, sizeof b,
                 "\r\nStarSight: %d packets, %d good (%d clean, %d fixed 1-bit, %d fixed soft/Chase, %d fixed by "
                 "combining copies), %d failed, %d headers repaired\r\n",
                 st.packets, good, st.crcOk, st.fixedSingle, st.fixedChase, st.fixedCombine, st.failed,
                 st.headerFixed);
        s += b;
        s += "Commands:";
        for (auto& kv : st.commands) {
            const char* name = ssCommandName(kv.first);
            if (name[0]) snprintf(b, sizeof b, " %s x%d", name, kv.second);
            else snprintf(b, sizeof b, " type %d x%d", kv.first, kv.second);
            s += b;
        }
        snprintf(b, sizeof b,
                 "\r\nUpdates: show lists %d new, %d replaced (new version), %d unchanged; %d titles, %d "
                 "descriptions, %d channels\r\nDatabase: %zu channels with schedules, %zu show lists, %zu titles, %zu "
                 "descriptions\r\n",
                 st.listsAdded, st.listsReplaced, st.listsUnchanged, st.titlesAdded, st.descriptionsAdded,
                 st.channelsAdded, r.db.channelIds().size(), r.db.showLists.size(), r.db.programs.size(),
                 r.db.descriptions.size());
        s += b;
        if (st.encrypted) {
            s += "Encrypted commands (flag 0x80, 8-byte cipher blocks, cannot be read):";
            for (auto& kv : st.encryptedByType) {
                snprintf(b, sizeof b, " %s x%d", kv.first == 5 ? "show lists" : ("type " + std::to_string(kv.first)).c_str(), kv.second);
                s += b;
            }
            if (!st.timeKeys.empty()) {
                snprintf(b, sizeof b, "; the time command carries %zu key words that change every minute:", st.timeKeys.size());
                s += b;
                for (uint32_t k : st.timeKeys) { snprintf(b, sizeof b, " %08X", k); s += b; }
            }
            s += "\r\n";
        }
        int comp = 0, whole = 0;
        for (auto& kv : r.db.programs)
            if (kv.second.compressed) { comp++; whole += !kv.second.partial; }
        if (comp) {
            snprintf(b, sizeof b, "Titles: %d Huffman-compressed, %d decoded in full, %d cut short (codes not recovered)\r\n",
                     comp, whole, comp - whole);
            s += b;
        }
        // what was on and what came next on every channel at the broadcast clock
        // (schedules are sent days ahead: when the broadcast clock is before
        // them, the table is for the first listed moment)
        SsTime first = r.db.firstTime();
        for (SsTime now : {r.db.nowTime(), first}) {
            if (!now || r.db.programs.empty()) continue;
            std::string rows;
            int n = 0;
            for (int id : r.db.channelIds()) {
                auto v = r.db.slotsFor(id);
                for (size_t k = 0; k < v.size(); k++) {
                    if (!(v[k].start <= now && now < v[k].start + v[k].duration)) continue;
                    std::string name, lab, call;
                    auto it = r.db.channels.find(id);
                    if (it != r.db.channels.end() && !it->second.name.empty()) name = it->second.name;
                    else if (ssKnownChannel(id, lab, call)) name = call.empty() ? lab : call;
                    else name = "channel " + std::to_string(id);
                    auto hm = [&](SsTime t) {
                        int y, mo, d, h, mi, wd;
                        ssCivil(t + r.db.utcOffset(), y, mo, d, h, mi, wd);
                        char x[16];
                        snprintf(x, sizeof x, "%d:%02d", h, mi);
                        return std::string(x);
                    };
                    std::string line = "  " + name + std::string(name.size() < 10 ? 10 - name.size() : 1, ' ') + hm(v[k].start) + "  " + r.db.titleOf(v[k]);
                    if (k + 1 < v.size()) line += "   |  next " + hm(v[k + 1].start) + "  " + r.db.titleOf(v[k + 1]);
                    rows += line + "\r\n";
                    n++;
                    break;
                }
            }
            if (n) {
                int y, mo, d, h, mi, wd;
                ssCivil(now + r.db.utcOffset(), y, mo, d, h, mi, wd);
                SsTime last = 0;
                for (auto& kv : r.db.showLists)
                    for (auto& sl : kv.second.slots) last = std::max(last, sl.start + sl.duration);
                int y2, mo2, d2, h2, mi2, wd2;
                ssCivil(last + r.db.utcOffset(), y2, mo2, d2, h2, mi2, wd2);
                snprintf(b, sizeof b, "\r\nStarSight, on the air at %d/%d/%d %d:%02d local (%s) and next; listings run to %d/%d:\r\n", mo, d, y, h,
                         mi, now == first && now != r.db.nowTime() ? "first listed time: the broadcast clock is before the schedules sent" : "broadcast clock",
                         mo2, d2);
                s += b + rows;
                break;
            }
        }
    }
    if (!r.silentRadio.empty()) {
        snprintf(b, sizeof b, "Silent Radio: %zu messages\r\n", r.silentRadio.size());
        s += b;
    }
    if (r.fieldsSwapped) s += "Fields: the capture stores field 2 first (found from line 21: XDS or captions); fields are reported as broadcast\r\n";
    for (auto& a : r.amol) {
        s += "\r\n";
        for (auto& l : a.report) s += l + "\r\n";
    }
    for (auto& a : r.gemstar) {
        s += "\r\n";
        for (auto& l : a.report) s += l + "\r\n";
    }
    for (auto& a : r.tvinform) {
        s += "\r\n";
        for (auto& l : a.report) s += l + "\r\n";
    }
    if (!r.fastReport.empty()) {
        s += "\r\n";
        for (auto& l : r.fastReport) s += l + "\r\n";
    }
    if (!r.legacy.dir.empty()) {
        s += "\r\nTeletext Rescue engine (625 lines), results folder " + r.legacy.dir + ":\r\n";
        for (auto& x : r.legacy.results) s += "  " + x.service + ": " + x.path + (x.note.empty() ? "" : " \xE2\x80\x94 " + x.note) + "\r\n";
        for (auto& v : r.legacy.vits) s += "  " + v + "\r\n";
    }
    for (auto& l : r.log) s += l + "\r\n";
    return s;
}
