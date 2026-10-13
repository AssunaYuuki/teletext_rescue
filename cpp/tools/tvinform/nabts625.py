"""Slice TV-Inform NABTS-style packets (55 55 E7 + 33 bytes at the NTSC NABTS rate 5.727 MHz)
from a bttv-pal raw .vbi (2048 x 32 rows, 35.46895 MHz).
usage: nabts625.py file.vbi out_prefix [rows=0,1,2,16,17,18] [first_line=7]
writes out_prefix.t33 + .t33.pos (u32 frame, u32 line), same as the legacy engine."""
import sys
import numpy as np

f, out = sys.argv[1], sys.argv[2]
rows = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else '0,1,2,16,17,18').split(',')]
first = int(sys.argv[4]) if len(sys.argv) > 4 else 7
FS, RATE = 35468950.0, 8 * 3579545.4545 / 5     # NTSC NABTS rate (Norpak)
T = FS / RATE
NB = 36 * 8
a = np.memmap(f, dtype=np.uint8, mode='r')
NF = a.size // 65536
a = a[: NF * 65536].reshape(NF, 32, 2048)
SYNC = np.unpackbits(np.array([0x55, 0x55, 0xE7], np.uint8), bitorder='little')
PH = np.arange(0, T, T / 10)
fo = open(out + '.t33', 'wb')
po = open(out + '.t33.pos', 'wb')
cnt = 0
CH = 2000
for c0 in range(0, NF, CH):
    blk = np.asarray(a[c0:c0 + CH][:, rows, :], dtype=np.float32)   # n x r x 2048
    n = blk.shape[0]
    L = blk.reshape(-1, 2048)
    lo = np.percentile(L[:, 100:1900], 5, axis=1)
    hi = np.percentile(L[:, 100:1900], 95, axis=1)
    thr = ((lo + hi) / 2)[:, None]
    sig = (hi - lo) > 40
    above = L > thr
    st = np.argmax(above[:, 20:], axis=1) + 20             # first rising edge of run-in
    best = np.full(len(L), -1.0)
    bits_best = np.zeros((len(L), NB), np.uint8)
    for sh in range(-2, 3):                                  # run-in may start one bit early/late
        for ph in PH:
            idx = st[:, None] + (sh - 0.5) * T + ph + (np.arange(NB) + 0.5) * T
            idx = np.clip(idx.astype(int), 0, 2047)
            v = np.take_along_axis(L, idx, axis=1) - thr
            b = (v > 0).astype(np.uint8)
            ok = (b[:, :24] == SYNC).all(1)
            q = np.where(ok, np.abs(v[:, :48]).min(1) + np.abs(v).mean(1), -1)
            upd = q > best
            best[upd] = q[upd]
            bits_best[upd] = b[upd]
    good = sig & (best > 0)
    by = np.packbits(bits_best[:, 24:], axis=1, bitorder='little')   # 33 bytes
    for k in np.nonzero(good)[0]:
        fr, r = divmod(int(k), len(rows))
        row = rows[r]
        line = first + row if row < 16 else first + 313 + (row - 16)
        fo.write(by[k].tobytes())
        po.write(np.array([c0 + fr, line], np.uint32).tobytes())
        cnt += 1
    if (c0 // CH) % 10 == 0:
        print(c0, NF, cnt, flush=True)
print('packets', cnt)
