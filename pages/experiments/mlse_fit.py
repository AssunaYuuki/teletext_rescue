# Декодер без обучения (подход decode-orc): модель канала подгоняется
# заново для каждой строки VBI.
#   * Каждый бит читается в P точках внутри битового интервала; для каждой
#     точки своя линейная модель y = c + Σ h[d]·a(i+d), d = -L..R, a = ±1.
#   * Сначала модель подгоняется по 24 известным битам (run-in + framing),
#     по качеству подгонки выбирается смещение начала пакета.
#   * Витерби (состояние — биты i-L..i+R) читает пакет; затем модель
#     подгоняется заново по всем 360 прочитанным битам и пакет читается ещё раз.
import numpy as np

SPB = 35.46895 / 6.9375                 # отсчётов на бит (bt8x8, 35,47 МГц)
PREB = np.unpackbits(np.array([0x55, 0x55, 0x27], np.uint8)[:, None], axis=1)[:, ::-1].ravel()
NB = 42 * 8; NT = 24 + NB
L, R = 3, 3                              # соседних бит слева и справа в модели
NTAP = L + R + 1; NS = 1 << NTAP
PH = np.array([0.3, 0.5, 0.7])           # точки чтения внутри бита (доля интервала)
STB = ((np.arange(NS)[:, None] >> np.arange(NTAP - 1, -1, -1)[None, :]) & 1) * 2.0 - 1   # старший = i-L

def norm(line):
    y = np.asarray(line, float); y = y - y.mean(); s = y.std(); return y / (s if s > 1e-6 else 1)

def sample(y, off):
    """(NT, P): отсчёты в точках PH каждого бита."""
    pos = off + (np.arange(NT)[:, None] + PH[None, :]) * SPB
    return np.interp(pos, np.arange(len(y)), y, left=0, right=0)

def design(bits_pm, idx):
    """Матрица регрессии для битов idx: соседние биты (±1) и единица."""
    pad = np.r_[np.zeros(L), bits_pm, np.zeros(R + 1)]
    return np.c_[np.stack([pad[idx + d + L] for d in range(-L, R + 1)], 1), np.ones(len(idx))]

def fit(Y, bits01, idx, prior=None, lam=0.0):
    """Коэффициенты (P, NTAP+1) по известным битам на позициях idx и невязка.
    prior, lam — гребневая регрессия к априорной модели (мало уравнений)."""
    X = design(bits01 * 2.0 - 1, idx)
    if prior is not None and lam > 0:
        A = X.T @ X + lam * np.eye(X.shape[1]); B = X.T @ Y[idx] + lam * prior.T
        C = np.linalg.solve(A, B)
    else:
        C, *_ = np.linalg.lstsq(X, Y[idx], rcond=None)
    r = Y[idx] - X @ C
    return C.T, float((r ** 2).mean())

def viterbi(Y, C):
    """MLSE по линейной модели C (P, NTAP+1). -> (биты NT, метрика)."""
    pred = STB @ C[:, :NTAP].T + C[:, NTAP]                       # (NS, P)
    bm = ((Y[:, None, :] - pred[None, :, :]) ** 2).sum(2)           # (NT, NS)
    # известные биты: преамбула и пустота после пакета
    for i in range(NT):
        for d in range(-L, R + 1):
            k = i + d
            if k < 24 or k >= NT:
                want = (PREB[k] if 0 <= k < 24 else 0) * 2.0 - 1
                if k < 0: want = -1.0
                bm[i] = np.where(STB[:, d + L] == want, bm[i], 1e9)
    prev0 = np.arange(NS) >> 1; prev1 = prev0 | (1 << (NTAP - 1))
    metric = bm[0]; back = np.zeros((NT, NS), bool)
    for i in range(1, NT):
        m0 = metric[prev0]; m1 = metric[prev1]; ch = m1 < m0; back[i] = ch
        metric = np.where(ch, m1, m0) + bm[i]
    s = int(np.argmin(metric)); best = float(metric[s])
    out = np.zeros(NT + R, np.uint8)
    for i in range(NT - 1, 0, -1):
        out[i + R] = s & 1; s = (s >> 1) | ((1 << (NTAP - 1)) if back[i][s] else 0)
    for k in range(R + 1): out[k] = (s >> (R - k)) & 1           # состояние i=0: биты -L..R
    return out[:NT], best

PRE_IDX = np.arange(L, 24 - R)            # биты преамбулы, у которых все соседи известны
PRE_X = design(PREB * 2.0 - 1, np.arange(0, 24))   # (24, NTAP+1) для предсказания преамбулы

class Decoder:
    """Декодер одной записи. Априорная модель канала — среднее по уверенно
    прочитанным строкам этой же записи (копится по ходу работы)."""
    def __init__(self):
        self.prior = None; self.n = 0

    def offset(self, y, lo, hi, step):
        """Смещение по совпадению отсчётов преамбулы с предсказанием модели
        (framing code 0x27 снимает неоднозначность периодичного run-in)."""
        offs = np.arange(lo, hi + 1e-9, step); best = []
        for off in offs:
            Y = sample(y, off)
            if self.prior is None:
                C, r = fit(Y, PREB.astype(float), PRE_IDX)
            else:
                r = float(((Y[:24] - PRE_X @ self.prior.T) ** 2).mean())
            best.append((r, off))
        best.sort()
        return [o for _, o in best[:3]]

    def decode(self, line, lo=110.0, hi=124.0, step=0.25):
        """-> (42 байта, смещение, метрика на бит)."""
        y = norm(line); best = None
        for off in self.offset(y, lo, hi, step):
            Y = sample(y, off)
            C, _ = fit(Y, PREB.astype(float), PRE_IDX, self.prior, 2.0)
            bits, met = viterbi(Y, C)
            for _ in range(2):                                           # уточнение по всему пакету
                C, _ = fit(Y, bits.astype(float), np.arange(L, NT - R))
                bits, met = viterbi(Y, C)
            if best is None or met < best[0]: best = (met, off, bits, C)
        met, off, bits, C = best
        data = np.packbits(bits[24:NT].reshape(-1, 8)[:, ::-1], axis=1).ravel()
        if confident(data):                                              # копим априорную модель
            self.n += 1; w = 1.0 / min(self.n, 200)
            self.prior = C if self.prior is None else (1 - w) * self.prior + w * C
        return data, off, met / NT

H8 = set()
for dd in range(16):
    b = [(dd >> i) & 1 for i in range(4)]
    p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
    p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
    H8.add(sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]])))
def confident(p):
    """Адрес читается и ошибок чётности не больше одной."""
    bad = int((np.unpackbits(np.asarray(p[2:], np.uint8)[:, None], axis=1).sum(1) % 2 == 0).sum())
    return int(p[0]) in H8 and int(p[1]) in H8 and bad <= 1

def refine(line, off, bits_data, passes=2):
    """Гибрид: первое чтение (bits_data, 336 бит) даёт декодер с шаблонами;
    по нему модель канала подгоняется под эту строку и пакет читается заново.
    -> (42 байта, метрика на бит)."""
    y = norm(line); Y = sample(y, off)
    bits = np.r_[PREB, np.asarray(bits_data, np.uint8)]
    met = None
    for _ in range(passes):
        C, _ = fit(Y, bits.astype(float), np.arange(L, NT - R))
        bits, met = viterbi(Y, C)
    return np.packbits(bits[24:NT].reshape(-1, 8)[:, ::-1], axis=1).ravel(), met / NT
