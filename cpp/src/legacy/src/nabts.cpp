// Разбор NABTS/NAPLPS.
#include "nabts.h"
#include "teletext.h"
#include <functional>

namespace nabts {
namespace {
#include "gen_naplps_font.inc"

const int PACKET = 33, PREFIX = 5, MAX_BLOCK = 28, GROUP_HDR = 8, MAX_FURTHER = 67, MAX_OPEN_GROUPS = 32, MAX_COPIES = 16;
inline int HAM(u8 b) { return ham::fix[b]; }
inline bool HAMC(u8 b) { return ham::dec[b] >= 0; }
inline bool ODD(u8 b) { return odd_parity(b); }

// ---------------------------------------------------------------- пакеты (§3)
struct Packet { bool valid = false; int channel = 0; bool attested = false; int ci = 0; u8 ci_byte = 0; bool sync = false; int suffix = 0; Bytes data; int integrity = 0; };
enum { UNCHECKED, CLEAN, CORRECTED, DAMAGED };

Packet decode_packet(const u8 *p) {
    Packet out;
    int pre[5];
    for (int i = 0; i < 5; i++) { pre[i] = HAM(p[i]); if (pre[i] < 0) return out; }
    out.valid = true;
    out.channel = (pre[0] << 8) | (pre[1] << 4) | pre[2];
    out.attested = HAMC(p[0]) && HAMC(p[1]) && HAMC(p[2]);
    out.ci = pre[3]; out.ci_byte = p[3];
    int ps = pre[4];
    out.sync = ps & 1;
    out.suffix = (((ps >> 3) & 1) << 1) | ((ps >> 2) & 1);
    static const int cut[4] = {0, 1, 2, MAX_BLOCK};
    int n = MAX_BLOCK - cut[out.suffix];
    out.data.assign(p + PREFIX, p + PREFIX + n);
    if (out.suffix == 1 || out.suffix == 2) {
        int lrc = 0; for (int i = 0; i < MAX_BLOCK; i++) lrc ^= p[PREFIX + i];
        int syn = lrc ^ 0xFF;
        if (syn == 0) out.integrity = CLEAN;
        else if ((syn & (syn - 1)) == 0) {
            std::vector<int> bad; for (int i = 0; i < MAX_BLOCK; i++) if (!ODD(p[PREFIX + i])) bad.push_back(i);
            if (bad.size() == 1) { if (bad[0] < n) out.data[bad[0]] ^= syn; out.integrity = CORRECTED; }
            else out.integrity = DAMAGED;
        } else out.integrity = DAMAGED;
    }
    return out;
}

// ---------------------------------------------------------------- группы (§4)
struct GHdr { bool attested; int type, ci, rep, further, final_bytes, routing; };
bool decode_group_header(const Bytes &b, GHdr &h) {
    if (b.size() < GROUP_HDR) return false;
    int n[8]; bool att = true;
    for (int i = 0; i < 8; i++) { n[i] = HAM(b[i]); if (n[i] < 0) return false; att &= HAMC(b[i]); }
    h = {att, n[0], n[1], n[2], (n[3] << 4) | n[4], (n[5] << 4) | n[6], n[7]};
    return true;
}
struct GroupOut { int channel; bool channel_attested; GHdr hdr; std::string outcome; int lost, damaged; Bytes data, present; bool intact; long clock; };

struct GroupAssembler {
    struct G { GHdr hdr; bool attested; int last_ci; long last_line; Bytes stream, present; int nominal; bool placeable; int seen, last_off, last_len, lost, damaged; };
    std::function<void(GroupOut &)> cb;
    std::map<int, G> open;
    long clock = 0;
    std::map<std::string, long> stats{{"packets", 0}, {"prefix_bad", 0}, {"header_bad", 0}, {"orphans", 0}, {"complete", 0}, {"superseded", 0}, {"unfinished", 0}, {"foreign", 0}};
    void append(G &g, const Packet &pk) {
        if (pk.data.empty()) return;
        g.last_off = (int)g.stream.size(); g.last_len = (int)pk.data.size();
        g.stream.insert(g.stream.end(), pk.data.begin(), pk.data.end());
        g.present.insert(g.present.end(), pk.data.size(), g.placeable ? 1 : 0);
    }
    void hole(G &g, int blocks) { size_t n = (size_t)blocks * g.nominal; g.stream.insert(g.stream.end(), n, 0); g.present.insert(g.present.end(), n, 0); }
    void emit(int ch, G &g, const std::string &outcome) {
        int end = g.last_off + g.last_len;
        if (g.hdr.final_bytes == 0) end = g.last_off;
        else if (g.hdr.final_bytes < g.last_len) end = g.last_off + g.hdr.final_bytes;
        end = std::min(end, (int)g.stream.size());
        GroupOut o{ch, g.attested, g.hdr, outcome, g.lost, g.damaged, {}, {}, false, clock};
        if (end > GROUP_HDR) { o.data.assign(g.stream.begin() + GROUP_HDR, g.stream.begin() + end); o.present.assign(g.present.begin() + GROUP_HDR, g.present.begin() + end); }
        o.intact = outcome == "complete" && g.lost == 0 && g.damaged == 0;
        stats[outcome]++;
        if (outcome == "complete" && g.hdr.type != 0) stats["foreign"]++;
        cb(o);
    }
    void add(const Packet &pk) {
        stats["packets"]++; clock++;
        if (!pk.valid) { stats["prefix_bad"]++; return; }
        if (pk.sync) begin(pk); else extend(pk);
    }
    void begin(const Packet &pk) {
        GHdr h;
        if (!decode_group_header(pk.data, h)) { stats["header_bad"]++; return; }
        if (h.further > MAX_FURTHER) return;
        auto it = open.find(pk.channel);
        if (it != open.end()) { G old = it->second; open.erase(it); emit(pk.channel, old, "superseded"); }
        else if ((int)open.size() >= MAX_OPEN_GROUPS) return;
        G g{h, pk.attested, pk.ci, clock, {}, {}, (int)pk.data.size(), true, 0, 0, 0, 0, pk.integrity == DAMAGED ? 1 : 0};
        append(g, pk);
        if (h.further == 0) { emit(pk.channel, g, "complete"); return; }
        open[pk.channel] = g;
    }
    int gap(G &g, const Packet &pk, long lines) {
        int exp = (g.last_ci + 1) % 16, read = ((pk.ci - exp) % 16 + 16) % 16;
        if (read <= lines) return read;
        int room = g.hdr.further - g.seen;
        int limit = (int)std::min<long>({lines, (long)room, 15L});
        int best = 0, bestd = 99;
        for (int c = 0; c <= limit; c++) { int d = popcount8(pk.ci_byte ^ ham::CODEWORDS[(exp + c) % 16]); if (d < bestd) { best = c; bestd = d; } }
        return best;
    }
    void extend(const Packet &pk) {
        auto it = open.find(pk.channel);
        if (it == open.end()) { stats["orphans"]++; return; }
        G &g = it->second;
        int gp = gap(g, pk, 15);
        if (gp) {
            g.lost += gp;
            int room = g.hdr.further - g.seen;
            g.seen = std::min(g.seen + gp, g.hdr.further);
            if (gp > room) g.placeable = false; else hole(g, gp);
        }
        g.last_ci = (g.last_ci + 1 + gp) % 16; g.last_line = clock;
        if (pk.integrity == DAMAGED) g.damaged++;
        append(g, pk); g.seen++;
        if (g.seen >= g.hdr.further) { G done = g; open.erase(it); emit(pk.channel, done, "complete"); }
    }
    void flush() { while (!open.empty()) { auto it = open.begin(); G g = it->second; int ch = it->first; open.erase(it); emit(ch, g, "unfinished"); } }
};

// ---------------------------------------------------------------- записи (§5)
const char *FLAG_NAMES[] = {"caption", "delay", "index", "more", "cyclic", "auto_acquire", "support_needed", "priority", "alarm", "update", "support_record"};
struct Cursor {
    const Bytes &b; size_t pos = 0; bool failed = false, clean = true;
    explicit Cursor(const Bytes &x) : b(x) {}
    int next() {
        if (failed || pos >= b.size()) { failed = true; return -1; }
        int v = HAM(b[pos]);
        if (v < 0) { failed = true; return -1; }
        if (!HAMC(b[pos])) clean = false;
        pos++; return v;
    }
};
struct Ext { int meaning, size; std::vector<int> data; };
struct RHdr { int type; long long address; bool long_form, linked = false, more_links = false; int order = 0; std::map<std::string, bool> flags; int version = 0; bool attested; std::vector<Ext> ext; size_t header_bytes; };

bool decode_record_header(const Bytes &b, RHdr &h) {
    if (b.size() < 5) return false;
    Cursor c(b);
    int rt = c.next(), rd = c.next();
    if (c.failed) return false;
    h.type = rt;
    int ext = rd & 1, link = rd & 2, cls = rd & 4, hext = rd & 8, n = ext ? 9 : 3;
    std::vector<int> digits;
    for (int i = 0; i < n; i++) digits.push_back(c.next());
    if (c.failed) return false;
    if (!ext) { digits.insert(digits.begin(), 4, 0); digits.push_back(0); digits.push_back(0); }
    long long a = 0; for (int d : digits) a = (a << 4) | d;
    h.address = a; h.long_form = ext;
    if (link) {
        int l1 = c.next(), l2 = c.next();
        if (c.failed) return false;
        h.linked = true; h.more_links = l1 & 8; h.order = ((l1 & 7) << 4) | l2;
    }
    for (auto *k : FLAG_NAMES) h.flags[k] = false;
    if (cls) {
        int group = 1;
        for (;;) {
            int ptr = c.next(); if (c.failed) return false;
            for (int pair = 0; pair < 3; pair++) {
                if (!((ptr >> pair) & 1)) continue;
                for (int half = 0; half < 2; half++) {
                    int f = c.next(); if (c.failed) return false;
                    int idx = pair * 2 + half + 1;
                    if (group != 1) continue;
                    if (idx == 3) { h.flags["caption"] = f & 8; h.flags["delay"] = f & 4; h.flags["index"] = f & 2; }
                    else if (idx == 4) { h.flags["more"] = f & 8; h.flags["cyclic"] = f & 4; h.flags["auto_acquire"] = f & 2; h.flags["support_needed"] = f & 1; }
                    else if (idx == 5) { h.flags["priority"] = f & 8; h.flags["alarm"] = f & 4; h.flags["update"] = f & 2; h.flags["support_record"] = f & 1; }
                    else if (idx == 6) h.version = f;
                }
            }
            if (!(ptr & 8)) break;
            group++;
        }
    }
    h.attested = c.clean;
    if (hext)
        for (;;) {
            int intro = c.next(), size = c.next(); if (c.failed) return false;
            Ext e{intro & 7, size, {}};
            for (int i = 0; i < size; i++) e.data.push_back(c.next());
            if (c.failed) return false;
            h.ext.push_back(e);
            if (!(intro & 8)) break;
        }
    h.header_bytes = c.pos;
    return true;
}

std::string reserved_purpose(int channel, long long a) {
    long long sh = (((a >> 20) & 0xFFFF) == 0 && (a & 0xFF) == 0) ? (a >> 8) & 0xFFF : 0x1000;
    if (sh == 0xFFF) return "Support Record";
    if (channel == 0 && sh == 0) return "Master Index / power-up";
    if (channel == 0 && sh == 0xFFE) return "Service Application Record";
    if (channel == 0xA00 && sh == 0) return "Start of captioning";
    if (channel == 0xB00 && sh == 0) return "Start of Flash";
    return "";
}

struct Msg { int channel; long long address; bool long_form; int type; std::map<std::string, bool> flags; int version; std::vector<Ext> ext; Bytes data, present; bool complete, intact, attested, aligned; int records; long clock; };

struct RecordAssembler {
    struct Open { Msg m; std::map<int, std::pair<Bytes, Bytes>> parts; int final = 999; long seq; };
    std::function<void(Msg &)> cb;
    std::map<std::tuple<int, long long, int>, Open> open;
    long seq = 0;
    std::map<std::string, long> stats{{"groups", 0}, {"foreign", 0}, {"header_bad", 0}, {"records", 0}};
    std::map<std::pair<int, int>, long> foreign;
    void add(GroupOut &g) {
        stats["groups"]++;
        if (g.hdr.type != 0) {
            stats["foreign"]++;
            if (g.channel_attested && g.hdr.attested) foreign[{g.channel, g.hdr.type}]++;
            return;
        }
        RHdr h;
        if (!decode_record_header(g.data, h)) { stats["header_bad"]++; return; }
        stats["records"]++;
        Bytes data(g.data.begin() + std::min(h.header_bytes, g.data.size()), g.data.end());
        Bytes present(g.present.begin() + std::min(h.header_bytes, g.present.size()), g.present.end());
        bool attested = h.attested && g.channel_attested;
        if (!h.linked) {
            Msg m{g.channel, h.address, h.long_form, h.type, h.flags, h.version, h.ext, data, present, true, g.intact, attested, true, 1, g.clock};
            cb(m); return;
        }
        auto key = std::make_tuple(g.channel, h.address, h.version);
        auto it = open.find(key);
        if (it == open.end()) {
            if (open.size() >= 64) {
                auto old = open.begin();
                for (auto q = open.begin(); q != open.end(); ++q) if (q->second.seq < old->second.seq) old = q;
                Open o = old->second; open.erase(old); emit(o, false);
            }
            Open o; o.m = Msg{g.channel, h.address, h.long_form, h.type, h.flags, h.version, h.ext, {}, {}, false, true, false, false, 0, 0};
            o.seq = seq++;
            it = open.emplace(key, o).first;
        }
        Open &o = it->second;
        if (h.order == 0) { o.m.flags = h.flags; o.m.ext = h.ext; o.m.type = h.type; }
        if (!g.intact) o.m.intact = false;
        if (attested) o.m.attested = true;
        if (!h.more_links) o.final = h.order;
        o.parts[h.order] = {data, present};
        o.m.clock = g.clock;
        if (o.final <= 127 && (int)o.parts.size() == o.final + 1) { Open done = o; open.erase(it); emit(done, true); }
    }
    void emit(Open &o, bool complete) {
        o.m.complete = complete; o.m.aligned = complete; o.m.records = (int)o.parts.size();
        o.m.data.clear(); o.m.present.clear();
        for (auto &kv : o.parts) { o.m.data.insert(o.m.data.end(), kv.second.first.begin(), kv.second.first.end()); o.m.present.insert(o.m.present.end(), kv.second.second.begin(), kv.second.second.end()); }
        cb(o.m);
    }
    void flush() { while (!open.empty()) { Open o = open.begin()->second; open.erase(open.begin()); emit(o, false); } }
};

// ---------------------------------------------------------------- каталог и голосование
using Copy = std::pair<Bytes, Bytes>;
size_t voted_length(const std::vector<Copy> &copies, const std::vector<char> *inc) {
    size_t best = 0; int sb = 0;
    for (size_t i = 0; i < copies.size(); i++) {
        if (inc && !(*inc)[i]) continue;
        int s = 0;
        for (size_t j = 0; j < copies.size(); j++) if ((!inc || (*inc)[j]) && copies[j].first.size() == copies[i].first.size()) s++;
        if (s > sb || (s == sb && copies[i].first.size() > best)) { best = copies[i].first.size(); sb = s; }
    }
    return best;
}
Copy vote_over(const std::vector<Copy> &copies, const std::vector<char> *inc = nullptr) {
    size_t n = voted_length(copies, inc);
    Bytes data(n, 0), present(n, 0);
    for (size_t pos = 0; pos < n; pos++) {
        std::vector<std::pair<int, int>> w; std::map<int, int> newest;     // значение -> вес (порядок появления)
        for (size_t ci = 0; ci < copies.size(); ci++) {
            if (inc && !(*inc)[ci]) continue;
            auto &d = copies[ci].first; auto &pr = copies[ci].second;
            if (pos >= d.size() || (pos < pr.size() && !pr[pos])) continue;
            int v = d[pos]; bool f = false;
            for (auto &x : w) if (x.first == v) { x.second += 255; f = true; }
            if (!f) w.push_back({v, 255});
            newest[v] = (int)ci;
        }
        if (w.empty()) continue;
        int best = w[0].first, bw = w[0].second;
        for (size_t k = 1; k < w.size(); k++) {
            int v = w[k].first; bool bc = ODD(best), vc = ODD(v);
            if (bc && !vc) continue;
            if (bc == vc && !(w[k].second > bw || (w[k].second == bw && newest[v] > newest[best]))) continue;
            best = v; bw = w[k].second;
        }
        data[pos] = (u8)best; present[pos] = 1;
    }
    return {data, present};
}
std::pair<int, int> agreement(const Copy &c, const Copy &vote, int slip) {
    int judged = 0, agreed = 0;
    for (size_t pos = 0; pos < vote.first.size(); pos++) {
        long src = (long)pos + slip;
        if (!vote.second[pos] || src < 0 || src >= (long)c.first.size() || (src < (long)c.second.size() && !c.second[src])) continue;
        judged++; agreed += c.first[src] == vote.first[pos];
    }
    return {judged, agreed};
}
bool outlier(std::pair<int, int> a) { return a.first >= 16 && a.second * 100 < a.first * 50; }
bool better(std::pair<int, int> a, std::pair<int, int> b) { return (long)a.second * std::max(1, b.first) > (long)b.second * std::max(1, a.first); }
Copy slide(const Copy &c, int slip) {
    long n = std::max(0L, (long)c.first.size() - slip);
    Bytes od(n, 0), op(n, 0);
    for (long pos = 0; pos < n; pos++) {
        long src = pos + slip;
        if (src < 0 || src >= (long)c.first.size() || (src < (long)c.second.size() && !c.second[src])) continue;
        od[pos] = c.first[src]; op[pos] = 1;
    }
    return {od, op};
}
Copy vote_record(const std::vector<Copy> &copies) {
    if (copies.empty()) return {};
    if (copies.size() == 1) return copies[0];
    Copy prov = vote_over(copies);
    std::vector<Copy> slid = copies; std::vector<char> inc(copies.size(), 1); int dropped = 0;
    for (size_t i = 0; i < copies.size(); i++) {
        auto un = agreement(copies[i], prov, 0);
        if (!outlier(un)) continue;
        auto best = un; int bs = 0;
        for (int s = -8; s <= 8; s++) { if (!s) continue; auto a = agreement(copies[i], prov, s); if (!outlier(a) && better(a, best)) { best = a; bs = s; } }
        if (bs) { slid[i] = slide(copies[i], bs); continue; }
        inc[i] = 0; dropped++;
    }
    if (dropped == 0 || dropped * 2 >= (int)copies.size()) return vote_over(slid);
    return vote_over(slid, &inc);
}
bool algorithmic_more(long long a, long long &out) {
    int tens = (a >> 4) & 0xF, units = a & 0xF;
    if (tens > 9 || units > 9) return false;
    int v = tens * 10 + units + 1;
    if (v > 99) return false;
    out = (a & ~0xFFLL) | ((v / 10) << 4) | (v % 10); return true;
}
bool more_extension(const std::vector<Ext> &exts, long long &out) {
    for (auto &e : exts) {
        if (e.meaning != 1) continue;
        if (e.size == 0) { out = 0; return true; }
        if (e.size == 3 || e.size == 9) { long long v = 0; for (int nb : e.data) v = (v << 4) | (nb & 0xF); out = e.size == 3 ? v << 8 : v; return true; }
    }
    return false;
}
const char *FLAGS_VOTED[] = {"caption", "cyclic", "priority", "alarm", "update", "support_record", "support_needed", "index", "more"};

struct Entry {
    int channel; long long address; int version; bool long_form; long first, last = 0;
    int seen = 0, intact_n = 0, attested_n = 0;
    std::map<std::string, int> flags_set, flags_att;
    std::vector<Copy> copies; bool kept_intact = false;
    int type = 0, records = 0; bool complete = false; Bytes data, present; bool has_more = false; long long more_address = 0;
};
struct Catalogue {
    std::map<std::tuple<int, long long, int>, Entry> entries;
    static void take(Entry &e, const Msg &m) {
        e.type = m.type; e.records = m.records; e.complete = m.complete; e.data = m.data; e.present = m.present;
        e.has_more = more_extension(m.ext, e.more_address);
        e.kept_intact = m.complete && m.intact;
    }
    void merge(const Msg &m) {
        auto key = std::make_tuple(m.channel, m.address, m.version);
        bool good = m.complete && m.intact;
        auto it = entries.find(key);
        Entry *e;
        if (it == entries.end()) {
            Entry ne; ne.channel = m.channel; ne.address = m.address; ne.version = m.version; ne.long_form = m.long_form; ne.first = m.clock;
            for (auto *k : FLAGS_VOTED) { ne.flags_set[k] = 0; ne.flags_att[k] = 0; }
            take(ne, m);
            e = &(entries[key] = ne);
        } else {
            e = &it->second;
            if ((good != e->kept_intact && good) || (good == e->kept_intact && m.data.size() > e->data.size())) take(*e, m);
        }
        if (good) e->copies.clear();
        else if (!e->kept_intact && m.aligned) { if (e->copies.size() >= MAX_COPIES) e->copies.erase(e->copies.begin()); e->copies.push_back({m.data, m.present}); }
        for (auto *k : FLAGS_VOTED) {
            auto f = m.flags.find(k);
            if (f != m.flags.end() && f->second) { e->flags_set[k]++; if (m.attested) e->flags_att[k]++; }
        }
        e->last = m.clock; e->seen++; e->intact_n += good; e->attested_n += m.attested;
    }
    std::pair<int, int> reconcile() {
        auto digits = [](const Entry &e) {
            std::vector<int> d;
            for (int s : {8, 4, 0}) d.push_back((e.channel >> s) & 0xF);
            for (int s = 32; s >= 0; s -= 4) d.push_back((e.address >> s) & 0xF);
            d.push_back(e.version & 0xF);
            return d;
        };
        std::vector<std::pair<std::tuple<int, long long, int>, std::vector<int>>> att;
        for (auto &kv : entries) if (kv.second.attested_n > 0) att.push_back({kv.first, digits(kv.second)});
        if (att.empty()) return {0, 0};
        int folded = 0, dropped = 0;
        std::vector<std::tuple<int, long long, int>> weak;
        for (auto &kv : entries) if (kv.second.attested_n == 0) weak.push_back(kv.first);
        for (auto &k : weak) {
            Entry e = entries[k]; entries.erase(k);
            auto d = digits(e);
            bool have = false, ambiguous = false; std::tuple<int, long long, int> found;
            for (auto &t : att) {
                int diff = 0; for (size_t i = 0; i < d.size(); i++) diff += d[i] != t.second[i];
                if (diff == 1) { if (have) { ambiguous = true; break; } found = t.first; have = true; }
            }
            if (!have || ambiguous) { dropped++; continue; }
            Entry &t = entries[found];
            t.seen += e.seen; t.intact_n += e.intact_n;
            for (auto *f : FLAGS_VOTED) t.flags_set[f] += e.flags_set[f];
            t.first = std::min(t.first, e.first); t.last = std::max(t.last, e.last);
            if (!t.kept_intact) for (auto &c : e.copies) { if (t.copies.size() >= MAX_COPIES) break; t.copies.push_back(c); }
            folded++;
        }
        return {folded, dropped};
    }
    std::vector<Record> records() {
        std::vector<Record> out;
        for (auto &kv : entries) {
            Entry &e = kv.second; Record r;
            r.channel = e.channel; r.address = e.address; r.version = e.version; r.long_form = e.long_form;
            r.type = e.type; r.records = e.records; r.complete = e.complete; r.data = e.data; r.present = e.present;
            r.has_more = e.has_more; r.more_address = e.more_address; r.seen = e.seen; r.intact_n = e.intact_n; r.attested_n = e.attested_n;
            r.first = e.first; r.last = e.last;
            bool a = e.attested_n > 0; int voters = a ? e.attested_n : e.seen;
            auto &src = a ? e.flags_att : e.flags_set;
            for (auto *k : FLAGS_VOTED) r.flags[k] = src[k] * 2 > voters;
            r.copies_voted = (int)e.copies.size();
            if (!e.copies.empty()) { auto v = vote_record(e.copies); r.data = v.first; r.present = v.second; }
            r.addr_text = !e.long_form ? address_text(e.address) : fmt("%09llX", e.address);
            r.purpose = reserved_purpose(e.channel, e.address);
            out.push_back(r);
        }
        return out;
    }
};

// ---------------------------------------------------------------- цвет
const Col BLACK{0, 0, 0, false}, WHITE{7, 7, 7, false}, TRANSPARENT{0, 0, 0, true};
Col hue(double angle) {
    struct P { double a; int i; } prim[3] = {{240.0, 0}, {120.0, 1}, {0.0, 2}};
    auto dist = [](double a, double b) { double d = fabs(a - b); return d > 180 ? 360 - d : d; };
    std::vector<std::pair<double, int>> ds;
    for (auto &p : prim) ds.push_back({dist(angle, p.a), p.i});
    std::sort(ds.begin(), ds.end());
    int c[3] = {0, 0, 0};
    c[ds[0].second] = 7; c[ds[2].second] = 0; c[ds[1].second] = (int)(ds[0].first / 60.0 * 7 + 0.5);
    return {c[0], c[1], c[2], false};
}
std::array<Col, 16> default_map() {
    std::array<Col, 16> m;
    for (int i = 0; i < 8; i++) m[i] = {i, i, i, false};
    for (int i = 0; i < 8; i++) m[8 + i] = hue(360.0 * i / 8);
    return m;
}
const std::array<Col, 16> DEFAULT_MAP = default_map();

struct ColourState {
    int mode = 0; Col direct = WHITE; int draw_addr = 0, bg_addr = 0;
    std::array<Col, 16> map = DEFAULT_MAP; std::array<bool, 16> used{};
    void reset() { mode = 0; direct = WHITE; draw_addr = 0; bg_addr = 0; reset_map(); }
    void reset_map() { map = DEFAULT_MAP; used.fill(false); }
    void reset_drawing_to_white() { mode = 0; direct = WHITE; }
    void select_mapped(int a) { mode = 1; draw_addr = ((a % 16) + 16) % 16; used[draw_addr] = true; }
    void select_mapped_bg(int a, int b) { mode = 2; a = ((a % 16) + 16) % 16; b = ((b % 16) + 16) % 16; if (a != b) { draw_addr = a; used[a] = true; } bg_addr = b; used[b] = true; }
    int index_of(const Col &c) const { for (int i = 0; i < 16; i++) if (map[i] == c) return i; return -1; }
    void reset_to_mapped(bool white) { reset_map(); mode = 1; if (white) { int i = index_of(WHITE); if (i >= 0) draw_addr = i; } }
    void set_colour(const Col &c) {
        if (mode != 0) { map[draw_addr] = c; used[draw_addr] = true; return; }
        direct = c;
        int i = index_of(c);
        if (i >= 0) { draw_addr = i; return; }
        for (int k = 0; k < 16; k++) { if (used[k] || map[k] == BLACK || map[k] == WHITE) continue; map[k] = c; used[k] = true; draw_addr = k; return; }
    }
    void write(int a, const Col &c) { a = ((a % 16) + 16) % 16; map[a] = c; used[a] = true; }
    void set_transparent() { if (mode == 0) direct = TRANSPARENT; else map[draw_addr] = TRANSPARENT; }
    Col drawing() const { return mode == 0 ? direct : map[draw_addr]; }
    Col background() const { return mode == 2 ? map[bg_addr] : BLACK; }
};
int increment_addr(int a) {
    for (int bit = 3; bit >= 0; bit--) {
        int m = 1 << bit;
        if (!(a & m)) { a |= m; for (int hi = 3; hi > bit; hi--) a &= ~(1 << hi); return a; }
    }
    return -1;
}
int addr_from_operand(int v, int nbytes) { int bits = std::max(1, nbytes) * 6; return bits <= 4 ? v << (4 - bits) : v >> (bits - 4); }

// ---------------------------------------------------------------- операнды PDI
double signed_frac(long bits, int n) {
    if (n <= 0) return 0.0;
    int neg = (bits >> (n - 1)) & 1;
    long mag = bits & ((1L << (n - 1)) - 1);
    double v = n > 1 ? (double)mag / (double)(1L << (n - 1)) : 0.0;
    return neg ? v - 1.0 : v;
}
struct Operands {
    const Bytes &b; size_t pos = 0; int fmt_[3]; bool truncated = false;
    Operands(const Bytes &x, const int f[3]) : b(x) { fmt_[0] = f[0]; fmt_[1] = f[1]; fmt_[2] = f[2]; }
    bool empty() const { return pos >= b.size(); }
    int remaining() const { return (int)(b.size() - pos); }
    int nxt() { if (pos >= b.size()) { truncated = true; return 0; } return b[pos++] & 0x3F; }
    int fixed() { truncated = false; return nxt(); }
    long single() { truncated = false; long v = 0; for (int i = 0; i < fmt_[0]; i++) v = (v << 6) | nxt(); return v; }
    Pt coord() {
        truncated = false;
        int comps = fmt_[2] ? 3 : 2, bpb = 6 / comps, mask = (1 << bpb) - 1, xs = 6 - bpb, ys = 6 - 2 * bpb;
        long x = 0, y = 0; int nb = 0;
        for (int i = 0; i < fmt_[1]; i++) { int p = nxt(); x = (x << bpb) | ((p >> xs) & mask); y = (y << bpb) | ((p >> ys) & mask); nb += bpb; }
        return {signed_frac(x, nb), signed_frac(y, nb)};
    }
    Col colour() {
        truncated = false;
        int words = std::max(1, std::min(fmt_[1], remaining()));
        int g = 0, r = 0, bl = 0, bits = 0;
        for (int i = 0; i < words; i++) {
            int p = nxt();
            for (int t : {1, 0}) { int base = t * 3; g = (g << 1) | ((p >> (base + 2)) & 1); r = (r << 1) | ((p >> (base + 1)) & 1); bl = (bl << 1) | ((p >> base) & 1); bits++; }
        }
        auto gun = [&](int v) { if (bits >= 3) return v >> (bits - 3); int mx = (1 << bits) - 1; return mx ? (v * 7 + mx / 2) / mx : 0; };
        return {gun(g), gun(r), gun(bl), false};
    }
};
std::pair<Pt, bool> clamp_unit(Pt p) {
    bool c = false;
    if (!(p.first >= 0.0 && p.first < 1.0)) { p.first = std::min(std::max(p.first, 0.0), 0.9999999); c = true; }
    if (!(p.second >= 0.0 && p.second < 1.0)) { p.second = std::min(std::max(p.second, 0.0), 0.9999999); c = true; }
    return {p, c};
}

// ---------------------------------------------------------------- растр
struct Surface {
    int w, h; std::vector<int> cells; std::map<int, int> *log = nullptr;
    Surface(int W, int H) : w(W), h(H), cells((size_t)W * H, -1) {}
    void set(int i, int ink) { cells[i] = ink; if (log) (*log)[i] = ink; }
    void put(int c, int r, int ink) { if (c >= 0 && c < w && r >= 0 && r < h) set(r * w + c, ink); }
};
int pel_anchor(double pos, double ext) { return (int)floor(std::min(pos, pos + ext)); }
int pel_span(double ext) { return std::max(1, (int)floor(fabs(ext) + 0.5)); }
struct TexPeriod { std::vector<std::pair<double, double>> on; double plen; };
const TexPeriod TEX[4] = {{{{0.0, 1.0}}, 1.0}, {{{0.0, 0.0}}, 2.0}, {{{0.0, 2.0}}, 6.0}, {{{0.0, 2.0}, {4.0, 4.0}}, 6.0}};
double normang(double a) { a = fmod(a, 2 * M_PI); return a < 0 ? a + 2 * M_PI : a; }

std::vector<Pt> arc_polyline(const std::vector<Pt> &ctrl, double tol) {
    if (ctrl.size() != 3) return ctrl;
    Pt s = ctrl[0], t = ctrl[1], e = ctrl[2];
    std::vector<Pt> out;
    if (hypot(s.first - e.first, s.second - e.second) < 1e-9) {
        double cx = (s.first + t.first) / 2, cy = (s.second + t.second) / 2, rad = hypot(s.first - t.first, s.second - t.second) / 2;
        if (rad < 1e-9) return {s};
        double a0 = atan2(s.second - cy, s.first - cx), step = sqrt(8 * tol / rad);
        int n = std::min(std::max(step > 0 ? (int)ceil(2 * M_PI / step) : 1, 3), 8192);
        for (int i = 0; i <= n; i++) out.push_back({cx + rad * cos(a0 + 2 * M_PI * i / n), cy + rad * sin(a0 + 2 * M_PI * i / n)});
        return out;
    }
    double ax = s.first, ay = s.second, bx = t.first, by = t.second, cx = e.first, cy = e.second;
    double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (fabs(d) < 1e-12) return {s, e};
    double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    double ox = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d, oy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
    double rad = hypot(ox - ax, oy - ay);
    if (rad < 1e-9) return {s, e};
    double a0 = atan2(ay - oy, ax - ox), to_t = normang(atan2(by - oy, bx - ox) - a0), sweep = normang(atan2(cy - oy, cx - ox) - a0);
    if (to_t > sweep) sweep -= 2 * M_PI;
    double step = sqrt(8 * tol / rad);
    int n = std::min(std::max(step > 0 ? (int)ceil(fabs(sweep) / step) : 1, 1), 8192);
    for (int i = 0; i <= n; i++) out.push_back({ox + rad * cos(a0 + sweep * i / n), oy + rad * sin(a0 + sweep * i / n)});
    return out;
}

const Page *g_page_for_mask = nullptr;   // заполняется на время отрисовки (маски шаблонов)

bool pattern_covers(int pattern, Pt pel, Pt mask_size, const Mask *mask, double x, double y) {
    auto band = [](double v, double pitch) { if (!(fabs(pitch) > 1e-9)) return true; return fmod(fabs(floor(v / fabs(pitch))), 2.0) < 1.0; };
    if (pattern == 1) return band(x, pel.first);
    if (pattern == 2) return band(y, pel.second);
    if (pattern == 3) return band(x, pel.first) || band(y, pel.second);
    if (!mask || !mask->defined()) return true;
    double sx = fabs(mask_size.first), sy = fabs(mask_size.second);
    if (!(sx > 1e-9 && sy > 1e-9)) return true;
    double fx = fmod(x, sx) / sx, fy = fmod(y, sy) / sy;
    if (fx < 0) fx += 1;
    if (fy < 0) fy += 1;
    if (mask_size.first < 0) fx = 1 - fx;
    if (mask_size.second < 0) fy = 1 - fy;
    int ex = std::min(std::max((int)(fx * mask->w), 0), mask->w - 1), ey = std::min(std::max((int)(fy * mask->h), 0), mask->h - 1);
    return mask->el[ey * mask->w + ex];
}

std::map<int, int> font_index() { std::map<int, int> m; for (int i = 0; i < (int)(sizeof FONT_CODES / sizeof FONT_CODES[0]); i++) m[FONT_CODES[i]] = i; return m; }
const FaceDef *face_for(int gridw, int cols, int rows) {
    int held = gridw <= 256 ? 1 : 3;
    const FaceDef *chosen = nullptr; bool cdiv = false; int cel = 0;
    for (int i = 0; i < held; i++) {
        const FaceDef &f = FACES[i];
        if (f.w > cols || f.h > rows) continue;
        bool div = rows % f.h == 0; int el = f.w * f.h;
        if (chosen) { if (cdiv && !div) continue; if (div == cdiv && el <= cel) continue; }
        chosen = &f; cdiv = div; cel = el;
    }
    return chosen ? chosen : &FACES[0];
}

struct Rasteriser {
    Surface &s; double cpu, rpu;
    struct Traced { int c0, r0, w, h; std::vector<char> cells; } *traced = nullptr;
    Rasteriser(Surface &surf, int gw, int gh, bool unit = false) : s(surf), cpu(gw), rpu(unit ? gh : gh / DISPLAY_H) {}
    Pt cp(Pt p) const { return {p.first * cpu, p.second * rpu}; }
    Pt cs(Pt z) const { return {z.first * cpu, z.second * rpu}; }
    void block(int ac, int ar, Pt pel, int ink) {
        int lc = ac + pel_span(pel.first) - 1, lr = ar + pel_span(pel.second) - 1;
        for (int r = ar; r <= lr; r++)
            for (int c = ac; c <= lc; c++) {
                if (traced) {
                    if (c >= traced->c0 && c < traced->c0 + traced->w && r >= traced->r0 && r < traced->r0 + traced->h) traced->cells[(r - traced->r0) * traced->w + (c - traced->c0)] = 1;
                    continue;
                }
                s.put(c, r, ink);
            }
    }
    void stamp_cells(Pt where, Pt pel, int ink) { block(pel_anchor(where.first, pel.first), pel_anchor(where.second, pel.second), pel, ink); }
    void stamp(Pt pt, Pt pel, int ink) { stamp_cells(cp(pt), cs(pel), ink); }
    void sweep(Pt a, Pt b, Pt pel, int ink) {
        int fc = pel_anchor(a.first, pel.first), fr = pel_anchor(a.second, pel.second), tc = pel_anchor(b.first, pel.first), tr = pel_anchor(b.second, pel.second);
        int cols = abs(tc - fc), rows = abs(tr - fr), cst = tc >= fc ? 1 : -1, rst = tr >= fr ? 1 : -1, c = fc, r = fr, err = cols - rows;
        for (int guard = 0; guard < 100000; guard++) {
            block(c, r, pel, ink);
            if (c == tc && r == tr) return;
            int e2 = 2 * err;
            if (e2 > -rows) { err -= rows; c += cst; }
            if (e2 < cols) { err += cols; r += rst; }
        }
    }
    void stroke(const std::vector<Pt> &pts, Pt pel, int tex, int ink, bool closed = false) {
        if (pts.empty()) return;
        if (pts.size() == 1) { stamp(pts[0], pel, ink); return; }
        Pt ps = cs(pel);
        const TexPeriod &T = TEX[tex & 3];
        double phase = 0;
        size_t n = closed ? pts.size() : pts.size() - 1;
        for (size_t i = 0; i < n; i++) {
            Pt a = cp(pts[i]), b = cp(pts[(i + 1) % pts.size()]);
            double run = b.first - a.first, rise = b.second - a.second, ln = hypot(run, rise), st = 0;
            if (ln > 0) {
                double ux = run / ln, uy = rise / ln; st = INFINITY;
                if (fabs(ux) > 1e-9) st = std::min(st, ps.first / fabs(ux));
                if (fabs(uy) > 1e-9) st = std::min(st, ps.second / fabs(uy));
                if (!std::isfinite(st)) st = 0;
            }
            if (tex == 0 || !(st > 0)) { sweep(a, b, ps, ink); continue; }
            auto at = [&](double k) { double t = std::min(std::max(k * st / ln, 0.0), 1.0); return Pt{a.first + run * t, a.second + rise * t}; };
            double ent = phase, ext = ent + ln / st;
            for (double p = floor(ent / T.plen); p <= floor(ext / T.plen); p++)
                for (auto &fl : T.on) {
                    double rf = std::max(p * T.plen + fl.first, ent), rl = std::min(p * T.plen + fl.second, ext);
                    if (rl >= rf) sweep(at(rf - ent), at(rl - ent), ps, ink);
                }
            stamp_cells(a, ps, ink); stamp_cells(b, ps, ink);
            phase = ext;
        }
    }
    void fill(const std::vector<Pt> &pts, Pt pel, int pattern, Pt mask_size, const Mask *mask, int ink) {
        if (pts.size() < 2) { if (!pts.empty()) stamp(pts[0], pel, ink); return; }
        Pt ps = cs(pel);
        std::vector<Pt> c; for (auto &p : pts) c.push_back(cp(p));
        double left = 1e300, right = -1e300, bottom = 1e300, top = -1e300;
        for (auto &p : c) { left = std::min(left, p.first); right = std::max(right, p.first); bottom = std::min(bottom, p.second); top = std::max(top, p.second); }
        left += std::min(0.0, ps.first) - 2; right += std::max(0.0, ps.first) + 2; bottom += std::min(0.0, ps.second) - 2; top += std::max(0.0, ps.second) + 2;
        int W = s.w, H = s.h;
        auto clc = [&](double v) { return std::min(std::max((int)floor(v), -W), 2 * W); };
        auto clr = [&](double v) { return std::min(std::max((int)floor(v), -H), 2 * H); };
        int c0 = clc(left), r0 = clr(bottom), w = clc(ceil(right)) - c0 + 1, h = clr(ceil(top)) - r0 + 1;
        if (w <= 0 || h <= 0) return;
        Traced tr{c0, r0, w, h, std::vector<char>((size_t)w * h, 0)};
        traced = &tr; stroke(pts, pel, 0, ink, true); traced = nullptr;
        std::vector<char> enclosed((size_t)w * h, 0), outside((size_t)w * h, 0), nearv((size_t)w * h, 0);
        int n = (int)c.size();
        for (int row = r0; row < r0 + h; row++) {
            double smp = row + 0.5; std::vector<double> xs;
            for (int i = 0; i < n; i++) {
                Pt f = c[i], t = c[(i + 1) % n];
                if ((f.second <= smp && smp < t.second) || (t.second <= smp && smp < f.second)) xs.push_back(f.first + (smp - f.second) / (t.second - f.second) * (t.first - f.first));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                int a = std::max(c0, (int)ceil(xs[i] - 0.5)), b = std::min(c0 + w - 1, (int)floor(xs[i + 1] - 0.5));
                for (int col = a; col <= b; col++) enclosed[(row - r0) * w + col - c0] = 1;
            }
        }
        for (int idx = 0; idx < w * h; idx++)
            if (tr.cells[idx]) {
                int y = idx / w, x = idx % w;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int yy = y + dy, xx = x + dx; if (yy >= 0 && yy < h && xx >= 0 && xx < w) nearv[yy * w + xx] = 1; }
            }
        std::vector<int> stack;
        for (int idx = 0; idx < w * h; idx++) if (!enclosed[idx] && !tr.cells[idx] && !nearv[idx]) { outside[idx] = 1; stack.push_back(idx); }
        while (!stack.empty()) {
            int idx = stack.back(); stack.pop_back();
            int y = idx / w, x = idx % w;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto &q : nb) if (q[0] >= 0 && q[0] < w && q[1] >= 0 && q[1] < h) { int j = q[1] * w + q[0]; if (!outside[j] && !tr.cells[j]) { outside[j] = 1; stack.push_back(j); } }
        }
        for (int row = std::max(r0, 0); row < std::min(r0 + h, H); row++)
            for (int col = std::max(c0, 0); col < std::min(c0 + w, W); col++) {
                if (outside[(row - r0) * w + col - c0]) continue;
                if (pattern && !pattern_covers(pattern, pel, mask_size, mask, (col + 0.5) / cpu, (row + 0.5) / rpu)) continue;
                s.set(row * W + col, ink);
            }
    }
    void fill_rect_cells(double left, double bottom, double width, double height, int ink) {
        if (width <= 0 || height <= 0) return;
        int fc = (int)floor(left), lc = std::max(fc, (int)ceil(left + width) - 1), fr = (int)floor(bottom), lr = std::max(fr, (int)ceil(bottom + height) - 1);
        for (int r = fr; r <= lr; r++) for (int c = fc; c <= lc; c++) s.put(c, r, ink);
    }
    bool field_cells(const Prim &p, int &cols, int &rows) const {
        Pt f = cs(p.size);
        if (fabs(f.first) < 1e-9 || fabs(f.second) < 1e-9) return false;
        cols = std::max(1, (int)lround(fabs(f.first))); rows = std::max(1, (int)lround(fabs(f.second)));
        return true;
    }
    void deposit_pattern(const Prim &p, const std::vector<int> &rows_bits, int pw, int ph, int ink, int bg) {
        int cols, rows; if (!field_cells(p, cols, rows)) return;
        Pt o = cp(p.origin);
        double cstep = p.size.first < 0 ? -1.0 : 1.0, rstep = p.size.second < 0 ? -1.0 : 1.0;
        for (int row = 0; row < rows; row++) {
            int sr = std::min(std::max(ph * row / rows, 0), ph - 1); int bits = rows_bits[sr];
            for (int col = 0; col < cols; col++) {
                int sc = std::min(std::max(pw * col / cols, 0), pw - 1);
                int lit = (bits >> (pw - 1 - sc)) & 1;
                int pen = p.reverse ? (lit ? bg : ink) : (lit ? ink : bg);
                if (pen < -1 || pen == -1) continue;
                int pc = col, pr = rows - 1 - row;
                if (p.rotation == 1) { int t = pc; pc = -pr + rows - 1; pr = t; }
                else if (p.rotation == 2) { pc = cols - 1 - pc; pr = rows - 1 - pr; }
                else if (p.rotation == 3) { int t = pc; pc = pr; pr = cols - 1 - t; }
                int sc_ = (int)floor(o.first + cstep * pc + (cstep < 0 ? -1.0 : 0.0)), sr_ = (int)floor(o.second + rstep * pr + (rstep < 0 ? -1.0 : 0.0));
                s.put(sc_, sr_, pen);
            }
        }
    }
    bool unit_grid() const { return fabs(rpu - s.h) < 1e-9; }
    void deposit_char(const Prim &p, const std::map<int, DrcsChar> &drcs, int ink, int bg) {
        if (p.rep == 'M') {
            int code = p.ch;
            int six = (code & 0x20 || code == 0x5F) ? ((code & 0x1F) | ((code >> 1) & 0x20)) : 0;
            Pt f = cs(p.size), o = cp(p.origin);
            double left = std::min(o.first, o.first + f.first), bottom = std::min(o.second, o.second + f.second), w = fabs(f.first), h = fabs(f.second);
            if (w < 1e-9 || h < 1e-9) return;
            Pt pel = cs(p.pel);
            double ic = p.underlined ? fabs(pel.first) : 0, ir = p.underlined ? fabs(pel.second) : 0, ew = w / 2, eh = h / 3;
            if (bg >= 0) fill_rect_cells(left, bottom, w, h, bg);
            for (int el = 0; el < 6; el++) {
                if (!(six & (1 << el))) continue;
                int col = el % 2, rft = el / 2;
                fill_rect_cells(left + col * ew, bottom + (2 - rft) * eh, std::max(0.0, ew - ic), std::max(0.0, eh - ir), (p.reverse && bg >= 0) ? bg : ink);
            }
            return;
        }
        if (p.rep == 'D') {
            auto it = drcs.find(p.ch);
            if (it == drcs.end()) return;
            const DrcsChar &g = it->second;
            int gw = std::min(g.w, 8);
            std::vector<int> rows(g.h, 0);
            for (int r = 0; r < g.h; r++) {
                int bits = 0;
                for (int c = 0; c < std::min(g.w, 8); c++) if (g.el[r * g.w + c]) bits |= 1 << (g.w - 1 - c);
                rows[g.h - 1 - r] = g.w > gw ? bits >> (g.w - gw) : bits;
            }
            deposit_pattern(p, rows, gw, g.h, ink, bg);
            return;
        }
        int u;
        if (p.rep == 'P') { u = p.ch; if (!(u >= 0x20 && u < 0x7F)) u = 0x20; }
        else u = (int)nabts_supp()[std::min(std::max(p.ch - 0x20, 0), 95)];
        int cols, rows; if (!field_cells(p, cols, rows)) return;
        const FaceDef *face = face_for(!unit_grid() ? s.w : 256, cols, rows);
        static const std::map<int, int> FI = font_index();
        auto fi = FI.find(u);
        if (fi == FI.end()) return;
        std::vector<int> pat(face->rows + fi->second * face->h, face->rows + (fi->second + 1) * face->h);
        deposit_pattern(p, pat, face->w, face->h, ink, bg);
    }
    void colour_run(Pt origin, Pt size, Pt pel, const std::vector<int> &inks) {
        if (inks.empty()) return;
        double left = std::min(origin.first, origin.first + size.first), bottom = std::min(origin.second, origin.second + size.second);
        double wu = fabs(size.first), hu = fabs(size.second), top = bottom + hu;
        double sx = fabs(pel.first) > 1e-9 ? fabs(pel.first) : 1.0 / cpu, sy = fabs(pel.second) > 1e-9 ? fabs(pel.second) : 1.0 / rpu;
        int cols = std::max(1, (int)floor(wu / sx));
        for (size_t i = 0; i < inks.size(); i++) {
            int col = (int)i % cols, row = (int)i / cols;
            double wx = left + col * sx, wy = top - (row + 1) * sy;
            if (wy + sy <= bottom) break;
            stamp({wx, wy}, {sx, sy}, inks[i]);
        }
    }
};

