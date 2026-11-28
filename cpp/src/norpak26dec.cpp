// ТВ-информ 1998, Norpak-26 container: files deciphered (see norpak26dec.h). A port of test\scripts\decode_all.py
// (the same text, byte for byte, for the same key and phase) with the key, language, documents and phase found here.
#include "norpak26dec.h"
#include "norpak26_model.inc"
#include "arjcheck.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>

namespace {
using B = std::vector<uint8_t>;
const uint8_t GAP = 0x07;   // place of 50 cut-out bytes, "[…]" in the text
const int W = 20;           // context that has to agree, before and after, to fill a gap from another file

// ------------------------------------------------------------------ CP866 characters (as Python sees them)
bool isRu(uint8_t c) { return (c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xF1); }   // А-Я а-я Ё ё
bool isRuAz(uint8_t c) { return (c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xEF); } // А-Я а-я (no Ё)
bool isRuLow(uint8_t c) { return (c >= 0xA0 && c <= 0xAF) || (c >= 0xE0 && c <= 0xEF); }
bool isLat(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDig(uint8_t c) { return c >= '0' && c <= '9'; }
bool isAlpha(uint8_t c) { return isLat(c) || (c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xF7); }
bool isWord(uint8_t c) { return isLat(c) || isRu(c); }                                      // [A-Za-zА-Яа-яЁё]
bool allowed(uint8_t c)
{
    if (isRu(c) || isLat(c) || isDig(c) || c == 0xFC) return true;   // 0xFC = №
    return c && strchr(" \n\t.,-:;!?()\"'%/+*=<>[]_#@$&", c) != nullptr;
}
uint8_t lowerCp(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c + 32);
    if (c >= 0x80 && c <= 0x8F) return (uint8_t)(c + 0x20);
    if (c >= 0x90 && c <= 0x9F) return (uint8_t)(c + 0x50);
    if (c >= 0xF0 && c <= 0xF7 && !(c & 1)) return (uint8_t)(c + 1);
    return c;
}
std::string utf8(uint8_t c)
{
    if (c == GAP) return "[\xE2\x80\xA6]";
    if (c < 0x80) return c >= 0x20 || c == '\t' || c == '\n' ? std::string(1, (char)c) : std::string("?");
    if (c <= 0xAF) return from_cp(0x410 + (c - 0x80));
    if (c >= 0xE0 && c <= 0xEF) return from_cp(0x440 + (c - 0xE0));
    if (c == 0xF0) return from_cp(0x401);
    if (c == 0xF1) return from_cp(0x451);
    if (c == 0xFC) return from_cp(0x2116);
    return "?";
}
std::string utf8(const std::string& s) { std::string r; for (unsigned char c : s) r += utf8(c); return r; }
// UTF-8 text to CP866 (for the keys of the table of contents)
std::string cp866(const char* u)
{
    std::string r;
    for (char32_t c : to_u32(u)) {
        if (c < 0x80) r += (char)c;
        else if (c >= 0x410 && c <= 0x43F) r += (char)(0x80 + (c - 0x410));
        else if (c >= 0x440 && c <= 0x44F) r += (char)(0xE0 + (c - 0x440));
        else if (c == 0x401) r += (char)0xF0;
        else if (c == 0x451) r += (char)0xF1;
        else r += '?';
    }
    return r;
}

inline uint8_t dec(uint8_t v, int k) { return (uint8_t)((kN26Sinv[v] - k) & 255); }

// ------------------------------------------------------------------ language models (norpak26_model.inc)
struct Lm { const uint8_t* q; const uint8_t* cls; int N; double base; };
const Lm LM[2] = {{kN26LmRu, kN26ClsRu, kN26NRu, kN26BaseRu}, {kN26LmEn, kN26ClsEn, kN26NEn, kN26BaseEn}};
const char* LANG[2] = {"ru", "en"};

double lmMean(const B& y, int k, int L)
{
    const Lm& m = LM[L];
    if (y.size() < 3) return -1e9;
    int a0 = m.cls[dec(y[0], k)], a1 = m.cls[dec(y[1], k)];
    int64_t s = 0;
    for (size_t i = 2; i < y.size(); i++) {
        int a2 = m.cls[dec(y[i], k)];
        s += m.q[(a0 * m.N + a1) * m.N + a2];
        a0 = a1; a1 = a2;
    }
    return -(double)s / (double)(y.size() - 2);
}
struct Key { double s = -1e9; int k = 0, lang = 0; double norm() const { return s - LM[lang].base; } };
// the best key for each language; the language whose best decoding is nearest its own typical text
Key bestKey(const B& y)
{
    Key r[2];
    for (int L = 0; L < 2; L++)
        for (int k = 0; k < 256; k++) {
            double s = lmMean(y, k, L);
            if (s > r[L].s) { r[L].s = s; r[L].k = k; r[L].lang = L; }
        }
    return r[0].norm() >= r[1].norm() ? r[0] : r[1];
}
const std::unordered_set<std::string>& dict()
{
    static std::unordered_set<std::string> d;
    if (d.empty()) {
        size_t a = 0;
        for (size_t i = 0; i < sizeof(kN26Dict); i++)
            if (kN26Dict[i] == '\n') { d.insert(std::string((const char*)kN26Dict + a, i - a)); a = i + 1; }
    }
    return d;
}

// ------------------------------------------------------------------ container
struct Cell { size_t a = 0; B pl; int ok = -1; };   // ok: 1 sum right, 0 wrong, -1 cell cut by the file's end
std::vector<Cell> cells(const B& b, int& phase)
{
    int bestG = -1, bestP = 0;
    for (int p = 0; p < 26; p++) {
        int good = 0;
        for (size_t s = p; s < b.size(); s += 26) {
            if (s < 25) continue;
            unsigned sum = 0; bool any = false;
            for (size_t q = s - 25; q < s; q++) { sum += b[q]; any |= b[q] != 0; }
            if (any && (sum & 255) == b[s]) good++;
        }
        if (good >= bestG) { bestG = good; bestP = p; }   // as Python max((good, p)): the later phase on a tie
    }
    phase = bestP;
    std::vector<Cell> out;
    for (long st = (long)bestP - 25; st < (long)b.size(); st += 26) {
        Cell c;
        size_t a = (size_t)std::max(st, 0L), e = (size_t)(st + 25);
        c.a = a;
        c.pl.assign(b.begin() + a, b.begin() + std::min(e, b.size()));
        if (st >= 0 && e < b.size()) {
            unsigned sum = 0;
            for (uint8_t x : c.pl) sum += x;
            c.ok = (sum & 255) == b[e] ? 1 : 0;
        }
        out.push_back(std::move(c));
    }
    return out;
}
bool isHdr(const B& c) { return c.size() >= 6 && c[0] == 1 && c[1] == 1 && c[3] == 0 && c[5] <= 1 && (c[5] * 256 + c[4]) <= 0x188; }
bool isFrame0(const Cell& c) { return c.ok == 1 && isHdr(c.pl) && c.pl.size() >= 8 && c.pl[2] == 0 && c.pl[6] == 0x37 && c.pl[7] == 0x0C; }

struct Seg { int cell = 0, k = 0, lang = 0; };
int segAt(const std::vector<Seg>& sg, int c)
{
    size_t r = 0;
    for (size_t i = 0; i < sg.size(); i++) if (sg[i].cell <= c) r = i;
    return (int)r;
}

// data of the good cells without frame headers, trailers and frame 0; ci = cell of every byte. istext: when the 3 bytes
// before a header are letters of the text, the trailer fell out with the cut and they are kept
template <class F>
void stream(const std::vector<Cell>& cs, int phase, F istext, bool useText, B& data, std::vector<int>& ci)
{
    data.clear(); ci.clear();
    std::vector<std::pair<size_t, int>> hdrs;   // data position, cell
    for (size_t i = 0; i < cs.size(); i++) {
        if (cs[i].ok == 0) continue;
        if (isHdr(cs[i].pl)) hdrs.push_back({data.size(), (int)i});
        data.insert(data.end(), cs[i].pl.begin(), cs[i].pl.end());
        ci.insert(ci.end(), cs[i].pl.size(), (int)i);
    }
    std::vector<char> keep(data.size(), 1);
    for (size_t j = 0; j < hdrs.size(); j++) {
        size_t pos = hdrs[j].first;
        const B& c = cs[hdrs[j].second].pl;
        int tr = 2;
        bool txt = useText && pos >= 3 && istext(&data[pos - 3], ci[pos - 1]);
        if (phase >= 0) txt = txt && ci[pos - 1] % 14 == phase && ci[pos] == ci[pos - 1] + 1;
        if (txt) tr = 0;
        if (c.size() >= 8 && c[6] == 0x37 && c[7] == 0x0C && c[2] == 0) {
            size_t end = j + 1 < hdrs.size() ? hdrs[j + 1].first - 2 : data.size();
            for (size_t q = pos >= 2 ? pos - 2 : 0; q < std::min(std::max(end, pos + 6), data.size()); q++) keep[q] = 0;
        } else
            for (size_t q = pos >= (size_t)tr ? pos - tr : 0; q < std::min(data.size(), pos + 6); q++) keep[q] = 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < data.size(); i++)
        if (keep[i]) { data[w] = data[i]; ci[w] = ci[i]; w++; }
    data.resize(w); ci.resize(w);
}

// ------------------------------------------------------------------ text
std::string toText(const B& data, const std::vector<int>& ci, const std::vector<Seg>& sg, int ph)
{
    std::vector<std::string> chunks;
    std::string cur;
    size_t n = data.size();
    for (size_t i = 0; i < n; i++) {
        uint8_t v = data[i];
        if (v == 0 && ((i > 0 && data[i - 1] == 0) || (i + 1 < n && data[i + 1] == 0))) {   // 2+ zeros: padding
            if (!cur.empty()) { chunks.push_back(cur); cur.clear(); }
            continue;
        }
        if (ph >= 0 && !cur.empty() && ci[i] > ci[i - 1] && ci[i - 1] % 14 == ph) cur += (char)GAP;
        uint8_t c = dec(v, sg[segAt(sg, ci[i])].k);
        cur += (char)(c == GAP ? 0x01 : c);   // a data byte 07 is no gap mark (dropped below as any control code)
    }
    if (!cur.empty()) chunks.push_back(cur);
    std::vector<std::string> keep;
    for (auto& t : chunks) {
        size_t len = 0, good = 0, let = 0;
        for (unsigned char c : t) {
            if (c == GAP) continue;
            len++; good += allowed(c) || c == '\r'; let += isAlpha(c);
        }
        if (!len) continue;
        double g = (double)good / len, l = (double)let / len;
        if ((len >= 12 && g > 0.9 && l > 0.35) || (len < 12 && good == len && l > 0.3 && !keep.empty())) keep.push_back(t);
    }
    std::string s;
    for (size_t i = 0; i < keep.size(); i++) s += (i ? "\n" : "") + keep[i];
    std::string u;
    for (size_t i = 0; i < s.size(); i++) {   // \r\n and \r -> \n; only allowed characters
        unsigned char c = s[i];
        if (c == '\r') { u += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') i++; continue; }
        if (allowed(c) || c == GAP) u += (char)c;
    }
    std::string t2;   // [ \t]+\n -> \n, then \n{3,} -> \n\n (two passes, as the regexes)
    for (size_t i = 0; i < u.size(); i++) {
        if (u[i] == ' ' || u[i] == '\t') {
            size_t j = i;
            while (j < u.size() && (u[j] == ' ' || u[j] == '\t')) j++;
            if (j < u.size() && u[j] == '\n') { i = j - 1; continue; }
            t2.append(u, i, j - i); i = j - 1; continue;
        }
        t2 += u[i];
    }
    std::string v;
    for (size_t i = 0; i < t2.size(); i++) {
        if (t2[i] == '\n') {
            size_t j = i;
            while (j < t2.size() && t2[j] == '\n') j++;
            v.append(std::min<size_t>(j - i, 2), '\n'); i = j - 1; continue;
        }
        v += t2[i];
    }
    size_t a = 0, b = v.size();
    while (a < b && (v[a] == '\n' || v[a] == (char)GAP)) a++;
    while (b > a && (v[b - 1] == '\n' || v[b - 1] == (char)GAP)) b--;
    return v.substr(a, b - a) + "\n";
}

// Latin letters inside Russian words (the sender's typing): to Cyrillic
std::string fixlat(std::string s)
{
    static const char* from = "aceopxykABCEHKMOPTX";
    static const uint8_t to[] = {0xA0, 0xE1, 0xA5, 0xAE, 0xE0, 0xE5, 0xE3, 0xAA, 0x80, 0x82, 0x91, 0x85, 0x8D, 0x8A, 0x8C, 0x8E, 0x90, 0x92, 0x95};
    for (size_t i = 0; i < s.size();) {
        if (!isWord((uint8_t)s[i])) { i++; continue; }
        size_t j = i;
        int cy = 0; bool la = false, onlyLike = true;
        while (j < s.size() && isWord((uint8_t)s[j])) {
            uint8_t c = (uint8_t)s[j];
            if (isRu(c)) cy++;
            else { la = true; onlyLike &= strchr(from, c) != nullptr; }
            j++;
        }
        if (cy >= 2 && la && onlyLike)
            for (size_t q = i; q < j; q++)
                if (isLat((uint8_t)s[q])) s[q] = (char)to[strchr(from, s[q]) - from];
        i = j;
    }
    return s;
}
// a line of scraps where a damaged place was: no normal word at all
std::string dropGarbage(const std::string& t, int& removed)
{
    auto badToken = [](const std::string& w) {
        bool letter = false, digit = false, cyr = false, lat = false;
        for (unsigned char c : w) { letter |= isWord(c); digit |= isDig(c); cyr |= isRu(c); lat |= isLat(c); }
        if (letter && digit) {
            // \d+[a-zA-Zа-яА-Я]{1,3} or [A-Za-zА-Яа-я]{1,4}\d+
            size_t d = 0; while (d < w.size() && isDig((uint8_t)w[d])) d++;
            bool ok1 = d >= 1 && w.size() - d >= 1 && w.size() - d <= 3;
            for (size_t q = d; q < w.size() && ok1; q++) ok1 = isLat((uint8_t)w[q]) || isRuAz((uint8_t)w[q]);
            size_t l = 0; while (l < w.size() && (isLat((uint8_t)w[l]) || isRuAz((uint8_t)w[l]))) l++;
            bool ok2 = l >= 1 && l <= 4 && l < w.size();
            for (size_t q = l; q < w.size() && ok2; q++) ok2 = isDig((uint8_t)w[q]);
            if (!ok1 && !ok2) return true;
        }
        return cyr && lat;
    };
    std::string out;
    size_t a = 0;
    bool first = true;
    while (a <= t.size()) {
        size_t e = t.find('\n', a);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(a, e - a);
        std::vector<std::string> toks;
        for (size_t i = 0; i < line.size();) {
            uint8_t c = (uint8_t)line[i];
            if (!(isWord(c) || isDig(c))) { i++; continue; }
            size_t j = i; while (j < line.size() && (isWord((uint8_t)line[j]) || isDig((uint8_t)line[j]))) j++;
            toks.push_back(line.substr(i, j - i)); i = j;
        }
        int nb = 0, good = 0;
        for (auto& w : toks) {
            nb += badToken(w);
            bool allC = true, allL = true, allD = true;
            for (unsigned char c : w) { allC &= isRu(c); allL &= isLat(c); allD &= isDig(c); }
            good += (w.size() >= 2 && (allC || allL)) || allD;
        }
        bool run5 = false;
        for (size_t i = 0, rc = 0, rl = 0; i < line.size() && !run5; i++) {
            uint8_t c = (uint8_t)line[i];
            rc = isRu(c) ? rc + 1 : 0; rl = isLat(c) ? rl + 1 : 0;
            run5 = rc >= 5 || rl >= 5;
        }
        if (!toks.empty() && nb >= 1 && !good && !run5) removed++;
        else { out += (first ? "" : "\n") + line; first = false; }
        if (e == t.size()) break;
        a = e + 1;
    }
    return out;
}

double med(std::vector<double> v) { std::sort(v.begin(), v.end()); size_t n = v.size(); return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2; }
double sd(const std::vector<double>& v) { double m = 0; for (double x : v) m += x; m /= v.size(); double s = 0; for (double x : v) s += (x - m) * (x - m); return std::sqrt(s / v.size()); }

uint32_t crc32(const uint8_t* p, size_t n)
{
    static uint32_t T[256];
    if (!T[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = T[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------ one file
struct Work {
    const Norpak26Input* in = nullptr;
    Norpak26Result r;
    std::vector<Cell> cs;
    int cellPhase = 0;
    B data; std::vector<int> ci;   // stream without phase
    std::vector<Seg> sg;
    double norm = -1e9;
    int poff = -1;
    std::string base;              // the text without gap marks: what other files' gaps are filled from
    bool text = false;
    bool istext(const uint8_t* b, int c) const
    {
        const Seg& s = sg[segAt(sg, c)];
        for (int i = 0; i < 3; i++) {
            uint8_t x = dec(b[i], s.k);
            if (s.lang == 0 ? !isRuLow(x) : !(x >= 'a' && x <= 'z')) return false;
        }
        return true;
    }
    void streamAt(int phase, B& d, std::vector<int>& c) const
    {
        stream(cs, phase, [this](const uint8_t* b, int cc) { return istext(b, cc); }, true, d, c);
    }
};

// key and language of every document of the file (border = frame 0)
void findKeys(Work& w)
{
    std::vector<int> bounds{0};
    for (size_t i = 0; i < w.cs.size(); i++) if (isFrame0(w.cs[i]) && i) bounds.push_back((int)i);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    struct S { int cell, k = -1, lang = 0; size_t n = 0; double norm = -1e9; };
    std::vector<S> segs;
    for (size_t j = 0; j < bounds.size(); j++) {
        int b0 = bounds[j], b1 = j + 1 < bounds.size() ? bounds[j + 1] : INT32_MAX;
        B y;
        for (size_t i = 0; i < w.data.size(); i++) if (w.ci[i] >= b0 && w.ci[i] < b1 && w.data[i]) y.push_back(w.data[i]);
        S s; s.cell = b0; s.n = y.size();
        if (y.size() >= 60) { Key k = bestKey(y); s.k = k.k; s.lang = k.lang; s.norm = k.norm(); }
        segs.push_back(s);
    }
    const S* main = nullptr;
    for (auto& s : segs) if (s.k >= 0 && (!main || s.n > main->n)) main = &s;
    S whole;
    if (!main) {
        B y;
        for (uint8_t v : w.data) if (v) y.push_back(v);
        Key k = bestKey(y);
        whole.cell = 0; whole.k = k.k; whole.lang = k.lang; whole.n = y.size(); whole.norm = k.norm();
        segs = {whole}; main = &segs[0];
    }
    S m = *main;
    w.sg.clear();
    for (auto& s : segs) {
        Seg g{s.cell, s.k >= 0 ? s.k : m.k, s.k >= 0 ? s.lang : m.lang};
        if (!w.sg.empty() && w.sg.back().k == g.k && w.sg.back().lang == g.lang) continue;
        w.sg.push_back(g);
    }
    w.r.k = m.k; w.r.lang = LANG[m.lang]; w.norm = m.norm;
    if (w.sg.size() > 1) for (auto& g : w.sg) w.r.segments += fmt("%s%d:%d:%s", w.r.segments.empty() ? "" : " ", g.cell, g.k, LANG[g.lang]);
}

// phase of the cut-out bytes from the text: at that phase the junctions of cells read worst (3-gram model) and the
// words across them are not words (dictionary). comb: per phase, how much worse than the others (z-scores)
bool phaseScores(const Work& w, std::array<double, 14>& comb)
{
    const Lm& LMm = LM[w.r.lang == "en"];   // the model of the main document
    const auto& D = dict();
    std::array<double, 14> lm{}, cnt{}, bad{}, tot{};
    const B& y = w.data;
    const auto& ci = w.ci;
    size_t n = y.size();
    std::vector<uint8_t> ch(n);
    for (size_t i = 0; i < n; i++) ch[i] = y[i] ? dec(y[i], w.sg[segAt(w.sg, ci[i])].k) : 0;
    for (size_t b = 2; b + 3 < n; b++) {
        if (ci[b + 1] != ci[b] + 1) continue;
        int p = ci[b] % 14;
        bool zero = false;
        for (size_t q = b - 2; q < b + 4; q++) zero |= y[q] == 0;
        if (!zero) {
            int k = w.sg[segAt(w.sg, ci[b])].k, a[6];
            for (int q = 0; q < 6; q++) a[q] = LMm.cls[dec(y[b - 2 + q], k)];
            for (int q = 0; q + 2 < 6; q++) lm[p] -= LMm.q[(a[q] * LMm.N + a[q + 1]) * LMm.N + a[q + 2]];
            cnt[p]++;
        }
        if (isAlpha(ch[b]) && isAlpha(ch[b + 1])) {
            size_t i = b, j = b + 1;
            while (i > 0 && isAlpha(ch[i - 1])) i--;
            while (j + 1 < n && isAlpha(ch[j + 1])) j++;
            if (j - i + 1 >= 3) {
                std::string word;
                for (size_t q = i; q <= j; q++) word += (char)lowerCp(ch[q]);
                tot[p]++; bad[p] += !D.count(word);
            }
        }
    }
    int phases = 0;
    for (double c : cnt) phases += c > 0;
    if (phases < 10) return false;
    std::vector<double> m(14), have, fr(14);
    for (int p = 0; p < 14; p++) { m[p] = lm[p] / std::max(cnt[p], 1.0); if (cnt[p] > 0) have.push_back(m[p]); fr[p] = bad[p] / std::max(tot[p], 1.0); }
    double mh = med(have);
    for (int p = 0; p < 14; p++) if (cnt[p] == 0) m[p] = mh;
    double mm = med(m), sm = sd(m), mf = med(fr), sf = sd(fr);
    for (int p = 0; p < 14; p++) comb[p] = -(m[p] - mm) / (sm + 1e-9) + (fr[p] - mf) / (sf + 1e-9);
    return true;
}

// ------------------------------------------------------------------ gaps filled from duplicates (one news item in several files)
struct FillIndex {
    struct E { uint64_t h; uint32_t f, i; };
    std::vector<E> e;
    const std::vector<const std::string*>* texts = nullptr;
    static uint64_t hash(const char* p) { uint64_t h = 1469598103934665603ULL; for (int i = 0; i < W; i++) { h ^= (uint8_t)p[i]; h *= 1099511628211ULL; } return h; }
    void build(const std::vector<const std::string*>& t)
    {
        texts = &t;
        for (uint32_t f = 0; f < t.size(); f++)
            for (size_t i = 0; i + W < t[f]->size(); i++) e.push_back({hash(t[f]->data() + i), f, (uint32_t)i});
        std::sort(e.begin(), e.end(), [](const E& a, const E& b) { return a.h != b.h ? a.h < b.h : a.f != b.f ? a.f < b.f : a.i < b.i; });
    }
    template <class F> void each(const std::string& key, F fn) const
    {
        uint64_t h = hash(key.data());
        auto it = std::lower_bound(e.begin(), e.end(), h, [](const E& a, uint64_t v) { return a.h < v; });
        for (; it != e.end() && it->h == h; ++it)
            if ((*texts)[it->f]->compare(it->i, W, key) == 0 && fn(it->f, it->i)) return;
    }
};
int alphaCount(const std::string& s) { int n = 0; for (unsigned char c : s) n += isAlpha(c); return n; }
struct FillLog { std::string after, inserted; uint32_t from; };
std::string tryFill(const FillIndex& ix, uint32_t self, const std::string& t, std::vector<FillLog>& log)
{
    const auto& T = *ix.texts;
    std::vector<std::string> parts;
    size_t a = 0;
    for (size_t i = 0; i <= t.size(); i++)
        if (i == t.size() || t[i] == (char)GAP) { parts.push_back(t.substr(a, i - a)); a = i + 1; }
    std::string out = parts[0];
    for (size_t j = 1; j < parts.size(); j++) {
        std::string left = out.size() >= (size_t)W ? out.substr(out.size() - W) : out;
        std::string right = parts[j].substr(0, W);
        bool done = false;
        if (left.size() == (size_t)W && right.size() == (size_t)W && alphaCount(left) >= 10 && alphaCount(right) >= 10)
            ix.each(left, [&](uint32_t m, uint32_t q) {
                if (m == self) return false;
                const std::string& b = *T[m];
                size_t s = q + W, from = s + 30, to = std::min(b.size(), s + 56 + W);
                size_t r = std::string::npos;
                for (size_t p = from; p + W <= to && r == std::string::npos; p++)
                    if (b.compare(p, W, right) == 0) r = p;
                if (r == std::string::npos) return false;
                std::string mid = b.substr(s, r - s);
                size_t nl = std::count(mid.begin(), mid.end(), '\n');
                if (mid.size() + nl < 44 || mid.size() + nl > 52 || mid.find("\n\n") != std::string::npos) return false;
                out += mid; log.push_back({left, mid, m}); done = true;
                return true;
            });
        if (!done) out += (char)GAP;
        out += parts[j];
    }
    return out;
}

// ------------------------------------------------------------------ table of contents
std::string sourceOf(const std::string& name, const std::string& firstUtf8)
{
    if (name.rfind("DR44", 0) == 0) return "Daily Review";
    if (name.rfind("N039", 0) == 0) return "\xD0\x9D\xD0\xB5\xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x81\xD0\xB8\xD0\xBC\xD0\xB0\xD1\x8F \xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB5\xD1\x82\xD0\xB0";
    if (name.rfind("V009", 0) == 0) return "\xD0\x9D\xD0\xB5\xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x81\xD0\xB8\xD0\xBC\xD0\xB0\xD1\x8F \xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB5\xD1\x82\xD0\xB0 (\xD0\x9D\xD0\x92\xD0\x9E)";
    static const char* S[][2] = {
        {"ROS_GAZ", "\xD0\xA0\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F \xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB5\xD1\x82\xD0\xB0"},
        {"TODAY", "\xD0\xA1\xD0\xB5\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xBD\xD1\x8F"},
        {"\xD0\x9C\xD0\x9E\xD0\xA1\xD0\x9A\xD0\x9E\xD0\x92\xD0\xA1\xD0\x9A\xD0\x98\xD0\x95 \xD0\x9D\xD0\x9E\xD0\x92\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x98",
         "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB5 \xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8"},
        {"DIP_MID", "\xD0\x9E\xD0\xB1\xD0\xB7\xD0\xBE\xD1\x80 \xD0\xBF\xD0\xB5\xD1\x87\xD0\xB0\xD1\x82\xD0\xB8 \xD0\x94\xD0\x98\xD0\x9F \xD0\x9C\xD0\x98\xD0\x94 \xD0\xA0\xD0\xA4"},
        {"RUSSICA", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xB8\xD0\xBA\xD0\xB0: Russian Press Digest"},
        {"RIA", "\xD0\xA0\xD0\x98\xD0\x90 \xD0\x9D\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8"},
        {"NEZ-GAZ", "\xD0\x9D\xD0\xB5\xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x81\xD0\xB8\xD0\xBC\xD0\xB0\xD1\x8F \xD0\xB3\xD0\xB0\xD0\xB7\xD0\xB5\xD1\x82\xD0\xB0"},
        {"DSNG", "\xD0\x9C\xD0\xB0\xD1\x82\xD0\xB5\xD1\x80\xD0\xB8\xD0\xB0\xD0\xBB\xD1\x8B 1 \xD0\x94\xD0\xA1\xD0\x9D\xD0\x93"},
        {"IKM", "\xD0\x98\xD0\xBD\xD1\x84\xD0\xBE\xD1\x80\xD0\xBC.-\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x81\xD1\x83\xD0\xBB\xD1\x8C\xD1\x82. \xD0\xBC\xD0\xB0\xD1\x82\xD0\xB5\xD1\x80\xD0\xB8\xD0\xB0\xD0\xBB\xD1\x8B"}};
    for (auto& s : S) if (firstUtf8.find(s[0]) != std::string::npos) return s[1];
    if (name.rfind("000", 0) == 0 || name.rfind("0177", 0) == 0 || name.rfind("RV98", 0) == 0) return "\xD0\x98\xD0\xA2\xD0\x90\xD0\xA0-\xD0\xA2\xD0\x90\xD0\xA1\xD0\xA1";
    return "";
}
// title: "Название:" line, an upper-case line with a dash among the first 6, else a line with 12+ letters
std::string titleOf(const std::vector<std::string>& linesCp)
{
    std::vector<std::string> L;
    for (auto& l : linesCp) {
        size_t a = l.find_first_not_of(" \t"), b = l.find_last_not_of(" \t");
        if (a != std::string::npos) L.push_back(l.substr(a, b - a + 1));
    }
    const std::string nazv = cp866("\xD0\x9D\xD0\xB0\xD0\xB7\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5:");
    for (auto& l : L)
        if (l.rfind(nazv, 0) == 0) {
            std::string t = l.substr(nazv.size());
            size_t a = t.find_first_not_of(' ');
            if (a != std::string::npos && t.size() - a > 2) return utf8(t.substr(a));
        }
    for (size_t i = 0; i < L.size() && i < 6; i++) {
        const std::string& l = L[i];
        bool up = l.size() >= 6 && l.find('-') != std::string::npos;
        for (unsigned char c : l) up &= (c >= 0x80 && c <= 0x9F) || c == 0xF0 || (c >= 'A' && c <= 'Z') || isDig(c) || c == '-' || c == ' ';
        if (up) return utf8(l);
    }
    const std::string gaz = cp866("\xD0\x93\xD0\xB0\xD0\xB7\xD0\xB5\xD1\x82\xD0\xB0");
    for (size_t i = 1; i < L.size() && i < 8; i++) {
        int let = 0;
        for (unsigned char c : L[i]) let += isRuAz(c) || isLat(c);
        if (let >= 12 && L[i].find(gaz) == std::string::npos && L[i].find("ROS_GAZ") == std::string::npos) return utf8(L[i].substr(0, 100));
    }
    return L.empty() ? "" : utf8(L[0].substr(0, 100));
}
std::string pad(const std::string& s, size_t w)
{
    size_t n = to_u32(s).size();
    return n >= w ? s : s + std::string(w - n, ' ');
}
std::string safeName(std::string s)
{
    for (char& c : s) if ((unsigned char)c < 32 || strchr("/\\:*?\"<>|", c)) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "noname" : s;
}

// ------------------------------------------------------------------ ARJ
struct Hit { size_t pos; int k, type; std::string name; };
std::vector<Hit> arjHits(const B& data)
{
    std::vector<Hit> h;
    B b(data.size());
    for (int k = 0; k < 256; k++) {
        for (size_t i = 0; i < data.size(); i++) b[i] = dec(data[i], k);
        for (size_t i = 0; i + 4 < b.size(); i++) {
            if (b[i] != 0x60 || b[i + 1] != 0xEA) continue;
            size_t sz = b[i + 2] | b[i + 3] << 8;
            if (sz < 20 || sz > 2600 || i + 8 + sz > b.size()) continue;
            uint32_t want = b[i + 4 + sz] | b[i + 5 + sz] << 8 | b[i + 6 + sz] << 16 | (uint32_t)b[i + 7 + sz] << 24;
            if (crc32(&b[i + 4], sz) != want) continue;
            Hit x{i, k, b[i + 10], ""};
            for (size_t q = i + 4 + b[i + 4]; q < i + 4 + sz && b[q] && x.name.size() < 64; q++) x.name += (char)b[q];
            h.push_back(x);
        }
    }
    std::sort(h.begin(), h.end(), [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    return h;
}
// Z9803062.A0e1 -> Z9803062.A01, Z9803062.AR -> Z9803062.ARJ (the name as sent lost a character)
std::string volumeName(const std::string& n)
{
    size_t dot = n.rfind('.');
    if (dot == std::string::npos) return n + ".ARJ";
    std::string ext = n.substr(dot + 1), d;
    for (char c : ext) if (isDig((uint8_t)c)) d += c;
    if (ext == "AR" || ext == "ar") return n.substr(0, dot) + ".ARJ";
    if (!ext.empty() && (ext[0] == 'A' || ext[0] == 'a') && !d.empty()) return n.substr(0, dot) + ".A" + (d.size() >= 2 ? d.substr(d.size() - 2) : "0" + d);
    return n;
}
struct Volume { std::string name; B bytes; std::string log; long score = -1; std::string from; };

void doArj(Work& w, const std::vector<Hit>& hits, std::map<std::string, Volume>& vols)
{
    const B& data = w.data;
    const auto& ci = w.ci;
    // frame 0 borders (data positions): a document of another key starts there
    std::vector<size_t> f0;
    for (size_t i = 0; i < w.cs.size(); i++)
        if (isFrame0(w.cs[i])) { auto it = std::lower_bound(ci.begin(), ci.end(), (int)i); f0.push_back((size_t)(it - ci.begin())); }
    struct Piece { size_t a, b; int k; std::string name; bool startLost; };
    std::vector<Piece> ps;
    if (hits[0].type == 2 && hits[0].pos > 200)   // the end of the previous volume before this volume's main header
        ps.push_back({0, hits[0].pos, hits[0].k - 1, volumeName(w.in->name), true});
    size_t start = ps.empty() ? 0 : hits[0].pos;
    int k = hits[0].k;
    ps.push_back({start, data.size(), k, "", false});
    size_t prev = hits[0].pos;
    for (auto& h : hits) {
        if (h.k == k) { prev = h.pos; continue; }
        size_t at = h.pos;
        if (h.type != 2)
            for (size_t f : f0) if (f > prev && f <= h.pos) at = f;
        ps.back().b = at;
        ps.push_back({at, data.size(), h.k, "", false});
        k = h.k; prev = h.pos;
    }
    for (size_t j = 0; j < ps.size(); j++) {
        Piece& p = ps[j];
        if (!p.name.empty()) continue;
        const Hit* main = nullptr;
        for (auto& h : hits) if (h.pos == p.a && h.type == 2 && h.k == p.k) main = &h;
        if (main && !main->name.empty()) p.name = main->name;
        else if (j == 0) { p.name = volumeName(w.in->name); p.startLost = true; }
        else {
            // the start of the next volume whose main header did not come: its number from the rise of the key
            // (the volumes of one archive were sent with keys one apart: .A02 k, .A03 k+1, ...)
            const std::string& pn = ps[j - 1].name;
            size_t dot = pn.rfind('.');
            int num = -1;
            if (dot != std::string::npos && pn.size() == dot + 4 && (pn[dot + 1] == 'A' || pn[dot + 1] == 'a') && isDig((uint8_t)pn[dot + 2]) && isDig((uint8_t)pn[dot + 3]))
                num = (pn[dot + 2] - '0') * 10 + (pn[dot + 3] - '0') + (p.k - ps[j - 1].k);
            p.name = num > 0 && num < 100 ? pn.substr(0, dot) + fmt(".A%02d", num) : volumeName(w.in->name) + fmt(".part%zu", j + 1);
            p.startLost = true;
        }
    }
    if (w.r.k < 0) w.r.k = ps[0].k;
    std::string note;
    for (auto& p : ps) {
        // the volume as it came (no holes put in for the 50 cut-out bytes: put in at a guessed phase they made the
        // unpacking worse on the 1998 recording; arjcheck finds the damaged places itself and goes on past them)
        Volume best;
        best.name = safeName(p.name);
        for (size_t i = p.a; i < p.b; i++) best.bytes.push_back(dec(data[i], p.k));
        ArjCheck c = arjCheck(best.bytes);
        best.score = 0;
        for (auto& f : c.files) best.score += (long)f.got - (long)f.unknown + (f.ok ? 1000000 : 0);
        best.log = c.summary();
        best.from = w.in->name + fmt(" k=%d", p.k) + (p.startLost ? ", the start of the volume did not come" : "");
        auto it = vols.find(best.name);
        if (it == vols.end() || best.score > it->second.score || (best.score == it->second.score && best.bytes.size() > it->second.bytes.size()))
            vols[best.name] = best;
        note += (note.empty() ? "" : "; ") + best.name + fmt(" (k=%d)", p.k);
    }
    w.r.kind = "arj";
    w.r.note = "ARJ: " + note;
}
}   // namespace

std::vector<Norpak26Result> norpak26DecodeAll(const std::vector<Norpak26Input>& in, const std::string& outDir, std::vector<std::string>& report)
{
    std::vector<Work> W_(in.size());
    std::vector<size_t> order(in.size());
    for (size_t i = 0; i < in.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return in[a].name < in[b].name; });
    std::vector<Work*> ws;
    for (size_t o : order) ws.push_back(&W_[o]);
    for (size_t i = 0; i < in.size(); i++) { W_[i].in = &in[i]; W_[i].r.name = in[i].name; }
    std::map<std::string, Volume> vols;
    const bool write = !outDir.empty();

    // keys, kind, phase
    for (Work* w : ws) {
        const B& raw = w->in->raw;
        w->cs = cells(raw, w->cellPhase);
        stream(w->cs, -1, [](const uint8_t*, int) { return false; }, false, w->data, w->ci);
        size_t nz = 0;
        for (uint8_t v : w->data) nz += v != 0;
        if (nz < 30) { w->r.kind = "empty"; w->r.note = "no text: only the document header"; continue; }
        if (w->in->streamOffset >= 0) {   // the cut is at the end of a bundle of the stream (14 rows of 26 bytes)
            int64_t start = w->in->streamOffset + w->cellPhase - 25;
            if (start >= 0 && start % 26 == 0) w->poff = (int)(((13 - (start / 26)) % 14 + 14) % 14);
        }
        findKeys(*w);
        if (w->norm >= -32) { w->text = true; w->r.kind = "text"; continue; }
        auto hits = arjHits(w->data);
        if (!hits.empty()) { doArj(*w, hits, vols); continue; }
        // not CP866 text: the key of its first bytes (a Russian header line, e.g. the Arabic bulletin of РИА «Новости»)
        B head;
        for (uint8_t v : w->data) if (v && head.size() < 40) head.push_back(v);
        Key kh = bestKey(head);
        if (kh.norm() >= -20) {
            w->r.kind = "raw"; w->r.k = kh.k; w->r.lang = "";
            std::string first;
            for (uint8_t v : w->data) {
                if (!v) continue;
                uint8_t c = dec(v, kh.k);
                if (c == '\r' || c == '\n') { if (!first.empty()) break; continue; }
                if (first.size() >= 80) break;
                first += allowed(c) ? (char)c : '?';
            }
            w->r.title = utf8(first);
            w->r.note = "not CP866 text (another alphabet or code page); its header: " + w->r.title;
            if (write) {
                B b;
                for (uint8_t v : w->data) if (v) b.push_back(dec(v, kh.k));
                std::string dir = path_join(outDir, "_raw");
                make_dirs(dir);
                w->r.path = path_join(dir, safeName(w->in->name) + ".bin");
                write_file(w->r.path, b);
                write_text(path_join(dir, safeName(w->in->name) + ".note.txt"),
                           "\xEF\xBB\xBF" + w->r.note + fmt("\r\nkey k = %d (plain = Sinv[cipher] - k)\r\n", kh.k));
            }
        } else
            w->r.note = "not deciphered: neither text nor an ARJ archive under the table";
    }
    std::vector<Work*> texts;
    for (Work* w : ws) if (w->text) texts.push_back(w);
    for (Work* w : texts) {
        std::array<double, 14> comb;
        if (!phaseScores(*w, comb)) continue;
        std::array<double, 14> o = comb;
        std::sort(o.begin(), o.end(), std::greater<double>());
        int p = (int)(std::max_element(comb.begin(), comb.end()) - comb.begin());
        if (o[0] >= 3.5 && o[0] - o[1] >= 1.2) { w->r.phase = p; w->r.phaseHow = "text"; }
        else if (w->poff >= 0 && comb[w->poff] >= 2.0 && comb[w->poff] >= o[0] - 1.0) { w->r.phase = w->poff; w->r.phaseHow = "stream place"; }
    }
    // the texts without gap marks; gaps are filled from them
    std::vector<const std::string*> baseOf;
    for (Work* w : texts) {
        B d; std::vector<int> c;
        w->streamAt(w->r.phase, d, c);
        w->base = toText(d, c, w->sg, -1);
    }
    for (Work* w : texts) baseOf.push_back(&w->base);
    FillIndex ix;
    ix.build(baseOf);
    // phase still not known: the one at which other files fill the gaps (2 fills or more, twice the next best)
    for (uint32_t t = 0; t < texts.size(); t++) {
        Work* w = texts[t];
        if (w->r.phase >= 0) continue;
        int best = -1, n1 = 0, n2 = 0;
        for (int p = 0; p < 14; p++) {
            B d; std::vector<int> c;
            w->streamAt(p, d, c);
            std::vector<FillLog> log;
            tryFill(ix, t, toText(d, c, w->sg, p), log);
            int n = (int)log.size();
            if (n > n1) { n2 = n1; n1 = n; best = p; } else if (n > n2) n2 = n;
        }
        if (n1 >= 2 && n1 >= 2 * n2) { w->r.phase = best; w->r.phaseHow = "duplicates"; }
    }
    // the texts
    int nText = 0, nRu = 0, nEn = 0, marks = 0, fills = 0, removed = 0, phased = 0;
    std::string fillsLog;
    for (uint32_t t = 0; t < texts.size(); t++) {
        Work* w = texts[t];
        std::string text;
        if (w->r.phase < 0) text = w->base;
        else {
            B d; std::vector<int> c;
            w->streamAt(w->r.phase, d, c);
            std::vector<FillLog> log;
            text = tryFill(ix, t, toText(d, c, w->sg, w->r.phase), log);
            std::string z;   // a run of gap marks is one gap
            for (char ch : text) if (!(ch == (char)GAP && !z.empty() && z.back() == (char)GAP)) z += ch;
            text = z;
            w->r.filled = (int)log.size();
            for (auto& l : log) fillsLog += w->in->name + "\t" + utf8(l.after) + "\t" + utf8(l.inserted) + "\t" + texts[l.from]->in->name + "\r\n";
            phased++;
        }
        int rm = 0;
        text = dropGarbage(fixlat(text), rm);
        removed += rm;
        bool any = false;
        for (unsigned char c : text) any |= !(c == ' ' || c == '\n' || c == '\t' || c == '\r');
        if (!any) { w->r.kind = "empty"; w->r.note = "no text: only the document header"; continue; }
        w->r.gaps = (int)std::count(text.begin(), text.end(), (char)GAP);
        marks += w->r.gaps; fills += w->r.filled;
        std::vector<std::string> cpLines;
        size_t a = 0;
        for (size_t i = 0; i <= text.size(); i++)
            if (i == text.size() || text[i] == '\n') { cpLines.push_back(text.substr(a, i - a)); a = i + 1; }
        if (!cpLines.empty() && cpLines.back().empty()) cpLines.pop_back();   // the text ends with a newline
        for (auto& l : cpLines) w->r.lines.push_back(utf8(l));
        w->r.title = titleOf(cpLines);
        w->r.source = sourceOf(w->in->name, w->r.lines.empty() ? "" : w->r.lines[0]);
        nText++; (w->r.lang == "en" ? nEn : nRu)++;
        if (write) {
            make_dirs(outDir);
            std::string out = "\xEF\xBB\xBF";
            for (auto& l : w->r.lines) out += l + "\r\n";
            w->r.path = path_join(outDir, safeName(w->in->name) + ".txt");
            write_text(w->r.path, out);
        }
    }
    // the archive volumes and what unpacks from them
    int arjFiles = 0, arjWhole = 0;
    std::vector<std::string> arjLines;
    for (auto& kv : vols) {
        Volume& v = kv.second;
        std::string line = "    " + v.name + fmt(" (%zu bytes, from %s): ", v.bytes.size(), v.from.c_str()) + v.log;
        if (write) {
            std::string dir = path_join(outDir, "_archive_ARJ");
            make_dirs(dir);
            std::string p = path_join(dir, v.name);
            write_file(p, v.bytes);
            ArjCheck c = arjCheck(v.bytes);
            if (!c.files.empty()) {
                std::string list = arjExtract(c, p + "_files");
                line += "\r\n      unpacked -> " + p + "_files\\:\r\n" + list;
                arjFiles += (int)c.files.size(); arjWhole += c.good();
            }
        }
        arjLines.push_back(line);
    }
    // report, table of contents, parameters
    int nRaw = 0, nEmpty = 0, nNot = 0, nArj = 0;
    for (Work* w : ws) { nRaw += w->r.kind == "raw"; nEmpty += w->r.kind == "empty"; nNot += w->r.kind.empty(); nArj += w->r.kind == "arj"; }
    report.push_back(fmt("  Norpak-26 files deciphered (table S + key per document): %d texts (%d Russian, %d English), "
                         "%d with the phase of the cut-out bytes known: %d places marked [\xE2\x80\xA6], %d filled from duplicates; "
                         "%d lines of scraps removed; %d ARJ volume files -> %zu volumes; %d kept as bytes (not CP866 text); %d without text; %d not deciphered",
                         nText, nRu, nEn, phased, marks, fills, removed, nArj, vols.size(), nRaw, nEmpty, nNot) +
                     (write ? " -> " + outDir : ""));
    for (auto& l : arjLines) report.push_back(l);
    if (write) {
        std::string toc = "\xEF\xBB\xBF\xD0\x9E\xD0\xB3\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 (TeletextRescue, Norpak-26)\r\n"
                          "file | source | language | characters | places [\xE2\x80\xA6] | filled from duplicates | title\r\n\r\n";
        std::string par = "\xEF\xBB\xBF" "file\tkind\tk\tlanguage\tdocuments (cell:k:language)\tphase\tphase from\tgaps\tfilled\tnote\r\n";
        for (Work* w : ws) {
            const auto& r = w->r;
            if (r.kind == "text") {
                size_t chars = 0;
                for (auto& l : r.lines) chars += to_u32(l).size() + 1;
                std::string lang = r.lang;
                if (!r.segments.empty()) {
                    std::set<std::string> ls;
                    for (auto& g : w->sg) ls.insert(LANG[g.lang]);
                    lang.clear();
                    for (auto& l : ls) lang += (lang.empty() ? "" : "+") + l;
                }
                toc += pad(r.name, 28) + " | " + pad(r.source, 29) + " | " + pad(lang, 5) + fmt(" | %6zu | %3d | %2d | ", chars, r.gaps, r.filled) + r.title + "\r\n";
            }
            par += r.name + "\t" + r.kind + "\t" + (r.k >= 0 ? fmt("%d", r.k) : "") + "\t" + r.lang + "\t" + r.segments + "\t" +
                   (r.phase >= 0 ? fmt("%d", r.phase) : "") + "\t" + r.phaseHow + fmt("\t%d\t%d\t", r.gaps, r.filled) + r.note + "\r\n";
        }
        std::string other;
        for (Work* w : ws) if (w->r.kind == "empty") other += (other.empty() ? "" : ", ") + w->r.name;
        toc += "\r\nWithout text (only the document header): " + other + "\r\n";
        other.clear();
        for (Work* w : ws) if (w->r.kind == "raw") other += (other.empty() ? "" : ", ") + w->r.name + " (" + w->r.title + ")";
        if (!other.empty()) toc += "Not CP866 text, kept as bytes in _raw\\: " + other + "\r\n";
        if (!vols.empty()) toc += fmt("ARJ archive: %zu volumes in _archive_ARJ\\", vols.size()) + (arjFiles ? fmt(", %d files inside, %d of them whole (CRC)", arjFiles, arjWhole) : std::string()) + "\r\n";
        make_dirs(outDir);
        write_text(path_join(outDir, "_\xD0\x9E\xD0\x93\xD0\x9B\xD0\x90\xD0\x92\xD0\x9B\xD0\x95\xD0\x9D\xD0\x98\xD0\x95.txt"), toc);
        write_text(path_join(outDir, "_norpak26_params.txt"), par);
        if (!fillsLog.empty()) write_text(path_join(outDir, "_gaps_filled.txt"), "\xEF\xBB\xBF" "file\tafter\tinserted\tfrom\r\n" + fillsLog);
    }
    std::vector<Norpak26Result> out;
    for (auto& w : W_) out.push_back(w.r);
    return out;
}
