#include "pagebuild.h"
#include <numeric>
#include <unordered_map>

// ---------------------------------------------------------------- счётчик с порядком вставки (как Counter)
template <class K>
struct Counter {
    std::vector<std::pair<K, long>> v;
    std::map<K, size_t> idx;
    void add(const K &k, long n = 1) {
        auto it = idx.find(k);
        if (it == idx.end()) { idx[k] = v.size(); v.push_back({k, n}); }
        else v[it->second].second += n;
    }
    long get(const K &k) const { auto it = idx.find(k); return it == idx.end() ? 0 : v[it->second].second; }
    bool empty() const { return v.empty(); }
    size_t size() const { return v.size(); }
    std::vector<std::pair<K, long>> most_common() const {
        auto r = v;
        std::stable_sort(r.begin(), r.end(), [](auto &a, auto &b) { return a.second > b.second; });
        return r;
    }
    std::pair<K, long> top() const { auto r = most_common(); return r.empty() ? std::pair<K, long>{} : r[0]; }
    long total() const { long s = 0; for (auto &p : v) s += p.second; return s; }
};

// ---------------------------------------------------------------- файлы потоков
std::vector<Packet42> read_t42(const std::string &path) {
    Bytes b = read_file(path);
    std::vector<Packet42> out(b.size() / 42);
    for (size_t i = 0; i < out.size(); i++) memcpy(out[i].data(), &b[i * 42], 42);
    return out;
}
void write_t42(const std::string &path, const std::vector<Packet42> &pk) {
    Bytes b(pk.size() * 42);
    for (size_t i = 0; i < pk.size(); i++) memcpy(&b[i * 42], pk[i].data(), 42);
    write_file(path, b);
}

void save_npy_i32(const std::string &path, const LineMap &m) {
    std::string hdr = fmt("{'descr': '<i4', 'fortran_order': False, 'shape': (%d, %d), }", m.frames, m.lpf);
    while ((10 + hdr.size() + 1) % 64) hdr += ' ';
    hdr += '\n';
    Bytes b = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, (u8)(hdr.size() & 0xFF), (u8)(hdr.size() >> 8)};
    b.insert(b.end(), hdr.begin(), hdr.end());
    size_t o = b.size(); b.resize(o + m.v.size() * 4);
    memcpy(&b[o], m.v.data(), m.v.size() * 4);
    write_file(path, b);
}
LineMap load_npy_i32(const std::string &path) {
    LineMap m;
    Bytes b = read_file(path);
    if (b.size() < 10 || b[0] != 0x93) throw std::runtime_error("not a .npy file: " + path);
    size_t hl, off;
    if (b[6] == 1) { hl = b[8] | (b[9] << 8); off = 10; } else { hl = b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24); off = 12; }
    std::string hdr(b.begin() + off, b.begin() + off + hl);
    size_t p = hdr.find("shape");
    p = hdr.find('(', p);
    sscanf(hdr.c_str() + p, "(%d, %d)", &m.frames, &m.lpf);
    m.v.resize((size_t)m.frames * m.lpf);
    memcpy(m.v.data(), &b[off + hl], m.v.size() * 4);
    return m;
}

static inline int H(u8 x) { return ham::dec[x]; }

// ---------------------------------------------------------------- .t34 -> .t42
int t34_to_t42(const std::string &src, const std::string &dst, Progress &pr) {
    Bytes d = read_file(src);
    size_t n = d.size() / 34;
    auto addr = [&](const u8 *p, int &m, int &r) {
        int a = H(p[0]), b = H(p[1]);
        if (a < 0 || b < 0) return false;
        m = a & 7; r = (a >> 3) | (b << 1); return true;
    };
    int hdr[8] = {0}, blk[8] = {0};
    for (size_t i = 0; i < n; i++) {
        const u8 *p = &d[i * 34]; int m, r;
        if (!addr(p, m, r)) continue;
        if (r == 0 && H(p[2]) >= 0 && H(p[3]) >= 0 && ((H(p[3]) << 4) | H(p[2])) != 0xFF) hdr[m]++;
        else if (r == 1 || r == 4 || r == 8 || r == 12 || r == 16 || r == 20) blk[m]++;
    }
    std::set<int> carrier;
    for (int m = 4; m < 8; m++) if (hdr[m] == 0 && blk[m] >= 6) carrier.insert(m);
    std::vector<Packet42> out;
    std::map<int, std::vector<Packet42>> page;
    auto close = [&](int m) {
        auto it = page.find(m);
        if (it == page.end()) return;
        for (auto &q : it->second) out.push_back(q);
        page.erase(it);
    };
    int ext = 0;
    for (size_t i = 0; i < n; i++) {
        const u8 *p = &d[i * 34]; int m, r;
        Packet42 q; q.fill(0x20); memcpy(q.data(), p, 34);
        if (!addr(p, m, r)) { out.push_back(q); continue; }
        if (carrier.count(m)) {
            auto it = page.find(m & 3);
            if (it == page.end() || it->second.empty()) continue;
            int first = r / 4 * 4;
            for (int g = 0; g < 4; g++)
                for (auto &pk : it->second) {
                    int mm, rr;
                    if (!addr(pk.data(), mm, rr) || rr != first + g) continue;
                    for (int k = 0; k < 8; k++) {
                        u8 c = p[2 + 8 * g + k], cur = pk[34 + k];
                        if (odd_parity(c) && (cur == 0x20 || !odd_parity(cur))) pk[34 + k] = c;
                    }
                }
            ext++; continue;
        }
        if (r == 0) { close(m); page[m] = {q}; }
        else if (page.count(m) && r >= 1 && r <= 23) page[m].push_back(q);
        else out.push_back(q);
    }
    std::vector<int> ms; for (auto &kv : page) ms.push_back(kv.first);
    for (int m : ms) close(m);
    write_t42(dst, out);
    std::string cs;
    for (int m : carrier) cs += (cs.empty() ? "" : ", ") + std::to_string(m);
    pr.log(fmt("row-continuation carriers: magazines %s; continuations %d", carrier.empty() ? "none" : ("[" + cs + "]").c_str(), ext));
    return (int)out.size();
}

// ---------------------------------------------------------------- служебные данные (extract_extras)
static std::string pid_of(int mag, int t, int u) { return fmt("%d%d%d", mag, t, u); }
static std::string sub_code(const int sc[4]) { return fmt("%X%X%X%X", sc[3] & 3, sc[2], sc[1] & 7, sc[0]); }

