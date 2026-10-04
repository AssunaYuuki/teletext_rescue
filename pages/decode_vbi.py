# Декодирует запись VBI и собирает проект страниц:
#   python decode_vbi.py запись.vbi --out папка [--lpf 32] [--no-train] [--procs N] [--cpu] [--no-repair]
# Формат: bt8x8, 8 бит на отсчёт, 2048 отсчётов на строку (35,47 МГц), --lpf строк на кадр.
#
# Два декодера, выбирается по записи:
#   * шаблоны (vbidecode, Витерби по средним формам сигнала) — лучший на сильно
#     размытой ленте (testvbi); на видеокарте (vbidecode_gpu, OpenCL) в сотни раз
#     быстрее, результат тот же; --cpu — только процессор;
#   * подгонка канала под каждую строку по 24 известным битам вступления, как в
#     decode-orc (teletext_slicer.cpp, детектор MLSE; у нас experiments/mlse_fit.py) —
#     не требует обучения, лучше на более чистых записях.
# Подготовка (на выборке строк):
#   1. базовые шаблоны (обучены на testvbi); если читают плохо — запись незнакомая:
#      декодером decode-orc ищется общее смещение начала пакета (как калибровка
#      смещения захвата в decode-orc, vbi_offset_calibration) и по уверенно
#      прочитанным строкам обучаются новые шаблоны;
#   2. два прохода дообучения шаблонов на этой записи;
#   3. если декодер decode-orc на выборке лучше — строки, которые шаблоны не
#      прочитали уверенно, дочитываются им.
# Строки "STEP ..." и "PROGRESS n из" — для индикатора хода в программе.
import os, subprocess, sys, time
os.environ.setdefault('VBI_W', '4')
for _k in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ.setdefault(_k, '1')                # процессов и так по числу ядер
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'experiments'))
SPL = 2048
arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d

H8 = {}
for dd in range(16):
    b = [(dd >> i) & 1 for i in range(4)]
    p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
    p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
    H8[sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))] = dd
def ham_fix(x):
    if x in H8: return x
    c = [w for w in H8 if bin(w ^ x).count('1') == 1]
    return c[0] if len(c) == 1 else None
def par_bad(p):
    p = np.frombuffer(p, np.uint8) if isinstance(p, (bytes, bytearray)) else np.asarray(p, np.uint8)
    return int((np.unpackbits(p[2:, None], axis=1).sum(1) % 2 == 0).sum())
def confident(p): return p[0] in H8 and p[1] in H8 and par_bad(p) <= 1
def score(p):
    """Чем меньше, тем лучше: ошибки чётности + штраф за нечитаемый адрес."""
    return par_bad(p) + 20 * ((ham_fix(int(p[0])) is None) + (ham_fix(int(p[1])) is None))

def open_vbi(path, lpf):
    v = np.memmap(path, dtype=np.uint8, mode='r')
    n = len(v) // (lpf * SPL)
    return v[:n * lpf * SPL].reshape(n, lpf, SPL)

# ---------- рабочие процессы
_V = None; _WIN = (113.0, 121.0)
def _init(path, lpf, tpl, win):
    global _V, V, _WIN
    import vbidecode as V
    if tpl: V.load_zoned(tpl)
    _V = open_vbi(path, lpf); _WIN = win
def _work(rng):
    """Кадры f0..f1 шаблонами -> (кадров, [(кадр, строка, байты)])."""
    f0, f1 = rng; out = []
    blk = np.asarray(_V[f0:f1], dtype=np.float32); sd = blk.std(2)
    for i in range(f1 - f0):
        for l in np.where(sd[i] > 20)[0]:
            b, off, met = V.decode_fast(blk[i, l], *_WIN)
            out.append((f0 + i, int(l), bytes(b), float(off)))
    return f1 - f0, out
def _lines(chunk):
    """[(кадр, строка)] шаблонами -> [(кадр, строка, байты, смещение)]."""
    out = []
    for f, l in chunk:
        b, off, met = V.decode_fast(np.asarray(_V[f, l], dtype=np.float32), *_WIN)
        out.append((int(f), int(l), bytes(b), off))
    return out
def _mlse(chunk):
    """[(кадр, строка)] декодером decode-orc -> [(кадр, строка, байты, смещение)]."""
    import mlse_fit as M
    D = M.Decoder(); out = []
    for f, l in chunk:
        b, off, met = D.decode(np.asarray(_V[f, l], float), _WIN[0], _WIN[1], 0.5)
        out.append((int(f), int(l), bytes(b), float(off)))
    return out

