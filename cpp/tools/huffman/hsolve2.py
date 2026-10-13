# Known-plaintext search for the StarSight Huffman code, with code lengths
# limited by English character frequencies, seeded by "News" (3 IDs with the
# same compressed bits and the same 1994 title).
import json, pickle, sys, math, collections
from hsolve import walk, kraft, prefix_free, bits_of, NUL

k = json.load(open(sys.argv[1], encoding='utf-8'))
kt = {p['id']: p.get('title') for p in k['programs'] if p.get('title')}
freq = collections.Counter()
for p in k['descriptions']: freq.update((p.get('text') or '') + NUL)
for p in k['programs']: freq.update((p.get('title') or '') + NUL)
tot = sum(freq.values())
SLACK = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
def window(ch):
    if ch == NUL: return (6, 6)
    if ch not in freq: return (9, 18)
    e = -math.log2(freq[ch] / tot)
    return (max(2, int(e - SLACK)), int(e + SLACK + 1.5))

comp, plain = pickle.load(open(sys.argv[2], 'rb'))
P = []
for i, c in comp.items():
    if i in kt:
        t = kt[i] + NUL
        if all(ch in freq or ch == NUL for ch in t):
            P.append((t, bits_of(c), i))

def solutions(code, pairs, limit):
    out = []
    def rec(code):
        if len(out) >= limit: return
        need = None
        for t, b in pairs:
            r = walk(code, t, b)
            if r[0] == 'bad': return
            if r[0] == 'need' and need is None: need = (t, b, r[1], r[2])
        if need is None:
            out.append(dict(code)); return
        t, b, ci, bi = need
        ch = t[ci]
        lo, hi = window(ch)
        kk = kraft(code)
        for L in range(lo, hi + 1):
            if bi + L > len(b): break
            cw = b[bi:bi + L]
            if kk + 2.0 ** -L > 1.0 + 1e-9: continue
            if not prefix_free(code, cw): continue
            code[ch] = cw
            rec(code)
            del code[ch]
            if len(out) >= limit: return
    rec(dict(code))
    return out

if __name__ == '__main__':
    seed = [p for p in P if p[0] == 'News' + NUL][:1]
    beam = [{NUL: '100111'}]
    acc = []
    for t, b, i in seed:
        beam = [s for c in beam for s in solutions(c, [(t, b)], 100000)]
        acc.append((t, b))
    print('seed beam', len(beam), flush=True)
    pending = [p for p in P if p[0] != 'News' + NUL]
    BEAM = int(sys.argv[4]) if len(sys.argv) > 4 else 20000
    while pending:
        common = set.intersection(*[set(c) for c in beam])
        pending.sort(key=lambda p: (len(set(p[0]) - common), len(p[0])))
        t, b, i = pending.pop(0)
        nb = []
        for c in beam:
            nb += solutions(c, [(t, b)], BEAM)
            if len(nb) > BEAM: break
        if not nb:
            continue
        if len(nb) > BEAM:
            if len(set(t) - common) <= 1: continue
            pending.append((t, b, i));
            if all(len(set(p[0]) - common) > 1 for p in pending): break
            continue
        beam = nb
        acc.append((t, b))
        print(f'accept {i} {t[:-1]!r} beam {len(beam)} known {len(common)}', flush=True)
    code = beam[0]
    print('accepted', len(acc), 'beam', len(beam))
    for ch, cw in sorted(code.items(), key=lambda kv: (len(kv[1]), kv[1])):
        agree = sum(c.get(ch) == cw for c in beam)
        print(repr(ch), cw, agree)
    pickle.dump((beam, acc), open('hbeam.pkl', 'wb'))
