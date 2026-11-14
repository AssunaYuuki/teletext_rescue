// BBC Micro Model B emulator (see bbcmicro.h).
#include "bbcmicro.h"
#include "nabts/saa5050.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace bbc {

// ================================================================ 6522 VIA
class Via {
public:
    uint8_t ora = 0, orb = 0, ddra = 0, ddrb = 0, acr = 0, pcr = 0, ifr = 0, ier = 0, sr = 0;
    int t1c = 0xFFFF, t2c = 0xFFFF;
    uint16_t t1l = 0xFFFF;
    uint8_t t2ll = 0xFF;
    bool t1hit = true, t2hit = true;
    bool ca1 = false, ca2 = false, cb1 = false, cb2 = false;
    std::function<uint8_t()> inA, inB;
    std::function<void(uint8_t)> outA, outB;

    void reset()
    {
        ora = orb = ddra = ddrb = acr = pcr = ifr = ier = sr = 0;
        t1c = t2c = 0xFFFF; t1l = 0xFFFF; t2ll = 0xFF; t1hit = t2hit = true;
    }
    bool irq() const { return (ifr & ier & 0x7F) != 0; }
    void update() { if (ifr & ier & 0x7F) ifr |= 0x80; else ifr &= 0x7F; }
    void flag(uint8_t b) { ifr |= b; update(); }
    void clear(uint8_t b) { ifr &= ~b; update(); }
    bool ca2Independent() const { return (pcr & 0x0A) == 0x02; }   // input mode, independent interrupt
    bool cb2Independent() const { return (pcr & 0xA0) == 0x20; }
    void setCA1(bool v) { if (v != ca1) { if (v == ((pcr & 1) != 0)) flag(0x02); ca1 = v; } }
    void setCA2(bool v)
    {
        if (v != ca2) { if (!(pcr & 0x08) && v == ((pcr & 0x04) != 0)) flag(0x01); ca2 = v; }
    }
    void setCB1(bool v) { if (v != cb1) { if (v == ((pcr & 0x10) != 0)) flag(0x10); cb1 = v; } }
    uint8_t portA() const { return (uint8_t)((ora & ddra) | (~ddra & 0xFF)); }
    uint8_t portB() const { return (uint8_t)((orb & ddrb) | (~ddrb & 0xFF)); }

    uint8_t read(int r)
    {
        switch (r & 15) {
        case 0: {
            uint8_t in = inB ? inB() : 0xFF;
            clear((uint8_t)(0x10 | (cb2Independent() ? 0 : 0x08)));
            return (uint8_t)((orb & ddrb) | (in & ~ddrb));
        }
        case 1: case 15: {
            uint8_t in = inA ? inA() : 0xFF;
            if ((r & 15) == 1) clear((uint8_t)(0x02 | (ca2Independent() ? 0 : 0x01)));
            return (uint8_t)((ora & ddra) | (in & ~ddra));
        }
        case 2: return ddrb;
        case 3: return ddra;
        case 4: clear(0x40); return (uint8_t)(t1c & 0xFF);
        case 5: return (uint8_t)((t1c >> 8) & 0xFF);
        case 6: return (uint8_t)(t1l & 0xFF);
        case 7: return (uint8_t)(t1l >> 8);
        case 8: clear(0x20); return (uint8_t)(t2c & 0xFF);
        case 9: return (uint8_t)((t2c >> 8) & 0xFF);
        case 10: clear(0x04); return sr;
        case 11: return acr;
        case 12: return pcr;
        case 13: return ifr;
        case 14: return (uint8_t)(ier | 0x80);
        }
        return 0xFF;
    }
    void write(int r, uint8_t v)
    {
        switch (r & 15) {
        case 0: orb = v; clear((uint8_t)(0x10 | (cb2Independent() ? 0 : 0x08))); if (outB) outB(portB()); break;
        case 1: case 15:
            ora = v;
            if ((r & 15) == 1) clear((uint8_t)(0x02 | (ca2Independent() ? 0 : 0x01)));
            if (outA) outA(portA());
            break;
        case 2: ddrb = v; if (outB) outB(portB()); break;
        case 3: ddra = v; if (outA) outA(portA()); break;
        case 4: case 6: t1l = (uint16_t)((t1l & 0xFF00) | v); break;
        case 5: t1l = (uint16_t)((t1l & 0xFF) | v << 8); t1c = t1l + 1; t1hit = false; clear(0x40); break;
        case 7: t1l = (uint16_t)((t1l & 0xFF) | v << 8); clear(0x40); break;
        case 8: t2ll = v; break;
        case 9: t2c = (t2ll | v << 8) + 1; t2hit = false; clear(0x20); break;
        case 10: sr = v; clear(0x04); break;
        case 11: acr = v; break;
        case 12: pcr = v; break;
        case 13: ifr &= ~(v & 0x7F); update(); break;
        case 14: if (v & 0x80) ier |= v & 0x7F; else ier &= ~(v & 0x7F); update(); break;
        }
    }
    void tick(int n)   // n cycles of 1 MHz
    {
        t1c -= n;
        while (t1c < 0) {
            if (!t1hit) { flag(0x40); if (!(acr & 0x40)) t1hit = true; }
            if (acr & 0x40) t1c += t1l + 2;
            else t1c += 0x10000;
        }
        if (!(acr & 0x20)) {
            t2c -= n;
            while (t2c < 0) {
                if (!t2hit) { flag(0x20); t2hit = true; }
                t2c += 0x10000;
            }
        }
    }
};

