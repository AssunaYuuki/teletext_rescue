#include "starsight.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// ---------------------------------------------------------------- time ---

static const int64_t kEpochDays = 8035; // 1992-01-01 in days since 1970-01-01

static void civilFromDays(int64_t z, int& y, int& m, int& d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}

static int64_t daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void ssCivil(SsTime t, int& y, int& mo, int& d, int& h, int& mi, int& wday)
{
    int64_t days = (t >= 0 ? t / 1440 : (t - 1439) / 1440);
    int64_t rem = t - days * 1440;
    civilFromDays(days + kEpochDays, y, mo, d);
    h = (int)(rem / 60);
    mi = (int)(rem % 60);
    wday = (int)(((days + kEpochDays) % 7 + 7 + 4) % 7); // 0 = Sunday
}

std::string ssIso(SsTime t)
{
    int y, mo, d, h, mi, wd;
    ssCivil(t, y, mo, d, h, mi, wd);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:00Z", y, mo, d, h, mi);
    return b;
}

bool ssParseIso(const std::string& s, SsTime& t, int* sec)
{
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return false;
    t = (daysFromCivil(y, mo, d) - kEpochDays) * 1440 + h * 60 + mi;
    if (sec) *sec = se;
    return true;
}

// ----------------------------------------------------------------- CRC ---

static uint32_t gCrcTab[256];
static uint32_t gSyn[2048][8];                   // syndrome of bit b followed by d zero bytes
static std::unordered_map<uint32_t, int> gSynMap; // syndrome -> d*8+b, -1 if ambiguous
static uint16_t gHdrSyn[9][8];

static void crcInit()
{
    static bool done = false;
    if (done) return;
    done = true;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        gCrcTab[i] = c;
    }
    for (int b = 0; b < 8; b++) {
        uint32_t c = gCrcTab[(1u << b) & 0xFF];
        for (int d = 0; d < 2048; d++) {
            gSyn[d][b] = c;
            c = gCrcTab[c & 0xFF] ^ (c >> 8);
        }
    }
    gSynMap.reserve(2048 * 8 * 2);
    for (int d = 0; d < 2048; d++)
        for (int b = 0; b < 8; b++) {
            auto it = gSynMap.find(gSyn[d][b]);
            if (it == gSynMap.end()) gSynMap[gSyn[d][b]] = d * 8 + b;
            else it->second = -1;
        }
    for (int d = 0; d < 9; d++)
        for (int b = 0; b < 8; b++) gHdrSyn[d][b] = (uint16_t)gSyn[d][b];
}

