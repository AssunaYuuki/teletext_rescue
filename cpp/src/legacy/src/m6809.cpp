// Процессор 6809: исполнение команд и дизассемблер.
#include "m6809.h"
#include <cstdio>

// такты страницы 1 по таблице Motorola (без добавки индексной адресации)
static const uint8_t CYC1[256] = {
    6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,   // 00
    0, 0, 2, 4, 0, 0, 5, 9, 0, 2, 3, 0, 3, 2, 8, 6,   // 10
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,   // 20
    4, 4, 4, 4, 5, 5, 5, 5, 0, 5, 3, 6, 20, 11, 0, 19,  // 30
    2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,   // 40
    2, 0, 0, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 0, 2,   // 50
    6, 0, 0, 6, 6, 0, 6, 6, 6, 6, 6, 0, 6, 6, 3, 6,   // 60
    7, 0, 0, 7, 7, 0, 7, 7, 7, 7, 7, 0, 7, 7, 4, 7,   // 70
    2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 4, 7, 3, 0,   // 80
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,   // 90
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,   // A0
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 8, 6, 6,   // B0
    2, 2, 2, 4, 2, 2, 2, 0, 2, 2, 2, 2, 3, 0, 3, 0,   // C0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,   // D0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,   // E0
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,   // F0
};

void M6809::reset() {
    DP = 0; CC = CC_I | CC_F; waiting = sync_wait = false; nmi_armed = nmi_req = false; illegal = false;
    PC = rd16(0xFFFE);
}

uint8_t M6809::add8(uint8_t a, uint8_t b, int c) {
    unsigned r = a + b + c;
    CC &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ b ^ r) & 0x10) CC |= CC_H;
    if ((a ^ r) & (b ^ r) & 0x80) CC |= CC_V;
    if (r & 0x100) CC |= CC_C;
    setNZ8((uint8_t)r);
    return (uint8_t)r;
}

uint8_t M6809::sub8(uint8_t a, uint8_t b, int c) {
    unsigned r = a - b - c;
    CC &= ~(CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ b) & (a ^ r) & 0x80) CC |= CC_V;
    if (r & 0x100) CC |= CC_C;
    setNZ8((uint8_t)r);
    return (uint8_t)r;
}

uint16_t M6809::add16(uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    CC &= ~(CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ r) & (b ^ r) & 0x8000) CC |= CC_V;
    if (r & 0x10000) CC |= CC_C;
    setNZ16((uint16_t)r);
    return (uint16_t)r;
}

uint16_t M6809::sub16(uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a - b;
    CC &= ~(CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ b) & (a ^ r) & 0x8000) CC |= CC_V;
    if (r & 0x10000) CC |= CC_C;
    setNZ16((uint16_t)r);
    return (uint16_t)r;
}

uint8_t M6809::rmw(int op, uint8_t v) {
    uint8_t r = v;
    switch (op) {
    case 0x0: r = sub8(0, v, 0); break;
    case 0x3: r = ~v; setNZ8(r); CC = (CC & ~CC_V) | CC_C; break;
    case 0x4: CC = (CC & ~CC_C) | (v & 1); r = v >> 1; setNZ8(r); break;
    case 0x6: r = (uint8_t)((CC & CC_C) << 7 | v >> 1); CC = (CC & ~CC_C) | (v & 1); setNZ8(r); break;
    case 0x7: CC = (CC & ~CC_C) | (v & 1); r = (v & 0x80) | v >> 1; setNZ8(r); break;
    case 0x8: r = (uint8_t)(v << 1); CC = (CC & ~(CC_C | CC_V)) | (v >> 7) | (((v ^ r) & 0x80) ? CC_V : 0); setNZ8(r); break;
    case 0x9: r = (uint8_t)(v << 1 | (CC & CC_C)); CC = (CC & ~(CC_C | CC_V)) | (v >> 7) | (((v ^ r) & 0x80) ? CC_V : 0); setNZ8(r); break;
    case 0xA: r = v - 1; CC = (CC & ~CC_V) | (v == 0x80 ? CC_V : 0); setNZ8(r); break;
    case 0xC: r = v + 1; CC = (CC & ~CC_V) | (v == 0x7F ? CC_V : 0); setNZ8(r); break;
    case 0xD: CC &= ~CC_V; setNZ8(v); break;
    case 0xF: r = 0; CC = (CC & ~(CC_N | CC_V | CC_C)) | CC_Z; break;
    default: illegal = true; break;
    }
    return r;
}