using PenFn = std::function<int(const Player::Ink &)>;
void draw_primitive(Rasteriser &ras, const Prim &p, const Page *page, bool definition, const std::map<int, DrcsChar> *drcs, const PenFn &pen) {
    auto fg_spec = [&]() { return p.caddr >= 0 ? Player::Ink{'m', {}, p.caddr} : Player::Ink{'d', p.colour, p.daddr}; };
    int ink = definition ? 1 : pen(fg_spec());
    int bg = (p.mode == 2 && p.baddr >= 0 && !definition) ? pen(Player::Ink{'m', {}, p.baddr}) : -1;
    switch (p.kind) {
    case Prim::POINT: if (!p.points.empty()) ras.stamp(p.points[0], p.pel, ink); break;
    case Prim::LINE: ras.stroke(p.points, p.pel, p.line_tex, ink); break;
    case Prim::ARC: case Prim::RECT: case Prim::POLY: {
        std::vector<Pt> outline;
        if (p.kind == Prim::ARC) outline = arc_polyline(p.points, 0.2 / ras.cpu);
        else if (p.kind == Prim::RECT) { Pt o = p.origin; double fx = o.first + p.size.first, fy = o.second + p.size.second; outline = {o, {fx, o.second}, {fx, fy}, {o.first, fy}}; }
        else outline = p.points;
        if (!p.filled) { ras.stroke(outline, p.pel, p.line_tex, ink, p.kind == Prim::RECT); return; }
        const Mask *mask = (p.pattern >= 4 && page) ? &page->masks[p.pattern - 4] : nullptr;
        ras.fill(outline, p.pel, definition ? 0 : p.pattern, p.mask_size, mask, ink);
        if (p.highlighted && !definition) {
            int hl = bg >= 0 ? bg : pen(Player::Ink{'c', BLACK, 0});
            ras.stroke(outline, p.pel, 0, hl, p.kind != Prim::ARC);
        }
        break;
    }
    case Prim::INCR: {
        if (definition) { ras.stroke(p.points, p.pel, p.line_tex, ink); return; }
        std::vector<int> inks;
        for (int e : p.incr) {
            if (p.mode == 0) {
                auto g = [&](int off) { return ((((e >> (3 + off)) & 1) << 1) | ((e >> off) & 1)) << 1; };
                inks.push_back(pen(Player::Ink{'c', {g(2), g(1), g(0), false}, 0}));
            } else inks.push_back(pen(Player::Ink{'m', {}, e % 16}));
        }
        ras.colour_run(p.origin, p.size, p.pel, inks);
        break;
    }
    case Prim::CHAR: {
        static const std::map<int, DrcsChar> none;
        ras.deposit_char(p, drcs ? *drcs : (page ? page->drcs : none), ink, bg);
        break;
    }
    }
}