uint32_t ssCrc(const uint8_t* d, size_t n, uint32_t c)
{
    crcInit();
    for (size_t i = 0; i < n; i++) c = gCrcTab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static int popcount32(uint32_t v) { return __builtin_popcount(v); }

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int be16(const uint8_t* p) { return p[0] << 8 | p[1]; }

// ------------------------------------------------------- packet repair ---

enum class Fix { None, Ok, Single, Chase, Fail };

// Syndrome of single bit flips at byte i (of m covered bytes), bit b.
static inline uint32_t synAt(int m, int i, int b) { return gSyn[m - 1 - i][b]; }

static bool lookupSingle(uint32_t syn, int m, int& byte, int& bit)
{
    auto it = gSynMap.find(syn);
    if (it == gSynMap.end() || it->second < 0) return false;
    int d = it->second >> 3;
    byte = m - 1 - d;
    bit = it->second & 7;
    return byte >= 0;
}

static Fix verifyPacket(std::vector<uint8_t>& p, const std::vector<float>& conf)
{
    const int L = (int)p.size(), m = L - 4;
    if (m <= 11 || m > 2048) return Fix::Fail;
    uint32_t syn = ssCrc(p.data(), m) ^ le32(&p[m]);
    if (syn == 0) return Fix::Ok;
    if (popcount32(syn) == 1) return Fix::Single; // the error is in the stored CRC
    int by, bi;
    if (lookupSingle(syn, m, by, bi)) {
        p[by] ^= (uint8_t)(1 << bi);
        return Fix::Single;
    }
    // Chase decoding: flip combinations of the least reliable bits, then
    // let the syndrome table repair one more arbitrary bit.
    const int K = 16;
    std::vector<int> idx;
    idx.reserve(m * 8);
    for (int i = 1; i < m * 8; i++) idx.push_back(i);
    int k = std::min<int>(K, (int)idx.size());
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](int a, int b) { return conf[a] < conf[b]; });
    uint32_t s[K];
    for (int a = 0; a < k; a++) s[a] = synAt(m, idx[a] >> 3, idx[a] & 7);
    auto apply = [&](std::initializer_list<int> set, uint32_t rest) {
        for (int a : set) p[idx[a] >> 3] ^= (uint8_t)(1 << (idx[a] & 7));
        if (rest && popcount32(rest) != 1 && lookupSingle(rest, m, by, bi)) p[by] ^= (uint8_t)(1 << bi);
    };
    auto good = [&](uint32_t r) {
        return r == 0 || popcount32(r) == 1 || lookupSingle(r, m, by, bi);
    };
    for (int a = 0; a < k; a++)
        if (good(syn ^ s[a])) { apply({a}, syn ^ s[a]); return Fix::Chase; }
    for (int a = 0; a < k; a++)
        for (int b = a + 1; b < k; b++)
            if (good(syn ^ s[a] ^ s[b])) { apply({a, b}, syn ^ s[a] ^ s[b]); return Fix::Chase; }
    for (int a = 0; a < k; a++)
        for (int b = a + 1; b < k; b++)
            for (int c = b + 1; c < k; c++)
                if (good(syn ^ s[a] ^ s[b] ^ s[c])) {
                    apply({a, b, c}, syn ^ s[a] ^ s[b] ^ s[c]);
                    return Fix::Chase;
                }
    return Fix::Fail;
}

int ssCountGoodPackets(const std::vector<uint8_t>& b)
{
    crcInit();
    int good = 0, n = (int)b.size();
    for (int i = 0; i + 15 < n;) {
        if (b[i] == 0x2C && (uint16_t)ssCrc(&b[i], 9) == (b[i + 9] | b[i + 10] << 8)) {
            int size = be16(&b[i + 1]);
            if (size >= 16 && size <= 2048 && i + size <= n) {
                int m = size - 4, by, bi;
                uint32_t syn = ssCrc(&b[i], m) ^ le32(&b[i + m]);
                if (syn == 0 || popcount32(syn) == 1 || lookupSingle(syn, m, by, bi)) {
                    good++;
                    i += size;
                    continue;
                }
            }
        }
        i++;
    }
    return good;
}

// --------------------------------------------------------- commands ---

static bool twoByteLength(int type)
{
    return type == 3 || type == 5 || type == 11 || type == 12 || type == 21 || type == 22 || type == 23 ||
           type == 24 || type == 29 || type >= 30;
}

static std::string hexdump(const uint8_t* p, size_t n, size_t max = 48)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < max; i++) {
        snprintf(b, sizeof b, "%02x ", p[i]);
        s += b;
    }
    if (n > max) s += "...";
    return s;
}

static std::string asciiz(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    return s;
}

static const char* kMpaa[] = {"", "G", "NR", "PG", "PG-13", "R", "X", "NC-17"};
static const char* kTraits[] = {"profanity", "nudity", "violence", "adult situations",
                                "adult themes", "", "", "adult language"};

std::string SsChannel::callLetters() const
{
    if (!nameFlag || !abbrevBits) {
        if (name.size() <= 5) return name;
        size_t dash = name.find('-');
        if (dash != std::string::npos && dash >= 3 && dash <= 5) return name.substr(0, dash); // KTVU-FOX
        std::string s;
        for (char c : name)
            if (isalnum((unsigned char)c) && s.size() < 4) s += (char)toupper((unsigned char)c);
        return s;
    }
    std::string s;
    for (int i = 0; i < 8 && i < (int)name.size(); i++)
        if (abbrevBits & (0x80 >> i)) s += name[i];
    return s;
}

