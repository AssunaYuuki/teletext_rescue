#include "syster.h"
#include "mlse.h"
#include "vbidecode.h"
#include <array>
#include <map>
#include <set>

namespace {
const double RATE = 284 * 15625.0;
const int W = 1600;                                    // строка в сетке 27 МГц (59 мкс: данные идут почти до конца)
const u8 SEQ[10] = {0x73, 0x9B, 0x5E, 0xB6, 0x49, 0xA1, 0x02, 0xEA, 0x15, 0xFD};

uint16_t crc(const u8 *d, size_t n) {
    uint16_t c = 0;
    while (n--) { c ^= *d++; for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xC003) : (uint16_t)(c >> 1); }
    return c;
}
// data: байты после 55 D0 18: 6C, номер части, 21 байт, CRC (2)
bool good(const u8 *d) { return d[0] == 0x6C && crc(d + 1, 22) == (uint16_t)(d[23] | (d[24] << 8)); }
uint16_t syndrome(const u8 *d) { return crc(d + 1, 22) ^ (uint16_t)(d[23] | (d[24] << 8)); }
bool in_seq(u8 s) { for (u8 v : SEQ) if (v == s) return true; return false; }
// CRC линеен: синдром одиночной ошибки однозначно указывает бит (192 бита части, номера, данных и CRC).
// Чиним один бит, если байт 6C и номер части после правки верны; 6C с одной ошибкой тоже принимаем.
bool fix1(u8 *d) {
    static const std::map<uint16_t, int> tab = [] {
        std::map<uint16_t, int> t; u8 z[25] = {}; uint16_t s0 = syndrome(z);
        for (int i = 8; i < 25 * 8; i++) { z[i / 8] ^= 1 << (i % 8); t[syndrome(z) ^ s0] = i; z[i / 8] ^= 1 << (i % 8); }
        return t;
    }();
    if (__builtin_popcount(d[0] ^ 0x6C) > 1) return false;
    uint16_t s = syndrome(d);
    if (s == 0) { if (!in_seq(d[1])) return false; d[0] = 0x6C; return true; }
    if (d[0] != 0x6C) return false;                      // две ошибки — не трогаем
    auto it = tab.find(s); if (it == tab.end()) return false;
    int i = it->second; d[i / 8] ^= 1 << (i % 8);
    if (!in_seq(d[1])) { d[i / 8] ^= 1 << (i % 8); return false; }
    return true;
}

