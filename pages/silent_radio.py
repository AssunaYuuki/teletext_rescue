"""Silent Radio (WTTW Chicago, 1989): бегущая строка для светодиодных табло в строке 21.

Формат найден по записи (описаний в сети нет):
  строка 21, сигнал как у субтитров CC, но 7/20 fsc = 1,2528 Мбит/с:
  вступление 1010101010, 000, 11, затем 16 бит = 2 байта на поле, старший бит первым.
Поток байтов — пакеты:
  FF  номер(2)  длина(2, старший байт первым)  00 03  КС(2)  C1 зона(4)  тело…
  (длина считается от байта после «00 03» до конца пакета без 2 байт; зоны C1 02 03 / 04 05 / 06 07
  — три программы для разных групп табло).
Тело — записи «00 <тип> …»:
  00 0C <длина LE> <скрипт>        — эффект: растр, текст, паузы, позиции;
  00 <шаблон> 02 стр1 03 стр2 … 1F — текст в шаблон прошивки табло (1B — бегущая строка,
                                     30/31 — строка счёта: 02-04 лига, 05 гости, 06 хозяева,
                                     07 08 счёт, 09 состояние).
Табло — 112×15 точек (две строки по 7 точек). Растр в скрипте — слова по 2 байта со старшим
битом 1: младшие 15 бит — столбец, бит 0 — верхняя точка.
Команды скрипта (что понятно): 1C lo hi — пауза lo+256·hi /256 с; 1D x и 0A x — позиция;
1F ',' — новый кадр; 1F <другой> — показать кадр (переход); 05 + 6 байт, 06 x, 07 x, 1B x — параметры
(пропускаются). Шрифт текста табло не передаётся — здесь обычный 5×7.

  python pages/silent_radio.py запись.vbi [папка]   (любой формат, что знает vbi_probe.py)
Вывод: папка «<запись>_silentradio»: index.html (проигрыватель табло), text.txt, packets.json.
"""
import json, os, re, sys
import numpy as np

W, H = 112, 15
FSC = 315e6 / 88
BITRATE = FSC * 7 / 20
PLAYER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'silent_radio_player.js')


def log(*a):
    print(*a, flush=True)


# ------------------------------------------------------------------ запись -> байты
def slice_line(y, T):
    """Строка -> 16 бит данных или None. y — отсчёты, T — отсчётов на бит."""
    n = len(y)
    a, b = int(n * 0.15), int(n * 0.29)                 # вступление: ~10–18 мкс от синхроимпульса
    seg = y[a:b]
    lo, hi = np.percentile(seg, 10), np.percentile(seg, 90)
    if hi - lo < 0.15 * max(hi, 1):
        return None
    th = (lo + hi) / 2
    d = y - th
    e = np.nonzero(np.sign(d[1:]) != np.sign(d[:-1]))[0]
    e = e[(e >= a) & (e < b)]
    if len(e) < 6:
        return None
    t = e + d[e] / (d[e] - d[e + 1])
    k = np.round((t - t[0]) / T)
    p = np.polyfit(k, t, 1)
    if abs(p[0] - T) > 0.08 * T:
        return None
    pos = p[1] + T / 2 + np.arange(31) * p[0]           # первый фронт — подъём первого бита «1»
    if pos[-1] >= n - 1:
        return None
    bits = (np.interp(pos, np.arange(n), y) > th).astype(np.uint8)
    if bits[0:10].tolist() != [1, 0] * 5 or bits[10:15].tolist() != [0, 0, 0, 1, 1]:
        return None
    return bits[15:31]


def find_line(R, T):
    """-> (строка записи, чётность поля или None) с наибольшей долей читаемых строк."""
    best = None
    pars = (0, 1) if R.unit == 'field' else (None,)
    for r in R.rows:
        for par in pars:
            uu = [u for u in range(par or 0, min(R.n, 4000), 74 if par is not None else 37)]
            ok = sum(slice_line(R.line(u, r), T) is not None for u in uu)
            if best is None or ok > best[0]:
                best = (ok, r, par)
    return (best[1], best[2]) if best and best[0] > 5 else (None, None)