// Parses the commands of one packet into db. Returns false on a structural
// error (lengths that do not add up), which marks a damaged packet.
static bool parseCommands(const std::vector<uint8_t>& p, SsDatabase& db, bool verified, int pos)
{
    const int end = (int)p.size() - 4;
    int j = 11;
    SsStats& st = db.stats;
    while (j < end) {
        const uint8_t* c = &p[j];
        int type = c[0] & 0x3F;
        bool enc = c[0] & 0x80;
        int len = twoByteLength(type) ? (j + 2 < end ? be16(c + 1) : 0) : (j + 1 < end ? c[1] : 0);
        if (len < 2 || j + len > end) return false;
        j += len;
        st.commands[type]++;
        if (enc) { st.encrypted++; st.encryptedByType[type]++; continue; }
        char line[256];
        switch (type) {
        case 1: { // Time
            if (len < 8) return false;
            SsClock k;
            k.time = be32(c + 2);
            k.dst = c[6] & 0x80;
            int hrs = c[6] & 0x0F;
            k.utcOffsetMinutes = (c[6] & 0x10 ? -1 : 1) * hrs * 60;
            k.seconds = c[7];
            if (len >= 12) st.timeKeys.insert(be32(c + 8)); // only where lists are encrypted (WHYY 1995)
            if (verified) db.clock.push_back(k);
            break;
        }
        case 2: // Daylight saving change
            if (len < 10) return false;
            if (verified) db.dstChanges.push_back({(SsTime)be32(c + 2), (SsTime)be32(c + 6)});
            break;
        case 4: { // Channel data (layout partly inferred from the patent)
            if (len < 9) return false;
            SsChannel ch;
            ch.id = be16(c + 3);
            ch.nameFlag = c[5] & 0x80;
            ch.native = ((c[2] & 0x80) ? 256 : 0) | c[6];
            ch.abbrevBits = c[7];
            ch.name = asciiz(c + 8, std::min(8, len - 8));
            ch.verified = verified;
            auto& slot = db.channels[ch.id];
            if (slot.id == 0 || (!slot.verified && verified)) { slot = ch; st.channelsAdded++; }
            snprintf(line, sizeof line, "channel %d '%s' raw %s", ch.id, ch.name.c_str(),
                     hexdump(c, len, 24).c_str());
            db.log.push_back(line);
            break;
        }
        case 5: { // Show list
            if (len < 11) return false;
            SsShowList sl;
            sl.version = c[3];
            sl.channel = be16(c + 4);
            sl.start = be32(c + 6);
            sl.verified = verified;
            int n = c[10], k = 11;
            SsTime t = sl.start;
            for (int e = 0; e < n; e++) {
                if (k + 4 > len) return false;
                SsSlot s;
                s.flags = c[k];
                s.duration = c[k + 1];
                s.program = be16(c + k + 2);
                s.start = t;
                k += 4;
                if (s.flags & 0x80) {
                    if (k + 2 > len) return false;
                    s.description = be16(c + k);
                    k += 2;
                }
                if (s.duration == 0) return false;
                t += s.duration;
                sl.slots.push_back(s);
            }
            if (k != len) return false;
            auto key = std::make_pair(sl.channel, sl.start);
            auto it = db.showLists.find(key);
            if (it == db.showLists.end()) {
                db.showLists[key] = std::move(sl);
                st.listsAdded++;
            } else if (it->second.version == sl.version && (it->second.verified || !verified)) {
                st.listsUnchanged++;
            } else if (verified || !it->second.verified) {
                it->second = std::move(sl);
                st.listsReplaced++;
            }
            break;
        }
        case 6: { // Show title
            if (len < 7) return false;
            SsProgram pr;
            pr.compressed = c[2] & 0x80;
            pr.cc = c[2] & 0x40;
            pr.stereo = c[2] & 0x20;
            pr.bw = c[2] & 0x10;
            pr.id = ((c[2] & 0x0F) << 16) | be16(c + 3);
            pr.theme = be16(c + 5);
            pr.raw.assign(c + 7, c + len);
            if (!pr.compressed) pr.title = asciiz(c + 7, len - 7);
            else {
                bool complete = false;
                pr.title = ssHuffmanDecode(c + 7, len - 7, &complete);
                pr.partial = !complete;
            }
            auto& slot = db.programs[pr.id];
            if (slot.id == 0 || (slot.title.empty() && !pr.title.empty()) || (slot.partial && !pr.partial)) {
                if (slot.id == 0) st.titlesAdded++;
                slot = pr;
            }
            break;
        }
        case 8: { // Show description
            // 08 len flags DID(2) theme(2) [critic|MPAA, traits, year] text
            // flags: 80 compressed, 40 CC, 20 stereo, 10 B&W, 08 ratings present
            if (len < 8) return false;
            SsDescription ds;
            int f = c[2];
            ds.compressed = f & 0x80;
            ds.cc = f & 0x40;
            ds.stereo = f & 0x20;
            ds.bw = f & 0x10;
            ds.id = be16(c + 3);
            ds.theme = be16(c + 5);
            int k = 7;
            if ((f & 0x08) && len >= 10) {
                ds.critic = (c[7] >> 5) & 7;
                int mp = (c[7] >> 1) & 0x0F;
                if (mp > 0 && mp < 8) { ds.ratingSystem = "MPAA"; ds.rating = kMpaa[mp]; }
                for (int bit = 0; bit < 8; bit++)
                    if ((c[8] >> bit) & 1 && kTraits[bit][0]) ds.advisories.push_back(kTraits[bit]);
                if (c[9]) ds.year = 1900 + c[9];
                k = 10;
            }
            ds.raw.assign(c + k, c + len);
            if (!ds.compressed) ds.text = asciiz(c + k, len - k);
            else {
                bool complete = false;
                ds.text = ssHuffmanDecode(c + k, len - k, &complete);
                ds.partial = !complete;
            }
            auto& slot = db.descriptions[ds.id];
            if (slot.id == 0 || (slot.text.empty() && !ds.text.empty()) || (slot.partial && !ds.partial)) {
                if (slot.id == 0) st.descriptionsAdded++;
                slot = ds;
            }
            break;
        }
        case 11:
        case 12: { // Theme categories / sub-categories: id, attributes, NUL-terminated name
            int n = len > 4 ? c[4] : 0, k = 5;
            for (int e = 0; e < n && k + 2 < len; e++) {
                SsTheme th;
                th.id = c[k];
                k += 2;
                while (k < len && c[k]) th.name += (char)c[k++];
                k++;
                if (verified && th.id) db.themes[th.id] = th;
            }
            break;
        }
        case 20: // Sequence number
            if (len < 6) return false;
            if (verified) db.sequenceNumbers.push_back(be32(c + 2));
            break;
        default:
            if (db.log.size() < 4000) {
                const char* name = ssCommandName(type);
                snprintf(line, sizeof line, "@%d cmd %d (%s) len %d: %s", pos, type, name[0] ? name : "reserved/unknown", len,
                         hexdump(c, len).c_str());
                db.log.push_back(line);
            }
            break;
        }
    }
    return j == end;
}

