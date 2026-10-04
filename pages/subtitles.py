# Субтитры .srt со страницы телетекста — как в decode-orc
# (teletext_page_decoder.cpp: subtitle_page_completed / subtitle_clear_event /
#  extract_subtitle_text; teletext_sink_deps.cpp: format_srt), EN 300 706 §9.3.1.3:
#   * пакеты читаются по порядку передачи; страница считается переданной, когда
#     приходит следующий заголовок её магазина (в последовательном режиме C11 —
#     любой заголовок, §7.2.1);
#   * переданная страница с флагом C6 показывает текст: начинается титр (если текст
#     тот же, что на экране, — титр продолжается);
#   * заголовок этой страницы с C4 (стереть) или без C6 снимает титр;
#   * текст — только символы в рамках (0B…0A) на страницах C5/C6, без мозаики,
#     скрытых символов и байтов с ошибкой чётности; пробелы схлопываются.
# Время: номер поля пакета (по привязке пакета к строке VBI) / 50; если в записи
# bt8x8 есть счётчик кадров драйвера, кадр берётся по нему (потерянные кадры не
# сдвигают время). Без привязки к строкам — по номеру пакета и подгонке часов.
#   python subtitles.py [папка проекта] [--page 888] [--out файл.srt]
import os, sys, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import charsets as CS, tt_level1 as L1

_H = {}
for _d in range(16):
    _b = [(_d >> i) & 1 for i in range(4)]
    _p1 = 1 ^ _b[0] ^ _b[2] ^ _b[3]; _p2 = 1 ^ _b[0] ^ _b[1] ^ _b[3]; _p3 = 1 ^ _b[0] ^ _b[1] ^ _b[2]
    _p4 = 1 ^ (_p1 ^ _p2 ^ _p3 ^ _b[0] ^ _b[1] ^ _b[2] ^ _b[3])
    _H[sum(v << i for i, v in enumerate([_p1, _b[0], _p2, _b[1], _p3, _b[2], _p4, _b[3]]))] = _d
def ham(x):
    """Хэмминг 8/4 с исправлением одиночной ошибки -> 4 бита или None."""
    if x in _H: return _H[x]
    c = [w for w in _H if bin(w ^ x).count('1') == 1]
    return _H[c[0]] if len(c) == 1 else None

