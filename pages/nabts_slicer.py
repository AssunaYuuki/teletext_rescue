"""Чтение строк NABTS с записи VBI (MLSE, как детектор decode-orc для лент VHS).

На ленте полоса обрезана, вступление 1010… размыто, пороговое чтение не работает
(decode-orc nabts_sink/instructions.md, «detector»). Поэтому для каждой строки
подгоняется линейная модель канала по 24 известным битам (0x55 0x55 и код кадра 0xE7),
и пакет читается Витерби; затем модель уточняется по всему прочитанному пакету.
Основа — pages/experiments/mlse_fit.py (то же для WST).

  python pages/nabts_slicer.py запись.vbi [--out файл.t33] [--lines 15,16,17] [--fields N] [--cpu]
Витерби — на видеокарте (OpenCL), если она есть; --cpu — на процессоре.
Формат записи — дамп cx23885 (1440 отсчётов, 27 МГц, 12 строк на поле = строки 10–21),
как в decode-orc vbi_source_format.cpp.
"""
import os, sys
import numpy as np

FS = 27e6
BITRATE = 5727272.0
SPB = FS / BITRATE                      # 4,714 отсчёта на бит
PREB = np.unpackbits(np.array([0x55, 0x55, 0xE7], np.uint8)[:, None], axis=1)[:, ::-1].ravel()
NB = 33 * 8; NT = 24 + NB
L, R = 3, 3
NTAP = L + R + 1; NS = 1 << NTAP
PH = np.array([0.3, 0.5, 0.7])
STB = ((np.arange(NS)[:, None] >> np.arange(NTAP - 1, -1, -1)[None, :]) & 1) * 2.0 - 1
PREV0 = np.arange(NS) >> 1; PREV1 = PREV0 | (1 << (NTAP - 1))

# Ограничения Витерби по известным битам: на шагах i, где окно задевает преамбулу или
# пустоту после пакета, недопустимые состояния получают большую метрику.
_BAN = np.zeros((NT, NS), bool)
for _i in range(NT):
    for _d in range(-L, R + 1):
        _k = _i + _d
        if _k < 24 or _k >= NT:
            _want = -1.0 if _k < 0 or _k >= NT else PREB[_k] * 2.0 - 1
            _BAN[_i] |= STB[:, _d + L] != _want


def norm(line):
    y = np.asarray(line, float); y = y - y.mean(); s = y.std()
    return y / (s if s > 1e-6 else 1)


def sample(y, off):
    pos = off + (np.arange(NT)[:, None] + PH[None, :]) * SPB
    return np.interp(pos, np.arange(len(y)), y, left=0, right=0)


def design(bits_pm, idx):
    pad = np.r_[np.zeros(L), bits_pm, np.zeros(R + 1)]
    return np.c_[np.stack([pad[idx + d + L] for d in range(-L, R + 1)], 1), np.ones(len(idx))]


def fit(Y, bits01, idx, prior=None, lam=0.0):
    X = design(bits01 * 2.0 - 1, idx)
    if prior is not None and lam > 0:
        A = X.T @ X + lam * np.eye(X.shape[1]); B = X.T @ Y[idx] + lam * prior.T
        C = np.linalg.solve(A, B)
    else:
        C, *_ = np.linalg.lstsq(X, Y[idx], rcond=None)
    r = Y[idx] - X @ C
    return C.T, float((r ** 2).mean())


def viterbi(Y, C):
    pred = STB @ C[:, :NTAP].T + C[:, NTAP]
    bm = ((Y[:, None, :] - pred[None, :, :]) ** 2).sum(2)
    bm[_BAN] = 1e9
    metric = bm[0]; back = np.zeros((NT, NS), bool)
    for i in range(1, NT):
        m0 = metric[PREV0]; m1 = metric[PREV1]; ch = m1 < m0; back[i] = ch
        metric = np.where(ch, m1, m0) + bm[i]
    s = int(np.argmin(metric)); best = float(metric[s])
    out = np.zeros(NT + R, np.uint8)
    for i in range(NT - 1, 0, -1):
        out[i + R] = s & 1; s = (s >> 1) | ((1 << (NTAP - 1)) if back[i][s] else 0)
    for k in range(R + 1):
        out[k] = (s >> (R - k)) & 1
    return out[:NT], best


PRE_IDX = np.arange(L, 24 - R)
PRE_X = design(PREB * 2.0 - 1, np.arange(0, 24))

_HAM = set([0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA])


def good_prefix(p):
    """Все пять байт префикса — точные кодовые слова Hamming 8/4."""
    return all(int(b) in _HAM for b in p[:5])


