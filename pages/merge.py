# Собирает страницы как можно полнее: основа — сборка с учётом границ полей,
# недостающие ряды берутся из ближайшей по времени версии той же подстраницы полной сборки.
#   python merge.py good.json full.json out.json [extras.json]
import sys, json, os
ex_path = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extras.json')
clk = json.load(open(ex_path, encoding='utf-8')).get('_meta', {}).get('clock') if os.path.exists(ex_path) else None
def set_clock(hdr, t):
    # часы в заголовке (позиции 32-39) — время эфира этой версии, а не случайной передачи
    if not clk or not hdr: return hdr
    s = int(round(clk[0] + clk[1] * t)); txt = f'{s // 3600:02}:{s // 60 % 60:02}:{s % 60:02}'
    h = list(hdr)
    if chr(h[34]) == ':' and chr(h[37]) == ':': h[32:40] = [ord(c) for c in txt]
    return h
good = json.load(open(sys.argv[1])); full = json.load(open(sys.argv[2]))
out = {}; filled = 0; shared_filled = [0]
for pid in sorted(set(good) | set(full)):
    G = good.get(pid) or []; F = full.get(pid) or []
    gs = {s.get('s') for s in G}                                # подстраницы, которых нет в основе, — из полной
    G = list(G) + [{'t': s['t'], 'n': s['n'], 'rows': {}, 'c': {}, 's': s.get('s')} for s in F if s.get('s') not in gs]
    # ряд общий, если во всех подстраницах он один и тот же (с точностью до 3 байт)
    shared = set()
    if len({x.get('s') for x in G}) > 1:
        byrow = {}
        for x in G + F:
            for r, b in x['rows'].items(): byrow.setdefault(r, []).append((x.get('s'), b))
        for r, L in byrow.items():
            if r == '0': continue
            ref = max(L, key=lambda e: sum(1 for _, b in L if b == e[1]))[1]
            if all(sum(p != q for p, q in zip(b, ref)) <= 3 for _, b in L): shared.add(r)
    res = []
    for s in G:
        rows = dict(s['rows']); cnt = dict(s.get('c') or {})     # c — сколько копий подтвердили ряд
        same = [x for x in F if x.get('s') == s.get('s')]        # сначала та же подстраница
        if same:
            f = min(same, key=lambda x: abs(x['t'] - s['t']))
            for r, b in f['rows'].items():
                if r not in rows: rows[r] = b; cnt[r] = (f.get('c') or {}).get(r, 1); filled += 1
        # затем ряды, одинаковые во всех подстраницах (логотип, рубрики, подвал), — из любой
        for r in [r for r in shared if r not in rows]:
            src = min((x for x in G + F if r in x['rows']), key=lambda x: abs(x['t'] - s['t']), default=None)
            if src: rows[r] = src['rows'][r]; cnt[r] = (src.get('c') or {}).get(r, 1); filled += 1; shared_filled[0] += 1
        if '0' in rows: rows['0'] = set_clock(rows['0'], s['t'])
        res.append({'t': s['t'], 'n': s['n'], 'rows': rows, 'c': cnt, **({'s': s['s']} if s.get('s') else {})})
    res.sort(key=lambda x: (x.get('s') or '', x['t'])); out[pid] = res
json.dump(out, open(sys.argv[3], 'w'), separators=(',', ':'))
tot = sum(len(s['rows']) for v in out.values() for s in v)
print(len(out), 'pages;', 'rows filled in {0} ({1:.0%}), of which shared between subpages {2}'.format(filled, filled / tot, shared_filled[0]))
