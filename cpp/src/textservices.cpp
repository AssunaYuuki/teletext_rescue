#include "textservices.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

static bool oddParity(uint8_t b) { return __builtin_popcount(b) & 1; }

double ccParityRatio(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& valid)
{
    size_t ok = 0, n = 0;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (!valid.empty() && !valid[i]) continue;
        n++;
        ok += oddParity(bytes[i]);
    }
    return n ? (double)ok / n : 0.0;
}

// Characters that differ from ASCII in the 608 basic set.
static std::string ccChar(uint8_t c)
{
    switch (c) {
    case 0x2A: return "\xC3\xA1"; // a acute
    case 0x5C: return "\xC3\xA9";
    case 0x5E: return "\xC3\xAD";
    case 0x5F: return "\xC3\xB3";
    case 0x60: return "\xC3\xBA";
    case 0x7B: return "\xC3\xA7";
    case 0x7C: return "\xC3\xB7";
    case 0x7D: return "\xC3\x91";
    case 0x7E: return "\xC3\xB1";
    case 0x7F: return "\xE2\x96\x88";
    default: return std::string(1, (char)c);
    }
}

namespace {
// One caption row: UTF-8 cells and a cursor, so text-mode redraws overwrite.
struct CcRow {
    std::vector<std::string> cells;
    size_t cursor = 0;
    void put(const std::string& c)
    {
        if (cursor >= cells.size()) cells.resize(cursor + 1, " ");
        cells[cursor++] = c;
    }
    std::string str() const
    {
        std::string s;
        for (auto& c : cells) s += c;
        size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    void clear() { cells.clear(); cursor = 0; }
};
} // namespace

void ccDecodeCaptions(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines, int firstChannel)
{
    // A field carries two channels (control byte 0x10-0x17 vs 0x18-0x1F),
    // each in caption mode (CC) or text mode (T, entered with TR/RTD).
    // Buffers: [channel * 2 + textMode].
    CcRow row[4];
    std::string last[4];
    bool text[2] = {false, false};
    int ch = 0;
    bool inXds = false;
    uint8_t p1 = 0, p2 = 0;
    auto flush = [&](int k) {
        std::string s = row[k].str();
        if (!s.empty() && s != last[k])
            lines.push_back(std::string(k & 1 ? "T" : "CC") + std::to_string(firstChannel + (k >> 1)) + ": " + s);
        if (!s.empty()) last[k] = s;
        row[k].clear();
    };
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        uint8_t b1 = bytes[i], b2 = bytes[i + 1];
        if (!oddParity(b1) || !oddParity(b2)) continue;
        uint8_t c1 = b1 & 0x7F, c2 = b2 & 0x7F;
        if (c1 == 0 && c2 == 0) continue;
        // XDS packets share field 2: skip from a start code to its checksum
        if (c1 >= 0x01 && c1 <= 0x0F) { inXds = c1 != 0x0F; continue; }
        if (inXds && c1 >= 0x20) continue;
        inXds = false;
        if (c1 >= 0x10 && c1 <= 0x1F) {
            bool dup = (b1 == p1 && b2 == p2);
            p1 = b1;
            p2 = b2;
            if (dup) continue;
            ch = (c1 & 0x08) ? 1 : 0;
            uint8_t base = c1 & 0xF7;
            int k = ch * 2 + text[ch];
            if ((base == 0x14 || base == 0x15) && c2 >= 0x20 && c2 <= 0x2F) {
                switch (c2) {
                case 0x2A: text[ch] = true; row[ch * 2 + 1].clear(); break; // TR: text restart
                case 0x2B: text[ch] = true; break;                          // RTD
                case 0x21: if (row[k].cursor) row[k].cursor--; break;       // backspace
                case 0x24: row[k].cells.resize(row[k].cursor); break;       // delete to end of row
                case 0x2D: flush(k); break;                                 // carriage return
                case 0x2C:
                case 0x2F: flush(ch * 2); break;                            // EDM, EOC
                case 0x20: case 0x25: case 0x26: case 0x27: case 0x29:       // caption modes
                    flush(ch * 2);
                    text[ch] = false;
                    break;
                }
            } else if (base == 0x17 && c2 >= 0x21 && c2 <= 0x23) {
                row[k].cursor += c2 - 0x20;                                 // tab offset
            } else if (c2 >= 0x40) {                                        // preamble address code
                if (text[ch]) row[k].cursor = (c2 & 0x10) ? ((c2 >> 1) & 7) * 4 : 0;
                else if (!row[k].cells.empty()) { row[k].cursor = row[k].cells.size(); row[k].put(" "); }
            } else if (base == 0x11 && c2 >= 0x30 && c2 <= 0x3F) {
                static const char* sp[16] = {"\xC2\xAE", "\xC2\xB0", "\xC2\xBD", "\xC2\xBF", "\xE2\x84\xA2", "\xC2\xA2",
                                             "\xC2\xA3", "\xE2\x99\xAA", "\xC3\xA0", " ", "\xC3\xA8", "\xC3\xA2",
                                             "\xC3\xAA", "\xC3\xAE", "\xC3\xB4", "\xC3\xBB"}; // (R) deg 1/2 inv? TM cent pound note a` space e` a^ e^ i^ o^ u^
                row[k].put(sp[c2 - 0x30]);
            }
            continue;
        }
        p1 = p2 = 0;
        CcRow& r = row[ch * 2 + text[ch]];
        if (c1 >= 0x20) r.put(ccChar(c1));
        if (c2 >= 0x20) r.put(ccChar(c2));
    }
    for (int k = 0; k < 4; k++) flush(k);
}

