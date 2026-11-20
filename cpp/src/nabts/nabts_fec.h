// NABTS datacast with forward error correction (RFC 2728 §3.2-3.3 and Appendix A, EIA-516 bundles): every data
// packet carries 26 data bytes and a 2-byte row code, a bundle of N data rows is closed (or led) by two check rows,
// each column of a bundle is a codeword of the same code. Over GF(256), modulus 0x11D, alpha = 0x1D:
//   s0 = sum v_k alpha^k, s1 = sum v_k alpha^3k; a codeword has s0 = s1 = 0, which corrects one byte or fills two
//   erasures. A row is a codeword with its two suffix bytes first.
// The algorithm (rows corrected on their own, then the bundle by its columns; the layout - data rows to a bundle,
// which check rows are sent, before or after their data - learnt from the stream; failing rows rebuilt by the row
// and column codes together).
//
// Header only and without std includes of its own: it is included both by src/nabts/wavetop.cpp and by the old
// engine's copy (src/legacy/src/wavetop.cpp, compiled inside namespace legacy). The includer brings <array>, <map>,
// <string>, <vector>, <algorithm> and the Hamming tables ham::dec / ham::fix.
#ifndef TR_NABTS_FEC_H
#define TR_NABTS_FEC_H

namespace nabtsfec {

constexpr int kRowBytes = 28;    // 26 data + 2 row code bytes, as the packet carries them after its 5-byte prefix
constexpr int kDataBytes = 26;
constexpr int kMaxPositions = 34; // check rows + data rows of the largest bundle believed (32 data rows)

struct Field {
    int exp[512], log[256];
    Field() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = -1;
    }
};
inline const Field &field() { static const Field f; return f; }
inline int mul(int a, int b) { const Field &F = field(); return a && b ? F.exp[(F.log[a] + F.log[b]) % 255] : 0; }
inline int div(int a, int b) { const Field &F = field(); return a && b ? F.exp[(F.log[a] + 255 - F.log[b]) % 255] : 0; }
inline int alpha(int k) { return field().exp[(8 * k) % 255]; }   // alpha = 0x1D = x^8

// s0, s1 of a codeword of n bytes (position k weighs alpha^k and alpha^3k)
inline void sums(const int *v, int n, int &s0, int &s1)
{
    const Field &F = field();
    s0 = s1 = 0;
    for (int k = 0; k < n; k++) {
        if (!v[k]) continue;
        int l = F.log[v[k]];
        s0 ^= F.exp[(l + 8 * k) % 255];
        s1 ^= F.exp[(l + 24 * k) % 255];
    }
}
// one wrong byte put right (Appendix A 12.4.3: byte = 1/2 log_alpha(s1/s0)); false when the sums say more than one
inline bool correctOne(int *v, int n)
{
    int s0, s1;
    sums(v, n, s0, s1);
    if (!s0 && !s1) return true;
    if (!s0 || !s1) return false;
    const Field &F = field();
    int ratio = (F.log[s1] + 255 - F.log[s0]) % 255, k = ratio * 16 % 255;   // alpha^2 = x^16, 16 is its own inverse mod 255
    if (k >= n) return false;
    v[k] ^= div(s0, alpha(k));
    return true;
}
// positions a and b (a == b: one) filled from the sums (Appendix A 12.4.4, Cramer's rule); false when they cannot be
inline bool fill(int *v, int n, int a, int b)
{
    v[a] = v[b] = 0;
    int s0, s1;
    sums(v, n, s0, s1);
    if (a == b) { v[a] = div(s0, alpha(a)); return mul(v[a], alpha(3 * a)) == s1; }
    int x1 = alpha(a), x2 = alpha(b), y1 = alpha(3 * a), y2 = alpha(3 * b);
    int det = mul(x1, y2) ^ mul(x2, y1);
    if (!det) return false;
    v[a] = div(mul(s0, y2) ^ mul(s1, x2), det);
    v[b] = div(mul(x1, s1) ^ mul(y1, s0), det);
    return true;
}
// a row as sent (data 0..25, code 26..27) in codeword order (code first) and back
inline void toCodeword(const uint8_t *row, int *cw) { cw[0] = row[26]; cw[1] = row[27]; for (int i = 0; i < 26; i++) cw[2 + i] = row[i]; }
inline void fromCodeword(const int *cw, uint8_t *row) { row[26] = (uint8_t)cw[0]; row[27] = (uint8_t)cw[1]; for (int i = 0; i < 26; i++) row[i] = (uint8_t)cw[2 + i]; }
inline int cwPos(int i) { return i < 26 ? i + 2 : i - 26; }   // byte i of a row as sent -> its place in the codeword
inline bool rowHolds(const uint8_t *row) { int cw[28], s0, s1; toCodeword(row, cw); sums(cw, 28, s0, s1); return !s0 && !s1; }

