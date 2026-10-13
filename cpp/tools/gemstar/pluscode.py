# VCR Plus+ / Gemstar PlusCode decoder, a port of FREQCCD_.EXE 28ec:0fcf (07b0 digit mixing,
# 000c bit unpacking). Tables are the EXE's (DS:3d82, 3f02). Checked against pluscode_emu.py.
U = [37, 32, 39, 33, 31, 35, 36, 29, 38, 82, 28, 41, 34, 176, 40, 30, 184, 186, 88, 180, 86, 92, 90, 172, 78, 188, 23, 22, 46, 80, 138, 42, 25, 27, 19, 74, 43, 72, 164, 84, 44, 24, 16, 17, 178, 18, 45, 21, 182, 15, 94, 68, 14, 26, 158, 70, 76, 20, 160, 47, 170, 168, 162, 13, 132, 12, 11, 0, 191, 140, 122, 66, 129, 128, 125, 136, 133, 60, 120, 1, 147, 48, 130, 144, 64, 62, 187, 10, 127, 167, 166, 113, 141, 114, 139, 177, 95, 146, 124, 3, 151, 126, 174, 190, 134, 112, 9, 6, 171, 116, 110, 2, 143, 7, 4, 189, 8, 156, 152, 5, 109, 49, 93, 50, 145, 142, 81, 65, 96, 183, 163, 137, 58, 83, 148, 135, 115, 179, 157, 85, 77, 119, 97, 161, 117, 173, 98, 159, 185, 102, 150, 123, 121, 101, 91, 71, 181, 61, 59, 52, 175, 63, 108, 131, 56, 111, 105, 57, 99, 169, 51, 149, 87, 54, 165, 100, 55, 154, 67, 53, 89, 104, 79, 153, 75, 73, 103, 69, 106, 155, 107, 118]
W = [25, 13, 34, 14, 52, 25, 25, 43, 45, 14, 43, 51, 21, 12, 23, 43, 51, 32, 35, 52, 34, 25, 34, 52, 13, 15, 41, 35, 52, 51, 12]
P = [0] + [10 ** i for i in range(9)]       # P[1] = 1 .. P[9] = 10^8

def cdiv(a, b):  # Borland long division truncates toward zero
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q
def cmod(a, b): return a - cdiv(a, b) * b

def mix(year, month, code):
    """28ec:07b0 -> packed value Z."""
    N = 0
    for i in range(7, -1, -1):
        if code >= P[i + 1]: N = i + 1; break
    d = [0] * 22
    rest = code
    for i in range(N, 0, -1):
        rest -= d[i + 1] * P[i + 1]
        d[i] = cdiv(rest, P[i])
    if N != 1:
        while True:
            for _ in range(3):
                for i in range(2, N + 1):
                    d[i] = cmod(d[i] + d[i - 1], 10)
            if d[N] != 0: break
    X = d[3] * 100 + d[2] * 10 + d[1] - 1
    A = cmod(X, 32)
    B = cdiv(X, 32) + 1 if X >= 0 else cdiv(-X, 32)
    N -= 3
    skipA = False
    if N >= 1:
        for i in range(1, N + 1): d[i] = d[i + 3]
        for i in range(1, N + 1): A += d[i]
        A = cmod(A, 32)
        d[N + 1] = 0
        if N >= 4:
            S = 5 - N
            T = year * 12 + month
            R = cmod(T, 31)
            while True:
                for i in range(1, 32):
                    k = R - i
                    if k < 0: k += 31
                    v = W[k]
                    a = cmod(v, 10) - S
                    b = cdiv(v, 10) - S
                    if a <= 0:
                        a = 2 if b >= 3 else 4
                    elif b <= 0:
                        b = 2 if a >= 3 else 4
                    y = d[b] * 10 + d[a]
                    while True:
                        z = cmod(U[y] - T, 192)
                        if z < 0: z += 192
                        if z > 99: y = z; continue
                        break
                    d[b] = cdiv(z, 10); d[a] = cmod(z, 10)
                if d[N] != 0: break
            skipA = True
        else:
            C = cmod(B, 10)
            while True:
                for _ in range(cmod(year, 16) + 1):
                    d[N] = cmod(d[N] + C, 10)
                    for j in range(N - 1, 0, -1):
                        d[j] = cmod(d[j] + d[j + 1], 10)
                    A += d[1]
                if d[N] != 0: break
    if not skipA:
        A = cmod(A + B * (month + 1), 32)
    Z = 0
    for i in range(1, N + 1): Z += d[i] * P[i]
    Z = Z * 1000 + ((B - 1) << 5) + A + 1
    return Z

def unpack(Z):
    """28ec:000c -> channel, day, hour, minute, duration minutes."""
    Z -= 1
    H = cdiv(Z, 1000)
    L = Z - H * 1000
    day = cdiv(L, 32) + 1
    e = [0] * 30
    m = cmod(L, 32)
    for i in range(1, 6): e[i] = (m & (1 << (i - 1))) >> (i - 1)
    e[28] = 0
    for i in range(27, 10, -1):
        H -= e[i + 1] * (1 << (i - 10))
        e[i] = cdiv(H, 1 << (i - 11))
    h = {1: e[1], 2: e[3], 3: e[5], 4: e[11], 5: e[14], 6: e[15], 7: e[16], 8: e[18], 9: e[20], 10: e[21],
         11: e[22], 12: e[24], 13: e[25], 14: e[26]}
    g = {1: e[2], 2: e[4], 3: e[12], 4: e[13], 5: e[17], 6: e[19], 7: e[23], 8: e[27]}
    if e[27] != 0:
        h[12], h[13], h[14], g[7] = e[23], e[24], e[25], e[26]
    channel = sum(g[i] << (i - 1) for i in range(1, 9)) + 1
    T = sum(h[i] << (i - 1) for i in range(1, 15))
    Lx = Sx = 0
    if T >= 0x300:
        if T <= 0xe3f:
            r = cmod(T - 768, 10) + 1
            if r < 6: Lx = r * 5
            else: Sx = (r - 5) * 5
            T = cdiv(T - 768, 10)
        elif T <= 0x197f:
            r = cmod(T - 3648, 6) + 1
            if r == 1: Lx = 15
            else: Sx = (r - 1) * 5
            T = cdiv(T - 3648, 6) + 288
        elif T <= 0x359f:
            q = cmod(T - 6528, 25)
            Lx = (cmod(q, 5) + 1) * 5
            Sx = (cdiv(q, 5) + 1) * 5
            T = cdiv(T - 6528, 25)
        else:
            Sx = (cmod(T - 13728, 5) + 1) * 5
            Lx = 15
            T = cdiv(T - 13728, 5) + 288
    if T < 192:
        tA = cmod(U[T], 48); tB = cdiv(U[T], 48)
    else:
        tA = 47 - cmod(T - 192, 48); tB = cdiv(T, 48)
    hour = cdiv(tA, 2)
    minute = cmod(tA, 2) * 30 + Sx
    dur = (tB + 1) * 30 - Lx
    return channel, day, hour, minute, dur

def decode(year, month, code):
    """-> channel, yymmdd, hhmm, length (minutes): what 28ec:0fcf writes."""
    ch, day, hour, minute, dur = unpack(mix(year % 100, month, code))
    return ch, (year % 100) * 10000 + month * 100 + day, hour * 100 + minute, dur
