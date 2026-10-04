"""Зашифрованная пакетная рассылка 5,7273 Мбит/с (вероятно, PBS National Datacast) — запись WTTW 1989,
строки 20 и 22. Формат найден по записи:
  вступление 0x55 0x55, код кадра 0x2D (младший бит первым), затем 34 байта:
    биты 0–3   всегда 1100;
    биты 4–15  12-битный адрес (получатель/канал);
    байты 2–32 31 байт данных — зашифрованы (энтропия 8 бит/байт, без чётности и структуры);
    байт 33    0x00.
  Пакеты идут пачками и по кругу (карусель), каждый повторяется несколько раз.
Расшифровать данные без ключа нельзя; извлекается всё остальное: пакеты (голосование по копиям),
адреса, их порядок и период повторения, расписание пачек.

Сигнал с ленты VHS: полоса ~2 МГц при 5,7 Мбит/с, поэтому чтение — MLSE (Витерби) с моделью канала,
подобранной вслепую по самим строкам (как в decode-orc для NABTS, но без известного префикса).

  python pages/datacast.py запись.vbi [папка] [--lines 22,20]
Вывод: «<запись>_datacast»: index.html (отчёт), report.txt, packets.csv, addresses.csv,
packets_34byte.bin, packets.json.
"""
import collections, csv, html, json, os, sys, time
from multiprocessing import Pool
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vbi_probe as VP

RATE = 8 * VP.FSC / 5
K = 5                       # память канала, бит в каждую сторону
PH = np.array([-0.25, 0.25])  # два отсчёта на бит
NTAP = 2 * K + 1; NS = 1 << (NTAP - 1)
_ST = ((np.arange(1 << NTAP)[:, None] >> np.arange(NTAP - 1, -1, -1)[None, :]) & 1) * 2.0 - 1
_HI = ((np.arange(1 << NTAP) >> 1) >> (NTAP - 2)) & 1
HDR = np.array([int(c) for c in '1010101010101010' + '10110100'], np.uint8)
PKT_BITS = 272
FIELD_S = 1001 / 60000.0


def log(*a):
    print(*a, flush=True)


def viterbi(Y, C):
    pred = _ST @ C[:, :NTAP].T + C[:, NTAP]
    nb = len(Y); M = np.zeros(NS); back = np.zeros((nb, NS), np.uint8)
    f0 = np.arange(NS); f1 = f0 | NS
    for i in range(nb):
        bm = ((Y[i][None, :] - pred) ** 2).sum(1)
        v = M[np.arange(1 << NTAP) >> 1] + bm
        c0 = v[f0]; c1 = v[f1]; ch = c1 < c0
        M = np.where(ch, c1, c0); back[i] = _HI[np.where(ch, f1, f0)]
    s = int(np.argmin(M)); out = np.zeros(nb + NTAP - 1, np.uint8)
    for i in range(nb - 1, -1, -1):
        out[i + NTAP - 1] = s & 1; s = (s >> 1) | (int(back[i][s]) << (NTAP - 2))
    for k in range(NTAP - 1):
        out[k] = (s >> (NTAP - 2 - k)) & 1
    return out


class Line:
    """Отсчёты строки -> векторы по битам (с тактом по переходам через ноль)."""
    def __init__(self, fs, ns):
        self.T = fs / RATE
        self.lo, self.hi = int(ns * 0.154), int(ns * 0.967)

    def prep(self, y):
        T, lo, hi = self.T, self.lo, self.hi
        s = y[lo:hi]; w = int(T * 12) | 1
        d = s - np.convolve(s, np.ones(w) / w, 'same')
        z = np.nonzero(np.sign(d[1:]) != np.sign(d[:-1]))[0]
        if len(z) < 20:
            return None
        t = z + d[z] / (d[z] - d[z + 1]) + lo
        t0 = np.angle(np.exp(2j * np.pi * t / T).mean()) / (2 * np.pi) * T
        c0 = t0 + np.ceil((lo - t0) / T) * T
        nb = int((hi - c0) / T)
        pos = c0 + (np.arange(-K, nb + K)[:, None] + 0.5 + PH[None, :]) * T
        Y = np.interp(pos, np.arange(len(y)), y)
        m, sd = y[lo + 60:hi].mean(), y[lo + 60:hi].std()
        return (Y - m) / (sd or 1)