def read_bytes(path, progress=None, row=None, parity=None):
    import vbi_probe as VP
    fmt, _ = VP.detect_format(path)
    if fmt is None:
        raise ValueError('unknown recording format')
    R = VP.Rec(path, fmt)
    T = R.fs / BITRATE
    if row is None:
        row, parity = find_line(R, T)
    if row is None:
        raise ValueError('no Silent Radio signal (line-21 style data at 1.25 Mbit/s) found')
    log('Silent Radio data on line {0} (record row {1}), {2} {3}s'.format(R.tv[row], row, R.n, R.unit))
    out = bytearray(); good = bad = 0
    for u in range(parity or 0, R.n, 2 if parity is not None else 1):
        b = slice_line(R.line(u, row), T)
        if b is None:
            bad += 1
            continue
        good += 1
        out += bytes(np.packbits(b))
        if progress and u % 2000 < 2:
            progress(u, R.n)
    log('{0}s with data {1}, without {2}; {3} bytes'.format(R.unit, good, bad, len(out)))
    return bytes(out), R.tv[row]


# ------------------------------------------------------------------ пакеты
HDR = re.compile(rb'\xff(..)(..)\x00\x03(..)\xc1(...)', re.S)
ZONES = {b'\x02\x03': 'A', b'\x04\x05': 'B', b'\x06\x07': 'C'}


def packets(data):
    """-> список пакетов по порядку первого появления (копии сведены голосованием по байтам)."""
    raw = []
    for m in HDR.finditer(data):
        p = m.start()
        L = data[p + 3] << 8 | data[p + 4]
        if L < 8 or p + 13 + L > len(data):
            continue
        raw.append((p, data[p:p + 13 + L]))
    groups = {}
    order = []
    for p, q in raw:
        if q[12] < 0x80:                                 # служебные пакеты (C1 xx xx 4F…)
            continue
        key = (q[1:5], q[10:12])
        if key not in groups:
            groups[key] = []; order.append(key)
        groups[key].append(q)
    out = []
    for key in order:
        cps = groups[key]
        whole = [c for c in cps if c[-6:] == bytes(6)]  # копия цела, если конец (6 нулей) на месте
        cps = whole or cps
        n = min(len(c) for c in cps)
        arr = np.frombuffer(b''.join(c[:n] for c in cps), np.uint8).reshape(len(cps), n)
        voted = bytes(int(np.bincount(arr[:, i]).argmax()) for i in range(n))
        out.append({'seq': voted[1] << 8 | voted[2], 'zone': ZONES.get(voted[10:12], voted[10:12].hex()),
                    'copies': len(groups[key]), 'intact': len(whole),
                    'body': voted[16:-2]})
    return out


def records(body):
    """Тело пакета -> записи [('script', bytes) | ('tpl', тип, {поле: текст}) | ('ctl', bytes)]."""
    out = []; i = 0; n = len(body)
    while i < n:
        if body[i] != 0:
            i += 1; continue
        if i + 1 >= n:
            break
        t = body[i + 1]
        if t == 0x0C and i + 3 < n:
            L = body[i + 2] | body[i + 3] << 8
            out.append(('script', body[i + 4:i + 4 + L])); i += 4 + L; continue
        if t == 0x0A:
            out.append(('ctl', body[i:i + 5])); i += 5; continue
        if t == 0:
            i += 1; continue
        j = i + 2; fields = {}; cur = None; buf = ''
        while j < n and body[j] != 0x1F:
            c = body[j]
            if 2 <= c <= 9:
                if cur is not None: fields[cur] = buf
                cur, buf = c, ''
            elif 32 <= c < 127:
                buf += chr(c)
            j += 1
        if cur is not None: fields[cur] = buf
        out.append(('tpl', t, fields)); i = j + 1
    return out


# ------------------------------------------------------------------ шрифт 5×7 (столбцы, бит 0 — верх)
_F = bytes.fromhex(
    '0000000000' '00005f0000' '0007000700' '147f147f14' '242a7f2a12' '2313086462' '3649562050' '0008070300'
    '001c224100' '0041221c00' '2a1c7f1c2a' '08083e0808' '0080703000' '0808080808' '0000606000' '2010080402'
    '3e5149453e' '00427f4000' '7249494946' '2141494d33' '1814127f10' '2745454539' '3c4a494931' '4121110907'
    '3649494936' '464949291e' '0000140000' '0040340000' '0008142241' '1414141414' '0041221408' '0201590906'
    '3e415d594e' '7c1211127c' '7f49494936' '3e41414122' '7f4141413e' '7f49494941' '7f09090901' '3e41415173'
    '7f0808087f' '00417f4100' '2040413f01' '7f08142241' '7f40404040' '7f021c027f' '7f0408107f' '3e4141413e'
    '7f09090906' '3e4151215e' '7f09192946' '2649494932' '03017f0103' '3f4040403f' '1f2040201f' '3f4038403f'
    '6314081463' '0304780403' '61594d4543' '007f414141' '0204081020' '004141417f' '0402010204' '4040404040'
    '0003070800' '2054547840' '7f28444438' '3844444428' '384444287f' '3854545418' '00087e0902' '18a4a49c78'
    '7f08040478' '00447d4000' '2040403d00' '7f10284400' '00417f4000' '7c0478047c' '7c08040478' '3844444438'
    'fc18242418' '18242418fc' '7c08040408' '4854545424' '04043f4424' '3c4040207c' '1c2040201c' '3c4030403c'
    '4428102844' '4c9090907c' '4464544c44' '0008364100' '0000770000' '0041360800' '0201020402')


