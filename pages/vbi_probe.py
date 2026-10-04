"""Что за запись VBI и что в ней передаётся — без подсказок пользователя.

  python pages/vbi_probe.py запись.vbi [--json out.json]

Формат определяется по размеру и по самому сигналу (какая нарезка даёт повторяющуюся
от кадра к кадру структуру строк):
  bt8x8   — 32 строки × 2048 байт на кадр; PAL 35,47 МГц (строки 7–22, 320–335) или
            NTSC 28,64 МГц (1600 отсчётов, строки 10–21, 273–284);
  cx23885 — 12 строк × 1440 байт на поле, 27 МГц, строки 10–21;
  4fsc16  — 16 строк × 910 отсчётов по 16 бит на поле, 14,318 МГц (как TBC vhs-decode),
            первая строка — 14-я (по записи WTTW 1989: строка 21 — 8-я).
Служба строки определяется по скорости передачи (согласованность переходов через ноль
с тактом), по повторяемости (испытательные сигналы) и по пробному чтению:
  WST PAL 6,9375 Мбит/с, NABTS / WST NTSC 5,7273, Silent Radio 1,2528, CC 0,5035 (32 fH),
  VPS / WSS (бифазный код) и т. п.
"""
import json, os, sys
import numpy as np

FSC = 315e6 / 88
RATES = [('WST PAL teletext', 6.9375e6), ('NABTS / WST NTSC rate', 8 * FSC / 5),
         ('VPS', 5e6), ('Silent Radio', FSC * 7 / 20), ('WSS', 5e6 / 6), ('CC (line 21)', 32 * 15734.264)]


