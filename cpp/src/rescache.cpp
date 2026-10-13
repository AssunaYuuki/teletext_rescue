#include "rescache.h"
#include "ui/util.h"
#include <cstdio>
#include <sys/stat.h>

namespace {

constexpr uint32_t kMagic = 0x31435254;   // "TRC1"
constexpr uint32_t kVersion = 7;          // bump when a saved structure changes

// One archive type for both directions: R = reading.
template <bool R> struct Ar {
    FILE* f = nullptr;
    bool ok = true;
    void raw(void* p, size_t n)
    {
        if (!ok || !n) return;
        size_t k = R ? fread(p, 1, n, f) : fwrite(p, 1, n, f);
        if (k != n) ok = false;
    }
};

template <bool R, class T> std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>> io(Ar<R>& a, T& v) { a.raw(&v, sizeof v); }
template <bool R> void io(Ar<R>& a, std::string& s)
{
    uint64_t n = s.size();
    io(a, n);
    if (R) { if (n > (1ull << 32)) { a.ok = false; return; } s.resize((size_t)n); }
    a.raw(s.data(), (size_t)n);
}
template <bool R, class T> void io(Ar<R>& a, std::vector<T>& v)
{
    uint64_t n = v.size();
    io(a, n);
    if (R) { if (n > (1ull << 34) || !a.ok) { a.ok = false; return; } v.resize((size_t)n); }
    if constexpr (std::is_arithmetic_v<T>) a.raw(v.data(), (size_t)n * sizeof(T));
    else for (auto& x : v) { io(a, x); if (!a.ok) return; }
}
template <bool R, class A, class B> void io(Ar<R>& a, std::pair<A, B>& p) { io(a, p.first); io(a, p.second); }
template <bool R, class K, class V> void io(Ar<R>& a, std::map<K, V>& m)
{
    uint64_t n = m.size();
    io(a, n);
    if (R) {
        m.clear();
        for (uint64_t i = 0; i < n && a.ok; i++) { std::pair<K, V> kv; io(a, kv); m.emplace(std::move(kv)); }
    } else
        for (auto& kv : m) { K k = kv.first; io(a, k); io(a, kv.second); }
}
template <bool R, class K> void io(Ar<R>& a, std::set<K>& m)
{
    uint64_t n = m.size();
    io(a, n);
    if (R) { m.clear(); for (uint64_t i = 0; i < n && a.ok; i++) { K k; io(a, k); m.insert(k); } }
    else for (K k : m) io(a, k);
}
template <bool R, class... T> void all(Ar<R>& a, T&... x) { (io(a, x), ...); }

template <bool R> void io(Ar<R>& a, VbiGeometry& g)
{
    all(a, g.format, g.fmt, g.samplesPerLine, g.validSamples, g.lineBytes, g.linesPerFrame, g.lineSamples, g.firstLine, g.pal,
        g.lineRate, g.fastRate, g.frames, g.storedPerField, g.skipLines);
}
template <bool R> void io(Ar<R>& a, LineInfo& l) { all(a, l.index, l.lineNo, l.field, l.mode, l.detectRate, l.kind, l.tviRate); }
// StarSight
template <bool R> void io(Ar<R>& a, SsChannel& c) { all(a, c.id, c.name, c.native, c.nameFlag, c.abbrevBits, c.verified); }
template <bool R> void io(Ar<R>& a, SsProgram& p) { all(a, p.id, p.title, p.theme, p.cc, p.stereo, p.bw, p.compressed, p.partial, p.raw); }
template <bool R> void io(Ar<R>& a, SsDescription& d)
{
    all(a, d.id, d.text, d.theme, d.critic, d.ratingSystem, d.rating, d.year, d.advisories, d.cc, d.stereo, d.bw, d.compressed, d.partial, d.raw);
}
template <bool R> void io(Ar<R>& a, SsSlot& s) { all(a, s.start, s.duration, s.program, s.description, s.flags); }
template <bool R> void io(Ar<R>& a, SsShowList& s) { all(a, s.channel, s.start, s.version, s.verified, s.slots); }
template <bool R> void io(Ar<R>& a, SsClock& c) { all(a, c.time, c.seconds, c.utcOffsetMinutes, c.dst); }
template <bool R> void io(Ar<R>& a, SsTheme& t) { all(a, t.id, t.name); }
template <bool R> void io(Ar<R>& a, SsStats& s)
{
    all(a, s.streamBytes, s.packets, s.crcOk, s.fixedSingle, s.fixedChase, s.fixedCombine, s.failed, s.headerFixed, s.commands,
        s.encrypted, s.encryptedByType, s.timeKeys, s.listsAdded, s.listsReplaced, s.listsUnchanged, s.titlesAdded,
        s.descriptionsAdded, s.channelsAdded);
}
template <bool R> void io(Ar<R>& a, SsDatabase& d)
{
    all(a, d.channels, d.programs, d.descriptions, d.showLists, d.themes, d.clock, d.dstChanges, d.sequenceNumbers, d.stats, d.log);
}
// Silent Radio
template <bool R> void io(Ar<R>& a, SrFrame& f) { all(a, f.scroll, f.cols, f.lines, f.hold, f.speed, f.wipe, f.what); }
template <bool R> void io(Ar<R>& a, SilentRadioMessage& m) { all(a, m.frame, m.text, m.zone, m.seq, m.copies, m.intact, m.texts, m.frames); }
// AMOL
template <bool R> void io(Ar<R>& a, AmolStamp& s) { all(a, s.frame, s.sid, s.month, s.day, s.hour, s.minute, s.second, s.pm, s.parityOk, s.bits, s.fa, s.inverted); }
template <bool R> void io(Ar<R>& a, AmolRun& r) { all(a, r.first, r.last, r.seconds, r.bad); }
template <bool R> void io(Ar<R>& a, AmolStream::Other& o) { all(a, o.fa, o.payload, o.count, o.firstFrame); }
template <bool R> void io(Ar<R>& a, AmolStream::Text& t) { all(a, t.frame, t.text); }
template <bool R> void io(Ar<R>& a, AmolStream& s)
{
    all(a, s.lineNo, s.field, s.words, s.parityOk, s.parityBad, s.idle, s.faFixed, s.stamps, s.runs, s.other, s.texts, s.report);
}
// Gemstar
template <bool R> void io(Ar<R>& a, GemstarWord& w) { all(a, w.frame, w.x2, w.b[0], w.b[1], w.b[2], w.b[3], w.ok, w.quality); }
template <bool R> void io(Ar<R>& a, GemstarMessage& m) { all(a, m.frame, m.x2, m.words, m.copies); }
template <bool R> void io(Ar<R>& a, GemstarPacket& p) { all(a, p.frame, p.x2, p.type, p.data, p.raw, p.lrc, p.copies); }
template <bool R> void io(Ar<R>& a, GemstarShow& s)
{
    all(a, s.channel, s.year, s.month, s.day, s.start, s.duration, s.theme, s.misc, s.code, s.frame, s.title, s.line1, s.line2);
}
template <bool R> void io(Ar<R>& a, GemstarStream& s)
{
    all(a, s.lineNo, s.field, s.words2x, s.idle2x, s.words1x, s.null1x, s.parityBad1x, s.blocks, s.blockNumbers, s.idleWords,
        s.words, s.messages, s.packets, s.blocksGood, s.blocksBad, s.keyYear, s.keyMonth, s.shows, s.guide, s.report);
}
// ТВ-информ
template <bool R> void io(Ar<R>& a, TviWord& w) { all(a, w.frame, w.lineNo, w.field, w.bytes, w.quality); }
template <bool R> void io(Ar<R>& a, TviPage& p) { all(a, p.lineNo, p.field, p.copies, p.bytes); }
template <bool R> void io(Ar<R>& a, TviStream& s) { all(a, s.rate, s.nbits, s.words, s.pages, s.entropy, s.report); }
// 5.7 Mbit/s, teletext, old engine
template <bool R> void io(Ar<R>& a, FastPacket& p) { all(a, p.frame, p.lineNo, p.field, p.kind, p.syncErr, p.crc, p.bytes); }
template <bool R> void io(Ar<R>& a, TtResult& t) { all(a, t.project, t.packets, t.pages, t.subpages, t.service, t.system); }
template <bool R> void io(Ar<R>& a, LegacyLine& l) { all(a, l.row, l.tvLine, l.kind, l.signal); }
template <bool R> void io(Ar<R>& a, LegacyResult& r) { all(a, r.service, r.kind, r.path, r.note); }
template <bool R> void io(Ar<R>& a, LegacyReport& r) { all(a, r.format, r.dir, r.lines, r.results, r.vits); }

// Japanese teletext
template <bool R> void io(Ar<R>& a, JtesUnit& u) { all(a, u.type, u.data); }
template <bool R> void io(Ar<R>& a, JtesPage& p)
{
    all(a, p.channel, p.line, p.programme, p.page, p.hi, p.frame, p.seen, p.gaps, p.raster, p.header, p.units, p.title);
}
template <bool R> void io(Ar<R>& a, JtesResult& j) { all(a, j.packets, j.clean, j.fixed, j.failed, j.groups, j.channels, j.pages, j.report); }

template <bool R> void io(Ar<R>& a, DecodeResult& r)
{
    all(a, r.path, r.source, r.captureNotes, r.geom, r.slicerName, r.sliceSeconds, r.totalSeconds, r.lines, r.db, r.captions,
        r.xds, r.silentRadio, r.amol, r.gemstar, r.tvinform, r.fieldsSwapped, r.fastPackets, r.fastReport, r.fastText,
        r.nabtsT33, r.nabtsField, r.nabtsLine, r.legacy, r.teletext, r.colour, r.log, r.jtes);
}

// size and modification time of the recording: saved results only fit the same file
bool stamp(const std::string& p, int64_t& size, int64_t& mtime)
{
    struct _stat64 st;
    if (_wstat64(widen(p).c_str(), &st) != 0) return false;
    size = st.st_size;
    mtime = st.st_mtime;
    return true;
}

template <bool R> bool header(Ar<R>& a, int64_t& size, int64_t& mtime)
{
    uint32_t m = kMagic, v = kVersion;
    all(a, m, v, size, mtime);
    return a.ok && m == kMagic && v == kVersion;
}

} // namespace