def glyph(ch):
    c = ord(ch)
    if not 32 <= c < 127:
        c = 32
    g = list(_F[(c - 32) * 5:(c - 32) * 5 + 5])
    if c == 32:
        return [0, 0]
    while g and g[0] == 0: g.pop(0)
    while g and g[-1] == 0: g.pop()
    return [x & 0x7F for x in g]


def text_cols(s):
    cols = []
    for ch in s:
        cols += glyph(ch) + [0]
    return cols[:-1] if cols else cols


# ------------------------------------------------------------------ интерпретатор -> кадры
class Screen:
    def __init__(self):
        self.cols = [0] * W

    def put(self, x, col):
        if 0 <= x < W:
            self.cols[x] |= col & 0x7FFF

    def text(self, x, s, row):
        for c in text_cols(s):
            self.put(x, c << row); x += 1
        return x


def frame(cols, hold, what=''):
    return {'f': [int(c) for c in cols], 't': round(max(hold, 0.04), 3), 'w': what}


def run_script(s):
    """Скрипт эффекта -> кадры. Неизвестные команды пропускаются.
    Кадр показывается командой 1F <код> (код ' ' — сразу, другие — переход, здесь вытеснение);
    выдержка — следующая пауза 1C, без паузы — HOLD с."""
    HOLD = 2.0
    out = []; sc = Screen(); x = 0; saved = 0; dirty = False; i = 0; n = len(s)
    last = None

    def show(fx):
        nonlocal dirty, last
        out.append(frame(sc.cols, 0)); out[-1]['t'] = None
        if fx != 0x20: out[-1]['fx'] = 'wipe'
        last = out[-1]; dirty = False

    while i < n:
        c = s[i]
        if c >= 0x80 and i + 1 < n:                      # столбец растра
            sc.put(x, ((c & 0x7F) << 8) | s[i + 1]); x += 1; i += 2; dirty = True; continue
        if 32 <= c < 127:
            j = i
            while j < n and 32 <= s[j] < 127: j += 1
            x = sc.text(x, s[i:j].decode('latin-1'), 4); i = j; dirty = True; continue
        if c == 0x1C and i + 2 < n:                       # пауза, 1/256 с
            hold = (s[i + 1] | s[i + 2] << 8) / 256.0
            if dirty or last is None:
                show(0x20)
            last['t'] = round((last['t'] or 0) + hold, 3)
            i += 3; continue
        if c in (0x1D, 0x0A) and i + 1 < n:               # позиция
            x = s[i + 1]; saved = x if c == 0x1D else saved; i += 2; continue
        if c == 0x11:
            x = saved; i += 1; continue
        if c == 0x1F and i + 1 < n:
            a = s[i + 1]
            if a == 0x2C:                                 # новый кадр
                sc = Screen(); dirty = False
            elif a in (0x32, 0x38):                       # часы табло: время подставляет само табло
                x = sc.text(x, '12' if a == 0x32 else '00', 4); dirty = True
            elif a == 0x33:
                x = sc.text(x + 2, 'PM', 4); dirty = True
            elif a not in (0x3F, 0x39) and dirty:         # показать
                show(a)
            i += 2; continue
        if c == 0x05:
            i += 7; continue
        if c in (0x06, 0x07, 0x1B):
            i += 2; continue
        i += 1
    if dirty:
        show(0x20)
    for f in out:
        if f['t'] is None: f['t'] = HOLD
    return out


