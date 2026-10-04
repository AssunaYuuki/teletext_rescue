# Что передаёт поток телетекста кроме страниц: виды пакетов и служебные данные.
#   python service_packets.py поток.t42 [ещё.t42 ...]
# Пакеты (ETSI EN 300 706 §7, §9):
#   X/0 — заголовок страницы; X/1–23 — ряды; X/24 — ряд подсказок FLOF; X/25 — замена ряда 24;
#   X/26 — поправки символов (Hamming 24/18); X/27 — ссылки (FLOF);
#   X/28 — расширения страницы (наборы символов, цвета); M/29 — то же для магазина;
#   8/30 — служебные данные вещателя (§9.8): формат 1 — начальная страница, код сети (NI),
#          часовой пояс, дата (MJD), время UTC и строка статуса (название программы/канала);
#          формат 2 — метка программы (PDC, как VPS); M/30 других магазинов и X/31 — независимые
#          данные (передача файлов, подписи, служебная информация канала).
import collections, os, sys
import numpy as np
H8 = {}
for dd in range(16):
    b = [(dd >> i) & 1 for i in range(4)]
    p1 = 1 ^ b[0] ^ b[2] ^ b[3]; p2 = 1 ^ b[0] ^ b[1] ^ b[3]; p3 = 1 ^ b[0] ^ b[1] ^ b[2]
    p4 = 1 ^ (p1 ^ p2 ^ p3 ^ b[0] ^ b[1] ^ b[2] ^ b[3])
    H8[sum(v << i for i, v in enumerate([p1, b[0], p2, b[1], p3, b[2], p4, b[3]]))] = dd
REV = [int(f'{i:08b}'[::-1], 2) for i in range(256)]
odd = lambda x: bin(int(x)).count('1') % 2 == 1

KINDS = {0: 'X/0 page headers', 24: 'X/24 FLOF prompt row', 25: 'X/25 row 24 replacement',
         26: 'X/26 character enhancements', 27: 'X/27 FLOF links', 28: 'X/28 page enhancements',
         29: 'M/29 magazine enhancements', 30: 'M/30 service data', 31: 'X/31 independent data'}

def mjd_date(m):
    """MJD (модифицированная юлианская дата) -> 'ГГГГ-ММ-ДД' (ETSI EN 300 468 приложение C)."""
    yp = int((m - 15078.2) / 365.25); mp = int((m - 14956.1 - int(yp * 365.25)) / 30.6001)
    d = m - 14956 - int(yp * 365.25) - int(mp * 30.6001); k = 1 if mp in (14, 15) else 0
    return f'{1900 + yp + k:04}-{mp - 1 - k * 12:02}-{d:02}'

def bcd_nibbles(bs, plus1=True):
    """Время и дата 8/30 передаются BCD с прибавкой 1 к каждой цифре (§9.8.1)."""
    out = []
    for x in bs:
        for v in ((x >> 4) & 15, x & 15): out.append((v - 1) % 16 if plus1 else v)
    return out

def survey(path):
    st = np.fromfile(path, np.uint8); st = st[:len(st) // 42 * 42].reshape(-1, 42)
    kinds = collections.Counter(); n = len(st); bad = 0
    f1 = collections.defaultdict(collections.Counter); f2 = collections.Counter(); x31 = collections.Counter()
    for p in st:
        a, b = H8.get(int(p[0])), H8.get(int(p[1]))
        if a is None or b is None: bad += 1; continue
        mag = a & 7 or 8; row = (a >> 3) | (b << 1)
        kinds['X/1–23 page rows' if 1 <= row <= 23 else KINDS.get(row, f'X/{row}')] += 1
        if row == 30 and mag == 8:
            dc = H8.get(int(p[2]))
            if dc is None: continue
            if dc >> 1 == 0:                                  # формат 1
                ni = (REV[p[9]] << 8) | REV[p[10]]
                f1['network code (NI)'][f'{ni:04X}'] += 1
                mjd = bcd_nibbles(p[12:15])
                if all(0 <= v <= 9 for v in mjd[1:]):
                    m = int(''.join(map(str, mjd[1:]))); f1['date'][mjd_date(m)] += 1
                tm = bcd_nibbles(p[15:18])
                if all(0 <= v <= 9 for v in tm): f1['UTC time'][f'{tm[0]}{tm[1]}:{tm[2]}{tm[3]}'] += 1
                off = p[11]; sign = '-' if off & 0x40 else '+'
                f1['time zone']['{0}{1:g} h'.format(sign, ((off >> 1) & 0x1f) / 2)] += 1
                if all(odd(x) for x in p[22:42]):
                    f1['status row'][bytes(int(x) & 0x7f for x in p[22:42]).decode('latin-1').strip()] += 1
            else:
                f2['format 2 (PDC — programme label)'] += 1
        elif row == 31:
            x31['magazine {0}'.format(mag)] += 1
    print('\n=== {0}: {1} packets, address unreadable in {2:.1%}'.format(os.path.basename(path), n, bad / max(1, n)))
    for k, c in sorted(kinds.items(), key=lambda x: -x[1]): print(f'  {k:28} {c:8} ({c / max(1, n):.2%})')
    for k, c in f1.items():
        print('  8/30 format 1 {0}: '.format(k) + '; '.join(f'{v} ×{m}' for v, m in c.most_common(4)))
    for k, c in f2.items(): print(f'  8/30 {k}: {c}')
    if x31: print('  X/31 by magazine:', dict(x31))

if __name__ == '__main__':
    for f in sys.argv[1:]: survey(f)