// ---------------------------------------------------------------- интерпретатор NAPLPS
const Pt FIELD_NORMAL{1.0 / 40.0, 5.0 / 128.0};
const int ESC = 0x1B, NSR = 0x1F, CAN = 0x18, APS = 0x1C;
bool transparent_c0(int b) { return (b >= 0 && b <= 6) || (b >= 0x10 && b <= 0x17); }
double draw_cost(Prim::Kind k) {
    switch (k) { case Prim::CHAR: case Prim::POINT: return 0.004; case Prim::LINE: return 0.012; case Prim::INCR: return 0.05; default: return 0.02; }
}
enum { SET_PRIMARY, SET_SUPP, SET_PDI, SET_MOSAIC, SET_MACRO, SET_DRCS, SET_NULL };
const int STORAGE = 3072, MAX_MACRO_DEPTH = 8;

struct Esc { std::string kind; int len, slot, val; };
Esc parse_escape(const Bytes &b, size_t pos) {
    size_t i = pos; std::vector<int> inter;
    while (i < b.size() && b[i] >= 0x20 && b[i] <= 0x2F) inter.push_back(b[i++]);
    if (i >= b.size()) return {"trunc", (int)(1 + i - pos), 0, 0};
    int f = b[i];
    if (!(f >= 0x30 && f <= 0x7E)) return {"bad", (int)(1 + i - pos), 0, 0};
    int ln = (int)(1 + i - pos + 1);
    if (inter.empty()) {
        if (f == 0x6E) return {"shift", ln, 2, 0};
        if (f == 0x6F) return {"shift", ln, 3, 0};
        if (f >= 0x40 && f <= 0x5F) return {"c1", ln, 0, f};
        return {"unsup", ln, 0, 0};
    }
    if (inter.size() > 1 || inter[0] == 0x21 || inter[0] == 0x22) return {"unsup", ln, 0, 0};
    int slot; bool allow96;
    switch (inter[0]) {
    case 0x28: slot = 0; allow96 = false; break;
    case 0x29: case 0x2D: slot = 1; allow96 = true; break;
    case 0x2A: case 0x2E: slot = 2; allow96 = true; break;
    case 0x2B: case 0x2F: slot = 3; allow96 = true; break;
    default: return {"unsup", ln, 0, 0};
    }
    int s = SET_NULL; bool need96 = false;
    switch (f) {
    case 0x42: s = SET_PRIMARY; break; case 0x7C: s = SET_SUPP; break; case 0x57: s = SET_PDI; need96 = true; break;
    case 0x7D: s = SET_MOSAIC; need96 = true; break; case 0x7A: s = SET_MACRO; need96 = true; break; case 0x7B: s = SET_DRCS; need96 = true; break;
    }
    if (s == SET_NULL || (need96 && !allow96)) return {"desig", ln, slot, SET_NULL};
    return {"desig", ln, slot, s};
}
bool supp_nonspacing(int code) { return code >= 0x40 && code <= 0x4F; }