std::string resultsCachePath(const std::string& recording) { return path_join(legacyReportDir(recording), "results.trc"); }

bool resultsCached(const std::string& recording)
{
    int64_t s, t, s2 = 0, t2 = 0;
    if (!stamp(recording, s, t)) return false;
    FILE* f = _wfopen(widen(resultsCachePath(recording)).c_str(), L"rb");
    if (!f) return false;
    Ar<true> a{f};
    bool ok = header(a, s2, t2) && s2 == s && t2 == t;
    fclose(f);
    return ok;
}

bool saveResults(const std::string& recording, const DecodeResult& r)
{
    int64_t s, t;
    if (!stamp(recording, s, t)) return false;
    std::string p = resultsCachePath(recording);
    make_dirs(legacyReportDir(recording));
    FILE* f = _wfopen(widen(p + ".part").c_str(), L"wb");
    if (!f) return false;
    Ar<false> a{f};
    header(a, s, t);
    io(a, const_cast<DecodeResult&>(r));
    bool ok = a.ok;
    fclose(f);
    if (!ok) return false;
    _wremove(widen(p).c_str());
    return _wrename(widen(p + ".part").c_str(), widen(p).c_str()) == 0;
}

bool loadResults(const std::string& recording, DecodeResult& r, std::string& err)
{
    int64_t s, t, s2 = 0, t2 = 0;
    if (!stamp(recording, s, t)) { err = "cannot read the recording"; return false; }
    FILE* f = _wfopen(widen(resultsCachePath(recording)).c_str(), L"rb");
    if (!f) { err = "no saved results"; return false; }
    Ar<true> a{f};
    bool ok = header(a, s2, t2) && s2 == s && t2 == t;
    if (ok) {
        r = DecodeResult{};
        io(a, r);
        ok = a.ok;
    }
    fclose(f);
    if (!ok) { err = "the saved results do not fit this recording"; return false; }
    // NABTS records hold pointers: rebuilt from the saved packet stream
    if (!r.nabtsT33.empty()) {
        r.nabtsRecords = nabts::read_packets(r.nabtsT33, r.nabtsSummary);
        nabts::interpret(r.nabtsRecords, 256, 200);
    }
    r.log.push_back("opened the saved results: " + resultsCachePath(recording));
    return true;
}