def tpl_frames(t, f, league):
    """Шаблон прошивки -> кадры (как показал бы табло; эффекты переходов неизвестны)."""
    if t == 0x1B:                                         # бегущая строка
        s = f.get(2, '')
        return [{'scroll': text_cols('   ' + s), 'row': 4, 'speed': 40, 'w': s}], league
    if t in (0x30, 0x31):
        lg = f.get(2) or f.get(3) or f.get(4) or league
        l1 = '{0} {1}  {2} {3}'.format(f.get(5, ''), f.get(7, ''), f.get(6, ''), f.get(8, ''))
        l2 = '{0}  {1}'.format(lg, f.get(9, ''))
        return [two_lines(l1, l2, 3.0)], lg
    l1, l2 = f.get(2, ''), f.get(3)
    return [two_lines(l1, l2, 3.0)], league


def two_lines(l1, l2, hold):
    rows = [(l1, 0), (l2, 8)] if l2 is not None else [(l1, 4)]
    wide = max(len(text_cols(s)) for s, _ in rows)
    if wide > W:                                          # не помещается — едет
        lines = [{'cols': text_cols(s), 'row': r} for s, r in rows]
        return {'scroll2': lines, 'speed': 40, 'w': ' / '.join(s for s, _ in rows)}
    sc = Screen()
    for s, r in rows:
        c = text_cols(s); x0 = (W - len(c)) // 2
        for k, v in enumerate(c): sc.put(x0 + k, v << r)
    return frame(sc.cols, hold, ' / '.join(s for s, _ in rows))


def build(pk):
    """Пакеты -> программы по зонам: [{'seq','title','frames'}]."""
    progs = {}
    for p in pk:
        if len(p['body']) < 4:
            continue
        frames = []; texts = []; league = ''
        for r in records(p['body']):
            if r[0] == 'script':
                frames += run_script(r[1])
            elif r[0] == 'tpl':
                fr, league = tpl_frames(r[1], r[2], league); frames += fr
                texts.append(' / '.join(v for k, v in sorted(r[2].items()) if v))
        if not frames:
            continue
        title = texts[0] if texts else 'graphics'
        progs.setdefault(p['zone'], []).append({'seq': '%04X' % p['seq'], 'title': title[:60],
                                                'text': texts, 'frames': frames, 'copies': p['copies']})
    return progs


def export(src, out=None, progress=None):
    out = out or os.path.splitext(src)[0] + '_silentradio'
    data, line = read_bytes(src, progress)
    return save(src, out, data, line)


def save(src, out, data, line):
    """Байты строки -> папка out: index.html (табло), text.txt, packets.json, line21_bytes.bin."""
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'line21_bytes.bin'), 'wb').write(data)
    pk = packets(data)
    log('packets: {0} distinct ({1} with copies)'.format(len(pk), sum(p['copies'] > 1 for p in pk)))
    progs = build(pk)
    with open(os.path.join(out, 'text.txt'), 'w', encoding='utf-8') as fh:
        for z in sorted(progs):
            fh.write('===== zone {0}\n'.format(z))
            for it in progs[z]:
                for t in it['text']:
                    fh.write(t + '\n')
                fh.write('\n')
    json.dump({'source': os.path.basename(src), 'line': line, 'zones': progs},
              open(os.path.join(out, 'packets.json'), 'w', encoding='utf-8'))
    html = PAGE.replace('/*DATA*/', json.dumps({'source': os.path.basename(src), 'line': line, 'zones': progs})) \
               .replace('/*PLAYER*/', open(PLAYER, encoding='utf-8').read())
    open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(html)
    log('written to ' + out)
    return out


PAGE = '''<!doctype html><html><head><meta charset="utf-8"><title>Silent Radio</title>
<style>
body{background:#111;color:#ccc;font:14px sans-serif;margin:16px}
#sign{background:#000;border:10px solid #222;border-radius:6px;display:block;margin:8px 0;max-width:100%}
#list{max-height:45vh;overflow:auto;border:1px solid #333;margin-top:8px}
#list div{padding:3px 6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#list div.on{background:#523;color:#fff}
button,select{background:#222;color:#ddd;border:1px solid #444;padding:3px 10px}
#cap{color:#fa4;min-height:1.3em}
</style></head><body>
<div>Silent Radio — <span id="src"></span> · zone <select id="zone"></select>
<button id="play">Pause</button> <button id="prev">&lt;</button> <button id="next">&gt;</button>
<label><input type="checkbox" id="loop" checked> loop</label></div>
<canvas id="sign"></canvas><div id="cap"></div><div id="list"></div>
<script>var DATA=/*DATA*/;</script><script>/*PLAYER*/</script></body></html>'''


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else None
    export(src, out)


if __name__ == '__main__':
    main()