class Decoder:
    def __init__(self):
        self.prior = None; self.n = 0

    def offsets(self, y, lo, hi, step):
        best = []
        for off in np.arange(lo, hi + 1e-9, step):
            Y = sample(y, off)
            if self.prior is None:
                _, r = fit(Y, PREB.astype(float), PRE_IDX)
            else:
                r = float(((Y[:24] - PRE_X @ self.prior.T) ** 2).mean())
            best.append((r, off))
        best.sort()
        return [o for _, o in best[:2]]

    def decode(self, line, lo, hi, step=0.25):
        """-> (33 байта, смещение, метрика на бит)."""
        y = norm(line); best = None
        for off in self.offsets(y, lo, hi, step):
            Y = sample(y, off)
            C, _ = fit(Y, PREB.astype(float), PRE_IDX, self.prior, 2.0)
            bits, met = viterbi(Y, C)
            C, _ = fit(Y, bits.astype(float), np.arange(L, NT - R))
            bits, met = viterbi(Y, C)
            if best is None or met < best[0]:
                best = (met, off, bits, C)
        met, off, bits, C = best
        data = np.packbits(bits[24:NT].reshape(-1, 8)[:, ::-1], axis=1).ravel()
        if good_prefix(data):
            self.n += 1; w = 1.0 / min(self.n, 200)
            self.prior = C if self.prior is None else (1 - w) * self.prior + w * C
        return data, off, met / NT


def has_signal(line):
    x = np.asarray(line, float)
    return x[100:1380].std() > 12


# ---------------------------------------------------------------------------
# Пакетное чтение: тот же алгоритм, что Decoder.decode, но сразу тысячи строк —
# смещение и подгонка модели в numpy, Витерби на видеокарте (OpenCL, одна рабочая
# группа на строку, один поток на состояние) или в numpy, если видеокарты нет.
# ---------------------------------------------------------------------------
KM = np.zeros(NT, np.int32); KV = np.zeros(NT, np.int32)      # известные биты состояния на шаге
for _i in range(NT):
    for _d in range(-L, R + 1):
        _k = _i + _d
        if _k < 24 or _k >= NT:
            _bit = NTAP - 1 - (_d + L)
            KM[_i] |= 1 << _bit
            if 0 <= _k < 24 and PREB[_k]:
                KV[_i] |= 1 << _bit

KERNEL = r'''
#define NS %(NS)d
#define NTAP %(NTAP)d
#define NT %(NT)d
#define NP %(NP)d
#define RR %(R)d
#define NW (NS / 32)
__kernel void viterbi(__global const float *Y,     /* (задач, NT, NP) */
                      __global const float *C,     /* (задач, NP, NTAP + 1) */
                      __global const int *km, __global const int *kv,
                      __global uint *back,         /* (задач, NT, NW) */
                      __global float *out_met,
                      __global uchar *out_bits)    /* (задач, NT + RR) */
{
    const int t = get_group_id(0), s = get_local_id(0);
    __local float met[2][NS]; __local uint bw[NW];
    __local float rmin[NS]; __local int rarg[NS];
    __global const float *c = C + (size_t)t * NP * (NTAP + 1);
    __global const float *y = Y + (size_t)t * NT * NP;
    float pr[NP];
    for (int p = 0; p < NP; p++) {
        float v = c[p * (NTAP + 1) + NTAP];
        for (int k = 0; k < NTAP; k++)
            v += (((s >> (NTAP - 1 - k)) & 1) ? 1.0f : -1.0f) * c[p * (NTAP + 1) + k];
        pr[p] = v;
    }
    int cur = 0;
    for (int i = 0; i < NT; i++) {
        if (s < NW) bw[s] = 0;
        barrier(CLK_LOCAL_MEM_FENCE);
        float bm = 0.0f;
        for (int p = 0; p < NP; p++) { float d = y[i * NP + p] - pr[p]; bm += d * d; }
        if ((s & km[i]) != kv[i]) bm = 1e9f;
        if (i == 0) {
            met[0][s] = bm;
        } else {
            float m0 = met[cur][s >> 1], m1 = met[cur][(s >> 1) | (NS >> 1)];
            int ch = m1 < m0;
            met[cur ^ 1][s] = (ch ? m1 : m0) + bm;
            if (ch) atomic_or(&bw[s >> 5], 1u << (s & 31));
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        if (i > 0) {
            if (s < NW) back[((size_t)t * NT + i) * NW + s] = bw[s];
            cur ^= 1;
        }
    }
    rmin[s] = met[cur][s]; rarg[s] = s;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int h = NS / 2; h > 0; h >>= 1) {
        if (s < h && (rmin[s + h] < rmin[s] || (rmin[s + h] == rmin[s] && rarg[s + h] < rarg[s]))) {
            rmin[s] = rmin[s + h]; rarg[s] = rarg[s + h];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (s == 0) {
        /* обратный проход, как viterbi(): бит k хранится в o[k] */
        __global uchar *o = out_bits + (size_t)t * (NT + RR);
        int q = rarg[0];
        out_met[t] = rmin[0];
        for (int i = NT - 1; i > 0; i--) {
            o[i + RR] = q & 1;
            uint w = back[((size_t)t * NT + i) * NW + (q >> 5)];
            q = (q >> 1) | (((w >> (q & 31)) & 1) ? (1 << (NTAP - 1)) : 0);
        }
        for (int k = 0; k <= RR; k++) o[k] = (q >> (RR - k)) & 1;
    }
}
'''