struct TextState { int rotation = 0, path = 0, ics = 0, irs = 0, move = 0; Pt field = FIELD_NORMAL; bool reverse = false, underlined = false;
    void reset() { *this = TextState(); } };

struct Interpreter {
    int gw, gh;
    std::vector<std::shared_ptr<Prim>> prims;
    int g[4]; int locked = 0, invoked = 0; bool single = false;
    int fmt_[3] = {1, 3, 0}; Pt pel{0, 0};
    TextState text; int line_tex = 0, pattern = 0; bool highlight = false; Pt mask_size = FIELD_NORMAL;
    ColourState colour;
    Pt field_origin{0, 0}, field_size{1, 1}, cursor{0, 0}, dp{0, 0};
    std::array<Blink, 16> blink{};
    std::map<int, std::pair<Bytes, bool>> macros; std::map<int, DrcsChar> drcs; std::array<Mask, 4> masks{};
    int storage = 0;
    double clock = 0; std::vector<Event> events;
    bool have_snap = false; std::array<Col, 16> snap_m; std::array<Blink, 16> snap_b;
    std::vector<std::pair<Bytes, size_t>> frames;
    std::string collecting; Bytes body; int def_code = 0; bool def_transmit = false;
    DrcsChar *drcs_target = nullptr; Mask *mask_target = nullptr;
    bool have_last_drcs = false; int last_drcs = 0x7F; int last_graphic = -1;
    size_t def_frame = 0; bool def_had_code = false; int wrap = 0;

