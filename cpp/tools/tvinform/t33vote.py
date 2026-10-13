"""TV-Inform / Norpak NABTS packets (.t33 from the legacy engine): Hamming-decode
headers, group by continuity index, vote repeated copies, print the result."""
import sys
import numpy as np

H84 = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
       0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]


def ham(b):
    best = min(range(16), key=lambda v: bin(H84[v] ^ b).count('1'))
    return best if bin(H84[best] ^ b).count('1') <= 1 else -1


raw = np.fromfile(sys.argv[1], np.uint8)
pk = raw[: len(raw) // 33 * 33].reshape(-1, 33)
print(len(pk), 'packets')
hdr = [[ham(b) for b in p[:5]] for p in pk]
from collections import Counter
print('addr', Counter(tuple(h[:3]) for h in hdr).most_common(5))
print('PS', Counter(h[4] for h in hdr).most_common(5))
# sequence of CI
ci = [h[3] for h in hdr]
print('CI seq', ''.join('%x' % c if c >= 0 else '?' for c in ci[:200]))

# cluster each CI's payloads (Hamming distance <= 24 bits) and vote
groups = {}
for p, h in zip(pk, hdr):
    if h[:3] != [15, 0, 0] or h[3] < 0:
        continue
    bits = np.unpackbits(p[5:])
    gl = groups.setdefault(h[3], [])
    for g in gl:
        if np.count_nonzero(g[0] != bits) <= 24:
            g[1].append(bits)
            break
    else:
        gl.append([bits, [bits]])
for c in sorted(groups):
    for g in sorted(groups[c], key=lambda g: -len(g[1]))[:4]:
        v = (np.mean(g[1], 0) > 0.5).astype(np.uint8)
        print('CI %2d n=%4d  %s' % (c, len(g[1]), np.packbits(v).tobytes().hex(' ')))