MlseModel model() {
    MlseModel m;
    m.spb = 27e6 / RATE; m.pre[0] = 0x55; m.pre[1] = 0xD0; m.pre[2] = 0x18; m.nbytes = 26;            // +1 байт после CRC: правый контекст для последнего бита
    m.L = 3; m.R = 3; m.ntop = 2; m.refits = 1; m.good = good;
    return m;
}
void line27(const Rec &R, int u, int r, float *out) {
    auto y = R.line(u, r);
    double mx = 1; for (float v : y) mx = std::max(mx, (double)v);
    for (int i = 0; i < W; i++) out[i] = (float)std::clamp(interp(i / 27e6 * R.fs, y.data(), (int)y.size()) * (255.0 / mx), 0.0, 255.0);
}
double find_pre(const float *y, const MlseModel &M) {
    int bits[24]; for (int i = 0; i < 24; i++) bits[i] = ((M.pre[i / 8] >> (i % 8)) & 1) ? 1 : -1;
    double best = 0, boff = -1;
    for (double off = 0; off <= 300; off += 0.5) {
        double s = 0, m = 0, e = 0, v[24];
        for (int i = 0; i < 24; i++) { v[i] = interp(off + (i + 0.5) * M.spb, y, W); m += v[i]; }
        m /= 24;
        for (int i = 0; i < 24; i++) { s += (v[i] - m) * bits[i]; e += (v[i] - m) * (v[i] - m); }
        double c = e > 0 ? s / sqrt(e * 24) : 0;
        if (c > best) { best = c; boff = off; }
    }
    return best > 0.5 ? boff : -1;
}
// ---- шифр ECM (вариант DES со своими таблицами; ключ 8 байт, ECM 16 байт = две половины по 8)
namespace sdes {
const u8 LS[16] = {1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 0};
const u8 S[128] = {
    0x1F, 0xB0, 0x28, 0xEB, 0xD1, 0x0D, 0x42, 0x7E, 0xC5, 0x59, 0x93, 0x34, 0xA6, 0x6A, 0xFC, 0x87,
    0xB0, 0xE3, 0x17, 0x7D, 0x2B, 0x96, 0xDE, 0x48, 0x0A, 0x34, 0x6C, 0x81, 0xC5, 0x5F, 0xA9, 0xF2,
    0x2E, 0xD0, 0x72, 0xB7, 0x95, 0x0C, 0x48, 0xEB, 0x53, 0x6A, 0xC9, 0x14, 0xAF, 0xF1, 0x36, 0x8D,
    0x8D, 0x4E, 0xB1, 0xE8, 0x6B, 0x35, 0x17, 0xD2, 0xF0, 0x93, 0x56, 0x2F, 0x0C, 0xCA, 0xA9, 0x74,
    0xB2, 0x4F, 0xD4, 0x18, 0x0B, 0xF6, 0x7E, 0x25, 0xC1, 0x3C, 0x6A, 0x83, 0xAD, 0x50, 0x97, 0xE9,
    0xE9, 0xB4, 0x42, 0x27, 0x3E, 0xCB, 0x85, 0x18, 0x56, 0x0A, 0x9F, 0x70, 0xF1, 0xAD, 0x6C, 0xD3,
    0x35, 0xE0, 0x5B, 0x0D, 0x68, 0xD3, 0x96, 0x7A, 0xF9, 0x2E, 0xC2, 0xB1, 0x1F, 0x84, 0xAC, 0x47,
    0x6B, 0x1C, 0x0D, 0xA3, 0xD6, 0x7A, 0x30, 0xC5, 0x84, 0xF1, 0xBE, 0x58, 0xE9, 0x2F, 0x47, 0x92};
const u8 S2[128] = {
    0xD1, 0x34, 0xBD, 0xE3, 0x8B, 0x58, 0x42, 0x9E, 0x7A, 0xAF, 0xC0, 0x05, 0x2C, 0xF6, 0x17, 0x69,
    0xB4, 0xD7, 0xE3, 0x48, 0x5E, 0x21, 0x8D, 0x72, 0x09, 0x60, 0x3F, 0xA6, 0x95, 0xCB, 0xFA, 0x1C,
    0x82, 0x27, 0x14, 0xCA, 0xF9, 0x90, 0x6F, 0x5C, 0xEB, 0xD8, 0x7D, 0xA3, 0x4E, 0x35, 0xB1, 0x06,
    0x5C, 0x90, 0x6F, 0xF9, 0x35, 0x4E, 0x82, 0x27, 0x06, 0xEB, 0xCA, 0x14, 0xA3, 0xD8, 0x7D, 0xB1,
    0x52, 0xF8, 0x6F, 0x16, 0x9C, 0xCB, 0x09, 0xA5, 0xED, 0x27, 0x3A, 0x81, 0x43, 0xB4, 0xD0, 0x7E,
    0x2E, 0x95, 0xB2, 0x6F, 0x79, 0x06, 0xC7, 0xF8, 0x4B, 0xE0, 0xD1, 0x3C, 0xA4, 0x5A, 0x1D, 0x83,
    0x0C, 0xE2, 0x7B, 0x18, 0x90, 0x4D, 0xC7, 0xB1, 0x63, 0x8F, 0xDE, 0x25, 0x39, 0xF6, 0xA4, 0x5A,
    0xF2, 0x17, 0x85, 0x4E, 0x5C, 0xB0, 0x2B, 0xED, 0xA4, 0x79, 0x38, 0x93, 0x6F, 0xCA, 0xD1, 0x06};
const u8 KC[48] = {28, 31, 24, 10, 18, 14, 7, 26, 4, 21, 11, 16, 12, 27, 15, 9, 20, 30, 5, 13, 23, 29, 8, 19,
                   62, 51, 44, 37, 57, 48, 56, 38, 60, 52, 45, 41, 54, 43, 58, 49, 36, 61, 59, 63, 40, 53, 47, 50};
const u8 KE[48] = {31, 0, 1, 2, 3, 4, 3, 4, 5, 6, 7, 8, 7, 8, 9, 10, 11, 12, 11, 12, 13, 14, 15, 16,
                   15, 16, 17, 18, 19, 20, 19, 20, 21, 22, 23, 24, 23, 24, 25, 26, 27, 28, 27, 28, 29, 30, 31, 0};
const u8 P[32] = {0x31, 0x12, 0x50, 0x33, 0x13, 0x21, 0x42, 0x00, 0x51, 0x52, 0x30, 0x43, 0x53, 0x70, 0x22, 0x03,
                  0x73, 0x62, 0x41, 0x60, 0x23, 0x20, 0x02, 0x01, 0x61, 0x63, 0x40, 0x32, 0x10, 0x11, 0x71, 0x72};
const int KP[8] = {0, 3, 2, 1, 4, 5, 6, 7}, IP[8] = {4, 0, 5, 1, 6, 2, 7, 3}, FP[8] = {7, 3, 6, 2, 5, 1, 4, 0};

u8 sbox(int i) { return i < 128 ? S[i] : S2[i - 128]; }
void permute(const u8 *in, u8 *out, const int *p) {
    u8 t[8]; memcpy(t, in, 8);
    for (int j = 7; j >= 0; j--)
        for (int i = 0; i < 8; i++) {
            if (p[0] & 3) { out[j] = (u8)(out[j] << 1 | (t[p[i]] & 1)); t[p[i]] >>= 1; }
            else { out[p[i]] = (u8)(out[p[i]] >> 1 | (t[j] & 1 ? 0x80 : 0)); t[j] >>= 1; }
        }
}
void expand(const u8 *e, const u8 *d, u8 *r) {
    for (int j = 0; j < 8; j++) {
        r[j] = 0;
        for (int i = 6; i > 0; i--) {
            r[j] <<= 1;
            int b = e[(7 - j) * 6 + i - 1] & (e[0] == 31 ? 0x1F : 0xFF);
            if (d[b >> 3] & (1 << (b & 7))) r[j] |= 1;
        }
    }
}
void rotate(int round, u8 *k) {
    for (int n = 0; n < LS[round]; n++) {
        for (int j = 0; j < 3; j++) { k[j] = (u8)(k[j] >> 1 | (k[j + 1] & 1) << 7); k[j + 4] = (u8)(k[j + 4] >> 1 | (k[j + 5] & 1) << 7); }
        k[3] = (u8)(k[3] >> 1 | ((k[0] >> 3) & 1) << 7);
        k[7] = (u8)(k[7] >> 1 | ((k[4] >> 3) & 1) << 7);
    }
}
void rounds(u8 *k, u8 *cw, bool enc) {
    u8 kr[16][8];
    for (int i = 0; i < 16; i++) { memcpy(kr[i], k, 8); rotate(i, k); }
    for (int i = 0; i < 16; i++) {
        u8 ek[8], ecw[8], r[4] = {0, 0, 0, 0};
        expand(KC, kr[enc ? 15 - i : i], ek);
        expand(KE, cw, ecw);
        for (int j = 31, c = 0; c < 8; c++) {
            u8 x = (ek[c] ^ ecw[c]) & 0x3F;
            u8 sb = sbox((x >> 1) | ((0x20 * (8 - c)) & 0xFF));
            if (x & 1) sb = (u8)(sb << 4);
            for (int l = 0; l < 4; l++, j--) {
                int b = P[j] & 3; u8 m = (u8)(1 << ((P[j] >> 4) & 7));
                r[b] = sb & 0x80 ? (u8)(r[b] & ~m) : (u8)(r[b] | m);
                sb <<= 1;
            }
        }
        for (int l = 0; l < 4; l++) { r[l] ^= cw[l + 4]; cw[l + 4] = cw[l]; cw[l] = r[l]; }
    }
}
// одна половина ECM (8 байт) через шифр
void half(const u8 *in, const u8 *key, bool enc, u8 *out) {
    u8 k[8] = {}, c[8] = {};
    permute(key, k, KP); k[0] = (u8)(k[4] << 4);
    permute(in, c, IP);
    rounds(k, c, enc);
    memset(out, 0, 8); permute(c, out, FP);
}
// расшифровка ECM: открытый текст 16 байт и контрольное слово (60 бит)
uint64_t decrypt(const u8 *ecm, const u8 *key, u8 *plain) {
    half(ecm, key, false, plain); half(ecm + 8, key, false, plain + 8);
    u8 b[8]; memcpy(b, plain + 4, 4); memcpy(b + 4, plain + 12, 4);
    u8 w[8];
    for (int i = 0; i < 4; i++) {
        w[i] = b[i + 4] & (i == 3 ? 0x7F : 0xFF);
        w[i + 4] = (u8)(((b[i] << 1) & (i == 3 ? 0x1F : 0xFF)) | ((b[i == 0 ? 7 : i - 1] >> 7) & 1));
    }
    uint64_t cw = 0; for (int i = 0; i < 8; i++) cw |= (uint64_t)w[i] << (8 * i);
    return cw;
}
}