using Row = std::array<uint8_t, kRowBytes>;

// one packet of a channel as it came
struct InRow {
    Row bytes{};
    Row received{};        // as it came: a row two bytes wrong can be "put right" into another codeword by its own code
    int step = 1;          // continuity index steps from the packet before (1 = none lost)
    bool check = false;    // PS b8 b6 = 1 1: a check row of a bundle
    bool filler = false;   // PS b4: the data block ends in filler (0x15, 0xEA ...)
    bool good = false;     // its row code holds (as received or after one byte put right)
    bool corrected = false;
    bool prefixFixed = false;   // the address or index needed the Hamming correction
    int field = 0;
    size_t packet = 0;     // index in the .t33
};

// what became of a data row of a bundle. Corrected: one byte put right by its own code and the bundle's columns
// agree; Unverified: the same, but the bundle had too many rows lost to check it (a row two bytes wrong is turned into
// another codeword by its own code one time in nine or so)
enum RowState { Received = 0, Corrected = 1, Rebuilt = 2, Unverified = 3, Damaged = 4, Missing = 5 };

struct Bundle {
    int positions = 0;                 // 2 check + data rows
    std::vector<Row> rows;             // by codeword position: 0, 1 the check rows, 2.. the data rows in order
    std::vector<int> state;            // RowState of each position
    std::vector<bool> filler;          // the data row's PS said filler (unknown for a rebuilt row: false)
    int field = 0;                     // the field its first row came in
    bool whole() const { for (int q = 2; q < positions; q++) if (state[q] > Rebuilt) return false; return true; }
    // every data row there, some perhaps unchecked: for a record that has a check of its own (WaveTop's CRC-16)
    bool usable() const { for (int q = 2; q < positions; q++) if (state[q] > Unverified) return false; return true; }
};

struct Channel {
    int address = 0;
    int dataRows = 0, checkRows = 0;           // rows that came
    int rowsClean = 0, rowsCorrected = 0;      // row code held as received / after one byte
    int rowsRebuilt = 0, rowsJointly = 0;      // data rows the column code rebuilt (with the row codes together)
    int rowsUnverified = 0;                    // put right by their own code in bundles the columns could not check
    int rowsDamaged = 0, rowsMissing = 0;      // data rows nothing put right: as received / lost
    int indicesMisread = 0;
    int layoutData = 0;                        // data rows to a bundle (0: no bundles seen)
    std::string layoutChecks;                  // the two check positions: 'C' sent, 'M' never sent
    bool checksLead = false;                   // check rows sent before their data rows
    std::vector<Bundle> bundles;
    std::string summary() const;
};

// the commonest run of data rows a check row closes, and how the stream goes from the last of such a run to the next
// data row ("CC", "CM", "MC")
template <class R> inline void learnLayout(const R &rows, int &data, std::string &checks)
{
    data = 0; checks.clear();
    std::map<int, long> runs;
    int run = 0;
    for (auto &r : rows) {
        if (!r.check) { if (run > 0 && r.step != 1) run = 0; run++; continue; }
        if (run > 0) runs[run]++;
        run = 0;
    }
    long best = 0;
    for (auto &kv : runs) if (kv.second > best && kv.first <= kMaxPositions - 2) { best = kv.second; data = kv.first; }
    if (!data) return;
    std::map<std::string, long> tails;
    run = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].check) { run = 0; continue; }
        run = rows[i].step == 1 ? run + 1 : 1;
        if (run != data) continue;
        std::string tail;
        size_t nx = i + 1;
        while (nx < rows.size() && rows[nx].check && tail.size() < 4) { tail.append(rows[nx].step - 1, 'M'); tail += 'C'; nx++; }
        if (nx < rows.size() && !rows[nx].check && !tail.empty()) { tail.append(rows[nx].step - 1, 'M'); tails[tail]++; }
    }
    best = 0;
    std::string t;
    for (auto &kv : tails) if (kv.second > best) { best = kv.second; t = kv.first; }
    if (t.size() == 2 && t.find('C') != std::string::npos) checks = t;
}

