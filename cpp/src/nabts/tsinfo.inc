// Everything a DVB transport stream says about itself, as lines of text (dvbts.cpp ts_info): the channels (SDT), the
// network (NIT), each program's streams with their languages, teletext pages and subtitles (PAT / PMT), the time
// of the broadcast (TDT / TOT), the programme on the air and the next one (EIT present / following), how long the
// stream is (PCR), and what is wrong with it (continuity errors, scrambled packets).
#include <cstdio>
#include <map>
#include <set>
#include "tsdetail.inc"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
// a DVB text (EN 300 468 annex A): the first byte names the character table
std::string dvbText(const u8* p, int n)
{
    if (n <= 0) return "";
    int cp = 20269;   // ISO 6937 (the default table)
    int i = 0;
    if (p[0] >= 0x01 && p[0] <= 0x0B) { static const int t[12] = {0, 28595, 28596, 28597, 28598, 28599, 0, 0, 0, 28603, 28604, 28605}; cp = t[p[0]] ? t[p[0]] : 28591; i = 1; }
    else if (p[0] == 0x10 && n >= 3) { int k = p[2]; static const int t[16] = {0, 28591, 28592, 28593, 28594, 28595, 28596, 28597, 28598, 28599, 0, 0, 0, 28603, 28604, 28605}; cp = k < 16 && t[k] ? t[k] : 28591; i = 3; }
    else if (p[0] == 0x11 || p[0] == 0x14) { cp = 1201; i = 1; }   // UCS-2 big endian
    else if (p[0] == 0x15) { cp = 65001; i = 1; }
    else if (p[0] < 0x20) i = 1;
    std::string raw;
    for (int k = i; k < n; k++) {
        u8 c = p[k];
        if (cp != 1201 && (c == 0x8A)) { raw += '\n'; continue; }   // CR/LF of the table
        if (cp != 1201 && ((c >= 0x80 && c <= 0x9F) || c < 0x20)) continue;   // emphasis on/off and controls
        raw += (char)c;
    }
#ifdef _WIN32
    if (cp == 1201) {
        std::wstring w;
        for (size_t k = 0; k + 1 < raw.size(); k += 2) w += (wchar_t)(((u8)raw[k] << 8) | (u8)raw[k + 1]);
        int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string o((size_t)m, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), o.data(), m, nullptr, nullptr);
        return o;
    }
    if (cp == 65001) return raw;
    int m = MultiByteToWideChar(cp, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    if (m <= 0) cp = 28591, m = MultiByteToWideChar(cp, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    std::wstring w((size_t)m, 0);
    MultiByteToWideChar(cp, 0, raw.c_str(), (int)raw.size(), w.data(), m);
    int k = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string o((size_t)k, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), o.data(), k, nullptr, nullptr);
    return o;
#else
    std::string o;   // Latin-1 as UTF-8 (other tables approximately)
    for (u8 c : std::string(raw)) { if (c < 0x80) o += (char)c; else { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 0x3F)); } }
    return o;