// --------------------------------------------------------------- decode ---

struct Candidate {
    int pos;
    std::vector<uint8_t> p;
    std::vector<float> conf;
    bool done = false;
};

void ssDecodeStream(const SoftStream& s, SsDatabase& db, bool keepUnverified)
{
    crcInit();
    const auto& b = s.bytes;
    const int n = (int)b.size();
    db.stats.streamBytes += n;
    std::vector<Candidate> failed;

    auto handle = [&](Candidate& cd, Fix fx) {
        SsDatabase tmp;
        bool okStruct = parseCommands(cd.p, tmp, true, cd.pos);
        if (!okStruct) return false;
        if (getenv("TR_SSDUMP")) {
            for (uint8_t x : cd.p) fprintf(stderr, "%02x", x);
            fprintf(stderr, "\n");
        }
        // reparse straight into db now that the structure checked out
        SsStats keep = db.stats;
        parseCommands(cd.p, db, true, cd.pos);
        (void)keep;
        db.stats.crcOk += (fx == Fix::Ok);
        db.stats.fixedSingle += (fx == Fix::Single);
        db.stats.fixedChase += (fx == Fix::Chase);
        return true;
    };

    int i = 0;
    while (i + 15 < n) {
        if (b[i] != 0x2C) { i++; continue; }
        uint8_t h[11];
        memcpy(h, &b[i], 11);
        uint16_t syn = (uint16_t)ssCrc(h, 9) ^ (uint16_t)(h[9] | h[10] << 8);
        bool hdrOk = syn == 0, hdrFixed = false;
        if (!hdrOk && popcount32(syn) == 1) hdrOk = hdrFixed = true;
        if (!hdrOk) {
            for (int d = 0; d < 8 && !hdrOk; d++) // never flips byte 0, the sync
                for (int bit = 0; bit < 8; bit++)
                    if (gHdrSyn[d][bit] == syn) {
                        h[8 - d] ^= (uint8_t)(1 << bit);
                        hdrOk = hdrFixed = true;
                        break;
                    }
        }
        int size = be16(h + 1);
        if (!hdrOk || size < 16 || size > 2048 || i + size > n) { i++; continue; }
        Candidate cd;
        cd.pos = i;
        cd.p.assign(b.begin() + i, b.begin() + i + size);
        memcpy(cd.p.data(), h, 11);
        cd.conf.assign(s.conf.begin() + (size_t)i * 8, s.conf.begin() + (size_t)(i + size) * 8);
        db.stats.packets++;
        db.stats.headerFixed += hdrFixed;
        std::vector<uint8_t> orig = cd.p;
        Fix fx = verifyPacket(cd.p, cd.conf);
        if (fx != Fix::Fail && handle(cd, fx)) {
            i += size;
            continue;
        }
        cd.p = orig;
        failed.push_back(std::move(cd));
        i += hdrFixed ? 1 : size;
    }

    // Repeated carousel copies of the same packet: borrow weak bits from
    // copies where the same bit was received confidently.
    for (size_t a = 0; a < failed.size(); a++) {
        Candidate& ca = failed[a];
        const int L = (int)ca.p.size();
        std::vector<float> vote(L * 8, 0.0f);
        int copies = 0;
        for (size_t o = 0; o < failed.size(); o++) {
            if (o == a || failed[o].p.size() != ca.p.size()) continue;
            const Candidate& cb = failed[o];
            int same = 0;
            for (int k = 11; k < L - 4; k++) same += ca.p[k] == cb.p[k];
            if (same < (L - 15) * 0.8) continue;
            copies++;
            for (int k = 0; k < L * 8; k++) {
                int bit = (cb.p[k >> 3] >> (k & 7)) & 1;
                vote[k] += bit ? cb.conf[k] : -cb.conf[k];
            }
        }
        if (!copies) continue;
        std::vector<uint8_t> p = ca.p;
        std::vector<float> conf = ca.conf;
        for (int k = 0; k < L * 8; k++) {
            int bit = (p[k >> 3] >> (k & 7)) & 1;
            float own = bit ? conf[k] : -conf[k];
            if (conf[k] < 0.35f && std::abs(vote[k]) > conf[k]) {
                int want = vote[k] > 0;
                if (want != bit) p[k >> 3] ^= (uint8_t)(1 << (k & 7));
                conf[k] = std::abs(own + vote[k]);
            }
        }
        Candidate cd{ca.pos, p, conf};
        Fix fx = verifyPacket(cd.p, cd.conf);
        if (fx != Fix::Fail && handle(cd, Fix::None)) {
            db.stats.fixedCombine++;
            ca.done = true;
        }
    }

    for (auto& cd : failed) {
        if (cd.done) continue;
        db.stats.failed++;
        if (keepUnverified) {
            SsDatabase tmp;
            if (parseCommands(cd.p, tmp, false, cd.pos)) {
                // only schedule data is trusted from unverified packets, and
                // never over verified data (see show-list merge rules)
                for (auto& kv : tmp.showLists) {
                    auto it = db.showLists.find(kv.first);
                    if (it == db.showLists.end()) db.showLists[kv.first] = kv.second;
                }
            }
        }
    }
}