// cycle and offset of every row (data rows first, then the check rows, as sent from a data run's first row),
// anchored again at every check row and at a data row where a check row was due
inline void placeRows(const std::vector<InRow> &rows, int data, const std::string &checks, std::vector<std::pair<long, int>> &at)
{
    const int period = data + 2;
    auto sent = [&](int o) { return o >= data && checks[o - data] == 'C'; };
    at.assign(rows.size(), {0, 0});
    long cycle = 0;
    int cur = -1;
    for (size_t i = 0; i < rows.size(); i++) {
        int off = 0;
        if (cur >= 0) { int moved = cur + rows[i].step; cycle += moved / period; off = moved % period; }
        if (rows[i].check && !sent(off)) {
            int best = period, bd = period;
            for (int o = data; o < period; o++) { int d = (o + period - off) % period; if (sent(o) && d < bd) { best = o; bd = d; } }
            if (cur >= 0 && best < off) cycle++;
            off = best;
        } else if (!rows[i].check && off >= data) { off = 0; cycle++; }
        cur = off;
        at[i] = {cycle, off};
    }
}

// rows with the bytes |er| marks filled by the column codes and the rows' own codes in turn (two erasures each);
// false where some stay erased or a code does not hold once all are filled
inline bool fillErasures(std::vector<Row> &rows, std::vector<std::array<bool, kRowBytes>> er)
{
    const int P = (int)rows.size();
    int col[kMaxPositions], cw[kRowBytes];
    for (bool progress = true; progress;) {
        progress = false;
        for (int j = 0; j < kRowBytes; j++) {
            int at[3], na = 0;
            for (int q = 0; q < P && na <= 2; q++) if (er[q][j]) at[na++] = q;
            if (!na || na > 2) continue;
            for (int q = 0; q < P; q++) col[q] = rows[q][j];
            if (!fill(col, P, at[0], at[na - 1])) return false;
            for (int k = 0; k < na; k++) { rows[at[k]][j] = (uint8_t)col[at[k]]; er[at[k]][j] = false; }
            progress = true;
        }
        for (int q = 0; q < P; q++) {
            int at[3], na = 0;
            for (int i = 0; i < kRowBytes && na <= 2; i++) if (er[q][i]) at[na++] = cwPos(i);
            if (!na || na > 2) continue;
            toCodeword(rows[q].data(), cw);
            if (!fill(cw, kRowBytes, std::min(at[0], at[na - 1]), std::max(at[0], at[na - 1]))) return false;
            fromCodeword(cw, rows[q].data());
            er[q].fill(false);
            progress = true;
        }
    }
    for (int q = 0; q < P; q++) {
        for (int i = 0; i < kRowBytes; i++) if (er[q][i]) return false;
        if (!rowHolds(rows[q].data())) return false;
    }
    for (int j = 0; j < kRowBytes; j++) {
        int s0, s1;
        for (int q = 0; q < P; q++) col[q] = rows[q][j];
        sums(col, P, s0, s1);
        if (s0 || s1) return false;
    }
    return true;
}