def cri_template(spb, blur=0.4, lead=1):
    """Run-in + framing code 0x27 в отсчётах записи, размытые гауссом (decode-orc
    vbi_cri_template.cpp: blur 0,4 бита, 1 бит нуля перед run-in); среднее 0, норма 1."""
    from scipy.special import erf
    bits = [(b >> i) & 1 for b in (0x55, 0x55, 0x27) for i in range(8)]
    n = int(np.ceil((lead + len(bits)) * spb)); x = np.arange(n); v = np.full(n, -0.5); prev = 0
    for i, b in enumerate(bits):
        if b != prev: v += (1 if b else -1) * 0.5 * (1 + erf((x - (lead + i) * spb) / (blur * spb * np.sqrt(2))))
        prev = b
    v -= v.mean(); return v / np.linalg.norm(v), lead * spb

def cri_peak(y, t, hi=700):
    """Нормированная корреляция строки с шаблоном: (максимум, позиция)."""
    y = np.asarray(y[:hi], float); n = len(t); k = np.ones(n)
    c = np.correlate(y, t, 'valid')
    s = np.sqrt(np.maximum(np.convolve(y * y, k, 'valid') - np.convolve(y, k, 'valid') ** 2 / n, 1e-9))
    r = c / s; i = int(np.argmax(r)); return float(r[i]), i

