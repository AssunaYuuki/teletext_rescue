# Disassemble a range of FREQCCD_.EXE given Ghidra seg:off (image loaded at segment 0x1000).
# usage: dis16.py EXE seg:off count   |  dis16.py EXE seg:off -words N  (dump words)
import sys, struct, capstone
exe = open(sys.argv[1], 'rb').read()
hdr = struct.unpack_from('<H', exe, 8)[0] * 16
seg, off = (int(x, 16) for x in sys.argv[2].split(':'))
def fo(s, o): return hdr + (s - 0x1000) * 16 + o
if sys.argv[3] == '-words':
    n = int(sys.argv[4], 0); p = fo(seg, off)
    print(' '.join('%04x' % w for w in struct.unpack_from('<%dH' % n, exe, p))); sys.exit()
if sys.argv[3] == '-bytes':
    n = int(sys.argv[4], 0); p = fo(seg, off)
    print(exe[p:p+n].hex(' ')); sys.exit()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
p = fo(seg, off)
for i in md.disasm(exe[p:p + int(sys.argv[3], 0)], off):
    print('%04x  %-24s %s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