bool M6809::cond(int c) {
    bool C = CC & CC_C, Z = CC & CC_Z, N = CC & CC_N, V = CC & CC_V, r = true;
    switch (c >> 1) {
    case 0: r = true; break;            // BRA / BRN
    case 1: r = !(C || Z); break;       // BHI / BLS
    case 2: r = !C; break;              // BCC / BCS
    case 3: r = !Z; break;              // BNE / BEQ
    case 4: r = !V; break;              // BVC / BVS
    case 5: r = !N; break;              // BPL / BMI
    case 6: r = N == V; break;          // BGE / BLT
    case 7: r = !Z && N == V; break;    // BGT / BLE
    }
    return (c & 1) ? !r : r;
}

uint16_t M6809::indexed(int &cyc) {
    uint8_t pb = fetch();
    uint16_t &R = ireg((pb >> 5) & 3);
    uint16_t a;
    if (!(pb & 0x80)) {
        int off = pb & 0x1F; if (off & 0x10) off -= 0x20;
        cyc += 1;
        return (uint16_t)(R + off);
    }
    switch (pb & 0x0F) {
    case 0x0: a = R; R += 1; cyc += 2; break;
    case 0x1: a = R; R += 2; cyc += 3; break;
    case 0x2: R -= 1; a = R; cyc += 2; break;
    case 0x3: R -= 2; a = R; cyc += 3; break;
    case 0x4: a = R; break;
    case 0x5: a = (uint16_t)(R + (int8_t)B); cyc += 1; break;
    case 0x6: a = (uint16_t)(R + (int8_t)A); cyc += 1; break;
    case 0x8: a = (uint16_t)(R + (int8_t)fetch()); cyc += 1; break;
    case 0x9: a = (uint16_t)(R + fetch16()); cyc += 4; break;
    case 0xB: a = (uint16_t)(R + D()); cyc += 4; break;
    case 0xC: { int8_t o = (int8_t)fetch(); a = (uint16_t)(PC + o); cyc += 1; break; }
    case 0xD: { uint16_t o = fetch16(); a = (uint16_t)(PC + o); cyc += 5; break; }
    case 0xF: a = fetch16(); cyc += 2; break;
    default: illegal = true; a = R; break;
    }
    if (pb & 0x10) { a = rd16(a); cyc += 3; }
    return a;
}

uint16_t M6809::ea(int mode, int &cyc) {
    if (mode == 0) return (uint16_t)(DP << 8 | fetch());
    if (mode == 1) return indexed(cyc);
    return fetch16();
}

void M6809::push_all(bool entire) {
    pushS16(PC);
    if (entire) {
        pushS16(U); pushS16(Y); pushS16(X); pushS(DP); pushS(B); pushS(A);
    }
    pushS(CC);
}

uint16_t M6809::getr(int r) {
    switch (r) {
    case 0: return D(); case 1: return X; case 2: return Y; case 3: return U; case 4: return S; case 5: return PC;
    case 8: return 0xFF00 | A; case 9: return 0xFF00 | B; case 10: return 0xFF00 | CC; case 11: return 0xFF00 | DP;
    }
    return 0xFFFF;
}

void M6809::setr(int r, uint16_t v) {
    switch (r) {
    case 0: setD(v); break; case 1: X = v; break; case 2: Y = v; break; case 3: U = v; break;
    case 4: S = v; nmi_armed = true; break; case 5: PC = v; break;
    case 8: A = v & 0xFF; break; case 9: B = v & 0xFF; break; case 10: CC = v & 0xFF; break; case 11: DP = v & 0xFF; break;
    }
}