    Interpreter(int w, int h) : gw(w), gh(h) { reset_decoder(); }
    void reset_env() { g[0] = SET_PRIMARY; g[1] = SET_PDI; g[2] = SET_SUPP; g[3] = SET_MOSAIC; locked = invoked = 0; single = false; }
    Pt home() const { return {0.0, DISPLAY_H - fabs(text.field.second)}; }
    void reset_decoder() {
        reset_env(); fmt_[0] = 1; fmt_[1] = 3; fmt_[2] = 0; pel = {0, 0};
        text.reset(); line_tex = 0; pattern = 0; highlight = false; mask_size = FIELD_NORMAL;
        colour = ColourState(); colour.reset();
        field_origin = {0, 0}; field_size = {1, 1}; cursor = home(); dp = {0, 0};
        blink = {}; macros.clear(); drcs.clear(); masks = {}; storage = 0;
    }
    void nsr_reset() {
        reset_env(); fmt_[0] = 1; fmt_[1] = 3; fmt_[2] = 0; pel = {0, 0}; text.reset();
        field_origin = {0, 0}; field_size = {1, 1}; line_tex = 0; pattern = 0; highlight = false; mask_size = FIELD_NORMAL;
        colour.reset_drawing_to_white();
    }
    void apply_caption_state() {
        nsr_reset(); cursor = {0, 0}; dp = {0, 0};
        colour.write(0, TRANSPARENT); colour.write(1, BLACK); colour.write(7, WHITE); colour.select_mapped_bg(7, 1);
    }
    std::pair<int, int> drcs_size() const {
        double cols = fabs(text.field.first) / (1.0 / gw), rows = fabs(text.field.second) / (DISPLAY_H / gh);
        return {std::min(std::max((int)lround(cols), 1), 256), std::min(std::max((int)lround(rows), 1), 256)};
    }
    void snap_state() {
        if (!have_snap || colour.map != snap_m) { snap_m = colour.map; Event e{'m', clock, nullptr, colour.map, {}}; events.push_back(e); }
        bool beq = have_snap; if (have_snap) for (int i = 0; i < 16; i++) if (!(blink[i] == snap_b[i])) beq = false;
        if (!beq) { snap_b = blink; Event e{'b', clock, nullptr, {}, blink}; events.push_back(e); }
        have_snap = true;
    }
    std::shared_ptr<Page> run(const Bytes &record, bool keep_display = false) {
        std::vector<std::shared_ptr<Prim>> carried = keep_display ? prims : std::vector<std::shared_ptr<Prim>>();
        prims = carried; clock = 0; events.clear();
        for (auto &p : carried) { p->t = 0; events.push_back({'p', 0.0, p, {}, {}}); }
        have_snap = false; snap_state();
        frames.clear(); collecting.clear(); body.clear(); def_code = 0; def_transmit = false;
        drcs_target = nullptr; mask_target = nullptr; have_last_drcs = false; last_drcs = 0x7F; last_graphic = -1;
        def_frame = 0; def_had_code = false; wrap = 0;
        Bytes rec(record.size()); for (size_t i = 0; i < record.size(); i++) rec[i] = record[i] & 0x7F;
        frames.push_back({rec, 0});
        long steps = 0;
        while (!frames.empty()) {
            if (++steps > 400000) break;
            if (!step()) { frames.pop_back(); if (!frames.empty() && def_frame >= frames.size()) def_frame = frames.size() - 1; }
        }
        end_definition(); snap_state();
        auto &fm = colour.map;
        for (auto &p : prims) {
            if (p->caddr >= 0) p->colour = fm[p->caddr % 16];
            if (p->baddr >= 0) p->background = fm[p->baddr % 16];
            if (p->blink_addr >= 0) p->blink_to = fm[p->blink_addr % 16];
        }
        auto page = std::make_shared<Page>();
        page->prims = prims; page->colour_map = fm; page->drcs = drcs; page->masks = masks; page->events = events; page->end = clock;
        return page;
    }
    bool step() {
        auto &fr = frames.back();
        if (fr.second >= fr.first.size()) return false;
        int byte = fr.first[fr.second]; fr.second++;
        bool from_def = frames.size() - 1 == def_frame;
        if (collecting == "macro" || collecting == "macrox") {
            if (from_def) {
                if (byte == ESC) {
                    Esc e = parse_escape(fr.first, fr.second);
                    if (e.kind == "c1" && e.val >= 0x40 && e.val <= 0x45) { fr.second += e.len - 1; end_definition(); exec_c1(e.val); return true; }
                }
                if (collecting == "macro") { body.push_back((u8)byte); return true; }
                size_t start = fr.second - 1;
                exec_byte(byte);
                auto &d = frames[def_frame];
                if (d.second > start) body.insert(body.end(), d.first.begin() + start, d.first.begin() + d.second);
                return true;
            }
            exec_byte(byte); return true;
        }
        if ((collecting == "drcs" || collecting == "mask") && from_def && !transparent_c0(byte)) {
            bool term = false;
            if (byte == ESC) { Esc e = parse_escape(fr.first, fr.second); term = e.kind == "c1" && e.val >= 0x40 && e.val <= 0x45; }
            if (!term) def_had_code = true;
        }
        exec_byte(byte);
        return true;
    }
    void exec_byte(int b) { if (b < 0x20) exec_c0(b); else exec_graphic(b); }
    void exec_c0(int byte) {
        if (transparent_c0(byte)) return;
        auto &fr = frames.back();
        if (byte == ESC) {
            Esc e = parse_escape(fr.first, fr.second);
            fr.second += e.len - 1;
            if (e.kind == "desig") g[e.slot] = e.val;
            else if (e.kind == "shift") { locked = invoked = e.slot; single = false; }
            else if (e.kind == "c1") exec_c1(e.val);
            return;
        }
        if (byte == 0x0F) { locked = invoked = 0; single = false; }
        else if (byte == 0x0E) { locked = invoked = 1; single = false; }
        else if (byte == 0x19) { invoked = 2; single = true; }
        else if (byte == 0x1D) { invoked = 3; single = true; }
        else if (byte == 0x08) move_by('b');
        else if (byte == 0x09) move_by('f');
        else if (byte == 0x0A) { if (wrap == 1) { wrap = 3; return; } if (wrap == 2) { wrap = 0; return; } move_by('d'); }
        else if (byte == 0x0B) move_by('u');
        else if (byte == 0x0D) { if (wrap == 1) { wrap = 2; return; } if (wrap == 3) { wrap = 0; return; } move_cursor({field_origin.first, cursor.second}); }
        else if (byte == 0x0C) { if (colour.mode == 2) clear_display(colour.background(), colour.bg_addr); else clear_display(BLACK, -1); move_cursor(home()); }
        else if (byte == 0x1E) move_cursor(home());
        else if (byte == NSR) {
            nsr_reset();
            auto &f2 = frames.back();
            if (f2.second + 1 < f2.first.size()) {
                int rb = f2.first[f2.second] & 0x7F, cb = f2.first[f2.second + 1] & 0x7F;
                if (rb >= 0x40 && cb >= 0x40) {
                    f2.second += 2;
                    double dx = fabs(text.field.first), dy = fabs(text.field.second);
                    move_cursor({(cb & 0x3F) * dx, DISPLAY_H - ((rb & 0x3F) + 1) * dy});
                    return;
                }
                if (rb >= 0x20 && rb < 0x40 && cb >= 0x20 && cb < 0x40) f2.second += 2;
            }
            move_cursor(home());
        } else if (byte == CAN) frames.resize(1);
        else if (byte == APS) {
            auto &f2 = frames.back();
            if (f2.second + 1 >= f2.first.size()) return;
            int rb = f2.first[f2.second], cb = f2.first[f2.second + 1];
            if (rb < 0x20 || cb < 0x20) return;
            f2.second += 2;
            double dx = fabs(text.field.first), dy = fabs(text.field.second);
            move_cursor({((cb & 0x7F) - 32) * dx, ((rb & 0x7F) - 32) * dy});
        }
    }
    void clear_display(const Col &c, int addr) {
        prims.clear(); snap_state();
        events.push_back({'c', clock, nullptr, {}, {}});
        if (c == BLACK && addr < 0) return;
        auto p = std::make_shared<Prim>(); p->kind = Prim::RECT; p->filled = true;
        p->origin = {0, 0}; p->size = {1.0, DISPLAY_H}; p->points = {{0, 0}, {1.0, DISPLAY_H}};
        p->mode = colour.mode; p->colour = c; p->caddr = addr; p->daddr = addr >= 0 ? addr : colour.draw_addr; p->t = clock;
        prims.push_back(p); events.push_back({'p', clock, p, {}, {}});
    }
    void exec_c1(int c) {
        if (c >= 0x40 && c <= 0x44) {
            std::string terminated = collecting;
            end_definition();
            if (c == 0x43 && terminated == "drcs" && have_last_drcs) { begin_definition("drcs", last_drcs >= 0x7F ? 0x20 : last_drcs + 1); return; }
            auto &fr = frames.back(); int code = 0;
            if (fr.second < fr.first.size()) { code = fr.first[fr.second]; if (code >= 0x20) fr.second++; else return; }
            if (c == 0x40) { def_transmit = false; begin_definition("macro", code); }
            else if (c == 0x41) { def_transmit = false; begin_definition("macrox", code); }
            else if (c == 0x42) { def_transmit = true; begin_definition("macro", code); }
            else if (c == 0x43) begin_definition("drcs", code);
            else begin_definition("mask", code);
            return;
        }
        if (c == 0x45) end_definition();
        else if (c == 0x46) {
            auto &fr = frames.back();
            if (fr.second >= fr.first.size() || last_graphic < 0) return;
            int cnt = fr.first[fr.second];
            if (cnt < 0x40) return;
            fr.second++;
            int gch = last_graphic;
            for (int i = 0; i < (cnt & 0x3F); i++) exec_graphic(gch);
        } else if (c == 0x47) {
            if (last_graphic < 0) return;
            double dx = fabs(text.field.first);
            if (dx <= 0) return;
            double limit = field_origin.first + fabs(field_size.first);
            int gch = last_graphic;
            for (int i = 0; i < (int)(1.0 / dx) + 1; i++) { if (cursor.first + dx > limit) break; exec_graphic(gch); }
        } else if (c == 0x48) text.reverse = true;
        else if (c == 0x49) text.reverse = false;
        else if (c == 0x4A) text.field = {1.0 / 80.0, 5.0 / 128.0};
        else if (c == 0x4B) text.field = {1.0 / 32.0, 3.0 / 64.0};
        else if (c == 0x4C) text.field = FIELD_NORMAL;
        else if (c == 0x4D) text.field = {1.0 / 40.0, 5.0 / 64.0};
        else if (c == 0x4F) text.field = {1.0 / 20.0, 5.0 / 64.0};
        else if (c == 0x4E) {
            Blink b; b.set = true; b.on = 5; b.off = 5; b.delay = 0; b.start = clock;
            if (colour.mode == 2) { b.to = colour.bg_addr; b.has_col = false; } else { b.to = -1; b.has_col = true; b.col = BLACK; }
            blink[colour.draw_addr] = b;
        } else if (c == 0x5E) blink[colour.draw_addr] = Blink();
        else if (c == 0x59) text.underlined = true;
        else if (c == 0x5A) text.underlined = false;
    }
    void exec_graphic(int byte) {
        int s = g[invoked];
        if (single) { invoked = locked; single = false; }
        if (s == SET_PDI) { if (byte >= 0x20 && byte <= 0x3F) exec_pdi(byte); return; }
        if (s == SET_MACRO) { invoke_macro(byte); return; }
        if (s == SET_NULL) return;
        last_graphic = byte;
        auto p = make(Prim::CHAR);
        p->ch = byte;
        p->rep = s == SET_PRIMARY ? 'P' : s == SET_SUPP ? 'S' : s == SET_MOSAIC ? 'M' : 'D';
        p->origin = cursor; p->points = {cursor}; p->size = text.field; p->rotation = text.rotation; p->path = text.path;
        p->reverse = text.reverse; p->underlined = text.underlined;
        emit(p);
        if (s != SET_SUPP || !supp_nonspacing(byte)) move_by('f');
    }
    Bytes gather() {
        auto &fr = frames.back(); Bytes ops;
        while (fr.second < fr.first.size()) {
            int x = fr.first[fr.second];
            if (x >= 0x40 && x <= 0x7F) { ops.push_back((u8)x); fr.second++; continue; }
            if (x < 0x20 && transparent_c0(x)) { fr.second++; continue; }
            break;
        }
        return ops;
    }
    void exec_pdi(int op) {
        Bytes ops = gather();
        Operands r(ops, fmt_);
        if (op == 0x20) pdi_reset(r);
        else if (op == 0x21) pdi_domain(r);
        else if (op == 0x22) pdi_text(r);
        else if (op == 0x23) pdi_texture(r);
        else if (op == 0x3C) pdi_set_colour(r);
        else if (op == 0x3E) pdi_select_colour(r);
        else if (op == 0x3F) pdi_blink(r);
        else if (op == 0x3D) {
            // WAIT: длительность в десятых долях секунды; у пакета из нескольких байтов первый — не время
            // (в записях ExtraVision это всегда 0x5C, а «40» и «4A» одиночными идут как 0 и 1 с)
            snap_state(); int s = 0;
            for (size_t i = ops.size() >= 2 ? 1 : 0; i < ops.size(); i++) s += ops[i] & 0x3F;
            clock += 0.1 * s;
        }
        else if (op <= 0x27) pdi_point(op, r);
        else if (op <= 0x2B) pdi_line(op, r);
        else if (op <= 0x2F) pdi_arc(op, r);
        else if (op <= 0x33) pdi_rect(op, r);
        else if (op <= 0x37) pdi_poly(op, r);
        else if (op == 0x38) pdi_field(r);
        else pdi_incremental(op, r, ops);
    }
    void pdi_reset(Operands &r) {
        int b1 = r.empty() ? 0 : r.fixed(), b2 = r.empty() ? 0 : r.fixed();
        auto bit = [](int v, int i) { return (v >> (i - 1)) & 1; };
        if (bit(b1, 1)) { fmt_[0] = 1; fmt_[1] = 3; fmt_[2] = 0; pel = {0, 0}; }
        int ca = bit(b1, 3) * 2 + bit(b1, 2);
        if (ca == 1) { colour.mode = 0; colour.reset_map(); colour.set_colour(WHITE); }
        else if (ca == 2) colour.reset_to_mapped(colour.mode == 0);
        else if (ca == 3) colour.reset_to_mapped(true);
        int sa = bit(b1, 6) * 4 + bit(b1, 5) * 2 + bit(b1, 4);
        if (sa == 1 || sa == 7) clear_display(BLACK, -1);
        else if (sa == 2 || sa == 5 || sa == 6) clear_display(colour.drawing(), colour.mode == 0 ? -1 : colour.draw_addr);
        if (bit(b2, 1)) { text.reset(); field_origin = {0, 0}; field_size = {1, 1}; move_cursor(home()); }
        if (bit(b2, 2)) blink = {};
        if (bit(b2, 4)) { line_tex = 0; pattern = 0; highlight = false; mask_size = FIELD_NORMAL; }
        if (bit(b2, 5)) { for (auto &kv : macros) storage -= std::min(storage, (int)kv.second.first.size()); macros.clear(); }
        if (bit(b2, 6)) { storage -= std::min(storage, 11 * (int)drcs.size()); drcs.clear(); }
    }
    void pdi_domain(Operands &r) {
        if (r.empty()) return;
        int b1 = r.fixed();
        fmt_[0] = (b1 & 3) + 1; fmt_[1] = ((b1 >> 2) & 7) + 1; fmt_[2] = (b1 >> 5) & 1;
        if (r.empty()) return;
        for (int i = 0; i < 3; i++) r.fmt_[i] = fmt_[i];
        pel = r.coord();
    }
    void pdi_text(Operands &r) {
        if (r.empty()) return;
        int b1 = r.fixed();
        text.rotation = b1 & 3; text.path = (b1 >> 2) & 3; text.ics = (b1 >> 4) & 3;
        if (!r.empty()) { int b2 = r.fixed(); text.irs = b2 & 3; text.move = (b2 >> 2) & 3; }
        if (!r.empty()) text.field = r.coord();
    }
    void pdi_texture(Operands &r) {
        if (r.empty()) return;
        int b1 = r.fixed();
        line_tex = b1 & 3; highlight = (b1 >> 2) & 1; pattern = (b1 >> 3) & 7;
        if (!r.empty()) mask_size = r.coord();
    }
    void pdi_set_colour(Operands &r) {
        if (r.empty()) { colour.set_transparent(); return; }
        bool first = true; int addr = 0;
        while (!r.empty()) {
            Col c = r.colour();
            if (first) { colour.set_colour(c); addr = colour.draw_addr; first = false; continue; }
            if (colour.mode == 0) { colour.set_colour(c); continue; }
            addr = increment_addr(addr);
            if (addr < 0) break;
            colour.write(addr, c);
        }
    }
    void pdi_select_colour(Operands &r) {
        int nb = fmt_[0], words = nb ? r.remaining() / nb : 0;
        if (words == 0) { colour.mode = 0; return; }
        int a = addr_from_operand((int)r.single(), nb);
        if (words == 1) { colour.select_mapped(a); return; }
        int b = addr_from_operand((int)r.single(), nb);
        colour.select_mapped_bg(a, b);
    }
    void pdi_blink(Operands &r) {
        if (r.empty()) { blink[colour.draw_addr] = Blink(); return; }
        int frm = colour.draw_addr;
        for (;;) {
            int to = addr_from_operand((int)r.single(), fmt_[0]);
            int on = r.empty() ? 0 : r.fixed(), off = r.empty() ? 0 : r.fixed(), delay = r.empty() ? 0 : r.fixed();
            Blink b;
            if (on && off) { b.set = true; b.to = to % 16; b.has_col = false; b.on = on; b.off = off; b.delay = delay; b.start = clock; }
            blink[frm % 16] = b;
            if (r.empty()) return;
            frm = increment_addr(frm);
            if (frm < 0) return;
        }
    }
    Pt resolve(Pt p) const { return clamp_unit(p).first; }
    void pdi_point(int op, Operands &r) {
        bool rel = op == 0x25 || op == 0x27, vis = op == 0x26 || op == 0x27;
        while (!r.empty()) {
            Pt w = r.coord();
            Pt t = resolve(rel ? Pt{dp.first + w.first, dp.second + w.second} : w);
            move_dp(t);
            if (vis) { auto p = make(Prim::POINT); p->origin = t; p->points = {t}; emit(p); }
            if (r.truncated) return;
        }
    }
    void pdi_line(int op, Operands &r) {
        bool rel = op == 0x29 || op == 0x2B, has_start = op == 0x2A || op == 0x2B;
        while (!r.empty()) {
            Pt s = dp;
            if (has_start) { s = resolve(r.coord()); if (r.empty()) return; }
            Pt w = r.coord();
            Pt e = rel ? resolve({s.first + w.first, s.second + w.second}) : resolve(w);
            auto p = make(Prim::LINE); p->origin = s; p->points = {s, e}; emit(p);
            move_dp(e);
            if (r.truncated) return;
        }
    }
    void pdi_arc(int op, Operands &r) {
        bool filled = op == 0x2D || op == 0x2F, has_start = op == 0x2E || op == 0x2F;
        Pt s = dp;
        if (has_start) { if (r.empty()) return; s = resolve(r.coord()); }
        if (r.empty()) return;
        auto p = make(Prim::ARC); p->filled = filled; p->origin = s; p->points = {s};
        Pt prev = s;
        while (!r.empty() && p->points.size() < 256) {
            Pt w = r.coord(); prev = resolve({prev.first + w.first, prev.second + w.second}); p->points.push_back(prev);
            if (r.truncated) break;
        }
        if (p->points.size() < 2) return;
        if (p->points.size() == 2) p->points.push_back(s);
        Pt e = p->points.back();
        emit(p); move_dp(e);
    }
    void pdi_rect(int op, Operands &r) {
        bool filled = op == 0x31 || op == 0x33, has_start = op == 0x32 || op == 0x33;
        while (!r.empty()) {
            Pt s = dp;
            if (has_start) { s = resolve(r.coord()); if (r.empty()) return; }
            Pt ext = r.coord();
            Pt corner = resolve({s.first + ext.first, s.second + ext.second});
            auto p = make(Prim::RECT); p->filled = filled; p->origin = s; p->size = {corner.first - s.first, corner.second - s.second}; p->points = {s, corner};
            emit(p);
            move_dp(resolve({s.first + ext.first, s.second}));
            if (r.truncated) return;
        }
    }
    void pdi_poly(int op, Operands &r) {
        bool filled = op == 0x35 || op == 0x37, has_start = op == 0x36 || op == 0x37;
        Pt s = dp;
        if (has_start) { if (r.empty()) return; s = resolve(r.coord()); }
        auto p = make(Prim::POLY); p->filled = filled; p->origin = s; p->points = {s};
        Pt prev = s;
        while (!r.empty() && p->points.size() < 256) {
            Pt w = r.coord();
            if (w.first == 0.0 && w.second == 0.0) continue;
            prev = resolve({prev.first + w.first, prev.second + w.second}); p->points.push_back(prev);
            if (r.truncated) break;
        }
        if (p->points.size() < 3) return;
        emit(p); move_dp(s);
    }
    void pdi_field(Operands &r) {
        if (r.empty()) { field_origin = {0, 0}; field_size = {1, 1}; move_dp(field_origin); return; }
        Pt first = r.coord();
        if (r.empty()) { field_origin = dp; field_size = first; return; }
        Pt ext = r.coord();
        field_origin = resolve(first); field_size = ext; move_dp(field_origin);
    }
    void pdi_incremental(int op, Operands &r, const Bytes &ops) {
        if (op == 0x39) {
            auto p = make(Prim::INCR); p->origin = field_origin; p->size = field_size; p->points = {field_origin};
            for (u8 b : ops) p->incr.push_back(b & 0x3F);
            emit(p); return;
        }
        if (r.empty()) return;
        Pt inc = r.coord();
        auto p = make(Prim::POLY); p->filled = op == 0x3B; p->origin = dp; p->points = {dp};
        Pt cur = dp;
        while (!r.empty() && p->points.size() < 256) {
            int d = r.fixed() & 7;
            double dx = (d >= 1 && d <= 3) ? inc.first : (d >= 5 && d <= 7) ? -inc.first : 0.0;
            double dy = (d >= 3 && d <= 5) ? inc.second : (d == 7 || d == 0 || d == 1) ? -inc.second : 0.0;
            cur = resolve({cur.first + dx, cur.second + dy}); p->points.push_back(cur);
        }
        if (p->points.size() >= 2) emit(p);
        move_dp(cur);
    }
    std::shared_ptr<Prim> make(Prim::Kind k) {
        auto p = std::make_shared<Prim>(); p->kind = k;
        p->pel = pel; p->line_tex = line_tex; p->pattern = pattern; p->mask_size = mask_size; p->highlighted = highlight;
        p->mode = colour.mode; p->colour = colour.drawing(); p->background = colour.background();
        if (colour.mode != 0) p->caddr = colour.draw_addr;
        if (colour.mode == 2) p->baddr = colour.bg_addr;
        p->daddr = colour.draw_addr;
        const Blink &bl = blink[colour.draw_addr];
        if (bl.set) { p->blinking = true; p->blink_addr = bl.to; p->blink_to = bl.to >= 0 ? colour.map[bl.to] : bl.col; }
        return p;
    }
    void emit(std::shared_ptr<Prim> p) {
        if (collecting == "drcs" || collecting == "mask") { draw_into_definition(*p); return; }
        snap_state();
        p->t = clock; prims.push_back(p); events.push_back({'p', clock, p, {}, {}});
        clock += draw_cost(p->kind) * (p->filled ? 2.0 : 1.0);
    }
    bool stays_on_row(Pt pt) const {
        bool horiz = text.path == 0 || text.path == 1;
        double across = horiz ? pt.second - cursor.second : pt.first - cursor.first, row = fabs(horiz ? text.field.second : text.field.first);
        return fabs(across) < std::max(row, 1e-9) * 0.5;
    }
    void move_dp(Pt pt) { dp = pt; if (text.move == 0 || text.move == 2) { if (!stays_on_row(pt)) wrap = 0; cursor = pt; } }
    void move_cursor(Pt pt) { wrap = 0; cursor = resolve(pt); if (text.move == 0 || text.move == 1) dp = cursor; }
    void move_by(char mv) {
        bool horiz = text.path == 0 || text.path == 1;
        static const double ICS[4] = {1.0, 1.25, 1.5, 1.0}, IRS[4] = {1.0, 1.25, 1.5, 2.0};
        double chard = fabs(horiz ? text.field.first : text.field.second) * ICS[text.ics & 3], rowd = fabs(horiz ? text.field.second : text.field.first) * IRS[text.irs & 3];
        double dist = (mv == 'f' || mv == 'b') ? chard : rowd;
        static const double PX[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        double px = PX[text.path][0], py = PX[text.path][1], sx, sy;
        if (mv == 'f') { sx = px; sy = py; } else if (mv == 'b') { sx = -px; sy = -py; } else if (mv == 'd') { sx = py; sy = -px; } else { sx = -py; sy = px; }
        double nx = cursor.first + sx * dist, ny = cursor.second + sy * dist, right = field_origin.first + fabs(field_size.first);
        bool wrapped = mv == 'f' && text.path == 0 && nx + fabs(text.field.first) > right + 1e-9;
        if (wrapped) { nx = field_origin.first; ny -= rowd; }
        move_cursor({nx, ny});
        if (wrapped) wrap = 1;
    }
    void begin_definition(const std::string &what, int code) {
        collecting = what; body.clear(); def_code = code; drcs_target = nullptr; mask_target = nullptr;
        def_frame = frames.empty() ? 0 : frames.size() - 1; def_had_code = false;
        if (what == "drcs") {
            auto wh = drcs_size();
            if (code >= 0x20 && code <= 0x7F) {
                DrcsChar d{code, wh.first, wh.second, std::vector<char>((size_t)wh.first * wh.second, 0)};
                if (!drcs.count(code)) { if (storage + 11 <= STORAGE) { storage += 11; drcs[code] = d; } }
                else drcs[code] = d;
                auto it = drcs.find(code); drcs_target = it == drcs.end() ? nullptr : &it->second;
            }
            last_drcs = code; have_last_drcs = true;
            return;
        }
        if (what == "mask") {
            if (!(code >= 0x41 && code <= 0x44)) { collecting.clear(); return; }
            masks[code - 0x41] = Mask{16, 16, std::vector<char>(256, 0)};
            mask_target = &masks[code - 0x41];
        }
    }
    void end_definition() {
        std::string c = collecting;
        if (c.empty()) return;
        if (c == "macro" || c == "macrox") {
            if (def_code >= 0x20 && def_code <= 0x7F) {
                auto it = macros.find(def_code);
                if (it != macros.end()) { storage -= std::min(storage, (int)it->second.first.size()); macros.erase(it); }
                if (!body.empty() && storage + (int)body.size() <= STORAGE) { storage += (int)body.size(); macros[def_code] = {body, def_transmit}; }
            }
        } else {
            if (c == "drcs" && !def_had_code && drcs_target) { drcs.erase(def_code); storage -= std::min(storage, 11); }
            dp = {0, 0};
        }
        collecting.clear(); body.clear(); drcs_target = nullptr; mask_target = nullptr; def_transmit = false;
    }
    void draw_into_definition(const Prim &p) {
        int w = 0, h = 0; std::vector<char> *el = nullptr;
        if (drcs_target) { w = drcs_target->w; h = drcs_target->h; el = &drcs_target->el; }
        else if (mask_target) { w = mask_target->w; h = mask_target->h; el = &mask_target->el; }
        if (!el || w == 0 || h == 0) return;
        bool black = p.colour.g == 0 && p.colour.r == 0 && p.colour.b == 0 && !p.colour.t;
        Surface surf(w, h);
        Rasteriser ras(surf, w, h, true);
        draw_primitive(ras, p, nullptr, true, &drcs, [](const Player::Ink &) { return 1; });
        for (size_t i = 0; i < surf.cells.size(); i++) if (surf.cells[i] >= 0) (*el)[i] = !black;
    }
    void invoke_macro(int code) {
        if (collecting == "macrox" && code == def_code) return;
        auto it = macros.find(code);
        if (it == macros.end() || it->second.second) return;
        if ((int)frames.size() > MAX_MACRO_DEPTH) return;
        frames.push_back({it->second.first, 0});
    }
};

std::string page_text(const Page &page) {
    struct C { double y, x, h, w; std::string t; long order; };
    std::vector<std::pair<std::pair<long long, long long>, C>> cells;   // порядок вставки
    long order = 0; std::string marks;
    auto find = [&](std::pair<long long, long long> k) { for (size_t i = 0; i < cells.size(); i++) if (cells[i].first == k) return (int)i; return -1; };
    for (auto &pp : page.prims) {
        const Prim &p = *pp;
        if (p.kind == Prim::RECT && p.filled) {
            double x0 = p.origin.first, y0 = p.origin.second, x1 = x0 + p.size.first, y1 = y0 + p.size.second;
            double xl = std::min(x0, x1), xh = std::max(x0, x1), yl = std::min(y0, y1), yh = std::max(y0, y1);
            cells.erase(std::remove_if(cells.begin(), cells.end(), [&](auto &kv) { auto &v = kv.second; double cx = v.x + v.w / 2, cy = v.y + v.h / 2; return xl <= cx && cx <= xh && yl <= cy && cy <= yh; }), cells.end());
            continue;
        }
        if (p.kind != Prim::CHAR || p.rep == 'M' || p.rep == 'D') continue;
        if (p.rep == 'S' && supp_nonspacing(p.ch)) { marks += from_cp(nabts_supp()[p.ch - 0x20]); continue; }
        std::string ch = (p.rep == 'P' && p.ch >= 0x20 && p.ch < 0x7F) ? std::string(1, (char)p.ch) : p.rep == 'S' ? from_cp(nabts_supp()[std::min(std::max(p.ch - 0x20, 0), 95)]) : " ";
        double x = p.origin.first, y = p.origin.second, w = fabs(p.size.first), h = fabs(p.size.second);
        std::string t = ch + marks; marks.clear();
        if (!strip(t).empty()) {
            bool shadow = false;
            size_t from = cells.size() > 120 ? cells.size() - 120 : 0;
            for (size_t i = from; i < cells.size(); i++) { auto &v = cells[i].second; if (v.t == t && fabs(v.x - x) < w * 0.34 && fabs(v.y - y) < h * 0.34) { shadow = true; break; } }
            if (shadow) continue;
        }
        std::pair<long long, long long> key{llround(x * 1024), llround(y * 1024)};
        int fi = find(key); if (fi >= 0) cells.erase(cells.begin() + fi);
        cells.push_back({key, {y, x, h, w, t, ++order}});
    }
    std::vector<C> placed; for (auto &kv : cells) placed.push_back(kv.second);
    std::stable_sort(placed.begin(), placed.end(), [](const C &a, const C &b) {
        double ya = round(a.y * 1e6) / 1e6, yb = round(b.y * 1e6) / 1e6;
        if (ya != yb) return ya > yb; return a.x < b.x; });
    if (placed.empty()) return "";
    std::string out; double ly = placed[0].y, lh = placed[0].h;
    for (size_t i = 0; i < placed.size(); i++) {
        auto &v = placed[i];
        if (i && fabs(v.y - ly) > std::max(std::max(lh, v.h) / 2.0, 1e-6)) { out += '\n'; ly = v.y; lh = v.h; }
        out += v.t;
    }
    std::string res; for (auto &l : split(out, '\n')) res += (res.empty() && &l == &l ? "" : "") + rstrip(l) + "\n";
    if (!res.empty()) res.pop_back();
    return res;
}
}  // namespace

