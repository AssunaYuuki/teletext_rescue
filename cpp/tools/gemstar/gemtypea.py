# Gemstar Type A packets (07 7C/7A .. chk 0F lrc) from gemstar_words.csv, descrambled as
# FREQCCD_.EXE (Gemstar VBI Data Monitor 1.5) does it:
#   FUN_1a3b_0000  sum(07..chk) = 0 mod 128, last byte = XOR(07..chk) ^ 8F (bytes with parity)
#   FUN_1bac_0168  descramble payload buf[13..len-3) with key file <month*100+year%100>.INP and
#                  the "INDEX SYSTEM,INC" substitution table; checksum buf[11] + sum(payload) = 0
# usage: gemtypea.py gemstar_words.csv EXE INPdir
import sys, csv, glob, os, struct, collections

def load_exe_table(exe):
    d = open(exe, 'rb').read()
    hdr = struct.unpack_from('<H', d, 8)[0] * 16
    ds = hdr + (0x33cf - 0x1000) * 16
    return d[ds + 0x111e: ds + 0x111e + 0x100]

def load_keys(dirn):
    keys = {}
    for f in glob.glob(os.path.join(dirn, '*.INP')):
        v = int(os.path.basename(f)[:-4])
        keys[(v // 100, v % 100)] = [int(x) for x in open(f).read().split()]
    return keys

def descramble(buf, key, T):
    """buf = 7-bit packet bytes from 07 to the LRC; returns (payload, checksum ok)."""
    n = len(buf) - 16
    if buf[len(buf) - 4] == 0: n -= 1
    p = list(buf[13:13 + n])
    if n > 9:
        r = buf[6] ^ buf[7] ^ buf[8]
        prev = 0
        b = (p[0] - 0x10) & 0xff
        for k in key:
            s = abs(k)
            if s >= n: continue
            pos = (s + prev) % n
            a = (p[pos] + b) & 0xff
            if a & 0x80: a = (a + 0x90) & 0xff
            if r & 1: a = (a + 1) & 0xff
            r = (r >> 1) | ((r & 1) << 7)
            p[pos] = T[a]
            if k < 0: p[prev], p[pos] = p[pos], p[prev]
            b = (p[pos] - 0x10) & 0xff
            prev = pos
    ok = (sum(p) + buf[11]) & 0x7f == 0
    return p, ok

def packets(rows):
    """rows of (frame, bytes) for one line/field -> list of (frame, raw packet bytes)."""
    out = []
    cur = None
    for fr, bs in rows:
        for b in bs:
            if b & 0x7f == 0x07 and cur is None or b == 0x07:
                cur = (fr, [b]); continue
            if cur is None: continue
            cur[1].append(b)
            pk = cur[1]
            if len(pk) >= 2 and pk[-2] & 0x7f == 0x0f and len(pk) >= 5:
                out.append((cur[0], pk)); cur = None
            elif len(pk) > 200: cur = None
    return out

def check(pk):
    s = sum(b & 0x7f for b in pk[:-2]) & 0x7f
    x = 0
    for b in pk[:-2]: x ^= b
    x ^= 0x8f ^ pk[-1]
    return s == 0, x == 0

def main():
    words, exe, inpdir = sys.argv[1:4]
    T = load_exe_table(exe)
    keys = load_keys(inpdir)
    streams = collections.defaultdict(list)
    for r in csv.DictReader(open(words)):
        if r['framing'] != '2x': continue
        streams[(r['line'], r['field'])].append((int(r['frame']), bytes.fromhex(r['bytes'])))
    for st, rows in streams.items():
        seen = collections.Counter()
        for fr, pk in packets(rows):
            sok, lok = check(pk)
            b7 = [b & 0x7f for b in pk]
            if not sok or b7[1] not in (0x7a, 0x7c) or len(pk) < 17: continue
            k = bytes(b7)
            seen[k] += 1
            if seen[k] > 1: continue
            hdr = ''.join(chr(c) for c in b7[2:13])
            res = []
            for (m, y), key in sorted(keys.items()):
                if (b7[6] ^ m) & 1: continue
                p, ok = descramble(b7, key, T)
                if ok: res.append('%02d/%02d:%s' % (m, y, ''.join(chr(c) if 32 <= c < 127 else '.' for c in p)))
            print(st, fr, 'lrc' if lok else 'LRC!', hex(b7[1]), repr(hdr), 'sub', b7[5] & 7, len(pk), res[:3] if res else '-')

main()
