# Gemstar Type B blocks (2x lines, 4 bytes per line, block = 05 FC ..) decrypted as
# FREQCCD_.EXE does it (segment 23cd, checked against the disassembly):
#   block: 05 FC b2 b3 idx tag flags chk | data (64 bytes if b3&1 else 32) | 8 more bytes
#   key   = 23cd:0072(year-1980, month, {idx, tag}); month+1 if (flags^month)&1 (22fd:07ab)
#   decrypt = 4 x (23cd:01ea key mix, 23cd:0157 Feistel on 32+32 bytes)   (23cd:0561)
#   check: sum(64 data bytes) ^ chain == chk; chain[idx+1] = chain[idx] ^ chk  (22fd:07ab)
# usage: gemtypeb.py gemstar_words.csv EXE year month
import sys, csv, struct, collections

def tables(exe):
    d = open(exe, 'rb').read()
    ds = struct.unpack_from('<H', d, 8)[0] * 16 + (0x33cf - 0x1000) * 16
    return d[ds + 0x111e: ds + 0x111e + 0x80], d[ds + 0x2882: ds + 0x2982], d[ds + 0x2982: ds + 0x2a82]

T, P0, P1 = None, None, None

def keysched(a, b, idx, tag):
    a &= 0xff; b &= 0xff
    if not tag & 0x80:
        b += 1
        if b == 13: b = 1; a = (a + 1) & 0xff
    l7 = ((tag << 3) & 0xf8) | (idx & 7)
    y = ((idx >> 4) & 0xf) | ((idx << 4) & 0xf0)
    y ^= b
    y = ((y >> 1) | (y << 7)) & 0xff
    l8 = ((y - a) & 0xff) ^ 0x39
    cur = a ^ ((b + l7) & 0xff)
    l1 = a
    out = []
    for i in range(16):
        l1 = (l1 + b) & 0xff
        s = T[l1 & 0x7f]
        out.append(s)
        cur = (cur + s) & 0xff
    P = P1 if tag & 0x80 else P0
    l1 = cur
    for i in range(16):
        c = out[i]
        out[i] = c ^ l1
        l1 = P[(c + l7) & 0xff] ^ l8
    return out

def mix(d, sel, k):
    A, B = P0, P1
    def sw():
        nonlocal A, B
        A, B = B, A
    if sel & 1: sw()
    cl = A[((d[8] ^ d[0x18]) + d[0xa]) & 0xff]; k[0] ^= cl; cl = k[0]
    cl ^= (d[0x17] + d[6]) & 0xff; cl = A[cl] ^ k[1]; k[1] = cl
    cl = (d[0x11] + d[7] + cl) & 0xff
    if sel & 2: sw()
    cl = A[cl] ^ k[2]; k[2] = cl
    cl = (cl - d[0x16] + d[0xd]) & 0xff; cl = A[cl] ^ k[3]; k[3] = cl
    cl = (cl + d[0x13]) & 0xff
    if sel & 4: sw()
    cl = (cl + d[9]) & 0xff; cl = A[cl] ^ k[0]; k[0] = cl
    cl = ((cl ^ d[0x1f]) + d[0xf]) & 0xff; cl = A[cl] ^ k[1]; k[1] = cl
    cl = (cl - d[0x1b]) & 0xff
    if sel & 8: sw()
    cl = (cl + d[1]) & 0xff; cl = A[cl] ^ k[2]; k[2] = cl
    cl = (d[0x15] + d[0xd] + cl) & 0xff; cl = A[cl] ^ k[3]; k[3] = cl
    cl &= d[0x1c]
    if sel & 0x10: sw()
    cl = (cl + d[5]) & 0xff; cl = A[cl] ^ k[0]; k[0] = cl
    cl = ((cl ^ d[0x1e]) + d[0]) & 0xff; cl = A[cl] ^ k[1]; k[1] = cl
    cl = (cl - d[0x10]) & 0xff
    if sel & 0x20: sw()
    cl = (cl + d[4]) & 0xff; cl = A[cl] ^ k[2]; k[2] = cl
    cl = ((cl | d[0x12]) + d[0xc]) & 0xff; cl = A[cl] ^ k[3]; k[3] = cl
    cl = ((cl | d[0x19]) + d[3]) & 0xff
    if sel & 0x40: A = B
    cl = A[cl] ^ k[0]; k[0] = cl
    cl = ((cl ^ d[0x1d]) + d[0xe]) & 0xff; cl = A[cl] ^ k[1]; k[1] = cl
    cl = (cl + d[0x14] + d[2]) & 0xff; cl = A[cl] ^ k[2]; k[2] = cl
    cl = (cl - d[0x1a] + d[0xb]) & 0xff; cl = A[cl] ^ k[3]; k[3] = cl
    cl = (cl + d[0x18]) & 0xff; k[0] ^= A[cl]