class Reader:
    """Декодирование одной записи: подготовка по выборке и чтение строк."""
    def __init__(self, path, lpf=32, procs=None, gpu=True, workdir=None, log=print):
        import vbidecode as V
        self.V = V; self.path = os.path.abspath(path); self.lpf = lpf; self.log = log
        self.procs = procs or max(1, (os.cpu_count() or 2) - 1)
        self.v = open_vbi(self.path, lpf); self.N = len(self.v)
        self.win = (113.0, 121.0); self.fallback = False; self.repair = True
        V.load_zoned()                                            # базовые шаблоны (обучены на testvbi)
        self.tpl = os.path.join(workdir, 'zoned_tpl.npy') if workdir else None
        self.save_tpl()
        self.gpu = None
        if gpu:
            try:
                import vbidecode_gpu as G
                if G.available(): self.gpu = G.GPUDecoder(); log('decoding on the GPU: {0}'.format(self.gpu.name))
            except Exception as e:
                log('GPU unavailable ({0}: {1}) — decoding on the CPU'.format(type(e).__name__, e))

    def save_tpl(self):
        if self.tpl: np.save(self.tpl, self.V._ZT)
        elif not hasattr(self, '_tmp'):
            import tempfile; self._tmp = tempfile.mkdtemp(); self.tpl = os.path.join(self._tmp, 'zoned_tpl.npy')
            np.save(self.tpl, self.V._ZT)
    def set_templates(self, T):
        self.V.set_zoned(T); np.save(self.tpl, T)
        if self.gpu: self.gpu.set_templates()

    def pool(self, fn, chunks, total, label=''):
        from multiprocessing import Pool
        out = []
        with Pool(self.procs, initializer=_init, initargs=(self.path, self.lpf, self.tpl, self.win)) as p:
            for res in p.imap_unordered(fn, chunks):
                out += res; print(f'PROGRESS {len(out)} {total} {label}', flush=True)
        return out

    def tpl_lines(self, sel):
        """[(кадр, строка)] шаблонами -> [(кадр, строка, байты, смещение)]."""
        if self.gpu:
            out = []
            for a in range(0, len(sel), 20000):
                part = sel[a:a + 20000]
                b, o, _ = self.gpu.decode(np.stack([np.asarray(self.v[f, l], np.float32) for f, l in part]), *self.win)
                out += [(int(f), int(l), bytes(b[i]), float(o[i])) for i, (f, l) in enumerate(part)]
            return out
        return self.pool(_lines, [sel[i:i + 50] for i in range(0, len(sel), 50)], len(sel))

    def mlse_lines(self, sel, win=None):
        keep = self.win
        if win: self.win = win
        try: return self.pool(_mlse, [sel[i:i + 40] for i in range(0, len(sel), 40)], len(sel), 'decode-orc decoder')
        finally: self.win = keep

    def train(self, res):
        """Шаблоны по уверенно прочитанным строкам [(кадр, строка, байты, смещение)]."""
        V = self.V; S = []
        for f, l, b, off in res:
            b = np.frombuffer(b, np.uint8)
            if confident(b): S.append((V.norm(np.asarray(self.v[f, l])), off, V.bits_of(b)))
        if len(S) >= 300: self.set_templates(V.train_zoned(S))
        return len(S)

    def sample(self, n_frames=1500, n_lines=2500):
        rng = np.random.default_rng(0)
        fs = np.sort(rng.choice(self.N, min(self.N, n_frames), replace=False))
        cand = [(int(f), int(l)) for f in fs for l in np.where(np.asarray(self.v[f], np.float32).std(1) > 20)[0]]
        rng.shuffle(cand); return cand[:n_lines]

    def calibrate(self, cand, acc=0.5):
        """Общее смещение начала пакета по корреляции с run-in + framing code, как
        калибровка смещения захвата в decode-orc (vbi_offset_calibration.cpp): медиана
        положений пика по строкам, где корреляция выше порога. -> смещение или None."""
        t, lead = cri_template(self.V.SPB); pos = []
        for f, l in cand[:1500]:
            r, i = cri_peak(self.v[f, l], t)
            if r >= acc: pos.append(i + lead)
        if len(pos) < 30: return None
        pos = np.asarray(pos); h = np.bincount(np.round(pos).astype(int)); m = int(np.argmax(np.convolve(h, np.ones(9), 'same')))
        near = pos[np.abs(pos - m) <= 8]
        if len(near) < 30: return None
        self.log('run-in found on {0} of {1} sampled lines, offset {2:.1f} samples (spread {3:.1f})'.format(len(pos), min(len(cand), 1500), np.median(near), np.median(np.abs(near - np.median(near))) * 1.4826))
        return float(np.median(near))

    def prepare(self, cand, train=True, step=print):
        """Подготовка по выборке строк. -> доля уверенно прочитанных или None, если запись не читается."""
        V = self.V; rate = lambda r: sum(confident(np.frombuffer(x[2], np.uint8)) for x in r) / max(1, len(r))
        step('calibrating the offset from the run-in')
        off = self.calibrate(cand)
        if off is not None: self.win = (off - 6, off + 6)
        step('checking with the base templates')
        r0 = rate(self.tpl_lines(cand)); self.log('base templates: {0:.0%} of sampled lines read confidently'.format(r0))
        rm = None
        if r0 < 0.3:
            step('unfamiliar recording: calibrating with the decode-orc decoder')
            m = self.mlse_lines(cand[:1200], (90.0, 135.0))
            ok = [x for x in m if confident(np.frombuffer(x[2], np.uint8))]
            rm = len(ok) / max(1, len(m))
            self.log('decode-orc decoder without training: {0:.0%} of sampled lines read confidently'.format(rm))
            if len(ok) >= 100:
                med = float(np.median([x[3] for x in ok]))
                self.win = (med - 6, med + 6)
                self.log('packet start offset: {0:.1f} samples (search window {1:.0f}–{2:.0f})'.format(med, self.win[0], self.win[1]))
                refined = []
                for f, l, b, off in ok:                         # точное смещение под модель шаблонов
                    y = V.norm(np.asarray(self.v[f, l])); bits = np.r_[V.PREB, V.bits_of(np.frombuffer(b, np.uint8))]
                    o = V.best_offset(y, bits, off - 3, off + 3, 1.0); o = V.best_offset(y, bits, o - 1, o + 1, 0.25)
                    refined.append((f, l, b, o))
                self.train(refined)
            elif r0 < 0.03:
                return None
        rt = r0
        if train:
            res = self.tpl_lines(cand); rt = rate(res)
            for it in range(1, 6):                     # дообучение, пока растёт доля уверенных строк
                step('adapting templates to the recording, pass {0}'.format(it))
                if self.train(res) < 300:
                    self.log('too few confident lines — templates are left unchanged'); break
                res = self.tpl_lines(cand); prev, rt = rt, rate(res)
                self.log('{0:.0%} of sampled lines read confidently'.format(rt))
                if it >= 2 and rt < prev + 0.01: break
        if rm is not None and rm > rt + 0.05:
            self.fallback = True
            self.log('on this recording the decode-orc decoder is better ({0:.0%} vs {1:.0%}): it will read the lines the templates could not'.format(rm, rt))
        best = max(rt, rm or 0)
        return best if best >= 0.03 else None

    def decode_all(self, step=print, frames=None):
        """Все строки с сигналом -> {(кадр, строка): байты}."""
        frames = frames if frames is not None else range(self.N)
        frames = np.asarray(list(frames)); N = len(frames)
        res = {}; offs = {}; t0 = time.time()
        def eta(done):
            r = (time.time() - t0) / max(done, 1) * (N - done)
            return '~{0:.0f} min left'.format(r / 60) if r >= 90 else '~{0:.0f} s left'.format(r)
        if self.gpu:
            step('decoding {0} frames on the GPU'.format(N))
            for a in range(0, N, 400):
                fr = frames[a:a + 400]; blk = np.asarray(self.v[fr], np.float32); F, L = np.where(blk.std(2) > 20)
                if len(F):
                    b, o, _ = self.gpu.decode(blk[F, L], *self.win)
                    for i, (f, l) in enumerate(zip(F, L)):
                        res[(int(fr[f]), int(l))] = bytes(b[i]); offs[(int(fr[f]), int(l))] = float(o[i])
                print(f'PROGRESS {min(N, a + 400)} {N} {eta(min(N, a + 400))}', flush=True)
        else:
            step('decoding {0} frames in {1} processes'.format(N, self.procs))
            from multiprocessing import Pool
            if len(frames) == self.N: ranges = [(a, min(self.N, a + 100)) for a in range(0, self.N, 100)]
            else: ranges = [(int(f), int(f) + 1) for f in frames]
            done = 0
            with Pool(self.procs, initializer=_init, initargs=(self.path, self.lpf, self.tpl, self.win)) as p:
                for nf, out in p.imap_unordered(_work, ranges):
                    for f, l, b, o in out: res[(f, l)] = b; offs[(f, l)] = o
                    done += nf; print(f'PROGRESS {done} {N} {eta(done)}', flush=True)
        if self.fallback:
            weak = [k for k, b in res.items() if not confident(np.frombuffer(b, np.uint8))]
            step('reading the rest with the decode-orc decoder: {0} lines'.format(len(weak)))
            better = 0
            for f, l, b, off in self.mlse_lines(weak):
                if score(np.frombuffer(b, np.uint8)) < score(np.frombuffer(res[(f, l)], np.uint8)):
                    res[(f, l)] = b; offs.pop((f, l), None); better += 1
            self.log('the decode-orc decoder improved {0} of {1} lines'.format(better, len(weak)))
        if self.repair:
            self.repair_all(res, offs, step)
        return res

    def repair_all(self, res, offs, step=print):
        """Починка байтов с ошибкой чётности, как repair_damaged_bytes в decode-orc:
        в каждом таком байте переворачивается бит, в котором декодер был уверен меньше всего
        (vbidecode.repair). Только для строк, прочитанных шаблонами."""
        V = self.V
        todo = [k for k, b in res.items() if k in offs and
                any(bin(b[i]).count('1') % 2 == 0 for i in V.display_bytes(np.frombuffer(b, np.uint8)))]
        step('repairing damaged bytes: {0} lines'.format(len(todo)))
        fixed = 0
        for n, (f, l) in enumerate(todo):
            r, k = V.repair(self.v[f, l], offs[(f, l)], res[(f, l)])
            res[(f, l)] = bytes(r); fixed += k
            if n % 2000 == 0: print(f'PROGRESS {n} {len(todo)}', flush=True)
        self.log('bytes repaired: {0} in {1} lines'.format(fixed, len(todo)))

