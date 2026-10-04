"""NABTS (.t33) — разбор и показ страниц NAPLPS (CBS ExtraVision, NBC Teletext).

Порт decode-orc 2.9.2, orc/plugins/stages/nabts_sink:
  nabts_packet.cpp       — пакет 33 байта (CEA-516 §3)
  nabts_data_group.cpp   — сборка групп данных (§4)
  nabts_record.cpp       — заголовки записей, связанные серии (§5)
  nabts_record_catalogue — каталог записей, голосование копий, цепочки More
  naplps_*.cpp           — интерпретатор NAPLPS (ANSI X3.110) и растр приёмника

Запуск:  python pages/nabts.py файл.t33 [папка]  → PNG и текст каждой записи.
"""
import math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import naplps_font

# ---------------------------------------------------------------------------
# Hamming 8/4 и чётность (ETSI EN 300 706 §8.2, как teletext_slicer)
# ---------------------------------------------------------------------------
_CODEWORDS = [0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
              0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA]
HAM = [-1] * 256      # значение или -1 (не исправить)
HAM_CLEAN = [False] * 256
for _b in range(256):
    for _v, _c in enumerate(_CODEWORDS):
        _d = bin(_b ^ _c).count('1')
        if _d == 0:
            HAM[_b] = _v; HAM_CLEAN[_b] = True
        elif _d == 1:
            HAM[_b] = _v
ODD = [bin(_b).count('1') & 1 == 1 for _b in range(256)]

PACKET = 33
PREFIX = 5
MAX_BLOCK = 28
DISPLAY_H = 0.78125            # X3.110 Table D1: нижние 0.78125 единичного экрана

# ---------------------------------------------------------------------------
# Пакеты (§3)
# ---------------------------------------------------------------------------
class Packet:
    __slots__ = ('valid', 'channel', 'attested', 'ci', 'ci_byte', 'sync', 'not_full',
                 'suffix', 'data', 'integrity')


def decode_packet(p):
    out = Packet()
    out.valid = False
    pre = [HAM[b] for b in p[:PREFIX]]
    if min(pre) < 0:
        return out
    out.valid = True
    out.channel = (pre[0] << 8) | (pre[1] << 4) | pre[2]
    out.attested = HAM_CLEAN[p[0]] and HAM_CLEAN[p[1]] and HAM_CLEAN[p[2]]
    out.ci = pre[3]
    out.ci_byte = p[3]
    ps = pre[4]
    out.sync = bool(ps & 1)
    out.not_full = bool(ps & 2)
    out.suffix = ((ps >> 3) & 1) << 1 | ((ps >> 2) & 1)   # 0 нет, 1 LRC, 2 LRC+резерв, 3 пачка
    n = MAX_BLOCK - (0, 1, 2, MAX_BLOCK)[out.suffix]
    data = bytearray(p[PREFIX:PREFIX + n])
    out.integrity = 'unchecked'
    if out.suffix in (1, 2):
        blk = p[PREFIX:PREFIX + MAX_BLOCK]
        lrc = 0
        for b in blk:
            lrc ^= b
        syn = lrc ^ 0xFF
        if syn == 0:
            out.integrity = 'clean'
        elif syn & (syn - 1) == 0:
            bad = [i for i, b in enumerate(blk) if not ODD[b]]
            if len(bad) == 1:
                if bad[0] < n:
                    data[bad[0]] ^= syn
                out.integrity = 'corrected'
            else:
                out.integrity = 'damaged'
        else:
            out.integrity = 'damaged'
    out.data = bytes(data)
    return out

# ---------------------------------------------------------------------------
# Группы данных (§4)
# ---------------------------------------------------------------------------
GROUP_HDR = 8
MAX_FURTHER = 67
MAX_OPEN_GROUPS = 32


class Group:
    pass


def decode_group_header(b):
    if len(b) < GROUP_HDR:
        return None
    n = [HAM[x] for x in b[:GROUP_HDR]]
    if min(n) < 0:
        return None
    h = Group()
    h.attested = all(HAM_CLEAN[x] for x in b[:GROUP_HDR])
    h.type, h.ci, h.rep = n[0], n[1], n[2]
    h.further = (n[3] << 4) | n[4]
    h.final_bytes = (n[5] << 4) | n[6]
    h.routing = n[7]
    return h


class GroupAssembler:
    def __init__(self, callback, lines_known=False):
        # В .t33 нет пустых строк VBI, поэтому число строк между пакетами канала
        # неизвестно, и пропуск, названный индексом непрерывности, принимается как есть
        # (в decode-orc его ограничивает счётчик строк записи).
        self.cb = callback
        self.lines_known = lines_known
        self.open = {}
        self.clock = 0
        self.stats = dict(packets=0, prefix_bad=0, header_bad=0, orphans=0,
                          complete=0, superseded=0, unfinished=0, foreign=0)

    def _append(self, g, pk):
        if not pk.data:
            return
        g.last_off = len(g.stream); g.last_len = len(pk.data)
        g.stream += pk.data
        g.present += bytes([1 if g.placeable else 0]) * len(pk.data)

    def _hole(self, g, blocks):
        n = blocks * g.nominal
        g.stream += bytes(n); g.present += bytes(n)

    def _emit(self, ch, g, outcome):
        end = g.last_off + g.last_len
        if g.hdr.final_bytes == 0:
            end = g.last_off
        elif g.hdr.final_bytes < g.last_len:
            end = g.last_off + g.hdr.final_bytes
        end = min(end, len(g.stream))
        out = Group()
        out.channel = ch; out.channel_attested = g.attested; out.hdr = g.hdr
        out.outcome = outcome; out.lost = g.lost; out.damaged = g.damaged
        out.data = bytes(g.stream[GROUP_HDR:end]) if end > GROUP_HDR else b''
        out.present = bytes(g.present[GROUP_HDR:end]) if end > GROUP_HDR else b''
        out.intact = outcome == 'complete' and g.lost == 0 and g.damaged == 0
        out.clock = self.clock
        self.stats[outcome] += 1
        if outcome == 'complete' and g.hdr.type != 0:
            self.stats['foreign'] += 1
        self.cb(out)

    def add(self, pk):
        self.stats['packets'] += 1
        self.clock += 1
        if not pk.valid:
            self.stats['prefix_bad'] += 1
            return
        if pk.sync:
            self._begin(pk)
        else:
            self._extend(pk)

    def _begin(self, pk):
        h = decode_group_header(pk.data)
        if h is None:
            self.stats['header_bad'] += 1
            return
        if h.further > MAX_FURTHER:
            return
        if pk.channel in self.open:
            self._emit(pk.channel, self.open.pop(pk.channel), 'superseded')
        elif len(self.open) >= MAX_OPEN_GROUPS:
            return
        g = Group()
        g.hdr = h; g.attested = pk.attested; g.last_ci = pk.ci; g.last_line = self.clock
        g.stream = bytearray(); g.present = bytearray(); g.nominal = len(pk.data)
        g.placeable = True; g.seen = 0; g.last_off = 0; g.last_len = 0
        g.lost = 0; g.damaged = 1 if pk.integrity == 'damaged' else 0
        self._append(g, pk)
        if h.further == 0:
            self._emit(pk.channel, g, 'complete')
            return
        self.open[pk.channel] = g

    def _gap(self, g, pk, lines):
        exp = (g.last_ci + 1) % 16
        read = (pk.ci - exp) % 16
        if read <= lines:
            return read
        room = g.hdr.further - g.seen
        limit = min(lines, room, 15)
        best, bestd = 0, 99
        for c in range(limit + 1):
            d = bin(pk.ci_byte ^ _CODEWORDS[(exp + c) % 16]).count('1')
            if d < bestd:
                best, bestd = c, d
        return best

    def _extend(self, pk):
        g = self.open.get(pk.channel)
        if g is None:
            self.stats['orphans'] += 1
            return
        lines = self.clock - g.last_line - 1 if self.lines_known else 15
        gap = self._gap(g, pk, lines)
        if gap:
            g.lost += gap
            room = g.hdr.further - g.seen
            g.seen = min(g.seen + gap, g.hdr.further)
            if gap > room:
                g.placeable = False
            else:
                self._hole(g, gap)
        g.last_ci = (g.last_ci + 1 + gap) % 16
        g.last_line = self.clock
        if pk.integrity == 'damaged':
            g.damaged += 1
        self._append(g, pk)
        g.seen += 1
        if g.seen >= g.hdr.further:
            self._emit(pk.channel, self.open.pop(pk.channel), 'complete')

    def flush(self):
        for ch in list(self.open):
            self._emit(ch, self.open.pop(ch), 'unfinished')

# ---------------------------------------------------------------------------
# Записи (§5)
# ---------------------------------------------------------------------------
class _Cursor:
    def __init__(self, b):
        self.b = b; self.pos = 0; self.failed = False; self.clean = True

    def next(self):
        if self.failed or self.pos >= len(self.b):
            self.failed = True
            return -1
        v = HAM[self.b[self.pos]]
        if v < 0:
            self.failed = True
            return -1
        if not HAM_CLEAN[self.b[self.pos]]:
            self.clean = False
        self.pos += 1
        return v


class Rec:
    pass


FLAG_NAMES = ('caption', 'delay', 'index', 'more', 'cyclic', 'auto_acquire', 'support_needed',
              'priority', 'alarm', 'update', 'support_record')


def decode_record_header(b):
    if len(b) < 5:
        return None
    c = _Cursor(b)
    rt = c.next(); rd = c.next()
    if c.failed:
        return None
    h = Rec()
    h.type = rt
    ext, link, cls, hext = rd & 1, rd & 2, rd & 4, rd & 8
    n = 9 if ext else 3
    digits = [c.next() for _ in range(n)]
    if c.failed:
        return None
    if not ext:
        digits = [0, 0, 0, 0] + digits + [0, 0]
    a = 0
    for d in digits:
        a = (a << 4) | d
    h.address = a; h.long_form = bool(ext)
    h.linked = False; h.more_links = False; h.order = 0
    if link:
        l1 = c.next(); l2 = c.next()
        if c.failed:
            return None
        h.linked = True; h.more_links = bool(l1 & 8); h.order = ((l1 & 7) << 4) | l2
    h.flags = {k: False for k in FLAG_NAMES}
    h.version = 0
    if cls:
        group = 1
        while True:
            ptr = c.next()
            if c.failed:
                return None
            for pair in range(3):
                if not (ptr >> pair) & 1:
                    continue
                for half in range(2):
                    f = c.next()
                    if c.failed:
                        return None
                    idx = pair * 2 + half + 1
                    if group != 1:
                        continue
                    if idx == 3:
                        h.flags['caption'] = bool(f & 8); h.flags['delay'] = bool(f & 4); h.flags['index'] = bool(f & 2)
                    elif idx == 4:
                        h.flags['more'] = bool(f & 8); h.flags['cyclic'] = bool(f & 4)
                        h.flags['auto_acquire'] = bool(f & 2); h.flags['support_needed'] = bool(f & 1)
                    elif idx == 5:
                        h.flags['priority'] = bool(f & 8); h.flags['alarm'] = bool(f & 4)
                        h.flags['update'] = bool(f & 2); h.flags['support_record'] = bool(f & 1)
                    elif idx == 6:
                        h.version = f
            if not ptr & 8:
                break
            group += 1
    h.attested = c.clean
    h.extensions = []
    if hext:
        while True:
            intro = c.next(); size = c.next()
            if c.failed:
                return None
            data = [c.next() for _ in range(size)]
            if c.failed:
                return None
            h.extensions.append((intro & 7, size, data))
            if not intro & 8:
                break
    h.header_bytes = c.pos
    return h