Json extract_extras(const std::vector<Packet42> &st, Progress &pr) {
    std::map<std::string, std::map<int, Counter<std::string>>> flof;
    std::map<std::string, std::map<std::string, Counter<std::string>>> x26;
    std::map<std::string, std::map<std::string, std::map<std::string, Counter<std::string>>>> x26s;
    std::map<int, std::string> cursub, cur;
    std::map<std::string, Counter<int>> nat;
    Counter<std::string> g0, tx, c6;
    std::map<std::string, Counter<std::string>> subs;
    std::vector<std::pair<long, int>> clock;
    std::vector<std::string> order;                   // порядок появления страниц
    auto seen = [&](const std::string &p) { if (std::find(order.begin(), order.end(), p) == order.end()) order.push_back(p); };
    for (size_t k = 0; k < st.size(); k++) {
        const u8 *p = st[k].data();
        int a = H(p[0]), b = H(p[1]);
        if (a < 0 || b < 0) continue;
        int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
        if (row == 0) {
            int u = H(p[2]), t = H(p[3]);
            std::string pid = (u >= 0 && t >= 0 && t <= 9 && u <= 9) ? pid_of(mag, t, u) : "";
            cur[mag] = pid;
            int sc[4]; bool ok = true;
            for (int i = 0; i < 4; i++) { sc[i] = H(p[4 + i]); if (sc[i] < 0) ok = false; }
            cursub[mag] = ok ? sub_code(sc) : "";
            int d[6]; bool dok = true;
            for (int i = 0; i < 6; i++) { d[i] = H(p[4 + i]); if (d[i] < 0) dok = false; }
            if (!pid.empty() && dok) {
                seen(pid);
                tx.add(pid); c6.add(pid, (d[3] >> 3) & 1); nat[pid].add(national_of(d[5]));
                subs[pid].add(fmt("%d,%d,%d,%d", d[3] & 3, d[2], d[1] & 7, d[0]));
            }
            char txt[9];
            for (int i = 0; i < 8; i++) txt[i] = (char)(p[34 + i] & 0x7F);
            txt[8] = 0;
            auto dig = [&](int i) { return txt[i] >= '0' && txt[i] <= '9'; };
            if (txt[2] == ':' && txt[5] == ':' && dig(0) && dig(1) && dig(3) && dig(4) && dig(6) && dig(7)) {
                int hh = (txt[0] - '0') * 10 + txt[1] - '0', mm = (txt[3] - '0') * 10 + txt[4] - '0', ss = (txt[6] - '0') * 10 + txt[7] - '0';
                if (hh < 24 && mm < 60 && ss < 60) clock.push_back({(long)k, hh * 3600 + mm * 60 + ss});
            }
            continue;
        }
        if ((row == 28 || row == 29) && (H(p[2]) == 0 || H(p[2]) == 4)) {
            int tr[13]; bool ok = true;
            for (int i = 0; i < 13; i++) { tr[i] = ham::h2418(p[3 + 3 * i], p[4 + 3 * i], p[5 + 3 * i]); if (tr[i] < 0) ok = false; }
            if (ok && (row == 29 || (tr[0] & 0xF) == 0)) {
                int val = (tr[0] >> 7) & 0x7F;
                std::string cs = "latin";
                if (((val >> 3) & 0xF) == 4) { int nb = val & 7; cs = nb == 0 ? "cyr1" : nb == 4 ? "cyr2" : nb == 5 ? "cyr3" : "latin"; }
                g0.add(cs);
            }
        }
        auto it = cur.find(mag);
        if (it == cur.end() || it->second.empty()) continue;
        const std::string &pid = it->second;
        if (row == 27 && H(p[2]) == 0) {
            for (int i = 0; i < 6; i++) {
                int d[6]; bool ok = true;
                for (int q = 0; q < 6; q++) { d[q] = H(p[3 + 6 * i + q]); if (d[q] < 0) ok = false; }
                if (!ok) continue;
                int pu = d[0], pt = d[1];
                if (pu > 9 || pt > 9) continue;
                int m = mag ^ (((d[3] >> 3) & 1) | (((d[5] >> 2) & 1) << 1) | (((d[5] >> 3) & 1) << 2));
                seen(pid);
                flof[pid][i].add(fmt("%d%d%d", m ? m : 8, pt, pu));
            }
        } else if (row == 26) {
            int r_ = -1;
            for (int i = 0; i < 13; i++) {
                int t = ham::h2418(p[3 + 3 * i], p[4 + 3 * i], p[5 + 3 * i]);
                if (t < 0) break;
                int addr = t & 0x3F, mode = (t >> 6) & 0x1F, data = (t >> 11) & 0x7F;
                if (addr >= 40) {
                    if (mode == 0x1F) break;
                    r_ = addr == 40 ? 24 : (mode == 0x04 ? addr - 40 : -1);
                } else if (r_ >= 0) {
                    std::string ch;
                    if (mode == 0x0F) { char32_t g = g2_extract(data); if (g) ch = from_cp(g); }
                    else if (mode == 0x09) { if (data >= 0x20) ch = from_cp(data); }
                    else if (mode >= 0x10 && data >= 0x20) ch = compose_nfc(data, diacritic_mark(mode - 0x10));
                    if (!strip(ch).empty()) {
                        std::string pos = fmt("%d,%d", r_, addr);
                        seen(pid);
                        x26[pid][pos].add(ch);
                        if (!cursub[mag].empty()) x26s[pid][cursub[mag]][pos].add(ch);
                    }
                }
            }
        }
    }
    Json out = Json::object();
    auto pick = [](const Counter<std::string> &c, std::string &v) {
        auto t = c.top();
        if (t.second >= 2 && t.second >= 0.6 * c.total()) { v = t.first; return true; }
        return false;
    };
    std::vector<std::string> pids = order;
    std::sort(pids.begin(), pids.end());
    for (auto &pid : pids) {
        Json e = Json::object();
        if (tx.get(pid)) {
            e["tx"] = (int)tx.get(pid);
            int ns = 0; for (auto &kv : subs[pid].v) if (kv.second >= 3) ns++;
            e["subpages"] = ns ? ns : 1;
            if (c6.get(pid) >= std::max(2.0, 0.5 * tx.get(pid))) e["boxed"] = true;
            e["nat"] = nat[pid].empty() ? 1 : nat[pid].top().first;
        }
        if (flof.count(pid)) {
            std::vector<std::string> links(6);
            for (int i = 0; i < 6; i++) { auto c = flof[pid].find(i); if (c != flof[pid].end()) pick(c->second, links[i]); }
            if (!links[0].empty() || !links[1].empty() || !links[2].empty() || !links[3].empty()) {
                Json a = Json::array();
                for (int i : {0, 1, 2, 3, 5}) a.a.push_back(links[i].empty() ? Json() : Json(links[i]));
                e["flof"] = a;
            }
        }
        if (x26.count(pid)) {
            Json ov = Json::object();
            for (auto &kv : x26[pid]) { std::string ch; if (pick(kv.second, ch)) ov[kv.first] = ch; }
            if (ov.size()) e["x26"] = ov;
            Json per = Json::object();
            for (auto &sk : x26s[pid]) {
                Json o = Json::object();
                for (auto &kv : sk.second) { std::string ch; if (pick(kv.second, ch)) o[kv.first] = ch; }
                if (o.size()) per[sk.first] = o;
            }
            if (per.size() > 1 || (per.size() == 1 && per.o[0].first != "0000")) e["x26s"] = per;
        }
        if (e.size()) out[pid] = e;
    }
    Json meta = Json::object();
    if (!clock.empty()) {
        size_t n = clock.size();
        std::vector<double> K(n), T(n);
        double kmin = 1e300, kmax = -1e300;
        for (size_t i = 0; i < n; i++) { K[i] = clock[i].first / 650.0; T[i] = clock[i].second; kmin = std::min(kmin, K[i]); kmax = std::max(kmax, K[i]); }
        double ptp = kmax - kmin; if (ptp == 0) ptp = 1;
        // скорость часов относительно записи: медиана наклонов между далёкими точками
        std::vector<double> sl;
        uint64_t s = 88172645463325252ull;
        for (int q = 0; q < 4000; q++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; size_t i0 = s % n;
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; size_t i1 = s % n;
            double dk = K[i1] - K[i0];
            if (fabs(dk) > 0.2 * ptp) sl.push_back((T[i1] - T[i0]) / dk);
        }
        double slope = sl.size() > 10 ? median(sl) : 1.0;
        std::vector<double> res(n); for (size_t i = 0; i < n; i++) res[i] = T[i] - slope * K[i];
        double med = median(res);
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
        for (size_t i = 0; i < n; i++) if (fabs(res[i] - med) < 30) { sx += K[i]; sy += T[i]; sxx += K[i] * K[i]; sxy += K[i] * T[i]; m++; }
        double b_ = slope, a_ = med;
        if (m > 1 && m * sxx - sx * sx != 0) { b_ = (m * sxy - sx * sy) / (m * sxx - sx * sx); a_ = (sy - b_ * sx) / m; }
        Json c = Json::array(); c.push(round(a_ * 100) / 100); c.push(round(b_ * 1e5) / 1e5);
        meta["clock"] = c;
    }
    Counter<int> allnat;
    for (auto &kv : nat) for (auto &p : kv.second.v) allnat.add(p.first, p.second);
    if (!allnat.empty()) meta["national"] = allnat.top().first;
    if (!g0.empty() && g0.top().second >= 2) meta["g0"] = g0.top().first;
    meta["service"] = service_info(st);
    out["_meta"] = meta;
    int nf = 0, nx = 0, nb = 0;
    for (auto &kv : out.o) { if (kv.second.has("flof")) nf++; if (kv.second.has("x26")) nx++; if (kv.second.get_bool("boxed")) nb++; }
    pr.log(fmt("key links on %d pages, character enhancements on %d pages, service (C6) %d; %s", nf, nx, nb, meta.dump().c_str()));
    return out;
}


