import sys, collections
TWO = lambda t: t in (3,5,11,12,21,22,23,24,29) or t >= 30
def cmds(path):
    for l in open(path):
        l = l.strip()
        if not l.startswith('2c'): continue
        p = bytes.fromhex(l); j = 11; end = len(p) - 4
        while j < end:
            t = p[j] & 0x3f
            L = (p[j+1] << 8 | p[j+2]) if TWO(t) else p[j+1]
            if L < 2: break
            yield p[j:j+L]; j += L
if __name__ == '__main__':
    want = int(sys.argv[2]); c = collections.Counter(); n = 0
    for x in cmds(sys.argv[1]):
        if x[0] & 0x3f != want: continue
        c[(x[0] & 0x80, x[2] & 0x80)] += 1
        if n < int(sys.argv[3] if len(sys.argv) > 3 else 15): print(x.hex(' ')); n += 1
    print(c)
