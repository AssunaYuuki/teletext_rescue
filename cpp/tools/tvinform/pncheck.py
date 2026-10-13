"""Check that TV-Inform test packets carry 7-bit LFSR states with odd parity."""
import sys

pk = {
    0: '57 ab d5 2a d5 ea 75 7a fd fe bf df 2f 97 0b 65 a2 51 a8 54 ea b5 da 6d 76 3b 9d ee',
    7: 'da ad d6 ab 15 8a 45 62 f1 38 dc 6e f7 fb fd 3e 1f 4f a7 13 49 64 32 19 8c 86 43 d2',
}
if len(sys.argv) > 1:
    pk = {i: l for i, l in enumerate(open(sys.argv[1]).read().split('\n')) if l.strip()}

def par(b):
    return bin(b).count('1') & 1

for c, s in pk.items():
    b = bytes.fromhex(s)
    print('CI', c, 'odd parity:', ''.join('1' if par(x) else '0' for x in b))
    v = [x & 0x7f for x in b]
    # bit stream: byte n+1 = (byte n >> 1) | new<<6  -> new bits
    seq = [(v[0] >> i) & 1 for i in range(7)]
    ok = 0
    for n in range(1, len(v)):
        if (v[n] & 0x3f) == (v[n - 1] >> 1):
            ok += 1
        seq.append(v[n] >> 6)
    print('   window overlap ok %d/%d' % (ok, len(v) - 1))
    # find LFSR taps: s[k+7] = XOR of s[k+t] for t in taps
    for mask in range(1, 128):
        if all(seq[k + 7] == (sum(seq[k + t] for t in range(7) if mask >> t & 1) & 1)
               for k in range(1, len(seq) - 8)):
            print('   recurrence taps mask', bin(mask))
