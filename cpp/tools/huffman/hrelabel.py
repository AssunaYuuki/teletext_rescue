# Relabel pass: keep every codeword, but let each one take the meaning that
# the trigram model likes best over all its occurrences in the decoded texts.
# Usage: hrelabel.py in.pkl out.pkl [rounds]
import sys, pickle, collections
sys.argv = [sys.argv[0], sys.argv[1]] + sys.argv[2:]
import hgrow3 as H   # LM, streams, decode_from, strings, uni

code = pickle.load(open(sys.argv[1], 'rb'))
out = sys.argv[2]
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 4
# symbols fixed by the known-plaintext pairs: never relabelled
FIXED = set(pickle.load(open('hseed.pkl', 'rb')).keys()) | {'C', 'The '}
cands = [c for c in H.uni if c != H.NUL] + H.strings

def tokens(code):
    inv = {v: k for k, v in code.items()}
    T = []
    for b in H.streams:
        o, at, ok = H.decode_from(inv, b, 0)
        T.append((o, at is None and ok))
    return T

def ctx_score(prev, sym, nxt):
    # log2 P(sym chars | 2 chars before) + P(next 2 chars | ...)
    c = ('\x02\x02' + prev)[-2:]
    t = 0.0
    for ch in sym + nxt:
        t += H.lp(c, ch); c = (c + ch)[-2:]
    return t

for rnd in range(rounds):
    T = tokens(code)
    occ = collections.defaultdict(list)   # sym -> (prev text, next text)
    for o, ok in T:
        if not ok: continue
        for i, s in enumerate(o):
            prev = ''.join(o[max(0, i - 3):i])[-2:]
            nxt = ''.join(o[i + 1:i + 4])[:2] if i + 1 < len(o) else '\0'
            occ[s].append((prev, nxt))
    changed = 0
    taken = set(code)
    for s, lst in sorted(occ.items(), key=lambda kv: -len(kv[1])):
        if s in FIXED or len(lst) < 5: continue
        sample = lst[:400]
        base = sum(ctx_score(p, s, n) for p, n in sample)
        best = (base, s)
        for c in cands:
            if c in taken and c != s: continue
            sc = sum(ctx_score(p, c, n) for p, n in sample)
            if sc > best[0]: best = (sc, c)
        if best[1] != s and best[0] - base > 2.0 * len(sample):
            w = code.pop(s)
            code[best[1]] = w
            taken.discard(s); taken.add(best[1])
            print(f'round {rnd}: {s!r} -> {best[1]!r} ({w}, {len(lst)} uses, +{(best[0]-base)/len(sample):.1f} bits/use)', flush=True)
            changed += 1
    if not changed: break
pickle.dump(code, open(out, 'wb'))
print('done', len(code))