// ================================================================ the machine
Machine::Machine() : pix_(W * H, 0)
{
    sys_ = new Via;
    usr_ = new Via;
    sys_->outB = [this](uint8_t v) { sysPortB(v); };
    sys_->outA = [this](uint8_t v) { sdb_ = v; updateKeys(); };
    sys_->inA = [this] { return sysPortAIn(); };
    sys_->inB = [] { return (uint8_t)0xFF; };   // joystick buttons up, no speech
    usr_->inA = [] { return (uint8_t)0xFF; };
    usr_->inB = [] { return (uint8_t)0xFF; };
}

Machine::~Machine() { delete sys_; delete usr_; }

bool Machine::setRoms(const std::vector<uint8_t>& os, const std::vector<uint8_t>& basic, std::string& err)
{
    if (os.size() != 16384) { err = "MOS ROM must be 16384 bytes"; return false; }
    if (basic.size() != 16384) { err = "BASIC ROM must be 16384 bytes"; return false; }
    os_ = os;
    for (auto& r : rom_) r.clear();
    rom_[15] = basic;
    return true;
}

void Machine::reset(bool power)
{
    if (power) {
        ram_.fill(0);
        sys_->reset(); usr_->reset();
        ic32_ = 0; sdb_ = 0; romsel_ = 15;
        std::memset(crtc_, 0, sizeof crtc_); ulaCtrl_ = 0; std::memset(palette_, 0, sizeof palette_);
        psg_ = Psg();
        for (auto& c : keys_) for (auto& k : c) k = false;
        typed_.clear(); typeHold_ = 0; typedKey_ = -1;
        paste_.clear();
    }
    s_ = 0xFD; p_ = 0x24;
    pc_ = rd16(0xFFFC);
}

// ---------------------------------------------------------------- memory map
uint8_t Machine::rd(uint16_t a)
{
    if (a < 0x8000) return ram_[a];
    if (a < 0xC000) { auto& r = rom_[romsel_]; return r.empty() ? 0xFF : r[a - 0x8000]; }
    if (a < 0xFC00 || a >= 0xFF00) return os_.empty() ? 0xFF : os_[a - 0xC000];
    if (a < 0xFE00) return 0xFF;   // FRED, JIM
    switch (a & 0xFFE0) {
    case 0xFE00:
        if ((a & 0x18) == 0x00) {   // CRTC
            if (a & 1) return crtcAddr_ >= 14 && crtcAddr_ < 18 ? crtc_[crtcAddr_] : 0;
            return 0;
        }
        if ((a & 0x18) == 0x08) return (a & 1) ? 0 : 0x02;   // ACIA: transmit register empty
        return 0xFE;   // serial ULA, video ULA: write only
    case 0xFE20: return 0xFE;
    case 0xFE40: return sys_->read(a & 15);
    case 0xFE60: return usr_->read(a & 15);
    case 0xFEC0:
        switch (a & 3) {
        case 0: return adcStatus_;
        case 1: return adcHigh_;
        case 2: return adcLow_;
        }
        return 0;
    }
    return 0xFE;   // 8271, Econet, Tube: not fitted
}

void Machine::wr(uint16_t a, uint8_t v)
{
    if (a < 0x8000) { ram_[a] = v; return; }
    if (a < 0xFE00 || a >= 0xFF00) return;
    switch (a & 0xFFE0) {
    case 0xFE00:
        if ((a & 0x18) == 0x00) {
            if (a & 1) { if (crtcAddr_ < 18) crtc_[crtcAddr_] = v; }
            else crtcAddr_ = v & 31;
        }
        return;
    case 0xFE20:
        if ((a & 0x10) == 0) {   // video ULA
            if (a & 1) palette_[v >> 4] = v & 15;
            else ulaCtrl_ = v;
        }
        return;
    case 0xFE40: sys_->write(a & 15, v); return;
    case 0xFE60: usr_->write(a & 15, v); return;
    case 0xFEC0:
        if ((a & 3) == 0) {   // start a conversion: done at once (the joystick in the middle)
            adcStatus_ = (uint8_t)((v & 0x0F) | 0x40);
            adcHigh_ = 0x80; adcLow_ = 0;
            sys_->flag(0x10);   // end of conversion on CB1
        }
        return;
    }
    if ((a & 0xFFF0) == 0xFE30) romsel_ = v & 15;
}

// ---------------------------------------------------------------- system VIA: latch, keyboard, sound
void Machine::sysPortB(uint8_t v)
{
    int bit = v & 7;
    bool val = (v & 8) != 0;
    uint8_t old = ic32_;
    if (val) ic32_ |= (uint8_t)(1 << bit); else ic32_ &= (uint8_t)~(1 << bit);
    if (bit == 0 && (old & 1) && !val) psg_.write(sdb_);   // sound write enable goes low
    updateKeys();
}

void Machine::updateKeys()
{
    // SHIFT as the typing needs it (or the PC's shift)
    keys_[0][0] = typedKey_ >= 0 ? typedShift_ : heldShift_;
    if (typedKey_ >= 0) keys_[1][0] = typedCtrl_;
    bool any = false;
    if (ic32_ & 8) {   // keyboard scanned by the hardware: any key of rows 1-7
        for (int c = 0; c < 10; c++) for (int r = 1; r < 8; r++) if (keys_[c][r]) any = true;
    } else {
        int c = sdb_ & 15;
        for (int r = 1; r < 8; r++) if (keys_[c][r]) any = true;
    }
    sys_->setCA2(any);
}

uint8_t Machine::sysPortAIn()
{
    uint8_t v = sdb_;
    if (!(ic32_ & 8)) {
        int c = sdb_ & 15, r = (sdb_ >> 4) & 7;
        if (keys_[c][r]) v |= 0x80; else v &= 0x7F;
    }
    return v;
}