// a bundle with two or more rows failing their row code and at most one lost: the columns that do not hold are shared
// out among the failing rows (each at least two), every sharing tried; taken only when one reading alone fills it
inline bool repairJointly(std::vector<Row> &rows, const std::vector<bool> &arrived, const std::vector<bool> &good)
{
    const int P = (int)rows.size();
    std::vector<int> missing, failing;
    for (int q = 0; q < P; q++) { if (!arrived[q]) missing.push_back(q); else if (!good[q]) failing.push_back(q); }
    if (failing.size() < 2 || missing.size() > 1) return false;
    std::vector<Row> recv = rows;
    std::vector<std::array<bool, kRowBytes>> shared(P);
    for (auto &s : shared) s.fill(false);
    for (int q : missing) { recv[q].fill(0); shared[q].fill(true); }
    std::vector<int> flagged;
    int col[kMaxPositions];
    for (int j = 0; j < kRowBytes; j++) {
        for (int q = 0; q < P; q++) col[q] = recv[q][j];
        bool holds;
        if (missing.empty()) { int s0, s1; sums(col, P, s0, s1); holds = !s0 && !s1; }
        else holds = fill(col, P, missing[0], missing[0]);
        if (!holds) flagged.push_back(j);
    }
    if (flagged.size() < 2 * failing.size()) return false;
    const long kMaxReadings = 4096;
    long readings = 1;
    for (size_t j = 0; j < flagged.size(); j++) { readings *= (long)failing.size(); if (readings > kMaxReadings) return false; }
    bool found = false, ambiguous = false;
    std::vector<Row> best;
    for (long r = 0; r < readings && !ambiguous; r++) {
        std::vector<std::vector<int>> wrong(failing.size());
        long d = r;
        for (int j : flagged) { wrong[d % failing.size()].push_back(j); d /= (long)failing.size(); }
        bool ok = true;
        for (auto &w : wrong) ok &= w.size() >= 2;
        if (!ok) continue;
        auto er = shared;
        for (size_t k = 0; k < failing.size(); k++) for (int j : wrong[k]) er[failing[k]][j] = true;
        std::vector<Row> t = recv;
        if (!fillErasures(t, er)) continue;
        if (found && t != best) ambiguous = true;
        best = t; found = true;
    }
    if (!found || ambiguous) return false;
    rows = best;
    return true;
}