// известные ключи: свободный доступ (общий для всех операторов), Premiere, Canal+ France
struct KnownKey { const char *name; u8 k[8]; };
const KnownKey KEYS[] = {
    {"the free-access key", {0xC4, 0xA5, 0xA8, 0x18, 0x74, 0x93, 0xC7, 0x65}},
    {"the Premiere (Germany) key", {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34}},
    {"the Canal+ France key", {0x00, 0xAE, 0x52, 0x90, 0x49, 0xF1, 0xF1, 0xBB}},
};

std::string hexs(const u8 *d, int n) { std::string o; for (int i = 0; i < n; i++) o += fmt("%02X", d[i]); return o; }
std::string tstamp(double s) { return fmt("%02d:%02d:%04.1f", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)); }

// разбор блоков: флаги, ECM (расшифровка ключом свободного доступа), EMM с адресами карт
std::string analyse_blocks(const std::vector<std::pair<int, std::array<u8, 168>>> &msgs, double rate) {
    std::string t;
    if (msgs.empty()) return t;
    auto m1 = [](const std::array<u8, 168> &b) { return b.data(); };
    auto m2 = [](const std::array<u8, 168> &b) { return b.data() + 84; };
    // флаги
    std::map<int, int> flags, aud, op;
    for (auto &m : msgs) {
        flags[m1(m.second)[0]]++;
        aud[(m1(m.second)[1] >> 3) | ((m1(m.second)[0] & 1) << 5)]++;
        op[m2(m.second)[0] << 16 | m2(m.second)[1] << 8 | m2(m.second)[2]]++;
    }
    t += "\nBlock contents (" + std::to_string(msgs.size()) + " complete blocks)\n";
    for (auto &kv : flags) {
        int f = kv.first;
        t += fmt("  control byte %02X in %d blocks: ", f, kv.second);
        t += std::string(f & 2 ? "Syster line shuffling" : "Discret 11 line delays") + (f & 0x20 ? ", picture scrambled" : ", picture in clear");
        if (f & 0x20) t += f & 0x10 ? ", sound spectrum inverted at 12.8 kHz" : ", sound not inverted";
        t += f & 8 ? ", key table 2" : ", key table 1";
        if (f & 4) t += ", cut-and-rotate";
        t += "\n";
    }
    for (auto &kv : aud) t += fmt("  audience %02X in %d blocks", kv.first, kv.second) + (kv.first == 0x11 ? " (free access: any decoder with a key)\n" : kv.first == 0 ? " (subscribers only)\n" : "\n");
    for (auto &kv : op) t += fmt("  message 2 bytes 0-2 = %02X %02X %02X in %d blocks\n", kv.first >> 16, (kv.first >> 8) & 255, kv.first & 255, kv.second);
    // ECM: каждая половина (8 байт) приходит в своём блоке; номер c: c/2 — ECM, c&1 — половина
    struct Ecm { int u; int c; int ap; u8 e[16]; bool dec = false; u8 plain[16]; uint64_t cw = 0; };
    std::vector<Ecm> ecms;
    std::map<int, std::pair<int, std::array<u8, 8>>> halves;
    int changes = 0; std::vector<int> change_u;
    for (auto &m : msgs) {
        const u8 *a = m1(m.second);
        int c = ((a[1] & 7) << 4) | (a[2] >> 4), ap = a[2] & 15;
        std::array<u8, 8> h; memcpy(h.data(), a + 3, 8);
        halves[c] = {m.first, h};
        if (ap == 0) { changes++; change_u.push_back(m.first); }
        if (c & 1) {
            auto it = halves.find(c - 1);
            if (it != halves.end() && m.first - it->second.first < 200) {
                Ecm e; e.u = m.first; e.c = c >> 1; e.ap = ap;
                memcpy(e.e, it->second.second.data(), 8); memcpy(e.e + 8, h.data(), 8);
                ecms.push_back(e);
            }
        }
    }
    // ключ верный, если почти все ECM дают одинаковые данные программы (байты 4–11)
    std::string best; int nb = 0; const KnownKey *key = nullptr;
    for (auto &K : KEYS) {
        std::map<std::string, int> pdata;
        for (auto &e : ecms) { e.cw = sdes::decrypt(e.e, K.k, e.plain); pdata[hexs(e.plain + 4, 8)]++; }
        for (auto &kv : pdata) if (kv.second > nb) { nb = kv.second; best = kv.first; }
        if (!ecms.empty() && nb >= 0.8 * ecms.size() && nb >= 3) { key = &K; break; }
        nb = 0; best.clear();
    }
    bool fa = key != nullptr;
    std::set<std::string> distinct; for (auto &e : ecms) distinct.insert(hexs(e.e, 16));
    t += fmt("\nECM (entitlement control messages): %zu, %zu different; the control word changes %d times", ecms.size(), distinct.size(), changes);
    if (change_u.size() > 2) t += fmt(" (every %.1f s)", (change_u.back() - change_u.front()) / rate / (change_u.size() - 1));
    t += "\n";
    if (fa) {
        const u8 *d = ecms[0].plain + 4;
        for (auto &e : ecms) if (hexs(e.plain + 4, 8) == best) { d = e.plain + 4; break; }
        for (auto &e : ecms) e.dec = hexs(e.plain + 4, 8) == best;
        t += std::string("  decrypted with ") + key->name + " (" + std::to_string(nb) + " of " + std::to_string(ecms.size()) + " give the same programme data):\n";
        t += fmt("  programme data %s: window %02X, channel %02X, audience %02X%s, byte 3 %02X", best.c_str(), d[0], d[1], d[2], d[2] == 0x11 ? " (free access)" : "", d[3]);
        auto date = [](int v) -> std::string {
            if (!(v & 0x8000) || v == 0xFFFF) return v == 0xFFFF ? "open (FFFF)" : "not set";
            int h = (v >> 8) & 1, m = (v >> 5) & 7, day = v & 31;
            return fmt("%02d.%02d.%d", day, h ? m + 6 : m, 1990 + ((v >> 9) & 0x3F));
        };
        t += ", date " + date(d[4] | d[5] << 8) + ", PPV date " + date(d[6] | d[7] << 8) + "\n";
        t += "  each ECM also carries a 60-bit control word that seeds the line-shuffling generator (listed below)\n";
    } else if (!ecms.empty()) {
        t += "  not readable with any known key: the operator's subscription key is needed\n";
    }
    // EMM: 72 байта в каждом сообщении с 12-го; FF FF FF в адресе — заполнитель
    struct Emm { int u, which; std::array<u8, 72> d; };
    std::vector<Emm> emms; std::set<std::string> seen; int fill = 0;
    for (auto &m : msgs)
        for (int w = 0; w < 2; w++) {
            const u8 *e = (w ? m2(m.second) : m1(m.second)) + 12;
            if (e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF) { fill++; continue; }
            std::string k = hexs(e, 72); if (!seen.insert(k).second) continue;
            Emm x; x.u = m.first; x.which = w; memcpy(x.d.data(), e, 72); emms.push_back(x);
        }
    std::map<int, int> addr;
    for (auto &e : emms) addr[e.d[1] | e.d[2] << 8 | e.d[3] << 16]++;
    t += fmt("\nEMM (entitlement management messages, 72 bytes: 00, 24-bit card address, 64 encrypted bytes, 4-byte signature): %zu different, %d fillers\n", emms.size(), fill);
    if (!addr.empty()) {
        std::map<int, int> grp; for (auto &kv : addr) grp[kv.first >> 16]++;
        t += fmt("  %zu card addresses %06X-%06X; groups:", addr.size(), addr.begin()->first, addr.rbegin()->first);
        for (auto &kv : grp) t += fmt(" %02Xxxxx (%d)", kv.first, kv.second);
        std::map<int, int> per; for (auto &kv : addr) per[kv.second]++;
        t += "\n  messages per card:"; for (auto &kv : per) t += fmt(" %dx%d", kv.first, kv.second);
        t += "\n";
    }
    // списки
    t += "\nECM list (time, ECM number, flag, encrypted ECM" + std::string(fa ? ", decrypted, control word" : "") + "; flag 0 = this control word is now in use)\n";
    for (auto &e : ecms) {
        t += tstamp(e.u / rate) + fmt("  %02X %X  ", e.c, e.ap) + hexs(e.e, 16);
        if (fa) t += "  " + hexs(e.plain, 16) + fmt("  %016llX", (unsigned long long)e.cw) + (e.dec ? "" : "  (?)");
        t += "\n";
    }
    if (!emms.empty()) {
        t += "\nEMM list (time, card address, message)\n";
        for (auto &e : emms) t += tstamp(e.u / rate) + fmt("  %06X  ", e.d[1] | e.d[2] << 8 | e.d[3] << 16) + hexs(e.d.data(), 72) + "\n";
    }
    return t;
}
}