void Machine::key(int k, bool down)
{
    if (k == K_SHIFT) { heldShift_ = down; updateKeys(); return; }
    int r = (k >> 4) & 7, c = k & 15;
    if (c < 16) keys_[c][r] = down;
    updateKeys();
}

void Machine::releaseAll()
{
    for (auto& c : keys_) for (auto& k : c) k = false;
    heldShift_ = false;
    updateKeys();
}

// a character -> key and SHIFT / CTRL on the BBC keyboard (letters by the CAPS LOCK)
static bool charKey(int ch, bool caps, int& key, bool& shift, bool& ctrl)
{
    static const int letters[26] = {K_A, K_B, K_C, K_D, K_E, K_F, K_G, K_H, K_I, K_J, K_K, K_L, K_M,
                                    K_N, K_O, K_P, K_Q, K_R, K_S, K_T, K_U, K_V, K_W, K_X, K_Y, K_Z};
    static const int digits[10] = {K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9};
    shift = ctrl = false;
    if (ch >= 'A' && ch <= 'Z') { key = letters[ch - 'A']; shift = !caps; return true; }
    if (ch >= 'a' && ch <= 'z') { key = letters[ch - 'a']; shift = caps; return true; }
    if (ch >= '0' && ch <= '9') { key = digits[ch - '0']; return true; }
    if (ch >= 1 && ch <= 26 && ch != 8 && ch != 9 && ch != 13) { key = letters[ch - 1]; ctrl = true; return true; }
    struct M { char c; int k; bool s; };
    static const M map[] = {
        {'!', K_1, 1}, {'"', K_2, 1}, {'#', K_3, 1}, {'$', K_4, 1}, {'%', K_5, 1}, {'&', K_6, 1}, {'\'', K_7, 1}, {'(', K_8, 1}, {')', K_9, 1},
        {'-', K_MINUS, 0}, {'=', K_MINUS, 1}, {'^', K_CARET, 0}, {'~', K_CARET, 1}, {'\\', K_BACKSLASH, 0}, {'|', K_BACKSLASH, 1},
        {'@', K_AT, 0}, {'[', K_LBRACKET, 0}, {'{', K_LBRACKET, 1}, {'_', K_UNDERSCORE, 0}, {'`', K_UNDERSCORE, 1},
        {';', K_SEMICOLON, 0}, {'+', K_SEMICOLON, 1}, {':', K_COLON, 0}, {'*', K_COLON, 1}, {']', K_RBRACKET, 0}, {'}', K_RBRACKET, 1},
        {',', K_COMMA, 0}, {'<', K_COMMA, 1}, {'.', K_PERIOD, 0}, {'>', K_PERIOD, 1}, {'/', K_SLASH, 0}, {'?', K_SLASH, 1},
        {' ', K_SPACE, 0}, {'\r', K_RETURN, 0}, {'\n', K_RETURN, 0}, {'\t', K_TAB, 0}, {8, K_DELETE, 0}, {127, K_DELETE, 0}, {27, K_ESCAPE, 0},
    };
    for (auto& m : map) if (m.c == ch) { key = m.k; shift = m.s; return true; }
    if (ch == 0xA3) { key = K_UNDERSCORE; shift = true; return true; }   // £ (Latin-1)
    return false;
}

void Machine::typeChars(const std::string& s)
{
    for (unsigned char c : s) typed_.push_back({c, false});
}

void Machine::typingFrame()
{
    if (typeHold_ > 0) {
        if (--typeHold_ > 0) return;
        if (typedKey_ >= 0) {   // release, then a moment up before the next key
            keys_[typedKey_ & 15][(typedKey_ >> 4) & 7] = false;
            typedKey_ = -1;
            if (typedCtrl_) keys_[1][0] = false;
            typedCtrl_ = false;
            updateKeys();
            typeHold_ = 2;
            return;
        }
    }
    while (!typed_.empty()) {
        int ch = typed_.front().first;
        typed_.pop_front();
        int k; bool sh, ct;
        if (!charKey(ch, capsLock(), k, sh, ct)) continue;
        typedKey_ = k; typedShift_ = sh; typedCtrl_ = ct;
        keys_[k & 15][(k >> 4) & 7] = true;
        updateKeys();
        typeHold_ = 3;
        return;
    }
}

void Machine::paste(const std::string& s)
{
    for (unsigned char c : s) paste_.push_back(c == '\n' ? 13 : c);
}

// ---------------------------------------------------------------- 6502
static const uint8_t CYC[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7};

void Machine::irq(bool brk)
{
    push((uint8_t)(pc_ >> 8)); push((uint8_t)pc_);
    push((uint8_t)((p_ | 0x20) & (brk ? 0xFF : ~0x10)));
    p_ |= 0x04;
    pc_ = rd16(0xFFFE);
    cycles_ += 7;
}

void Machine::adc(uint8_t v)
{
    int c = p_ & 1;
    if (p_ & 0x08) {   // decimal
        int lo = (a_ & 15) + (v & 15) + c, hi = (a_ >> 4) + (v >> 4);
        if (lo > 9) { lo += 6; hi++; }
        uint8_t bin = (uint8_t)(a_ + v + c);
        p_ = (uint8_t)(p_ & ~0xC3);
        if (!bin) p_ |= 2;
        if (hi & 8) p_ |= 0x80;
        if (~(a_ ^ v) & (a_ ^ (hi << 4)) & 0x80) p_ |= 0x40;
        if (hi > 9) hi += 6;
        if (hi > 15) p_ |= 1;
        a_ = (uint8_t)((hi << 4) | (lo & 15));
        return;
    }
    int t = a_ + v + c;
    p_ = (uint8_t)(p_ & ~0xC3);
    if (t > 255) p_ |= 1;
    if (~(a_ ^ v) & (a_ ^ t) & 0x80) p_ |= 0x40;
    a_ = (uint8_t)t;
    setNZ(a_);
}

