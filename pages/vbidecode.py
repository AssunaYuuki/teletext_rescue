# Декодер телетекста из сырого VBI (capture.vbi: 2048 отсчётов x 32 строки на кадр).
#
# Сигнал прошёл через VHS: несущая run-in (3,47 МГц) почти срезана, каждый бит
# размазан на соседние. Поэтому биты восстанавливаются алгоритмом Витерби
# (MLSE): для каждого узора из 2W+1 соседних бит хранится средняя форма
# сигнала в окне бита (отдельно по дробной фазе отсчёта и по зоне строки —
# края строки искажены иначе, чем середина). Шаблоны обучаются на строках,
# привязанных к stream.t42 (train_decoder.py -> zoned_tpl.npy).
import os, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
SPB = float(os.environ.get('VBI_SPB', '5.110'))   # отсчётов на бит (номинал 5.1126; по данным 5.110)
PRE = np.array([0x55, 0x55, 0x27], np.uint8)     # run-in + framing code
NB = 42 * 8                                      # бит данных в пакете
W = int(os.environ.get('VBI_W', '4'))            # бит влияния с каждой стороны
NS = 1 << (2 * W + 1)                            # состояний Витерби
NPH = 8                                          # квантование дробной фазы
NX = 6                                           # отсчётов в окне бита (5 или 6)
ZONES = [0, 8, 16, 32, 48, 64, 300, 320, 336]    # зоны строки по номеру бита данных
NZ = len(ZONES) - 1
h = np.load(os.path.join(HERE, 'channel_h.npy'))  # линейная модель канала (для поиска смещения)

def bits_of(by):                                 # LSB first
    return np.unpackbits(np.asarray(by, np.uint8)[:, None], axis=1)[:, ::-1].ravel()
def bytes_of(bits):
    return np.packbits(np.asarray(bits, np.uint8).reshape(-1, 8)[:, ::-1], axis=1).ravel()
PREB = bits_of(PRE)
NTOT = 24 + NB
ALLB = np.r_[PREB, np.zeros(NB, np.uint8)]
STB = ((np.arange(NS)[:, None] >> np.arange(2 * W, -1, -1)[None, :]) & 1) * 2.0 - 1   # старший бит = j-W

def norm(line):
    y = line.astype(float); y = y - y.mean(); s = y.std(); return y / (s if s > 1e-6 else 1)

def stepwave(bits, off, n=2048):
    x = np.arange(n); k = np.floor((x - off) / SPB).astype(int)
    w = np.zeros(n); ok = (k >= 0) & (k < len(bits)); w[ok] = bits[k[ok]] * 2.0 - 1; return w

def best_offset(y, bits, lo, hi, step):
    """Смещение начала пакета по корреляции с линейной моделью канала."""
    best = (-1e18, lo)
    for off in np.arange(lo, hi, step):
        t = np.convolve(stepwave(bits, off), h, 'same'); c = (y * t).sum() / np.sqrt((t * t).sum())
        if c > best[0]: best = (c, off)
    return best[1]

def windows(off, j):
    x0 = off + j * SPB; xs = int(np.ceil(x0)); ph = int((xs - x0) * NPH) % NPH
    return xs, int(np.ceil(off + (j + 1) * SPB)), ph

def zone_of(j):
    i = j - 24
    for z in range(NZ):
        if i < ZONES[z + 1]: return z
    return NZ - 1

def pattern(pad, j):
    s = 0
    for d in range(-W, W + 1): s = (s << 1) | int(pad[j + d])
    return s

def train_zoned(samples, k=5.0):
    """samples: [(нормированная строка, смещение, биты данных)] -> шаблоны (NZ,NPH,NS,NX)."""
    S = np.zeros((NZ, NPH, NS, NX)); C = np.zeros_like(S)
    for y, off, bits in samples:
        pad = np.r_[PREB, bits, np.zeros(W, np.uint8)]
        for j in range(W, NTOT):
            xs, xe, ph = windows(off, j); n = xe - xs; z = zone_of(j); p = pattern(pad, j)
            S[z, ph, p, :n] += y[xs:xe]; C[z, ph, p, :n] += 1
    G = S.sum(0) / np.maximum(C.sum(0), 1)       # общие шаблоны — для редких узоров в зоне
    return (S + k * G[None]) / (C + k)

_ZT = None
def load_zoned(path=None):
    global _ZT; _ZT = np.load(path or os.path.join(HERE, 'zoned_tpl.npy'))
def set_zoned(T):
    global _ZT; _ZT = T