// ---------------------------------------------------------------- канал и время (8/30, заголовок)
static int rev8(int x) { int r = 0; for (int i = 0; i < 8; i++) if (x & (1 << i)) r |= 0x80 >> i; return r; }
static std::string hex_of(const u8 *p, int n) { std::string s; for (int i = 0; i < n; i++) s += fmt("%02X", p[i] & 0x7F); return s; }
static std::string iso_of(long mjd, int hh, int mm, int ss) {
    // MJD -> дата (EN 300 468, приложение C)
    long yp = (long)((mjd - 15078.2) / 365.25), mp = (long)((mjd - 14956.1 - (long)(yp * 365.25)) / 30.6001);
    long d = mjd - 14956 - (long)(yp * 365.25) - (long)(mp * 30.6001), k = (mp == 14 || mp == 15) ? 1 : 0;
    long y = 1900 + yp + k, m = mp - 1 - k * 12;
    return fmt("%04ld-%02ld-%02ld %02d:%02d:%02d", y, m, d, hh, mm, ss);
}

Json service_info(const std::vector<Packet42> &st) {
    Counter<std::string> status, ni, offs;
    std::string first, last;
    std::vector<std::array<u8, 24>> heads;
    for (auto &pk : st) {
        const u8 *p = pk.data();
        int a = H(p[0]), b = H(p[1]);
        if (a < 0 || b < 0) continue;
        int row = (a >> 3) | (b << 1);
        if (row == 0 && heads.size() < 200000) {
            std::array<u8, 24> h; bool ok = true;
            for (int k = 0; k < 24; k++) { if (!odd_parity(p[10 + k])) ok = false; h[k] = p[10 + k] & 0x7F; }
            if (ok) heads.push_back(h);
            continue;
        }
        if (row != 30 || (a & 7) != 0) continue;            // 8/30
        int dc = H(p[2]);
        if (dc < 0 || dc > 3) continue;
        bool sok = true; for (int k = 22; k < 42; k++) if (!odd_parity(p[k])) sok = false;
        if (sok) status.add(hex_of(p + 22, 20));
        if (dc >> 1) continue;                                // формат 2 (PDC): только строка состояния
        ni.add(fmt("%04X", rev8(p[9]) | (rev8(p[10]) << 8)));
        int to = p[11];
        offs.add(fmt("%d", ((to >> 6) & 1 ? -1 : 1) * ((to >> 1) & 0x1F) * 30));
        auto dig = [&](int x) { return x - 1; };
        int d[5] = {dig(p[12] & 15), dig(p[13] >> 4), dig(p[13] & 15), dig(p[14] >> 4), dig(p[14] & 15)};
        int t[6] = {dig(p[15] >> 4), dig(p[15] & 15), dig(p[16] >> 4), dig(p[16] & 15), dig(p[17] >> 4), dig(p[17] & 15)};
        bool ok = true; for (int x : d) if (x < 0 || x > 9) ok = false; for (int x : t) if (x < 0 || x > 9) ok = false;
        if (!ok) continue;
        long mjd = d[0] * 10000L + d[1] * 1000 + d[2] * 100 + d[3] * 10 + d[4];
        int hh = t[0] * 10 + t[1], mm = t[2] * 10 + t[3], ss = t[4] * 10 + t[5];
        if (mjd < 40000 || mjd > 70000 || hh > 23 || mm > 59 || ss > 59) continue;
        std::string iso = iso_of(mjd, hh, mm, ss);
        if (first.empty()) first = iso;
        last = iso;
    }
    Json out = Json::object();
    if (!status.empty() && status.top().second >= 2) out["status"] = status.top().first;
    if (!ni.empty() && ni.top().second >= 2) out["ni"] = ni.top().first;
    if (!first.empty()) {
        Json u = Json::array(); u.push(first); u.push(last); out["utc"] = u;
        if (!offs.empty()) out["offset_min"] = (double)std::stoi(offs.top().first);
    }
    // текст заголовка без номера страницы (имя канала, службы, дата): самый частый вид (цифры не различаются),
    // из заголовков этого вида — самый частый текст
    if (heads.size() >= 20) {
        auto mask = [](const std::array<u8, 24> &h) { std::string m(h.begin(), h.end()); for (auto &c : m) if (c >= '0' && c <= '9') c = '#'; return m; };
        Counter<std::string> tm;
        for (auto &x : heads) tm.add(mask(x));
        auto top = tm.top();
        Counter<std::string> real;
        for (auto &x : heads) if (mask(x) == top.first) real.add(std::string(x.begin(), x.end()));
        std::string h = real.top().first;
        size_t k = 0;
        while (k < h.size() && (h[k] == ' ' || (u8)h[k] < 0x20)) k++;
        size_t d = k; while (d < h.size() && h[d] >= '0' && h[d] <= '9') d++;
        if (d - k == 3) k = d;                               // номер страницы
        bool any = false; for (size_t i = k; i < h.size(); i++) if ((u8)h[i] > 0x20) any = true;
        if (any && top.second >= 0.05 * heads.size()) out["header"] = hex_of((const u8 *)h.data() + k, (int)(h.size() - k));
    }
    return out;
}

// ---------------------------------------------------------------- сборка страниц (export_json)
namespace {
using R40 = std::array<u8, 40>;
using SegRows = std::vector<std::pair<int, R40>>;           // ряды в порядке прихода
struct Tx { long i; std::shared_ptr<SegRows> rows; std::string sub; bool has_sub; };
struct Seg { bool has_pid = false; std::string pid; std::shared_ptr<SegRows> rows; long i = 0; std::string prev; };
const int CONTENT_LO = 5, CONTENT_HI = 21;
bool content_row(int r) { return r >= CONTENT_LO && r <= CONTENT_HI; }
bool informative(const R40 &b) { int n = 0; for (u8 c : b) n += (c != 0x20 && c != 0xA0); return n >= 12; }
int dist(const R40 &a, const R40 &b) { int d = 0; for (int k = 0; k < 40; k++) d += a[k] != b[k]; return d; }
bool near4(const R40 &a, const R40 &b) { return dist(a, b) <= 4; }
R40 vote(const std::vector<const R40 *> &mem) {
    R40 out;
    for (int k = 0; k < 40; k++) {
        Counter<int> c;
        for (auto m : mem) if (odd_parity((*m)[k])) c.add((*m)[k]);
        out[k] = (u8)((c.empty() ? 0x20 : c.top().first) & 0x7F);
    }
    return out;
}
}