// a channel's rows into bundles, each put right as far as its codes allow
inline void decodeChannel(Channel &ch, std::vector<InRow> &rows)
{
    for (auto &r : rows) {
        (r.check ? ch.checkRows : ch.dataRows)++;
        r.received = r.bytes;
        int cw[kRowBytes];
        toCodeword(r.bytes.data(), cw);
        int s0, s1;
        sums(cw, kRowBytes, s0, s1);
        if (!s0 && !s1) { r.good = true; ch.rowsClean++; continue; }
        if (correctOne(cw, kRowBytes)) { fromCodeword(cw, r.bytes.data()); r.good = r.corrected = true; ch.rowsCorrected++; }
    }
    learnLayout(rows, ch.layoutData, ch.layoutChecks);
    if (!ch.layoutData || ch.layoutChecks.empty()) return;
    const int data = ch.layoutData, P = data + 2;
    std::vector<std::pair<long, int>> at;
    placeRows(rows, data, ch.layoutChecks, at);
    auto bundlesFor = [&](bool lead) {
        std::map<long, std::vector<int>> b;
        for (size_t i = 0; i < rows.size(); i++) {
            bool chk = at[i].second >= data;
            long id = chk && lead ? at[i].first + 1 : at[i].first;
            int pos = chk ? at[i].second - data : 2 + at[i].second;
            auto &s = b[id];
            if (s.empty()) s.assign(P, -1);
            if (s[pos] < 0) s[pos] = (int)i;
        }
        return b;
    };
    auto verified = [&](const std::map<long, std::vector<int>> &B) {
        long v = 0;
        int col[kMaxPositions];
        for (auto &kv : B) {
            int ne = 0, e = 0;
            for (int q = 0; q < P; q++) if (kv.second[q] < 0 || !rows[kv.second[q]].good) { ne++; e = q; }
            if (ne > 1) continue;
            for (int j = 0; j < kRowBytes; j++) {
                for (int q = 0; q < P; q++) col[q] = kv.second[q] >= 0 && rows[kv.second[q]].good ? rows[kv.second[q]].bytes[j] : 0;
                bool h;
                if (!ne) { int s0, s1; sums(col, P, s0, s1); h = !s0 && !s1; } else h = fill(col, P, e, e);
                v += h;
            }
        }
        return v;
    };
    auto close = bundlesFor(false), lead = bundlesFor(true);
    ch.checksLead = verified(lead) > verified(close);
    auto &B = ch.checksLead ? lead : close;
    int col[kMaxPositions];
    // the rows |er| filled from the columns (0-2 of them) and every column and filled row checked; false when they do not hold
    auto byColumns = [&](std::vector<Row> &t, const std::vector<int> &er) {
        if (er.size() > 2) return false;
        for (int j = 0; j < kRowBytes; j++) {
            for (int q = 0; q < P; q++) col[q] = t[q][j];
            if (er.empty()) { int s0, s1; sums(col, P, s0, s1); if (s0 || s1) return false; continue; }
            if (!fill(col, P, er[0], er.back())) return false;
            for (int q : er) t[q][j] = (uint8_t)col[q];
        }
        for (int q : er) if (!rowHolds(t[q].data())) return false;   // the rebuilt rows' own codes confirm the reading
        return true;
    };
    for (auto &kv : B) {
        Bundle bu;
        bu.positions = P;
        bu.rows.assign(P, Row{});
        bu.state.assign(P, Missing);
        bu.filler.assign(P, false);
        bu.field = 0x7fffffff;
        std::vector<bool> arrived(P), good(P);
        std::vector<int> er, corr;
        std::vector<Row> asReceived(P, Row{});
        for (int q = 0; q < P; q++) {
            int s = kv.second[q];
            arrived[q] = s >= 0;
            good[q] = s >= 0 && rows[s].good;
            if (!good[q]) er.push_back(q);
            if (s >= 0) {
                bu.rows[q] = rows[s].bytes;
                asReceived[q] = rows[s].received;
                bu.state[q] = rows[s].good ? (rows[s].corrected ? Corrected : Received) : Damaged;
                if (rows[s].corrected) corr.push_back(q);
                bu.filler[q] = rows[s].filler;
                bu.field = std::min(bu.field, rows[s].field);
            }
        }
        // 1) the failing and lost rows filled by the columns; 2) the rows their own code changed taken as wrong too (one
        // of them may have been two bytes wrong and changed into another codeword); 3) the failing rows, those included as
        // they came, rebuilt by the row and column codes together
        std::vector<int> rebuilt;
        bool repaired = false, jointly = false;
        {
            std::vector<Row> t = bu.rows;
            if (byColumns(t, er)) { bu.rows = t; repaired = true; rebuilt = er; }
        }
        if (!repaired && !corr.empty()) {
            std::vector<int> er2 = er;
            for (int q : corr) er2.push_back(q);
            std::sort(er2.begin(), er2.end());
            std::vector<Row> t = bu.rows;
            if (byColumns(t, er2)) { bu.rows = t; repaired = true; rebuilt = er2; }
        }
        if (!repaired && er.size() + corr.size() >= 2) {
            std::vector<Row> t = bu.rows;
            std::vector<bool> g = good;
            for (int q : corr) { t[q] = asReceived[q]; g[q] = false; }
            if (repairJointly(t, arrived, g)) {
                bu.rows = t; repaired = jointly = true;
                for (int q = 0; q < P; q++) if (!g[q]) rebuilt.push_back(q);
            }
        }
        // two rows filled from the columns spend both their sums: the other rows' own corrections are then not checked, and
        // a row its code turned into another codeword carries that codeword's difference into the filled rows, where their
        // own codes hold all the same (the code is linear) - so those are not checked either
        const bool columnsChecked = repaired && (jointly || rebuilt.size() < 2);
        bool unchecked = false;
        if (!columnsChecked)
            for (int q = 2; q < P; q++) unchecked |= bu.state[q] == Corrected && std::find(rebuilt.begin(), rebuilt.end(), q) == rebuilt.end();
        for (int q = 2; q < P; q++) {
            bool re = std::find(rebuilt.begin(), rebuilt.end(), q) != rebuilt.end();
            if (repaired) {
                if (re && !good[q] && unchecked) { bu.state[q] = Unverified; ch.rowsUnverified++; }
                else if (re && !good[q]) { bu.state[q] = Rebuilt; ch.rowsRebuilt++; ch.rowsJointly += jointly; }
                else if (re) bu.state[q] = Corrected;   // filled from the columns and its own code holds
                else if (bu.state[q] == Corrected && !columnsChecked) { bu.state[q] = Unverified; ch.rowsUnverified++; }
                continue;
            }
            if (bu.state[q] == Corrected) { bu.state[q] = Unverified; ch.rowsUnverified++; }
            else if (!good[q]) { if (arrived[q]) ch.rowsDamaged++; else ch.rowsMissing++; }
        }
        ch.bundles.push_back(std::move(bu));
    }
}