void Machine::sbc(uint8_t v)
{
    int c = p_ & 1;
    int t = a_ - v - (1 - c);
    if (p_ & 0x08) {
        int lo = (a_ & 15) - (v & 15) - (1 - c), hi = (a_ >> 4) - (v >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        if (hi < 0) hi -= 6;
        p_ = (uint8_t)(p_ & ~0xC3);
        if (t >= 0) p_ |= 1;
        if ((a_ ^ v) & (a_ ^ t) & 0x80) p_ |= 0x40;
        if (!(uint8_t)t) p_ |= 2;
        if (t & 0x80) p_ |= 0x80;
        a_ = (uint8_t)((hi << 4) | (lo & 15));
        return;
    }
    p_ = (uint8_t)(p_ & ~0xC3);
    if (t >= 0) p_ |= 1;
    if ((a_ ^ v) & (a_ ^ t) & 0x80) p_ |= 0x40;
    a_ = (uint8_t)t;
    setNZ(a_);
}

void Machine::step()
{
    // typed-in text: OSRDCH answers from it (the MOS read character routine, where RDCHV points)
    if (!paste_.empty()) {
        if (pc_ == (uint16_t)(ram_[0x210] | ram_[0x211] << 8)) {
            a_ = paste_.front(); paste_.pop_front();
            p_ &= ~1;   // C clear: not Escape
            uint16_t r = (uint16_t)(pull() | pull() << 8);
            pc_ = (uint16_t)(r + 1);
            cycles_ += 6;
            return;
        }
        // REMV (remove / examine a character of a buffer) for the keyboard buffer, as OSRDCH waits on it
        if (pc_ == (uint16_t)(ram_[0x22C] | ram_[0x22D] << 8) && x_ == 0) {
            a_ = y_ = paste_.front();
            if (!(p_ & 0x40)) paste_.pop_front();   // V clear: remove, V set: only look
            p_ &= ~1;   // C clear: there is one
            uint16_t r = (uint16_t)(pull() | pull() << 8);
            pc_ = (uint16_t)(r + 1);
            cycles_ += 6;
            return;
        }
    }
    if (!(p_ & 0x04) && (sys_->irq() || usr_->irq())) { irq(false); return; }
    uint8_t op = rd(pc_++);
    int cyc = CYC[op];
    uint16_t ad = 0;
    // addressing
    auto imm = [&] { ad = pc_++; };
    auto zp = [&] { ad = rd(pc_++); };
    auto zpx = [&] { ad = (uint8_t)(rd(pc_++) + x_); };
    auto zpy = [&] { ad = (uint8_t)(rd(pc_++) + y_); };
    auto abs_ = [&] { ad = rd16(pc_); pc_ += 2; };
    auto absx = [&](bool pen) { uint16_t b = rd16(pc_); pc_ += 2; ad = (uint16_t)(b + x_); if (pen && (b & 0xFF00) != (ad & 0xFF00)) cyc++; };
    auto absy = [&](bool pen) { uint16_t b = rd16(pc_); pc_ += 2; ad = (uint16_t)(b + y_); if (pen && (b & 0xFF00) != (ad & 0xFF00)) cyc++; };
    auto indx = [&] { uint8_t z = (uint8_t)(rd(pc_++) + x_); ad = (uint16_t)(rd(z) | rd((uint8_t)(z + 1)) << 8); };
    auto indy = [&](bool pen) { uint8_t z = rd(pc_++); uint16_t b = (uint16_t)(rd(z) | rd((uint8_t)(z + 1)) << 8); ad = (uint16_t)(b + y_); if (pen && (b & 0xFF00) != (ad & 0xFF00)) cyc++; };
    auto branch = [&](bool c) {
        int8_t o = (int8_t)rd(pc_++);
        if (c) { uint16_t n = (uint16_t)(pc_ + o); cyc += ((n & 0xFF00) != (pc_ & 0xFF00)) ? 2 : 1; pc_ = n; }
    };
    auto asl = [&](uint8_t v) { p_ = (uint8_t)((p_ & ~1) | (v >> 7)); v <<= 1; setNZ(v); return v; };
    auto lsr = [&](uint8_t v) { p_ = (uint8_t)((p_ & ~1) | (v & 1)); v >>= 1; setNZ(v); return v; };
    auto rol = [&](uint8_t v) { int c = p_ & 1; p_ = (uint8_t)((p_ & ~1) | (v >> 7)); v = (uint8_t)(v << 1 | c); setNZ(v); return v; };
    auto ror = [&](uint8_t v) { int c = p_ & 1; p_ = (uint8_t)((p_ & ~1) | (v & 1)); v = (uint8_t)(v >> 1 | c << 7); setNZ(v); return v; };
    auto bit = [&](uint8_t v) { p_ = (uint8_t)((p_ & 0x3D) | (v & 0xC0) | ((a_ & v) ? 0 : 2)); };
    auto rmw = [&](std::function<uint8_t(uint8_t)> f) { uint8_t v = rd(ad); wr(ad, v); wr(ad, f(v)); };

    switch (op) {
    // loads / stores
    case 0xA9: imm(); a_ = rd(ad); setNZ(a_); break;
    case 0xA5: zp(); a_ = rd(ad); setNZ(a_); break;
    case 0xB5: zpx(); a_ = rd(ad); setNZ(a_); break;
    case 0xAD: abs_(); a_ = rd(ad); setNZ(a_); break;
    case 0xBD: absx(true); a_ = rd(ad); setNZ(a_); break;
    case 0xB9: absy(true); a_ = rd(ad); setNZ(a_); break;
    case 0xA1: indx(); a_ = rd(ad); setNZ(a_); break;
    case 0xB1: indy(true); a_ = rd(ad); setNZ(a_); break;
    case 0xA2: imm(); x_ = rd(ad); setNZ(x_); break;
    case 0xA6: zp(); x_ = rd(ad); setNZ(x_); break;
    case 0xB6: zpy(); x_ = rd(ad); setNZ(x_); break;
    case 0xAE: abs_(); x_ = rd(ad); setNZ(x_); break;
    case 0xBE: absy(true); x_ = rd(ad); setNZ(x_); break;
    case 0xA0: imm(); y_ = rd(ad); setNZ(y_); break;
    case 0xA4: zp(); y_ = rd(ad); setNZ(y_); break;
    case 0xB4: zpx(); y_ = rd(ad); setNZ(y_); break;
    case 0xAC: abs_(); y_ = rd(ad); setNZ(y_); break;
    case 0xBC: absx(true); y_ = rd(ad); setNZ(y_); break;
    case 0x85: zp(); wr(ad, a_); break;
    case 0x95: zpx(); wr(ad, a_); break;
    case 0x8D: abs_(); wr(ad, a_); break;
    case 0x9D: absx(false); wr(ad, a_); break;
    case 0x99: absy(false); wr(ad, a_); break;
    case 0x81: indx(); wr(ad, a_); break;
    case 0x91: indy(false); wr(ad, a_); break;
    case 0x86: zp(); wr(ad, x_); break;
    case 0x96: zpy(); wr(ad, x_); break;
    case 0x8E: abs_(); wr(ad, x_); break;
    case 0x84: zp(); wr(ad, y_); break;
    case 0x94: zpx(); wr(ad, y_); break;
    case 0x8C: abs_(); wr(ad, y_); break;
    // transfers
    case 0xAA: x_ = a_; setNZ(x_); break;
    case 0xA8: y_ = a_; setNZ(y_); break;
    case 0x8A: a_ = x_; setNZ(a_); break;
    case 0x98: a_ = y_; setNZ(a_); break;
    case 0xBA: x_ = s_; setNZ(x_); break;
    case 0x9A: s_ = x_; break;
    // stack
    case 0x48: push(a_); break;
    case 0x68: a_ = pull(); setNZ(a_); break;
    case 0x08: push((uint8_t)(p_ | 0x30)); break;
    case 0x28: p_ = (uint8_t)((pull() & 0xCF) | 0x20); break;
    // logic / arithmetic
#define ALU(base, body)                                                                          \
    case base + 0x09: imm(); { uint8_t v = rd(ad); body; } break;                                \
    case base + 0x05: zp(); { uint8_t v = rd(ad); body; } break;                                 \
    case base + 0x15: zpx(); { uint8_t v = rd(ad); body; } break;                                \
    case base + 0x0D: abs_(); { uint8_t v = rd(ad); body; } break;                               \
    case base + 0x1D: absx(true); { uint8_t v = rd(ad); body; } break;                           \
    case base + 0x19: absy(true); { uint8_t v = rd(ad); body; } break;                           \
    case base + 0x01: indx(); { uint8_t v = rd(ad); body; } break;                               \
    case base + 0x11: indy(true); { uint8_t v = rd(ad); body; } break;
    ALU(0x00, a_ |= v; setNZ(a_))
    ALU(0x20, a_ &= v; setNZ(a_))
    ALU(0x40, a_ ^= v; setNZ(a_))
    ALU(0x60, adc(v))
    ALU(0xC0, cmp(a_, v))
    ALU(0xE0, sbc(v))
#undef ALU
    case 0xE0: imm(); cmp(x_, rd(ad)); break;
    case 0xE4: zp(); cmp(x_, rd(ad)); break;
    case 0xEC: abs_(); cmp(x_, rd(ad)); break;
    case 0xC0: imm(); cmp(y_, rd(ad)); break;
    case 0xC4: zp(); cmp(y_, rd(ad)); break;
    case 0xCC: abs_(); cmp(y_, rd(ad)); break;
    case 0x24: zp(); bit(rd(ad)); break;
    case 0x2C: abs_(); bit(rd(ad)); break;
    // increments
    case 0xE6: zp(); rmw([&](uint8_t v) { v++; setNZ(v); return v; }); break;
    case 0xF6: zpx(); rmw([&](uint8_t v) { v++; setNZ(v); return v; }); break;
    case 0xEE: abs_(); rmw([&](uint8_t v) { v++; setNZ(v); return v; }); break;
    case 0xFE: absx(false); rmw([&](uint8_t v) { v++; setNZ(v); return v; }); break;
    case 0xC6: zp(); rmw([&](uint8_t v) { v--; setNZ(v); return v; }); break;
    case 0xD6: zpx(); rmw([&](uint8_t v) { v--; setNZ(v); return v; }); break;
    case 0xCE: abs_(); rmw([&](uint8_t v) { v--; setNZ(v); return v; }); break;
    case 0xDE: absx(false); rmw([&](uint8_t v) { v--; setNZ(v); return v; }); break;
    case 0xE8: x_++; setNZ(x_); break;
    case 0xC8: y_++; setNZ(y_); break;
    case 0xCA: x_--; setNZ(x_); break;
    case 0x88: y_--; setNZ(y_); break;
    // shifts
    case 0x0A: a_ = asl(a_); break;
    case 0x06: zp(); rmw(asl); break;
    case 0x16: zpx(); rmw(asl); break;
    case 0x0E: abs_(); rmw(asl); break;
    case 0x1E: absx(false); rmw(asl); break;
    case 0x4A: a_ = lsr(a_); break;
    case 0x46: zp(); rmw(lsr); break;
    case 0x56: zpx(); rmw(lsr); break;
    case 0x4E: abs_(); rmw(lsr); break;
    case 0x5E: absx(false); rmw(lsr); break;
    case 0x2A: a_ = rol(a_); break;
    case 0x26: zp(); rmw(rol); break;
    case 0x36: zpx(); rmw(rol); break;
    case 0x2E: abs_(); rmw(rol); break;
    case 0x3E: absx(false); rmw(rol); break;
    case 0x6A: a_ = ror(a_); break;
    case 0x66: zp(); rmw(ror); break;
    case 0x76: zpx(); rmw(ror); break;
    case 0x6E: abs_(); rmw(ror); break;
    case 0x7E: absx(false); rmw(ror); break;
    // jumps
    case 0x4C: pc_ = rd16(pc_); break;
    case 0x6C: { uint16_t i = rd16(pc_); pc_ = (uint16_t)(rd(i) | rd((uint16_t)((i & 0xFF00) | ((i + 1) & 0xFF))) << 8); break; }
    case 0x20: { uint16_t t = rd16(pc_); pc_++; push((uint8_t)(pc_ >> 8)); push((uint8_t)pc_); pc_ = t; break; }
    case 0x60: { uint16_t r = (uint16_t)(pull() | pull() << 8); pc_ = (uint16_t)(r + 1); break; }
    case 0x40: { p_ = (uint8_t)((pull() & 0xCF) | 0x20); uint16_t r = (uint16_t)(pull() | pull() << 8); pc_ = r; break; }
    case 0x00: pc_++; push((uint8_t)(pc_ >> 8)); push((uint8_t)pc_); push((uint8_t)(p_ | 0x30)); p_ |= 0x04; pc_ = rd16(0xFFFE); break;
    // branches
    case 0x10: branch(!(p_ & 0x80)); break;
    case 0x30: branch(p_ & 0x80); break;
    case 0x50: branch(!(p_ & 0x40)); break;
    case 0x70: branch(p_ & 0x40); break;
    case 0x90: branch(!(p_ & 1)); break;
    case 0xB0: branch(p_ & 1); break;
    case 0xD0: branch(!(p_ & 2)); break;
    case 0xF0: branch(p_ & 2); break;
    // flags
    case 0x18: p_ &= ~1; break;
    case 0x38: p_ |= 1; break;
    case 0x58: p_ &= ~4; break;
    case 0x78: p_ |= 4; break;
    case 0xB8: p_ &= ~0x40; break;
    case 0xD8: p_ &= ~8; break;
    case 0xF8: p_ |= 8; break;
    case 0xEA: break;
    // undocumented: the ones that only read (NOPs of their length); the rest as NOP
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: pc_++; break;
    case 0x04: case 0x44: case 0x64: pc_++; break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: pc_++; break;
    case 0x0C: pc_ += 2; break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: absx(true); rd(ad); break;
    case 0xA7: zp(); a_ = x_ = rd(ad); setNZ(a_); break;                    // LAX
    case 0xB7: zpy(); a_ = x_ = rd(ad); setNZ(a_); break;
    case 0xAF: abs_(); a_ = x_ = rd(ad); setNZ(a_); break;
    case 0xBF: absy(true); a_ = x_ = rd(ad); setNZ(a_); break;
    case 0x87: zp(); wr(ad, a_ & x_); break;                                // SAX
    case 0x8F: abs_(); wr(ad, a_ & x_); break;
    case 0xEB: imm(); sbc(rd(ad)); break;
    default:
        break;
    }
    cycles_ += cyc;
}

// ---------------------------------------------------------------- one frame
void Machine::runFrame()
{
    typingFrame();
    const int FRAME = 40000;
    int64_t done = 0;
    int viaHalf = 0;
    int slice = 0;
    while (done < FRAME) {
        int64_t before = cycles_;
        step();
        int c = (int)(cycles_ - before);
        done += c;
        viaHalf += c;
        if (viaHalf >= 2) { int n = viaHalf / 2; viaHalf -= n * 2; sys_->tick(n); usr_->tick(n); }
        slice += c;
        if (slice >= 2500) { makeSound(slice); slice = 0; }
    }
    if (slice) makeSound(slice);
    cycles_ = 0;
    // vertical sync: CA1 of the system VIA
    sys_->setCA1(true);
    sys_->setCA1(false);
    frame_++;
    render();
}

// ---------------------------------------------------------------- sound
void Machine::Psg::write(uint8_t v)
{
    if (v & 0x80) latch = (v >> 4) & 7;
    int ch = latch >> 1;
    if (latch & 1) { vol[ch] = v & 15; return; }
    if (ch == 3) { noise = v & 7; lfsr = 0x4000; return; }
    if (v & 0x80) period[ch] = (uint16_t)((period[ch] & 0x3F0) | (v & 0x0F));
    else period[ch] = (uint16_t)((period[ch] & 0x0F) | ((v & 0x3F) << 4));
}

int16_t Machine::Psg::sample()
{
    static double amp[16];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 15; i++) amp[i] = 7000.0 * std::pow(10.0, -0.1 * i);
        amp[15] = 0;
        init = true;
    }
    const double step = 250000.0 / RATE;   // chip ticks (4 MHz / 16) per sample
    double s = 0;
    for (int ch = 0; ch < 3; ch++) {
        int n = period[ch] ? period[ch] : 1024;
        if (n < 2) { s += amp[vol[ch]]; continue; }   // a period of 1 or less: steady level
        phase[ch] += step;
        while (phase[ch] >= n) { phase[ch] -= n; out[ch] ^= 1; }
        s += out[ch] ? amp[vol[ch]] : -amp[vol[ch]];
    }
    int rate = noise & 3;
    double np = rate == 3 ? 2.0 * std::max<int>(1, period[2]) : (double)(32 << rate);
    phase[3] += step;
    while (phase[3] >= np) {
        phase[3] -= np;
        int fb = (noise & 4) ? ((lfsr ^ (lfsr >> 1)) & 1) : (lfsr & 1);
        lfsr = (uint16_t)((lfsr >> 1) | (fb << 14));
    }
    s += (lfsr & 1) ? amp[vol[3]] : -amp[vol[3]];
    return (int16_t)std::clamp(s, -32000.0, 32000.0);
}