double syster_check(const Rec &R, int row, const std::vector<int> &uu) {
    MlseDecoder dec(model());
    int ok = 0, n = 0; std::vector<float> y(W);
    for (int u : uu) {
        line27(R, u, row, y.data());
        double o = find_pre(y.data(), dec.M);
        n++;
        if (o < 0) continue;
        auto q = dec.decode(y.data(), W, std::max(0.0, o - 6), o + 6, 0.5);
        if (!q.good && q.data.size() >= 25) { u8 d[25]; memcpy(d, q.data.data(), 25); q.good = fix1(d); }
        ok += q.good;
    }
    return round(1000.0 * ok / std::max(1, n)) / 1000;
}

int syster_report(const Rec &R, const std::vector<int> &rows, const std::string &out_txt, Progress &pr) {
    MlseModel M = model();
    MlseDecoder dec(M);
    std::vector<double> offs; std::vector<float> y(W);
    pr.step("Reading Nagravision (Syster) access data");
    for (int u = 0; u < R.n && offs.size() < 300; u += std::max(1, R.n / 600))
        for (int r : rows) {
            line27(R, u, r, y.data());
            double o = find_pre(y.data(), M); if (o < 0) continue;
            auto q = dec.decode(y.data(), W, std::max(0.0, o - 6), o + 6, 0.5);
            if (q.good) offs.push_back(q.off);
        }
    if (offs.empty() || !dec.has_prior()) { pr.log("Syster: no readable lines"); return 0; }
    double c = median(offs), lo = std::max(0.0, c - 8), hi = c + 8;
    MlseBatch bd(M, dec.prior, dec.n, true, pr);
    struct Pk { int u, line; std::array<u8, 25> d; bool ok, fixed = false; double pre; };
    std::vector<Pk> pk;
    std::vector<std::pair<int, int>> FL;
    for (int u = 0; u < R.n; u++) for (int r : rows) FL.push_back({u, r});
    std::vector<float> buf;
    for (size_t b = 0; b < FL.size(); b += bd.batch()) {
        size_t n = std::min(bd.batch(), FL.size() - b);
        buf.resize(n * W);
        std::vector<double> po(n);
        parallel_for(n, [&](size_t i) {
            float *o = &buf[i * W];
            line27(R, FL[b + i].first, FL[b + i].second, o);
            // своя преамбула у каждой строки: сдвигаем так, чтобы она легла в c
            po[i] = find_pre(o, M);
            if (po[i] >= 0) {
                std::vector<float> t(o, o + W);
                double sh = po[i] - c;
                for (int k = 0; k < W; k++) o[k] = (float)interp(k + sh, t.data(), W);
            }
        }, &pr, 64);
        std::vector<const float *> rp(n); for (size_t i = 0; i < n; i++) rp[i] = &buf[i * W];
        auto res = bd.decode(rp, W, lo, hi, nullptr, 0.5);
        for (size_t i = 0; i < n; i++) {
            Pk p; p.u = FL[b + i].first; p.line = R.tv.at(FL[b + i].second); p.ok = res[i].good; p.pre = po[i];
            for (int k = 0; k < 25; k++) p.d[k] = k < (int)res[i].data.size() ? res[i].data[k] : 0;
            p.fixed = !p.ok && po[i] >= 0 && fix1(p.d.data());
            if (p.fixed) p.ok = true;
            pk.push_back(p);
        }
        pr.progress(FL[b + n - 1].first, R.n);
        if (pr.cancel) break;
    }
    if (const char *dp = getenv("TR_SYSDUMP")) {           // отладка: все пакеты в текст
        std::string o;
        for (auto &p : pk) {
            o += fmt("%d %d %d %.1f ", p.u, p.line, (int)p.ok, p.pre);
            for (u8 v : p.d) o += fmt("%02X", v);
            o += "\n";
        }
        write_text(dp, o);
    }
    // блоки: части по номеру; две части XOR (0 и 1) восстанавливают одну потерянную чётную / нечётную
    auto seq_index = [](u8 s) { for (int i = 0; i < 10; i++) if (SEQ[i] == s) return i; return -1; };
    std::map<int, std::pair<int, int>> per_line;    // строка -> (прочитано, верных)
    int okn = 0;
    int nfix = 0;
    for (auto &p : pk) { auto &q = per_line[p.line]; q.first++; if (p.ok) { q.second++; okn++; } nfix += p.fixed; }
    struct Block { int u; std::array<std::array<u8, 21>, 10> part; int have = 0; int recovered = 0; };
    std::vector<Block> blocks;
    Block cur{}; int last = -1; bool open = false;
    auto close = [&]() {
        if (!open) return;
        for (int par = 0; par < 2; par++) {              // XOR-восстановление
            int miss = -1, nm = 0;
            for (int i = par; i < 10; i += 2) if (!(cur.have >> i & 1)) { miss = i; nm++; }
            if (nm == 1) {
                std::array<u8, 21> x{};
                for (int i = par; i < 10; i += 2) if (i != miss) for (int k = 0; k < 21; k++) x[k] ^= cur.part[i][k];
                cur.part[miss] = x; cur.have |= 1 << miss; cur.recovered++;
            }
        }
        blocks.push_back(cur); open = false;
    };
    for (auto &p : pk) {
        if (!p.ok) continue;
        int si = seq_index(p.d[1]); if (si < 0) continue;
        if (!open || si <= last) { close(); cur = Block{}; cur.u = p.u; open = true; }
        memcpy(cur.part[si].data(), &p.d[2], 21); cur.have |= 1 << si; last = si;
    }
    close();
    int full = 0, rec = 0;
    for (auto &b : blocks) if (b.have == 0x3FF) { full++; rec += b.recovered > 0; }
    double rate = (R.ntsc() ? 30000.0 / 1001 : 25.0) * (R.unit == "field" ? 2 : 1);
    std::string txt = "Nagravision (Syster) access data in " + basename(R.path) + "\n";
    txt += "NRZ 284 fH, 28 bytes per line: sync 55 D0 18 6C, part number, 21 data bytes, CRC-16. 10 parts make a block:\n"
           "two 84-byte messages (conditional-access ECM/EMM, encrypted) and two XOR parts that restore one lost part.\n\n";
    for (auto &kv : per_line) {
        std::vector<double> po; int nopre = 0;
        for (auto &p : pk) if (p.line == kv.first) { if (p.pre < 0) nopre++; else po.push_back(p.pre / 27.0); }
        std::sort(po.begin(), po.end());
        auto q = [&](double f) { return po.empty() ? 0.0 : po[(size_t)(f * (po.size() - 1))]; };
        txt += fmt("line %d: %d read, %d with a correct CRC (%.0f%%); no sync %d; sync at %.2f us (5-95%%: %.2f-%.2f)\n", kv.first, kv.second.first, kv.second.second,
                   100.0 * kv.second.second / std::max(1, kv.second.first), nopre, q(0.5), q(0.05), q(0.95));
    }
    txt += fmt("(%d of them repaired: one wrong bit found from the CRC)\n", nfix);
    txt += fmt("\nblocks %zu, complete %d (%d completed from the XOR parts)\n\n", blocks.size(), full, rec);
    std::vector<std::pair<int, std::array<u8, 168>>> msgs;
    for (auto &b : blocks) {
        if (b.have != 0x3FF) continue;
        std::array<u8, 168> m; int k = 0;
        for (int i : {8, 9, 6, 7, 4, 5, 2, 3}) for (u8 v : b.part[i]) m[k++] = v;
        msgs.push_back({b.u, m});
    }
    txt += analyse_blocks(msgs, rate);
    txt += "\nAll different blocks (message 1 = control byte, ECM part, EMM 1; message 2 = operator bytes, EMM 2)\n";
    std::map<std::string, int> seen;
    for (auto &b : blocks) {
        if (b.have != 0x3FF) continue;
        std::string m1, m2;
        for (int i : {8, 9, 6, 7}) for (u8 v : b.part[i]) m1 += fmt("%02X", v);
        for (int i : {4, 5, 2, 3}) for (u8 v : b.part[i]) m2 += fmt("%02X", v);
        if (seen[m1 + m2]++) continue;
        double s = b.u / rate;
        txt += fmt("%02d:%02d:%04.1f  message 1 ", (int)(s / 3600), (int)(s / 60) % 60, fmod(s, 60)) + m1 + "\n            message 2 " + m2 + "\n";
    }
    write_text(out_txt, txt);
    pr.log(fmt("Syster: %d lines with a correct CRC, %zu blocks (%d complete)", okn, blocks.size(), full));
    return okn;
}