class Rec:
    """Доступ к строкам записи: line(u, r) -> float32 отсчёты; единица u — кадр или поле."""
    def __init__(self, path, fmt):
        self.path, self.fmt = path, fmt
        if fmt.startswith('bt8x8'):
            self.a = np.memmap(path, np.uint8, 'r'); n = len(self.a) // 65536
            self.a = self.a[:n * 65536].reshape(n, 32, 2048)
            ntsc = fmt == 'bt8x8-ntsc'
            self.fs = 28636363.0 if ntsc else 35468950.0
            self.ns = 1600 if ntsc else 2044
            self.rows = list(range(12)) + list(range(16, 28)) if ntsc else list(range(32))
            first = (10, 273) if ntsc else (7, 320)
            self.tv = {r: first[r // 16] + r % 16 for r in self.rows}
            self.scale = 1.0; self.unit = 'frame'
        elif fmt == 'cx23885':
            self.a = np.memmap(path, np.uint8, 'r'); n = len(self.a) // 17280
            self.a = self.a[:n * 17280].reshape(n, 12, 1440)
            self.fs, self.ns, self.rows = 27e6, 1440, list(range(12))
            self.tv = {r: 10 + r for r in self.rows}; self.scale = 1.0; self.unit = 'field'
        elif fmt == '4fsc16':
            self.a = np.memmap(path, '<u2', 'r'); n = len(self.a) // (910 * 16)
            self.a = self.a[:n * 910 * 16].reshape(n, 16, 910)
            self.fs, self.ns, self.rows = 4 * FSC, 910, list(range(16))
            self.tv = {r: 14 + r for r in self.rows}; self.scale = 64.0; self.unit = 'field'
        self.n = len(self.a)

    def line(self, u, r):
        return np.asarray(self.a[u, r, :self.ns], np.float32) / self.scale

    def tvline(self, u, r):
        """Номер строки ТВ в поле (у дампов по полям чётность поля неизвестна — поля A и B)."""
        return self.tv[r]


def _structure(a, n_units, sample):
    """Сходство соседних единиц (кадров/полей) по средней форме строк: у верной нарезки выше."""
    idx = np.linspace(0, n_units - 2, 40).astype(int)
    s = []
    for i in idx:
        x = np.asarray(sample(i), np.float32).ravel(); y = np.asarray(sample(i + 1), np.float32).ravel()
        x = x - x.mean(); y = y - y.mean()
        s.append((x * y).sum() / np.sqrt((x * x).sum() * (y * y).sum() + 1e-9))
    return float(np.median(s))


def detect_format(path):
    size = os.path.getsize(path)
    cand = []
    if size % 65536 == 0 or size % 65536 < 4:
        a = np.memmap(path, np.uint8, 'r'); n = size // 65536
        b = a[:n * 65536].reshape(n, 32, 2048)
        smp = np.asarray(b[np.linspace(0, n - 1, 20).astype(int)])
        ntsc = bool((smp[:, :-1, 1604:2040] == 0).mean() > 0.99)
        cand.append(('bt8x8-ntsc' if ntsc else 'bt8x8-pal', _structure(b, n, lambda i: b[i, :-1, :1600])))
    if size % 17280 == 0:
        a = np.memmap(path, np.uint8, 'r'); n = size // 17280; b = a.reshape(n, 12, 1440)
        cand.append(('cx23885', _structure(b, n, lambda i: b[i])))
    if size % 29120 == 0:
        a = np.memmap(path, '<u2', 'r'); n = size // 29120; b = a.reshape(n, 16, 910)
        # у 4fsc16 в начале каждой строки синхроимпульс (младшие 6 бит отсчётов — нули)
        low = float((np.asarray(b[:50]) & 63).mean() < 1)
        cand.append(('4fsc16', _structure(b, n, lambda i: b[i]) + low))
    if not cand:
        return None, []
    cand.sort(key=lambda c: -c[1])
    return cand[0][0], cand


def zc_coherence(y, fs, rate, lo, hi):
    """Согласованность переходов через ноль с тактом rate (0…1), переходы взвешены крутизной.
    Фон вычитается двумя способами (медиана и скользящее среднее ~8 бит), берётся лучший."""
    T = fs / rate
    s = y[lo:hi]
    w = max(int(T * 8), int(fs * 2e-6)) | 1
    best = 0.0
    for d in (s - np.median(s), s - np.convolve(s, np.ones(w) / w, 'same')):
        z = np.nonzero(np.sign(d[1:]) != np.sign(d[:-1]))[0]
        if len(z) < 12:
            continue
        t = z + d[z] / (d[z] - d[z + 1]); sl = np.abs(d[z + 1] - d[z])
        best = max(best, float(abs((sl * np.exp(2j * np.pi * t / T)).sum()) / (sl.sum() + 1e-9)))
    return best


def probe(path, units=240, log=print):
    fmt, cand = detect_format(path)
    if fmt is None:
        return {'file': os.path.abspath(path), 'format': None, 'lines': []}
    R = Rec(path, fmt)
    log('format: {0} ({1} {2}s, {3:.1f} MHz, {4} samples per line)'.format(fmt, R.n, R.unit, R.fs / 1e6, R.ns))
    us = np.linspace(0, R.n - 2, min(units, R.n - 1)).astype(int)
    lo, hi = int(R.ns * 0.1), int(R.ns * 0.97)
    if fmt == '4fsc16':
        lo = 140                                          # после синхроимпульса и вспышки
    allr = np.array([R.line(u, k)[lo:hi] for u in us[:8] for k in R.rows])
    span = np.percentile(allr, 99.5) - np.percentile(allr, 0.5)
    thr = max(0.05 * span, 3 * np.percentile(allr.std(1), 10))   # «есть сигнал»: заметно выше шума
    out = []
    keys = [(r, p) for r in R.rows for p in ((0, 1) if R.unit == 'field' else (None,))]
    for r, par in keys:
        uu = [u for u in us if par is None or u % 2 == par]
        Y = np.array([R.line(u, r) for u in uu]); Y2 = np.array([R.line(u + (2 if par is not None else 1), r) for u in uu
                                                                 if u + 2 < R.n] or [R.line(uu[0], r)])
        act_lvl = Y[:, lo:hi].std(1)
        act = act_lvl > thr
        sig = float(act.mean())
        za = Y[:len(Y2), lo:hi] - Y[:len(Y2), lo:hi].mean(1, keepdims=True)
        zb = Y2[:, lo:hi] - Y2[:, lo:hi].mean(1, keepdims=True)
        cc = (za * zb).sum(1) / np.sqrt((za ** 2).sum(1) * (zb ** 2).sum(1) + 1e-9)
        stat = float(np.median(cc[act[:len(cc)]])) if act[:len(cc)].any() else 0.0
        coh = {}
        if sig > 0.01:
            rows = Y[act][:40]
            for name, rate in RATES:
                coh[name] = float(np.mean([zc_coherence(y, R.fs, rate, lo, hi) for y in rows]))
        tvl = R.tvline(0 if par in (None, 0) else 1, r)
        kind, best = classify(sig, stat, coh, tvl)
        base_kind = kind
        if kind.startswith('test signal') or kind == 'empty':
            # редкие пачки данных поверх испытательного сигнала (WTTW: строка 20)
            big = Y[act_lvl > 2.0 * np.median(act_lvl)]
            if len(big) >= 6:
                c = float(np.mean([zc_coherence(y, R.fs, dict(RATES)['NABTS / WST NTSC rate'], lo, hi) for y in big[:30]]))
                if c > 0.2:
                    kind = 'NABTS / WST NTSC rate'; best = kind
                    sig = round(len(big) / len(Y), 3)
        if best:                                          # доля полей, где такт этой службы есть
            rt = dict(RATES)[best]; base = coh[best]
            sig = float(np.mean([zc_coherence(y, R.fs, rt, lo, hi) > 0.5 * base for y in Y[:160]]))
        out.append({'row': r, 'parity': par, 'tv_line': tvl, 'base_kind': base_kind, 'signal': round(sig, 3), 'static': round(stat, 3),
                    'rate': best, 'kind': kind, 'coherence': {k: round(v, 3) for k, v in coh.items()}})
    res = {'file': os.path.abspath(path), 'format': fmt, 'units': R.n, 'unit': R.unit,
           'candidates': cand, 'lines': out}
    confirm(R, res, log)
    return res


def classify(sig, stat, coh, tvl):
    if sig < 0.01:
        return 'empty', None
    if stat > 0.9:
        return 'test signal (VITS) or still picture', None
    if not coh:
        return 'unknown', None
    rate = dict(RATES)
    name = max(coh, key=coh.get)
    # сетка медленного такта входит в сетку быстрого, если быстрый — его кратное
    for k in sorted(coh, key=lambda k: rate[k]):
        q = rate[name] / rate[k]
        if rate[k] < rate[name] and abs(q - round(q)) < 0.02 and coh[k] >= 0.75 * coh[name]:
            name = k; break
    if coh[name] < 0.2:
        return 'picture or unknown signal', None
    if name == 'VPS' and tvl != 16:
        return 'data at {0:.2f} Mbit/s'.format(rate[name] / 1e6), name
    if name == 'WSS' and tvl not in (23, 336):
        return 'data at {0:.2f} Mbit/s'.format(rate[name] / 1e6), name
    return name, name


def confirm(R, res, log):
    """Пробное чтение там, где по такту похоже на известную службу."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    for L in res['lines']:
        k = L['kind']
        step = max(1, R.n // 400)
        if L['parity'] is not None: step += step % 2
        uu = list(range(L['parity'] or 0, R.n, step))[:300]
        if k == 'Silent Radio':
            import silent_radio as SR
            T = R.fs / SR.BITRATE
            ok = sum(SR.slice_line(R.line(u, L['row']), T) is not None for u in uu)
            L['read'] = round(ok / max(1, len(uu)), 3)
            if ok < 0.05 * len(uu): L['kind'] = 'data at 1.25 Mbit/s (not Silent Radio framing)'
        elif k == 'CC (line 21)':
            ok = sum(cc_slice(R.line(u, L['row']), R.fs) is not None for u in uu)
            L['read'] = round(ok / max(1, len(uu)), 3)
            if ok < 0.05 * len(uu): L['kind'] = 'data at 0.5 Mbit/s (not CC framing)'
        elif k == 'NABTS / WST NTSC rate':
            r = nabts_check(R, L['row'], uu[:60])
            L['read'] = r
            if r >= 0.3:
                L['kind'] = 'NABTS'
            else:
                r2 = datacast_check(R, L['row'], L['parity'])
                if r2 >= 0.15:
                    L['read'] = r2; L['kind'] = 'encrypted datacast (5.73 Mbit/s, PBS National Datacast?)'
                elif L['base_kind'] != k:                # пачки поверх испытательного сигнала не подтвердились
                    L['kind'] = L['base_kind']; L['read'] = None
                else:
                    L['kind'] = 'data at 5.73 Mbit/s (not NABTS; scrambled or unknown format)'
    for L in res['lines']:
        log('  row {0:2} line {1:3}{2}: {3:<55} signal {4:4.0%}{5}'.format(
            L['row'], L['tv_line'], '' if L['parity'] is None else ' field ' + 'AB'[L['parity']],
            L['kind'], L['signal'], '' if L.get('read') is None else '  read {0:.0%}'.format(L['read'])))


def cc_slice(y, fs):
    """EIA-608: 7 периодов вступления, 001, 2 байта (7 бит + нечётность, младший первым)."""
    T = fs / (32 * 15734.264)
    n = len(y)
    a, b = int(n * 0.12), int(n * 0.45)
    seg = y[a:b]; lo_, hi_ = np.percentile(seg, 5), np.percentile(seg, 95)
    if hi_ - lo_ < 10:
        return None
    th = (lo_ + hi_) / 2; d = y - th
    e = np.nonzero(np.sign(d[1:]) != np.sign(d[:-1]))[0]; e = e[(e >= a) & (e < b)]
    if len(e) < 8:
        return None
    t = e + d[e] / (d[e] - d[e + 1])
    k = np.round((t - t[0]) / (T / 2)); p = np.polyfit(k, t, 1)
    if abs(p[0] - T / 2) > 0.1 * T:
        return None
    last = p[1] + k.max() * p[0]                         # последний фронт вступления (спад)
    for sh in (0.5, 1.5):
        pos = last + sh * T + np.arange(19) * T
        if pos[-1] >= n - 1: return None
        b_ = (np.interp(pos, np.arange(n), y) > th).astype(int)
        if b_[0:3].tolist() == [0, 0, 1] or b_[1:3].tolist() == [0, 1]:
            s0 = 3 if b_[0:3].tolist() == [0, 0, 1] else 2
            pos = last + sh * T + (np.arange(16) + s0) * T
            bb = (np.interp(pos, np.arange(n), y) > th).astype(int)
            return [sum(int(bb[j * 8 + i]) << i for i in range(8)) for j in range(2)]
    return None


def datacast_check(R, row, par):
    """Доля строк с данными, где после слепого MLSE находится заголовок 55 55 2D (pages/datacast.py)."""
    try:
        import datacast as DC
    except Exception:
        return 0.0
    uu = DC.active_units(R, row, par)
    if len(uu) < 10:
        return 0.0
    L = DC.Line(R.fs, R.ns)
    C = DC.train(R, row, uu[::max(1, len(uu) // 40)][:40], L, log=lambda *a: None)
    test = uu[::max(1, len(uu) // 30)][:30]
    _ok = 0
    for u in test:
        Y = L.prep(R.line(u, row))
        if Y is None: continue
        b = DC.viterbi(Y, C)
        _ok += min(int((b[s:s + 24] != DC.HDR).sum()) for s in range(0, 30)) <= 3
    return round(_ok / len(test), 3)


def nabts_check(R, row, uu):
    """Доля строк с верным префиксом NABTS (декодер nabts_slicer на строке, пересчитанной в 27 МГц)."""
    try:
        import nabts_slicer as NS
    except Exception:
        return 0.0
    dec = NS.Decoder(); ok = 0; n = 0
    for u in uu:
        y = R.line(u, row)
        if y.std() < 5: continue
        t27 = np.arange(1440) / 27e6 * R.fs + int(R.ns * 0.1)
        y27 = np.interp(t27, np.arange(len(y)), y)
        d, _, _ = dec.decode(y27 * (255.0 / max(1, y27.max())), 0, 200, 1.0)
        ok += NS.good_prefix(d); n += 1
    return round(ok / max(1, n), 3)


def main():
    src = sys.argv[1]
    res = probe(src)
    if '--json' in sys.argv:
        json.dump(res, open(sys.argv[sys.argv.index('--json') + 1], 'w', encoding='utf-8'), indent=1)


if __name__ == '__main__':
    main()