void Machine::makeSound(int cyclesDone)
{
    sndAcc_ += cyclesDone * (double)RATE / 2000000.0;
    int n = (int)sndAcc_;
    sndAcc_ -= n;
    for (int i = 0; i < n; i++) snd_.push_back(psg_.sample());
    if (snd_.size() > RATE) snd_.erase(snd_.begin(), snd_.begin() + (snd_.size() - RATE));   // nobody takes it
}

void Machine::takeSound(std::vector<int16_t>& out)
{
    out.insert(out.end(), snd_.begin(), snd_.end());
    snd_.clear();
}

// ---------------------------------------------------------------- picture
static uint32_t rgbOf(int c) { return ((c & 1) ? 0xFF0000 : 0) | ((c & 2) ? 0x00FF00 : 0) | ((c & 4) ? 0x0000FF : 0); }

void Machine::render()
{
    std::fill(pix_.begin(), pix_.end(), 0);
    if (ulaCtrl_ & 0x02) { renderTeletext(); return; }
    int r1 = crtc_[1], r6 = crtc_[6] & 0x7F, r9 = crtc_[9] & 31;
    if (!r1 || !r6) return;
    int start = (crtc_[12] << 8 | crtc_[13]) & 0x3FFF;
    int cursor = (crtc_[14] << 8 | crtc_[15]) & 0x3FFF;
    bool hi = ulaCtrl_ & 0x10;
    int cpl = 10 << ((ulaCtrl_ >> 2) & 3);
    int ppb = hi ? cpl / 10 : cpl / 5;
    if (ppb < 1) ppb = 1;
    int unit = hi ? 8 : 16;
    int pw = std::max(1, unit / ppb);
    static const int screenlen[4] = {0x4000, 0x5000, 0x2000, 0x2800};
    int slen = screenlen[(ic32_ >> 4) & 3];
    int total = r1 * unit;
    int x0 = std::max(0, (W - total) / 2);
    int lines = r6 * (r9 + 1);
    int y0 = std::max(0, (256 - lines) / 2);
    int cs = crtc_[10] & 31, ce = crtc_[11] & 31, blink = (crtc_[10] >> 5) & 3;
    bool cursorOn = (ulaCtrl_ & 0x80) && blink != 1 && (blink == 0 || ((frame_ >> (blink == 2 ? 3 : 4)) & 1));
    int cwBytes = 1 << std::max(0, (int)((ulaCtrl_ >> 5) & 3) - 1);
    if (((ulaCtrl_ >> 5) & 3) == 0) cwBytes = 1;
    bool flash = ulaCtrl_ & 1;
    uint32_t pal[16];
    for (int i = 0; i < 16; i++) {
        int c = (palette_[i] & 7) ^ 7;
        if ((palette_[i] & 8) && flash) c ^= 7;
        pal[i] = rgbOf(c);
    }
    int y = 0;
    for (int row = 0; row < r6; row++)
        for (int ra = 0; ra <= r9; ra++, y++) {
            int sy = y0 + y;
            if (sy >= 256) return;
            uint32_t* l0 = &pix_[(size_t)(2 * sy) * W];
            uint32_t* l1 = l0 + W;
            int cxs = -1;
            for (int col = 0; col < r1; col++) {
                int ma = (start + row * r1 + col) & 0x3FFF;
                int x = x0 + col * unit;
                if (x >= W) break;
                bool cur = cursorOn && ma == cursor && ra >= cs && ra <= ce;
                if (cur) cxs = col;
                bool curHere = cursorOn && cxs >= 0 && col < cxs + cwBytes && ra >= cs && ra <= ce;
                if (ra & 8) {   // modes 3 and 6: the gap lines
                    if (curHere) for (int i = 0; i < unit && x + i < W; i++) l0[x + i] = l1[x + i] = 0xFFFFFF;
                    continue;
                }
                int addr = (ma << 3) | (ra & 7);
                if (addr & 0x8000) addr -= slen;
                uint8_t b = ram_[addr & 0x7FFF];
                for (int p = 0; p < ppb; p++) {
                    int idx = ((b >> 4) & 8) | ((b >> 3) & 4) | ((b >> 2) & 2) | ((b >> 1) & 1);
                    b = (uint8_t)((b << 1) | 1);
                    uint32_t c = pal[idx];
                    if (curHere) c ^= 0xFFFFFF;
                    for (int i = 0; i < pw; i++) {
                        int xx = x + p * pw + i;
                        if (xx < W) l0[xx] = l1[xx] = c;
                    }
                }
            }
        }
}