def train(R, row, units, L, log=log):
    """Слепая подгонка модели канала (решения Витерби <-> наименьшие квадраты)."""
    Ys = [Y for Y in (L.prep(R.line(u, row)) for u in units) if Y is not None][:40]
    d = np.arange(-K, K + 1); C = np.zeros((len(PH), NTAP + 1))
    for p in range(len(PH)):
        C[p, :NTAP] = np.exp(-0.5 * ((d - PH[p]) / 0.9) ** 2); C[p, :NTAP] /= C[p, :NTAP].sum() * 0.6
    for it in range(8):
        X = []; Yc = []
        for Y in Ys:
            b = viterbi(Y, C) * 2.0 - 1
            X.append(np.c_[np.stack([b[np.arange(len(Y)) + j] for j in range(NTAP)], 1), np.ones(len(Y))]); Yc.append(Y)
        X = np.concatenate(X); Yc = np.concatenate(Yc)
        C = np.linalg.lstsq(X, Yc, rcond=None)[0].T
    log('  channel model for line {0}: residual {1:.3f}'.format(R.tv[row], float((Yc - X @ C.T).std())))
    return C


_W = {}


def _init(path, fmt, row, C):
    _W['R'] = VP.Rec(path, fmt); _W['row'] = row; _W['C'] = C
    _W['L'] = Line(_W['R'].fs, _W['R'].ns)


def _work(units):
    R, row, C, L = _W['R'], _W['row'], _W['C'], _W['L']
    out = []
    for u in units:
        Y = L.prep(R.line(u, row))
        if Y is None:
            continue
        b = viterbi(Y, C)
        sc = [int((b[s:s + 24] != HDR).sum()) for s in range(0, 30)]
        s = int(np.argmin(sc))
        if sc[s] <= 3 and s + 24 + PKT_BITS <= len(b):
            out.append((u, b[s + 24:s + 24 + PKT_BITS].copy()))
    return out


def active_units(R, row, par):
    """Поля (кадры), где в строке есть данные: размах заметно выше обычного для этой строки."""
    uu = list(range(par or 0, R.n, 2 if par is not None else 1))
    lo, hi = int(R.ns * 0.33), int(R.ns * 0.93)
    sd = np.array([R.line(u, row)[lo:hi].std() for u in uu], np.float32)
    q1, q9 = np.percentile(sd, 10), np.percentile(sd, 95)
    return [u for u, s in zip(uu, sd) if s > (q1 + q9) / 2]


