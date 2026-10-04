"""Испытательные сигналы (VITS) записи: что это за сигнал и АЧХ тракта по multiburst.

Multiburst (NTC-7 / FCC): белый флаг и пачки синусоид 0,5; 1,25; 2,0; 3,0; 3,58; 4,1 МГц
(у PAL — 0,5; 1,0; 2,0; 4,0; 4,8; 5,8). Амплитуда каждой пачки относительно 0,5 МГц — это
частотная характеристика всей цепочки: передатчик, приёмник, видеомагнитофон, оцифровка.
Запись WTTW 1989 (VHS): 1,25 МГц −1 дБ, 2 МГц −6 дБ, 3 МГц и выше — нет.

  python pages/vits.py запись.vbi
"""
import os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vbi_probe as VP

MB_NTSC = [0.5, 1.25, 2.0, 3.0, 3.58, 4.1]
MB_PAL = [0.5, 1.0, 2.0, 4.0, 4.8, 5.8]


def median_line(R, row, par, n=150):
    uu = list(range(par or 0, R.n, 2 if par is not None else 1))
    uu = uu[::max(1, len(uu) // n)][:n]
    return np.median(np.array([R.line(u, row) for u in uu]), 0)


def bursts(m, fs):
    """Пачки синусоид в строке: (частота МГц, размах). Пачка — участок, где сигнал колеблется
    вокруг своего среднего не меньше трёх периодов."""
    n = len(m); w = max(3, int(fs * 0.6e-6)) | 1
    act = np.abs(np.convolve(np.abs(np.diff(m, prepend=m[0])), np.ones(w) / w, 'same'))
    thr = 0.15 * act[int(n * 0.12):].max()
    on = act > thr; on[:int(n * 0.12)] = False
    out = []; i = 0
    while i < n:
        if not on[i]: i += 1; continue
        j = i
        while j < n and on[j]: j += 1
        seg = m[i:j]
        if j - i > fs * 0.8e-6:
            c = seg - seg.mean()
            z = np.count_nonzero(np.sign(c[1:]) != np.sign(c[:-1]))
            cycles = z / 2.0
            if cycles >= 3:
                k0, k1 = int(len(seg) * 0.15), int(len(seg) * 0.85)
                out.append((round(cycles / ((j - i) / fs) / 1e6, 2), float(np.percentile(seg[k0:k1], 98) - np.percentile(seg[k0:k1], 2))))
        i = j
    return out


def multiburst(m, fs, freqs):
    """-> {стандартная частота: размах} или None, если пачек меньше трёх разных частот."""
    bs = bursts(m, fs)
    amp = {}
    for f, a in bs:
        k = min(freqs, key=lambda x: abs(x - f))
        if abs(k - f) < 0.25 * k:
            amp[k] = max(amp.get(k, 0), a)
    if len(amp) < 3 or freqs[0] not in amp:
        return None
    return {f: amp.get(f, 0.0) for f in freqs}


def analyse(path, log=print, res=None):
    fmt, _ = VP.detect_format(path)
    R = VP.Rec(path, fmt)
    res = res or VP.probe(path, log=lambda *a: None)
    freqs = MB_PAL if fmt == 'bt8x8-pal' else MB_NTSC
    out = []
    for L in res['lines']:
        if not L['kind'].startswith('test signal'):
            continue
        m = median_line(R, L['row'], L['parity'])
        mb = multiburst(m, R.fs, freqs)
        if mb:
            ref = mb[freqs[0]] or 1
            resp = [(f, round(20 * np.log10(max(mb[f], 1e-6) / ref), 1)) for f in freqs]
            out.append({'tv_line': L['tv_line'], 'parity': L['parity'], 'kind': 'multiburst', 'response_db': resp})
    for o in out:
        log('multiburst on line {0}{1}: '.format(o['tv_line'], '' if o['parity'] is None else ' field ' + 'AB'[o['parity']])
            + ', '.join('{0} MHz {1:+.1f} dB'.format(f, db) for f, db in o['response_db']))
    return out


def response_text(out):
    if not out:
        return []
    o = out[0]
    t = ['Frequency response of the recording (multiburst test signal, line {0}):'.format(o['tv_line'])]
    t.append('  ' + ', '.join('{0} MHz {1:+.1f} dB'.format(f, db) if db > -30 else '{0} MHz —'.format(f) for f, db in o['response_db']))
    cut = [f for f, db in o['response_db'] if db < -20]
    if cut:
        t.append('  (nothing above ~{0} MHz passes — typical for VHS luma)'.format(min(cut)))
    return t


if __name__ == '__main__':
    analyse(sys.argv[1])
