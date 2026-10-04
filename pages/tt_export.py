"""Данные страниц, сохранение правок и экспорт (output.t42, output.html, html/).

Состояние страницы: {'versions': [{'t': секунды записи, 'n': передач, 'rows': {'0'..'24': [40 байт]}}],
                     'deleted': bool, 'flof': [5 ссылок], 'x26': {'r,c': символ},
                     'boxed': bool, 'tx': принято заголовков, 'subpages': число подстраниц}
"""
import collections, json, os, shutil, unicodedata
import charsets as CS
import tt_level1 as L1

PG = os.path.dirname(os.path.abspath(__file__))       # модули и шаблоны программы

# ---------------------------------------------------------------- проект
# Проект — папка с pages.json, extras.json, quality.json, project.json и правками
# pages_edited.json; экспорт пишется в неё же.
def set_project(path=None):
    global PROJ, OUTDIR, BASE, EDITED, EXTRAS, QUALITY, META, STREAM, NAME, TITLE, CHARSET, NATIONAL, CHARSET2
    PROJ = os.path.abspath(path or PG)
    OUTDIR = PROJ
    BASE, EDITED, EXTRAS, QUALITY = (os.path.join(PROJ, f) for f in
                                     ('pages.json', 'pages_edited.json', 'extras.json', 'quality.json'))
    META = load_json(os.path.join(PROJ, 'project.json'), {}) or {}
    STREAM = META.get('stream') or ''
    NAME = os.path.basename(META.get('source') or PROJ); TITLE = 'Teletext Rescue · {0}'.format(NAME)
    # набор символов: настройка проекта (как в decode-orc — «местная практика»),
    # иначе объявленный каналом (X/28, M/29), иначе латиница; вариант латиницы — по странице
    em = (load_json(EXTRAS, {}) or {}).get('_meta') or {}
    CHARSET = META.get('charset') or em.get('g0') or 'latin'
    NATIONAL = em.get('national', 1)
    # второй набор для ESC (§15.3): настройка; для кириллицы по умолчанию латиница —
    # пара, которую decode-orc советует для русских служб (латинские слова среди кириллицы)
    CHARSET2 = META.get('charset2', 'latin' if CHARSET.startswith('cyr') else None)
    return PROJ

