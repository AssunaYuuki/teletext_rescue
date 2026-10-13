"""TV-Inform (Norpak NABTS) product code: rows = 2 check bytes (packet bytes 26,27) + 26 data bytes,
columns = 2 FEC packets (CI 14, 15) + 14 data packets (CI 0..13); both RS over GF(256) (poly 0x11D)
with checks s0 = sum v[k]*b^k = 0, s1 = sum v[k]*b^(3k) = 0, b = 0x1D, check symbols at k = 0, 1.
usage: norpak_fec.py file.t33 out.bin  -> data stream per bundle (364 bytes), report to stdout"""
import sys
import numpy as np

EXP = [0] * 510
LOG = [0] * 256
x = 1
for i in range(255):
    EXP[i] = x
    LOG[x] = i
    x <<= 1
    if x & 0x100:
        x ^= 0x11D
for i in range(255, 510):
    EXP[i] = EXP[i - 255]
B = 8                                   # log of 0x1D


def mul(a, b):
    return EXP[LOG[a] + LOG[b]] if a and b else 0


def div(a, b):
    return EXP[(LOG[a] - LOG[b]) % 255] if a else 0


def pw(e):
    return EXP[e % 255]


def synd(v):
    s0 = s1 = 0
    for k, a in enumerate(v):
        if a:
            s0 ^= EXP[(LOG[a] + B * k) % 255]
            s1 ^= EXP[(LOG[a] + 3 * B * k) % 255]
    return s0, s1


def fix1(v):
    """correct one error in place; True if the word is (now) a codeword"""
    s0, s1 = synd(v)
    if not s0 and not s1:
        return True
    if not s0 or not s1:
        return False
    # error e at k: s0 = e*b^k, s1 = e*b^3k -> b^2k = s1/s0
    l2 = (LOG[s1] - LOG[s0]) % 255            # = 2*B*k mod 255
    for k in range(len(v)):
        if (2 * B * k) % 255 == l2:
            e = div(s0, pw(B * k))
            v[k] ^= e
            return synd(v) == (0, 0)
    return False


def erase2(v, j, k):
    v[j] = v[k] = 0
    s0, s1 = synd(v)
    a, b, c, d = pw(B * j), pw(B * k), pw(3 * B * j), pw(3 * B * k)
    det = mul(a, d) ^ mul(b, c)
    if not det:
        return False
    v[j] = div(mul(s0, d) ^ mul(b, s1), det)
    v[k] = div(mul(a, s1) ^ mul(c, s0), det)
    return True


def erase1(v, j):
    v[j] = 0
    s0, s1 = synd(v)
    e = div(s0, pw(B * j))
    if mul(e, pw(3 * B * j)) != s1:
        return False
    v[j] = e
    return True


H84 = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]
HAM = {}
for v_, c_ in enumerate(H84):
    HAM[c_] = v_
    for b_ in range(8):
        HAM[c_ ^ (1 << b_)] = v_


def decode_bundle(rows):
    """rows: 16 lists of 28 bytes or None (missing), index = CI. Returns 14x26 data or None."""
    M = [list(r) if r is not None else None for r in rows]
    bad = set(i for i in range(16) if M[i] is None)
    for i in range(16):
        if M[i] is not None:
            w = M[i][26:] + M[i][:26]
            if fix1(w):
                M[i] = w[2:] + w[:2]
            else:
                bad.add(i)
    if len(bad) > 2:
        return None, len(bad)
    for i in bad:
        M[i] = [0] * 28
    order = [14, 15] + list(range(14))       # code position -> CI
    pos = {ci: k for k, ci in enumerate(order)}
    eb = sorted(pos[i] for i in bad)
    for c in range(28):
        v = [M[ci][c] for ci in order]
        if len(eb) == 2:
            ok = erase2(v, eb[0], eb[1])
        elif len(eb) == 1:
            ok = erase1(v, eb[0])
        else:
            ok = fix1(v)
        if not ok:
            return None, len(bad)
        for k, ci in enumerate(order):
            M[ci][c] = v[k]
    return [M[ci][:26] for ci in range(14)], len(bad)


def main():
    f = sys.argv[1]
    raw = np.fromfile(f, np.uint8)
    pk = raw[: len(raw) // 33 * 33].reshape(-1, 33)
    pos = np.fromfile(f + '.pos', np.uint32).reshape(-1, 2)[: len(pk)]
    out = open(sys.argv[2], 'wb') if len(sys.argv) > 2 else None
    cur = {}
    last = {}
    stats = {'ok': 0, 'fail': 0}
    for p, (fr, ln) in zip(pk, pos):
        h = [HAM.get(int(b), -1) for b in p[:5]]
        if min(h[:3]) < 0 or h[3] < 0:
            continue
        a = h[0] * 256 + h[1] * 16 + h[2]
        ci = h[3]
        rows = cur.setdefault(a, [None] * 16)
        if ci < last.get(a, -1) or (ci == last.get(a, -1)):
            # new bundle started: decode the old one
            if sum(r is not None for r in rows) >= 14:
                d, nb = decode_bundle(rows)
                if d is None:
                    stats['fail'] += 1
                else:
                    stats['ok'] += 1
                    if out:
                        out.write(np.array([a], '<u2').tobytes() + np.array([fr], '<u4').tobytes() + bytes(sum(d, [])))
            rows = cur[a] = [None] * 16
        rows[ci] = list(p[5:33])
        last[a] = ci
    print(stats)


if __name__ == '__main__':
    main()