// ------------------------------------------------------------ database ---

void SsDatabase::merge(const SsDatabase& o)
{
    for (auto& kv : o.channels) {
        auto& c = channels[kv.first];
        if (c.id == 0 || (c.name.empty() && !kv.second.name.empty()) || (!c.verified && kv.second.verified))
            c = kv.second;
    }
    for (auto& kv : o.programs) {
        auto& p = programs[kv.first];
        if (p.id == 0 || (p.title.empty() && !kv.second.title.empty()) || (p.partial && !kv.second.partial)) p = kv.second;
    }
    for (auto& kv : o.descriptions) {
        auto& d = descriptions[kv.first];
        if (d.id == 0 || (d.text.empty() && !kv.second.text.empty()) || (d.partial && !kv.second.partial)) d = kv.second;
    }
    for (auto& kv : o.showLists) {
        auto it = showLists.find(kv.first);
        if (it == showLists.end()) {
            showLists[kv.first] = kv.second;
            stats.listsAdded++;
        } else if (it->second.version == kv.second.version && (it->second.verified || !kv.second.verified)) {
            stats.listsUnchanged++;
        } else if (kv.second.verified || !it->second.verified) {
            it->second = kv.second;
            stats.listsReplaced++;
        }
    }
    for (auto& kv : o.themes)
        if (!themes.count(kv.first) || themes[kv.first].name.empty()) themes[kv.first] = kv.second;
    clock.insert(clock.end(), o.clock.begin(), o.clock.end());
    std::sort(clock.begin(), clock.end(), [](const SsClock& a, const SsClock& b) { return a.time < b.time; });
    for (auto& d : o.dstChanges)
        if (std::find(dstChanges.begin(), dstChanges.end(), d) == dstChanges.end()) dstChanges.push_back(d);
    sequenceNumbers.insert(sequenceNumbers.end(), o.sequenceNumbers.begin(), o.sequenceNumbers.end());
    log.insert(log.end(), o.log.begin(), o.log.end());
    const SsStats& s = o.stats;
    stats.streamBytes += s.streamBytes;
    stats.packets += s.packets;
    stats.crcOk += s.crcOk;
    stats.fixedSingle += s.fixedSingle;
    stats.fixedChase += s.fixedChase;
    stats.fixedCombine += s.fixedCombine;
    stats.failed += s.failed;
    stats.headerFixed += s.headerFixed;
    stats.encrypted += s.encrypted;
    for (auto& kv : s.encryptedByType) stats.encryptedByType[kv.first] += kv.second;
    stats.timeKeys.insert(s.timeKeys.begin(), s.timeKeys.end());
    for (auto& kv : s.commands) stats.commands[kv.first] += kv.second;
    stats.titlesAdded += s.titlesAdded;
    stats.descriptionsAdded += s.descriptionsAdded;
    stats.channelsAdded += s.channelsAdded;
}