if __name__ == '__main__':
    VBI = os.path.abspath(sys.argv[1]); OUT = os.path.abspath(arg('--out')); LPF = int(arg('--lpf', 32))
    os.makedirs(OUT, exist_ok=True)
    def step(t): print('STEP', t, flush=True)
    def log(t): print(t, flush=True)
    R = Reader(VBI, LPF, int(arg('--procs', 0)) or None, '--cpu' not in sys.argv, OUT, log)
    N = R.N
    if N == 0: sys.exit('the file is smaller than one frame ({0} x {1} bytes) — check the format'.format(LPF, SPL))
    log('{0}: {1} frames ({2:.0f} s), {3} lines of {4} samples'.format(os.path.basename(VBI), N, N / 25, LPF, SPL))
    R.repair = '--no-repair' not in sys.argv
    cand = R.sample()
    if not cand: sys.exit('no lines with a teletext signal were found in the recording')
    if R.prepare(cand, '--no-train' not in sys.argv, step) is None:
        sys.exit('The decoder cannot read this recording: almost no line gave a valid address\n(neither with templates nor with the decode-orc decoder). Probably a different capture format\n(sample rate, number of lines) or the signal is too distorted. If you have a .t42 stream\nfrom another program (e.g. vhs-teletext), open it with “Open stream .t42”.')
    res = R.decode_all(step)

    step('writing the stream')
    lp = np.full((N, LPF), -1, np.int32); pk = []; rej = 0
    for (f, l) in sorted(res):
        b = res[(f, l)]
        # не телетекст или сплошные ошибки; адрес не исправляем (decode-orc address_attested):
        # пакет с исправимым адресом пишется как прочитан, к странице его не припишут
        if ham_fix(b[0]) is None or ham_fix(b[1]) is None or par_bad(b) > 10: rej += 1; continue
        lp[f, l] = len(pk); pk.append(bytes(b))
    t42 = os.path.join(OUT, 'stream.t42'); open(t42, 'wb').write(b''.join(pk))
    lines = os.path.join(OUT, 'line_pkt.npy'); np.save(lines, lp)
    log('packets {0}, lines discarded {1}'.format(len(pk), rej))
    subprocess.run([sys.executable, os.path.join(HERE, 'build_project.py'), '--t42', t42, '--out', OUT,
                    '--lines', lines, '--lpf', str(LPF), '--vbi', VBI], check=True)
