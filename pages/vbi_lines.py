# Обзор записи VBI: что передаётся в каждой строке кадра.
#   python vbi_lines.py запись.vbi [ещё.vbi ...] [--frames 300] [--json out.json]
# Форматы и номера строк — как в decode-orc (orc/plugins/stages/vbi_source/
# vbi_source_format.cpp, instructions.md):
#   bt8x8 PAL/SECAM — 35 468 950 Гц, 2044 отсчёта, 16 записей на поле = строки 7–22 и 320–335;
#   bt8x8 NTSC      — 28 636 363 Гц, 1600 отсчётов (дальше нули), записи 0–11 = строки
#                     10–21 и 273–284, записи 12–15 — уже начало картинки.
#   Последние 4 байта кадра — номер кадра драйвера (по нему видны потерянные кадры).
#   SECAM: опознавание цвета («бутылки», ~4,4 МГц) на строках 8–15 и 321–328.
# Для каждой записи по выборке кадров:
#   сигнал  — доля кадров с данными в строке;
#   статика — сходство строки в соседних кадрах (испытательные сигналы ≈1);
#   бит     — длительность бита в отсчётах по вступлению 1010… (телетекст PAL ≈5,11,
#             NTSC при 28,64 МГц ≈5,00);
#   МГц     — средняя частота спектра строки;
#   пакеты  — доля строк с сигналом, из которых декодер (decode_vbi.Reader: шаблоны на
#             видеокарте + декодер decode-orc) прочитал пакет телетекста с верным адресом;
#             дальше — магазины и виды пакетов (заголовки X/0, ряды 1–25, X/26–27, X/28–31).
#   --no-decode — без декодирования (только физические признаки).
import collections, json, os, sys
import numpy as np
SPL, LPF = 2048, 32
FS_PAL, FS_NTSC = 35468950.0, 28636363.0
arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
NFR = int(arg('--frames', 300))

def frame_counter(v):
    """Номера кадров драйвера (uint32 в конце кадра) по выборке кадров."""
    idx = np.linspace(0, len(v) - 1, min(len(v), 5000)).astype(int)
    return idx, np.array([int.from_bytes(bytes(v[i, -1, -4:]), 'little') for i in idx], np.int64)

def bit_period(y):
    """Длительность бита по вступлению 1010…: автокорреляция начала активной части."""
    e = np.convolve(np.abs(np.diff(y)), np.ones(16) / 16, 'same')
    if e.max() < 3: return None
    s = int(np.argmax(e > 0.4 * e.max()))
    seg = y[s:s + 110] - y[s:s + 110].mean()
    if len(seg) < 110: return None
    ac = np.correlate(seg, seg, 'full')[len(seg) - 1:]; ac /= ac[0] or 1
    lag = 7 + int(np.argmax(ac[7:24]))                   # период вступления = 2 бита
    if ac[lag] < 0.3: return None
    a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]; den = a - 2 * b + c
    return (lag + (0.5 * (a - c) / den if den else 0)) / 2

def vits_kind(m):
    """Вид испытательного сигнала по средней форме строки m (ITU-R BT.473 / CCIR)."""
    n = len(m); mid = m[int(n * .3):int(n * .6)]; tail = m[int(n * .55):int(n * .9)]
    hf = np.abs(np.diff(mid)).mean()
    steps = np.abs(np.diff(np.convolve(tail, np.ones(15) / 15, 'valid'))) > 1.0
    if hf > 2.5: return 'multiburst (sine packets, as CCIR 18)'
    if tail[-1] - tail[0] > 30 and steps.mean() < 0.3: return 'bar, 2T/20T pulses and staircase (as CCIR 17/330)'
    return 'white bar / reference level (as CCIR 331)'

def kind(sig, stat, bit, mhz, ntsc, tvline, fs, mean=None):
    if ntsc and 22 <= tvline <= 25 or ntsc and 285 <= tvline <= 288:
        return 'start of the picture (not VBI)' if sig > 0.02 else 'empty'
    if sig < 0.02: return 'empty'
    tt = fs / (5727272.0 if ntsc else 6937500.0)
    if not ntsc and stat > 0.6 and 3.9 < mhz < 4.9: return 'SECAM colour identification (“bottles”)'
    if stat > 0.9: return 'test signal: ' + (vits_kind(mean) if mean is not None else 'static pattern')
    if bit and abs(bit - tt) < 0.25:
        return 'teletext ' + ('NTSC (WST/NABTS, 5.73 Mbit/s)' if ntsc else 'PAL/SECAM (6.94 Mbit/s)')
    if not ntsc and tvline == 16: return 'VPS (programme label)?'
    if ntsc and tvline in (21, 284) and mhz < 1.5: return 'CC captions (line 21)?'
    return 'data of unknown kind'