def read(path, lines=None, log=log, progress=None):
    """-> (пакеты: список (единица, строка ТВ, биты 272)), Rec."""
    fmt, _ = VP.detect_format(path)
    R = VP.Rec(path, fmt)
    if lines is None:
        res = VP.probe(path, log=lambda *a: None)
        lines = [(L['row'], L['parity']) for L in res['lines'] if L['kind'].startswith('encrypted datacast')]
    pk = []
    for row, par in lines:
        uu = active_units(R, row, par)
        if len(uu) < 20:
            continue
        log('STEP Reading line {0}{1}: {2} {3}s with data'.format(R.tv[row], '' if par is None else ' field ' + 'AB'[par], len(uu), R.unit))
        C = train(R, row, uu[::max(1, len(uu) // 40)], Line(R.fs, R.ns), log)
        chunks = [uu[i:i + 200] for i in range(0, len(uu), 200)]
        done = 0
        with Pool(max(1, (os.cpu_count() or 2) - 1), _init, (path, fmt, row, C)) as P:
            for part in P.imap(_work, chunks):
                pk += [(u, R.tv[row], b) for u, b in part]; done += 1
                print('PROGRESS {0} {1}'.format(done, len(chunks)), flush=True)
    return pk, R


def vote(pk, maxdiff=21):
    """Копии одного пакета (расхождение ≤ maxdiff бит) -> голосование."""
    if not pk:
        return []
    U = np.array([p[0] for p in pk]); Ln = np.array([p[1] for p in pk]); D = np.array([p[2] for p in pk])
    P = np.packbits(D, axis=1)
    pc = np.unpackbits(np.arange(256, dtype=np.uint8)[:, None], axis=1).sum(1)
    lab = -np.ones(len(P), int); c = 0
    for i in range(len(P)):
        if lab[i] >= 0: continue
        m = (pc[P[i] ^ P].sum(1) <= maxdiff) & (lab < 0); lab[m] = c; c += 1
    out = []
    for k in range(c):
        m = lab == k; g = D[m]; v = (g.mean(0) > 0.5).astype(np.uint8)
        out.append({'units': sorted(U[m].tolist()), 'lines': sorted(set(Ln[m].tolist())), 'bits': v,
                    'agree': float(1 - (g != v).mean())})
    out.sort(key=lambda p: p['units'][0])
    for p in out:
        b = np.packbits(p['bits'], bitorder='little')
        p['bytes'] = bytes(b)
        p['marker'] = int(p['bits'][:4].dot(1 << np.arange(4)))
        p['address'] = int(p['bits'][4:16].dot(1 << np.arange(12)))
        p['payload'] = bytes(b[2:33])
    return out


def fix_headers(packets):
    """Заголовок (маркер + адрес, 16 бит) не защищён кодом: редкий заголовок, отличающийся от частого
    на 1–2 бита, — ошибка чтения; исправляется на частый (p['fixed'] = True)."""
    hd = lambda p: p['marker'] | p['address'] << 4
    cnt = collections.Counter(hd(p) for p in packets)
    tot = len(packets)
    good = [h for h, n in cnt.items() if n >= max(5, 0.002 * tot)]
    for p in packets:
        h = hd(p)
        if h in good or len(p['units']) >= 3:           # заголовок из голосования копий надёжен
            continue
        d = [(bin(h ^ g).count('1'), -cnt[g], g) for g in good]
        if d and min(d)[0] <= 2:
            g = min(d)[2]; p['marker'], p['address'], p['fixed'] = g & 15, g >> 4, True
    return packets


def bursts(units, gap):
    """Единицы с данными -> пачки (начало, конец) при разрыве > gap."""
    if not units: return []
    u = sorted(units); out = [[u[0], u[0]]]
    for x in u[1:]:
        if x - out[-1][1] > gap: out.append([x, x])
        else: out[-1][1] = x
    return out


def analyse(packets, R, raw_units):
    """Всё, что видно без ключа."""
    per = R.unit == 'field' and FIELD_S or 2 * FIELD_S
    A = collections.OrderedDict()
    for p in packets:
        a = A.setdefault(p['address'], {'packets': 0, 'copies': 0, 'lines': set(), 'first': p['units'][0], 'last': p['units'][-1], 'gaps': []})
        a['packets'] += 1; a['copies'] += len(p['units']); a['lines'].update(p['lines'])
        a['first'] = min(a['first'], p['units'][0]); a['last'] = max(a['last'], p['units'][-1])
        a['gaps'] += np.diff(p['units']).tolist()
    for a in A.values():
        g = [x for x in a['gaps'] if x > 60]
        a['repeat_s'] = round(float(np.median(g)) * per, 1) if g else None
    gaps = collections.Counter(int(x) for p in packets for x in np.diff(p['units']) if x > 60)
    br = bursts(raw_units, 6)
    bl = [b[1] - b[0] + 1 for b in br]; bg = [br[i + 1][0] - br[i][1] for i in range(len(br) - 1)]
    rep = {'packets': len(packets), 'copies': sum(len(p['units']) for p in packets),
           'addresses': len(A), 'markers': collections.Counter(p['marker'] for p in packets).most_common(),
           'agree': round(float(np.mean([p['agree'] for p in packets])), 4) if packets else None,
           'carousel_s': [(round(k * per, 1), v) for k, v in gaps.most_common(6)],
           'bursts': len(br), 'burst_len_s': round(float(np.median(bl)) * per, 2) if bl else None,
           'burst_gap_s': round(float(np.median(bg)) * per, 2) if bg else None,
           'unit_s': per}
    # энтропия данных (признак шифрования)
    if packets:
        X = np.frombuffer(b''.join(p['payload'] for p in packets), np.uint8).reshape(len(packets), 31)
        def H(x):
            c = np.bincount(x, minlength=256) / len(x); c = c[c > 0]; return float(-(c * np.log2(c)).sum())
        rep['payload_entropy'] = round(float(np.mean([H(X[:, i]) for i in range(31)])), 2)
        rep['max_entropy'] = round(float(np.log2(min(256, len(packets)))), 2)
    rep['channels'] = channels(packets, per)
    rep['contigs'] = assemble(packets)
    return A, rep, br


def channels(packets, per):
    """Виды пакетов (маркер) -> карусель (пакеты повторяются) или поток (блоки адресов по кругу).
    Маркеры, которых меньше 1 %, — скорее ошибки чтения (маркер искажён)."""
    out = []
    tot = len(packets)
    for m, n in collections.Counter(p['marker'] for p in packets).most_common():
        g = sorted((p for p in packets if p['marker'] == m), key=lambda p: p['units'][0])
        ch = {'marker': '%X' % m, 'packets': n, 'lines': sorted(set(l for p in g for l in p['lines'])),
              'addresses': len(set(p['address'] for p in g)),
              'copies_avg': round(float(np.mean([len(p['units']) for p in g])), 1)}
        if n < 0.01 * tot:
            ch['kind'] = 'rare (probably read errors in the marker)'
        elif ch['copies_avg'] >= 1.5:
            gaps = [x for p in g for x in np.diff(p['units']) if x > 60]
            ch['kind'] = 'carousel'
            ch['repeat_s'] = round(float(np.median(gaps)) * per, 1) if gaps else None
            ch['bytes'] = n * 31
        else:
            seq = [p['address'] for p in g]
            best = None
            sc = [(np.mean([seq[i] == seq[i + L] for i in range(len(seq) - L)]) if len(seq) > L else 0, L) for L in range(2, 65)]
            top = max(sc)[0]
            best = min(L for v, L in sc if v >= 0.9 * top) if top > 0.4 else None
            ch['kind'] = 'stream'
            if best:
                # порядок блока — самый частый в окнах длиной best
                order = list(collections.Counter(tuple(seq[i:i + best]) for i in range(0, len(seq) - best, 1)
                                                 if len(set(seq[i:i + best])) == best).most_common(1)[0][0])
                d = []
                for a in order:
                    t = sorted(p['units'][0] for p in g if p['address'] == a)
                    d += [x for x in np.diff(t) if x > 20]
                blk = float(np.median(d)) * per if d else None
                if d: ch['block_range_s'] = [round(float(np.percentile(d, 15)) * per, 1), round(float(np.percentile(d, 85)) * per, 1)]
                ch['block'] = ['%03X' % a for a in order]
                ch['block_s'] = round(blk, 2) if blk else None
                if blk: ch['bit_s'] = round(best * 31 * 8 / blk)
        if ch['kind'] == 'stream':
            ch['fixed_bits'] = fixed_bits(g)
        out.append(ch)
    return out


def fixed_bits(g, min_n=50):
    """Биты данных (после адреса), одинаковые почти во всех пакетах каждого адреса — открытые поля
    заголовка (WTTW: тип 9 — бит 0, тип B — биты 2–6); остальное меняется на ~50 % от пакета к пакету."""
    A = collections.defaultdict(list)
    for p in g:
        A[p['address']].append(np.unpackbits(np.frombuffer(p['payload'], np.uint8), bitorder='little')[:244])
    common = None
    for xs in A.values():
        if len(xs) < min_n: continue
        m = np.array(xs).mean(0)
        f = set(np.flatnonzero((m > 0.97) | (m < 0.03)).tolist())
        common = f if common is None else common & f
    return sorted(common) if common else []


def assemble(packets, k=8):
    """Карусель (повторяющиеся пакеты) — кольцевой поток байтов: на каждом проходе границы пакетов
    сдвигаются, поэтому одни и те же байты приходят в разных пакетах со сдвигом. Пакеты (30 байт данных,
    последний полубайт — не данные) склеиваются по точным перекрытиям в непрерывные фрагменты."""
    big = [m for m, n in collections.Counter(p['marker'] for p in packets).items() if n >= 0.01 * len(packets)]
    rep = [p for p in packets if p['marker'] in big and len(p['units']) >= 2]
    C = list(dict.fromkeys(p['payload'][:30] for p in rep))
    if not C:
        return []
    idx = collections.defaultdict(list)
    for i, c in enumerate(C):
        idx[c[:k]].append(i)
    nxt = {}
    for i, c in enumerate(C):
        best = None
        for o in range(1, len(c) - k + 1):
            for j in idx.get(c[o:o + k], ()):
                L = len(c) - o
                if j != i and c[o:] == C[j][:L] and (best is None or L > best[1]):
                    best = (j, L)
        if best: nxt[i] = best
    has_prev = set(j for j, _ in nxt.values())
    out = []
    for s0 in (i for i in range(len(C)) if i not in has_prev):
        seq = bytearray(C[s0]); i = s0; seen = {s0}
        while i in nxt and nxt[i][0] not in seen:
            j, L = nxt[i]; seq += C[j][L:]; seen.add(j); i = j
        out.append(bytes(seq))
    out.sort(key=len, reverse=True)
    return out


def save(src, out, packets, R, raw_units):
    os.makedirs(out, exist_ok=True)
    A, rep, br = analyse(packets, R, raw_units)
    per = rep['unit_s']
    ct = rep.pop('contigs')
    rep['contig_n'] = len(ct); rep['contig_bytes'] = sum(len(c) for c in ct)
    rep['contig_max'] = len(ct[0]) if ct else 0
    with open(os.path.join(out, 'carousel_stream.bin'), 'wb') as fh:      # фрагменты подряд, длинные первыми
        for c in ct: fh.write(c)
    with open(os.path.join(out, 'carousel_stream.txt'), 'w') as fh:
        for i, c in enumerate(ct):
            fh.write('fragment {0}: {1} bytes\n'.format(i + 1, len(c)))
            for o in range(0, len(c), 32): fh.write('  {0:05x}  {1}\n'.format(o, c[o:o + 32].hex(' ')))
    with open(os.path.join(out, 'packets_34byte.bin'), 'wb') as fh:
        for p in packets: fh.write(p['bytes'])
    with open(os.path.join(out, 'packets.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['first_' + R.unit, 'time_s', 'copies', 'vbi_lines', 'address (* = corrected)', 'agree', 'payload_hex'])
        for p in packets:
            w.writerow([p['units'][0], round(p['units'][0] * per, 2), len(p['units']), '/'.join(map(str, p['lines'])),
                        '%03X' % p['address'] + ('*' if p.get('fixed') else ''), round(p['agree'], 3), p['payload'].hex()])
    with open(os.path.join(out, 'addresses.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['address', 'packets', 'copies', 'vbi_lines', 'first_s', 'last_s', 'repeat_s'])
        for a, v in sorted(A.items(), key=lambda kv: -kv[1]['packets']):
            w.writerow(['%03X' % a, v['packets'], v['copies'], '/'.join(map(str, sorted(v['lines']))),
                        round(v['first'] * per, 1), round(v['last'] * per, 1), v['repeat_s']])
    txt = report_text(src, R, rep, A)
    open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8').write(txt)
    json.dump({'source': os.path.basename(src), 'summary': rep,
               'addresses': {'%03X' % a: {k: (sorted(v) if isinstance(v, set) else v) for k, v in d.items() if k != 'gaps'} for a, d in A.items()},
               'packets': [{'units': p['units'], 'lines': p['lines'], 'address': '%03X' % p['address'],
                            'payload': p['payload'].hex(), 'agree': round(p['agree'], 3)} for p in packets]},
              open(os.path.join(out, 'packets.json'), 'w', encoding='utf-8'))
    open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(page(src, R, rep, A, packets, br))
    log(txt)
    return out


def report_text(src, R, rep, A):
    L = ['Encrypted datacast (5.727 Mbit/s, probably PBS National Datacast) — ' + os.path.basename(src), '']
    L.append('Packet: run-in 55 55, framing 2D, 34 bytes: marker (4 bits) + address (12 bits) + 31 bytes data + 00')
    L.append('Distinct packets: {0} (from {1} received copies; copies agree on {2:.1%} of bits)'.format(rep['packets'], rep['copies'], rep['agree'] or 0))
    L.append('Addresses: {0}; markers: {1}'.format(rep['addresses'], ', '.join('{0:X} x{1}'.format(m, n) for m, n in rep['markers'])))
    L.append('Bursts: {0}; a burst typically lasts {1} s, pauses between bursts {2} s'.format(rep['bursts'], rep['burst_len_s'], rep['burst_gap_s']))
    L.append('Carousel: a packet comes again after ' + ', '.join('{0} s ({1} times)'.format(s, n) for s, n in rep['carousel_s']))
    L.append('')
    L.append('Channels (by packet type):')
    for c in rep['channels']:
        t = '  type {0}: {1} packets, line {2}, {3} addresses — {4}'.format(c['marker'], c['packets'], '/'.join(map(str, c['lines'])), c['addresses'], c['kind'])
        if c['kind'] == 'carousel':
            t += ': each packet sent {0} times on average, again after ~{1} s ({2} bytes of data in all)'.format(c['copies_avg'], c['repeat_s'], c['bytes'])
        elif c.get('block'):
            t += ': blocks of {0} packets (addresses {1}) every {2}–{3} s — {4} bytes per block, ~{5} bit/s'.format(
                len(c['block']), ' '.join(c['block']), *(c.get('block_range_s') or [c['block_s']] * 2), 31 * len(c['block']), c.get('bit_s'))
        L.append(t)
        if c.get('fixed_bits'):
            L.append('      data bits that never change for an address (an open header field): ' + ', '.join(map(str, c['fixed_bits'])))
    L.append('')
    if 'payload_entropy' in rep:
        L.append('Data: {0} bits per byte (max {1} for this many packets) — no structure: encrypted, cannot be read without the key'.format(rep['payload_entropy'], rep['max_entropy']))
    L.append('')
    if rep.get('contig_n'):
        L.append('Carousel as a byte stream: the same bytes come again in other packets shifted by whole bytes, so the')
        L.append('carousel is a looping file; reassembled into {0} continuous fragments, {1} bytes in all, longest {2} bytes'.format(
            rep['contig_n'], rep['contig_bytes'], rep['contig_max']))
        L.append('(carousel_stream.bin / .txt). The file was coded as a whole before being cut into packets; it is dense')
        L.append('binary: no archive signature, not a raster, no self-synchronising scrambler (taps up to 25), no short LFSR')
        L.append('(Berlekamp-Massey), no async start/stop framing, no 256-byte table — encrypted or compressed.')
        L.append('')
    L.append('Busiest addresses (address: packets, lines, repeat period):')
    for a, v in sorted(A.items(), key=lambda kv: -kv[1]['packets'])[:25]:
        L.append('  {0:03X}: {1:4} packets, line {2}, repeats every {3} s'.format(a, v['packets'], '/'.join(map(str, sorted(v['lines']))), v['repeat_s']))
    return '\n'.join(L) + '\n'


def page(src, R, rep, A, packets, br):
    per = rep['unit_s']; tot = R.n * per
    rows = ''.join('<tr><td>{0:03X}</td><td>{1}</td><td>{2}</td><td>{3}</td><td>{4}</td></tr>'.format(
        a, v['packets'], v['copies'], '/'.join(map(str, sorted(v['lines']))), v['repeat_s'] or '')
        for a, v in sorted(A.items(), key=lambda kv: -kv[1]['packets']))
    bars = ''.join('<div class="b" style="left:{0:.3f}%;width:{1:.3f}%"></div>'.format(
        100 * b[0] * per / tot, max(0.15, 100 * (b[1] - b[0] + 1) * per / tot)) for b in br)
    pk = ''.join('<tr><td>{0:.2f}</td><td>{1}</td><td>{2:03X}</td><td>{3}</td><td class="h">{4}</td></tr>'.format(
        p['units'][0] * per, len(p['units']), p['address'], '/'.join(map(str, p['lines'])),
        ' '.join(p['payload'].hex()[i:i + 2] for i in range(0, 62, 2))) for p in packets[:3000])
    summ = '<pre>' + html.escape(report_text(src, R, rep, A).split('Busiest addresses')[0].strip()) + '</pre>'
    return '''<!doctype html><html><head><meta charset="utf-8"><title>Datacast</title><style>
body{{background:#111;color:#ccc;font:14px sans-serif;margin:16px}} h2{{color:#fa4}}
table{{border-collapse:collapse;font:12px Consolas,monospace}} td,th{{border:1px solid #333;padding:2px 6px}}
.tl{{position:relative;height:24px;background:#222;margin:8px 0}} .b{{position:absolute;top:0;bottom:0;background:#fa4}}
.h{{color:#888}} pre{{white-space:pre-wrap;font:13px Consolas,monospace}} .wrap{{max-height:50vh;overflow:auto;display:inline-block}}</style></head><body>
<h2>Encrypted datacast — {0}</h2>{1}
<p>Data bursts over the recording ({2:.0f} s):</p><div class="tl">{3}</div>
<h2>Addresses</h2><div class="wrap"><table><tr><th>address</th><th>packets</th><th>copies</th><th>VBI line</th><th>repeat, s</th></tr>{4}</table></div>
<h2>Packets</h2><div class="wrap"><table><tr><th>time, s</th><th>copies</th><th>address</th><th>line</th><th>31 bytes of data (encrypted)</th></tr>{5}</table></div>
</body></html>'''.format(html.escape(os.path.basename(src)), summ, tot, bars, rows, pk)


def export(src, out=None, lines=None):
    out = out or os.path.splitext(src)[0] + '_datacast'
    t = time.time()
    pk, R = read(src, lines)
    log('STEP Combining copies of {0} packets'.format(len(pk)))
    packets = fix_headers(vote(pk))
    raw_units = sorted(set(u for u, _, _ in pk))
    save(src, out, packets, R, raw_units)
    log('done in {0:.0f} s, written to {1}'.format(time.time() - t, out))
    return out


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else None
    lines = None
    if '--lines' in sys.argv:
        fmt, _ = VP.detect_format(src); R = VP.Rec(src, fmt)
        want = [int(x) for x in sys.argv[sys.argv.index('--lines') + 1].split(',')]
        rows = [r for r in R.rows if R.tv[r] in want]
        lines = [(r, p) for r in rows for p in ((0, 1) if R.unit == 'field' else (None,))]
    export(src, out, lines)


if __name__ == '__main__':
    main()