// ================================================================ публичное
std::string address_text(long long a) {
    if (((a >> 20) & 0xFFFF) == 0 && (a & 0xFF) == 0) return fmt("%03llX", (a >> 8) & 0xFFF);
    return fmt("%09llX", a);
}
std::string record_label(const Record &r) { return fmt("%03X/%s v%d", r.channel, r.addr_text.c_str(), r.version); }
std::string record_name(const Record &r) { return fmt("%03X-%s-v%d", r.channel, r.addr_text.c_str(), r.version); }
std::string flags_text(const Record &r) {
    static const std::pair<const char *, const char *> names[] = {{"caption", "captions"}, {"cyclic", "cyclic"}, {"priority", "priority"}, {"alarm", "alarm"},
        {"update", "update"}, {"support_record", "support"}, {"support_needed", "needs support"}, {"index", "index"}, {"more", "more"}};
    std::string s;
    for (auto &n : names) { auto it = r.flags.find(n.first); if (it != r.flags.end() && it->second) s += (s.empty() ? "" : ", ") + std::string(n.second); }
    return s;
}

std::vector<Record> read_t33(const std::string &path, Summary &summ, Progress *pr) {
    Bytes raw = read_file(path);
    long n = (long)(raw.size() / PACKET);
    Catalogue cat; std::vector<Msg> msgs;
    RecordAssembler ra; ra.cb = [&](Msg &m) { msgs.push_back(m); };
    GroupAssembler ga; ga.cb = [&](GroupOut &g) { ra.add(g); };
    for (long i = 0; i < n; i++) {
        ga.add(decode_packet(&raw[i * PACKET]));
        if (!msgs.empty()) { for (auto &m : msgs) cat.merge(m); msgs.clear(); }
        if (pr && i % 20000 == 0) pr->progress(i, n, "parsing the stream");
    }
    ga.flush(); ra.flush();
    for (auto &m : msgs) cat.merge(m);
    auto fd = cat.reconcile();
    auto recs = cat.records();
    summ.packets = n; summ.groups = ga.stats; summ.foreign = ra.foreign; summ.folded = fd.first; summ.dropped = fd.second;
    return recs;
}

