import sys, random, struct
sys.argv = ['x', '../../../Gemstar Freqccd_ Version 1.5/FREQCCD_.EXE']
exec(open('pluscode_emu.py').read().replace("if __name__ == '__main__':", "if False:"))
import pluscode
e = Emu(sys.argv[1])
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 3)
bad = []
for _ in range(3000):
    y = random.randint(0, 99); m = random.randint(1, 12); c = random.choice([random.randint(1, 999), random.randint(1000, 999999), random.randint(1, 99999999)])
    z, _o = call(e, 0x28ec, 0x07b0, [y, 0, m, 0, c & 0xffff, c >> 16, 0, 0x8000])
    z = z - (1 << 32) if z >= 1 << 31 else z
    p = pluscode.mix(y, m, c)
    if z != p: bad.append((y, m, c, len(str(c)), z, p))
print(len(bad), 'bad of 3000')
for b in bad[:15]: print(b)