int M6809::step() {
    // прерывания
    if (nmi_req) {
        nmi_req = false;
        if (!waiting || sync_wait) { CC |= CC_E; push_all(true); }
        waiting = sync_wait = false;
        CC |= CC_I | CC_F; PC = rd16(0xFFFC); cycles += 19; return 19;
    }
    if (firq && !(CC & CC_F)) {
        if (!waiting || sync_wait) { CC &= ~CC_E; push_all(false); }
        waiting = sync_wait = false;
        CC |= CC_I | CC_F; PC = rd16(0xFFF6); cycles += 10; return 10;
    }
    if (irq && !(CC & CC_I)) {
        if (!waiting || sync_wait) { CC |= CC_E; push_all(true); }
        waiting = sync_wait = false;
        CC |= CC_I; PC = rd16(0xFFF8); cycles += 19; return 19;
    }
    if (waiting) {
        if (sync_wait && (irq || firq)) { waiting = sync_wait = false; }   // SYNC: замаскированное прерывание просто будит
        else { cycles += 1; return 1; }
    }

    uint8_t op = fetch();
    int cyc = CYC1[op];
    if (op == 0x10) { cyc = page2(fetch()); cycles += cyc; return cyc; }
    if (op == 0x11) { cyc = page3(fetch()); cycles += cyc; return cyc; }

    int hi = op >> 4, lo = op & 0x0F;
    uint16_t a; uint8_t v; uint16_t w;
    switch (hi) {
    case 0x0: case 0x6: case 0x7: {   // память: NEG..CLR, JMP
        int m = hi == 0 ? 0 : hi == 6 ? 1 : 2;
        a = ea(m, cyc);
        if (lo == 0xE) { PC = a; break; }
        v = rd(a);
        v = rmw(lo, v);
        if (lo != 0xD) wr(a, v);
        break;
    }
    case 0x4: A = rmw(lo, A); break;
    case 0x5: B = rmw(lo, B); break;
    case 0x1:
        switch (op) {
        case 0x12: break;   // NOP
        case 0x13: waiting = sync_wait = true; break;   // SYNC
        case 0x16: w = fetch16(); PC += w; break;   // LBRA
        case 0x17: w = fetch16(); pushS16(PC); PC += w; break;   // LBSR
        case 0x19: {   // DAA
            unsigned cf = 0, lsn = A & 0xF, msn = A >> 4;
            if ((CC & CC_H) || lsn > 9) cf |= 0x06;
            if ((CC & CC_C) || msn > 9 || (msn > 8 && lsn > 9)) cf |= 0x60;
            unsigned t = A + cf;
            CC &= ~CC_V; if (t & 0x100) CC |= CC_C;
            A = (uint8_t)t; setNZ8(A); break;
        }
        case 0x1A: CC |= fetch(); break;
        case 0x1C: CC &= fetch(); break;
        case 0x1D: A = (B & 0x80) ? 0xFF : 0; setNZ16(D()); CC &= ~CC_V; break;
        case 0x1E: { uint8_t pb = fetch(); uint16_t r1 = getr(pb >> 4), r2 = getr(pb & 15); setr(pb >> 4, r2); setr(pb & 15, r1); break; }
        case 0x1F: { uint8_t pb = fetch(); setr(pb & 15, getr(pb >> 4)); break; }
        default: illegal = true; break;
        }
        break;
    case 0x2: { int8_t o = (int8_t)fetch(); if (cond(lo)) PC = (uint16_t)(PC + o); break; }
    case 0x3:
        switch (op) {
        case 0x30: X = indexed(cyc); CC = (CC & ~CC_Z) | (X ? 0 : CC_Z); break;
        case 0x31: Y = indexed(cyc); CC = (CC & ~CC_Z) | (Y ? 0 : CC_Z); break;
        case 0x32: S = indexed(cyc); nmi_armed = true; break;
        case 0x33: U = indexed(cyc); break;
        case 0x34: case 0x36: {   // PSHS / PSHU
            uint8_t pb = fetch(); bool s = op == 0x34;
            auto p8 = [&](uint8_t x) { if (s) pushS(x); else pushU(x); cyc++; };
            auto p16 = [&](uint16_t x) { if (s) pushS16(x); else pushU16(x); cyc += 2; };
            if (pb & 0x80) p16(PC);
            if (pb & 0x40) p16(s ? U : S);
            if (pb & 0x20) p16(Y);
            if (pb & 0x10) p16(X);
            if (pb & 0x08) p8(DP);
            if (pb & 0x04) p8(B);
            if (pb & 0x02) p8(A);
            if (pb & 0x01) p8(CC);
            break;
        }
        case 0x35: case 0x37: {   // PULS / PULU
            uint8_t pb = fetch(); bool s = op == 0x35;
            auto q8 = [&]() { cyc++; return s ? pullS() : pullU(); };
            auto q16 = [&]() { cyc += 2; return s ? pullS16() : pullU16(); };
            if (pb & 0x01) CC = q8();
            if (pb & 0x02) A = q8();
            if (pb & 0x04) B = q8();
            if (pb & 0x08) DP = q8();
            if (pb & 0x10) X = q16();
            if (pb & 0x20) Y = q16();
            if (pb & 0x40) { if (s) U = q16(); else { S = q16(); nmi_armed = true; } }
            if (pb & 0x80) PC = q16();
            break;
        }
        case 0x39: PC = pullS16(); break;
        case 0x3A: X += B; break;
        case 0x3B:
            CC = pullS();
            if (CC & CC_E) { A = pullS(); B = pullS(); DP = pullS(); X = pullS16(); Y = pullS16(); U = pullS16(); cyc += 9; }
            PC = pullS16();
            break;
        case 0x3C: CC &= fetch(); CC |= CC_E; push_all(true); waiting = true; sync_wait = false; break;
        case 0x3D: { uint16_t r = (uint16_t)(A * B); setD(r); CC = (CC & ~(CC_Z | CC_C)) | (r ? 0 : CC_Z) | ((r & 0x80) ? CC_C : 0); break; }
        case 0x3F: CC |= CC_E; push_all(true); CC |= CC_I | CC_F; PC = rd16(0xFFFA); break;
        default: illegal = true; break;
        }
        break;
    default: {   // 0x8..0xF: операции с A/B/D/X/U
        int m = hi & 3;   // 0 imm, 1 dir, 2 idx, 3 ext
        bool isB = hi >= 0xC;
        uint8_t &R = isB ? B : A;
        bool wide = lo == 3 || lo == 0xC || lo == 0xE || lo == 0xF || (lo == 0xD && isB);
        if (op == 0x8D) { int8_t o = (int8_t)fetch(); pushS16(PC); PC = (uint16_t)(PC + o); break; }   // BSR
        if (lo == 0xD && !isB) { a = ea(m - 1, cyc); pushS16(PC); PC = a; break; }   // JSR
        bool store = lo == 7 || (lo == 0xD && isB) || lo == 0xF;
        if (m == 0 && store) { illegal = true; break; }
        if (m == 0) a = PC, PC += wide ? 2 : 1;
        else a = ea(m - 1, cyc);
        if (wide) {
            switch (lo) {
            case 0x3: w = rd16(a); if (isB) setD(add16(D(), w)); else setD(sub16(D(), w)); break;   // ADDD / SUBD
            case 0xC: w = rd16(a); if (isB) { setD(w); setNZ16(w); CC &= ~CC_V; } else sub16(X, w); break;   // LDD / CMPX
            case 0xD: setNZ16(D()); CC &= ~CC_V; wr16(a, D()); break;   // STD
            case 0xE: w = rd16(a); if (isB) U = w; else X = w; setNZ16(w); CC &= ~CC_V; break;   // LDU / LDX
            case 0xF: w = isB ? U : X; setNZ16(w); CC &= ~CC_V; wr16(a, w); break;   // STU / STX
            }
            break;
        }
        switch (lo) {
        case 0x0: R = sub8(R, rd(a), 0); break;
        case 0x1: sub8(R, rd(a), 0); break;
        case 0x2: R = sub8(R, rd(a), CC & CC_C); break;
        case 0x4: R &= rd(a); setNZ8(R); CC &= ~CC_V; break;
        case 0x5: setNZ8(R & rd(a)); CC &= ~CC_V; break;
        case 0x6: R = rd(a); setNZ8(R); CC &= ~CC_V; break;
        case 0x7: setNZ8(R); CC &= ~CC_V; wr(a, R); break;
        case 0x8: R ^= rd(a); setNZ8(R); CC &= ~CC_V; break;
        case 0x9: R = add8(R, rd(a), CC & CC_C); break;
        case 0xA: R |= rd(a); setNZ8(R); CC &= ~CC_V; break;
        case 0xB: R = add8(R, rd(a), 0); break;
        }
        break;
    }
    }
    if (cyc == 0) cyc = 2;
    cycles += cyc;
    return cyc;
}

