# Robust known-plaintext search for the StarSight title Huffman code.
# pairs come from program IDs shared between WHYY 1998 (compressed) and
# KCET 1994 (plain); many pairs are wrong (IDs reused), so pairs are added
# one at a time and kept only when some code is consistent with them.
import json, pickle, sys, random

NUL = '\0'
MAXL = 15

def bits_of(c):
    return ''.join(format(b, '08b') for b in c)

def walk(code, t, b):
    """Advance through text t over bits b with known codes.
    Returns ('ok',) if fully consistent, ('bad',) on mismatch,
    ('need', ci, bi) at the first unknown char."""
    bi = 0
    for ci, ch in enumerate(t):
        cw = code.get(ch)
        if cw is None:
            return ('need', ci, bi)
        if not b.startswith(cw, bi):
            return ('bad',)
        bi += len(cw)
    rest = b[bi:]
    if len(rest) >= 8 or '1' in rest:
        return ('bad',)
    return ('ok',)

def kraft(code):
    return sum(2.0 ** -len(c) for c in code.values())

def prefix_free(code, new):
    for c in code.values():
        if c.startswith(new) or new.startswith(c):
            return False
    return True

def solutions(code, pairs, limit):
    """All extensions of code consistent with all pairs (up to limit)."""
    out = []
    def rec(code):
        if len(out) >= limit:
            return
        # pick the pair whose next unknown has fewest options: just first need
        need = None
        for t, b in pairs:
            r = walk(code, t, b)
            if r[0] == 'bad':
                return
            if r[0] == 'need' and need is None:
                need = (t, b, r[1], r[2])
        if need is None:
            out.append(dict(code))
            return
        t, b, ci, bi = need
        ch = t[ci]
        k = kraft(code)
        for L in range(1, MAXL + 1):
            if bi + L > len(b):
                break
            cw = b[bi:bi + L]
            if k + 2.0 ** -L > 1.0 + 1e-9:
                continue
            if not prefix_free(code, cw):
                continue
            code[ch] = cw
            rec(code)
            del code[ch]
            if len(out) >= limit:
                return
    rec(dict(code))
    return out

def load_pairs():
    k = json.load(open(sys.argv[1], encoding='utf-8'))
    kt = {p['id']: p.get('title') for p in k['programs'] if p.get('title')}
    comp, plain = pickle.load(open(sys.argv[2], 'rb'))
    P = []
    for i, c in comp.items():
        if i in kt:
            t = kt[i]
            r = len(c) * 8 / (len(t) + 1)
            if 3.3 < r < 6.5:
                P.append((t + NUL, bits_of(c), i))
    return P

if __name__ == '__main__':
    P = load_pairs()
    print(len(P), 'pairs')
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    random.seed(seed)
    base = {NUL: '100111'}
    accepted = []
    beam = [base]
    pending = sorted(P, key=lambda p: len(p[0]))
    for rnd in range(6):
        left = []
        for t, b, i in pending:
            # how many unknown characters would this pair add?
            unk = len(set(t) - set(beam[0].keys()))
            if unk > 2 + rnd:
                left.append((t, b, i)); continue
            nb = []
            for code in beam:
                nb += solutions(code, [(t, b)], 400)
                if len(nb) > 3000:
                    break
            if not nb:
                continue          # rejected
            if len(nb) > 3000:
                left.append((t, b, i)); continue
            # keep only codes consistent with all accepted pairs (already so)
            beam = nb
            accepted.append((t, i))
            print(f'accept {i} {t!r} beam {len(beam)} known {len(beam[0])}', flush=True)
        pending = left
    code = beam[0]
    for ch, cw in sorted(code.items(), key=lambda kv: (len(kv[1]), kv[1])):
        print(repr(ch), cw)
