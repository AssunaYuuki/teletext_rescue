# Извлекает из потока служебные данные страниц -> pages/extras.json:
#   flof — ссылки цветных кнопок (пакет X/27/0): красная, зелёная, жёлтая, голубая, индекс;
#   x26  — поправки символов (пакет X/26, Hamming 24/18): буквы с диакритикой
#          и символы набора G2, которых нет в немецком наборе G0;
#   boxed — флаг C6 в заголовке: страница для показа поверх картинки или служебная;
#   tx, subpages — сколько раз принят заголовок и сколько разных подстраниц;
#   _meta.clock — эфирное время = a + b * (номер пакета / 650), по часам в заголовках;
#   nat — национальный вариант латиницы по флагам C12–C14 заголовка (_meta.national — самый частый);
#   _meta.g0 — основной набор символов, если канал объявляет его пакетами X/28/0 или M/29/0
#          (как в decode-orc: все 13 троек Hamming 24/18 должны читаться, обозначение 0100 = кириллица).
# X/26 и X/27 идут сразу за заголовком, а заголовок часто последний в поле VBI,
# поэтому пакеты приписываются последнему заголовку магазина без сброса на
# границе поля; ошибочные привязки отсеиваются голосованием по передачам.
#   python pages/extract_extras.py [поток.t42] [--out extras.json]
import collections, json, os, sys, unicodedata
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from charsets import national_of
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
if len(sys.argv) < 2 or sys.argv[1].startswith('--'):
    sys.exit('usage: extract_extras.py stream.t42 [--out extras.json]')
src = sys.argv[1]
st = np.frombuffer(open(src, 'rb').read(), dtype=np.uint8).reshape(-1, 42)

H8 = {}
for dd in range(16):
    b = [(dd >> i) & 1 for i in range(4)]
    p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
    p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
    H8[sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))] = dd

POS = [3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23]
def h2418(b0, b1, b2):
    """Тройка Hamming 24/18 -> 18 бит данных или None при ошибке."""
    w = int(b0) | (int(b1) << 8) | (int(b2) << 16)
    bit = lambda p: (w >> (p - 1)) & 1
    for k in (1, 2, 4, 8, 16):                     # нечётная чётность по группам
        if sum(bit(p) for p in range(1, 24) if p & k) % 2 != 1: return None
    if sum(bit(p) for p in range(1, 25)) % 2 != 1: return None
    return sum(bit(p) << i for i, p in enumerate(POS))

# набор G2 (латиница) и диакритические знаки (режимы 0x11..0x1F)
G2 = dict(zip(range(0x20, 0x80),   # таблица G2 Latin (ETS 300 706, табл. 36)
    ' ¡¢£$¥#§¤‘“«←↑→↓°±²³×µ¶·÷’”»¼½¾¿ ̀́̂̃̄̆̇̈.̧̊_̨̋̌'
    '―¹®©™♪₠‰α   ⅛⅜⅝⅞ΩÆĐªĦ ĲĿŁØŒºÞŦŊŉĸæđðħıĳŀłøœßþŧŋ■'))
DIA = {1: '̀', 2: '́', 3: '̂', 4: '̃', 5: '̄', 6: '̆', 7: '̇', 8: '̈',
       10: '̊', 11: '̧', 13: '̋', 14: '̨', 15: '̌'}

