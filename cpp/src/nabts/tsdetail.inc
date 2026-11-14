// What a stream itself says (dvbts.cpp, ts_info): the picture of an MPEG-2 / H.264 video, the format of MPEG / AC-3 /
// AAC sound, from the first bytes of their PES packets. Each returns "" when it cannot tell.
namespace {
std::string mpeg2Video(const Bytes& v)
{
    for (size_t i = 0; i + 12 < v.size(); i++) {
        if (v[i] || v[i + 1] || v[i + 2] != 1 || v[i + 3] != 0xB3) continue;
        const u8* s = &v[i + 4];
        int w = s[0] << 4 | s[1] >> 4, h = (s[1] & 15) << 8 | s[2], ar = s[3] >> 4, fr = s[3] & 15;
        long br = (long)(s[4] << 10 | s[5] << 2 | s[6] >> 6) * 400;
        static const char* ars[5] = {"", "square pixels", "4:3", "16:9", "2.21:1"};
        static const double fps[9] = {0, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60};
        return fmt("%dx%d", w, h) + (ar >= 1 && ar <= 4 ? std::string(", ") + ars[ar] : "") + (fr >= 1 && fr <= 8 ? fmt(", %g frames/s", fps[fr]) : "") +
               (br ? fmt(", up to %.1f Mbit/s", br / 1e6) : "");
    }
    return "";
}
// H.264: the sequence parameter set (profile, level, size)
struct Bits {
    std::vector<u8> d; size_t p = 0; bool bad = false;
    int u(int n) { int v = 0; while (n--) { if (p / 8 >= d.size()) { bad = true; return 0; } v = v << 1 | (d[p / 8] >> (7 - p % 8) & 1); p++; } return v; }
    int ue() { int z = 0; while (!u(1)) { if (bad || ++z > 31) { bad = true; return 0; } } return (1 << z) - 1 + u(z); }
    int se() { int k = ue(); return k & 1 ? (k + 1) / 2 : -(k / 2); }
};
std::string h264Video(const Bytes& v)
{
    for (size_t i = 0; i + 8 < v.size(); i++) {
        if (v[i] || v[i + 1] || v[i + 2] != 1 || (v[i + 3] & 0x1F) != 7) continue;
        Bits b;
        for (size_t k = i + 4; k < v.size() && k < i + 200; k++) {   // without the emulation prevention bytes
            if (k >= 2 && v[k] == 3 && v[k - 1] == 0 && v[k - 2] == 0) continue;
            b.d.push_back(v[k]);
        }
        int prof = b.u(8); b.u(8); int lev = b.u(8); b.ue();
        const char* pn = prof == 66 ? "Baseline" : prof == 77 ? "Main" : prof == 88 ? "Extended" : prof == 100 ? "High" : prof == 110 ? "High 10" : prof == 122 ? "High 4:2:2" : prof == 244 ? "High 4:4:4" : "";
        std::string out = (pn[0] ? std::string(pn) + " profile" : fmt("profile %d", prof)) + fmt(", level %d.%d", lev / 10, lev % 10);
        int chroma = 1;
        if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128) {
            chroma = b.ue(); if (chroma == 3) b.u(1);
            b.ue(); b.ue(); b.u(1);
            if (b.u(1)) return out;   // scaling matrices: the size is further than this reads
        }
        b.ue();
        int poc = b.ue();
        if (poc == 0) b.ue();
        else if (poc == 1) { b.u(1); b.se(); b.se(); int n = b.ue(); for (int k = 0; k < n && k < 256; k++) b.se(); }
        b.ue(); b.u(1);
        int wm = b.ue(), hm = b.ue(), frameOnly = b.u(1);
        if (!frameOnly) b.u(1);
        b.u(1);
        int cl = 0, cr = 0, ct = 0, cb = 0;
        if (b.u(1)) { cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue(); }
        if (b.bad) return out;
        int cx = chroma == 0 || chroma == 3 ? 1 : 2, cy = (chroma == 1 ? 2 : 1) * (2 - frameOnly);
        int w = (wm + 1) * 16 - cx * (cl + cr), h = (2 - frameOnly) * (hm + 1) * 16 - cy * (ct + cb);
        return fmt("%dx%d, ", w, h) + (frameOnly ? "progressive or MBAFF, " : "interlaced, ") + out;
    }
    return "";
}
std::string mpegAudio(const Bytes& a)
{
    for (size_t i = 0; i + 4 <= a.size(); i++) {
        if (a[i] != 0xFF || (a[i + 1] & 0xE0) != 0xE0) continue;
        int ver = (a[i + 1] >> 3) & 3, layer = (a[i + 1] >> 1) & 3, bri = a[i + 2] >> 4, sri = (a[i + 2] >> 2) & 3, mode = a[i + 3] >> 6;
        if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
        static const int br[2][3][16] = {
            {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}, {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
            {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
        static const int sr[3] = {44100, 48000, 32000};
        int v1 = ver == 3 ? 0 : 1, L = 3 - layer;   // layer bits 3 = I, 2 = II, 1 = III
        int rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
        static const char* modes[4] = {"stereo", "joint stereo", "dual channel", "mono"};
        static const char* lays[3] = {"I", "II", "III"};
        return fmt("MPEG-%s Layer %s, %d kbit/s, %.1f kHz, %s", ver == 3 ? "1" : ver == 2 ? "2" : "2.5", lays[L], br[v1][L][bri], rate / 1000.0, modes[mode]);
    }
    return "";
}
std::string ac3Audio(const Bytes& a)
{
    for (size_t i = 0; i + 8 <= a.size(); i++) {
        if (a[i] != 0x0B || a[i + 1] != 0x77) continue;
        int fscod = a[i + 4] >> 6, frm = a[i + 4] & 0x3F, bsid = a[i + 5] >> 3, acmod = a[i + 6] >> 5;
        if (bsid > 10) return fscod == 3 ? "E-AC-3" : fmt("E-AC-3, %d kHz", fscod == 0 ? 48 : fscod == 1 ? 44 : 32);
        if (fscod == 3 || frm > 37) continue;
        static const int kb[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
        static const char* ch[8] = {"1+1 (dual mono)", "mono", "stereo", "3.0", "2.1", "3.1", "2.2", "5.0 / 5.1"};
        return fmt("AC-3, %d kbit/s, %s kHz, %s", kb[frm / 2], fscod == 0 ? "48" : fscod == 1 ? "44.1" : "32", ch[acmod]);
    }
    return "";
}
std::string aacAudio(const Bytes& a)
{
    for (size_t i = 0; i + 7 <= a.size(); i++) {
        if (a[i] != 0xFF || (a[i + 1] & 0xF6) != 0xF0) continue;
        int prof = a[i + 2] >> 6, sri = (a[i + 2] >> 2) & 15, ch = (a[i + 2] & 1) << 2 | a[i + 3] >> 6;
        static const int sr[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        if (sri > 12) continue;
        static const char* pr[4] = {"Main", "LC", "SSR", "LTP"};
        return fmt("AAC %s, %.1f kHz, %d channels", pr[prof], sr[sri] / 1000.0, ch);
    }
    return "";
}
std::string clockOf(double s)   // 1:02:03.4 / 2:03.4
{
    if (s < 0) s = 0;
    int h = (int)(s / 3600), m = (int)(s / 60) % 60;
    double sec = s - h * 3600 - m * 60;
    return h ? fmt("%d:%02d:%04.1f", h, m, sec) : fmt("%d:%04.1f", m, sec);
}
}  // namespace