PagesBuild export_pages(const std::vector<Packet42> &st, const ExportOpts &o, Progress &pr) {
    const long N = (long)st.size();
    auto hbits = [](int x) { return ((x >> 1) & 1) | (((x >> 3) & 1) << 1) | (((x >> 5) & 1) << 2) | (((x >> 7) & 1) << 3); };
    std::vector<char> boundary(N, 0);
    {
        std::vector<char> ev(N, 0); std::map<int, int> last;
        for (long i = 0; i < N; i++) {
            int a = hbits(st[i][0]), b = hbits(st[i][1]);
            int m = a & 7, r = (a >> 3) | (b << 1);
            if (r > 24) continue;
            auto it = last.find(m);
            if (it != last.end() && r != 0 && r != it->second + 1) ev[i] = 1;
            last[m] = r;
        }
        const long STEP = 260, WIN = 1040;
        for (long s0 = 0; s0 < N; s0 += STEP) {
            long w0 = std::max(0L, s0 + STEP / 2 - WIN / 2);
            int cnt[13] = {0}; bool any = false;
            for (long i = w0; i < std::min(N, w0 + WIN); i++) if (ev[i]) { cnt[i % 13]++; any = true; }
            int ph = 0;
            if (any) for (int k = 1; k < 13; k++) if (cnt[k] > cnt[ph]) ph = k;
            for (long i = s0; i < std::min(N, s0 + STEP); i++) if (i % 13 == ph) boundary[i] = 1;
        }
    }
    if (o.lines) {
        std::fill(boundary.begin(), boundary.end(), 0);
        const LineMap &lp = *o.lines;
        size_t tot = lp.v.size(), F = o.field;
        for (size_t f0 = 0; f0 + F <= tot; f0 += F) {
            int32_t mn = INT32_MAX;
            for (size_t k = f0; k < f0 + F; k++) if (lp.v[k] >= 0) mn = std::min(mn, lp.v[k]);
            if (mn != INT32_MAX && mn < N) boundary[mn] = 1;
        }
    }
    const int MIN_TX = o.sparse ? 1 : 3, MIN_ROW = o.sparse ? 1 : 2;
    std::map<std::string, std::vector<Tx>> tx;
    std::map<int, std::shared_ptr<Seg>> cur;                // нет ключа или null — нет сегмента
    std::map<int, int> last;
    std::vector<std::pair<std::string, std::shared_ptr<Seg>>> orphans;
    std::vector<std::shared_ptr<Seg>> loose;
    std::map<int, std::string> lasthdr;
    std::map<std::string, Counter<std::string>> follow;
    auto close = [&](int mag, const std::string &next_pid) {
        auto it = cur.find(mag);
        if (it == cur.end() || !it->second) return;
        auto seg = it->second;
        if (!seg->has_pid) {
            if (!next_pid.empty() && !seg->rows->empty()) orphans.push_back({next_pid, seg});
            else if (!seg->rows->empty() && !seg->prev.empty()) loose.push_back(seg);
        } else if (!next_pid.empty()) follow[next_pid].add(seg->pid);
    };
    for (long i = 0; i < N; i++) {
        if (o.field_reset && boundary[i]) {
            std::vector<int> ms; for (auto &kv : cur) ms.push_back(kv.first);
            for (int m : ms) close(m, "");
            cur.clear(); last.clear();
            for (int m = 1; m <= 8; m++) {
                auto s = std::make_shared<Seg>(); s->rows = std::make_shared<SegRows>(); s->i = i;
                auto lh = lasthdr.find(m); if (lh != lasthdr.end()) s->prev = lh->second;
                cur[m] = s; last[m] = 0;
            }
        }
        const u8 *p = st[i].data();
        int a = H(p[0]), b = H(p[1]);
        if (a < 0 || b < 0) continue;
        int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
        if (row == 0) {
            int u = H(p[2]), t = H(p[3]);
            if (u < 0 || t < 0 || t > 9 || u > 9) { cur[mag] = nullptr; continue; }
            std::string pid = pid_of(mag, t, u);
            close(mag, pid); lasthdr[mag] = pid;
            int sc[4]; bool ok = true;
            for (int k = 0; k < 4; k++) { sc[k] = H(p[4 + k]); if (sc[k] < 0) ok = false; }
            auto rows = std::make_shared<SegRows>();
            R40 h; for (int k = 0; k < 8; k++) h[k] = 0x20; memcpy(&h[8], p + 10, 32);
            rows->push_back({0, h});
            tx[pid].push_back({i, rows, ok ? sub_code(sc) : "", ok});
            auto s = std::make_shared<Seg>(); s->has_pid = true; s->pid = pid; s->rows = rows; s->i = i;
            cur[mag] = s; last[mag] = 0;
        } else if (row >= 1 && row <= 24) {
            auto it = cur.find(mag);
            if (it == cur.end() || !it->second) continue;
            R40 r; memcpy(r.data(), p + 2, 40);
            if (row <= last[mag]) {
                // повтор уже принятого ряда (DVB шлёт группы рядов дважды: 1,2,3,1,2,3,4,5,6…) — страница продолжается;
                // другой ряд с тем же номером — значит, начался чужой кусок
                bool rep = false;
                for (auto &rb : *it->second->rows) if (rb.first == row && dist(rb.second, r) <= 4) { rep = true; break; }
                if (!rep) cur[mag] = nullptr;
                continue;
            }
            it->second->rows->push_back({row, r}); last[mag] = row;
        }
    }
    // хвосты-сироты
    std::map<std::string, long> hdrs;
    for (auto &kv : tx) hdrs[kv.first] = (long)kv.second.size();
    long med = 0;
    { std::vector<long> v; for (auto &kv : hdrs) v.push_back(kv.second); std::sort(v.begin(), v.end()); if (!v.empty()) med = v[v.size() / 2]; }
    std::vector<std::string> known;
    for (auto &kv : hdrs) if (kv.second >= 3) known.push_back(kv.first);   // map -> уже по возрастанию
    auto hd = [&](const std::string &p) { auto it = hdrs.find(p); return it == hdrs.end() ? 0L : it->second; };
    auto prev_page = [&](const std::string &p) {
        std::string best;
        for (auto &k : known) if (k[0] == p[0] && k < p) best = k;
        return best;
    };
    for (auto &op : orphans) {
        const std::string &nxt = op.first;
        if (hd(nxt) > 2 * med) continue;
        std::string prev;
        auto f = follow.find(nxt);
        prev = (f != follow.end() && !f->second.empty()) ? f->second.top().first : prev_page(nxt);
        if (prev.empty() || hd(prev) > 2 * med) continue;
        tx[prev].push_back({op.second->i, op.second->rows, "", false});
    }
    auto cand_pages = [&](const std::string &A) {
        std::vector<std::string> same;
        for (auto &k : known) if (k[0] == A[0]) same.push_back(k);
        auto it = std::find(same.begin(), same.end(), A);
        if (it == same.end()) return std::vector<std::string>();
        size_t j = it - same.begin();
        return std::vector<std::string>(same.begin() + j, same.begin() + std::min(same.size(), j + 3));
    };
    int added = 0;
    for (int pass = 0; pass < 4; pass++) {
        std::map<std::pair<char, int>, std::vector<std::pair<const R40 *, std::string>>> idx;
        for (auto &kv : tx)
            for (auto &t : kv.second)
                for (auto &rb : *t.rows)
                    if (content_row(rb.first) && informative(rb.second)) idx[{kv.first[0], rb.first}].push_back({&rb.second, kv.first});
        std::vector<std::shared_ptr<Seg>> rest; int n0 = added;
        std::vector<std::pair<std::string, std::shared_ptr<Seg>>> to_add;
        for (auto &seg : loose) {
            auto C = cand_pages(seg->prev);
            if (C.empty()) continue;
            Counter<std::string> hits;
            for (auto &rb : *seg->rows) {
                if (!content_row(rb.first) || !informative(rb.second)) continue;
                std::set<std::string> owners;
                auto it = idx.find({seg->prev[0], rb.first});
                if (it != idx.end()) for (auto &e : it->second) if (near4(*e.first, rb.second)) owners.insert(e.second);
                if (owners.size() == 1 && std::find(C.begin(), C.end(), *owners.begin()) != C.end()) hits.add(*owners.begin());
            }
            auto mc = hits.most_common();
            if (mc.size() == 1 || (mc.size() > 1 && mc[1].second * 3 <= mc[0].second)) { to_add.push_back({mc[0].first, seg}); added++; }
            else rest.push_back(seg);
        }
        for (auto &ta : to_add) tx[ta.first].push_back({ta.second->i, ta.second->rows, "", false});
        loose = rest;
        if (added == n0) break;
    }
    pr.log(fmt("orphans by content: %d remaining %zu", added, loose.size()));
    // оставшиеся куски — группы по совпадающим рядам
    std::vector<int> parent(loose.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    std::map<std::pair<char, int>, std::vector<std::pair<int, const R40 *>>> buckets;
    for (size_t si = 0; si < loose.size(); si++)
        for (auto &rb : *loose[si]->rows)
            if (content_row(rb.first) && informative(rb.second)) buckets[{loose[si]->prev[0], rb.first}].push_back({(int)si, &rb.second});
    // связность «отличаются не более чем в 3 знаках»: одинаковые ряды склеиваются сразу, а пары кандидатов
    // ищутся по 4 блокам по 10 знаков (при ≤ 3 отличиях хотя бы один блок совпадает) — вместо перебора всех пар
    auto unite = [&](int a, int b) { int x = find(a), y = find(b); if (x != y) parent[x] = y; };
    for (auto &kv : buckets) {
        auto &items = kv.second;
        std::map<R40, int> uniq_of; std::vector<const R40 *> U; std::vector<int> owner;
        for (auto &it : items) {
            auto ins = uniq_of.insert({*it.second, (int)U.size()});
            if (ins.second) { U.push_back(it.second); owner.push_back(it.first); }
            else unite(it.first, owner[ins.first->second]);
        }
        for (int blk = 0; blk < 4; blk++) {
            std::unordered_map<std::string, std::vector<int>> by;
            for (size_t u = 0; u < U.size(); u++) by[std::string((const char *)U[u]->data() + blk * 10, 10)].push_back((int)u);
            for (auto &g : by) {
                auto &v = g.second;
                for (size_t i = 0; i < v.size(); i++)
                    for (size_t j = i + 1; j < v.size(); j++) {
                        if (find(owner[v[i]]) == find(owner[v[j]])) continue;
                        if (dist(*U[v[i]], *U[v[j]]) <= 3) unite(owner[v[i]], owner[v[j]]);
                    }
            }
        }
    }
    std::map<int, std::vector<int>> groups;
    for (size_t si = 0; si < loose.size(); si++) groups[find((int)si)].push_back((int)si);
    int gadded = 0;
    for (auto &g : groups) {
        if (g.second.size() < 2) continue;
        std::set<std::string> C; bool first = true;
        for (int si : g.second) {
            auto c = cand_pages(loose[si]->prev);
            std::set<std::string> cs(c.begin(), c.end());
            if (first) { C = cs; first = false; }
            else { std::set<std::string> x; for (auto &v : C) if (cs.count(v)) x.insert(v); C = x; }
        }
        if (C.size() == 1) {
            std::string pg = *C.begin();
            for (int si : g.second) { tx[pg].push_back({loose[si]->i, loose[si]->rows, "", false}); gadded++; }
        }
    }
    pr.log(fmt("orphans by candidate intersection: %d", gadded));
    for (auto &kv : tx) std::stable_sort(kv.second.begin(), kv.second.end(), [](const Tx &a, const Tx &b) { return a.i < b.i; });

    // подстраницы
    struct TT { long i; std::shared_ptr<SegRows> rows; std::string sub; };
    auto assign_subs = [&](const std::vector<Tx> &T) {
        std::vector<TT> out;
        Counter<std::string> main;
        for (auto &t : T) if (t.has_sub) main.add(t.sub);
        if (main.empty()) { for (auto &t : T) out.push_back({t.i, t.rows, "0000"}); return out; }
        std::set<std::string> kn;
        for (auto &kv : main.v) if (kv.second >= MIN_TX) kn.insert(kv.first);
        if (kn.empty()) kn.insert(main.top().first);
        if (kn.size() == 1) {
            std::string only = *kn.begin();
            for (auto &t : T) if (!t.has_sub || kn.count(t.sub)) out.push_back({t.i, t.rows, only});
            return out;
        }
        std::map<int, std::vector<std::pair<const R40 *, std::string>>> ref;
        for (auto &t : T)
            if (t.has_sub && kn.count(t.sub))
                for (auto &rb : *t.rows) if (content_row(rb.first) && informative(rb.second)) ref[rb.first].push_back({&rb.second, t.sub});
        std::string lastsub;
        for (auto &t : T) {
            if (t.has_sub) { if (kn.count(t.sub)) { lastsub = t.sub; out.push_back({t.i, t.rows, t.sub}); } continue; }
            Counter<std::string> hits;
            for (auto &rb : *t.rows) {
                if (!content_row(rb.first) || !informative(rb.second)) continue;
                std::set<std::string> own;
                for (auto &e : ref[rb.first]) if (near4(*e.first, rb.second)) own.insert(e.second);
                if (own.size() == 1) hits.add(*own.begin());
            }
            auto mc = hits.most_common();
            if (!mc.empty() && (mc.size() == 1 || mc[1].second * 3 <= mc[0].second)) out.push_back({t.i, t.rows, mc[0].first});
            else if (!lastsub.empty()) out.push_back({t.i, t.rows, lastsub});
        }
        return out;
    };

    auto build = [&](const std::vector<TT> &T) {
        struct Cl { const R40 *rep; std::vector<const R40 *> mem; };
        std::map<int, std::vector<Cl>> reps;
        std::vector<std::vector<std::pair<int, int>>> lab;
        for (auto &t : T) {
            std::vector<std::pair<int, int>> L;
            for (auto &rb : *t.rows) {
                auto &cl = reps[rb.first]; bool done = false;
                for (size_t ci = 0; ci < cl.size(); ci++)
                    if (dist(*cl[ci].rep, rb.second) <= 6) { cl[ci].mem.push_back(&rb.second); L.push_back({rb.first, (int)ci}); done = true; break; }
                if (!done) { cl.push_back({&rb.second, {&rb.second}}); L.push_back({rb.first, (int)cl.size() - 1}); }
            }
            lab.push_back(L);
        }
        struct Snap { std::vector<std::pair<int, int>> st; long t0, t1; int n; };
        auto st_get = [](const std::vector<std::pair<int, int>> &st, int r) { for (auto &p : st) if (p.first == r) return p.second; return -1; };
        auto st_set = [](std::vector<std::pair<int, int>> &st, int r, int ci) { for (auto &p : st) if (p.first == r) { p.second = ci; return; } st.push_back({r, ci}); };
        std::vector<std::pair<int, int>> state; std::vector<Snap> snaps;
        for (size_t k = 0; k < T.size(); k++) {
            bool changed = false;
            for (auto &rc : lab[k]) {
                int r = rc.first, ci = rc.second;
                if (r == 0) continue;
                if (st_get(state, r) != ci && (int)reps[r][ci].mem.size() >= MIN_ROW) {
                    if (st_get(state, r) >= 0) changed = true;
                    st_set(state, r, ci);
                    if (!snaps.empty() && !changed) st_set(snaps.back().st, r, ci);
                }
            }
            if (changed || snaps.empty()) snaps.push_back({state, T[k].i, T[k].i, 1});
            else { snaps.back().t1 = T[k].i; snaps.back().n++; }
        }
        for (int k = (int)snaps.size() - 2; k >= 0; k--)
            for (auto &rc : snaps[k + 1].st) if (st_get(snaps[k].st, rc.first) < 0) snaps[k].st.push_back(rc);
        std::vector<Snap> good;
        for (auto &s : snaps) if (s.n >= MIN_ROW) good.push_back(s);
        if (good.empty() && !snaps.empty()) {
            size_t b = 0; for (size_t k = 1; k < snaps.size(); k++) if (snaps[k].n > snaps[b].n) b = k;
            good.push_back(snaps[b]);
        }
        R40 hdr; hdr.fill(32);
        if (reps.count(0) && !reps[0].empty()) {
            auto &cl = reps[0]; size_t b = 0;
            for (size_t k = 1; k < cl.size(); k++) if (cl[k].mem.size() > cl[b].mem.size()) b = k;
            hdr = vote(cl[b].mem);
        }
        std::vector<Version> out;
        std::map<std::vector<std::pair<int, int>>, size_t> seen;
        for (auto &s : good) {
            auto key = s.st; std::sort(key.begin(), key.end());
            auto it = seen.find(key);
            if (it != seen.end()) { out[it->second].n += s.n; continue; }
            Version v; v.t = round(s.t0 / 650.0 * 10) / 10; v.n = s.n;
            Row h; std::copy(hdr.begin(), hdr.end(), h.begin()); v.rows[0] = h;
            for (auto &rc : s.st) {
                R40 r = vote(reps[rc.first][rc.second].mem);
                Row rr; std::copy(r.begin(), r.end(), rr.begin());
                v.rows[rc.first] = rr; v.c[rc.first] = (int)reps[rc.first][rc.second].mem.size();
            }
            seen[key] = out.size(); out.push_back(v);
        }
        return out;
    };
    PagesBuild res;
    std::map<int, int> nsub;
    for (auto &kv : tx) {
        if ((int)kv.second.size() < MIN_TX) continue;
        std::map<std::string, std::vector<TT>> by;
        for (auto &t : assign_subs(kv.second)) by[t.sub].push_back(t);
        std::vector<Version> out;
        for (auto &sb : by) {
            if ((int)sb.second.size() < MIN_TX && by.size() > 1) continue;
            auto snaps = build(sb.second);
            for (auto &s : snaps) s.s = sb.first;
            out.insert(out.end(), snaps.begin(), snaps.end());
        }
        if (!out.empty()) {
            std::set<std::string> ss; for (auto &v : out) ss.insert(v.s);
            nsub[std::min((int)ss.size(), 5)]++;
            res[kv.first] = out;
        }
    }
    // чужие ряды
    struct Occ { int c; std::string pid; Version *sn; };
    std::map<std::pair<int, Row>, std::vector<Occ>> occ;
    for (auto &kv : res)
        for (auto &sn : kv.second)
            for (auto &rb : sn.rows)
                if (content_row(rb.first) && informative(rb.second)) {
                    auto c = sn.c.find(rb.first);
                    occ[{rb.first, rb.second}].push_back({c == sn.c.end() ? 0 : c->second, kv.first, &sn});
                }
    int dropped = 0;
    for (auto &kv : occ) {
        auto &L = kv.second;
        int best = 0; for (auto &e : L) best = std::max(best, e.c);
        std::set<std::string> bestp, allp;
        for (auto &e : L) { allp.insert(e.pid); if (e.c == best) bestp.insert(e.pid); }
        if (allp.size() < 2) continue;
        for (auto &e : L)
            if (!bestp.count(e.pid) && e.c * 3 <= best) { e.sn->rows.erase(kv.first.first); e.sn->c.erase(kv.first.first); dropped++; }
    }
    pr.log(fmt("foreign rows removed: %d", dropped));
    std::map<int, int> nv;
    for (auto &kv : res) nv[std::min((int)kv.second.size(), 5)]++;
    std::string a, b;
    for (auto &x : nv) a += fmt("%s%d: %d", a.empty() ? "" : ", ", x.first, x.second);
    for (auto &x : nsub) b += fmt("%s%d: %d", b.empty() ? "" : ", ", x.first, x.second);
    pr.log(fmt("%zu pages; versions per page: {%s} ; subpages: {%s}", res.size(), a.c_str(), b.c_str()));
    return res;
}

// ---------------------------------------------------------------- объединение сборок (merge)
PagesBuild merge_pages(const PagesBuild &good, const PagesBuild &full, const Json &extras, Progress &pr) {
    const Json *clk = extras["_meta"].find("clock");
    auto set_clock = [&](Row h, double t) {
        if (!clk || !clk->is_arr() || clk->size() < 2) return h;
        long s = lround(clk->a[0].num() + clk->a[1].num() * t);
        std::string txt = fmt("%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
        if (h[34] == ':' && h[37] == ':') for (int k = 0; k < 8; k++) h[32 + k] = (u8)txt[k];
        return h;
    };
    PagesBuild out; long filled = 0, shared_filled = 0;
    std::set<std::string> pids;
    for (auto &kv : good) pids.insert(kv.first);
    for (auto &kv : full) pids.insert(kv.first);
    for (auto &pid : pids) {
        std::vector<Version> G = good.count(pid) ? good.at(pid) : std::vector<Version>();
        const std::vector<Version> F = full.count(pid) ? full.at(pid) : std::vector<Version>();
        std::set<std::string> gs; for (auto &s : G) gs.insert(s.s);
        for (auto &s : F) if (!gs.count(s.s)) { Version v; v.t = s.t; v.n = s.n; v.s = s.s; G.push_back(v); }
        std::set<int> shared;
        std::set<std::string> subs; for (auto &x : G) subs.insert(x.s);
        if (subs.size() > 1) {
            std::map<int, std::vector<const Row *>> byrow;
            for (const std::vector<Version> *L : {(const std::vector<Version> *)&G, &F}) for (auto &x : *L) for (auto &rb : x.rows) byrow[rb.first].push_back(&rb.second);
            for (auto &kv : byrow) {
                if (kv.first == 0) continue;
                auto &L = kv.second; size_t bi = 0; int bc = -1;
                for (size_t i = 0; i < L.size(); i++) {
                    int c = 0; for (auto *b : L) c += (*b == *L[i]);
                    if (c > bc) { bc = c; bi = i; }
                }
                bool all = true;
                for (auto *b : L) { int d = 0; for (int k = 0; k < 40; k++) d += (*b)[k] != (*L[bi])[k]; if (d > 3) { all = false; break; } }
                if (all) shared.insert(kv.first);
            }
        }
        std::vector<Version> res;
        for (auto &s : G) {
            Version v = s;
            const Version *f = nullptr;
            for (auto &x : F) if (x.s == s.s && (!f || fabs(x.t - s.t) < fabs(f->t - s.t))) f = &x;
            if (f) for (auto &rb : f->rows) if (!v.rows.count(rb.first)) {
                v.rows[rb.first] = rb.second; auto c = f->c.find(rb.first); v.c[rb.first] = c == f->c.end() ? 1 : c->second; filled++;
            }
            for (int r : shared) {
                if (v.rows.count(r)) continue;
                const Version *src = nullptr;
                for (const std::vector<Version> *L : {(const std::vector<Version> *)&G, &F}) for (auto &x : *L) if (x.rows.count(r) && (!src || fabs(x.t - s.t) < fabs(src->t - s.t))) src = &x;
                if (src) { v.rows[r] = src->rows.at(r); auto c = src->c.find(r); v.c[r] = c == src->c.end() ? 1 : c->second; filled++; shared_filled++; }
            }
            if (v.rows.count(0)) v.rows[0] = set_clock(v.rows[0], s.t);
            res.push_back(v);
        }
        std::stable_sort(res.begin(), res.end(), [](const Version &a, const Version &b) { return a.s != b.s ? a.s < b.s : a.t < b.t; });
        out[pid] = res;
    }
    long tot = 0; for (auto &kv : out) for (auto &s : kv.second) tot += (long)s.rows.size();
    pr.log(fmt("%zu pages; rows filled in %ld (%.0f%%), of which shared between subpages %ld", out.size(), filled, tot ? 100.0 * filled / tot : 0.0, shared_filled));
    return out;
}

// ---------------------------------------------------------------- карта качества ленты (quality)
Json quality_map(const std::string &vbi, const std::vector<Packet42> &st, const LineMap &lp, int lpf, const Json &extras, Progress &pr) {
    const int SPL = 2048, FPM = 25 * 60;
    MappedFile mf(vbi);
    long nfr = (long)(mf.size() / ((uint64_t)lpf * SPL));
    nfr = std::min<long>(nfr, lp.frames);
    const u8 *v = mf.data();
    auto line_std = [&](long f, int l) {
        const u8 *y = v + ((uint64_t)f * lpf + l) * SPL;
        double s = 0, s2 = 0;
        for (int k = 0; k < SPL; k++) { s += y[k]; s2 += (double)y[k] * y[k]; }
        double m = s / SPL; return sqrt(std::max(0.0, s2 / SPL - m * m));
    };
    std::vector<int> DATA;
    {
        long step = std::max(1L, nfr / 300); std::vector<int> cnt(lpf, 0); int ns = 0;
        for (long f = 0; f < nfr; f += step) { ns++; for (int l = 0; l < lpf; l++) cnt[l] += line_std(f, l) > 20; }
        for (int l = 0; l < lpf; l++) if (ns && cnt[l] > 0.02 * ns) DATA.push_back(l);
        if (DATA.empty()) for (int l = 0; l < lpf; l++) DATA.push_back(l);
    }
    std::vector<std::array<u8, 40>> badb(st.size());
    for (size_t i = 0; i < st.size(); i++) for (int k = 0; k < 40; k++) badb[i][k] = !odd_parity(st[i][2 + k]);
    const Json *clk = extras["_meta"].find("clock");
    Json rows = Json::array();
    for (long m0 = 0; m0 < nfr; m0 += FPM) {
        long m1 = std::min(nfr, m0 + FPM);
        long got = 0, exp = 0, unread = 0, nbad = 0, nby = 0; long kmin = LONG_MAX, kmax = -1;
        for (long f = m0; f < m1; f++) {
            for (int l : DATA) {
                bool sig = line_std(f, l) > 20, g = lp.at(f, l) >= 0;
                got += g; exp += (sig || g); unread += (sig && !g);
            }
            for (int l = 0; l < lpf; l++) {
                int32_t k = lp.at(f, l);
                if (k < 0 || k >= (int32_t)st.size()) continue;
                for (int q = 0; q < 40; q++) nbad += badb[k][q];
                nby += 40; kmin = std::min<long>(kmin, k); kmax = std::max<long>(kmax, k);
            }
            if ((f - m0) % 300 == 0) pr.progress(f, nfr);
        }
        Json r = Json::object();
        r["minute"] = (int)(m0 / FPM); r["from_s"] = round(m0 / 25.0 * 10) / 10; r["to_s"] = round(m1 / 25.0 * 10) / 10;
        r["received"] = round(10000.0 * got / std::max(1L, exp)) / 100;
        r["unreadable"] = round(10000.0 * unread / std::max(1L, exp)) / 100;
        r["parity"] = nby ? Json(round(100000.0 * nbad / nby) / 1000) : Json();
        if (clk && clk->size() >= 2 && kmax >= 0) {
            double a = clk->a[0].num(), b = clk->a[1].num();
            r["air"] = hms(a + b * kmin / 650.0) + "\xE2\x80\x93" + hms(a + b * kmax / 650.0);
        }
        pr.log(fmt("min %2d (%s): received %5.1f%%  unreadable %4.1f%%  errors %.3f%%", (int)(m0 / FPM), r.get_str("air").c_str(),
                   r["received"].num(), r["unreadable"].num(), r["parity"].num()));
        rows.push(r);
    }
    Json q = Json::object();
    q["stream"] = "stream.t42"; q["minutes"] = rows;
    return q;
}

// ---------------------------------------------------------------- проект
static std::pair<bool, std::pair<double, double>> field_skipping(const std::vector<Packet42> &st, const LineMap &lp, int lpf) {
    int half = lpf / 2;
    std::map<int, std::pair<int, std::pair<int, int>>> last;   // mag -> (row, (frame, half))
    long cnt[2][2] = {{0, 0}, {0, 0}};                            // in, edge
    for (int f = 0; f < lp.frames; f++)
        for (int l = 0; l < lpf; l++) {
            int32_t k = lp.at(f, l);
            if (k < 0 || k >= (int32_t)st.size()) continue;
            int a = H(st[k][0]), b = H(st[k][1]);
            if (a < 0 || b < 0) continue;
            int m = a & 7, r = (a >> 3) | (b << 1);
            if (r > 24) continue;
            auto fh = std::make_pair(f, l / half);
            auto it = last.find(m);
            if (it != last.end() && r != 0) {
                int pr = it->second.first; int kind = it->second.second == fh ? 0 : 1;
                cnt[kind][0]++;
                cnt[kind][1] += (r != pr + 1 && pr != 0) || (pr == 0 && r > 3);
            }
            last[m] = {r, fh};
        }
    double rin = cnt[0][0] ? (double)cnt[0][1] / cnt[0][0] : 0, redge = cnt[1][0] ? (double)cnt[1][1] / cnt[1][0] : 0;
    return {redge > 2 * std::max(rin, 0.01), {rin, redge}};
}

static bool sparse_stream(const std::vector<Packet42> &st) {
    std::map<std::tuple<int, int, int>, int> cnt;
    for (auto &p : st) {
        int a = H(p[0]), b = H(p[1]);
        if (a < 0 || b < 0 || ((a >> 3) | (b << 1)) != 0) continue;
        cnt[{a & 7, p[2], p[3]}]++;
    }
    if (cnt.empty()) return false;
    std::vector<double> v; for (auto &kv : cnt) v.push_back(kv.second);
    return median(v) < 6;
}
static long headers(const std::vector<Packet42> &st) {
    long n = 0;
    for (auto &p : st) { int a = H(p[0]), b = H(p[1]); if (a >= 0 && b >= 0 && ((a >> 3) | (b << 1)) == 0) n++; }
    return n;
}

void build_project(const std::string &t42_in, const std::string &out, const std::string &lines_npy, int lpf,
                   const std::string &vbi, Progress &pr) {
    std::string T42 = abspath(t42_in), SOURCE = T42;
    make_dirs(out);
    uint64_t sz = file_size(T42);
    if (ends_with_i(T42, ".t34") || (sz % 42 && !(sz % 34))) {
        pr.step("converting .t34 (525 lines, 32 characters per row) to .t42");
        std::string conv = path_join(out, "stream.t42");
        pr.log(fmt("packets %d", t34_to_t42(T42, conv, pr)));
        T42 = conv;
    }
    auto st = read_t42(T42);
    pr.step("service data: keys, flags, clock");
    Json ex = extract_extras(st, pr);
    save_json(path_join(out, "extras.json"), ex, 0);
    bool skip = false; Json rate;
    LineMap lp;
    if (!lines_npy.empty()) {
        lp = load_npy_i32(lines_npy);
        pr.step("checking for skipped fields");
        auto fsk = field_skipping(st, lp, lpf);
        skip = fsk.first;
        rate = Json::object(); rate["in"] = fsk.second.first; rate["edge"] = fsk.second.second;
        pr.log(fmt("row breaks: within a field %.1f%%, at boundaries %.1f%% -> %s", fsk.second.first * 100, fsk.second.second * 100,
                   skip ? "fields are skipped, building field-aware" : "continuous recording"));
    }
    if (headers(st) < 3) throw std::runtime_error("The stream has almost no page headers \xE2\x80\x94 there is nothing to build pages from.");
    bool sparse = sparse_stream(st);
    pr.step(std::string("building pages") + (sparse ? " (pages repeat rarely \xE2\x80\x94 a cleaned stream or a short recording: each page is taken from its first copy)" : ""));
    ExportOpts fo; fo.field_reset = false; fo.sparse = sparse;
    PagesBuild full = export_pages(st, fo, pr);
    PagesBuild good = full;
    if (skip) {
        ExportOpts go; go.sparse = sparse; go.lines = &lp; go.field = lpf / 2;
        good = export_pages(st, go, pr);
    }
    PagesBuild merged = merge_pages(good, full, ex, pr);
    save_json(path_join(out, "pages.json"), build_to_json(merged));
    if (!vbi.empty() && !lp.empty()) {
        pr.step("tape quality map");
        save_json(path_join(out, "quality.json"), quality_map(vbi, st, lp, lpf, ex, pr), 1);
    }
    Json old = load_json(path_join(out, "project.json"), Json::object());
    std::string charset = old.get_str("charset");
    const Json &em = ex["_meta"];
    if (charset.empty()) charset = em.get_str("g0");
    if (charset.empty()) {
        std::vector<const u8 *> rows;
        for (auto &kv : merged) if (!kv.second.empty()) for (auto &rb : kv.second[0].rows) if (rb.first != 0) rows.push_back(rb.second.data());
        charset = guess_charset(rows, (int)em.get_num("national", 0));
        std::string name = charset;
        for (auto &cn : CHARSET_NAMES) if (cn.first == charset) name = cn.second;
        pr.log("character set: " + name + " (guessed from the text; change it in the \xE2\x80\x9C" "Character set\xE2\x80\x9D menu)");
    }
    Json pj = Json::object();
    pj["source"] = abspath(!vbi.empty() ? vbi : SOURCE);
    pj["stream"] = T42;
    pj["lines"] = lines_npy.empty() ? Json() : Json(abspath(lines_npy));
    pj["field_skipping"] = skip;
    pj["break_rates"] = rate;
    pj["charset"] = charset;
    if (lpf != 32) pj["lpf"] = lpf;
    save_json(path_join(out, "project.json"), pj, 1);
    pr.step("done");
}

// ---------------------------------------------------------------- субтитры
std::vector<std::pair<std::string, int>> subtitle_pages(const std::string &t42) {
    auto st = read_t42(t42);
    std::map<std::string, int> res;
    for (auto &p : st) {
        int a = ham::fix[p[0]], b = ham::fix[p[1]];
        if (a < 0 || b < 0 || ((a >> 3) | (b << 1))) continue;
        int h[6]; for (int i = 0; i < 6; i++) h[i] = ham::fix[p[2 + i]];
        if (h[0] < 0 || h[1] < 0 || h[5] < 0 || !(h[5] & 8)) continue;
        int pn = h[0] | (h[1] << 4);
        if (pn == 0xFF || (pn & 0xF) > 9 || (pn >> 4) > 9) continue;
        res[fmt("%d%02X", (a & 7) ? (a & 7) : 8, pn)]++;
    }
    std::vector<std::pair<std::string, int>> v(res.begin(), res.end());
    std::stable_sort(v.begin(), v.end(), [](auto &x, auto &y) { return x.second > y.second; });
    return v;
}

std::vector<double> packet_times(size_t n, const std::string &lines, const std::string &vbi, int lpf, const Json *clock) {
    std::vector<double> t(n, NAN);
    if (!lines.empty() && exists(lines)) {
        LineMap lp = load_npy_i32(lines);
        std::vector<long long> frame(lp.frames);
        for (int f = 0; f < lp.frames; f++) frame[f] = f;
        if (!vbi.empty() && exists(vbi)) {
            MappedFile mf(vbi);
            long N = (long)(mf.size() / ((uint64_t)lpf * 2048));
            if (N == lp.frames && N > 1) {
                std::vector<long long> cnt(N);
                for (long f = 0; f < N; f++) { uint32_t c; memcpy(&c, mf.data() + (uint64_t)(f + 1) * lpf * 2048 - 4, 4); cnt[f] = c; }
                long ok = 0; std::vector<double> d;
                for (long f = 1; f < N; f++) { d.push_back((double)(cnt[f] - cnt[f - 1])); ok += cnt[f] - cnt[f - 1] >= 1; }
                if (ok > 0.99 * (N - 1) && median(d) == 1) for (long f = 0; f < N; f++) frame[f] = cnt[f] - cnt[0];
            }
        }
        for (int f = 0; f < lp.frames; f++)
            for (int l = 0; l < lp.lpf; l++) {
                int32_t k = lp.at(f, l);
                if (k >= 0 && (size_t)k < n) t[k] = (frame[f] * 2 + (l >= lpf / 2)) / 50.0;
            }
        double last = 0;
        for (size_t i = 0; i < n; i++) { if (std::isnan(t[i])) t[i] = last; else last = t[i]; }
        return t;
    }
    double b = (clock && clock->size() >= 2) ? clock->a[1].num() : 1.0;
    for (size_t i = 0; i < n; i++) t[i] = i / 650.0 * b;
    return t;
}

static std::string page_text_sub(const std::map<int, Row> &rows, const Charset &t, const Charset *t2, bool boxed_only) {
    Rows rr; for (auto &kv : rows) if (kv.first >= 1) rr[kv.first] = kv.second;
    auto cs = level1_cells(rr, t, t2, nullptr);
    std::map<int, std::map<int, std::string>> grid;
    for (auto &c : cs) {
        if (c.mosaic || c.conceal || (boxed_only && !c.box)) continue;
        std::string ch = c.text.empty() ? " " : c.text;
        if (!strip(ch).empty()) grid[c.r][c.c] = ch;
    }
    std::vector<std::string> out;
    for (auto &kv : grid) {
        std::string s; bool sp = true;
        for (int col = 0; col < 40; col++) {
            auto it = kv.second.find(col);
            if (it != kv.second.end()) { s += it->second; sp = false; }
            else if (!sp) { s += ' '; sp = true; }
        }
        s = rstrip(s);
        if (!s.empty()) out.push_back(s);
    }
    std::string r;
    for (size_t i = 0; i < out.size(); i++) r += (i ? "\n" : "") + out[i];
    return r;
}

std::vector<Cue> subtitle_cues(const std::string &t42, const std::string &page, const std::vector<double> &times,
                               const std::string &charset, const std::string &charset2) {
    auto st = read_t42(t42);
    size_t n = st.size();
    int want_m = (page[0] - '0') % 8, want_p = (int)strtol(page.substr(1).c_str(), nullptr, 16);
    const Charset *t2 = charset2.empty() ? nullptr : &charset_table(charset2, 0);
    struct Pg { int p; bool c4, c5, c6; int nat; std::map<int, Row> rows; double last; };
    std::map<int, Pg> opened;
    std::vector<Cue> out; bool has_cur = false; Cue cur{};
    auto clear = [&](double at) { if (has_cur && at > cur.a) out.push_back({cur.a, at, cur.text}); has_cur = false; };
    auto completed = [&](Pg &pg) {
        if (!pg.c6) { clear(pg.last); return; }
        std::string txt = page_text_sub(pg.rows, charset_table(charset, pg.nat), t2, pg.c5 || pg.c6);
        if (has_cur && cur.text == txt) return;
        clear(pg.last);
        if (!txt.empty()) { cur = {pg.last, 0, txt}; has_cur = true; }
    };
    auto terminate = [&](int m) {
        auto it = opened.find(m);
        if (it == opened.end()) return;
        Pg pg = it->second; opened.erase(it);
        if (pg.p == want_p && m == want_m) completed(pg);
    };
    for (size_t i = 0; i < n; i++) {
        const u8 *p = st[i].data();
        int a = ham::fix[p[0]], b = ham::fix[p[1]];
        if (a < 0 || b < 0) continue;
        int m = a & 7, row = (a >> 3) | (b << 1); double tm = times[i];
        if (row == 0) {
            int h[8]; for (int k = 0; k < 8; k++) h[k] = ham::fix[p[2 + k]];
            if (h[0] < 0 || h[1] < 0) continue;
            int pn = h[0] | (h[1] << 4);
            bool c4 = h[3] >= 0 && (h[3] & 8), c5 = h[5] >= 0 && (h[5] & 4), c6 = h[5] >= 0 && (h[5] & 8), serial = h[7] >= 0 && (h[7] & 1);
            if (serial) { std::vector<int> ms; for (auto &kv : opened) ms.push_back(kv.first); for (int mm : ms) terminate(mm); }
            else terminate(m);
            if (m == want_m && pn == want_p && (c4 || !c6)) clear(tm);
            if (pn == 0xFF) continue;
            opened[m] = {pn, c4, c5, c6, national_of(h[7] >= 0 ? h[7] : 0), {}, tm};
        } else if (row <= 24 && opened.count(m)) {
            Pg &pg = opened[m]; pg.last = tm;
            Row r; for (int k = 0; k < 40; k++) r[k] = odd_parity(p[2 + k]) ? (p[2 + k] & 0x7F) : 0x20;
            pg.rows[row] = r;
        }
    }
    std::vector<int> ms; for (auto &kv : opened) ms.push_back(kv.first);
    for (int m : ms) terminate(m);
    clear(n ? times[n - 1] : 0.0);
    std::vector<Cue> res;
    for (auto &c : out) if (c.b > c.a) res.push_back(c);
    return res;
}

static std::string srt_time(double s) {
    long long ms = llround(s * 1000);
    return fmt("%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}
std::string to_srt(const std::vector<Cue> &cs) {
    std::string out;
    for (size_t i = 0; i < cs.size(); i++)
        out += fmt("%zu\n", i + 1) + srt_time(cs[i].a) + " --> " + srt_time(cs[i].b) + "\n" + cs[i].text + "\n\n";
    return out;
}
