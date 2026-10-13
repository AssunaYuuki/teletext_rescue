# Gemstar compressed text, a port of FREQCCD_.EXE segment 2a4f (checked against the disassembly):
#   2a4f:000c decompress(src, len, dst)   2a4f:01cb one token   2a4f:068d ld_dict(DDPACKET.BIN)
#   2a4f:05ce/060a dynamic dictionary word   2a4f:06d5 built-in dictionary word   2a4f:05a7 digram
import struct

class Tables:
    def __init__(self, exe, dictfile=None):
        d = open(exe, 'rb').read()
        ds = struct.unpack_from('<H', d, 8)[0] * 16 + (0x33cf - 0x1000) * 16
        self.ds = d[ds: ds + 0x8000]
        self.dict = open(dictfile, 'rb').read() if dictfile else b''
    def b(self, off): return self.ds[off]

class Decoder:
    def __init__(self, t):
        self.t = t
    def decode(self, data):
        self.inp = list(data)          # tokens still to read (front = next)
        self.out = []
        self.F = 4
        self.eof = False
        while self.inp and not self.eof:
            self.token()
        return bytes(self.out)
    def getc(self):
        if not self.inp:
            self.eof = True; return 0
        return self.inp.pop(0)
    def put(self, c): self.out.append(c & 0xff)
    def space(self): self.put(0x20)
    def space2(self): self.F &= 0xf3; self.put(0x20)
    def digram(self, c):
        i = (c & 0x7f) * 2
        return self.t.b(0x46f3 + i), self.t.b(0x46f4 + i)
    def setgroup(self, n):
        h = self.t.dict
        self.gbase = struct.unpack_from('<H', h, n * 8)[0] - 0x7a00
        self.glen = h[n * 8 + 2]
        self.gcnt = h[n * 8 + 3: n * 8 + 8] + h[n * 8 + 8: n * 8 + 16]
    def pushdyn(self, c):
        ln, off, k = self.glen, self.gbase, 0
        while k < len(self.gcnt) and self.gcnt[k] <= c:
            off += self.gcnt[k] * ln; c -= self.gcnt[k]; ln += 1; k += 1
            if k > 8: return
        p = off + c * ln
        self.inp[0:0] = list(self.t.dict[p:p + ln])
    def pushstatic(self, c):
        up = c & 0x80
        c &= 0x7f
        ln = self.t.b(0x4590)
        off, k = 0, 0
        while self.t.b(0x4591 + k) <= c:
            off += ln * self.t.b(0x4591 + k); c -= self.t.b(0x4591 + k); ln += 1; k += 1
            if k > 8: return
        off += c * ln
        word = [self.t.b(0x4595 + off + i) for i in range(ln)]
        res = []
        for b in word:
            if not up: res.append(b)
            elif b < 0x80: res.append((b + 0x1a) & 0xff)
            else:
                x, y = self.digram(b)
                res.append(0x7f if x == 0x27 else (x - 7) & 0xff)
                res.append((y - 7) & 0xff)
        self.inp[0:0] = res
    def emit(self, c, si, c3=0, tri=None):
        # labels 0x28b / 0x293 / 0x2c4 of 2a4f:01cb
        while True:
            if si != 'skip':
                if self.F & 1:
                    self.F ^= 1
                    if not self.F & 4: self.space()
                if self.F & 2:
                    self.F ^= 2
                    if c & 0x40: c = (c - 0x20) & 0xff
            self.put(c)
            self.F &= 0xfb
            if si == 1:
                si = 'skip'; c = c3; continue
            if si == 3 and tri:
                c = tri.pop(0); self.F &= 0xf7
                if not tri: si = 0
                continue
            return
    def number(self):
        while True:
            c = self.getc()
            if self.eof: return
            c3 = c
            self.F |= 0x20
            n = (c >> 4) & 0xf
            while True:
                if n == 0xf:
                    if self.F & 0x20:
                        k = c3 & 0xf
                        for _ in range(k): self.space2()
                    self.F = 0
                    return
                self.put(n + 0x30 if n < 10 else self.t.b(0x46c4 + n))
                if self.F & 0x20:
                    self.F ^= 0x20; n = c3 & 0xf; continue
                break
    def token(self):
        F = self
        if self.F & 0x10:
            return self.number()
        c = self.getc()
        if self.eof: return
        if c >= 0x80:
            a, b = self.digram(c)
            self.F &= 0xf7
            return self.emit(a, 1, b)
        orig = c
        if c < 0x20:
            self.F |= 1
            if c == 0:
                return self.emit(0x26, 0)
            c = self.t.b(0x408a + orig)
            if c & 0x80: self.F |= 2; c &= 0x7f
            return self.pushstatic(c)
        if c & 0x40:
            if c <= 0x73:
                if c <= 0x59:
                    self.F &= 0xf7
                    return self.emit(orig + 0x21, 0)
                c = orig - 0x19
                if not self.F & 8: self.F |= 1; self.F |= 8
                return self.emit(c, 0)
            c = self.t.b(0x464e + orig)
            if c & 0x80:
                self.F |= 8
                return self.emit(c & 0x7f, 0)
            if c & 0x40: return self.emit(c & 0x3f, 0)
            self.F &= 0xf7
            return self.emit(c, 0)
        if c & 0x10:
            if orig <= 0x39:
                base = (orig * 3 + 0x70) & 0xff
                tri = [self.t.b(0x46d4 + base + k) for k in range(3)]
                self.F &= 0xf7
                return self.emit(tri.pop(0), 3, tri=tri)
            if orig in (0x3a, 0x3b):
                if orig == 0x3b: self.space2()
                c = self.getc()
                if self.eof: return
                if c & 0x80 and c & 0x40:
                    c &= 0x7f; self.put(0x1b)
                if c == 0x5b: self.F = 8
                return self.emit(c, 0)
            if orig == 0x3c:
                self.F |= 3; self.F &= 0xf7; return
            if orig == 0x3d:
                self.F = 0x10 | (self.F & 4)
                return self.number()
            self.F &= 0xfd
            if orig == 0x3e:
                self.F = 4; self.put(0x3e & 0xd); return
            if self.F & 4: self.F ^= 4
            else: self.F ^= 8
            return
        if c & 8: self.F = 1
        if c & 4: self.F |= 2
        n = c & 3
        if n != 3:
            self.setgroup(n)
            c = self.getc()
            if self.eof: return
            return self.pushdyn(c)
        c = self.getc()
        if self.eof: return
        if c == 0:
            return self.emit(0x26, 0)
        if c & 0x80: self.F &= 0xfd
        return self.pushstatic(c)