inline std::string Channel::summary() const
{
    char s[512];
    snprintf(s, sizeof s, "address %03X: %d data rows, %d check rows; row code held %d, put right %d; bundles %zu of %d data rows "
                          "(check rows %s, %s); rebuilt %d (%d with the row codes), unchecked %d, left damaged %d, lost %d",
             address, dataRows, checkRows, rowsClean, rowsCorrected, bundles.size(), layoutData, layoutChecks.c_str(),
             checksLead ? "before their data" : "after their data", rowsRebuilt, rowsJointly, rowsUnverified, rowsDamaged, rowsMissing);
    return s;
}

// every channel of a .t33 (33 bytes a packet: 3 address nibbles, CI, PS in Hamming 8/4, then 28 bytes) that sends
// data rows with a row code; minRows / minCleanPercent: what a channel needs to be taken for an FEC datacast
inline std::vector<Channel> decode(const uint8_t *t33, size_t n, const std::vector<int> &field, int minRows = 30, int minCleanPercent = 30)
{
    std::map<int, std::vector<InRow>> held;
    std::map<int, int> lastCi, misread;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = t33 + i * 33;
        int d[5];
        bool fixed = false, bad = false;
        for (int k = 0; k < 5; k++) {
            d[k] = ham::dec[p[k]];
            if (d[k] < 0) { d[k] = ham::fix[p[k]]; fixed = true; }
            if (d[k] < 0) bad = true;
        }
        if (bad) continue;
        int ps = d[4];
        bool data = (ps >> 2) == 2, check = (ps >> 2) == 3;   // b8 b6 = 1 0: data block + 2-byte code; 1 1: a check row
        if (!data && !check) continue;
        InRow r;
        for (int k = 0; k < kRowBytes; k++) r.bytes[k] = p[5 + k];
        bool clean = rowHolds(r.bytes.data());
        if (fixed && !clean) continue;   // an address put right into a neighbouring one files a row under another channel
        int addr = d[0] << 8 | d[1] << 4 | d[2];
        auto &H = held[addr];
        if (H.size() > 2000000) continue;
        r.step = 1;
        auto lc = lastCi.find(addr);
        if (lc != lastCi.end()) { int s = (d[3] - lc->second + 16) % 16; r.step = s ? s : 16; }
        lastCi[addr] = d[3];
        r.prefixFixed = fixed;
        // a row whose index needed correcting and fits neither neighbour, while they are two apart: it is the one between
        if (!H.empty()) {
            InRow &b = H.back();
            if (b.prefixFixed && b.step != 1 && r.step != 1 && (b.step + r.step) % 16 == 2) { b.step = r.step = 1; misread[addr]++; }
        }
        r.check = check;
        r.filler = ps & 2;
        r.field = i < field.size() ? field[i] : (int)i;
        r.packet = i;
        H.push_back(r);
    }
    std::vector<Channel> out;
    for (auto &kv : held) {
        int dataRows = 0, clean = 0;
        for (auto &r : kv.second) if (!r.check) { dataRows++; clean += rowHolds(r.bytes.data()); }
        if (dataRows < minRows || clean * 100 < dataRows * minCleanPercent) continue;
        Channel ch;
        ch.address = kv.first;
        ch.indicesMisread = misread[kv.first];
        decodeChannel(ch, kv.second);
        out.push_back(std::move(ch));
    }
    return out;
}

}  // namespace nabtsfec

#endif