def set_charset2(cs):
    global CHARSET2
    CHARSET2 = cs or None; META['charset2'] = CHARSET2
    json.dump(META, open(os.path.join(PROJ, 'project.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

def page_table2(pg=None):
    """Набор, в который переключает ESC, или None. Вариант второй латиницы — английский
    (§15.3: флаги C12–C14 ко второму набору не относятся)."""
    if not CHARSET2: return None
    return CS.table(CHARSET2, 0)

def fitted(rows, over, t):
    """Только те поправки X/26, что подходят к своим клеткам (over_fits)."""
    out = {}
    for k, v in (over or {}).items():
        r, c = k.split(','); b = rows.get(r)
        if b and over_fits(v, b[int(c)] & 0x7f, t): out[k] = v
    return out

def set_charset(cs):
    """Сменить основной набор символов проекта (сохраняется в project.json)."""
    global CHARSET
    CHARSET = cs; META['charset'] = cs
    json.dump(META, open(os.path.join(PROJ, 'project.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

def page_table(pg=None):
    """96 символов для кодов 0x20..0x7F на этой странице."""
    return CS.table(CHARSET, (pg or {}).get('nat', NATIONAL))

def is_project(path):
    return os.path.exists(os.path.join(path, 'pages.json'))

def rel(path):
    """Путь для сообщений: относительно папки проекта или testvbi."""
    for base in (OUTDIR, PROJ):
        r = os.path.relpath(path, base)
        if not r.startswith('..'): return r.replace(os.sep, '/')
    return path

# ---------------------------------------------------------------- состояние
def load_json(path, default=None):
    return json.load(open(path, encoding='utf-8')) if os.path.exists(path) else default

def clock():
    """(a, b): эфирное время = a + b * t (t — секунды записи), по часам в заголовках."""
    return (load_json(EXTRAS, {}).get('_meta') or {}).get('clock')

def air_time(t, clk=None):
    clk = clk or clock()
    if not clk: return '{0:.0f} s of recording'.format(t)
    s = int(round(clk[0] + clk[1] * t)) % 86400; return f'{s // 3600:02}:{s // 60 % 60:02}:{s % 60:02}'

def span(pages):
    """(первая, последняя) секунда записи по версиям страниц."""
    ts = [s['t'] for pg in pages.values() for s in pg['versions']]
    return (min(ts), max(ts)) if ts else (0, 0)

def load_state():
    """-> (страницы, список изменённых, описание источника)."""
    j = load_json(EDITED)
    if j and j.get('edited'):                           # без правок — берём свежую сборку
        return j['pages'], j['edited'], 'your edits ({0})'.format(rel(EDITED))
    base = load_json(BASE)
    if base is None:
        raise FileNotFoundError('no {0} — the project has not been built'.format(BASE) + (' (python pages/build_pages.py)' if PROJ == DATA else ''))
    ex = load_json(EXTRAS, {})
    pages = {pid: {'versions': snaps, 'deleted': False, **ex.get(pid, {})} for pid, snaps in base.items()}
    return pages, [], 'original build ({0})'.format(rel(BASE))

def save_state(pages, edited):
    tmp = EDITED + '.tmp'
    json.dump({'pages': pages, 'edited': sorted(edited)}, open(tmp, 'w', encoding='utf-8'), separators=(',', ':'))
    os.replace(tmp, EDITED)

# ---------------------------------------------------------------- текст
def glyph(c, t):
    return t[(c & 0x7f) - 0x20]

def over_fits(ov, c, t):
    """Поправка X/26 ложится поверх пробела, того же символа или той же буквы без
    диакритики (каналы ставят в ряду уровня 1 основную букву: 'e' под 'é'); иначе — нет:
    поправка могла прийти от другой версии страницы."""
    if not ov: return None
    g = glyph(c, t)
    base = unicodedata.normalize('NFD', ov)[0]
    return ov if (c == 0x20 or g == ov or g == base) else None

def overlay(pg, s=None):
    """Поправки X/26 для версии s: своей подстраницы, если они известны, иначе общие."""
    per = pg.get('x26s') or {}
    return per.get((s or {}).get('s')) or pg.get('x26')

def row_text(b, over=None, r=None, t=None, t2=None):
    """Ряд как текст (как видит зритель): управляющие коды и мозаика — пробелы, ESC переключает набор."""
    t = t or page_table(); key = str(r if r is not None else 1)
    rows = {key: b}
    return L1.row_texts(rows, t, t2 if t2 is not None else page_table2(), fitted(rows, over, t)).get(int(key), '')

def _row_text_old(b, over=None, r=None, t=None):
    out = []; mos = False; t = t or page_table()
    for i, c in enumerate(b):
        c &= 0x7f
        ov = over_fits((over or {}).get(f'{r},{i}'), c, t)
        if ov: out.append(ov); continue
        if c < 0x20:
            if 0x10 <= c <= 0x17: mos = True
            elif c <= 0x07: mos = False
            out.append(' ')
        elif mos and (c & 0x20): out.append(' ')
        else: out.append(glyph(c, t))
    return ''.join(out).rstrip()

def page_title(pg):
    """Название раздела: правая часть рядов 2-3 (рядом с логотипом RTL World);
    перенос со знаком «-» склеивается (UEFA CHAM- / PIONS LEAGUE)."""
    t = page_table(pg)
    for s in pg['versions']:
        r2, r3 = s['rows'].get('2'), s['rows'].get('3')
        t3 = row_text(r3, t=t)[24:].strip() if r3 else ''
        if not t3: continue
        t2 = row_text(r2, t=t)[24:].strip() if r2 else ''
        if t2.endswith('-'): return t2[:-1] + t3
        return (t2 + ' ' + t3).strip() if t2 and not t2[0].isdigit() else t3
    return ''

def service_note(pid, pg):
    if pg.get('boxed'):
        return 'service: for display over the picture (C6 flag)'
    return ''

# ---------------------------------------------------------------- t42
def _ham_table():
    enc = {}
    for d in range(16):
        b = [(d >> i) & 1 for i in range(4)]
        p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
        p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
        enc[d] = sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))
    return enc
HAM = _ham_table()
DEHAM = {v: k for k, v in HAM.items()}

def odd(c):
    c &= 0x7f
    return c | 0x80 if bin(c).count('1') % 2 == 0 else c

def mrag(mag, row):
    return bytes([HAM[(mag & 7) | ((row & 1) << 3)], HAM[row >> 1]])

POS2418 = [3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23]
def h2418(data):
    """18 бит -> 3 байта Hamming 24/18 (нечётная чётность, как в пакетах X/26)."""
    w = 0
    for i, p in enumerate(POS2418): w |= ((data >> i) & 1) << (p - 1)
    for k in (1, 2, 4, 8, 16):
        if sum((w >> (q - 1)) & 1 for q in range(1, 24) if q & k) % 2 != 1: w |= 1 << (k - 1)
    if sum((w >> q) & 1 for q in range(24)) % 2 != 1: w |= 1 << 23
    return bytes([w & 0xff, (w >> 8) & 0xff, (w >> 16) & 0xff])

# набор G2 Latin (ETS 300 706, табл. 36) с 0x20; 0x40-0x4F — диакритика (для X/26 не нужна)
G2_LATIN = (' ¡¢£$¥#§¤‘“«←↑→↓°±²³×µ¶·÷’”»¼½¾¿' + ' ' * 16 +
            '―¹®©™♪₠‰α   ⅛⅜⅝⅞ΩÆĐªĦ ĲĿŁØŒºÞŦŊŉĸæđðħıĳŀłøœßþŧŋ■')
G2_REV = {ch: 0x20 + i for i, ch in enumerate(G2_LATIN) if ch.strip() and ch not in '$#'}
DIA_REV = {'̀': 1, '́': 2, '̂': 3, '̃': 4, '̄': 5, '̆': 6, '̇': 7, '̈': 8,
           '̊': 10, '̧': 11, '̋': 13, '̨': 14, '̌': 15}

def x26_packets(mag, over):
    """Поправки символов {'r,c': символ} -> пакеты X/26 (по 13 троек)."""
    trips = []
    for r in sorted({int(k.split(',')[0]) for k in over}):
        trips.append((40 if r == 24 else 40 + r) | (0x04 << 6))          # активная позиция: ряд
        for k in sorted((k for k in over if int(k.split(',')[0]) == r), key=lambda k: int(k.split(',')[1])):
            c = int(k.split(',')[1]); ch = over[k]; d = unicodedata.normalize('NFD', ch)
            if ch in G2_REV: mode, data = 0x0f, G2_REV[ch]
            elif len(d) == 2 and d[1] in DIA_REV and ord(d[0]) < 0x80: mode, data = 0x10 + DIA_REV[d[1]], ord(d[0])
            elif ord(ch) < 0x80: mode, data = 0x10, ord(ch)
            else: continue
            trips.append(c | (mode << 6) | (data << 11))
    trips.append(63 | (0x1f << 6))                                          # конец
    pk = []
    for i in range(0, len(trips), 13):
        part = trips[i:i + 13]; part += [63 | (0x1f << 6)] * (13 - len(part))
        pk.append(mrag(mag, 26) + bytes([HAM[(i // 13) & 15]]) + b''.join(h2418(t) for t in part))
    return pk

def x27_packet(mag, links):
    """Ссылки FLOF [красная, зелёная, жёлтая, голубая, индекс] -> пакет X/27/0."""
    out = bytearray(mrag(mag, 27) + bytes([HAM[0]]))
    six = list(links[:4]) + [None, links[4] if len(links) > 4 else None]
    for p in six:
        if p:
            lm = (int(p[0]) & 7) ^ (mag & 7)
            out += bytes([HAM[int(p[2])], HAM[int(p[1])], HAM[0xf], HAM[0x7 | ((lm & 1) << 3)], HAM[0xf], HAM[0x3 | ((lm >> 1) << 2)]])
        else:
            out += bytes([HAM[0xf], HAM[0xf], HAM[0xf], HAM[0x7], HAM[0xf], HAM[0x3]])
    out += bytes([HAM[0xf]]) + b'\x00\x00'           # управление ссылками; CRC не вычисляется
    return bytes(out)

def header_controls():
    """Байты подкода/управления (4..9) заголовка для каждой страницы — самые
    частые в stream.t42, чтобы сохранить национальный набор и флаги."""
    path = STREAM
    ctl = collections.defaultdict(collections.Counter)
    if path and os.path.exists(path):
        d = open(path, 'rb').read()
        for i in range(0, len(d) - 41, 42):
            p = d[i:i + 42]
            a, b = DEHAM.get(p[0]), DEHAM.get(p[1])
            if a is None or b is None or ((a >> 3) | (b << 1)) != 0: continue
            u, t = DEHAM.get(p[2]), DEHAM.get(p[3])
            if u is None or t is None or u > 9 or t > 9 or any(DEHAM.get(x) is None for x in p[4:10]): continue
            ctl[f"{a & 7 or 8}{t}{u}"][bytes(p[4:10])] += 1
    out = {k: c.most_common(1)[0][0] for k, c in ctl.items()}
    common = collections.Counter(out.values()).most_common(1)
    return out, (common[0][0] if common else bytes([HAM[0]] * 6))

def sub_ctl(ctl, sub):
    """Байты 4..9 заголовка с кодом подстраницы sub ('0002' = S4 S3 S2 S1); флаги C4–C6 сохраняются."""
    if not sub: return ctl
    try: s4, s3, s2, s1 = (int(c, 16) for c in sub)
    except ValueError: return ctl
    c = list(ctl)
    c[0] = HAM[s1]; c[1] = HAM[(s2 & 7) | (DEHAM.get(c[1], 0) & 8)]
    c[2] = HAM[s3]; c[3] = HAM[(s4 & 3) | (DEHAM.get(c[3], 0) & 12)]
    return bytes(c)

def version_label(pg, k, clk=None):
    """'подстраница 0002 · версия 1/3 · ≈04:05:10 эфира' для версии k."""
    V = pg['versions']; s = V[k]; sub = s.get('s')
    same = [i for i, x in enumerate(V) if x.get('s') == sub]
    subs = sorted({x.get('s') for x in V if x.get('s')})
    head = 'subpage {0} ({1} of {2}) · '.format(sub, subs.index(sub) + 1, len(subs)) if sub and len(subs) > 1 else ''
    if s.get('full'): return '{0}complete page · up to ≈{1} broadcast'.format(head, air_time(s["t"], clk))
    return '{0}version {1}/{2} · ≈{3} broadcast'.format(head, same.index(k) + 1, len(same), air_time(s["t"], clk))

def to_t42(pages):
    ctl, default = header_controls()
    out = bytearray(); n = 0
    for pid in sorted(pages):
        pg = pages[pid]
        if pg.get('deleted'): continue
        mag, tens, units = int(pid[0]), int(pid[1]), int(pid[2])
        for s in pg['versions']:
            rows = s['rows']
            hdr = rows.get('0') or [0x20] * 40
            out += mrag(mag, 0) + bytes([HAM[units], HAM[tens]]) + sub_ctl(ctl.get(pid, default), s.get('s'))
            out += bytes(odd(c) for c in hdr[8:40]); n += 1
            if pg.get('flof'):
                out += x27_packet(mag, pg['flof']); n += 1
            if pg.get('x26'):
                for q in x26_packets(mag, pg['x26']): out += q; n += 1
            for r in sorted(int(k) for k in rows if k != '0'):
                if 1 <= r <= 24:
                    out += mrag(mag, r) + bytes(odd(c) for c in rows[str(r)][:40]); n += 1
    return bytes(out), n

# ---------------------------------------------------------------- HTML
def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')

def _versions_for_html(pg, clk):
    t = page_table(pg); out = []
    for k, s in enumerate(pg['versions']):
        x = fitted(s['rows'], overlay(pg, s), t)
        out.append({'t': s['t'], 'air': air_time(s['t'], clk), 'rows': s['rows'], 'label': version_label(pg, k, clk),
                    **({'x': x} if x else {})})
    return out

def to_output_html(pages):
    tpl = open(os.path.join(PG, 'output_template.html'), encoding='utf-8').read()
    render = open(os.path.join(PG, 'tt_render.js'), encoding='utf-8').read()
    clk = clock()
    data = {pid: {'v': _versions_for_html(pg, clk), 'f': pg.get('flof'), 'x': pg.get('x26'),
                  'note': service_note(pid, pg), 'cs': page_table(pg), 'cs2': page_table2(pg)}
            for pid, pg in pages.items() if not pg.get('deleted')}
    t0, t1 = span(pages)
    sub = '{0} pages, recovered from {1}; broadcast ≈{2}–{3}'.format(len(data), NAME, air_time(t0, clk), air_time(t1, clk))
    return (tpl.replace('__TITLE__', esc(TITLE)).replace('__SUBTITLE__', sub).replace('__RENDER__', render)
               .replace('__DATA__', json.dumps(data, separators=(',', ':'), ensure_ascii=False)))

INDEX_CSS = '''
:root{--bg:#f4f4f2;--panel:#fff;--fg:#1d1d1b;--mut:#5d5c58;--acc:#1f6fc9;--line:#dcdbd6;--note:#9a5b00}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#141413;--panel:#1d1d1c;--fg:#ececea;--mut:#a3a29c;--acc:#5aa0ee;--line:#34342f;--note:#f0a640}}
:root[data-theme="dark"]{--bg:#141413;--panel:#1d1d1c;--fg:#ececea;--mut:#a3a29c;--acc:#5aa0ee;--line:#34342f;--note:#f0a640}
body{margin:0;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif;padding:16px}
.wrap{max-width:1100px;margin:auto}h1{font-size:20px;margin:0 0 4px}.sub{color:var(--mut);margin-bottom:14px}
a{color:var(--acc)}
table{border-collapse:collapse;width:100%;background:var(--panel);font-size:13px}
th,td{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;white-space:nowrap}
th{position:sticky;top:0;background:var(--panel);color:var(--mut);font-weight:600;cursor:pointer}
td.n{text-align:right;font-variant-numeric:tabular-nums}td.t{white-space:normal}
td.note{color:var(--note);white-space:normal}
.bar{display:inline-block;height:8px;background:var(--acc);border-radius:0 4px 4px 0;vertical-align:middle;margin-left:6px}
input{font:inherit;padding:4px 8px;margin-bottom:10px;border:1px solid var(--line);background:var(--panel);color:var(--fg);border-radius:4px;width:min(320px,100%)}
.scroll{overflow-x:auto}
'''

def to_page_htmls(pages, outdir):
    """Каждая страница — отдельный html/NNN.html; плюс index.html (со статистикой), quality.html и tt_render.js."""
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):                       # убрать страницы, которых больше нет
        if f[:3].isdigit() and f.endswith('.html') and len(f) == 8:
            os.remove(os.path.join(outdir, f))
    shutil.copy(os.path.join(PG, 'tt_render.js'), os.path.join(outdir, 'tt_render.js'))
    tpl = open(os.path.join(PG, 'page_template.html'), encoding='utf-8').read()
    clk = clock()
    ids = sorted(p for p, pg in pages.items() if not pg.get('deleted'))
    exist = json.dumps(ids)
    for i, pid in enumerate(ids):
        pg = pages[pid]; vers = _versions_for_html(pg, clk); t = page_table(pg)
        text = []
        for k, s in enumerate(pg['versions']):
            text.append(f'=== {vers[k]["label"]} ===')
            for r in range(25):
                b = s['rows'].get(str(r))
                text.append(row_text(b, overlay(pg, s), r, t) if b else '')
        note = service_note(pid, pg)
        html = (tpl.replace('__PID__', pid).replace('__PREV__', ids[i - 1]).replace('__NEXT__', ids[(i + 1) % len(ids)])
                   .replace('__NOTE__', f'<div class="note">{esc(note)}</div>' if note else '')
                   .replace('__TEXT__', esc('\n'.join(text))).replace('__EXIST__', exist)
                   .replace('__EXTRA__', json.dumps({'flof': pg.get('flof'), 'cs': t, 'cs2': page_table2(pg)}, ensure_ascii=False))
                   .replace('__HINT_TITLE__', esc(TITLE))
                   .replace('__DATA__', json.dumps(vers, separators=(',', ':'))))
        open(os.path.join(outdir, f'{pid}.html'), 'w', encoding='utf-8').write(html)
    open(os.path.join(outdir, 'index.html'), 'w', encoding='utf-8').write(index_html(pages, ids, clk))
    open(os.path.join(outdir, 'quality.html'), 'w', encoding='utf-8').write(quality_html())
    return len(ids)

def index_html(pages, ids, clk):
    mx = max((pages[p].get('tx', 0) for p in ids), default=1) or 1
    t0, t1 = span({p: pages[p] for p in ids})
    rows = []
    for p in ids:
        pg = pages[p]; tx = pg.get('tx', 0); v = pg['versions']
        when = f'{air_time(v[0]["t"], clk)}–{air_time(v[-1]["t"], clk)}' if len(v) > 1 else air_time(v[0]['t'], clk)
        rows.append(f'<tr><td><a href="{p}.html"><b>{p}</b></a></td><td class="t">{esc(page_title(pg))}</td>'
                    f'<td class="n" data-v="{tx}">{tx}<span class="bar" style="width:{60 * tx / mx:.0f}px"></span></td>'
                    f'<td class="n">{len(v)}</td><td class="n">{pg.get("subpages", 1)}</td><td>{when}</td>'
                    f'<td class="note">{esc(service_note(p, pg))}</td></tr>')
    return f'''<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{esc(TITLE)}</title><style>{INDEX_CSS}</style></head><body><div class="wrap">
<h1>{esc(TITLE)}</h1>
<div class="sub">{len(ids)} страниц · эфир ≈{air_time(t0, clk)}–{air_time(t1, clk)}{' · <a href="quality.html">карта качества ленты</a>' if os.path.exists(QUALITY) else ''}</div>
<input id="q" placeholder="Поиск: номер или название">
<div class="scroll"><table id="t"><thead><tr><th>Стр.</th><th>Раздел</th><th title="сколько раз принят заголовок страницы">Принята, раз</th>
<th title="сколько разных версий содержимого">Версий</th><th title="разных кодов подстраниц">Подстраниц</th><th>Эфир (версии)</th><th>Пометка</th></tr></thead>
<tbody>{''.join(rows)}</tbody></table></div></div>
<script>
const q=document.getElementById('q'),tb=document.querySelector('#t tbody');
q.oninput=()=>{{const s=q.value.toLowerCase();for(const r of tb.rows)r.style.display=r.textContent.toLowerCase().includes(s)?'':'none'}};
document.querySelectorAll('th').forEach((th,i)=>th.onclick=()=>{{const rs=[...tb.rows],num=i>=2&&i<=4,dir=th.dataset.d=th.dataset.d=='1'?'-1':'1';
 rs.sort((a,b)=>{{const x=a.cells[i],y=b.cells[i];const u=num?+(x.dataset.v||x.textContent):x.textContent,w=num?+(y.dataset.v||y.textContent):y.textContent;return (u>w?1:u<w?-1:0)*dir}});rs.forEach(r=>tb.appendChild(r))}});
</script></body></html>'''

def quality_html():
    """Карта качества ленты по минутам: три малых графика (по одной метрике), подсказки и таблица."""
    Q = load_json(QUALITY)
    if not Q: return '<!doctype html><meta charset="utf-8"><p>карты качества нет: её строит сборка из записи .vbi</p>'
    data = json.dumps(Q['minutes'], ensure_ascii=False)
    return '''<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Качество ленты</title><style>''' + INDEX_CSS + '''
.viz-root{--surface-1:#fcfcfb;--text-primary:#0b0b0b;--text-secondary:#52514e;--grid:#e4e3df;--series-1:#2a78d6}
@media (prefers-color-scheme: dark){:root:where(:not([data-theme="light"])) .viz-root{--surface-1:#1a1a19;--text-primary:#fff;--text-secondary:#c3c2b7;--grid:#33332f;--series-1:#3987e5}}
:root[data-theme="dark"] .viz-root{--surface-1:#1a1a19;--text-primary:#fff;--text-secondary:#c3c2b7;--grid:#33332f;--series-1:#3987e5}
.grid3{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px;margin:12px 0}
.card{background:var(--surface-1);border:1px solid var(--line);border-radius:6px;padding:10px 12px;position:relative}
.card h3{margin:0;font-size:14px;color:var(--text-primary)}.card p{margin:2px 0 6px;font-size:12px;color:var(--text-secondary)}
svg{width:100%;height:auto;display:block}svg text{fill:var(--text-secondary);font-size:10px}
.tip{position:absolute;pointer-events:none;background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:4px 8px;font-size:12px;color:var(--text-primary);display:none;white-space:nowrap;box-shadow:0 2px 6px #0003}
</style></head><body><div class="wrap viz-root">
<h1>Карта качества ленты</h1><div class="sub">По минутам записи ''' + esc(NAME) + ''' (в скобках — эфирное время). <a href="index.html">← к страницам</a></div>
<div class="grid3" id="charts"></div>
<h3>Таблица</h3><div class="scroll"><table id="tbl"><thead><tr><th>Минута записи</th><th>Эфир</th><th>Принято строк, %</th><th>Нечитаемо, %</th><th>Ошибки чётности, %</th></tr></thead><tbody></tbody></table></div>
</div><script>
const D=''' + data + ''';
D.forEach(d=>d.missed=+(100-d.received).toFixed(2));
const M=[['missed','Не принято строк телетекста','доля строк VBI с телетекстом, из которых пакет не получен, %',null],
         ['unreadable','Из них нечитаемы','сигнал есть, пакет не получен ни одним декодером, %',null],
         ['parity','Ошибки чётности','доля байт с нарушенной чётностью в принятых пакетах, %',null]];
const box=document.getElementById('charts');
for(const [k,title,desc,dom] of M){
  const vals=D.map(d=>d[k]??0),W=320,H=150,L=34,B=22,T=8,R=6,n=vals.length,bw=(W-L-R)/n;
  const lo=dom?dom[0]:0,hi=dom?dom[1]:Math.max(...vals)*1.15||1,y=v=>T+(H-T-B)*(1-(Math.max(v,lo)-lo)/(hi-lo));
  let s=`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${title}">`;
  for(let i=0;i<=4;i++){const v=lo+(hi-lo)*i/4,yy=y(v);s+=`<line x1="${L}" x2="${W-R}" y1="${yy}" y2="${yy}" stroke="var(--grid)" stroke-width="1"/><text x="${L-4}" y="${yy+3}" text-anchor="end">${+v.toFixed(v<1?2:1)}</text>`}
  vals.forEach((v,i)=>{const x=L+i*bw+1,yy=y(v),h=H-B-yy;
    s+=`<path d="M${x},${H-B}V${yy+Math.min(4,h)}q0,-4 4,-4h${bw-10}q4,0 4,4V${H-B}Z" fill="var(--series-1)"/>`;
    s+=`<rect x="${L+i*bw}" y="${T}" width="${bw}" height="${H-T-B}" fill="transparent" data-i="${i}"/>`;
    if(n<=12||i%2==0)s+=`<text x="${L+i*bw+bw/2}" y="${H-8}" text-anchor="middle">${D[i].minute}</text>`});
  s+=`<line x1="${L}" x2="${W-R}" y1="${H-B}" y2="${H-B}" stroke="var(--text-secondary)" stroke-width="1"/></svg>`;
  const c=document.createElement('div');c.className='card';c.innerHTML=`<h3>${title}</h3><p>${desc}${dom?' (ось от '+dom[0]+'%)':''}</p>${s}<div class="tip"></div>`;
  const tip=c.querySelector('.tip');
  c.querySelectorAll('rect[data-i]').forEach(r=>{r.onmousemove=e=>{const d=D[+r.dataset.i];tip.style.display='block';
    tip.innerHTML=`<b>минута ${d.minute}</b> (${d.air||''})<br>${title}: ${(d[k]??0).toFixed(k=='parity'?3:1)}%`;
    const rc=c.getBoundingClientRect();tip.style.left=Math.min(e.clientX-rc.left+12,rc.width-180)+'px';tip.style.top=(e.clientY-rc.top-40)+'px'};
    r.onmouseleave=()=>tip.style.display='none'});
  box.appendChild(c);
}
document.querySelector('#tbl tbody').innerHTML=D.map(d=>`<tr><td class="n">${d.minute}</td><td>${d.air||''}</td><td class="n">${d.received.toFixed(1)}</td><td class="n">${d.unreadable.toFixed(1)}</td><td class="n">${(d.parity??0).toFixed(3)}</td></tr>`).join('');
</script></body></html>'''

# ---------------------------------------------------------------- полные страницы
def _row_dist(a, b):
    return sum((x & 0x7f) != (y & 0x7f) for x, y in zip(a, b))

def full_versions(pg):
    """По одной полной странице на подстраницу: каждый ряд — из всех версий этой
    подстраницы. Из вариантов ряда берётся тот, за который больше всего копий
    (с учётом почти одинаковых — до 6 отличающихся байт: это один ряд, принятый с
    помехами), и внутри этой группы — побайтное голосование по числу копий.
    Так из повреждённых приёмов собирается одна чистая страница."""
    by = collections.OrderedDict()
    for s in pg['versions']: by.setdefault(s.get('s'), []).append(s)
    allv = pg['versions']
    out = []
    for sub, V in by.items():
        last = max(V, key=lambda x: x['t'])
        rows = {}; cnt = {}
        for r in sorted({r for x in V for r in x['rows']}, key=int):
            var = [(x['rows'][r], (x.get('c') or {}).get(r) or x.get('n') or 1, x['t']) for x in V if r in x['rows']]
            if r == '0':
                rows[r] = list(last['rows'].get('0') or var[-1][0]); continue
            score = [sum(w for b2, w, _ in var if _row_dist(b, b2) <= 6) for b, _, _ in var]
            best = max(range(len(var)), key=lambda i: (score[i], var[i][2]))
            # в побайтное голосование идут и почти такие же ряды других подстраниц:
            # общий ряд (рубрики, подвал), испорченный помехой в одной подстранице,
            # поправят остальные; настоящие различия подстраниц дальше 6 байт
            grp = [((x['rows'][r]), (x.get('c') or {}).get(r) or x.get('n') or 1) for x in allv
                   if r in x['rows'] and _row_dist(var[best][0], x['rows'][r]) <= 6]
            row = []
            for k in range(40):
                votes = collections.Counter()
                for b, w in grp: votes[b[k] & 0x7f] += w
                row.append(votes.most_common(1)[0][0])
            rows[r] = row; cnt[r] = score[best]
        out.append({'t': last['t'], 'n': sum(x.get('n') or 1 for x in V), 'rows': rows, 'c': cnt,
                    **({'s': sub} if sub else {}), 'full': True})
    return out

def full_pages(pages):
    """Все страницы проекта в виде полных страниц (по одной на подстраницу)."""
    return {pid: {**pg, 'versions': full_versions(pg)} for pid, pg in pages.items()}

def export_all(pages, full=False):
    """output.t42 (+ однократная копия прежнего в output.orig.t42), output.html, html/ — в папку экспорта проекта.
    full=True — только полные страницы: output_full.t42, output_full.html, html_full/."""
    if full: pages = full_pages(pages)
    tag = '_full' if full else ''
    t42, n = to_t42(pages)
    out = os.path.join(OUTDIR, f'output{tag}.t42'); bak = os.path.join(OUTDIR, 'output.orig.t42')
    note = ''
    if not full and os.path.exists(out) and not os.path.exists(bak):
        shutil.copy2(out, bak); note = '; the previous output.t42 was kept as output.orig.t42'
    tmp = out + '.tmp'; open(tmp, 'wb').write(t42); os.replace(tmp, out)
    open(os.path.join(OUTDIR, f'output{tag}.html'), 'w', encoding='utf-8').write(to_output_html(pages))
    k = to_page_htmls(pages, os.path.join(OUTDIR, f'html{tag}'))
    where = ' in {0}'.format(OUTDIR)
    return 'output{0}.t42 ({1} packets), output{2}.html and {3} pages in html{4}/ written{5}{6}'.format(tag, n, tag, k, tag, where, note)

set_project()