def address_text(a):
    if (a >> 20) & 0xFFFF == 0 and a & 0xFF == 0:
        return '%03X' % ((a >> 8) & 0xFFF)
    return '%09X' % a


def reserved_purpose(channel, a):
    short = (a >> 8) & 0xFFF if ((a >> 20) & 0xFFFF == 0 and a & 0xFF == 0) else 0x1000
    if short == 0xFFF:
        return 'Support Record'
    if channel == 0 and short == 0:
        return 'Master Index / power-up'
    if channel == 0 and short == 0xFFE:
        return 'Service Application Record'
    if channel == 0xA00 and short == 0:
        return 'Start of captioning'
    if channel == 0xB00 and short == 0:
        return 'Start of Flash'
    return ''


class RecordAssembler:
    """Группы → сообщения (несвязанная запись или собранная серия §5.2.6)."""

    def __init__(self, callback):
        self.cb = callback
        self.open = {}
        self.seq = 0
        self.stats = dict(groups=0, foreign=0, header_bad=0, records=0)
        self.foreign = {}

    def add(self, g):
        self.stats['groups'] += 1
        if g.hdr.type != 0:
            self.stats['foreign'] += 1
            if g.channel_attested and g.hdr.attested:
                k = (g.channel, g.hdr.type)
                self.foreign[k] = self.foreign.get(k, 0) + 1
            return
        h = decode_record_header(g.data)
        if h is None:
            self.stats['header_bad'] += 1
            return
        self.stats['records'] += 1
        data = g.data[h.header_bytes:]
        present = g.present[h.header_bytes:]
        attested = h.attested and g.channel_attested
        if not h.linked:
            m = Rec()
            m.channel = g.channel; m.address = h.address; m.long_form = h.long_form
            m.type = h.type; m.flags = dict(h.flags); m.version = h.version
            m.extensions = h.extensions; m.data = data; m.present = present
            m.complete = True; m.intact = g.intact; m.attested = attested
            m.aligned = True; m.records = 1; m.clock = g.clock
            self.cb(m)
            return
        key = (g.channel, h.address, h.version)
        o = self.open.get(key)
        if o is None:
            if len(self.open) >= 64:
                old = min(self.open, key=lambda k: self.open[k].seq)
                self._emit(self.open.pop(old), False)
            o = Rec()
            o.channel = g.channel; o.address = h.address; o.long_form = h.long_form
            o.type = h.type; o.flags = dict(h.flags); o.version = h.version
            o.extensions = h.extensions; o.parts = {}; o.final = 999
            o.intact = True; o.attested = False; o.seq = self.seq; self.seq += 1
            self.open[key] = o
        if h.order == 0:
            o.flags = dict(h.flags); o.extensions = h.extensions; o.type = h.type
        if not g.intact:
            o.intact = False
        if attested:
            o.attested = True
        if not h.more_links:
            o.final = h.order
        o.parts[h.order] = (data, present)
        o.clock = g.clock
        if o.final <= 127 and len(o.parts) == o.final + 1:
            self._emit(self.open.pop(key), True)

    def _emit(self, o, complete):
        o.complete = complete
        o.aligned = complete
        o.records = len(o.parts)
        o.data = b''.join(o.parts[k][0] for k in sorted(o.parts))
        o.present = b''.join(o.parts[k][1] for k in sorted(o.parts))
        self.cb(o)

    def flush(self):
        for k in list(self.open):
            self._emit(self.open.pop(k), False)

# ---------------------------------------------------------------------------
# Каталог записей и голосование копий (nabts_record_catalogue.cpp)
# ---------------------------------------------------------------------------
MAX_COPIES = 16


def _voted_length(copies, inc):
    best, support_best = 0, 0
    for i, c in enumerate(copies):
        if inc and not inc[i]:
            continue
        s = sum(1 for j, o in enumerate(copies) if (not inc or inc[j]) and len(o[0]) == len(c[0]))
        if s > support_best or (s == support_best and len(c[0]) > best):
            best, support_best = len(c[0]), s
    return best


def _vote_over(copies, inc=None):
    n = _voted_length(copies, inc)
    data = bytearray(n); present = bytearray(n)
    for pos in range(n):
        w = {}; newest = {}
        for ci, (d, pr) in enumerate(copies):
            if inc and not inc[ci]:
                continue
            if pos >= len(d) or (pos < len(pr) and not pr[pos]):
                continue
            v = d[pos]
            w[v] = w.get(v, 0) + 255
            newest[v] = ci
        if not w:
            continue
        best = None
        for v in w:
            if best is None:
                best = v; continue
            bc, vc = ODD[best], ODD[v]
            if bc and not vc:
                continue
            if bc == vc and not (w[v] > w[best] or (w[v] == w[best] and newest[v] > newest[best])):
                continue
            best = v
        data[pos] = best; present[pos] = 1
    return bytes(data), bytes(present)


def _agreement(copy, vote, slip):
    d, pr = copy
    vd, vp = vote
    judged = agreed = 0
    for pos in range(len(vd)):
        src = pos + slip
        if not vp[pos] or src < 0 or src >= len(d) or (src < len(pr) and not pr[src]):
            continue
        judged += 1
        agreed += d[src] == vd[pos]
    return judged, agreed


def _outlier(a):
    return a[0] >= 16 and a[1] * 100 < a[0] * 50


def _better(a, b):
    return a[1] * max(1, b[0]) > b[1] * max(1, a[0])


def _slide(copy, slip):
    d, pr = copy
    n = max(0, len(d) - slip)
    od = bytearray(n); op = bytearray(n)
    for pos in range(n):
        src = pos + slip
        if src < 0 or src >= len(d) or (src < len(pr) and not pr[src]):
            continue
        od[pos] = d[src]; op[pos] = 1
    return bytes(od), bytes(op)


def vote_record(copies):
    if not copies:
        return b'', b''
    if len(copies) == 1:
        return copies[0]
    prov = _vote_over(copies)
    slid = list(copies)
    inc = [1] * len(copies)
    dropped = 0
    for i, c in enumerate(copies):
        un = _agreement(c, prov, 0)
        if not _outlier(un):
            continue
        best, best_slip = un, 0
        for s in range(-8, 9):
            if s == 0:
                continue
            a = _agreement(c, prov, s)
            if not _outlier(a) and _better(a, best):
                best, best_slip = a, s
        if best_slip:
            slid[i] = _slide(c, best_slip)
            continue
        inc[i] = 0; dropped += 1
    if dropped == 0 or dropped * 2 >= len(copies):
        return _vote_over(slid)
    return _vote_over(slid, inc)