int M6809::page2(uint8_t op) {
    int cyc = 5;
    if (op >= 0x21 && op <= 0x2F) { uint16_t o = fetch16(); if (cond(op & 15)) { PC += o; cyc = 6; } return cyc; }
    if (op == 0x3F) { CC |= CC_E; push_all(true); PC = rd16(0xFFF4); return 20; }
    int hi = op >> 4, lo = op & 15, m = hi & 3;
    uint16_t a;
    if (hi < 8 || (lo != 3 && lo != 0xC && lo != 0xE && lo != 0xF)) { illegal = true; return 2; }
    bool isS = hi >= 0xC;
    if (m == 0) { if (lo == 0xF) { illegal = true; return 2; } a = PC; PC += 2; cyc = 4; }
    else { a = ea(m - 1, cyc); cyc += 2; }
    switch (lo) {
    case 0x3: if (isS) illegal = true; else sub16(D(), rd16(a)); break;   // CMPD
    case 0xC: if (isS) illegal = true; else sub16(Y, rd16(a)); break;     // CMPY
    case 0xE: { uint16_t w = rd16(a); if (isS) { S = w; nmi_armed = true; } else Y = w; setNZ16(w); CC &= ~CC_V; break; }   // LDS / LDY
    case 0xF: { uint16_t w = isS ? S : Y; setNZ16(w); CC &= ~CC_V; wr16(a, w); break; }   // STS / STY
    }
    return cyc;
}