def viterbi(y, off):
    """MLSE по шаблонам: -> (биты данных, метрика)."""
    T = _ZT; metric = None; back = []
    prev0 = np.arange(NS) >> 1; prev1 = prev0 | (1 << (2 * W))
    for j in range(W, NTOT):
        xs, xe, ph = windows(off, j); n = xe - xs
        bm = np.zeros(NS) if (xe > len(y) or xs < 0) else ((y[xs:xe][None, :] - T[zone_of(j), ph, :, :n]) ** 2).sum(1)
        for d in range(-W, W + 1):              # известная преамбула и пустота после данных
            k = j + d
            if k < 24 or k >= NTOT:
                want = (ALLB[k] if k < 24 else 0) * 2.0 - 1
                bm = np.where(STB[:, d + W] == want, bm, 1e9)
        if metric is None: metric = bm; continue
        m0 = metric[prev0]; m1 = metric[prev1]; choose = m1 < m0; back.append(choose)
        metric = np.where(choose, m1, m0) + bm
    s = int(np.argmin(metric)); best = float(metric[s]); out = np.zeros(NTOT + W, np.uint8)
    for j in range(NTOT - 1, W, -1):           # состояние на шаге j: биты j-W..j+W
        out[j + W] = s & 1; ch = back[j - W - 1][s]; s = (s >> 1) | ((1 << (2 * W)) if ch else 0)
    for d in range(2 * W + 1): out[d] = (s >> (2 * W - d)) & 1
    return out[24:NTOT], best

def decode_blind(line, lo=113.0, hi=121.0):
    """Декодирование строки без эталона: смещение перебирается по метрике модели.
    -> (42 байта, смещение, метрика на отсчёт)."""
    y = norm(line); best = None
    for off in np.arange(lo, hi + 0.01, 0.5):
        bits, met = viterbi(y, off)
        if best is None or met < best[0]: best = (met, off, bits)
    met, off, bits = best
    for o in (off - 0.25, off + 0.25):
        b, m = viterbi(y, o)
        if m < met: met, off, bits = m, o, b
    return bytes_of(bits), off, met / (NTOT * SPB)

_T2 = None
def _t2():
    """ΣT² по первым 5 и 6 отсчётам окна: (NZ, NPH, NS, 2)."""
    global _T2
    if _T2 is None or _T2[0] is not _ZT:
        c = np.cumsum(_ZT ** 2, axis=3); _T2 = (_ZT, np.stack([c[..., 4], c[..., 5]], -1))
    return _T2[1]

def viterbi_multi(y, offs, j0=W, j1=NTOT, trace=True):
    """Витерби сразу для нескольких смещений. Ошибка окна считается как
    Σy² − 2·y·T + ΣT² матричным умножением по группам с одинаковой фазой.
    j0..j1 — диапазон окон бит (часть строки, без обратного прохода — для выбора смещения).
    -> (биты (K, NB) или None, метрики (K,))."""
    T = _ZT; T2 = _t2(); offs = np.asarray(offs, float); K = len(offs)
    prev0 = np.arange(NS) >> 1; prev1 = prev0 | (1 << (2 * W))
    ny = len(y); yp = np.r_[y, np.zeros(NX + 2)]
    metric = None; back = []; kk = np.arange(K)
    for j in range(j0, j1):
        z = zone_of(j)
        x0 = offs + j * SPB; xs = np.ceil(x0).astype(int); ph = ((xs - x0) * NPH).astype(int) % NPH
        n = np.ceil(offs + (j + 1) * SPB).astype(int) - xs                               # 5 или 6 отсчётов
        idx = xs[:, None] + np.arange(NX)[None, :]
        Y = yp[np.clip(idx, 0, ny + NX)] * ((np.arange(NX)[None, :] < n[:, None]) & (idx < ny))
        bm = np.empty((K, NS))
        for p_ in np.unique(ph):
            g = ph == p_
            bm[g] = (Y[g] ** 2).sum(1)[:, None] - 2 * (Y[g] @ T[z, p_].T) + T2[z, p_][:, n[g] - 5].T
        for d in range(-W, W + 1):
            k = j + d
            if k < 24 or k >= NTOT:
                want = (ALLB[k] if k < 24 else 0) * 2.0 - 1
                bm = np.where((STB[:, d + W] == want)[None, :], bm, 1e9)
        if metric is None: metric = bm; continue
        m0 = metric[:, prev0]; m1 = metric[:, prev1]; choose = m1 < m0
        if trace: back.append(choose)
        metric = np.where(choose, m1, m0) + bm
    s = metric.argmin(1); best = metric[kk, s]
    if not trace: return None, best
    out = np.zeros((K, NTOT + W), np.uint8)
    for j in range(NTOT - 1, W, -1):
        out[:, j + W] = s & 1; ch = back[j - W - 1][kk, s]; s = (s >> 1) | np.where(ch, 1 << (2 * W), 0)
    for d in range(2 * W + 1): out[:, d] = (s >> (2 * W - d)) & 1
    return out[:, 24:NTOT], best

