# Собирает проект страниц из любого потока .t42 или .t34:
#   python build_project.py --t42 поток.t42 --out папка [--lines line_pkt.npy --lpf 32 --vbi запись.vbi]
# В папке появятся pages.json, extras.json (кнопки, флаги, часы, статистика),
# project.json и, если дана запись VBI, quality.json.
# Пропуск полей VBI (как у кассеты LP, проигранной в SP) определяется
# автоматически по разрывам рядов на границах полей; для обычной записи
# страницы собираются без учёта полей.
# .t34 — поток 525-строчного WST (NTSC, например TBS Electra): пакеты по 34 байта
# (ITU-R BT.653 Table 1b, как пишет decode-orc): ряд — 32 символа, у заголовка 24.
# Переводится в .t42 (ряды дополняются пробелами до 40 символов) — stream.t42 в папке
# проекта; дальше сборка обычная.
# Строки "STEP ..." — для индикатора хода в программе.
import json, os, subprocess, sys, tempfile
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
T42 = os.path.abspath(arg('--t42')); OUT = os.path.abspath(arg('--out'))
SOURCE = T42
LINES = arg('--lines'); LPF = int(arg('--lpf', 32)); VBI = arg('--vbi')
os.makedirs(OUT, exist_ok=True)
def step(t): print('STEP', t, flush=True)
def run(*a): subprocess.run([sys.executable, *a], check=True)

H8 = {}
for dd in range(16):
    b = [(dd >> i) & 1 for i in range(4)]
    p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
    p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
    H8[sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))] = dd