flof = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
x26 = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
x26s = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(collections.Counter)))
cursub = {}                                           # магазин -> код подстраницы открытой страницы
cur = {}
nat = collections.defaultdict(collections.Counter); g0 = collections.Counter()
CYR_DESIG = {0b000: 'cyr1', 0b100: 'cyr2', 0b101: 'cyr3'}
tx = collections.Counter(); subs = collections.defaultdict(collections.Counter); c6 = collections.Counter()
clock = []                                             # (номер пакета, секунды эфира)
for k, p in enumerate(st):
    a, b = H8.get(int(p[0])), H8.get(int(p[1]))
    if a is None or b is None: continue
    mag = a & 7 or 8; row = (a >> 3) | (b << 1)
    if row == 0:
        u, t = H8.get(int(p[2])), H8.get(int(p[3]))
        cur[mag] = pid = f'{mag}{t}{u}' if u is not None and t is not None and t <= 9 and u <= 9 else None
        sc = [H8.get(int(x)) for x in p[4:8]]
        cursub[mag] = f'{sc[3] & 3:X}{sc[2]:X}{sc[1] & 7:X}{sc[0]:X}' if None not in sc else None
        d = [H8.get(int(x)) for x in p[4:10]]
        if pid and None not in d:
            tx[pid] += 1; c6[pid] += (d[3] >> 3) & 1; nat[pid][national_of(d[5])] += 1
            subs[pid][(d[3] & 3, d[2], d[1] & 7, d[0])] += 1
        txt = bytes(int(c) & 0x7f for c in p[34:42]).decode('latin-1')
        if len(txt) == 8 and txt[2] == ':' and txt[5] == ':' and all(txt[i].isdigit() for i in (0, 1, 3, 4, 6, 7)):
            hh, mm, ss = int(txt[:2]), int(txt[3:5]), int(txt[6:])
            if hh < 24 and mm < 60 and ss < 60: clock.append((k, hh * 3600 + mm * 60 + ss))
        continue
    if row in (28, 29) and H8.get(int(p[2])) in (0, 4):  # обозначение набора символов (§9.4.2)
        tr = [h2418(*p[3 + 3 * i:6 + 3 * i]) for i in range(13)]
        if None not in tr and (row == 29 or tr[0] & 0xF == 0):
            val = (tr[0] >> 7) & 0x7F
            g0[CYR_DESIG.get(val & 7, 'latin') if (val >> 3) & 0xF == 0b0100 else 'latin'] += 1
    pid = cur.get(mag)
    if not pid: continue
    if row == 27 and H8.get(int(p[2])) == 0:          # FLOF
        for i in range(6):
            d = [H8.get(int(x)) for x in p[3 + 6 * i:9 + 6 * i]]
            if None in d: continue
            pu, pt = d[0], d[1]
            if pu > 9 or pt > 9: continue               # FF = ссылки нет
            m = mag ^ (((d[3] >> 3) & 1) | (((d[5] >> 2) & 1) << 1) | (((d[5] >> 3) & 1) << 2))
            flof[pid][i][f'{m or 8}{pt}{pu}'] += 1
    elif row == 26:                                    # поправки символов
        r_ = None
        for i in range(13):
            t = h2418(*p[3 + 3 * i:6 + 3 * i])
            if t is None: break                         # дальше тройкам доверять нельзя
            addr = t & 0x3f; mode = (t >> 6) & 0x1f; data = (t >> 11) & 0x7f
            if addr >= 40:
                if mode == 0x1f: break                  # конец
                r_ = 24 if addr == 40 else addr - 40 if mode == 0x04 else None
            elif r_ is not None:
                ch = None
                if mode == 0x0f: ch = G2.get(data)
                elif mode == 0x09: ch = chr(data) if data >= 0x20 else None
                elif mode >= 0x10 and data >= 0x20:
                    ch = unicodedata.normalize('NFC', chr(data) + DIA.get(mode - 0x10, ''))
                if ch and ch.strip():
                    x26[pid][f'{r_},{addr}'][ch] += 1
                    if cursub.get(mag): x26s[pid][cursub[mag]][f'{r_},{addr}'][ch] += 1

out = {}
for pid in set(flof) | set(x26) | set(tx):
    e = {}
    if tx[pid]:
        e['tx'] = tx[pid]
        e['subpages'] = sum(1 for n in subs[pid].values() if n >= 3) or 1
        if c6[pid] >= max(2, 0.5 * tx[pid]): e['boxed'] = True
        e['nat'] = nat[pid].most_common(1)[0][0] if nat[pid] else 1
    if pid in flof:
        links = []
        for i in range(6):
            c = flof[pid].get(i)
            ok = c and c.most_common(1)[0][1] >= 2 and c.most_common(1)[0][1] >= 0.6 * sum(c.values())
            links.append(c.most_common(1)[0][0] if ok else None)
        if any(links[:4]): e['flof'] = links[:4] + [links[5]]
    if pid in x26:
        ov = {}
        for pos, c in x26[pid].items():
            ch, n = c.most_common(1)[0]
            if n >= 2 and n >= 0.6 * sum(c.values()): ov[pos] = ch
        if ov: e['x26'] = ov
        per = {}                                       # поправки по подстраницам (если их несколько)
        for sub, poss in x26s[pid].items():
            o = {}
            for pos, c in poss.items():
                ch, n = c.most_common(1)[0]
                if n >= 2 and n >= 0.6 * sum(c.values()): o[pos] = ch
            if o: per[sub] = o
        if len(per) > 1 or (per and set(per) != {'0000'}): e['x26s'] = per
    if e: out[pid] = e
# эфирное время: устойчивая линейная подгонка часов по номеру пакета
if clock:
    K = np.array([c[0] for c in clock], float) / 650.0; T = np.array([c[1] for c in clock], float)
    # скорость часов относительно записи: медиана наклонов между далёкими точками
    # (на testvbi ≈1,96 — захват терял кадры; на непрерывной записи ≈1)
    i = np.random.default_rng(0).integers(0, len(K), (2, 4000))
    dk = K[i[1]] - K[i[0]]; far = np.abs(dk) > 0.2 * (np.ptp(K) or 1)
    slope = float(np.median((T[i[1]] - T[i[0]])[far] / dk[far])) if far.sum() > 10 else 1.0
    med = np.median(T - slope * K)
    ok = np.abs(T - slope * K - med) < 30               # отбросить часы с ошибками приёма
    b_, a_ = np.polyfit(K[ok], T[ok], 1) if ok.sum() > 1 else (slope, med)
    out['_meta'] = {'clock': [round(float(a_), 2), round(float(b_), 5)]}
allnat = sum(nat.values(), collections.Counter())
meta = out.setdefault('_meta', {})
if allnat: meta['national'] = allnat.most_common(1)[0][0]
if g0 and g0.most_common(1)[0][1] >= 2: meta['g0'] = g0.most_common(1)[0][0]
OUT = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(src)), 'extras.json')
json.dump(out, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
print('stream {0}: key links on {1} pages, character enhancements on {2} pages, service (C6) {3}; {4}'.format(os.path.basename(src), sum('flof' in e for e in out.values()), sum('x26' in e for e in out.values()), sum(bool(e.get('boxed')) for e in out.values() if 'tx' in e), out.get('_meta')))
