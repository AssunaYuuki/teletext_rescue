# TV-Inform (ТВ-информ) VBI slicer, GOST R 50822-95 physical layer.
#
# Slices the digital-data lines of a ТВ-информ recording into raw bytes.
# Only the physical layer is standardised (GOST R 50822-95); the packet
# format above it (subscriber address, rubric, error protection) was
# proprietary to AO "ТВ-информ" and is NOT decoded here -- the payload comes
# out as raw bytes for analysis.
#
# Clocks: fТ1 1.734375 MHz (111*fstr, 90 bits/line), fТ2 3.46875 MHz
# (222*fstr, 180 bits), fТ3 6.9375 MHz (444*fstr, 360 bits). NRZ, first data
# bit 10.6 us after the sync edge, a 2-bit pilot pulse leads the sync lines.
#
# Usage: python -I tvinform.py <capture.vbi> [--rate 1|2|3] [--fs HZ]
#        [--line-bytes N] [--lines-per-frame N] [--out out.bin]
# Defaults match the bttv capture ort99-tvi.vbi: fs=28.636 MHz, 2048-byte
# lines, 32 lines/frame, data on line indices 0,1,16,17, fТ2 (180 bits).
import sys, struct

FSTR = 15625.0  # SECAM/PAL line frequency
RATES = {1: 111 * FSTR, 2: 222 * FSTR, 3: 444 * FSTR}
BITS = {1: 90, 2: 180, 3: 360}


def pctile(a, q):
    s = sorted(a)
    return s[min(len(s) - 1, int(q * len(s)))]


def slice_line(line, fs, rate_hz, nbits):
    T = fs / rate_hz
    n = len(line)
    lo, hi = pctile(line, 0.08), pctile(line, 0.92)
    if hi - lo < 30:
        return None
    thr = (lo + hi) / 2.0
    # first sustained rising edge (data start after the sync edge)
    st = None
    search_hi = min(n - int(T) - 1, int(fs * 20e-6))
    for i in range(int(fs * 7e-6), search_hi):
        if line[i] > thr and line[i - 1] <= thr:
            if sum(line[i:i + int(T)]) / int(T) > thr:
                st = i
                break
    if st is None:
        return None
    # clock recovery: fit st + k*T to the zero crossings (least squares)
    xs, ks = [], []
    end = min(n - 1, st + int(nbits * T) + int(T))
    for i in range(st, end):
        if (line[i] - thr) * (line[i + 1] - thr) < 0:
            frac = (thr - line[i]) / (line[i + 1] - line[i])
            e = i + frac
            k = round((e - st) / T)
            if abs((e - st) / T - k) < 0.35:
                xs.append(e)
                ks.append(k)
    if len(xs) >= 20:
        m = len(xs)
        sk = sum(ks); sx = sum(xs)
        skk = sum(k * k for k in ks); skx = sum(k * x for k, x in zip(ks, xs))
        det = m * skk - sk * sk
        if det:
            T2 = (m * skx - sk * sx) / det
            st2 = (sx - T2 * sk) / m
            if 0.9 * T < T2 < 1.1 * T:
                T, st = T2, st2
    bits = []
    for k in range(nbits):
        c = st + (k + 0.5) * T
        if c >= n:
            return None
        bits.append(1 if line[int(round(c))] > thr else 0)
    return bits


def pack(bits, msb=False):
    out = bytearray()
    for i in range(0, len(bits) - 7, 8):
        v = 0
        for j in range(8):
            v |= bits[i + j] << (7 - j if msb else j)
        out.append(v)
    return bytes(out)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return
    path = a[0]
    rate = 2; fs = 28.636e6; lb = 2048; lpf = 32; msb = False
    data_lines = [0, 1, 16, 17]; out = None
    i = 1
    while i < len(a):
        if a[i] == '--rate': rate = int(a[i + 1]); i += 2
        elif a[i] == '--fs': fs = float(a[i + 1]); i += 2
        elif a[i] == '--line-bytes': lb = int(a[i + 1]); i += 2
        elif a[i] == '--lines-per-frame': lpf = int(a[i + 1]); i += 2
        elif a[i] == '--lines': data_lines = [int(x) for x in a[i + 1].split(',')]; i += 2
        elif a[i] == '--msb': msb = True; i += 1
        elif a[i] == '--out': out = a[i + 1]; i += 2
        else: i += 1
    pages = '--pages' in a
    rate_hz, nbits = RATES[rate], BITS[rate]
    with open(path, 'rb') as fp:
        raw = fp.read()
    frame = lb * lpf
    nfr = len(raw) // frame
    print('TV-Inform slicer (GOST R 50822-95): %s' % path)
    print('  %d frames of %d lines x %d bytes; clock fT%d=%.4f MHz, %d bits/line, %.2f samples/bit'
          % (nfr, lpf, lb, rate, rate_hz / 1e6, nbits, fs / rate_hz))
    print('  data line indices: %s' % data_lines)
    packets = []
    ok = 0
    for f in range(nfr):
        base = f * frame
        for li in data_lines:
            o = base + li * lb
            line = raw[o:o + lb]
            if len(line) < lb:
                continue
            bits = slice_line(line, fs, rate_hz, nbits)
            if bits is None:
                continue
            ok += 1
            packets.append((f, li, pack(bits, msb)))
    print('  sliced %d / %d candidate lines' % (ok, nfr * len(data_lines)))
    if out:
        with open(out, 'wb') as w:
            for f, li, by in packets:
                w.write(struct.pack('<IH', f, li) + bytes([len(by)]) + by)
        print('  wrote %d packets to %s (record: u32 frame, u16 line, u8 len, bytes)' % (len(packets), out))
    else:
        for f, li, by in packets[:20]:
            print('  f%-4d L%-2d %s' % (f, li, by.hex()))
    if pages:
        dump_pages(packets, data_lines)
    print('  NOTE: payload is the proprietary AO "TV-Inform" packet format; only')
    print('        the GOST physical layer is public, so bytes are raw (undecoded).')


def _ham(a, b):
    return sum(bin(x ^ y).count('1') for x, y in zip(a, b))


def dump_pages(packets, data_lines, thresh=16, min_copies=5, top=10):
    # A noisy recording repeats each carousel page many times; clustering the
    # packets of a line by Hamming distance and voting each cluster bit by bit
    # recovers the clean page. Reveals the recurring sync words and fields.
    for li in data_lines:
        v = [by for f, l, by in packets if l == li]
        if not v:
            continue
        n = len(v[0])
        reps = []  # [centroid, [members]]
        for b in v:
            for r in reps:
                if _ham(b, r[0]) <= thresh:
                    r[1].append(b)
                    break
            else:
                reps.append([b, [b]])
        reps = [r for r in reps if len(r[1]) >= min_copies]
        reps.sort(key=lambda r: -len(r[1]))
        print('  line %2d: %d stable pages (>= %d copies) of %d packets'
              % (li, len(reps), min_copies, len(v)))
        for r in reps[:top]:
            g = r[1]
            voted = bytes(
                sum((1 if sum((b[j] >> k) & 1 for b in g) * 2 > len(g) else 0) << k
                    for k in range(8))
                for j in range(n))
            txt = ''.join(chr(x) if 32 <= x < 127 else '.' for x in voted)
            print('    %5dx %s |%s' % (len(g), voted.hex(), txt))


if __name__ == '__main__':
    main()
