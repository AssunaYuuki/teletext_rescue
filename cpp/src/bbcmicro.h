#pragma once
// BBC Micro Model B emulator: 6502 at 2 MHz, 32 KB RAM, MOS 1.20 + BASIC II (ROM files, not built in), system and
// user VIA (6522: timers, interrupts, keyboard matrix, addressable latch), CRTC 6845 + video ULA (modes 0-6),
// SAA5050 teletext (mode 7), SN76489 sound. No discs or cassettes: programs are typed in (pasted) through OSRDCH,
// the way the telesoftware loader *EXECs them.
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace bbc {

// BBC keys by their internal key number (row << 4 | column of the keyboard matrix, as INKEY -1-n)
enum Key : int {
    K_SHIFT = 0x00, K_CTRL = 0x01,
    K_Q = 0x10, K_3 = 0x11, K_4 = 0x12, K_5 = 0x13, K_F4 = 0x14, K_8 = 0x15, K_F7 = 0x16, K_MINUS = 0x17, K_CARET = 0x18, K_LEFT = 0x19,
    K_F0 = 0x20, K_W = 0x21, K_E = 0x22, K_T = 0x23, K_7 = 0x24, K_I = 0x25, K_9 = 0x26, K_0 = 0x27, K_UNDERSCORE = 0x28, K_DOWN = 0x29,
    K_1 = 0x30, K_2 = 0x31, K_D = 0x32, K_R = 0x33, K_6 = 0x34, K_U = 0x35, K_O = 0x36, K_P = 0x37, K_LBRACKET = 0x38, K_UP = 0x39,
    K_CAPSLOCK = 0x40, K_A = 0x41, K_X = 0x42, K_F = 0x43, K_Y = 0x44, K_J = 0x45, K_K = 0x46, K_AT = 0x47, K_COLON = 0x48, K_RETURN = 0x49,
    K_SHIFTLOCK = 0x50, K_S = 0x51, K_C = 0x52, K_G = 0x53, K_H = 0x54, K_N = 0x55, K_L = 0x56, K_SEMICOLON = 0x57, K_RBRACKET = 0x58, K_DELETE = 0x59,
    K_TAB = 0x60, K_Z = 0x61, K_SPACE = 0x62, K_V = 0x63, K_B = 0x64, K_M = 0x65, K_COMMA = 0x66, K_PERIOD = 0x67, K_SLASH = 0x68, K_COPY = 0x69,
    K_ESCAPE = 0x70, K_F1 = 0x71, K_F2 = 0x72, K_F3 = 0x73, K_F5 = 0x74, K_F6 = 0x75, K_F8 = 0x76, K_F9 = 0x77, K_BACKSLASH = 0x78, K_RIGHT = 0x79,
};

class Via;   // 6522

class Machine {
public:
    Machine();
    ~Machine();
    // os: MOS 1.20 (16 KB), basic: BASIC II (16 KB, sideways slot 15)
    bool setRoms(const std::vector<uint8_t>& os, const std::vector<uint8_t>& basic, std::string& err);
    void reset(bool power);              // power: memory cleared (power on); else BREAK
    // one 50 Hz frame (40 000 cycles of 2 MHz): the picture is drawn, the sound of the frame made
    void runFrame();
    // the picture: 640 x 512, 0x00RRGGBB
    static constexpr int W = 640, H = 512;
    const std::vector<uint32_t>& picture() const { return pix_; }
    // sound of the frames run since the last call: 16-bit mono at 44 100 Hz
    void takeSound(std::vector<int16_t>& out);
    static constexpr int RATE = 44100;
    // keys: held down / up (several at once)
    void key(int k, bool down);
    void releaseAll();
    // characters typed as on the BBC keyboard (each one pressed for a moment: what the PC sends as text)
    void typeChars(const std::string& s);
    // text given to BASIC as typed, fast (OSRDCH answered from it): a program's lines and RUN
    void paste(const std::string& s);
    bool pasting() const { return !paste_.empty(); }
    bool busyTyping() const { return !typed_.empty() || typeHold_ > 0; }
    uint64_t frames() const { return frame_; }
    bool capsLock() const { return !(ic32_ & 0x40); }
    // memory and state (tests)
    uint8_t peek(uint16_t a) const { return ram_[a & 0x7FFF]; }
    uint16_t pc() const { return pc_; }
    // the text in mode 7 screen memory (tests, status)
    std::string screenText() const;

private:
    // ---- 6502
    uint16_t pc_ = 0; uint8_t a_ = 0, x_ = 0, y_ = 0, s_ = 0xFD, p_ = 0x24;
    int64_t cycles_ = 0;        // run in the frame
    bool nmiPending_ = false;
    void step();
    uint8_t rd(uint16_t a);
    void wr(uint16_t a, uint8_t v);
    uint16_t rd16(uint16_t a) { return rd(a) | rd((uint16_t)(a + 1)) << 8; }
    void push(uint8_t v) { wr((uint16_t)(0x100 | s_--), v); }
    uint8_t pull() { return rd((uint16_t)(0x100 | ++s_)); }
    void irq(bool brk);
    void setNZ(uint8_t v) { p_ = (uint8_t)((p_ & 0x7D) | (v & 0x80) | (v ? 0 : 2)); }
    void adc(uint8_t v);
    void sbc(uint8_t v);
    void cmp(uint8_t r, uint8_t v) { uint16_t t = (uint16_t)(r - v); p_ = (uint8_t)((p_ & 0x7C) | (r >= v ? 1 : 0) | (t & 0x80) | ((uint8_t)t ? 0 : 2)); }
    // ---- memory
    std::array<uint8_t, 0x8000> ram_{};
    std::vector<uint8_t> os_, rom_[16];
    int romsel_ = 15;
    // ---- devices
    Via* sys_ = nullptr;
    Via* usr_ = nullptr;
    uint8_t ic32_ = 0;           // addressable latch on the system VIA port B
    uint8_t sdb_ = 0;            // slow data bus (system VIA port A out)
    void sysPortB(uint8_t v);    // latch writes
    void updateKeys();
    uint8_t sysPortAIn();
    bool keys_[16][8]{};
    // CRTC 6845 + ULA
    uint8_t crtcAddr_ = 0, crtc_[18]{};
    uint8_t ulaCtrl_ = 0, palette_[16]{};
    void render();
    void renderTeletext();
    std::vector<uint32_t> pix_;
    uint64_t frame_ = 0;
    // SN76489
    struct Psg {
        uint16_t period[4]{1, 1, 1, 0}; uint8_t vol[4]{15, 15, 15, 15};
        double phase[4]{}; int out[4]{1, 1, 1, 1}; uint16_t lfsr = 0x4000; int latch = 0; uint8_t noise = 0;
        void write(uint8_t v);
        int16_t sample();
    } psg_;
    bool soundEnable_ = true;    // latch bit 0 high = not writing
    std::vector<int16_t> snd_;
    double sndAcc_ = 0;
    void makeSound(int cyclesDone);
    // ADC (uPD7002): conversions end at once
    uint8_t adcStatus_ = 0x40, adcHigh_ = 0x80, adcLow_ = 0;
    // ---- keyboard input from the PC
    std::deque<std::pair<int, bool>> typed_;   // (key, shift) to press one by one
    int typeHold_ = 0;           // frames the current typed key stays down / up
    int typedKey_ = -1;
    bool typedShift_ = false, typedCtrl_ = false;
    bool heldShift_ = false;     // PC shift (for keys that are not text)
    void typingFrame();
    std::deque<uint8_t> paste_;
};

}  // namespace bbc
