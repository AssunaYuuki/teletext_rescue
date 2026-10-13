"""Statistics of a .t33 (+ .t33.pos) NABTS packet dump: lines, addresses, PS,
test-PRBS packets vs data packets. Writes data packets (non-test) to <out>.bin."""
import sys
from collections import Counter
import numpy as np

H84 = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
       0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]
HAM = np.full(256, -1, np.int16)
for v, c in enumerate(H84):
    HAM[c] = v
    for k in range(8):
        HAM[c ^ (1 << k)] = v

f = sys.argv[1]
raw = np.fromfile(f, np.uint8)
pk = raw[: len(raw) // 33 * 33].reshape(-1, 33)
pos = np.fromfile(f + '.pos', np.uint32).reshape(-1, 2)[: len(pk)]
print(len(pk), 'packets')
print('lines', Counter(pos[:, 1].tolist()).most_common(12))
h = HAM[pk[:, :5]]
addr = h[:, 0] * 256 + h[:, 1] * 16 + h[:, 2]
print('addr', [('%03x' % a if a >= 0 else 'bad', n) for a, n in Counter(addr.tolist()).most_common(12)])
print('PS', Counter(h[:, 4].tolist()).most_common(8))

# test PRBS: bytes 6..31 odd parity and 7-bit window overlap
d = pk[:, 6:32].astype(np.int32)
par = np.unpackbits(pk[:, 6:32, None], axis=2).sum(2) & 1
v = d & 0x7f
ov = (v[:, 1:] & 0x3f) == (v[:, :-1] >> 1)
test = (par.mean(1) > 0.85) & (ov.mean(1) > 0.8)
print('test-PRBS packets', int(test.sum()), 'other', int((~test).sum()))
for a, n in Counter(addr[~test].tolist()).most_common(10):
    m = (~test) & (addr == a)
    print('  addr %s  n=%d lines %s PS %s' % ('%03x' % a if a >= 0 else 'bad', n,
          Counter(pos[m, 1].tolist()).most_common(4), Counter(h[m, 4].tolist()).most_common(4)))
if len(sys.argv) > 2:
    sel = ~test & (addr >= 0)
    np.savez(sys.argv[2], pk=pk[sel], pos=pos[sel], addr=addr[sel], h=h[sel])
    print('saved', int(sel.sum()))