void interpret(std::vector<Record> &records, int gw, int gh) {
    Interpreter it(gw, gh);
    std::map<int, Record *> support;
    std::map<std::pair<int, long long>, Record *> latest;
    auto pres = [](int t) { return t == 0 || t == 1 || t == 3; };
    for (auto &r : records) if (pres(r.type)) { if (r.flags["support_record"]) support[r.channel] = &r; latest[{r.channel, r.address}] = &r; }
    std::map<std::pair<int, long long>, Record *> pred;
    for (auto &kv : latest) {
        Record *r = kv.second; long long succ;
        if (r->has_more) succ = r->more_address;
        else { if (!r->flags["more"]) continue; if (!algorithmic_more(r->address, succ)) continue; }
        if (succ == r->address) continue;
        pred[{kv.first.first, succ}] = r;
    }
    for (auto &r : records) {
        r.page.reset(); r.chain_base = r.address; r.chain_pos = 0; r.text.clear();
        if (!pres(r.type) || r.data.empty()) continue;
        std::vector<Record *> prefix; std::set<long long> seen{r.address}; long long addr = r.address; bool ring = false;
        while (pred.count({r.channel, addr})) {
            Record *pr = pred[{r.channel, addr}];
            if (seen.count(pr->address)) { ring = true; break; }
            seen.insert(pr->address); prefix.insert(prefix.begin(), pr); addr = pr->address;
        }
        if (ring) {
            long long base = r.address; size_t bi = 0;
            for (size_t i = 0; i < prefix.size(); i++) if (prefix[i]->address < base) { base = prefix[i]->address; bi = i; }
            if (base == r.address) prefix.clear(); else prefix.erase(prefix.begin(), prefix.begin() + bi);
            addr = base;
        }
        r.chain_base = addr; r.chain_pos = (int)prefix.size();
        bool needs = r.flags["support_needed"], cap = r.flags["caption"];
        for (auto *m : prefix) { needs |= m->flags["support_needed"]; cap |= m->flags["caption"]; }
        it.reset_decoder();
        if (needs && !r.flags["support_record"] && support.count(r.channel)) it.run(support[r.channel]->data);
        if (cap) it.apply_caption_state();
        bool keep = false;
        for (auto *m : prefix) { it.run(m->data, keep); keep = true; }
        r.page = it.run(r.data, keep);
        r.text = page_text(*r.page);
    }
}