std::vector<SsSlot> SsDatabase::slotsFor(int channel) const
{
    std::vector<SsSlot> v;
    auto it = showLists.lower_bound({channel, INT64_MIN});
    for (; it != showLists.end() && it->first.first == channel; ++it)
        for (auto& s : it->second.slots) v.push_back(s);
    std::stable_sort(v.begin(), v.end(), [](const SsSlot& a, const SsSlot& b) { return a.start < b.start; });
    std::vector<SsSlot> out;
    for (auto& s : v) {
        if (!out.empty() && s.start < out.back().start + out.back().duration) {
            if (s.flags & 0x01) continue; // dummy continuation of the previous day
            if (out.back().flags & 0x01) out.pop_back();
            else if (s.start == out.back().start) continue;
        }
        out.push_back(s);
    }
    return out;
}

std::vector<int> SsDatabase::channelIds() const
{
    std::set<int> ids;
    for (auto& kv : showLists) ids.insert(kv.first.first);
    for (auto& kv : channels) ids.insert(kv.first);
    return std::vector<int>(ids.begin(), ids.end());
}

SsTime SsDatabase::firstTime() const
{
    SsTime t = INT64_MAX;
    for (auto& kv : showLists)
        for (auto& s : kv.second.slots)
            if (!(s.flags & 0x01)) t = std::min(t, s.start);
    return t == INT64_MAX ? 0 : t;
}

