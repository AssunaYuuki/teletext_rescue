// What was on the air: the programme names the recording itself tells, put
// together from every service that carries one.
//   XDS            programme name, network, call letters, time of day (UTC)
//   StarSight      the schedule of the station's own channel at that time
//   AMOL           network source ID and the network clock (local time)
//   teletext       channel name and time from packet 8/30
#include "decoder.h"
#include <set>
#include <cstdio>
#include <cstring>
#include <map>

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static SsTime ssFromCivil(int y, int mo, int d, int h, int mi)
{
    // days from 1992-01-01 (civil-from-days inverse, proleptic Gregorian)
    auto days = [](int y, int m, int d) {
        y -= m <= 2;
        int era = (y >= 0 ? y : y - 399) / 400;
        int yoe = y - era * 400;
        int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return (int64_t)era * 146097 + doe - 719468;
    };
    return (days(y, mo, d) - days(1992, 1, 1)) * 1440 + h * 60 + mi;
}

std::vector<std::string> onAirSummary(const DecodeResult& r)
{
    std::vector<std::string> out;
    // --- XDS
    std::vector<std::pair<std::string, int>> names;   // in order of first appearance
    std::string network, call;
    int th = -1, tm = 0, tmo = 0, td = 0, ty = 0;
    for (auto& l : r.xds) {
        auto after = [&](const char* key) -> std::string {
            size_t p = l.find(key);
            return p == std::string::npos ? std::string() : trim(l.substr(p + strlen(key)));
        };
        std::string v;
        if (l.rfind("XDS Current ", 0) == 0 && !(v = after("program name: ")).empty()) {
            bool found = false;
            for (auto& n : names) if (n.first == v) { n.second++; found = true; }
            if (!found) names.push_back({v, 1});
        } else if (!(v = after("network: ")).empty()) network = v;
        else if (!(v = after("call letters: ")).empty()) call = v.substr(0, v.find(','));
        else if (l.find("time of day ") != std::string::npos)
            sscanf(l.c_str() + l.find("time of day ") + 12, "%d:%d UTC, %d/%d/%d", &th, &tm, &tmo, &td, &ty);
    }
    std::string station = call.empty() ? network : call + (network.empty() || network == call ? "" : " (" + network + ")");
    for (auto& n : names) {
        if (n.first == "Name") continue;   // placeholder some encoders send
        out.push_back("XDS programme name: " + n.first + (station.empty() ? "" : "  on " + station) + "  (" + std::to_string(n.second) + " packets)");
    }
    // --- StarSight: the station's own channel at the broadcast time
    if (!r.db.showLists.empty()) {
        SsTime t = th >= 0 ? ssFromCivil(ty, tmo, td, th, tm) : r.db.nowTime();
        std::string tsrc = th >= 0 ? "XDS clock" : "StarSight clock";
        std::vector<int> want;
        std::string up;
        for (char c : call) up += (char)toupper((unsigned char)c);
        if (up.size() < 3) {
            // no call letters in XDS: a call sign in the file name (KCET_1994..., WVIZ December 1995)
            std::string fn = r.path.substr(r.path.find_last_of("/\\") + 1), tok;
            fn += ' ';
            for (char c : fn) {
                if (isalpha((unsigned char)c)) { tok += (char)toupper((unsigned char)c); continue; }
                if (tok.size() == 4 && (tok[0] == 'K' || tok[0] == 'W')) {
                    for (int id : r.db.channelIds()) {
                        std::string lab, cl, nm;
                        auto it = r.db.channels.find(id);
                        nm = it != r.db.channels.end() ? it->second.name : (ssKnownChannel(id, lab, cl) ? cl : "");
                        if (nm.rfind(tok, 0) == 0) { up = tok; break; }
                    }
                    if (!up.empty()) { tsrc += ", station " + up + " from the file name"; break; }
                }
                tok.clear();
            }
        }
        if (up.size() >= 3)
            for (int id : r.db.channelIds()) {
                std::string nm;
                auto it = r.db.channels.find(id);
                if (it != r.db.channels.end()) nm = it->second.name;
                std::string lab, cl;
                if (nm.empty() && ssKnownChannel(id, lab, cl)) nm = cl;
                std::string u;
                for (char c : nm) u += (char)toupper((unsigned char)c);
                // "WYY12" from a misconfigured encoder still finds WHYY by its first and last letters
                bool match = u.rfind(up.substr(0, 4), 0) == 0 || (u.size() >= 4 && up.size() >= 3 && u[0] == up[0] && u.find(up.substr(1, 2)) != std::string::npos);
                if (match) want.push_back(id);
            }
        for (int id : want) {
            auto v = r.db.slotsFor(id);
            for (size_t k = 0; k < v.size(); k++) {
                const SsSlot& s = v[k];
                if (s.start <= t && t < s.start + s.duration) {
                    int y, mo, d, h, mi, wd;
                    ssCivil(s.start + r.db.utcOffset(), y, mo, d, h, mi, wd);
                    char b[300];
                    std::string title = r.db.titleOf(s);
                    snprintf(b, sizeof b, "StarSight schedule: %s  %d/%d %d:%02d, %d min  (channel %d at the %s)",
                             title.empty() ? ("programme " + std::to_string(s.program)).c_str() : title.c_str(), mo, d, h, mi, s.duration, id,
                             tsrc.c_str());
                    out.push_back(b);
                    if (k + 1 < v.size()) {   // and what came on next
                        const SsSlot& n = v[k + 1];
                        ssCivil(n.start + r.db.utcOffset(), y, mo, d, h, mi, wd);
                        snprintf(b, sizeof b, "StarSight next: %s  %d:%02d, %d min", r.db.titleOf(n).c_str(), h, mi, n.duration);
                        out.push_back(b);
                    }
                }
            }
        }
    }
    // --- AMOL
    for (auto& a : r.amol)
        for (auto& run : a.runs) {
            std::string src = amolSourceName(run.first.sid);
            char b[200];
            snprintf(b, sizeof b, "AMOL: source %d%s, network clock %d/%d %d:%02d %s .. %d:%02d %s", run.first.sid, src.empty() ? "" : (" " + src).c_str(),
                     run.first.month, run.first.day, run.first.hour, run.first.minute, run.first.pm ? "PM" : "AM", run.last.hour, run.last.minute,
                     run.last.pm ? "PM" : "AM");
            out.push_back(b);
        }
    // --- Gemstar clock packets (type 7A): first and last
    {
        GemstarClock first, last;
        for (auto& g : r.gemstar)
            for (auto& p : g.packets) {
                GemstarClock c = gemstarClock(p);
                if (!c.valid) continue;
                if (!first.valid || c.frame < first.frame) first = c;
                if (!last.valid || c.frame > last.frame) last = c;
            }
        if (first.valid) {
            char b[200];
            snprintf(b, sizeof b, "Gemstar clock: %d/%d/%d %d:%02d .. %d:%02d (zone %d)", first.month, first.day, first.year, first.hour, first.minute,
                     last.hour, last.minute, first.zone);
            out.push_back(b);
        }
        // decrypted Type B guide data: programmes and the channels they are on
        size_t shows = 0;
        std::set<int> chans;
        std::string first1;
        for (auto& g : r.gemstar)
            for (auto& s : g.shows) {
                shows++, chans.insert(s.channel);
                if (first1.empty()) first1 = s.title;
            }
        if (shows) {
            char b[300];
            snprintf(b, sizeof b, "Gemstar guide: %zu programmes on %zu guide channels (first: %s)", shows, chans.size(), first1.c_str());
            out.push_back(b);
        }
    }
    // --- teletext service line
    if (!r.teletext.service.empty()) out.push_back("Teletext: " + r.teletext.service);
    if (th >= 0) {
        char b[120];
        snprintf(b, sizeof b, "XDS clock: %02d:%02d UTC %d/%d/%d", th, tm, tmo, td, ty);
        out.push_back(b);
    }
    return out;
}