static const char* kXdsClass[] = {"", "Current", "Current", "Future", "Future", "Channel", "Channel",
                                  "Misc", "Misc", "Public", "Public", "Reserved", "Reserved", "Private", "Private"};

static std::string xdsText(const std::vector<uint8_t>& d)
{
    std::string s;
    for (uint8_t c : d)
        if (c >= 0x20 && c < 0x7F) s += (char)c;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// EIA/CEA-608 program types (basic and detailed), codes 0x20..0x7F
static const char* kXdsGenre[96] = {
    "Education", "Entertainment", "Movie", "News", "Religious", "Sports", "Other", "Action", "Advertisement", "Animated",
    "Anthology", "Automobile", "Awards", "Baseball", "Basketball", "Bulletin", "Business", "Classical", "College", "Combat",
    "Comedy", "Commentary", "Concert", "Consumer", "Contemporary", "Crime", "Dance", "Documentary", "Drama", "Elementary",
    "Erotica", "Exercise", "Fantasy", "Farm", "Fashion", "Fiction", "Food", "Football", "Foreign", "Fund raiser",
    "Game/quiz", "Garden", "Golf", "Government", "Health", "High school", "History", "Hobby", "Hockey", "Home",
    "Horror", "Information", "Instruction", "International", "Interview", "Language", "Legal", "Live", "Local", "Math",
    "Medical", "Meeting", "Military", "Miniseries", "Music", "Mystery", "National", "Nature", "Police", "Politics",
    "Premiere", "Prerecorded", "Product", "Professional", "Public", "Racing", "Reading", "Repair", "Repeat", "Review",
    "Romance", "Science", "Series", "Service", "Shopping", "Soap opera", "Special", "Suspense", "Talk", "Technical",
    "Tennis", "Travel", "Variety", "Video", "Weather", "Western"};
static const char* kXdsLang[8] = {"unknown", "English", "Spanish", "French", "German", "Italian", "other", "none"};
static const char* kDays[8] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static std::string xdsDescribe(int cls, int type, const std::vector<uint8_t>& d)
{
    char b[256];
    std::string s;
    int k = (cls + 1) / 2; // 1 current, 2 future, 3 channel, 4 misc, 5 public, 7 private
    const bool prog = k == 1 || k == 2;
    if (prog && type == 1 && d.size() >= 4) {
        snprintf(b, sizeof b, "program start %d/%d %02d:%02d UTC%s%s", d[3] & 0x0F, d[2] & 0x1F, d[1] & 0x1F, d[0] & 0x3F,
                 (d[3] & 0x10) ? ", tape delayed" : "", (d[1] & 0x20) ? ", daylight saving" : "");
        return b;
    }
    if (prog && type == 2 && d.size() >= 2) {
        snprintf(b, sizeof b, "program length %d:%02d", d[1] & 0x3F, d[0] & 0x3F);
        s = b;
        if (d.size() >= 4) {
            snprintf(b, sizeof b, ", elapsed %d:%02d", d[3] & 0x3F, d[2] & 0x3F);
            s += b;
            if (d.size() >= 6 && (d[4] & 0x3F)) { snprintf(b, sizeof b, ":%02d", d[4] & 0x3F); s += b; }
        }
        return s;
    }
    if (prog && type == 3) return "program name: " + xdsText(d);
    if (prog && type == 4) {
        s = "program type:";
        for (uint8_t c : d)
            if (c >= 0x20 && c <= 0x7F) s += std::string(" ") + kXdsGenre[c - 0x20] + ",";
        if (s.back() == ',') s.pop_back();
        return s;
    }
    if (prog && type == 5 && d.size() >= 2) {
        static const char* mpaa[] = {"N/A", "G", "PG", "PG-13", "R", "NC-17", "X", "NR"};
        static const char* tv[] = {"None", "TV-Y", "TV-Y7", "TV-G", "TV-PG", "TV-14", "TV-MA", "None"};
        static const char* caE[] = {"Exempt", "C", "C8+", "G", "PG", "14+", "18+", ""};
        static const char* caF[] = {"Exempt", "G", "8 ans +", "13 ans +", "16 ans +", "18 ans +", "", ""};
        // system: a1 a0 = bits 4 3 of the first byte; Canada also a3 (bit 3 of the second) and a2 (bit 5 of the first)
        int sys = (d[0] >> 3) & 3;
        if (sys == 0 || sys == 2) return std::string("rating MPAA ") + mpaa[d[0] & 7];
        if (sys == 1) {
            int g = d[1] & 7;
            std::string r = std::string("rating ") + tv[g];
            if (d[0] & 0x20) r += " D (dialogue)";
            if (d[1] & 0x20) r += g == 2 ? " FV (fantasy violence)" : " V (violence)";
            if (d[1] & 0x10) r += " S (sex)";
            if (d[1] & 0x08) r += " L (language)";
            return r;
        }
        bool french = (d[0] & 0x20) != 0;
        if (!(d[1] & 0x08)) return std::string("rating Canada ") + (french ? "French " + std::string(caF[d[1] & 7]) : "English " + std::string(caE[d[1] & 7]));
        return "rating (reserved system)";
    }
    if (prog && type == 6 && d.size() >= 2) {
        static const char* mt[] = {"unknown", "mono", "simulated stereo", "stereo", "stereo surround", "data service", "other", "none"};
        static const char* st[] = {"unknown", "mono", "video descriptions", "non-program audio", "special effects", "data service", "other", "none"};
        return std::string("audio: main ") + kXdsLang[(d[0] >> 3) & 7] + " " + mt[d[0] & 7] + ", second program (SAP) " +
               kXdsLang[(d[1] >> 3) & 7] + " " + st[d[1] & 7];
    }
    if (prog && type == 7) {
        static const char* svc[] = {"CC1", "T1", "CC2", "T2", "CC3", "T3", "CC4", "T4"};
        s = "caption services:";
        for (uint8_t c : d)
            if (c) s += std::string(" ") + svc[c & 7] + " " + kXdsLang[(c >> 3) & 7] + ",";
        if (s.back() == ',') s.pop_back();
        return s;
    }
    if (prog && type == 8 && !d.empty()) {
        static const char* cg[] = {"copying permitted", "no more copies (condition not used)", "one generation", "never copy"};
        static const char* aps[] = {"no APS", "PSP on, split burst off", "PSP on, 2-line split burst", "PSP on, 4-line split burst"};
        return std::string("copy protection CGMS-A: ") + cg[(d[0] >> 3) & 3] + ", " + aps[(d[0] >> 1) & 3] + ((d[0] & 1) ? ", analog source" : "");
    }
    if (prog && type == 9 && d.size() >= 2) {
        int a = 22 + (d[0] & 0x3F), e = 262 - (d[1] & 0x3F);
        snprintf(b, sizeof b, "aspect ratio: active video lines %d-%d (%.2f:1)%s", a, e, 4.0 / 3.0 * 241 / std::max(1, e - a + 1),
                 d.size() >= 3 && (d[2] & 1) ? ", anamorphic (squeezed)" : "");
        return b;
    }
    if (prog && (type == 0x0C || type == 0x0D)) return std::string("composite packet ") + (type == 0x0C ? "1" : "2") + ": " + xdsText(d);
    if (prog && type >= 0x10 && type <= 0x17) return "description " + std::to_string(type - 0x0F) + ": " + xdsText(d);
    if (k == 3 && type == 1) return "network: " + xdsText(d);
    if (k == 3 && type == 2) {
        std::string t = xdsText(d);
        // four letters, optionally followed by the two-digit native channel
        if (t.size() == 6 && isdigit((unsigned char)t[4]) && isdigit((unsigned char)t[5]))
            return "call letters: " + t.substr(0, 4) + ", channel " + std::to_string(atoi(t.substr(4).c_str()));
        return "call letters: " + t;
    }
    if (k == 3 && type == 3 && d.size() >= 2) {
        snprintf(b, sizeof b, "tape delay %d:%02d", d[1] & 0x1F, d[0] & 0x3F);
        return b;
    }
    if (k == 3 && type == 4 && d.size() >= 4) {
        snprintf(b, sizeof b, "transmission signal ID (TSID) %04X", (d[0] & 15) << 12 | (d[1] & 15) << 8 | (d[2] & 15) << 4 | (d[3] & 15));
        return b;
    }
    if (k == 4 && type == 1 && d.size() >= 6) {
        snprintf(b, sizeof b, "time of day %02d:%02d UTC, %d/%d/%d %s%s", d[1] & 0x1F, d[0] & 0x3F, d[3] & 0x0F, d[2] & 0x1F,
                 1990 + (d[5] & 0x3F), kDays[d[4] & 7], (d[3] & 0x20) ? " (tape delayed)" : "");
        return b;
    }
    if (k == 4 && type == 2 && d.size() >= 6) {
        snprintf(b, sizeof b, "impulse capture: start %d/%d %02d:%02d UTC, length %d:%02d", d[3] & 0x0F, d[2] & 0x1F, d[1] & 0x1F, d[0] & 0x3F,
                 d[5] & 0x3F, d[4] & 0x3F);
        return b;
    }
    if (k == 4 && type == 3) {
        s = "supplemental data on lines:";
        for (uint8_t c : d)
            if (c) s += " " + std::to_string(c & 0x1F) + ((c & 0x20) ? " field 2" : " field 1");
        return s;
    }
    if (k == 4 && type == 4 && !d.empty()) {
        snprintf(b, sizeof b, "local time zone UTC-%d%s", d[0] & 0x1F, (d[0] & 0x20) ? " DST" : "");
        return b;
    }
    if (k == 4 && type == 0x40 && !d.empty()) return "out-of-band channel " + std::to_string(d[0] & 0x3F);
    if (k == 5 && type == 1) return "weather service (NWS) code: " + xdsText(d);
    if (k == 5 && type == 2) return "weather service (NWS) message: " + xdsText(d);
    if (k == 7) s = "private type " + std::to_string(type) + ": ";
    else s = "type " + std::to_string(type) + ": ";
    std::string txt = xdsText(d);
    // CBS network 1997, line 21 field 2: one a second, 0x40 | second, minute, hour, date + 4 constant bytes
    // ("DGT_KG@K" = 20:07:04 on the 31st; agrees with the AMOL clock of the same feed to the second)
    if (k == 7 && type == 1 && d.size() == 8 && txt.size() == 8 && (d[0] & 0x3F) < 60 && (d[1] & 0x3F) < 60 && (d[2] & 0x3F) < 24 &&
        (d[3] & 0x3F) >= 1 && (d[3] & 0x3F) <= 31) {
        snprintf(b, sizeof b, " = clock %02d:%02d:%02d, day %d", d[2] & 0x3F, d[1] & 0x3F, d[0] & 0x3F, d[3] & 0x3F);
        return s + "\"" + txt + "\"" + b;
    }
    if (txt.size() * 2 >= d.size() && !txt.empty()) return s + "\"" + txt + "\"";
    for (uint8_t c : d) {
        snprintf(b, sizeof b, "%02x ", c);
        s += b;
    }
    return s;
}

void ccDecodeXds(const std::vector<uint8_t>& bytes, std::vector<std::string>& lines)
{
    // each class and type has its own packet in progress: a packet may be
    // interrupted by another one and resumed with a continue code
    struct Pkt { std::vector<uint8_t> data; int sum = 0; };
    std::map<std::pair<int, int>, Pkt> open;
    std::pair<int, int> cur{-1, -1};
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        uint8_t b1 = bytes[i], b2 = bytes[i + 1];
        if (!oddParity(b1) || !oddParity(b2)) { cur = {-1, -1}; continue; }
        uint8_t c1 = b1 & 0x7F, c2 = b2 & 0x7F;
        if (c1 >= 0x01 && c1 <= 0x0E) {
            int cls = (c1 - 1) | 1;              // start code of the class
            cur = {cls, c2};
            if (c1 & 1) { Pkt& p = open[cur]; p.data.clear(); p.sum = c1 + c2; }
            else if (!open.count(cur)) cur = {-1, -1};   // continue code: not included in the checksum
            continue;
        }
        if (cur.first < 0) continue;
        if (c1 == 0x0F) {
            Pkt& p = open[cur];
            int sum = p.sum + c1 + c2;
            if ((sum & 0x7F) == 0) {
                std::string s = std::string("XDS ") + kXdsClass[cur.first] + " " + xdsDescribe(cur.first, cur.second, p.data);
                if (lines.empty() || lines.back() != s) lines.push_back(s);
            }
            open.erase(cur);
            cur = {-1, -1};
            continue;
        }
        if (c1 >= 0x10 && c1 <= 0x1F) { cur = {-1, -1}; continue; } // a caption control interrupts
        Pkt& p = open[cur];
        p.data.push_back(c1);
        p.data.push_back(c2);
        p.sum += c1 + c2;
        if (p.data.size() > 32) { open.erase(cur); cur = {-1, -1}; }
    }
}

int ccCountXds(const std::vector<uint8_t>& bytes)
{
    std::vector<std::string> l;
    ccDecodeXds(bytes, l);
    return (int)l.size();
}