SsTime SsDatabase::nowTime() const
{
    if (!clock.empty()) return clock.back().time;
    return firstTime();
}

int SsDatabase::utcOffset() const
{
    if (clock.empty()) return -8 * 60 + 60;
    return clock.back().utcOffsetMinutes + (clock.back().dst ? 60 : 0);
}

std::string SsDatabase::titleOf(const SsSlot& s, bool* guessed) const
{
    if (guessed) *guessed = false; // titles come only from this recording
    // a list's first slot may be a placeholder for a show that began the day before
    if (s.program == 0 && (s.flags & 0x01)) return "(started earlier)";
    return titleOf(s.program);
}

std::string SsDatabase::titleOf(int program) const
{
    auto it = programs.find(program);
    if (it != programs.end() && !it->second.title.empty())
        return it->second.partial ? it->second.title + "\xE2\x80\xA6" : it->second.title;
    if (program == 0) return "";
    // the title of this programme was not sent while recording
    char b[48];
    snprintf(b, sizeof b, "(title not received, #%d)", program);
    return b;
}

#include "ss_known_channels.inc"

const char* ssCommandName(int type)
{
    switch (type) {
    case 1: return "time";
    case 2: return "daylight-saving change";
    case 3: return "region";
    case 4: return "channel data";
    case 5: return "show list";
    case 6: return "show title";
    case 8: return "show description";
    case 11: return "theme category";
    case 12: return "theme sub-category";
    case 13: return "subscriber-unit reset";
    case 14: return "authorization";
    case 17: return "key distribution";
    case 20: return "sequence number";
    case 21: return "station node status";
    case 22: return "assign IR codes";
    case 24: return "subscriber-unit";
    default: return "";
    }
}

bool ssKnownChannel(int id, std::string& label, std::string& call)
{
    int lo = 0, hi = (int)(sizeof kKnownChannels / sizeof kKnownChannels[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kKnownChannels[mid].id == id) {
            label = kKnownChannels[mid].label;
            call = kKnownChannels[mid].call;
            return true;
        }
        if (kKnownChannels[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

// ------------------------------------------------------------- Huffman ---

#include "ss_huffman.inc"

namespace {
struct HuffTree {
    struct Node { int child[2] = {-1, -1}; int sym = -1; };
    std::vector<Node> nodes;
    HuffTree()
    {
        nodes.emplace_back();
        for (int s = 0; s < (int)(sizeof kSsHuffman / sizeof kSsHuffman[0]); s++) {
            int at = 0;
            for (const char* b = kSsHuffman[s].bits; *b; b++) {
                int bit = *b == '1';
                if (nodes[at].child[bit] < 0) {
                    nodes[at].child[bit] = (int)nodes.size();
                    nodes.emplace_back();
                }
                at = nodes[at].child[bit];
            }
            nodes[at].sym = s;
        }
    }
};
}

std::string ssHuffmanDecode(const uint8_t* p, size_t n, bool* complete)
{
    static const HuffTree tree;
    std::string out;
    int at = 0;
    if (complete) *complete = false;
    for (size_t i = 0; i < n * 8; i++) {
        int bit = (p[i >> 3] >> (7 - (i & 7))) & 1;
        at = tree.nodes[at].child[bit];
        if (at < 0) return out;                 // codeword not in the table
        int s = tree.nodes[at].sym;
        if (s < 0) continue;
        const char* sym = kSsHuffman[s].sym;
        if (!*sym) {                            // end of text
            if (complete) *complete = true;
            return out;
        }
        out += sym;
        at = 0;
    }
    return out;
}