// ---------------------------------------------------------------- плеер
uint32_t rgb(const Col &c) {
    if (c.t) return 0;
    auto l = [](int v) { return std::min(v, 7) * 255 / 7; };
    return (l(c.r) << 16) | (l(c.g) << 8) | l(c.b);
}

Player::Player(const Page &page, int w, int h) : gw(w), gh(h), page_(page) {
    map = DEFAULT_MAP; blink = {};
    inks.push_back({'c', BLACK, 0}); ink_id[inks[0]] = 0;
    for (auto &e : page.events) if (e.type == 'b') for (auto &b : e.blink) if (b.set) has_blink_ = true;
    cells.assign((size_t)gw * gh, -1);
}
int Player::pen(const Ink &s) {
    auto it = ink_id.find(s);
    if (it != ink_id.end()) return it->second;
    int k = (int)inks.size(); inks.push_back(s); ink_id[s] = k; return k;
}
bool Player::advance(double T, bool all) {
    bool changed = false;
    Surface surf(gw, gh); surf.cells.swap(cells); surf.log = logging ? &log : nullptr;
    Rasteriser ras(surf, gw, gh);
    PenFn pf = [this](const Ink &s) { return pen(s); };
    while (i_ < ev().size() && (all || ev()[i_].t <= T)) {
        const Event &e = ev()[i_++]; changed = true;
        if (e.type == 'p') draw_primitive(ras, *e.p, &page_, false, nullptr, pf);
        else if (e.type == 'c') { std::fill(surf.cells.begin(), surf.cells.end(), -1); surf_id++; if (logging) log.clear(); }
        else if (e.type == 'm') { map = e.map; map_ver++; }
        else if (e.type == 'b') { blink = e.blink; blink_ver++; }
    }
    cells.swap(surf.cells);
    return changed;
}
Col Player::colour(const Ink &s, double T) const {
    if (s.k == 'c') return s.c;
    int a; Col col;
    if (s.k == 'm') { a = ((s.a % 16) + 16) % 16; col = map[a]; } else { a = ((s.a % 16) + 16) % 16; col = s.c; }
    const Blink &b = blink[a];
    if (b.set && T >= 0 && b.on && b.off) {
        double ph = T - b.start - 0.1 * b.delay;
        if (ph >= 0 && fmod(ph, 0.1 * (b.on + b.off)) >= 0.1 * b.on) col = b.to >= 0 ? map[b.to % 16] : b.col;
    }
    return col;
}
std::vector<Col> Player::blink_key(double T) const { std::vector<Col> v; for (auto &s : inks) v.push_back(colour(s, T)); return v; }
Image Player::image(double T, bool) const {
    std::vector<uint32_t> lut; for (auto &s : inks) lut.push_back(rgb(colour(s, T)));
    Image im(gw, gh);
    for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) { int c = cells[(size_t)(gh - 1 - y) * gw + x]; im.px[(size_t)y * gw + x] = lut[c < 0 ? 0 : c]; }
    return im;
}
Image render_page(const Page &page, int gw, int gh) { Player pl(page, gw, gh); pl.advance(0, true); return pl.image(-1); }
}  // namespace nabts

void nabts_dump(const std::string &src, const std::string &out, Progress &pr) {
    make_dirs(out);
    nabts::Summary s;
    auto recs = nabts::read_t33(src, s, &pr);
    pr.log(fmt("Packets %ld, groups %ld, records in catalogue %zu", s.packets, s.groups["complete"], recs.size()));
    nabts::interpret(recs);
    std::string txt;
    for (auto &r : recs) {
        txt += fmt("=== %s  type %d  received %d  ", nabts::record_label(r).c_str(), r.type, r.seen) + nabts::flags_text(r) + " " + r.purpose + "\n";
        if (r.page) { png_save(path_join(out, nabts::record_name(r) + ".png"), nabts::render_page(*r.page)); txt += r.text + "\n"; }
        txt += "\n";
    }
    if (!txt.empty()) txt.pop_back();
    write_text(path_join(out, "records.txt"), txt);
    pr.log("Done: " + out);
}