def feistel(d, n, k):
    for i in range(n):
        k[0] = (k[0] + k[2]) & 0xff
        dl = P0[(d[n + i] + k[0]) & 0xff]
        tmp = d[i]
        dl = P0[dl ^ k[1]]
        d[n + i] = tmp
        d[i] = dl ^ tmp
        k[1] = (k[1] + k[3]) & 0xff

def decrypt(data, year, month, idx, tag):
    d = list(data)
    key = keysched(year - 1980, month, idx, tag)
    for kk, sel in ((key[0:4], key[13]), (key[4:8], key[11]), (key[8:12], key[6]), (key[12:16], key[4])):
        k = list(kk)
        mix(d, sel, k)
        feistel(d, 0x20, k)
    return d

def blocks(rows):
    out, cur = [], None
    for fr, w in rows:
        if cur is None:
            if w[0] == 0x05 and w[1] == 0xfc: cur = (fr, bytearray(w))
            continue
        cur[1].extend(w)
        need = 0x50 if cur[1][3] & 1 else 0x30
        if len(cur[1]) >= need:
            out.append((cur[0], bytes(cur[1][:need]))); cur = None
    return out

def main():
    global T, P0, P1
    words, exe, year, month = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    T, P0, P1 = tables(exe)
    streams = collections.defaultdict(list)
    for r in csv.DictReader(open(words)):
        if r['framing'] == '2x':
            streams[(r['line'], r['field'])].append((int(r['frame']), bytes.fromhex(r['bytes'])))
    for st, rows in streams.items():
        bl = blocks(rows)
        good = 0
        chain = collections.defaultdict(int)
        for fr, b in bl:
            idx, tag, flags, chk = b[4], b[5], b[6], b[7]
            y, m = year, month
            if (flags >> 7 ^ m) & 1:
                m = m % 12 + 1
                if m == 1: y += 1
            d = decrypt(b[8:72] if len(b) == 0x50 else b[8:40] + bytes(32), y, m, idx, tag)
            s = sum(d) & 0xff
            key = (tag, idx)
            ok = any((s ^ c) == chk for c in range(256) if False) or None
            txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in d)
            print(st, fr, b[:8].hex(), 'sum^chk=%02x' % (s ^ chk), txt)

main()

def messages(words, exe, year, month):
    """-> list of (stream, frame, tag, bytes) complete messages with every block checksum right."""
    global T, P0, P1
    T, P0, P1 = tables(exe)
    streams = collections.defaultdict(list)
    for r in csv.DictReader(open(words)):
        if r['framing'] == '2x':
            streams[(r['line'], r['field'])].append((int(r['frame']), bytes.fromhex(r['bytes'])))
    res = []
    for st, rows in streams.items():
        cur = None
        for fr, b in blocks(rows):
            idx, tag, flags, chk = b[4], b[5], b[6], b[7]
            y, m = year, month
            if (flags >> 7 ^ m) & 1:
                m = m % 12 + 1
                if m == 1: y += 1
            n = 64 if len(b) == 0x50 else 32
            d = decrypt(b[8:8 + n] + bytes(64 - n), y, m, idx, tag)[:n]
            s = sum(d) & 0xff
            if idx == 0:
                if cur is not None and len(cur[3]): res.append((st, cur[0], cur[1], bytes(cur[3]), cur[4], None))
                cur = [fr, tag, 0, bytearray(), b[2:4]]
            if cur is None or cur[1] != tag or len(cur[3]) != idx * 64 or (s ^ cur[2]) != chk:
                if cur is not None and len(cur[3]): res.append((st, cur[0], cur[1], bytes(cur[3]), cur[4], False))
                cur = None; continue
            cur[2] ^= chk
            cur[3] += bytes(d)
            if n < 64 or flags & 0x7f:
                res.append((st, cur[0], cur[1], bytes(cur[3]), cur[4], True)); cur = None
        if cur is not None and len(cur[3]): res.append((st, cur[0], cur[1], bytes(cur[3]), cur[4], None))
    return res