class GPUViterbi:
    def __init__(self):
        import pyopencl as cl
        self.cl = cl
        dev = next(d for p in cl.get_platforms() for d in p.get_devices() if d.type & cl.device_type.GPU)
        self.name = dev.name
        self.ctx = cl.Context([dev]); self.q = cl.CommandQueue(self.ctx)
        src = KERNEL % dict(NS=NS, NTAP=NTAP, NT=NT, NP=len(PH), R=R)
        self.knl = cl.Kernel(cl.Program(self.ctx, src).build(), 'viterbi')
        mf = cl.mem_flags
        self.b_km = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=KM)
        self.b_kv = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=KV)

    def __call__(self, Y, C):
        """Y (n, NT, P), C (n, P, NTAP+1) -> (биты (n, NT), метрики (n,))."""
        cl = self.cl; mf = cl.mem_flags; n = len(Y)
        b_Y = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=np.ascontiguousarray(Y, np.float32))
        b_C = cl.Buffer(self.ctx, mf.READ_ONLY | mf.COPY_HOST_PTR, hostbuf=np.ascontiguousarray(C, np.float32))
        b_back = cl.Buffer(self.ctx, mf.READ_WRITE, size=n * NT * (NS // 32) * 4)
        b_met = cl.Buffer(self.ctx, mf.WRITE_ONLY, size=n * 4)
        b_bits = cl.Buffer(self.ctx, mf.WRITE_ONLY, size=n * (NT + R))
        self.knl(self.q, (n * NS,), (NS,), b_Y, b_C, self.b_km, self.b_kv, b_back, b_met, b_bits)
        met = np.empty(n, np.float32); bits = np.empty((n, NT + R), np.uint8)
        cl.enqueue_copy(self.q, met, b_met); cl.enqueue_copy(self.q, bits, b_bits)
        return bits[:, :NT], met.astype(float)


def viterbi_numpy(Y, C):
    """То же, что GPUViterbi, на процессоре (все строки сразу, по шагам)."""
    n = len(Y); rows = np.arange(n)
    pred = np.einsum('sk,npk->nsp', STB, C[:, :, :NTAP]) + C[:, None, :, NTAP]   # (n, NS, P)
    metric = None; back = np.zeros((NT, n, NS), bool)
    for i in range(NT):
        bm = ((Y[:, i, None, :] - pred) ** 2).sum(2)
        bm[:, _BAN[i]] = 1e9
        if i == 0:
            metric = bm; continue
        m0 = metric[:, PREV0]; m1 = metric[:, PREV1]; ch = m1 < m0; back[i] = ch
        metric = np.where(ch, m1, m0) + bm
    s = metric.argmin(1); best = metric[rows, s]
    out = np.zeros((n, NT + R), np.uint8)
    for i in range(NT - 1, 0, -1):
        out[:, i + R] = s & 1
        s = (s >> 1) | np.where(back[i, rows, s], 1 << (NTAP - 1), 0)
    for k in range(R + 1):
        out[:, k] = (s >> (R - k)) & 1
    return out[:, :NT], best


def _interp(y, pos, own=False):
    """np.interp(pos, arange(S), строка, left=0, right=0) для каждой строки y (n, S).
    own=False — общие точки pos для всех строк; True — у каждой строки свои pos (n, ...)."""
    S = y.shape[1]
    i0 = np.floor(pos).astype(int); fr = (pos - i0).astype(np.float32)
    ok = (pos >= 0) & (pos <= S - 1)
    i0 = np.clip(i0, 0, S - 1); i1 = np.clip(i0 + 1, 0, S - 1)
    if own:
        r = np.arange(len(y)).reshape((len(y),) + (1,) * (pos.ndim - 1))
        return np.where(ok, y[r, i0] * (1 - fr) + y[r, i1] * fr, 0)
    return np.where(ok, y[:, i0] * (1 - fr) + y[:, i1] * fr, 0)


_PRE_XP = design(PREB * 2.0 - 1, PRE_IDX)
_PRE_A = _PRE_XP.T @ _PRE_XP + 2.0 * np.eye(NTAP + 1)
_ALL_IDX = np.arange(L, NT - R)


def _refit(Y, bits):
    """Модель канала по всему прочитанному пакету каждой строки (как fit без prior)."""
    n = len(bits)
    pad = np.concatenate([np.zeros((n, L)), bits * 2.0 - 1, np.zeros((n, R + 1))], 1)
    X = np.stack([pad[:, _ALL_IDX + d + L] for d in range(-L, R + 1)] + [np.ones((n, len(_ALL_IDX)))], 2)
    A = np.einsum('nik,nil->nkl', X, X) + 1e-6 * np.eye(NTAP + 1)
    B = np.einsum('nik,nip->nkp', X, Y[:, _ALL_IDX])
    return np.linalg.solve(A, B).transpose(0, 2, 1)


class BatchDecoder:
    """Чтение многих строк сразу. prior — модель канала, накопленная Decoder на начале записи."""
    def __init__(self, prior, n_prior=50, gpu=True, log=print):
        self.prior = prior; self.n = n_prior
        self.vit = None
        if gpu:
            try:
                self.vit = GPUViterbi(); log('reading on the GPU: ' + self.vit.name)
            except Exception as e:
                log('GPU unavailable ({0}: {1}) — reading on the CPU'.format(type(e).__name__, e))
        if self.vit is None:
            self.vit = viterbi_numpy
        self.batch = 4096 if isinstance(self.vit, GPUViterbi) else 256

    def decode(self, rows, lo, hi, learn=None, step=0.25):
        """rows (n, 1440) -> (байты (n, 33), смещения (n,), метрики на бит (n,), префикс верен (n,)).
        learn (n,) — по каким строкам можно уточнять модель канала (по умолчанию по всем)."""
        y = np.asarray(rows, np.float32); n = len(y)
        y = y - y.mean(1, keepdims=True); sd = y.std(1, keepdims=True); y = y / np.where(sd > 1e-6, sd, 1)
        # 1) два лучших смещения по совпадению преамбулы с моделью (как Decoder.offsets)
        offs = np.arange(lo, hi + 1e-9, step)
        P = (PRE_X @ self.prior.T).astype(np.float32)                            # (24, P)
        pos = offs[:, None, None] + (np.arange(24)[:, None] + PH[None, :]) * SPB    # (K, 24, P)
        res = np.empty((n, len(offs)), np.float32)
        for a in range(0, n, 256):
            res[a:a + 256] = ((_interp(y[a:a + 256], pos) - P) ** 2).mean((2, 3))
        cand = offs[np.argsort(res, 1, kind='stable')[:, :2]].ravel()             # (2n,)
        posf = cand[:, None, None] + (np.arange(NT)[:, None] + PH[None, :]) * SPB
        Y = _interp(np.repeat(y, 2, 0), posf, own=True).astype(np.float32)        # (2n, NT, P)
        # 2) модель по преамбуле, притянутая к априорной (гребневая регрессия), Витерби
        B = np.einsum('ik,nip->nkp', _PRE_XP, Y[:, PRE_IDX]) + 2.0 * self.prior.T[None]
        C = np.linalg.solve(_PRE_A[None], B).transpose(0, 2, 1)
        bits, met = self.vit(Y, C)
        # 3) модель по всему пакету, Витерби ещё раз
        C = _refit(Y.astype(float), bits.astype(float))
        bits, met = self.vit(Y, C)
        met = met.reshape(n, 2); k = met.argmin(1); sel = np.arange(n) * 2 + k
        data = np.packbits(bits[sel, 24:NT].reshape(n, -1, 8)[:, :, ::-1], axis=2).reshape(n, 33)
        good = np.array([good_prefix(d) for d in data], bool)
        for i in np.flatnonzero(good if learn is None else good & learn):        # копим модель
            self.n += 1; w = 1.0 / min(self.n, 200)
            self.prior = (1 - w) * self.prior + w * C[sel[i]]
        return data, cand[sel], met[np.arange(n), k] / NT, good


def find_lines(a, lo, hi, fields=40):
    """Строки (номера 10–21), где пробное чтение даёт пакеты NABTS с верным префиксом."""
    out = []
    step = max(1, len(a) // fields)
    for ln in range(10, 22):
        rows = [a[f, ln - 10] for f in range(0, len(a), step)][:fields]
        rows = [r for r in rows if has_signal(r)]
        if len(rows) < fields // 4:
            continue
        dec = Decoder()
        ok = sum(good_prefix(dec.decode(r, lo, hi)[0]) for r in rows)
        print('line %d: %d of %d packets with a valid prefix' % (ln, ok, len(rows)), flush=True)
        if ok * 2 >= len(rows):
            out.append(ln)
    return out


def main():
    src = sys.argv[1]
    arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    out = arg('--out', os.path.splitext(src)[0] + '.t33')
    a = np.memmap(src, np.uint8, 'r').reshape(-1, 12, 1440)
    nf = min(len(a), int(arg('--fields', len(a))))
    lo, hi = float(arg('--lo', 0)), float(arg('--hi', 60))
    if arg('--lines'):
        lines = [int(v) for v in arg('--lines').split(',')]
    else:
        lines = find_lines(a, lo, hi)
        if not lines:
            print('No NABTS found on any of lines 10–21 (it may be WST or another service).')
            sys.exit(2)
    sig = np.zeros((nf, 12), bool)                       # строки с сигналом (не чёрные)
    for f in range(0, nf, 500):
        sig[f:f + 500] = np.asarray(a[f:min(nf, f + 500), :, 100:1380], np.float32).std(2) > 12
    # прочие строки: редкие пакеты (например, служебный канал раз в несколько секунд).
    # Берутся пакеты с верным префиксом, и только если верен префикс хотя бы у половины
    # строк с сигналом в этой строке и этом поле (чётном/нечётном) — испытательные
    # сигналы изредка тоже «читаются» с верным префиксом.
    # (строки, где сигнал почти всегда, уже проверены find_lines — там испытательные сигналы и т. п.)
    extra = [ln for ln in range(10, 22) if ln not in lines and 0 < sig[:, ln - 10].mean() < 0.25]
    print('STEP Reading NABTS from lines ' + ', '.join(map(str, lines))
          + ('; looking for packets on ' + ', '.join(map(str, extra)) if extra else ''), flush=True)
    # начало записи читается построчно: так копится модель канала и находится смещение
    dec = Decoder(); off = None
    for f in range(nf):
        for ln in lines:
            if sig[f, ln - 10]:
                _, off, _ = dec.decode(a[f, ln - 10], lo, hi)
        if dec.n > 200 or f > 400:
            break
    if dec.prior is None:
        print('No readable NABTS packets on lines ' + ', '.join(map(str, lines)))
        sys.exit(2)
    lo, hi = max(0, off - 6), off + 6
    bd = BatchDecoder(dec.prior, dec.n, gpu='--cpu' not in sys.argv)
    use = lines + extra
    F, Li = np.nonzero(sig[:, [ln - 10 for ln in use]])                  # по полям, в каждом по строкам
    Ln = np.array(use)[Li]
    order = np.lexsort((Ln, F)); F, Ln = F[order], Ln[order]
    main_line = np.isin(Ln, lines)
    D = np.zeros((len(F), 33), np.uint8); G = np.zeros(len(F), bool)
    for b in range(0, len(F), bd.batch):
        f, ln = F[b:b + bd.batch], Ln[b:b + bd.batch]
        mm = main_line[b:b + bd.batch]
        d, o, m, good = bd.decode(a[f, ln - 10], lo, hi, learn=mm)
        D[b:b + len(d)] = d; G[b:b + len(d)] = good
        if (good & mm).any():                                          # смещение могло уплыть
            c = float(np.median(o[good & mm])); lo, hi = max(0, c - 6), c + 6
        print('PROGRESS %d %d' % (int(f[-1]), nf), flush=True)
    keep = main_line.copy()
    for ln in extra:
        for par in (0, 1):
            sel = (Ln == ln) & (F % 2 == par)
            if sel.any() and G[sel].sum() * 2 >= sel.sum():
                keep |= sel & G
    pk = [bytes(D[i]) for i in np.flatnonzero(keep)]
    open(out, 'wb').write(b''.join(pk))
    ok = int(G[keep].sum())
    for ln in use:
        n = int((keep & (Ln == ln)).sum())
        if n:
            print('line %d: %d packets, %d with an error-free prefix' % (ln, n, int((keep & G & (Ln == ln)).sum())))
    print('lines with signal %d, error-free prefix %d (%.0f%%), written to %s'
          % (len(pk), ok, 100.0 * ok / max(1, len(pk)), out))


if __name__ == '__main__':
    main()