#endif
}
std::string mjdTime(const u8* p)   // 16-bit MJD + 6 BCD digits
{
    int mjd = p[0] << 8 | p[1];
    if (mjd == 0xFFFF) return "?";
    int y = (int)((mjd - 15078.2) / 365.25), mo = (int)((mjd - 14956.1 - (int)(y * 365.25)) / 30.6001);
    int d = mjd - 14956 - (int)(y * 365.25) - (int)(mo * 30.6001), k = mo == 14 || mo == 15;
    y += k; mo = mo - 1 - k * 12;
    auto bcd = [](u8 b) { return (b >> 4) * 10 + (b & 15); };
    char s[40];
    snprintf(s, sizeof s, "%02d.%02d.%04d %02d:%02d:%02d", d, mo, 1900 + y, bcd(p[2]), bcd(p[3]), bcd(p[4]));
    return s;
}
std::string bcdDur(const u8* p)
{
    auto bcd = [](u8 b) { return (b >> 4) * 10 + (b & 15); };
    char s[16];
    snprintf(s, sizeof s, "%d:%02d:%02d", bcd(p[0]), bcd(p[1]), bcd(p[2]));
    return s;
}
const char* streamKind(int t)
{
    switch (t) {
    case 0x01: return "MPEG-1 video"; case 0x02: return "MPEG-2 video"; case 0x03: return "MPEG-1 audio"; case 0x04: return "MPEG-2 audio";
    case 0x05: return "private sections"; case 0x06: return "private data (PES)"; case 0x0B: return "DSM-CC (data carousel)"; case 0x0F: return "AAC audio";
    case 0x11: return "AAC LATM audio"; case 0x1B: return "H.264 video"; case 0x24: return "HEVC video"; case 0x81: return "AC-3 audio (ATSC)";
    default: return "stream";
    }
}
// conditional access system by its id (ETR 162)
std::string caName(int id)
{
    int h = id >> 8;
    const char* n = h == 0x01 ? "Seca / Mediaguard" : h == 0x05 ? "Viaccess" : h == 0x06 ? "Irdeto" : h == 0x09 ? "NDS Videoguard" : h == 0x0B ? "Conax"
                  : h == 0x0D ? "Cryptoworks" : h == 0x0E ? "PowerVu" : h == 0x17 || h == 0x18 ? "Nagravision" : h == 0x26 ? "BISS" : h == 0x4A ? "DRE / other" : "";
    return (n[0] ? std::string(n) + " " : std::string()) + fmt("0x%04X", id);
}
std::string lang3(const u8* p) { std::string s; for (int i = 0; i < 3; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?'; return s; }
}  // namespace

std::vector<std::string> ts_info(const std::string &path) {
    std::vector<std::string> out;
    MappedFile mf(path);
    const u8 *d = mf.data(); uint64_t n = mf.size();
    uint64_t off = 0;
    int stride = ts_layout(d, std::min<uint64_t>(n, 1 << 16), off);
    if (!stride) { out.push_back("not an MPEG transport stream"); return out; }
    // sections put together per PID (pointer field, several packets)
    std::map<int, Bytes> sec;
    std::map<int, std::vector<Bytes>> tables;   // pid -> whole sections (first copy of each table id / extension / number)
    std::set<std::string> seenSec;
    std::map<int, long> count, ccErr, scrambled;
    std::map<int, int> lastCc;
    int64_t pcr0 = -1, pcr1 = -1;
    int pcrPid = -1;
    auto takeSections = [&](int pid, Bytes& b) {
        size_t i = 0;
        while (i + 3 <= b.size() && b[i] != 0xFF) {
            int len = ((b[i + 1] & 15) << 8) | b[i + 2];
            if (i + 3 + len > b.size()) break;
            Bytes s(b.begin() + (long)i, b.begin() + (long)(i + 3 + len));
            std::string key = std::to_string(pid) + ":" + std::to_string(s[0]) + ":" + (s.size() > 7 ? std::to_string(s[3] << 8 | s[4]) + ":" + std::to_string(s[6]) : "");
            if ((pid == 0x14 || seenSec.insert(key).second) && tables[pid].size() < 400) tables[pid].push_back(s);
            i += 3 + len;
        }
        b.erase(b.begin(), b.begin() + (long)std::min(i, b.size()));
    };
    std::set<int> psiPids = {0x00, 0x10, 0x11, 0x12, 0x14};
    std::map<int, Bytes> esData;                 // the first bytes of every stream
    std::map<int, double> ptsFirst, ptsLast;
    std::map<int, long> pesCount;
    std::set<int> started;
    std::set<int> pmtPids;
    uint64_t packets = 0;
    for (uint64_t p = off; p + 188 <= n; p += stride) {
        const u8 *t = d + p;
        if (t[0] != 0x47) continue;
        packets++;
        int pid = ((t[1] & 0x1F) << 8) | t[2];
        count[pid]++;
        if (t[1] & 0x80) continue;   // transport error indicator
        if ((t[3] >> 6) != 0) scrambled[pid]++;
        int afc = (t[3] >> 4) & 3, cc = t[3] & 15;
        if ((afc & 1) && pid != 0x1FFF) {
            auto lc = lastCc.find(pid);
            if (lc != lastCc.end() && cc != ((lc->second + 1) & 15) && cc != lc->second) ccErr[pid]++;
            lastCc[pid] = cc;
        }
        int h = 4;
        if (afc & 2) {
            int al = t[4];
            if (al >= 7 && (t[5] & 0x10)) {   // PCR: the stream's clock, for its length
                int64_t base = ((int64_t)t[6] << 25) | (t[7] << 17) | (t[8] << 9) | (t[9] << 1) | (t[10] >> 7);
                if (pcrPid < 0) pcrPid = pid;
                if (pid == pcrPid) { if (pcr0 < 0) pcr0 = base; pcr1 = base; }
            }
            h += 1 + al;
        }
        if (!(afc & 1) || h >= 188) continue;
        if (!psiPids.count(pid) && !pmtPids.count(pid)) {
            // a stream: its first bytes (what it is), when its PES packets come (PTS), how many
            const u8* q = t + h;
            if (t[1] & 0x40) {
                if (h + 14 <= 188 && q[0] == 0 && q[1] == 0 && q[2] == 1) {
                    pesCount[pid]++;
                    if (q[7] & 0x80) {
                        double pt = ((int64_t)((q[9] >> 1) & 7) << 30 | (int64_t)q[10] << 22 | (int64_t)(q[11] >> 1) << 15 | q[12] << 7 | q[13] >> 1) / 90000.0;
                        if (!ptsFirst.count(pid)) ptsFirst[pid] = pt;
                        ptsLast[pid] = pt;
                    }
                    started.insert(pid);
                    int hl = 9 + q[8];
                    auto& es = esData[pid];
                    if (es.size() < (size_t)(256 << 10) && h + hl < 188) es.insert(es.end(), q + hl, t + 188);
                }
            } else if (started.count(pid)) {
                auto& es = esData[pid];
                if (es.size() < (size_t)(256 << 10)) es.insert(es.end(), q, t + 188);
            }
            continue;
        }
        Bytes& b = sec[pid];
        if (t[1] & 0x40) {
            int ptr = t[h];
            if (!b.empty() && h + 1 + ptr <= 188) { b.insert(b.end(), t + h + 1, t + h + 1 + ptr); takeSections(pid, b); }
            b.assign(t + h + 1 + std::min(ptr, 187 - h), t + 188);
            takeSections(pid, b);
        } else if (!b.empty()) { b.insert(b.end(), t + h, t + 188); takeSections(pid, b); }
        // the PMTs named by the PAT are read as they come
        if (pid == 0)
            for (auto& s : tables[0])
                if (s[0] == 0x00)
                    for (size_t i = 8; i + 4 <= s.size() - 4; i += 4) { int prog = s[i] << 8 | s[i + 1], pp = ((s[i + 2] & 0x1F) << 8) | s[i + 3]; if (prog) pmtPids.insert(pp); }
    }
    out.push_back(fmt("MPEG transport stream: %d-byte packets, %llu packets, %zu PIDs", stride, (unsigned long long)packets, count.size()));
    if (pcr0 >= 0 && pcr1 > pcr0) {
        double sec = (pcr1 - pcr0) / 90000.0;
        out.push_back(fmt("length by the clock (PCR on PID 0x%X): %d:%02d:%02d", pcrPid, (int)sec / 3600, (int)sec / 60 % 60, (int)sec % 60));
    }
    // time of the broadcast
    for (auto& s : tables[0x14])
        if ((s[0] == 0x70 || s[0] == 0x73) && s.size() >= 8) { out.push_back("time of the broadcast (TDT, UTC): " + mjdTime(&s[3])); break; }
    // network
    for (auto& s : tables[0x10]) {
        if (s[0] != 0x40 || s.size() < 12) continue;
        int nid = s[3] << 8 | s[4], dl = ((s[8] & 15) << 8) | s[9];
        std::string name;
        for (int i = 10; i + 2 <= 10 + dl && i + 2 <= (int)s.size(); i += 2 + s[i + 1])
            if (s[i] == 0x40) name = dvbText(&s[i + 2], s[i + 1]);
        out.push_back(fmt("network (NIT): id %d", nid) + (name.empty() ? "" : " \xC2\xAB" + name + "\xC2\xBB"));
        break;
    }
    // channels
    std::map<int, std::string> svcName;
    for (auto& s : tables[0x11]) {
        if (s[0] != 0x42 || s.size() < 15) continue;
        int tsid = s[3] << 8 | s[4], onid = s[8] << 8 | s[9];
        for (size_t i = 11; i + 5 <= s.size() - 4;) {
            int sid = s[i] << 8 | s[i + 1], running = s[i + 3] >> 5, free = !(s[i + 3] & 0x10), dl = ((s[i + 3] & 15) << 8) | s[i + 4];
            std::string name, prov; int type = -1;
            for (size_t k = i + 5; k + 2 <= i + 5 + dl && k + 2 <= s.size(); k += 2 + s[k + 1])
                if (s[k] == 0x48 && k + 3 < s.size()) {
                    type = s[k + 2]; int pl = s[k + 3];
                    prov = dvbText(&s[k + 4], pl);
                    if (k + 4 + pl < s.size()) name = dvbText(&s[k + 5 + pl], s[k + 4 + pl]);
                }
            static const char* run[8] = {"", "not running", "starts in seconds", "pausing", "running", "off the air", "", ""};
            svcName[sid] = name;
            out.push_back(fmt("channel (SDT): service %d \xC2\xAB", sid) + name + "\xC2\xBB" + (prov.empty() ? "" : ", provider " + prov) +
                          (type == 1 ? ", TV" : type == 2 ? ", radio" : type == 0x0C ? ", data" : type == 0x19 || type == 0x11 ? ", HD TV" : type >= 0 ? fmt(", type 0x%02X", type) : "") +
                          (free ? ", free to air" : ", scrambled (CA)") + (run[running][0] ? std::string(", ") + run[running] : "") +
                          fmt("; transport stream %d of network %d", tsid, onid));
            i += 5 + dl;
        }
    }
    // programs and their streams
    for (int pp : pmtPids)
        for (auto& s : tables[pp]) {
            if (s[0] != 0x02 || s.size() < 16) continue;
            int prog = s[3] << 8 | s[4], pcr = ((s[8] & 0x1F) << 8) | s[9], pil = ((s[10] & 15) << 8) | s[11];
            out.push_back(fmt("program %d (PMT on PID 0x%X)", prog, pp) + (svcName.count(prog) ? " \xC2\xAB" + svcName[prog] + "\xC2\xBB" : "") + fmt(", clock on PID 0x%X:", pcr));
            size_t progLine = out.size() - 1;
            std::set<int> cas;   // conditional access systems of the program (said once, not on every stream)
            for (size_t k = 12; k + 2 <= 12 + (size_t)pil && k + 2 <= s.size(); k += 2 + s[k + 1])
                if (s[k] == 0x09 && s[k + 1] >= 4) cas.insert(s[k + 2] << 8 | s[k + 3]);
            for (size_t i = 12 + pil; i + 5 <= s.size() - 4;) {
                int st = s[i], ep = ((s[i + 1] & 0x1F) << 8) | s[i + 2], el = ((s[i + 3] & 15) << 8) | s[i + 4];
                std::string kind = streamKind(st), extra;
                for (size_t k = i + 5; k + 2 <= i + 5 + el && k + 2 <= s.size(); k += 2 + s[k + 1]) {
                    int tag = s[k], dl = s[k + 1];
                    const u8* q = &s[k + 2];
                    if (tag == 0x0A && dl >= 4) extra += ", language " + lang3(q);
                    else if (tag == 0x56 || tag == 0x46) {   // teletext / VBI teletext: language, kind, page
                        kind = tag == 0x56 ? "teletext (EN 300 472)" : "VBI teletext";
                        for (int j = 0; j + 5 <= dl; j += 5) {
                            static const char* tt[6] = {"", "initial page", "subtitles", "additional information", "programme schedule", "subtitles for the hard of hearing"};
                            int ty = q[j + 3] >> 3, mag = q[j + 3] & 7;
                            extra += ", " + lang3(q + j) + " " + (ty < 6 && tt[ty][0] ? tt[ty] : "page") + fmt(" %d%02X", mag ? mag : 8, q[j + 4]);
                        }
                    } else if (tag == 0x59) {
                        kind = "DVB subtitles (EN 300 743)";
                        for (int j = 0; j + 8 <= dl; j += 8) extra += ", " + lang3(q + j);
                    } else if (tag == 0x6A) kind = "AC-3 audio";
                    else if (tag == 0x7A) kind = "E-AC-3 audio";
                    else if (tag == 0x52 && dl >= 1) extra += fmt(", component %d", q[0]);
                    else if (tag == 0x09 && dl >= 4) cas.insert(q[0] << 8 | q[1]);
                    else if (tag == 0x45) kind = "VBI data (EN 301 775)";
                }
                long c = count.count(ep) ? count[ep] : 0;
                // what the stream itself says, and when it runs from the start of the recording
                {
                    const Bytes& es = esData[ep];
                    std::string det = st == 0x01 || st == 0x02 ? mpeg2Video(es) : st == 0x1B ? h264Video(es) : st == 0x03 || st == 0x04 ? mpegAudio(es)
                                    : st == 0x0F ? aacAudio(es) : kind.find("AC-3") != std::string::npos || st == 0x81 ? ac3Audio(es) : std::string();
                    if (!det.empty()) extra += "; " + det;
                    if (kind.find("subtitles") != std::string::npos && pesCount.count(ep)) extra += fmt("; %ld subtitle packets (PES)", pesCount[ep]);
                    if (ptsFirst.count(ep)) {
                        double base = 1e18;
                        for (auto& kv : ptsFirst) base = std::min(base, kv.second);
                        extra += "; " + clockOf(ptsFirst[ep] - base) + " - " + clockOf(ptsLast[ep] - base);
                    }
                }
                out.push_back(fmt("  PID 0x%X (%d): ", ep, ep) + kind + extra + fmt("; %ld packets", c) +
                              (ccErr.count(ep) ? fmt(", %ld continuity errors", ccErr[ep]) : "") + (scrambled.count(ep) ? fmt(", %ld scrambled", scrambled[ep]) : ""));
                i += 5 + el;
            }
            if (!cas.empty()) {
                std::string c;
                for (int id : cas) c += (c.empty() ? "" : ", ") + caName(id);
                long scr = 0;
                for (auto& kv : scrambled) scr += kv.second;
                out[progLine] += " conditional access " + c + (scr ? "" : " (the recording is not scrambled: descrambled when it was made)");
            }
            break;
        }
    // the programme on the air and the next one (EIT present / following of this stream)
    std::set<std::string> said;
    for (auto& s : tables[0x12]) {
        if (s[0] != 0x4E || s.size() < 20) continue;
        int sid = s[3] << 8 | s[4], num = s[6];
        for (size_t i = 14; i + 12 <= s.size() - 4;) {
            std::string start = mjdTime(&s[i + 2]), dur = bcdDur(&s[i + 7]);
            int dl = ((s[i + 10] & 15) << 8) | s[i + 11];
            std::string name, text, genre;
            for (size_t k = i + 12; k + 2 <= i + 12 + dl && k + 2 <= s.size(); k += 2 + s[k + 1]) {
                if (s[k] == 0x4D && k + 6 <= s.size()) {   // short event: language, name, text
                    int nl = s[k + 5];
                    name = dvbText(&s[k + 6], nl);
                    if (k + 6 + nl < s.size()) text = dvbText(&s[k + 7 + nl], s[k + 6 + nl]);
                }
                if (s[k] == 0x54 && s[k + 1] >= 1) genre = fmt("genre 0x%02X", s[k + 2]);
            }
            std::string line = std::string(num == 0 ? "on the air (EIT)" : "next (EIT)") + fmt(", service %d: ", sid) + start + " UTC, " + dur + "  \xC2\xAB" + name + "\xC2\xBB" +
                               (genre.empty() ? "" : ", " + genre) + (text.empty() ? "" : " \xE2\x80\x94 " + text);
            if (said.insert(line).second) out.push_back(line);
            i += 12 + dl;
        }
    }
    // PIDs no program names (data, padding)
    std::string other;
    for (auto& kv : count) {
        if (kv.first == 0x1FFF) { other += fmt(", padding %ld", kv.second); continue; }
        bool named = psiPids.count(kv.first) || pmtPids.count(kv.first);
        for (auto& l : out) if (l.find(fmt("PID 0x%X ", kv.first)) != std::string::npos) named = true;
        if (!named) other += fmt(", 0x%X: %ld", kv.first, kv.second);
    }
    if (!other.empty()) out.push_back("other PIDs (packets)" + other.substr(1));
    long cc = 0, sc = 0;
    for (auto& kv : ccErr) cc += kv.second;
    for (auto& kv : scrambled) sc += kv.second;
    out.push_back(fmt("errors: %ld continuity (packets lost in the recording), %ld scrambled packets", cc, sc));
    return out;
}