def _algorithmic_more(a):
    tens, units = (a >> 4) & 0xF, a & 0xF
    if tens > 9 or units > 9:
        return None
    v = tens * 10 + units + 1
    if v > 99:
        return None
    return (a & ~0xFF) | ((v // 10) << 4) | (v % 10)


def _more_extension(exts):
    for meaning, size, data in exts:
        if meaning != 1:
            continue
        if size == 0:
            return 0
        if size in (3, 9):
            v = 0
            for nb in data:
                v = (v << 4) | (nb & 0xF)
            return v << 8 if size == 3 else v
    return None


FLAGS_VOTED = ('caption', 'cyclic', 'priority', 'alarm', 'update', 'support_record',
               'support_needed', 'index', 'more')


class Catalogue:
    def __init__(self):
        self.entries = {}

    def merge(self, m):
        key = (m.channel, m.address, m.version)
        e = self.entries.get(key)
        good = m.complete and m.intact
        if e is None:
            e = Rec()
            e.channel = m.channel; e.address = m.address; e.version = m.version
            e.long_form = m.long_form; e.first = m.clock
            e.seen = e.intact_n = e.attested_n = 0
            e.flags_set = {k: 0 for k in FLAGS_VOTED}; e.flags_att = {k: 0 for k in FLAGS_VOTED}
            e.copies = []; e.kept_intact = False
            self._take(e, m)
            self.entries[key] = e
        elif good != e.kept_intact and good or (good == e.kept_intact and len(m.data) > len(e.data)):
            self._take(e, m)
        if good:
            e.copies = []
        elif not e.kept_intact and m.aligned:
            if len(e.copies) >= MAX_COPIES:
                e.copies.pop(0)
            e.copies.append((m.data, m.present))
        for k in FLAGS_VOTED:
            if m.flags.get(k):
                e.flags_set[k] += 1
                if m.attested:
                    e.flags_att[k] += 1
        e.last = m.clock
        e.seen += 1
        e.intact_n += good
        e.attested_n += bool(m.attested)

    @staticmethod
    def _take(e, m):
        e.type = m.type; e.records = m.records; e.complete = m.complete
        e.data = m.data; e.present = m.present
        e.more_address = _more_extension(m.extensions)
        e.kept_intact = m.complete and m.intact

    def reconcile(self):
        """Идентичности, ни разу не принятые чисто, — к соседу по одной цифре или вон."""
        def digits(e):
            return [(e.channel >> s) & 0xF for s in (8, 4, 0)] + \
                   [(e.address >> s) & 0xF for s in range(32, -1, -4)] + [e.version & 0xF]
        att = [(k, digits(e)) for k, e in self.entries.items() if e.attested_n > 0]
        if not att:
            return 0, 0
        folded = dropped = 0
        for k in [k for k, e in self.entries.items() if e.attested_n == 0]:
            e = self.entries.pop(k)
            d = digits(e)
            found = None
            for tk, td in att:
                if sum(1 for a, b in zip(d, td) if a != b) == 1:
                    if found is not None:
                        found = None; break
                    found = tk
            if found is None:
                dropped += 1
                continue
            t = self.entries[found]
            t.seen += e.seen; t.intact_n += e.intact_n
            for f in FLAGS_VOTED:
                t.flags_set[f] += e.flags_set[f]
            t.first = min(t.first, e.first); t.last = max(t.last, e.last)
            if not t.kept_intact:
                for c in e.copies:
                    if len(t.copies) >= MAX_COPIES:
                        break
                    t.copies.append(c)
            folded += 1
        return folded, dropped

    def records(self):
        out = []
        for key in sorted(self.entries):
            e = self.entries[key]
            r = Rec()
            r.__dict__.update({k: v for k, v in e.__dict__.items() if k not in ('copies',)})
            att = e.attested_n > 0
            voters = e.attested_n if att else e.seen
            src = e.flags_att if att else e.flags_set
            r.flags = {k: src[k] * 2 > voters for k in FLAGS_VOTED}
            r.copies_voted = len(e.copies)
            if e.copies:
                r.data, r.present = vote_record(e.copies)
            r.addr_text = address_text(e.address) if not e.long_form else '%09X' % e.address
            r.purpose = reserved_purpose(e.channel, e.address)
            out.append(r)
        return out

# ---------------------------------------------------------------------------
# NAPLPS: цвет и карта цветов
# ---------------------------------------------------------------------------
BLACK = (0, 0, 0, False)          # (green, red, blue, transparent), 3 бита на пушку
WHITE = (7, 7, 7, False)
TRANSPARENT = (0, 0, 0, True)


def _hue(angle):
    prim = [(240.0, 0), (120.0, 1), (0.0, 2)]          # green, red, blue
    def dist(a, b):
        d = abs(a - b)
        return 360 - d if d > 180 else d
    ds = sorted(((dist(angle, a), i) for a, i in prim))
    c = [0, 0, 0]
    c[ds[0][1]] = 7
    c[ds[2][1]] = 0
    c[ds[1][1]] = int(ds[0][0] / 60.0 * 7 + 0.5)
    return (c[0], c[1], c[2], False)


def default_colour_map():
    return [(i, i, i, False) for i in range(8)] + [_hue(360.0 * i / 8) for i in range(8)]


DEFAULT_MAP = default_colour_map()


class ColourState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.mode = 0
        self.direct = WHITE
        self.draw_addr = 0
        self.bg_addr = 0
        self.reset_map()

    def reset_map(self):
        self.map = list(DEFAULT_MAP)
        self.used = [False] * 16

    def reset_drawing_to_white(self):
        self.mode = 0; self.direct = WHITE

    def select_mapped(self, a):
        self.mode = 1; self.draw_addr = a % 16; self.used[self.draw_addr] = True

    def select_mapped_bg(self, a, b):
        self.mode = 2
        a %= 16; b %= 16
        if a != b:
            self.draw_addr = a; self.used[a] = True
        self.bg_addr = b; self.used[b] = True

    def reset_to_mapped(self, white):
        self.reset_map(); self.mode = 1
        if white and WHITE in self.map:
            self.draw_addr = self.map.index(WHITE)

    def set_colour(self, c):
        if self.mode != 0:
            self.map[self.draw_addr] = c; self.used[self.draw_addr] = True
            return
        self.direct = c
        if c in self.map:
            self.draw_addr = self.map.index(c)
            return
        for i in range(16):
            if self.used[i] or self.map[i] == BLACK or self.map[i] == WHITE:
                continue
            self.map[i] = c; self.used[i] = True; self.draw_addr = i
            return

    def write(self, a, c):
        self.map[a % 16] = c; self.used[a % 16] = True

    def set_transparent(self):
        if self.mode == 0:
            self.direct = TRANSPARENT
        else:
            self.map[self.draw_addr] = TRANSPARENT

    def drawing(self):
        return self.direct if self.mode == 0 else self.map[self.draw_addr]

    def background(self):
        return self.map[self.bg_addr] if self.mode == 2 else BLACK


def _increment_addr(a):
    for bit in range(3, -1, -1):
        m = 1 << bit
        if not a & m:
            a |= m
            for hi in range(3, bit, -1):
                a &= ~(1 << hi)
            return a
    return None


def _addr_from_operand(v, nbytes):
    bits = max(1, nbytes) * 6
    return v << (4 - bits) if bits <= 4 else v >> (bits - 4)

# ---------------------------------------------------------------------------
# NAPLPS: чтение операндов PDI (§5.3.1)
# ---------------------------------------------------------------------------
class Operands:
    def __init__(self, b, fmt):
        self.b = b; self.pos = 0; self.fmt = fmt; self.truncated = False

    def empty(self):
        return self.pos >= len(self.b)

    def remaining(self):
        return len(self.b) - self.pos

    def _next(self):
        if self.pos >= len(self.b):
            self.truncated = True
            return 0
        v = self.b[self.pos] & 0x3F
        self.pos += 1
        return v

    def fixed(self):
        self.truncated = False
        return self._next()

    def single(self):
        self.truncated = False
        v = 0
        for _ in range(self.fmt[0]):
            v = (v << 6) | self._next()
        return v

    def coord(self):
        self.truncated = False
        comps = 3 if self.fmt[2] else 2
        bpb = 6 // comps
        mask = (1 << bpb) - 1
        xs = 6 - bpb; ys = 6 - 2 * bpb
        x = y = 0; nb = 0
        for _ in range(self.fmt[1]):
            p = self._next()
            x = (x << bpb) | ((p >> xs) & mask)
            y = (y << bpb) | ((p >> ys) & mask)
            nb += bpb
        return (_signed_frac(x, nb), _signed_frac(y, nb))

    def colour(self):
        self.truncated = False
        words = max(1, min(self.fmt[1], self.remaining()))
        g = r = b = 0; bits = 0
        for _ in range(words):
            p = self._next()
            for t in (1, 0):
                base = t * 3
                g = (g << 1) | ((p >> (base + 2)) & 1)
                r = (r << 1) | ((p >> (base + 1)) & 1)
                b = (b << 1) | ((p >> base) & 1)
                bits += 1

        def gun(v):
            if bits >= 3:
                return v >> (bits - 3)
            mx = (1 << bits) - 1
            return (v * 7 + mx // 2) // mx if mx else 0
        return (gun(g), gun(r), gun(b), False)


def _signed_frac(bits, n):
    if n <= 0:
        return 0.0
    neg = (bits >> (n - 1)) & 1
    mag = bits & ((1 << (n - 1)) - 1)
    # mag — (n-1) бит дроби, старший бит = 1/2
    v = mag / float(1 << (n - 1)) if n > 1 else 0.0
    return v - 1.0 if neg else v


def _clamp_unit(p):
    x, y = p
    c = False
    if not 0.0 <= x < 1.0:
        x = min(max(x, 0.0), 0.9999999); c = True
    if not 0.0 <= y < 1.0:
        y = min(max(y, 0.0), 0.9999999); c = True
    return (x, y), c

# ---------------------------------------------------------------------------
# NAPLPS: примитивы и интерпретатор (naplps_interpreter.cpp)
# ---------------------------------------------------------------------------
class Prim:
    __slots__ = ('kind', 'points', 'origin', 'size', 'filled', 'highlighted', 'pel', 'line_tex',
                 'pattern', 'mask_size', 'mode', 'colour', 'background', 'caddr', 'baddr',
                 'blinking', 'blink_to', 'blink_addr', 'incr', 'char', 'rep', 'rotation', 'path',
                 'reverse', 'underlined', 't', 'daddr')

    def __init__(self, kind):
        self.kind = kind; self.points = []; self.origin = (0.0, 0.0); self.size = (0.0, 0.0)
        self.filled = False; self.highlighted = False; self.pel = (0.0, 0.0); self.line_tex = 0
        self.pattern = 0; self.mask_size = (0.0, 0.0); self.mode = 0; self.colour = WHITE
        self.background = BLACK; self.caddr = -1; self.baddr = -1; self.blinking = False
        self.blink_to = BLACK; self.blink_addr = -1; self.incr = None; self.char = 0
        self.rep = 'P'; self.rotation = 0; self.path = 0; self.reverse = False; self.underlined = False
        self.t = 0.0; self.daddr = 0          # время на экране (с) и адрес цвета рисования (мигание)


FIELD_NORMAL = (1.0 / 40.0, 5.0 / 128.0)


class TextState:
    def __init__(self):
        self.reset()

    def reset(self):
        self.rotation = 0; self.path = 0; self.ics = 0; self.irs = 0; self.move = 0
        self.field = FIELD_NORMAL; self.reverse = False; self.underlined = False


class DrcsChar:
    def __init__(self, code, w, h):
        self.code = code; self.w = w; self.h = h; self.el = [False] * (w * h)


class Mask:
    def __init__(self, w=0, h=0):
        self.w = w; self.h = h; self.el = [False] * (w * h)

    def defined(self):
        return self.w > 0 and self.h > 0


ESC, NSR, CAN, APS = 0x1B, 0x1F, 0x18, 0x1C
TRANSPARENT_C0 = set([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x11, 0x12, 0x13, 0x14,
                      0x15, 0x16, 0x17])
# Условная скорость рисования приёмника, с на примитив (декодеры 1980-х рисовали
# страницу секунды; стандарт её не задаёт) и интервалы мигания C1 BLINK START
# (§6.2.8.1: «implementation-dependent»), в десятых секунды.
DRAW_COST = {'char': 0.004, 'point': 0.004, 'line': 0.012, 'arc': 0.02, 'rect': 0.02, 'poly': 0.02, 'incr': 0.05}
BLINK_C1 = (5, 5, 0)
SET_PRIMARY, SET_SUPP, SET_PDI, SET_MOSAIC, SET_MACRO, SET_DRCS, SET_NULL = range(7)
STORAGE = 3072
MAX_MACRO_DEPTH = 8


def parse_escape(b, pos):
    """b[pos] — байт после ESC. Возвращает (вид, длина вместе с ESC, слот, набор/код)."""
    i = pos; inter = []
    while i < len(b) and 0x20 <= b[i] <= 0x2F:
        inter.append(b[i]); i += 1
    if i >= len(b):
        return ('trunc', 1 + i - pos, 0, 0)
    f = b[i]
    if not 0x30 <= f <= 0x7E:
        return ('bad', 1 + i - pos, 0, 0)
    ln = 1 + i - pos + 1
    if not inter:
        if f == 0x6E:
            return ('shift', ln, 2, 0)
        if f == 0x6F:
            return ('shift', ln, 3, 0)
        if 0x40 <= f <= 0x5F:
            return ('c1', ln, 0, f)
        return ('unsup', ln, 0, 0)
    if len(inter) > 1 or inter[0] in (0x21, 0x22):
        return ('unsup', ln, 0, 0)
    slots = {0x28: (0, False), 0x29: (1, True), 0x2A: (2, True), 0x2B: (3, True),
             0x2D: (1, True), 0x2E: (2, True), 0x2F: (3, True)}
    if inter[0] not in slots:
        return ('unsup', ln, 0, 0)
    slot, allow96 = slots[inter[0]]
    sets = {0x42: (SET_PRIMARY, False), 0x7C: (SET_SUPP, False), 0x57: (SET_PDI, True),
            0x7D: (SET_MOSAIC, True), 0x7A: (SET_MACRO, True), 0x7B: (SET_DRCS, True)}
    s, need96 = sets.get(f, (SET_NULL, False))
    if s == SET_NULL or (need96 and not allow96):
        return ('desig', ln, slot, SET_NULL)
    return ('desig', ln, slot, s)


def supplementary_nonspacing(code):
    return 0x40 <= code <= 0x4F


class Interpreter:
    def __init__(self, grid=(256, 200)):
        self.grid = grid
        self.prims = []
        self.reset_decoder()

    # --- состояние ---------------------------------------------------------
    def reset_env(self):
        self.g = [SET_PRIMARY, SET_PDI, SET_SUPP, SET_MOSAIC]
        self.locked = 0; self.invoked = 0; self.single = False

    def reset_decoder(self):
        self.reset_env()
        self.fmt = [1, 3, False]; self.pel = (0.0, 0.0)
        self.text = TextState()
        self.line_tex = 0; self.pattern = 0; self.highlight = False; self.mask_size = FIELD_NORMAL
        self.colour = ColourState()
        self.field_origin = (0.0, 0.0); self.field_size = (1.0, 1.0)
        self.cursor = self.home(); self.dp = (0.0, 0.0)
        self.blink = [None] * 16
        self.macros = {}; self.drcs = {}; self.masks = [Mask() for _ in range(4)]
        self.storage = 0

    def home(self):
        return (0.0, DISPLAY_H - abs(self.text.field[1]))

    def nsr_reset(self):
        self.reset_env()
        self.fmt = [1, 3, False]; self.pel = (0.0, 0.0)
        self.text.reset()
        self.field_origin = (0.0, 0.0); self.field_size = (1.0, 1.0)
        self.line_tex = 0; self.pattern = 0; self.highlight = False; self.mask_size = FIELD_NORMAL
        self.colour.reset_drawing_to_white()

    def apply_caption_state(self):
        self.nsr_reset()
        self.cursor = (0.0, 0.0); self.dp = (0.0, 0.0)
        self.colour.write(0, TRANSPARENT); self.colour.write(1, BLACK); self.colour.write(7, WHITE)
        self.colour.select_mapped_bg(7, 1)

    def drcs_size(self):
        gw, gh = self.grid
        cols = abs(self.text.field[0]) / (1.0 / gw)
        rows = abs(self.text.field[1]) / (DISPLAY_H / gh)
        return (min(max(int(round(cols)), 1), 256), min(max(int(round(rows)), 1), 256))

    # --- запуск записи -----------------------------------------------------
    def run(self, record, keep_display=False):
        carried = self.prims if keep_display else []
        self.prims = carried
        # Шкала событий для показа «как на экране»: примитивы с временем, очистки экрана,
        # смены карты цветов и процессов мигания. Время — паузы WAIT (§5.3.2.8) плюс
        # условная скорость рисования приёмника (DRAW_COST).
        self.clock = 0.0
        self.events = []
        for p in carried:
            p.t = 0.0; self.events.append(('p', 0.0, p))
        self._snap_m = self._snap_b = None
        self.snap_state()
        self.frames = []
        self.collecting = None
        self.body = bytearray(); self.def_code = 0; self.def_transmit = False
        self.drcs_target = None; self.mask_target = None
        self.have_last_drcs = False; self.last_drcs = 0x7F
        self.last_graphic = None
        self.def_frame = 0; self.def_had_code = False
        self.wrap = 0          # 0 нет, 1 взведено, 2 было APR, 3 было APD
        rec = bytes(b & 0x7F for b in record)
        self.frames.append([rec, 0])
        steps = 0
        while self.frames:
            steps += 1
            if steps > 400000:      # защита от зацикливания на повреждённых данных
                break
            if not self.step():
                self.frames.pop()
                if self.frames and self.def_frame >= len(self.frames):
                    self.def_frame = len(self.frames) - 1
        self.end_definition()
        self.snap_state()
        fm = self.colour.map
        for p in self.prims:
            if p.caddr >= 0:
                p.colour = fm[p.caddr % 16]
            if p.baddr >= 0:
                p.background = fm[p.baddr % 16]
            if p.blink_addr >= 0:
                p.blink_to = fm[p.blink_addr % 16]
        page = Rec()
        page.prims = list(self.prims)
        page.colour_map = list(fm)
        page.drcs = dict((k, v) for k, v in self.drcs.items())
        page.masks = list(self.masks)
        page.events = self.events
        page.end = self.clock
        return page

    def snap_state(self):
        m = tuple(self.colour.map); b = tuple(self.blink)
        if m != self._snap_m:
            self._snap_m = m; self.events.append(('map', self.clock, list(m)))
        if b != self._snap_b:
            self._snap_b = b; self.events.append(('blink', self.clock, list(b)))

    def step(self):
        fr = self.frames[-1]
        b, pos = fr
        if pos >= len(b):
            return False
        byte = b[pos]; fr[1] = pos + 1
        from_def = len(self.frames) - 1 == self.def_frame
        if self.collecting in ('macro', 'macrox'):
            if from_def:
                if byte == ESC:
                    k, ln, _, c1 = parse_escape(b, fr[1])
                    if k == 'c1' and c1 in (0x40, 0x41, 0x42, 0x43, 0x44, 0x45):
                        fr[1] += ln - 1
                        self.end_definition()
                        self.exec_c1(c1)
                        return True
                if self.collecting == 'macro':
                    self.body.append(byte)
                    return True
                start = fr[1] - 1
                self.exec_byte(byte)
                d = self.frames[self.def_frame]
                self.body += d[0][start:d[1]]
                return True
            self.exec_byte(byte)
            return True
        if self.collecting in ('drcs', 'mask') and from_def and byte not in TRANSPARENT_C0:
            term = False
            if byte == ESC:
                k, ln, _, c1 = parse_escape(b, fr[1])
                term = k == 'c1' and c1 in (0x40, 0x41, 0x42, 0x43, 0x44, 0x45)
            if not term:
                self.def_had_code = True
        self.exec_byte(byte)
        return True

    def exec_byte(self, byte):
        if byte < 0x20:
            self.exec_c0(byte)
        else:
            self.exec_graphic(byte)

    # --- C0 -----------------------------------------------------------------
    def exec_c0(self, byte):
        if byte in TRANSPARENT_C0:
            return
        fr = self.frames[-1]
        if byte == ESC:
            k, ln, slot, val = parse_escape(fr[0], fr[1])
            fr[1] += ln - 1
            if k == 'desig':
                self.g[slot] = val
            elif k == 'shift':
                self.locked = self.invoked = slot; self.single = False
            elif k == 'c1':
                self.exec_c1(val)
            return
        if byte == 0x0F:
            self.locked = self.invoked = 0; self.single = False
        elif byte == 0x0E:
            self.locked = self.invoked = 1; self.single = False
        elif byte == 0x19:
            self.invoked = 2; self.single = True
        elif byte == 0x1D:
            self.invoked = 3; self.single = True
        elif byte == 0x08:
            self.move_by('back')
        elif byte == 0x09:
            self.move_by('fwd')
        elif byte == 0x0A:
            if self.wrap == 1:
                self.wrap = 3; return
            if self.wrap == 2:
                self.wrap = 0; return
            self.move_by('down')
        elif byte == 0x0B:
            self.move_by('up')
        elif byte == 0x0D:
            if self.wrap == 1:
                self.wrap = 2; return
            if self.wrap == 3:
                self.wrap = 0; return
            self.move_cursor((self.field_origin[0], self.cursor[1]))
        elif byte == 0x0C:
            if self.colour.mode == 2:
                self.clear_display(self.colour.background(), self.colour.bg_addr)
            else:
                self.clear_display(BLACK, -1)
            self.move_cursor(self.home())
        elif byte == 0x1E:
            self.move_cursor(self.home())
        elif byte == NSR:
            self.nsr_reset()
            b, pos = fr
            if pos + 1 < len(b):
                rb, cb = b[pos] & 0x7F, b[pos + 1] & 0x7F
                if 0x40 <= rb <= 0x7F and 0x40 <= cb <= 0x7F:
                    fr[1] += 2
                    dx, dy = abs(self.text.field[0]), abs(self.text.field[1])
                    self.move_cursor(((cb & 0x3F) * dx, DISPLAY_H - ((rb & 0x3F) + 1) * dy))
                    return
                if 0x20 <= rb < 0x40 and 0x20 <= cb < 0x40:
                    fr[1] += 2
            self.move_cursor(self.home())
        elif byte == CAN:
            del self.frames[1:]
        elif byte == APS:
            b, pos = fr
            if pos + 1 >= len(b):
                return
            rb, cb = b[pos], b[pos + 1]
            if rb < 0x20 or cb < 0x20:
                return
            fr[1] += 2
            dx, dy = abs(self.text.field[0]), abs(self.text.field[1])
            self.move_cursor((((cb & 0x7F) - 32) * dx, ((rb & 0x7F) - 32) * dy))

    def clear_display(self, colour, addr):
        self.prims = []
        self.snap_state()
        self.events.append(('clear', self.clock))
        if colour == BLACK and addr < 0:
            return
        p = Prim('rect'); p.filled = True
        p.origin = (0.0, 0.0); p.size = (1.0, DISPLAY_H)
        p.points = [(0.0, 0.0), (1.0, DISPLAY_H)]
        p.mode = self.colour.mode; p.colour = colour; p.caddr = addr
        p.daddr = addr if addr >= 0 else self.colour.draw_addr
        p.t = self.clock
        self.prims.append(p); self.events.append(('p', self.clock, p))

    # --- C1 -----------------------------------------------------------------
    def exec_c1(self, c):
        if c in (0x40, 0x41, 0x42, 0x43, 0x44):
            terminated = self.collecting
            self.end_definition()
            if c == 0x43 and terminated == 'drcs' and self.have_last_drcs:
                nxt = 0x20 if self.last_drcs >= 0x7F else self.last_drcs + 1
                self.begin_definition('drcs', nxt)
                return
            fr = self.frames[-1]
            code = 0
            if fr[1] < len(fr[0]):
                code = fr[0][fr[1]]
                if code >= 0x20:
                    fr[1] += 1
                else:
                    return
            if c == 0x40:
                self.def_transmit = False; self.begin_definition('macro', code)
            elif c == 0x41:
                self.def_transmit = False; self.begin_definition('macrox', code)
            elif c == 0x42:
                self.def_transmit = True; self.begin_definition('macro', code)
            elif c == 0x43:
                self.begin_definition('drcs', code)
            else:
                self.begin_definition('mask', code)
            return
        if c == 0x45:
            self.end_definition()
        elif c == 0x46:
            fr = self.frames[-1]
            if fr[1] >= len(fr[0]) or self.last_graphic is None:
                return
            cnt = fr[0][fr[1]]
            if cnt < 0x40:
                return
            fr[1] += 1
            g = self.last_graphic
            for _ in range(cnt & 0x3F):
                self.exec_graphic(g)
        elif c == 0x47:
            if self.last_graphic is None:
                return
            dx = abs(self.text.field[0])
            if dx <= 0:
                return
            limit = self.field_origin[0] + abs(self.field_size[0])
            g = self.last_graphic
            for _ in range(int(1.0 / dx) + 1):
                if self.cursor[0] + dx > limit:
                    break
                self.exec_graphic(g)
        elif c == 0x48:
            self.text.reverse = True
        elif c == 0x49:
            self.text.reverse = False
        elif c == 0x4A:
            self.text.field = (1.0 / 80.0, 5.0 / 128.0)
        elif c == 0x4B:
            self.text.field = (1.0 / 32.0, 3.0 / 64.0)
        elif c == 0x4C:
            self.text.field = FIELD_NORMAL
        elif c == 0x4D:
            self.text.field = (1.0 / 40.0, 5.0 / 64.0)
        elif c == 0x4F:
            self.text.field = (1.0 / 20.0, 5.0 / 64.0)
        elif c == 0x4E:
            if self.colour.mode == 2:
                self.blink[self.colour.draw_addr] = (self.colour.bg_addr, None) + BLINK_C1 + (self.clock,)
            else:
                self.blink[self.colour.draw_addr] = (-1, BLACK) + BLINK_C1 + (self.clock,)
        elif c == 0x5E:
            self.blink[self.colour.draw_addr] = None
        elif c == 0x59:
            self.text.underlined = True
        elif c == 0x5A:
            self.text.underlined = False

    # --- G-наборы -------------------------------------------------------------
    def exec_graphic(self, byte):
        s = self.g[self.invoked]
        if self.single:
            self.invoked = self.locked; self.single = False
        if s == SET_PDI:
            if 0x20 <= byte <= 0x3F:
                self.exec_pdi(byte)
            return
        if s == SET_MACRO:
            self.invoke_macro(byte)
            return
        if s == SET_NULL:
            return
        self.last_graphic = byte
        p = self.make('char')
        p.char = byte
        p.rep = {SET_PRIMARY: 'P', SET_SUPP: 'S', SET_MOSAIC: 'M', SET_DRCS: 'D'}[s]
        p.origin = self.cursor; p.points = [self.cursor]
        p.size = self.text.field; p.rotation = self.text.rotation; p.path = self.text.path
        p.reverse = self.text.reverse; p.underlined = self.text.underlined
        self.emit(p)
        if s != SET_SUPP or not supplementary_nonspacing(byte):
            self.move_by('fwd')

    # --- PDI ------------------------------------------------------------------
    def gather(self):
        fr = self.frames[-1]
        b = fr[0]; ops = bytearray()
        while fr[1] < len(b):
            x = b[fr[1]]
            if 0x40 <= x <= 0x7F:
                ops.append(x); fr[1] += 1; continue
            if x < 0x20 and x in TRANSPARENT_C0:
                fr[1] += 1; continue
            break
        return bytes(ops)

    def exec_pdi(self, op):
        ops = self.gather()
        r = Operands(ops, list(self.fmt))
        if op == 0x20:
            self.pdi_reset(r)
        elif op == 0x21:
            self.pdi_domain(r)
        elif op == 0x22:
            self.pdi_text(r)
        elif op == 0x23:
            self.pdi_texture(r)
        elif op == 0x3C:
            self.pdi_set_colour(r)
        elif op == 0x3E:
            self.pdi_select_colour(r)
        elif op == 0x3F:
            self.pdi_blink(r)
        elif op == 0x3D:
            # WAIT (§5.3.2.8): пауза в десятых долях секунды
            self.snap_state()
            self.clock += 0.1 * sum(b & 0x3F for b in ops)
        elif op <= 0x27:
            self.pdi_point(op, r)
        elif op <= 0x2B:
            self.pdi_line(op, r)
        elif op <= 0x2F:
            self.pdi_arc(op, r)
        elif op <= 0x33:
            self.pdi_rect(op, r)
        elif op <= 0x37:
            self.pdi_poly(op, r)
        elif op == 0x38:
            self.pdi_field(r)
        else:
            self.pdi_incremental(op, r, ops)

    def pdi_reset(self, r):
        b1 = 0 if r.empty() else r.fixed()
        b2 = 0 if r.empty() else r.fixed()
        bit = lambda v, i: (v >> (i - 1)) & 1
        if bit(b1, 1):
            self.fmt = [1, 3, False]; self.pel = (0.0, 0.0)
        ca = bit(b1, 3) * 2 + bit(b1, 2)
        if ca == 1:
            self.colour.mode = 0; self.colour.reset_map(); self.colour.set_colour(WHITE)
        elif ca == 2:
            self.colour.reset_to_mapped(self.colour.mode == 0)
        elif ca == 3:
            self.colour.reset_to_mapped(True)
        sa = bit(b1, 6) * 4 + bit(b1, 5) * 2 + bit(b1, 4)
        if sa in (1, 7):
            self.clear_display(BLACK, -1)
        elif sa in (2, 5, 6):
            self.clear_display(self.colour.drawing(), -1 if self.colour.mode == 0 else self.colour.draw_addr)
        if bit(b2, 1):
            self.text.reset()
            self.field_origin = (0.0, 0.0); self.field_size = (1.0, 1.0)
            self.move_cursor(self.home())
        if bit(b2, 2):
            self.blink = [None] * 16
        if bit(b2, 4):
            self.line_tex = 0; self.pattern = 0; self.highlight = False; self.mask_size = FIELD_NORMAL
        if bit(b2, 5):
            for k, v in list(self.macros.items()):
                self.storage -= min(self.storage, len(v[0]))
            self.macros = {}
        if bit(b2, 6):
            self.storage -= min(self.storage, 11 * len(self.drcs))
            self.drcs = {}

    def pdi_domain(self, r):
        if r.empty():
            return
        b1 = r.fixed()
        self.fmt = [(b1 & 3) + 1, ((b1 >> 2) & 7) + 1, bool((b1 >> 5) & 1)]
        if r.empty():
            return
        r.fmt = list(self.fmt)
        self.pel = r.coord()

    def pdi_text(self, r):
        if r.empty():
            return
        b1 = r.fixed()
        t = self.text
        t.rotation = b1 & 3; t.path = (b1 >> 2) & 3; t.ics = (b1 >> 4) & 3
        if not r.empty():
            b2 = r.fixed()
            t.irs = b2 & 3; t.move = (b2 >> 2) & 3
        if not r.empty():
            t.field = r.coord()

    def pdi_texture(self, r):
        if r.empty():
            return
        b1 = r.fixed()
        self.line_tex = b1 & 3; self.highlight = bool((b1 >> 2) & 1); self.pattern = (b1 >> 3) & 7
        if not r.empty():
            self.mask_size = r.coord()

    def pdi_set_colour(self, r):
        if r.empty():
            self.colour.set_transparent()
            return
        first = True; addr = 0
        while not r.empty():
            c = r.colour()
            if first:
                self.colour.set_colour(c); addr = self.colour.draw_addr; first = False
                continue
            if self.colour.mode == 0:
                self.colour.set_colour(c)
                continue
            addr = _increment_addr(addr)
            if addr is None:
                break
            self.colour.write(addr, c)

    def pdi_select_colour(self, r):
        nb = self.fmt[0]
        words = r.remaining() // nb if nb else 0
        if words == 0:
            self.colour.mode = 0
            return
        a = _addr_from_operand(r.single(), nb)
        if words == 1:
            self.colour.select_mapped(a)
            return
        b = _addr_from_operand(r.single(), nb)
        self.colour.select_mapped_bg(a, b)

    def pdi_blink(self, r):
        if r.empty():
            self.blink[self.colour.draw_addr] = None
            return
        frm = self.colour.draw_addr
        while True:
            to = _addr_from_operand(r.single(), self.fmt[0])
            on = 0 if r.empty() else r.fixed()
            off = 0 if r.empty() else r.fixed()
            delay = r.fixed() if not r.empty() else 0
            self.blink[frm % 16] = (to % 16, None, on, off, delay, self.clock) if on and off else None
            if r.empty():
                return
            frm = _increment_addr(frm)
            if frm is None:
                return

    def resolve(self, p):
        return _clamp_unit(p)[0]

    def pdi_point(self, op, r):
        rel = op in (0x25, 0x27); vis = op in (0x26, 0x27)
        while not r.empty():
            w = r.coord()
            t = self.resolve((self.dp[0] + w[0], self.dp[1] + w[1]) if rel else w)
            self.move_dp(t)
            if vis:
                p = self.make('point'); p.origin = t; p.points = [t]
                self.emit(p)
            if r.truncated:
                return

    def pdi_line(self, op, r):
        rel = op in (0x29, 0x2B); has_start = op in (0x2A, 0x2B)
        while not r.empty():
            s = self.dp
            if has_start:
                s = self.resolve(r.coord())
                if r.empty():
                    return
            w = r.coord()
            e = self.resolve((s[0] + w[0], s[1] + w[1])) if rel else self.resolve(w)
            p = self.make('line'); p.origin = s; p.points = [s, e]
            self.emit(p)
            self.move_dp(e)
            if r.truncated:
                return

    def pdi_arc(self, op, r):
        filled = op in (0x2D, 0x2F); has_start = op in (0x2E, 0x2F)
        s = self.dp
        if has_start:
            if r.empty():
                return
            s = self.resolve(r.coord())
        if r.empty():
            return
        p = self.make('arc'); p.filled = filled; p.origin = s; p.points = [s]
        prev = s
        while not r.empty() and len(p.points) < 256:
            w = r.coord()
            prev = self.resolve((prev[0] + w[0], prev[1] + w[1]))
            p.points.append(prev)
            if r.truncated:
                break
        if len(p.points) < 2:
            return
        if len(p.points) == 2:
            p.points.append(s)
        e = p.points[-1]
        self.emit(p)
        self.move_dp(e)

    def pdi_rect(self, op, r):
        filled = op in (0x31, 0x33); has_start = op in (0x32, 0x33)
        while not r.empty():
            s = self.dp
            if has_start:
                s = self.resolve(r.coord())
                if r.empty():
                    return
            ext = r.coord()
            corner = self.resolve((s[0] + ext[0], s[1] + ext[1]))
            p = self.make('rect'); p.filled = filled; p.origin = s
            p.size = (corner[0] - s[0], corner[1] - s[1]); p.points = [s, corner]
            self.emit(p)
            self.move_dp(self.resolve((s[0] + ext[0], s[1])))
            if r.truncated:
                return

    def pdi_poly(self, op, r):
        filled = op in (0x35, 0x37); has_start = op in (0x36, 0x37)
        s = self.dp
        if has_start:
            if r.empty():
                return
            s = self.resolve(r.coord())
        p = self.make('poly'); p.filled = filled; p.origin = s; p.points = [s]
        prev = s
        while not r.empty() and len(p.points) < 256:
            w = r.coord()
            if w[0] == 0.0 and w[1] == 0.0:
                continue
            prev = self.resolve((prev[0] + w[0], prev[1] + w[1]))
            p.points.append(prev)
            if r.truncated:
                break
        if len(p.points) < 3:
            return
        self.emit(p)
        self.move_dp(s)

    def pdi_field(self, r):
        if r.empty():
            self.field_origin = (0.0, 0.0); self.field_size = (1.0, 1.0)
            self.move_dp(self.field_origin)
            return
        first = r.coord()
        if r.empty():
            self.field_origin = self.dp; self.field_size = first
            return
        ext = r.coord()
        self.field_origin = self.resolve(first); self.field_size = ext
        self.move_dp(self.field_origin)

    def pdi_incremental(self, op, r, ops):
        if op == 0x39:
            p = self.make('incr'); p.origin = self.field_origin; p.size = self.field_size
            p.points = [self.field_origin]; p.incr = [b & 0x3F for b in ops]
            self.emit(p)
            return
        if r.empty():
            return
        inc = r.coord()
        p = self.make('poly'); p.filled = op == 0x3B
        p.origin = self.dp; p.points = [self.dp]
        cur = self.dp
        while not r.empty() and len(p.points) < 256:
            d = r.fixed() & 7
            dx = inc[0] if d in (1, 2, 3) else (-inc[0] if d in (5, 6, 7) else 0.0)
            dy = inc[1] if d in (3, 4, 5) else (-inc[1] if d in (7, 0, 1) else 0.0)
            cur = self.resolve((cur[0] + dx, cur[1] + dy))
            p.points.append(cur)
        if len(p.points) >= 2:
            self.emit(p)
        self.move_dp(cur)

    # --- выпуск и перемещения ------------------------------------------------
    def make(self, kind):
        p = Prim(kind)
        p.pel = self.pel; p.line_tex = self.line_tex; p.pattern = self.pattern
        p.mask_size = self.mask_size; p.highlighted = self.highlight
        p.mode = self.colour.mode; p.colour = self.colour.drawing(); p.background = self.colour.background()
        if self.colour.mode != 0:
            p.caddr = self.colour.draw_addr
        if self.colour.mode == 2:
            p.baddr = self.colour.bg_addr
        p.daddr = self.colour.draw_addr
        bl = self.blink[self.colour.draw_addr]
        if bl:
            p.blinking = True; p.blink_addr = bl[0]
            p.blink_to = self.colour.map[bl[0]] if bl[0] >= 0 else bl[1]
        return p

    def emit(self, p):
        if self.collecting in ('drcs', 'mask'):
            self.draw_into_definition(p)
            return
        self.snap_state()
        p.t = self.clock
        self.prims.append(p)
        self.events.append(('p', self.clock, p))
        self.clock += DRAW_COST.get(p.kind, 0.02) * (2.0 if p.filled else 1.0)

    def stays_on_row(self, pt):
        horiz = self.text.path in (0, 1)
        across = pt[1] - self.cursor[1] if horiz else pt[0] - self.cursor[0]
        row = abs(self.text.field[1] if horiz else self.text.field[0])
        return abs(across) < max(row, 1e-9) * 0.5

    def move_dp(self, pt):
        self.dp = pt
        if self.text.move in (0, 2):
            if not self.stays_on_row(pt):
                self.wrap = 0
            self.cursor = pt

    def move_cursor(self, pt):
        self.wrap = 0
        self.cursor = self.resolve(pt)
        if self.text.move in (0, 1):
            self.dp = self.cursor

    def move_by(self, mv):
        t = self.text
        horiz = t.path in (0, 1)
        chard = abs(t.field[0] if horiz else t.field[1]) * (1.0, 1.25, 1.5, 1.0)[t.ics & 3]
        rowd = abs(t.field[1] if horiz else t.field[0]) * (1.0, 1.25, 1.5, 2.0)[t.irs & 3]
        dist = chard if mv in ('fwd', 'back') else rowd
        px, py = ((1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0))[t.path]
        sx, sy = {'fwd': (px, py), 'back': (-px, -py), 'down': (py, -px), 'up': (-py, px)}[mv]
        nx, ny = self.cursor[0] + sx * dist, self.cursor[1] + sy * dist
        right = self.field_origin[0] + abs(self.field_size[0])
        wrapped = mv == 'fwd' and t.path == 0 and nx + abs(t.field[0]) > right + 1e-9
        if wrapped:
            nx = self.field_origin[0]; ny -= rowd
        self.move_cursor((nx, ny))
        if wrapped:
            self.wrap = 1

    # --- макросы, DRCS, маски -------------------------------------------------
    def begin_definition(self, what, code):
        self.collecting = what; self.body = bytearray(); self.def_code = code
        self.drcs_target = None; self.mask_target = None
        self.def_frame = len(self.frames) - 1 if self.frames else 0
        self.def_had_code = False
        if what == 'drcs':
            w, h = self.drcs_size()
            if 0x20 <= code <= 0x7F:
                if code not in self.drcs:
                    if self.storage + 11 <= STORAGE:
                        self.storage += 11
                        self.drcs[code] = DrcsChar(code, w, h)
                else:
                    self.drcs[code] = DrcsChar(code, w, h)
                self.drcs_target = self.drcs.get(code)
            self.last_drcs = code; self.have_last_drcs = True
            return
        if what == 'mask':
            if not 0x41 <= code <= 0x44:
                self.collecting = None
                return
            self.mask_target = self.masks[code - 0x41] = Mask(16, 16)

    def end_definition(self):
        c = self.collecting
        if c is None:
            return
        if c in ('macro', 'macrox'):
            code = self.def_code
            if 0x20 <= code <= 0x7F:
                old = self.macros.pop(code, None)
                if old:
                    self.storage -= min(self.storage, len(old[0]))
                if self.body and self.storage + len(self.body) <= STORAGE:
                    self.storage += len(self.body)
                    self.macros[code] = (bytes(self.body), self.def_transmit)
        else:
            if c == 'drcs' and not self.def_had_code and self.drcs_target is not None:
                self.drcs.pop(self.def_code, None)
                self.storage -= min(self.storage, 11)
            self.dp = (0.0, 0.0)
        self.collecting = None; self.body = bytearray()
        self.drcs_target = None; self.mask_target = None; self.def_transmit = False

    def draw_into_definition(self, p):
        tgt = self.drcs_target or self.mask_target
        if tgt is None or tgt.w == 0 or tgt.h == 0:
            return
        black = p.colour[:3] == (0, 0, 0) and not p.colour[3]
        surf = Surface(tgt.w, tgt.h)
        ras = Rasteriser(surf, tgt.w, tgt.h, unit=True)
        draw_primitive(ras, p, None, definition=True, drcs=self.drcs)
        for i, v in enumerate(surf.cells):
            if v is not None:
                tgt.el[i] = not black

    def invoke_macro(self, code):
        if self.collecting == 'macrox' and code == self.def_code:
            return
        m = self.macros.get(code)
        if not m or m[1]:
            return
        if len(self.frames) > MAX_MACRO_DEPTH:
            return
        self.frames.append([m[0], 0])

# ---------------------------------------------------------------------------
# Растр приёмника (naplps_raster.cpp, nabts_raster_view.cpp)
# ---------------------------------------------------------------------------
class Surface:
    def __init__(self, w, h):
        self.w = w; self.h = h; self.cells = [None] * (w * h)

    def put(self, c, r, ink):
        if 0 <= c < self.w and 0 <= r < self.h:
            self.cells[r * self.w + c] = ink


def _pel_anchor(pos, ext):
    return int(math.floor(min(pos, pos + ext)))


def _pel_span(ext):
    return max(1, int(math.floor(abs(ext) + 0.5)))


TEX_PERIODS = {0: ([(0.0, 1.0)], 1.0), 1: ([(0.0, 0.0)], 2.0),
               2: ([(0.0, 2.0)], 6.0), 3: ([(0.0, 2.0), (4.0, 4.0)], 6.0)}


def _norm(a):
    a = math.fmod(a, 2 * math.pi)
    return a + 2 * math.pi if a < 0 else a


def arc_polyline(ctrl, tol):
    if len(ctrl) != 3:
        return list(ctrl)
    s, t, e = ctrl
    if math.hypot(s[0] - e[0], s[1] - e[1]) < 1e-9:
        cx, cy = (s[0] + t[0]) / 2, (s[1] + t[1]) / 2
        rad = math.hypot(s[0] - t[0], s[1] - t[1]) / 2
        if rad < 1e-9:
            return [s]
        a0 = math.atan2(s[1] - cy, s[0] - cx)
        step = math.sqrt(8 * tol / rad)
        n = min(max(int(math.ceil(2 * math.pi / step)) if step > 0 else 1, 3), 8192)
        return [(cx + rad * math.cos(a0 + 2 * math.pi * i / n), cy + rad * math.sin(a0 + 2 * math.pi * i / n))
                for i in range(n + 1)]
    ax, ay = s; bx, by = t; cx, cy = e
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-12:
        return [s, e]
    a2, b2, c2 = ax * ax + ay * ay, bx * bx + by * by, cx * cx + cy * cy
    ox = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    oy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    rad = math.hypot(ox - ax, oy - ay)
    if rad < 1e-9:
        return [s, e]
    a0 = math.atan2(ay - oy, ax - ox)
    to_t = _norm(math.atan2(by - oy, bx - ox) - a0)
    sweep = _norm(math.atan2(cy - oy, cx - ox) - a0)
    if to_t > sweep:
        sweep -= 2 * math.pi
    step = math.sqrt(8 * tol / rad)
    n = min(max(int(math.ceil(abs(sweep) / step)) if step > 0 else 1, 1), 8192)
    return [(ox + rad * math.cos(a0 + sweep * i / n), oy + rad * math.sin(a0 + sweep * i / n))
            for i in range(n + 1)]


class Rasteriser:
    def __init__(self, surf, gw, gh, unit=False):
        self.s = surf
        self.cpu = float(gw)                         # колонок на единицу
        self.rpu = float(gh) if unit else gh / DISPLAY_H
        self.traced = None                           # (c0, r0, w, h, bytearray)

    def cp(self, p):
        return (p[0] * self.cpu, p[1] * self.rpu)

    def cs(self, sz):
        return (sz[0] * self.cpu, sz[1] * self.rpu)

    def block(self, ac, ar, pel, ink):
        lc = ac + _pel_span(pel[0]) - 1; lr = ar + _pel_span(pel[1]) - 1
        tr = self.traced
        for r in range(ar, lr + 1):
            for c in range(ac, lc + 1):
                if tr is not None:
                    c0, r0, w, h, cells = tr
                    if c0 <= c < c0 + w and r0 <= r < r0 + h:
                        cells[(r - r0) * w + (c - c0)] = 1
                    continue
                self.s.put(c, r, ink)

    def stamp_cells(self, where, pel, ink):
        self.block(_pel_anchor(where[0], pel[0]), _pel_anchor(where[1], pel[1]), pel, ink)

    def stamp(self, pt, pel, ink):
        self.stamp_cells(self.cp(pt), self.cs(pel), ink)

    def sweep(self, a, b, pel, ink):
        fc, fr = _pel_anchor(a[0], pel[0]), _pel_anchor(a[1], pel[1])
        tc, tr = _pel_anchor(b[0], pel[0]), _pel_anchor(b[1], pel[1])
        cols, rows = abs(tc - fc), abs(tr - fr)
        cst = 1 if tc >= fc else -1; rst = 1 if tr >= fr else -1
        c, r = fc, fr; err = cols - rows
        while True:
            self.block(c, r, pel, ink)
            if c == tc and r == tr:
                return
            e2 = 2 * err
            if e2 > -rows:
                err -= rows; c += cst
            if e2 < cols:
                err += cols; r += rst

    def stroke(self, pts, pel, tex, ink, closed=False):
        if not pts:
            return
        if len(pts) == 1:
            self.stamp(pts[0], pel, ink)
            return
        ps = self.cs(pel)
        on, plen = TEX_PERIODS[tex]
        phase = 0.0
        n = len(pts) if closed else len(pts) - 1
        for i in range(n):
            a = self.cp(pts[i]); b = self.cp(pts[(i + 1) % len(pts)])
            run, rise = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(run, rise)
            st = 0.0
            if ln > 0:
                ux, uy = run / ln, rise / ln
                st = float('inf')
                if abs(ux) > 1e-9:
                    st = min(st, ps[0] / abs(ux))
                if abs(uy) > 1e-9:
                    st = min(st, ps[1] / abs(uy))
                if not math.isfinite(st):
                    st = 0.0
            if tex == 0 or not st > 0:
                self.sweep(a, b, ps, ink)
                continue

            def at(k):
                t = min(max(k * st / ln, 0.0), 1.0)
                return (a[0] + run * t, a[1] + rise * t)
            ent = phase; ext = ent + ln / st
            p = math.floor(ent / plen)
            while p <= math.floor(ext / plen):
                for f, l in on:
                    rf = max(p * plen + f, ent); rl = min(p * plen + l, ext)
                    if rl >= rf:
                        self.sweep(at(rf - ent), at(rl - ent), ps, ink)
                p += 1
            self.stamp_cells(a, ps, ink); self.stamp_cells(b, ps, ink)
            phase = ext

    def fill(self, pts, pel, pattern, mask_size, mask, ink):
        if len(pts) < 2:
            if pts:
                self.stamp(pts[0], pel, ink)
            return
        ps = self.cs(pel)
        cpts = [self.cp(p) for p in pts]
        left = min(p[0] for p in cpts) + min(0.0, ps[0]) - 2
        right = max(p[0] for p in cpts) + max(0.0, ps[0]) + 2
        bottom = min(p[1] for p in cpts) + min(0.0, ps[1]) - 2
        top = max(p[1] for p in cpts) + max(0.0, ps[1]) + 2
        W, H = self.s.w, self.s.h
        clc = lambda v: min(max(int(math.floor(v)), -W), 2 * W)
        clr = lambda v: min(max(int(math.floor(v)), -H), 2 * H)
        c0, r0 = clc(left), clr(bottom)
        w = clc(math.ceil(right)) - c0 + 1; h = clr(math.ceil(top)) - r0 + 1
        if w <= 0 or h <= 0:
            return
        cells = bytearray(w * h)
        self.traced = (c0, r0, w, h, cells)
        self.stroke(pts, pel, 0, ink, closed=True)
        self.traced = None
        enclosed = bytearray(w * h)
        n = len(cpts)
        for row in range(r0, r0 + h):
            smp = row + 0.5
            xs = []
            for i in range(n):
                f = cpts[i]; t = cpts[(i + 1) % n]
                if (f[1] <= smp < t[1]) or (t[1] <= smp < f[1]):
                    xs.append(f[0] + (smp - f[1]) / (t[1] - f[1]) * (t[0] - f[0]))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                a = max(c0, int(math.ceil(xs[i] - 0.5)))
                b = min(c0 + w - 1, int(math.floor(xs[i + 1] - 0.5)))
                base = (row - r0) * w - c0
                for col in range(a, b + 1):
                    enclosed[base + col] = 1
        outside = bytearray(w * h)
        # клетки рядом с контуром — не стартовые
        near = bytearray(w * h)
        for idx in range(w * h):
            if cells[idx]:
                y, x = divmod(idx, w)
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if 0 <= yy < h:
                        for dx in (-1, 0, 1):
                            xx = x + dx
                            if 0 <= xx < w:
                                near[yy * w + xx] = 1
        stack = []
        for idx in range(w * h):
            if not enclosed[idx] and not cells[idx] and not near[idx]:
                outside[idx] = 1; stack.append(idx)
        while stack:
            idx = stack.pop()
            y, x = divmod(idx, w)
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if 0 <= nx < w and 0 <= ny < h:
                    j = ny * w + nx
                    if not outside[j] and not cells[j]:
                        outside[j] = 1; stack.append(j)
        for row in range(max(r0, 0), min(r0 + h, H)):
            base = (row - r0) * w
            for col in range(max(c0, 0), min(c0 + w, W)):
                if outside[base + col - c0]:
                    continue
                if pattern and not _pattern_covers(pattern, pel, mask_size, mask,
                                                   (col + 0.5) / self.cpu, (row + 0.5) / self.rpu):
                    continue
                self.s.cells[row * W + col] = ink

    def fill_rect_cells(self, left, bottom, width, height, ink):
        if width <= 0 or height <= 0:
            return
        fc = int(math.floor(left)); lc = max(fc, int(math.ceil(left + width)) - 1)
        fr = int(math.floor(bottom)); lr = max(fr, int(math.ceil(bottom + height)) - 1)
        for r in range(fr, lr + 1):
            for c in range(fc, lc + 1):
                self.s.put(c, r, ink)

    def field_cells(self, p):
        f = self.cs(p.size)
        if abs(f[0]) < 1e-9 or abs(f[1]) < 1e-9:
            return None
        return max(1, int(round(abs(f[0])))), max(1, int(round(abs(f[1]))))

    def deposit_pattern(self, p, rows_bits, pw, ph, ink, bg):
        fc = self.field_cells(p)
        if fc is None:
            return
        cols, rows = fc
        o = self.cp(p.origin)
        cstep = -1.0 if p.size[0] < 0 else 1.0
        rstep = -1.0 if p.size[1] < 0 else 1.0
        for row in range(rows):
            sr = min(max(ph * row // rows, 0), ph - 1)
            bits = rows_bits[sr]
            for col in range(cols):
                sc = min(max(pw * col // cols, 0), pw - 1)
                lit = (bits >> (pw - 1 - sc)) & 1
                pen = (bg if lit else ink) if p.reverse else (ink if lit else bg)
                if pen is None:
                    continue
                pc, pr = col, rows - 1 - row
                if p.rotation == 1:
                    pc, pr = -pr + rows - 1, pc
                elif p.rotation == 2:
                    pc, pr = cols - 1 - pc, rows - 1 - pr
                elif p.rotation == 3:
                    pc, pr = pr, cols - 1 - pc
                sc_ = int(math.floor(o[0] + cstep * pc + (-1.0 if cstep < 0 else 0.0)))
                sr_ = int(math.floor(o[1] + rstep * pr + (-1.0 if rstep < 0 else 0.0)))
                self.s.put(sc_, sr_, pen)

    def deposit_char(self, p, drcs, ink, bg):
        if p.rep == 'M':
            code = p.char
            six = ((code & 0x1F) | ((code >> 1) & 0x20)) if (code & 0x20 or code == 0x5F) else 0
            f = self.cs(p.size); o = self.cp(p.origin)
            left = min(o[0], o[0] + f[0]); bottom = min(o[1], o[1] + f[1])
            w, h = abs(f[0]), abs(f[1])
            if w < 1e-9 or h < 1e-9:
                return
            pel = self.cs(p.pel)
            ic = abs(pel[0]) if p.underlined else 0.0
            ir = abs(pel[1]) if p.underlined else 0.0
            ew, eh = w / 2.0, h / 3.0
            if bg is not None:
                self.fill_rect_cells(left, bottom, w, h, bg)
            for el in range(6):
                if not six & (1 << el):
                    continue
                col, rft = el % 2, el // 2
                self.fill_rect_cells(left + col * ew, bottom + (2 - rft) * eh,
                                     max(0.0, ew - ic), max(0.0, eh - ir),
                                     bg if (p.reverse and bg is not None) else ink)
            return
        if p.rep == 'D':
            g = (drcs or {}).get(p.char)
            if g is None:
                return
            gw = min(g.w, 8)
            rows = [0] * g.h
            for r in range(g.h):
                bits = 0
                for c in range(min(g.w, 8)):
                    if g.el[r * g.w + c]:
                        bits |= 1 << (g.w - 1 - c)
                rows[g.h - 1 - r] = bits >> (g.w - gw) if g.w > gw else bits
            self.deposit_pattern(p, rows, gw, g.h, ink, bg)
            return
        u = p.char if p.rep == 'P' else SUPPLEMENTARY[p.char - 0x20]
        if p.rep == 'P' and not 0x20 <= u < 0x7F:
            u = 0x20
        fc = self.field_cells(p)
        if fc is None:
            return
        face = _face_for(self.s.w if not self.unit_grid() else 256, fc[0], fc[1])
        pat = _face_pattern(face, u if isinstance(u, int) else ord(u[0]))
        if pat is None:
            return
        self.deposit_pattern(p, pat, face[1], face[2], ink, bg)

    def unit_grid(self):
        return abs(self.rpu - self.s.h) < 1e-9

    def colour_run(self, origin, size, pel, inks):
        if not inks:
            return
        left = min(origin[0], origin[0] + size[0]); bottom = min(origin[1], origin[1] + size[1])
        wu, hu = abs(size[0]), abs(size[1]); top = bottom + hu
        sx = abs(pel[0]) if abs(pel[0]) > 1e-9 else 1.0 / self.cpu
        sy = abs(pel[1]) if abs(pel[1]) > 1e-9 else 1.0 / self.rpu
        cols = max(1, int(math.floor(wu / sx)))
        for i, ink in enumerate(inks):
            col, row = i % cols, i // cols
            wx, wy = left + col * sx, top - (row + 1) * sy
            if wy + sy <= bottom:
                break
            self.stamp((wx, wy), (sx, sy), ink)


def _pattern_covers(pattern, pel, mask_size, mask, x, y):
    def band(v, pitch):
        if not abs(pitch) > 1e-9:
            return True
        return math.fmod(abs(math.floor(v / abs(pitch))), 2.0) < 1.0
    if pattern == 1:
        return band(x, pel[0])
    if pattern == 2:
        return band(y, pel[1])
    if pattern == 3:
        return band(x, pel[0]) or band(y, pel[1])
    if mask is None or not mask.defined():
        return True
    sx, sy = abs(mask_size[0]), abs(mask_size[1])
    if not (sx > 1e-9 and sy > 1e-9):
        return True
    fx = math.fmod(x, sx) / sx; fy = math.fmod(y, sy) / sy
    if fx < 0:
        fx += 1
    if fy < 0:
        fy += 1
    if mask_size[0] < 0:
        fx = 1 - fx
    if mask_size[1] < 0:
        fy = 1 - fy
    ex = min(max(int(fx * mask.w), 0), mask.w - 1)
    ey = min(max(int(fy * mask.h), 0), mask.h - 1)
    return mask.el[ey * mask.w + ex]


SUPPLEMENTARY = (
    ' ¡¢£$¥#§¤‘“«←↑→↓°±²³×µ¶·÷’”»¼½¾¿'
    '̸̧̨̲⃗̀́̂̃̄̆̇̈̊̋̌'
    '―¹®©™♪─│╱╲◢◣⅛⅜⅝⅞'
    'ΩÆĐªĦ┼ĲĿŁØŒºÞŦŊŉĸæđðħıĳŀłøœßþŧŋ ')
assert len(SUPPLEMENTARY) == 96

_FONT_INDEX = {c: i for i, c in enumerate(naplps_font.CODES)}


def _face_for(gridw, cols, rows):
    held = 1 if gridw <= 256 else len(naplps_font.FACES)
    chosen = None; cdiv = False; cel = 0
    for face in naplps_font.FACES[:held]:
        _, fw, fh, _ = face
        if fw > cols or fh > rows:
            continue
        div = rows % fh == 0
        el = fw * fh
        if chosen is not None:
            if cdiv and not div:
                continue
            if div == cdiv and el <= cel:
                continue
        chosen, cdiv, cel = face, div, el
    return chosen or naplps_font.FACES[0]


def _face_pattern(face, code):
    i = _FONT_INDEX.get(code)
    if i is None:
        return None
    h = face[2]
    return face[3][i * h:(i + 1) * h]


def fg_spec(p):
    """«Чернила» примитива: адрес в карте цветов или прямой цвет (с адресом для мигания)."""
    return ('m', p.caddr) if p.caddr >= 0 else ('d', p.colour, p.daddr)


def draw_primitive(ras, p, page, definition=False, drcs=None, pen=None):
    """pen — функция «чернила → то, что кладётся в клетку» (Player кладёт номера чернил,
    чтобы смена карты цветов и мигание перекрашивали уже нарисованное, §5.3.2.5).
    Без pen в клетку кладётся цвет."""
    if pen is None:
        def pen(spec):
            if spec[0] == 'm':
                return page.colour_map[spec[1] % 16] if page is not None else p.colour
            return spec[1]
    ink = p.colour if definition else pen(fg_spec(p))
    bg = pen(('m', p.baddr)) if p.mode == 2 and p.baddr >= 0 and not definition else None
    k = p.kind
    if k == 'point':
        if p.points:
            ras.stamp(p.points[0], p.pel, ink)
    elif k == 'line':
        ras.stroke(p.points, p.pel, p.line_tex, ink)
    elif k in ('arc', 'rect', 'poly'):
        if k == 'arc':
            outline = arc_polyline(p.points, 0.2 / ras.cpu)
        elif k == 'rect':
            o = p.origin; fx, fy = o[0] + p.size[0], o[1] + p.size[1]
            outline = [o, (fx, o[1]), (fx, fy), (o[0], fy)]
        else:
            outline = p.points
        if not p.filled:
            ras.stroke(outline, p.pel, p.line_tex, ink, closed=(k == 'rect'))
            return
        mask = None
        if p.pattern >= 4 and page is not None:
            mask = page.masks[p.pattern - 4]
        ras.fill(outline, p.pel, p.pattern if not definition else 0, p.mask_size, mask, ink)
        if p.highlighted and not definition:
            hl = bg if bg is not None else pen(('c', BLACK))
            ras.stroke(outline, p.pel, 0, hl, closed=(k != 'arc'))
    elif k == 'incr':
        if definition:
            ras.stroke(p.points, p.pel, p.line_tex, ink)
            return
        inks = []
        for e in p.incr or []:
            if p.mode == 0:
                g = lambda off: (((e >> (3 + off)) & 1) << 1 | ((e >> off) & 1)) << 1
                inks.append(pen(('c', (g(2), g(1), g(0), False))))
            else:
                inks.append(pen(('m', e % 16)))
        ras.colour_run(p.origin, p.size, p.pel, inks)
    elif k == 'char':
        ras.deposit_char(p, drcs if drcs is not None else (page.drcs if page else {}), ink, bg)


_LUT8 = [v * 255 // 7 for v in range(8)]


def _rgb(c):
    if c is None or c[3]:
        return (0, 0, 0)
    return (_LUT8[min(c[1], 7)], _LUT8[min(c[0], 7)], _LUT8[min(c[2], 7)])


class Player:
    """Показ страницы во времени, как на экране приёмника: примитивы появляются в своё
    время (скорость рисования + паузы WAIT), очистка экрана стирает нарисованное, смена
    карты цветов перекрашивает уже нарисованное (§5.3.2.5), процессы мигания (§5.3.2.7,
    §6.2.8) чередуют цвет записи карты. Клетка экрана хранит номер «чернил», цвет
    вычисляется на каждый кадр."""

    def __init__(self, page, grid=(256, 200)):
        self.page = page; self.gw, self.gh = grid
        self.events = getattr(page, 'events', None) or [('p', 0.0, p) for p in page.prims]
        self.end = getattr(page, 'end', 0.0)
        self.i = 0
        self.map = list(DEFAULT_MAP); self.blink = [None] * 16
        self.inks = [('c', BLACK)]; self.ink_id = {('c', BLACK): 0}
        self.has_blink = any(e[0] == 'blink' and any(e[2]) for e in self.events)
        self._new_surface()

    def _new_surface(self):
        self.surf = Surface(self.gw, self.gh)
        self.ras = Rasteriser(self.surf, self.gw, self.gh)

    def pen(self, spec):
        k = self.ink_id.get(spec)
        if k is None:
            k = self.ink_id[spec] = len(self.inks); self.inks.append(spec)
        return k

    def advance(self, T=None):
        """Выполнить события до момента T (None — все). -> было ли что-то новое."""
        ev = self.events; changed = False
        while self.i < len(ev) and (T is None or ev[self.i][1] <= T):
            e = ev[self.i]; self.i += 1; changed = True
            if e[0] == 'p':
                draw_primitive(self.ras, e[2], self.page, pen=self.pen)
            elif e[0] == 'clear':
                self._new_surface()
            elif e[0] == 'map':
                self.map = e[2]
            elif e[0] == 'blink':
                self.blink = e[2]
        return changed

    def done(self):
        return self.i >= len(self.events)

    def _colour(self, spec, T):
        if spec[0] == 'c':
            return spec[1]
        if spec[0] == 'm':
            a = spec[1] % 16; col = self.map[a]
        else:
            a = spec[2] % 16; col = spec[1]
        b = self.blink[a]
        if b and T is not None:
            to, to_col, on, off, delay, start = b
            if on and off:
                ph = T - start - 0.1 * delay
                if ph >= 0 and (ph % (0.1 * (on + off))) >= 0.1 * on:
                    col = self.map[to % 16] if to >= 0 else to_col
        return col

    def blink_key(self, T):
        """Цвета всех чернил в момент T — кадр меняется только вместе с ними."""
        return tuple(self._colour(s, T) for s in self.inks)

    def image(self, T=None):
        """Кадр в момент T (None — мигание в фазе «включено»)."""
        import numpy as np
        from PIL import Image
        lut = np.array([_rgb(self._colour(s, T)) for s in self.inks], np.uint8)
        ids = np.fromiter((0 if c is None else c for c in self.surf.cells), np.int32, len(self.surf.cells))
        rgb = lut[ids].reshape(self.gh, self.gw, 3)[::-1]
        return Image.fromarray(np.ascontiguousarray(rgb), 'RGB')


def render_page(page, grid=(256, 200)):
    """Страница, как она стоит на экране после показа → картинка PIL."""
    pl = Player(page, grid)
    pl.advance(None)
    return pl.image(None)


def page_text(page):
    """Текст страницы, как он остаётся на экране: позже нарисованный символ заменяет
    прежний в той же клетке, залитый прямоугольник стирает символы под собой,
    тень (та же строка со сдвигом на пиксель) не повторяется."""
    cells = {}; order = 0; marks = ''
    for p in page.prims:
        if p.kind == 'rect' and p.filled:
            x0, y0 = p.origin; x1, y1 = x0 + p.size[0], y0 + p.size[1]
            xl, xh = min(x0, x1), max(x0, x1); yl, yh = min(y0, y1), max(y0, y1)
            for key in [k for k, v in cells.items()
                        if xl <= v[1] + v[3] / 2 <= xh and yl <= v[0] + v[2] / 2 <= yh]:
                del cells[key]
            continue
        if p.kind != 'char' or p.rep in ('M', 'D'):
            continue
        if p.rep == 'S' and supplementary_nonspacing(p.char):
            marks += SUPPLEMENTARY[p.char - 0x20]
            continue
        ch = chr(p.char) if p.rep == 'P' and 0x20 <= p.char < 0x7F else (
            SUPPLEMENTARY[p.char - 0x20] if p.rep == 'S' else ' ')
        x, y = p.origin; w, h = abs(p.size[0]), abs(p.size[1])
        t = ch + marks; marks = ''
        if t.strip() and any(v[4] == t and abs(v[1] - x) < w * 0.34 and abs(v[0] - y) < h * 0.34
                             for v in list(cells.values())[-120:]):
            continue                                    # тень
        key = (round(x * 1024), round(y * 1024))           # та же позиция (точнее пикселя)
        cells.pop(key, None)
        order += 1
        cells[key] = (y, x, h, w, t, order)
    placed = sorted(cells.values(), key=lambda v: (-round(v[0], 6), v[1]))
    if not placed:
        return ''
    out = []; ly, lh = placed[0][0], placed[0][2]
    for i, (y, x, h, w, t, _) in enumerate(placed):
        if i and abs(y - ly) > max(max(lh, h) / 2.0, 1e-6):
            out.append('\n'); ly, lh = y, h
        out.append(t)
    return '\n'.join(l.rstrip() for l in ''.join(out).split('\n'))

# ---------------------------------------------------------------------------
# Весь файл
# ---------------------------------------------------------------------------
def is_presentation(t):
    return t in (0, 1, 3)


def read_t33(path, progress=None):
    """Файл .t33 → (записи каталога, сводка)."""
    raw = open(path, 'rb').read()
    n = len(raw) // PACKET
    cat = Catalogue()
    msgs = []
    ra = RecordAssembler(msgs.append)
    ga = GroupAssembler(ra.add)
    for i in range(n):
        ga.add(decode_packet(raw[i * PACKET:(i + 1) * PACKET]))
        if msgs:
            for m in msgs:
                cat.merge(m)
            msgs.clear()
        if progress and i % 20000 == 0:
            progress(i, n)
    ga.flush(); ra.flush()
    for m in msgs:
        cat.merge(m)
    folded, dropped = cat.reconcile()
    recs = cat.records()
    summary = dict(packets=n, groups=ga.stats, records=ra.stats, foreign=ra.foreign,
                   identities_folded=folded, identities_dropped=dropped, catalogued=len(recs))
    return recs, summary


def interpret(records, grid=(256, 200)):
    """Каждой записи представления — страница (как nabts_interpret_records)."""
    it = Interpreter(grid)
    support = {}
    latest = {}
    for r in records:
        if is_presentation(r.type):
            if r.flags.get('support_record'):
                support[r.channel] = r
            latest[(r.channel, r.address)] = r
    pred = {}
    for (ch, a), r in latest.items():
        succ = r.more_address
        if succ is None:
            if not r.flags.get('more'):
                continue
            succ = _algorithmic_more(r.address)
            if succ is None:
                continue
        if succ == r.address:
            continue
        pred[(ch, succ)] = r
    for r in records:
        r.page = None; r.chain_base = r.address; r.chain_pos = 0
        if not is_presentation(r.type) or not r.data:
            continue
        prefix = []; seen = {r.address}; addr = r.address; ring = False
        while (r.channel, addr) in pred:
            pr = pred[(r.channel, addr)]
            if pr.address in seen:
                ring = True; break
            seen.add(pr.address); prefix.insert(0, pr); addr = pr.address
        if ring:
            base = r.address; bi = 0
            for i, m in enumerate(prefix):
                if m.address < base:
                    base, bi = m.address, i
            prefix = [] if base == r.address else prefix[bi:]
            addr = base
        r.chain_base = addr; r.chain_pos = len(prefix)
        needs = r.flags.get('support_needed') or any(m.flags.get('support_needed') for m in prefix)
        cap = r.flags.get('caption') or any(m.flags.get('caption') for m in prefix)
        it.reset_decoder()
        if needs and not r.flags.get('support_record') and r.channel in support:
            it.run(support[r.channel].data)
        if cap:
            it.apply_caption_state()
        keep = False
        for m in prefix:
            it.run(m.data, keep); keep = True
        r.page = it.run(r.data, keep)
        r.text = page_text(r.page)
    return records


def record_label(r):
    return '%03X/%s v%d' % (r.channel, r.addr_text, r.version)


def flags_text(r):
    names = dict(caption='captions', cyclic='cyclic', priority='priority', alarm='alarm',
                 update='update', support_record='support', support_needed='needs support',
                 index='index', more='more')
    return ', '.join(v for k, v in names.items() if r.flags.get(k))


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '_nabts'
    os.makedirs(out, exist_ok=True)
    recs, summ = read_t33(src, lambda i, n: print('PROGRESS %d %d' % (i, n), flush=True))
    print('Packets %d, groups %d, records in catalogue %d' % (summ['packets'], summ['groups']['complete'],
                                                          summ['catalogued']), flush=True)
    interpret(recs)
    txt = []
    for r in recs:
        name = '%03X-%s-v%d' % (r.channel, r.addr_text, r.version)
        txt.append('=== %s  type %d  received %d  %s %s' % (record_label(r), r.type, r.seen,
                                                         flags_text(r), r.purpose))
        if r.page is not None:
            render_page(r.page).save(os.path.join(out, name + '.png'))
            txt.append(r.text)
        txt.append('')
    open(os.path.join(out, 'records.txt'), 'w', encoding='utf-8').write('\n'.join(txt))
    print('Done:', out)


if __name__ == '__main__':
    main()
