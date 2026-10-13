// Процессор Motorola 6809 (MC6809 / MC68B09E / HD68A09EP): все команды, режимы адресации, прерывания.
// Память и порты — через Bus, его реализует машина (Norpak TTX650, Panasonic TU-2000x).
#pragma once
#include <cstdint>
#include <string>

struct Bus6809 {
    virtual uint8_t rd(uint16_t a) = 0;
    virtual void wr(uint16_t a, uint8_t v) = 0;
    virtual uint8_t peek(uint16_t a) { return rd(a); }   // чтение без побочных эффектов (дизассемблер)
    virtual ~Bus6809() {}
};

struct M6809 {
    enum { CC_C = 1, CC_V = 2, CC_Z = 4, CC_N = 8, CC_I = 16, CC_H = 32, CC_F = 64, CC_E = 128 };
    uint8_t A = 0, B = 0, DP = 0, CC = CC_I | CC_F;
    uint16_t X = 0, Y = 0, U = 0, S = 0, PC = 0;
    bool irq = false, firq = false;       // уровни линий (активны, пока машина их держит)
    bool nmi_armed = false, nmi_req = false;
    bool waiting = false;                 // CWAI или SYNC
    bool sync_wait = false;
    bool illegal = false;                 // встретилась неизвестная команда
    uint64_t cycles = 0;
    Bus6809 *bus = nullptr;

    void reset();
    void nmi() { if (nmi_armed) nmi_req = true; }
    int step();                           // одна команда (или приём прерывания); возвращает такты

    uint16_t D() const { return (uint16_t)(A << 8 | B); }
    void setD(uint16_t v) { A = v >> 8; B = v & 0xFF; }

private:
    uint8_t rd(uint16_t a) { return bus->rd(a); }
    void wr(uint16_t a, uint8_t v) { bus->wr(a, v); }
    uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) << 8 | rd((uint16_t)(a + 1))); }
    void wr16(uint16_t a, uint16_t v) { wr(a, v >> 8); wr((uint16_t)(a + 1), v & 0xFF); }
    uint8_t fetch() { return rd(PC++); }
    uint16_t fetch16() { uint16_t v = rd16(PC); PC += 2; return v; }
    void pushS(uint8_t v) { wr(--S, v); }
    void pushS16(uint16_t v) { pushS(v & 0xFF); pushS(v >> 8); }
    uint8_t pullS() { return rd(S++); }
    uint16_t pullS16() { uint16_t h = pullS(); return (uint16_t)(h << 8 | pullS()); }
    void pushU(uint8_t v) { wr(--U, v); }
    void pushU16(uint16_t v) { pushU(v & 0xFF); pushU(v >> 8); }
    uint8_t pullU() { return rd(U++); }
    uint16_t pullU16() { uint16_t h = pullU(); return (uint16_t)(h << 8 | pullU()); }
    uint16_t &ireg(int r) { return r == 0 ? X : r == 1 ? Y : r == 2 ? U : S; }
    uint16_t indexed(int &cyc);
    uint16_t ea(int mode, int &cyc);   // 0 direct, 1 indexed, 2 extended
    void push_all(bool entire);
    void setNZ8(uint8_t v) { CC = (CC & ~(CC_N | CC_Z)) | (v & 0x80 ? CC_N : 0) | (v ? 0 : CC_Z); }
    void setNZ16(uint16_t v) { CC = (CC & ~(CC_N | CC_Z)) | (v & 0x8000 ? CC_N : 0) | (v ? 0 : CC_Z); }
    uint8_t add8(uint8_t a, uint8_t b, int c);
    uint8_t sub8(uint8_t a, uint8_t b, int c);
    uint16_t add16(uint16_t a, uint16_t b);
    uint16_t sub16(uint16_t a, uint16_t b);
    uint8_t rmw(int op, uint8_t v);   // NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR по младшей тетраде кода
    bool cond(int c);
    uint16_t getr(int r);
    void setr(int r, uint16_t v);
    int page2(uint8_t op);
    int page3(uint8_t op);
};

// Дизассемблер: строка команды по адресу pc, len — её длина.
std::string dasm6809(Bus6809 &bus, uint16_t pc, int &len);
