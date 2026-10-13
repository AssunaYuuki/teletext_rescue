// Silent Radio LED sign: packets, records, sign scripts and templates.
// Ported from the Teletext Rescue program (silent_radio.cpp).
#include "silentradio.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

static const char *LED_FONT =
    "0000000000" "00005f0000" "0007000700" "147f147f14" "242a7f2a12" "2313086462" "3649562050" "0008070300"
    "001c224100" "0041221c00" "2a1c7f1c2a" "08083e0808" "0080703000" "0808080808" "0000606000" "2010080402"
    "3e5149453e" "00427f4000" "7249494946" "2141494d33" "1814127f10" "2745454539" "3c4a494931" "4121110907"
    "3649494936" "464949291e" "0000140000" "0040340000" "0008142241" "1414141414" "0041221408" "0201590906"
    "3e415d594e" "7c1211127c" "7f49494936" "3e41414122" "7f4141413e" "7f49494941" "7f09090901" "3e41415173"
    "7f0808087f" "00417f4100" "2040413f01" "7f08142241" "7f40404040" "7f021c027f" "7f0408107f" "3e4141413e"
    "7f09090906" "3e4151215e" "7f09192946" "2649494932" "03017f0103" "3f4040403f" "1f2040201f" "3f4038403f"
    "6314081463" "0304780403" "61594d4543" "007f414141" "0204081020" "004141417f" "0402010204" "4040404040"
    "0003070800" "2054547840" "7f28444438" "3844444428" "384444287f" "3854545418" "00087e0902" "18a4a49c78"
    "7f08040478" "00447d4000" "2040403d00" "7f10284400" "00417f4000" "7c0478047c" "7c08040478" "3844444438"
    "fc18242418" "18242418fc" "7c08040408" "4854545424" "04043f4424" "3c4040207c" "1c2040201c" "3c4030403c"
    "4428102844" "4c9090907c" "4464544c44" "0008364100" "0000770000" "0041360800" "0201020402";

std::vector<int> srGlyph(char32_t ch)
{
    int c = (int)ch;
    if (c < 32 || c >= 127) c = 32;
    if (c == 32) return {0, 0};
    std::vector<int> g;
    for (int k = 0; k < 5; k++) {
        unsigned v;
        sscanf(LED_FONT + ((c - 32) * 5 + k) * 2, "%2x", &v);
        g.push_back((int)v);
    }
    while (!g.empty() && g.front() == 0) g.erase(g.begin());
    while (!g.empty() && g.back() == 0) g.pop_back();
    for (auto &x : g) x &= 0x7F;
    return g;
}

std::vector<uint16_t> srTextColumns(const std::string &s)
{
    std::vector<uint16_t> cols;
    for (unsigned char ch : s) {
        for (int g : srGlyph(ch)) cols.push_back((uint16_t)g);
        cols.push_back(0);
    }
    if (!cols.empty()) cols.pop_back();
    return cols;
}

// ------------------------------------------------------------- frames ---

double SrFrame::duration() const
{
    if (!scroll) return hold;
    size_t n = 0;
    for (auto &l : lines) n = std::max(n, l.first.size());
    return (SR_W + n) / speed;
}

std::vector<uint16_t> SrFrame::at(double t, const std::vector<uint16_t> &prev) const
{
    std::vector<uint16_t> out(SR_W, 0);
    if (!scroll) {
        for (int x = 0; x < SR_W && x < (int)cols.size(); x++) out[x] = cols[x];
        if (wipe && t < 0.4) {
            int k = (int)(t / 0.4 * SR_W);
            for (int x = k; x < SR_W && x < (int)prev.size(); x++) out[x] = prev[x];
        }
        return out;
    }
    int off = (int)(t * speed);
    for (auto &l : lines)
        for (int x = 0; x < SR_W; x++) {
            int k = x - SR_W + off;
            if (k >= 0 && k < (int)l.first.size()) out[x] |= (uint16_t)(l.first[k] << l.second);
        }
    return out;
}