def field_skipping(t42, lines, lpf):
    """Доля разрывов последовательности рядов на границах полей и внутри поля."""
    st = np.frombuffer(open(t42, 'rb').read(), dtype=np.uint8).reshape(-1, 42)
    lp = np.load(lines); half = lpf // 2
    last = {}; cnt = {'in': [0, 0], 'edge': [0, 0]}
    for f in range(lp.shape[0]):
        for l in range(lpf):
            k = lp[f, l]
            if k < 0: continue
            a, b = H8.get(int(st[k][0])), H8.get(int(st[k][1]))
            if a is None or b is None: continue
            m = a & 7; r = (a >> 3) | (b << 1)
            if r > 24: continue
            if m in last and r != 0:
                pr, pf = last[m]; kind = 'in' if pf == (f, l // half) else 'edge'
                cnt[kind][0] += 1; cnt[kind][1] += (r != pr + 1 and pr != 0) or (pr == 0 and r > 3)
            last[m] = (r, (f, l // half))
    rate = {k: (v[1] / v[0] if v[0] else 0) for k, v in cnt.items()}
    return rate['edge'] > 2 * max(rate['in'], 0.01), rate

def t34_to_t42(src, dst):
    """34-байтные пакеты -> 42-байтные. Как в decode-orc (teletext_page_decoder.cpp,
    «row-extension packets»): столбцы 32–39 рядов передаются в магазине M|4 пакетами
    с номерами 1, 4, 8, … 20 — по 8 символов для четырёх рядов подряд (блок
    номер//4*4). Магазин 4–7 считается носителем продолжений, если в нём нет ни одного
    заголовка страницы. Пакеты страницы копятся до конца её передачи (следующий
    заголовок магазина), продолжения вписываются в её ряды; чего не пришло — пробелы."""
    d = np.fromfile(src, np.uint8); n = len(d) // 34; P = d[:n * 34].reshape(n, 34)
    def addr(p):
        a, b = H8.get(int(p[0])), H8.get(int(p[1]))
        return (None, None) if a is None or b is None else (a & 7, (a >> 3) | (b << 1))
    hdr = [0] * 8; blk = [0] * 8
    for p in P:
        m, r = addr(p)
        if m is None: continue
        if r == 0 and H8.get(int(p[2])) is not None and H8.get(int(p[3])) is not None                 and (H8[int(p[3])] << 4 | H8[int(p[2])]) != 0xFF: hdr[m] += 1
        elif r in (1, 4, 8, 12, 16, 20): blk[m] += 1
    carrier = {m for m in range(4, 8) if hdr[m] == 0 and blk[m] >= 6}
    odd = lambda x: bin(int(x)).count('1') % 2 == 1
    out = []; page = {}                                  # магазин -> [пакеты 42 байта]
    def close(m):
        for q in page.pop(m, []): out.append(q)
    ext = 0
    for p in P:
        m, r = addr(p)
        q = np.full(42, 0x20, np.uint8); q[:34] = p
        if m is None: out.append(q); continue
        if m in carrier:
            tgt = page.get(m & 3)
            if not tgt: continue
            first = r // 4 * 4
            for g in range(4):
                for pk in tgt:
                    mm, rr = addr(pk)
                    if rr != first + g: continue
                    for k in range(8):
                        c = p[2 + 8 * g + k]; cur = pk[34 + k]
                        if odd(c) and (cur == 0x20 or not odd(cur)): pk[34 + k] = c
            ext += 1; continue
        if r == 0: close(m); page[m] = [q]
        elif m in page and 1 <= r <= 23: page[m].append(q)
        else: out.append(q)
    for m in list(page): close(m)
    np.array(out, np.uint8).tofile(dst)
    print('row-continuation carriers: magazines {0}; continuations {1}'.format(sorted(carrier) or "none", ext), flush=True)
    return len(out)
if T42.lower().endswith('.t34') or (os.path.getsize(T42) % 42 and not os.path.getsize(T42) % 34):
    step('converting .t34 (525 lines, 32 characters per row) to .t42')
    conv = os.path.join(OUT, 'stream.t42')
    print('packets {0}'.format(t34_to_t42(T42, conv)), flush=True); T42 = conv
step('service data: keys, flags, clock')
run(os.path.join(HERE, 'extract_extras.py'), T42, '--out', os.path.join(OUT, 'extras.json'))
skip = False; rate = None
if LINES:
    step('checking for skipped fields')
    skip, rate = field_skipping(T42, LINES, LPF)
    print('row breaks: within a field {0:.1%}, at boundaries {1:.1%} -> {2}'.format(rate['in'], rate['edge'], 'fields are skipped, building field-aware' if skip else 'continuous recording'), flush=True)
tmp = tempfile.mkdtemp()
def sparse_stream(t42):
    """Уже очищенный поток: страница повторяется меньше 6 раз (в «сыром» ~20)."""
    st = np.frombuffer(open(t42, 'rb').read(), dtype=np.uint8).reshape(-1, 42)
    cnt = {}
    for p in st:
        a, b = H8.get(int(p[0])), H8.get(int(p[1]))
        if a is None or b is None or ((a >> 3) | (b << 1)) != 0: continue
        key = (a & 7, int(p[2]), int(p[3])); cnt[key] = cnt.get(key, 0) + 1
    return bool(cnt) and float(np.median(list(cnt.values()))) < 6
def headers(t42):
    st = np.frombuffer(open(t42, 'rb').read(), dtype=np.uint8).reshape(-1, 42)
    return sum(1 for p in st if H8.get(int(p[0])) is not None and H8.get(int(p[1])) is not None
               and ((H8[int(p[0])] >> 3) | (H8[int(p[1])] << 1)) == 0)
if headers(T42) < 3:
    sys.exit('The stream has almost no page headers — there is nothing to build pages from.')
SPARSE = ['--sparse'] if sparse_stream(T42) else []
step('building pages' + (' (pages repeat rarely — a cleaned stream or a short recording: each page is taken from its first copy)' if SPARSE else ''))
run(os.path.join(HERE, 'export_json.py'), T42, os.path.join(tmp, 'full.json'), '--no-field-reset', *SPARSE)
good = os.path.join(tmp, 'full.json')
if skip:
    good = os.path.join(tmp, 'good.json')
    run(os.path.join(HERE, 'export_json.py'), T42, good, '--lines', LINES, '--field', str(LPF // 2), *SPARSE)
run(os.path.join(HERE, 'merge.py'), good, os.path.join(tmp, 'full.json'), os.path.join(OUT, 'pages.json'), os.path.join(OUT, 'extras.json'))
if VBI and LINES:
    step('tape quality map')
    run(os.path.join(HERE, 'quality.py'), '--vbi', VBI, '--t42', T42, '--lines', LINES, '--lpf', str(LPF),
        '--extras', os.path.join(OUT, 'extras.json'), '--out', os.path.join(OUT, 'quality.json'))
# основной набор символов: прежний выбор пользователя; иначе объявленный каналом
# (X/28, M/29 — см. extract_extras); иначе угадывается по частым словам
# (decode-orc в этом случае требует настройку; угадывание — только начальное значение)
old = {}
try: old = json.load(open(os.path.join(OUT, 'project.json'), encoding='utf-8'))
except (OSError, ValueError): pass
ex = json.load(open(os.path.join(OUT, 'extras.json'), encoding='utf-8')); em = ex.get('_meta', {})
charset = old.get('charset') or em.get('g0')
if not charset:
    sys.path.insert(0, HERE); import charsets
    P = json.load(open(os.path.join(OUT, 'pages.json')))
    rows = [b for snaps in P.values() for s in snaps[:1] for k, b in s['rows'].items() if k != '0']
    charset = charsets.guess(rows, em.get('national', 0))
    print('character set: {0} (guessed from the text; change it in the “Character set” menu)'.format(charsets.CHARSETS[charset]), flush=True)
json.dump({'source': os.path.abspath(VBI or SOURCE), 'stream': T42, 'lines': LINES and os.path.abspath(LINES),
           'field_skipping': skip, 'break_rates': rate, 'charset': charset},
          open(os.path.join(OUT, 'project.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
step('done')