def packet_times(n, lines=None, vbi=None, lpf=32, clock=None):
    """Секунды записи для каждого из n пакетов."""
    if lines is not None and os.path.exists(lines):
        lp = np.load(lines); F, Ln = np.where(lp >= 0); k = lp[F, Ln]
        frame = F.astype(float)
        if vbi and os.path.exists(vbi):                    # счётчик кадров драйвера bt8x8
            v = np.memmap(vbi, np.uint8, 'r'); N = len(v) // (lpf * 2048)
            if N == lp.shape[0]:
                v = v[:N * lpf * 2048].reshape(N, lpf * 2048)
                cnt = np.frombuffer(np.ascontiguousarray(v[:, -4:]).tobytes(), '<u4').astype(np.int64)
                d = np.diff(cnt)
                if len(d) and (d >= 1).mean() > 0.99 and np.median(d) == 1:
                    frame = (cnt - cnt[0])[F].astype(float)
        fld = frame * 2 + (Ln >= lpf // 2)
        t = np.full(n, np.nan); t[k] = fld / 50.0
        last = 0.0                                         # пакеты без привязки — за предыдущим
        for i in range(n):
            if np.isnan(t[i]): t[i] = last
            else: last = t[i]
        return t
    b = clock[1] if clock else 1.0
    return np.arange(n) / 650.0 * b

def page_text(rows, t, t2, boxed_only):
    """Текст страницы для титра: ряды 1–24, '\n' между рядами (как extract_subtitle_text)."""
    cs, _, _ = L1.cells({str(r): v for r, v in rows.items() if r >= 1}, t, t2)
    grid = {}
    for c in cs:
        if c.mosaic or c.conceal or (boxed_only and not c.box): continue
        ch = c.text or ' '
        if ch.strip(): grid.setdefault(c.r, {})[c.c] = ch
    out = []
    for r in sorted(grid):
        s = ''; sp = True
        for col in range(40):
            ch = grid[r].get(col)
            if ch: s += ch; sp = False
            elif not sp: s += ' '; sp = True
        s = s.rstrip()
        if s: out.append(s)
    return '\n'.join(out)

def cues(t42, page, times, charset='latin', charset2=None):
    """-> [(начало, конец, текст)] для страницы page ('888')."""
    d = np.frombuffer(open(t42, 'rb').read(), np.uint8); n = len(d) // 42; d = d[:n * 42].reshape(n, 42)
    want_m = int(page[0]) % 8; want_p = int(page[1:], 16)
    t2 = CS.table(charset2, 0) if charset2 else None
    opened = {}                  # магазин -> dict(p, c4, c5, c6, nat, rows, last)
    out = []; cur = None         # cur: [начало, текст]
    def clear(at):
        nonlocal cur
        if cur and at > cur[0]: out.append((cur[0], at, cur[1]))
        cur = None
    def completed(pg):
        nonlocal cur
        if not pg['c6']: clear(pg['last']); return
        txt = page_text(pg['rows'], CS.table(charset, pg['nat']), t2, pg['c5'] or pg['c6'])
        if cur and cur[1] == txt: return
        clear(pg['last'])
        if txt: cur = [pg['last'], txt]
    def terminate(m):
        pg = opened.pop(m, None)
        if pg and pg['p'] == want_p and m == want_m: completed(pg)
    for i in range(n):
        p = d[i]; a, b = ham(int(p[0])), ham(int(p[1]))
        if a is None or b is None: continue
        m = a & 7; row = (a >> 3) | (b << 1); tm = float(times[i])
        if row == 0:
            h = [ham(int(x)) for x in p[2:10]]
            if None in h[:2]: continue
            pn = h[0] | (h[1] << 4)
            c4 = bool(h[3] is not None and h[3] & 8); c5 = bool(h[5] is not None and h[5] & 4)
            c6 = bool(h[5] is not None and h[5] & 8); serial = bool(h[7] is not None and h[7] & 1)
            for mm in (list(opened) if serial else [m]): terminate(mm)
            if m == want_m and pn == want_p and (c4 or not c6): clear(tm)
            if pn == 0xFF: continue
            opened[m] = dict(p=pn, c4=c4, c5=c5, c6=c6, nat=CS.national_of(h[7] or 0), rows={}, last=tm)
        elif row <= 24 and m in opened:
            pg = opened[m]; pg['last'] = tm
            pg['rows'][row] = [x & 0x7f if bin(int(x)).count('1') % 2 else 0x20 for x in p[2:42]]
    for m in list(opened): terminate(m)
    clear(float(times[-1]) if n else 0.0)
    return [c for c in out if c[1] > c[0]]

def srt_time(s):
    ms = int(round(s * 1000)); return f'{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}'

def to_srt(cs):
    return ''.join(f'{i}\n{srt_time(a)} --> {srt_time(b)}\n{t}\n\n' for i, (a, b, t) in enumerate(cs, 1))

def subtitle_pages(t42):
    """Страницы с флагом C6 в потоке: {'888': число заголовков} — кандидаты для субтитров."""
    d = np.frombuffer(open(t42, 'rb').read(), np.uint8); n = len(d) // 42; d = d[:n * 42].reshape(n, 42)
    res = {}
    for p in d:
        a, b = ham(int(p[0])), ham(int(p[1]))
        if a is None or b is None or (a >> 3) | (b << 1): continue
        h = [ham(int(x)) for x in p[2:8]]
        if None in h[:2] or h[5] is None or not h[5] & 8: continue
        pn = h[0] | (h[1] << 4)
        if pn == 0xFF or (pn & 0xF) > 9 or (pn >> 4) > 9: continue
        k = f'{(a & 7) or 8}{pn:02X}'; res[k] = res.get(k, 0) + 1
    return dict(sorted(res.items(), key=lambda x: -x[1]))

def export(page='888', out=None):
    """Субтитры текущего проекта tt_export -> (путь, число титров)."""
    import tt_export as X
    t42 = X.STREAM; lines = X.META.get('lines'); vbi = X.META.get('source'); lpf = X.META.get('lpf', 32)
    if vbi and not vbi.lower().endswith('.vbi'): vbi = None
    n = os.path.getsize(t42) // 42
    times = packet_times(n, lines, vbi, lpf, X.clock())
    cs = cues(t42, page, times, X.CHARSET, X.CHARSET2)
    out = out or os.path.join(X.OUTDIR, f'subtitles_{page}.srt')
    open(out, 'w', encoding='utf-8').write(to_srt(cs))
    return out, len(cs)

if __name__ == '__main__':
    import tt_export as X
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    get = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    if args and os.path.isdir(args[0]): X.set_project(args[0])
    page = get('--page')
    if not page:
        c = subtitle_pages(X.STREAM); print('pages with the C6 flag:', c or 'none')
        page = next(iter(c), '888')
    path, k = export(page, get('--out'))
    print('page {0}: {1} captions -> {2}'.format(page, k, path))
