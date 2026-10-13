# Runs FREQCCD_.EXE's own PlusCode decoder (FUN_28ec_0fcf) under unicorn as a reference:
#   fcf(long year%100, long month, long pluscode, long* channel, long* yyyymmdd, long* hhmm, long* minutes)
# usage: pluscode_emu.py EXE year month code [code...]
import sys, struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE
from unicorn.x86_const import *

class Emu:
    def __init__(self, exe):
        d = open(exe, 'rb').read()
        nrel, hdrp, relo = struct.unpack_from('<H', d, 6)[0], struct.unpack_from('<H', d, 8)[0], struct.unpack_from('<H', d, 0x18)[0]
        img = bytearray(d[hdrp * 16:])
        for i in range(nrel):
            off, seg = struct.unpack_from('<HH', d, relo + 4 * i)
            p = seg * 16 + off
            struct.pack_into('<H', img, p, (struct.unpack_from('<H', img, p)[0] + 0x1000) & 0xffff)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(0x10000, bytes(img))
        self.uc.mem_write(0xF0000, b'\xf4')          # hlt as the return address
    def decode(self, year, month, code):
        uc = self.uc
        out = 0x80000                                  # 8000:0000 output longs
        uc.mem_write(out, bytes(16))
        args = [year % 100, 0, month, 0, code & 0xffff, code >> 16]
        for k in range(4): args += [k * 4, 0x8000]
        sp = 0xFF00
        stack = struct.pack('<HH', 0, 0xF000) + b''.join(struct.pack('<H', a & 0xffff) for a in args)
        sp -= len(stack)
        uc.mem_write(0x90000 + sp, stack)
        uc.reg_write(UC_X86_REG_SS, 0x9000); uc.reg_write(UC_X86_REG_SP, sp)
        uc.reg_write(UC_X86_REG_DS, 0x33cf); uc.reg_write(UC_X86_REG_ES, 0x33cf)
        uc.reg_write(UC_X86_REG_CS, 0x28ec)
        uc.emu_start(0x28ec * 16 + 0x0fcf, 0xF0000, count=2000000)
        return struct.unpack('<4l', bytes(uc.mem_read(out, 16)))

if __name__ == '__main__':
    e = Emu(sys.argv[1])
    y, m = int(sys.argv[2]), int(sys.argv[3])
    for c in sys.argv[4:]:
        print(c, e.decode(y, m, int(c)))

def call(emu, seg, off, words, outseg=0x8000, outlen=24):
    uc = emu.uc
    uc.mem_write(outseg * 16, bytes(outlen))
    sp = 0xFF00
    stack = struct.pack('<HH', 0, 0xF000) + b''.join(struct.pack('<H', a & 0xffff) for a in words)
    sp -= len(stack)
    uc.mem_write(0x90000 + sp, stack)
    uc.reg_write(UC_X86_REG_SS, 0x9000); uc.reg_write(UC_X86_REG_SP, sp)
    uc.reg_write(UC_X86_REG_DS, 0x33cf); uc.reg_write(UC_X86_REG_ES, 0x33cf)
    uc.reg_write(UC_X86_REG_CS, seg)
    uc.emu_start(seg * 16 + off, 0xF0000, count=2000000)
    ax, dx = uc.reg_read(UC_X86_REG_AX), uc.reg_read(UC_X86_REG_DX)
    return (dx << 16 | ax), bytes(uc.mem_read(outseg * 16, outlen))