namespace {

typedef std::vector<uint8_t> Bytes;

struct Screen {
    std::vector<uint16_t> cols = std::vector<uint16_t>(SR_W, 0);
    void put(int x, int col) { if (x >= 0 && x < SR_W) cols[x] |= col & 0x7FFF; }
    int text(int x, const std::string &s, int row)
    {
        for (int c : srTextColumns(s)) put(x++, c << row);
        return x;
    }
};

SrFrame still(const std::vector<uint16_t> &cols, double hold, const std::string &what = "")
{
    SrFrame f;
    f.cols = cols;
    f.hold = std::max(hold, 0.04);
    f.what = what;
    return f;
}

// ------------------------------------------------------------ packets ---
struct Packet {
    int seq = 0;
    size_t pos = 0;
    std::string zone;
    int copies = 0, intact = 0;
    Bytes body;
};

std::vector<Packet> packets(const Bytes &data, std::vector<std::pair<size_t, size_t>> &ranges)
{
    struct Raw { size_t at; Bytes q; };
    std::vector<Raw> raw;
    for (size_t p = 0; p + 13 <= data.size(); p++) {
        if (data[p] != 0xFF || data[p + 5] != 0 || data[p + 6] != 3 || data[p + 9] != 0xC1) continue;
        size_t L = (data[p + 3] << 8) | data[p + 4];
        size_t at = p;
        p += 12; // matches do not overlap
        if (L < 8 || L > 4096 || at + 13 + L > data.size()) continue;
        raw.push_back({at, Bytes(data.begin() + at, data.begin() + at + 13 + L)});
        ranges.push_back({at, at + 13 + L});
    }
    // copies of one packet: same sequence number, length and zone
    std::map<std::string, std::vector<const Raw *>> groups;
    std::vector<std::string> order;
    for (auto &r : raw) {
        if (r.q[12] < 0x80) continue;
        std::string key(r.q.begin() + 1, r.q.begin() + 5);
        key += std::string(r.q.begin() + 10, r.q.begin() + 12);
        if (!groups.count(key)) order.push_back(key);
        groups[key].push_back(&r);
    }
    std::vector<Packet> out;
    for (auto &key : order) {
        auto &cps = groups[key];
        std::vector<const Raw *> whole; // a clean packet ends in six zero bytes
        for (auto *c : cps) {
            bool z = c->q.size() >= 6;
            for (size_t i = c->q.size() - 6; z && i < c->q.size(); i++) z = c->q[i] == 0;
            if (z) whole.push_back(c);
        }
        auto &use = whole.empty() ? cps : whole;
        size_t n = SIZE_MAX;
        for (auto *c : use) n = std::min(n, c->q.size());
        Bytes voted(n);
        for (size_t i = 0; i < n; i++) {
            int cnt[256] = {0};
            for (auto *c : use) cnt[c->q[i]]++;
            int b = 0;
            for (int v = 1; v < 256; v++) if (cnt[v] > cnt[b]) b = v;
            voted[i] = (uint8_t)b;
        }
        Packet p;
        p.seq = (voted[1] << 8) | voted[2];
        p.pos = cps[0]->at;
        if (voted[10] == 2 && voted[11] == 3) p.zone = "A";
        else if (voted[10] == 4 && voted[11] == 5) p.zone = "B";
        else if (voted[10] == 6 && voted[11] == 7) p.zone = "C";
        else { char b[8]; snprintf(b, sizeof b, "%02X%02X", voted[10], voted[11]); p.zone = b; }
        p.copies = (int)cps.size();
        p.intact = (int)whole.size();
        if (voted.size() > 18) p.body = Bytes(voted.begin() + 16, voted.end() - 2);
        out.push_back(std::move(p));
    }
    return out;
}

// records in a packet body: 00 0C len(2, LE) script | 00 0A .. control |
// 00 <template> fields (02..09 = field number, text) 1F
struct Record { int kind = 0; Bytes script; int t = 0; std::map<int, std::string> fields; };

std::vector<Record> records(const Bytes &body)
{
    std::vector<Record> out;
    size_t i = 0, n = body.size();
    while (i < n) {
        if (body[i] != 0) { i++; continue; }
        if (i + 1 >= n) break;
        int t = body[i + 1];
        if (t == 0x0C && i + 3 < n) {
            size_t L = body[i + 2] | (body[i + 3] << 8);
            size_t e = std::min(n, i + 4 + L);
            Record r; r.kind = 0; r.script.assign(body.begin() + i + 4, body.begin() + e);
            out.push_back(std::move(r));
            i += 4 + L;
            continue;
        }
        if (t == 0x0A) { Record r; r.kind = 2; out.push_back(r); i += 5; continue; }
        if (t == 0) { i++; continue; }
        size_t j = i + 2;
        Record r; r.kind = 1; r.t = t;
        int cur = -1; std::string buf;
        while (j < n && body[j] != 0x1F) {
            int c = body[j];
            if (c >= 2 && c <= 9) { if (cur >= 0) r.fields[cur] = buf; cur = c; buf.clear(); }
            else if (c >= 32 && c < 127) buf += (char)c;
            j++;
        }
        if (cur >= 0) r.fields[cur] = buf;
        out.push_back(std::move(r));
        i = j + 1;
    }
    return out;
}

// A sign script: text, raw columns (bytes >= 80: 15-bit column), cursor
// moves, holds (1C, 1/256 s), clear / show commands (1F xx).
std::vector<SrFrame> runScript(const Bytes &s, std::vector<std::string> &texts)
{
    const double HOLD = 2.0;
    std::vector<SrFrame> out;
    std::vector<bool> timed;
    Screen sc;
    int x = 0, saved = 0, last = -1;
    bool dirty = false;
    std::string words;
    size_t i = 0, n = s.size();
    auto show = [&](int fx) {
        SrFrame f = still(sc.cols, 0, words);
        f.wipe = fx != 0x20;
        out.push_back(f);
        timed.push_back(false);
        last = (int)out.size() - 1;
        dirty = false;
        if (!words.empty()) texts.push_back(words);
        words.clear();
    };
    while (i < n) {
        int c = s[i];
        if (c >= 0x80 && i + 1 < n) { sc.put(x++, ((c & 0x7F) << 8) | s[i + 1]); i += 2; dirty = true; continue; }
        if (c >= 32 && c < 127) {
            std::string t;
            while (i < n && s[i] >= 32 && s[i] < 127) t += (char)s[i++];
            x = sc.text(x, t, 4);
            words += (words.empty() ? "" : " ") + t;
            dirty = true;
            continue;
        }
        if (c == 0x1C && i + 2 < n) {
            double hold = (s[i + 1] | (s[i + 2] << 8)) / 256.0;
            if (dirty || last < 0) show(0x20);
            out[last].hold = (timed[last] ? out[last].hold : 0) + hold;
            timed[last] = true;
            i += 3;
            continue;
        }
        if ((c == 0x1D || c == 0x0A) && i + 1 < n) { x = s[i + 1]; if (c == 0x1D) saved = x; i += 2; continue; }
        if (c == 0x11) { x = saved; i++; continue; }
        if (c == 0x1F && i + 1 < n) {
            int a = s[i + 1];
            if (a == 0x2C) { sc = Screen(); dirty = false; }
            else if (a == 0x32 || a == 0x38) { x = sc.text(x, a == 0x32 ? "12" : "00", 4); dirty = true; }
            else if (a == 0x33) { x = sc.text(x + 2, "PM", 4); dirty = true; }
            else if (a != 0x3F && a != 0x39 && dirty) show(a);
            i += 2;
            continue;
        }
        if (c == 0x05) { i += 7; continue; }
        if (c == 0x06 || c == 0x07 || c == 0x1B) { i += 2; continue; }
        i++;
    }
    if (dirty) show(0x20);
    for (size_t k = 0; k < out.size(); k++) {
        if (!timed[k]) out[k].hold = HOLD;
        out[k].hold = std::max(out[k].hold, 0.04);
    }
    return out;
}

SrFrame twoLines(const std::string &l1, const std::string *l2, double hold)
{
    std::vector<std::pair<std::string, int>> rows;
    if (l2) { rows.push_back({l1, 0}); rows.push_back({*l2, 8}); }
    else rows.push_back({l1, 4});
    size_t wide = 0;
    for (auto &r : rows) wide = std::max(wide, srTextColumns(r.first).size());
    std::string w;
    for (auto &r : rows) w += (w.empty() ? "" : " / ") + r.first;
    if (wide > SR_W) {
        SrFrame f;
        f.scroll = true;
        for (auto &r : rows) f.lines.push_back({srTextColumns(r.first), r.second});
        f.what = w;
        return f;
    }
    Screen sc;
    for (auto &r : rows) {
        auto c = srTextColumns(r.first);
        int x0 = (SR_W - (int)c.size()) / 2;
        for (size_t k = 0; k < c.size(); k++) sc.put(x0 + (int)k, c[k] << r.second);
    }
    return still(sc.cols, hold, w);
}

// templates: 1B = crawl, 30/31 = sports score (teams, scores, league,
// status), anything else = one or two centred lines
SrFrame templateFrame(int t, std::map<int, std::string> &f, std::string &league)
{
    auto get = [&](int k) { auto it = f.find(k); return it == f.end() ? std::string() : it->second; };
    if (t == 0x1B) {
        SrFrame fr;
        fr.scroll = true;
        fr.lines.push_back({srTextColumns("   " + get(2)), 4});
        fr.what = get(2);
        return fr;
    }
    if (t == 0x30 || t == 0x31) {
        std::string lg = !get(2).empty() ? get(2) : !get(3).empty() ? get(3) : !get(4).empty() ? get(4) : league;
        std::string l1 = get(5) + " " + get(7) + "  " + get(6) + " " + get(8), l2 = lg + "  " + get(9);
        league = lg;
        return twoLines(l1, &l2, 3.0);
    }
    std::string l1 = get(2);
    if (f.count(3)) { std::string l2 = f[3]; return twoLines(l1, &l2, 3.0); }
    return twoLines(l1, nullptr, 3.0);
}

// Without packets: printable runs between the binary parts.
void looseText(const Bytes &bytes, const std::vector<int64_t> &frames, std::vector<SilentRadioMessage> &out)
{
    std::string cur;
    int64_t start = 0;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
        if (a != std::string::npos) {
            std::string s = cur.substr(a, b - a + 1);
            int letters = 0;
            for (char c : s) letters += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (s.size() >= 6 && letters * 2 >= (int)s.size() && (out.empty() || out.back().text != s)) {
                SilentRadioMessage m;
                m.frame = start;
                m.text = s;
                m.texts.push_back(s);
                m.frames.push_back(twoLines(s, nullptr, 3.0));
                out.push_back(std::move(m));
            }
        }
        cur.clear();
    };
    for (size_t i = 0; i < bytes.size(); i++) {
        uint8_t c = bytes[i];
        if (c >= 0x20 && c < 0x7F) {
            if (cur.empty()) start = frames.empty() ? 0 : frames[std::min(i / 2, frames.size() - 1)];
            cur += (char)c;
        } else if ((c == 0x0D || c == 0x0A) && !cur.empty()) {
            cur += ' ';
        } else {
            flush();
        }
    }
    flush();
}

} // namespace