PREFIX = 160
def decode_fast(line, lo=113.0, hi=121.0):
    """Декодирование строки без эталона: смещение выбирается по началу строки
    (преамбула + PREFIX бит: известная преамбула привязывает позицию; в середине
    строки сдвиг на целый бит неотличим) среди lo..hi с шагом 0,5, затем полный Витерби для
    лучшего смещения и соседних ±0,25. -> (42 байта, смещение, метрика на отсчёт)."""
    y = norm(line)
    offs = np.arange(lo, hi + 0.01, 0.5)
    _, met = viterbi_multi(y, offs, W, 24 + PREFIX, trace=False)
    o = offs[int(met.argmin())]; cand = np.array([o - 0.25, o, o + 0.25])
    bits, m = viterbi_multi(y, cand); k = int(m.argmin())
    return bytes_of(bits[k]), float(cand[k]), float(m[k]) / (NTOT * SPB)

# ---------- починка байтов с ошибкой чётности (decode-orc repair_damaged_bytes:
# «перевернуть бит, который детектор был ближе всего прочитать иначе»). Уверенность
# бита — на сколько вырастет ошибка шаблонов в окнах, которые этот бит задевает
# (2W+1 окон), если его перевернуть; переворачивается бит с наименьшим ростом.
_HAM = None
def _ham():
    global _HAM
    if _HAM is None:
        _HAM = {}
        for dd in range(16):
            b = [(dd >> i) & 1 for i in range(4)]
            p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
            p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
            _HAM[sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))] = dd
    return _HAM

def _hdec(x):
    """Хэмминг 8/4 с исправлением одиночной ошибки -> 4 бита или None."""
    H = _ham()
    if x in H: return H[x]
    c = [w for w in H if bin(w ^ x).count('1') == 1]
    return H[c[0]] if len(c) == 1 else None

def display_bytes(p):
    """Номера байтов пакета с битом чётности (символы страницы): ряд 0 — 10..41,
    ряды 1–25 — 2..41; X/26–31 и пакеты с нечитаемым адресом — ничего."""
    H = _ham(); a = [_hdec(int(p[0])), _hdec(int(p[1]))]
    if None in a: return range(0)
    row = (a[0] >> 3) | (a[1] << 1)
    return range(10, 42) if row == 0 else range(2, 42) if row <= 25 else range(0)

def repair(line, off, data):
    """42 байта, прочитанные по строке line со смещением off -> (починенные байты, сколько починено)."""
    d = np.frombuffer(bytes(data), np.uint8).copy()
    bad = [k for k in display_bytes(d) if bin(int(d[k])).count('1') % 2 == 0]
    if not bad: return d, 0
    y = norm(np.asarray(line)); ny = len(y)
    pad = np.r_[PREB, bits_of(d), np.zeros(W + 1, np.uint8)].astype(np.int64)
    J = np.arange(W, NTOT)
    x0 = off + J * SPB; xs = np.ceil(x0).astype(int); ph = ((xs - x0) * NPH).astype(int) % NPH
    n = np.ceil(off + (J + 1) * SPB).astype(int) - xs
    idx = xs[:, None] + np.arange(NX)[None, :]
    mask = (np.arange(NX)[None, :] < n[:, None]) & (idx < ny) & (idx >= 0)
    Y = np.where(mask, np.r_[y, np.zeros(NX + 2)][np.clip(idx, 0, ny)], 0.0)
    Z = np.array([zone_of(j) for j in J])
    P = np.zeros(len(J), np.int64)
    for dd in range(-W, W + 1): P = (P << 1) | pad[J + dd]
    def err(jj, pp):                                  # ошибка окон jj (индексы в J) при узорах pp
        Tw = _ZT[Z[jj], ph[jj], pp]
        return (((Y[jj] - Tw) ** 2) * mask[jj]).sum(-1)
    fixed = 0
    for k in bad:
        cand = 24 + 8 * k + np.arange(8)              # номера бит пакета (с преамбулой)
        jj = cand[:, None] + np.arange(-W, W + 1)[None, :] - W   # индексы окон в J
        ok = (jj >= 0) & (jj < len(J)); jj = np.clip(jj, 0, len(J) - 1)
        shift = W - (cand[:, None] - (jj + W))
        pf = P[jj] ^ (1 << shift)
        delta = ((err(jj, pf) - err(jj, P[jj])) * ok).sum(1)
        b = int(np.argmin(delta))
        d[k] ^= 1 << b; fixed += 1
        i = cand[b]; pad[i] ^= 1                      # узоры соседних окон — с новым битом
        P = np.zeros(len(J), np.int64)
        for dd in range(-W, W + 1): P = (P << 1) | pad[J + dd]
    return d, fixed