def decode_stats(path, frames):
    """Декодирование выборки кадров -> {запись: (доля пакетов, магазины, виды пакетов)}."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import decode_vbi as D
    R = D.Reader(path, LPF, log=lambda t: None)
    cand = R.sample(800, 2500)
    if not cand or R.prepare(cand, True, lambda t: None) is None: return None, R
    res = R.decode_all(lambda t: None, frames)
    st = collections.defaultdict(lambda: [0, 0, collections.Counter(), collections.Counter()])
    for (f, l), b in res.items():
        b = np.frombuffer(b, np.uint8); a, c = D.ham_fix(int(b[0])), D.ham_fix(int(b[1]))
        e = st[l]; e[0] += 1
        if a is None or c is None or D.par_bad(b) > 10: continue
        a, c = D.H8[a], D.H8[c]; e[1] += 1; e[2][a & 7 or 8] += 1; r = (a >> 3) | (c << 1)
        e[3]['X/0' if r == 0 else '1–25' if r <= 25 else 'X/26–27' if r <= 27 else 'X/28–31'] += 1
    return st, R

def survey(path):
    v = np.memmap(path, dtype=np.uint8, mode='r')
    n = len(v) // (LPF * SPL); v = v[:n * LPF * SPL].reshape(n, LPF, SPL)
    smp = np.asarray(v[np.linspace(0, n - 1, 50).astype(int)])
    ntsc = bool((smp[:, :-1, 1604:2040] == 0).mean() > 0.99)
    fs = FS_NTSC if ntsc else FS_PAL; fps = 30000 / 1001 if ntsc else 25
    first = (10, 273) if ntsc else (7, 320); half = LPF // 2; valid = 1600 if ntsc else 2044
    print(f'\n=== {path}')
    print('{0} frames = {1:.1f} min; system: {2}'.format(n, n / fps / 60, "NTSC (bt8x8 28.64 MHz, 1600 samples)" if ntsc else "PAL/SECAM (bt8x8 35.47 MHz)"))
    idx, c = frame_counter(v)
    if np.all(np.diff(c) > 0):
        span = int(c[-1] - c[0]) + 1
        print('driver frame counter {0}…{1}: recorded {2} of {3} — '.format(c[0], c[-1], n, span)
              + ('lost {0:.0%}'.format(1 - n / span) if n < 0.995 * span else 'no losses'))
    else:
        print('the frame counter is not monotonic — losses cannot be determined from it')
    fr = np.linspace(0, n - 2, min(NFR, n - 1)).astype(int)
    A = np.asarray(v[fr, :, :valid], np.float32); B = np.asarray(v[fr + 1, :, :valid], np.float32)
    rows = []
    dec = None
    if '--no-decode' not in sys.argv and not ntsc:
        dec, R = decode_stats(path, sorted(set(fr.tolist())))
        if dec is None: print('the teletext decoder cannot read this recording')
        else: print('decoder: {0}, packet start window {1:.0f}–{2:.0f}'.format("GPU" if R.gpu else "CPU", R.win[0], R.win[1])
                    + (', with the rest read by the decode-orc decoder' if R.fallback else ''))
    print('{0:>4} {1:>7} {2:>7} {3:>8} {4:>5} {5:>7}  what is carried'.format("rec", "TV line", "signal", "static", "MHz", "packets"))
    for l in range(LPF):
        y = A[:, l]; act = y.std(1) > 20; sig = float(act.mean())
        za = y - y.mean(1, keepdims=True); zb = B[:, l] - B[:, l].mean(1, keepdims=True)
        cc = (za * zb).sum(1) / np.sqrt((za ** 2).sum(1) * (zb ** 2).sum(1) + 1e-9)
        stat = float(np.median(cc[act])) if act.any() else 0.0
        bits = [b for b in (bit_period(y[i]) for i in np.where(act)[0][:60]) if b]
        bit = float(np.median(bits)) if len(bits) >= 3 else None
        P = (np.abs(np.fft.rfft(za[act] if act.any() else za, axis=1)) ** 2).mean(0); P[:3] = 0
        fq = np.fft.rfftfreq(valid, 1 / fs) / 1e6; mhz = float((P * fq).sum() / (P.sum() or 1))
        tvline = first[l // half] + l % half
        k = kind(sig, stat, bit, mhz, ntsc, tvline, fs, y[act].mean(0) if act.any() else None)
        pk = None; extra = ''
        if dec is not None and l in dec and dec[l][0]:
            n, ok, mags, kinds = dec[l]; pk = ok / n
            if pk >= 0.3 and stat < 0.9:
                k = 'teletext'
                tot = sum(kinds.values())
                extra = (' · magazines ' + ' '.join(f'{m}' for m, c in sorted(mags.items()) if c >= 0.03 * ok) +
                         ' · ' + ', '.join(f'{t} {c / tot:.0%}' for t, c in sorted(kinds.items()) if c / tot >= 0.005))
            elif k.startswith('teletext') or k == 'data of unknown kind':
                k = 'data, but not readable teletext' + (' (VPS?)' if tvline == 16 else '')
        rows.append(dict(record=l, tv_line=tvline, signal=round(sig, 3), static=round(stat, 3),
                         mhz=round(mhz, 2), packets=round(pk, 3) if pk is not None else None, kind=k + extra))
        print(f'{l:4} {tvline:7} {sig:7.0%} {stat:8.2f} {mhz:5.2f} {f"{pk:.0%}" if pk is not None else "—":>7}  {k}{extra}')
    return dict(file=os.path.abspath(path), frames=n, system='NTSC' if ntsc else 'PAL/SECAM', lines=rows)

if __name__ == '__main__':
    skip = {arg('--frames'), arg('--json')}
    res = [survey(f) for f in sys.argv[1:] if not f.startswith('--') and f not in skip]
    if arg('--json'): json.dump(res, open(arg('--json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