void srDecode(const std::vector<uint8_t> &bytes, const std::vector<int64_t> &frames,
              std::vector<SilentRadioMessage> &out)
{
    size_t before = out.size();
    std::vector<std::pair<size_t, size_t>> ranges;
    for (auto &p : packets(bytes, ranges)) {
        if (p.body.size() < 4) continue;
        SilentRadioMessage m;
        std::string league;
        for (auto &r : records(p.body)) {
            if (r.kind == 0) {
                auto f = runScript(r.script, m.texts);
                m.frames.insert(m.frames.end(), f.begin(), f.end());
            } else if (r.kind == 1) {
                m.frames.push_back(templateFrame(r.t, r.fields, league));
                std::string t;
                for (auto &kv : r.fields)
                    if (!kv.second.empty()) t += (t.empty() ? "" : " / ") + kv.second;
                if (!t.empty()) m.texts.push_back(t);
            }
        }
        if (m.frames.empty()) continue;
        // drop crumbs: stray bytes that happen to be printable
        m.texts.erase(std::remove_if(m.texts.begin(), m.texts.end(), [](const std::string &t) {
            int an = 0;
            for (char c : t) an += isalnum((unsigned char)c) != 0;
            return an < 2;
        }), m.texts.end());
        for (auto &t : m.texts) m.text += (m.text.empty() ? "" : " \xC2\xB7 ") + t;
        if (m.text.empty()) m.text = "(picture)";
        m.zone = p.zone;
        m.seq = p.seq;
        m.copies = p.copies;
        m.intact = p.intact;
        m.frame = frames.empty() ? 0 : frames[std::min(p.pos / 2, frames.size() - 1)];
        out.push_back(std::move(m));
    }
    // text outside the packets that were found (packets with a damaged header)
    Bytes rest = bytes;
    for (auto &r : ranges)
        for (size_t i = r.first; i < r.second && i < rest.size(); i++) rest[i] = 0;
    size_t packed = out.size();
    looseText(rest, frames, out);
    // a damaged copy of a packet that was also received whole adds nothing
    out.erase(std::remove_if(out.begin() + packed, out.end(), [&](const SilentRadioMessage &m) {
        for (size_t k = before; k < packed; k++)
            if (out[k].text.find(m.text) != std::string::npos) return true;
        return false;
    }), out.end());
    std::stable_sort(out.begin() + before, out.end(),
                     [](const SilentRadioMessage &a, const SilentRadioMessage &b) { return a.frame < b.frame; });
}