// the SAA5050 (English): a character code -> what it shows
static char32_t saaChar(int c)
{
    switch (c) {
    case 0x23: return 0xA3;     // £
    case 0x5B: return 0x2190;   // ←
    case 0x5C: return 0xBD;     // ½
    case 0x5D: return 0x2192;   // →
    case 0x5E: return 0x2191;   // ↑
    case 0x5F: return '#';
    case 0x60: return 0x2015;   // ― (drawn)
    case 0x7B: return 0xBC;
    case 0x7C: return 0x2016;
    case 0x7D: return 0xBE;
    case 0x7E: return 0xF7;
    case 0x7F: return 0x25A0;
    }
    return (char32_t)c;
}

void Machine::renderTeletext()
{
    const int CW = 12, CH = 20, X0 = (W - 40 * CW) / 2, Y0 = (H - 25 * CH) / 2;
    int start = (crtc_[12] << 8 | crtc_[13]) & 0x3FFF;
    int cursor = (crtc_[14] << 8 | crtc_[15]) & 0x3FFF;
    int blink = (crtc_[10] >> 5) & 3;
    bool cursorOn = blink != 1 && (blink == 0 || ((frame_ >> (blink == 2 ? 3 : 4)) & 1));
    bool flashOn = (frame_ % 48) < 32;
    bool prevDouble = false;
    for (int row = 0; row < 25; row++) {
        int fg = 7, bg = 0;
        bool gfx = false, sep = false, hold = false, flash = false, dbl = false, conceal = false;
        int held = 0x20; bool heldSep = false;
        bool rowDouble = false;
        bool bottom = prevDouble;
        for (int col = 0; col < 40; col++) {
            int ma = (start + row * 40 + col) & 0x3FFF;
            int addr = ((ma & 0x800) << 3) | 0x3C00 | (ma & 0x3FF);
            int c = ram_[addr & 0x7FFF] & 0x7F;
            // codes set at this cell
            if (c < 0x20) {
                switch (c) {
                case 0x09: flash = false; break;
                case 0x0C: dbl = false; break;
                case 0x18: conceal = true; break;
                case 0x19: sep = false; break;
                case 0x1A: sep = true; break;
                case 0x1C: bg = 0; break;
                case 0x1D: bg = fg; break;
                case 0x1E: hold = true; break;
                }
            }
            // what the cell shows
            int show = 0x20; bool mosaic = false, msep = sep;
            if (c >= 0x20) {
                if (gfx && (c & 0x20)) { mosaic = true; held = c; heldSep = sep; }
                show = c;
            } else if (hold && gfx) { show = held; mosaic = true; msep = heldSep; }
            int cfg = fg, cbg = bg;
            bool dh = dbl;
            // the cell's 12 x 20 dots
            bool dots[20][12] = {};
            bool visible = !(flash && !flashOn) && !conceal && !(bottom && !dh);
            if (visible && show != 0x20) {
                if (mosaic) {
                    static const int rows[4] = {0, 6, 14, 20};
                    for (int sr = 0; sr < 3; sr++)
                        for (int sc = 0; sc < 2; sc++) {
                            int bitn = sr * 2 + sc;
                            int mask = bitn < 5 ? (1 << bitn) : 0x40;
                            if (!(show & mask)) continue;
                            int ya = rows[sr], yb = rows[sr + 1], xa = sc * 6, xb = xa + 6;
                            if (msep) { xa += 2; yb -= 2; }
                            for (int yy = ya; yy < yb; yy++) for (int xx = xa; xx < xb; xx++) dots[yy][xx] = true;
                        }
                } else if (show == 0x60) {
                    for (int xx = 0; xx < 12; xx++) dots[9][xx] = dots[10][xx] = true;
                } else if (const Glyph20* g = saa_rounded(saaChar(show))) {
                    for (int yy = 0; yy < 20; yy++) for (int xx = 0; xx < 12; xx++) dots[yy][xx] = (*g)[yy][xx];
                }
            }
            uint32_t F = rgbOf(cfg), B = rgbOf(cbg);
            bool cur = cursorOn && ma == cursor;
            for (int yy = 0; yy < CH; yy++) {
                int sy = dh ? (bottom ? 10 + yy / 2 : yy / 2) : yy;
                uint32_t* line = &pix_[(size_t)(Y0 + row * CH + yy) * W + X0 + col * CW];
                for (int xx = 0; xx < CW; xx++) line[xx] = dots[sy][xx] ? F : B;
                if (cur && yy >= 18) for (int xx = 0; xx < CW; xx++) line[xx] ^= 0xFFFFFF;
            }
            // codes set after this cell
            if (c < 0x20) {
                if (c >= 0x01 && c <= 0x07) { fg = c; gfx = false; conceal = false; held = 0x20; }
                else if (c >= 0x11 && c <= 0x17) { fg = c - 0x10; gfx = true; conceal = false; }
                else if (c == 0x08) flash = true;
                else if (c == 0x0D) { dbl = true; rowDouble = true; }
                else if (c == 0x1F) hold = false;
            }
        }
        prevDouble = rowDouble && !bottom;
    }
}

std::string Machine::screenText() const
{
    std::string s;
    int start = (crtc_[12] << 8 | crtc_[13]) & 0x3FFF;
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 40; col++) {
            int ma = (start + row * 40 + col) & 0x3FFF;
            int addr = ((ma & 0x800) << 3) | 0x3C00 | (ma & 0x3FF);
            int c = ram_[addr & 0x7FFF] & 0x7F;
            s += (char)(c < 32 ? ' ' : c);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        s += '\n';
    }
    return s;
}

}  // namespace bbc