int M6809::page3(uint8_t op) {
    int cyc = 5;
    if (op == 0x3F) { CC |= CC_E; push_all(true); PC = rd16(0xFFF2); return 20; }
    int hi = op >> 4, lo = op & 15, m = hi & 3;
    if (hi < 8 || hi > 0xB || (lo != 3 && lo != 0xC)) { illegal = true; return 2; }
    uint16_t a;
    if (m == 0) { a = PC; PC += 2; } else { a = ea(m - 1, cyc); cyc += 2; }
    sub16(lo == 3 ? U : S, rd16(a));   // CMPU / CMPS
    return cyc;
}

// ---------------------------------------------------------------- дизассемблер

static const char *RMW[16] = {"NEG", 0, 0, "COM", "LSR", 0, "ROR", "ASR", "ASL", "ROL", "DEC", 0, "INC", "TST", "JMP", "CLR"};
static const char *BR[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ", "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
static const char *ALU_A[16] = {"SUBA", "CMPA", "SBCA", "SUBD", "ANDA", "BITA", "LDA", "STA", "EORA", "ADCA", "ORA", "ADDA", "CMPX", "JSR", "LDX", "STX"};
static const char *ALU_B[16] = {"SUBB", "CMPB", "SBCB", "ADDD", "ANDB", "BITB", "LDB", "STB", "EORB", "ADCB", "ORB", "ADDB", "LDD", "STD", "LDU", "STU"};
static const char *REGN[16] = {"D", "X", "Y", "U", "S", "PC", "?", "?", "A", "B", "CC", "DP", "?", "?", "?", "?"};

static std::string hx(unsigned v, int w) { char b[16]; snprintf(b, sizeof b, w == 2 ? "$%02X" : "$%04X", v); return b; }

static std::string dis_idx(Bus6809 &bus, uint16_t &p) {
    uint8_t pb = bus.peek(p++);
    static const char *RN[4] = {"X", "Y", "U", "S"};
    const char *r = RN[(pb >> 5) & 3];
    char b[64];
    if (!(pb & 0x80)) { int off = pb & 0x1F; if (off & 0x10) off -= 0x20; snprintf(b, sizeof b, "%d,%s", off, r); return b; }
    std::string s;
    switch (pb & 15) {
    case 0x0: s = std::string(",") + r + "+"; break;
    case 0x1: s = std::string(",") + r + "++"; break;
    case 0x2: s = std::string(",-") + r; break;
    case 0x3: s = std::string(",--") + r; break;
    case 0x4: s = std::string(",") + r; break;
    case 0x5: s = std::string("B,") + r; break;
    case 0x6: s = std::string("A,") + r; break;
    case 0x8: { int8_t o = (int8_t)bus.peek(p++); snprintf(b, sizeof b, "%d,%s", o, r); s = b; break; }
    case 0x9: { uint16_t o = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; s = hx(o, 4) + "," + r; break; }
    case 0xB: s = std::string("D,") + r; break;
    case 0xC: { int8_t o = (int8_t)bus.peek(p++); s = hx((uint16_t)(p + o), 4) + ",PCR"; break; }
    case 0xD: { uint16_t o = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; s = hx((uint16_t)(p + o), 4) + ",PCR"; break; }
    case 0xF: { uint16_t o = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; s = hx(o, 4); break; }
    default: s = "???"; break;
    }
    return (pb & 0x10) ? "[" + s + "]" : s;
}

static std::string dis_mode(Bus6809 &bus, uint16_t &p, int m, bool wide) {
    if (m == 0) {
        if (wide) { uint16_t v = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; return "#" + hx(v, 4); }
        return "#" + hx(bus.peek(p++), 2);
    }
    if (m == 1) return "<" + hx(bus.peek(p++), 2);
    if (m == 2) return dis_idx(bus, p);
    uint16_t v = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; return hx(v, 4);
}

static std::string reglist(uint8_t pb, bool s) {
    static const char *N[8] = {"CC", "A", "B", "DP", "X", "Y", "?", "PC"};
    std::string r;
    for (int i = 0; i < 8; i++) if (pb & (1 << i)) { if (!r.empty()) r += ","; r += i == 6 ? (s ? "U" : "S") : N[i]; }
    return r;
}

std::string dasm6809(Bus6809 &bus, uint16_t pc, int &len) {
    uint16_t p = pc;
    uint8_t op = bus.peek(p++);
    std::string s;
    int hi = op >> 4, lo = op & 15;
    if (op == 0x10 || op == 0x11) {
        uint8_t o2 = bus.peek(p++);
        int h2 = o2 >> 4, l2 = o2 & 15;
        if (op == 0x10 && o2 >= 0x21 && o2 <= 0x2F) {
            uint16_t o = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2;
            s = std::string("L") + BR[l2] + " " + hx((uint16_t)(p + o), 4);
        } else if (o2 == 0x3F) s = op == 0x10 ? "SWI2" : "SWI3";
        else if (h2 >= 8) {
            const char *n = nullptr;
            if (op == 0x10) {
                if (l2 == 3 && h2 < 0xC) n = "CMPD"; else if (l2 == 0xC && h2 < 0xC) n = "CMPY";
                else if (l2 == 0xE) n = h2 < 0xC ? "LDY" : "LDS"; else if (l2 == 0xF && (h2 & 3)) n = h2 < 0xC ? "STY" : "STS";
            } else if (h2 < 0xC) { if (l2 == 3) n = "CMPU"; else if (l2 == 0xC) n = "CMPS"; }
            if (n) s = std::string(n) + " " + dis_mode(bus, p, h2 & 3, true);
        }
        if (s.empty()) s = "???";
    } else if (hi == 0 || hi == 6 || hi == 7) {
        s = RMW[lo] ? RMW[lo] : "???";
        s += " " + dis_mode(bus, p, hi == 0 ? 1 : hi == 6 ? 2 : 3, false);
    } else if (hi == 4 || hi == 5) {
        s = RMW[lo] && lo != 0xE ? std::string(RMW[lo]) + (hi == 4 ? "A" : "B") : "???";
    } else if (hi == 1) {
        switch (op) {
        case 0x12: s = "NOP"; break; case 0x13: s = "SYNC"; break;
        case 0x16: case 0x17: { uint16_t o = (uint16_t)(bus.peek(p) << 8 | bus.peek((uint16_t)(p + 1))); p += 2; s = std::string(op == 0x16 ? "LBRA " : "LBSR ") + hx((uint16_t)(p + o), 4); break; }
        case 0x19: s = "DAA"; break;
        case 0x1A: s = "ORCC #" + hx(bus.peek(p++), 2); break;
        case 0x1C: s = "ANDCC #" + hx(bus.peek(p++), 2); break;
        case 0x1D: s = "SEX"; break;
        case 0x1E: case 0x1F: { uint8_t pb = bus.peek(p++); s = std::string(op == 0x1E ? "EXG " : "TFR ") + REGN[pb >> 4] + "," + REGN[pb & 15]; break; }
        default: s = "???";
        }
    } else if (hi == 2) {
        int8_t o = (int8_t)bus.peek(p++);
        s = std::string(BR[lo]) + " " + hx((uint16_t)(p + o), 4);
    } else if (hi == 3) {
        switch (op) {
        case 0x30: s = "LEAX " + dis_idx(bus, p); break; case 0x31: s = "LEAY " + dis_idx(bus, p); break;
        case 0x32: s = "LEAS " + dis_idx(bus, p); break; case 0x33: s = "LEAU " + dis_idx(bus, p); break;
        case 0x34: s = "PSHS " + reglist(bus.peek(p++), true); break; case 0x35: s = "PULS " + reglist(bus.peek(p++), true); break;
        case 0x36: s = "PSHU " + reglist(bus.peek(p++), false); break; case 0x37: s = "PULU " + reglist(bus.peek(p++), false); break;
        case 0x39: s = "RTS"; break; case 0x3A: s = "ABX"; break; case 0x3B: s = "RTI"; break;
        case 0x3C: s = "CWAI #" + hx(bus.peek(p++), 2); break; case 0x3D: s = "MUL"; break; case 0x3F: s = "SWI"; break;
        default: s = "???";
        }
    } else {
        bool isB = hi >= 0xC;
        int m = hi & 3;
        if (op == 0x8D) { int8_t o = (int8_t)bus.peek(p++); s = "BSR " + hx((uint16_t)(p + o), 4); }
        else if (op == 0xCD || op == 0x87 || op == 0xC7 || op == 0x8F || op == 0xCF) s = "???";
        else {
            bool wide = lo == 3 || lo == 0xC || lo == 0xE || lo == 0xF || (lo == 0xD && isB);
            s = std::string(isB ? ALU_B[lo] : ALU_A[lo]) + " " + dis_mode(bus, p, m, wide);
        }
    }
    len = (uint16_t)(p - pc);
    return s;
}
