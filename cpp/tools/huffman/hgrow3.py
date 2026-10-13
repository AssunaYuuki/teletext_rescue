# Greedy growth of the StarSight Huffman code (symbols may be strings).
# For a codeword w the decoding after w does not depend on what w means, so
# each w is decoded once and every symbol is scored by a trigram model.
import json, pickle, sys, math, collections, random

KJ = r'C:\Users\troxi\Desktop\vbi\eng\KCET\KCET_1994-05-03.json'
k = json.load(open(KJ, encoding='utf-8'))
corpus = [(p.get('text') or '') for p in k['descriptions']] + [(p.get('title') or '') for p in k['programs']]
NUL = '\0'
tri = collections.Counter(); bi = collections.Counter(); uni = collections.Counter()
for s in corpus:
    s = '\x02\x02' + s + NUL
    for i in range(2, len(s)):
        tri[s[i-2:i+1]] += 1; bi[s[i-2:i]] += 1; uni[s[i]] += 1
UT = sum(uni.values())
def lp(ctx, ch):
    a = tri.get(ctx + ch, 0); b = bi.get(ctx, 0)
    u = (uni.get(ch, 0) + 0.05) / (UT + 5)
    return math.log2((a + 2.0 * u) / (b + 2.0))
BONUS = 4.6
MARGIN = 1.5
def lps(ctx, s, L):
    t = 0.0
    for ch in s:
        t += lp(ctx, ch); ctx = (ctx + ch)[-2:]
    return t + L + MARGIN, ctx
def lpseq(ctx, syms):
    t = 0.0
    for sy in syms:
        a, ctx = lps(ctx, sy, len(code[sy])); t += a
    return t, ctx

# candidate symbols: single chars + frequent substrings
sub = collections.Counter()
for s in corpus:
    for L in range(2, 7):
        for i in range(len(s) - L + 1):
            sub[s[i:i+L]] += 1
strings = [w for w, c in sub.most_common(4000) if c >= 40]
strings.sort(key=lambda w: -sub[w] * (len(w) - 1))
strings = strings[:400]

def bits(c): return ''.join(format(x, '08b') for x in c)
code = pickle.load(open(sys.argv[1], 'rb'))
if isinstance(code, tuple): code = code[0][0]
comp, plain = pickle.load(open('titles98.pkl', 'rb'))
D, T = pickle.load(open('extra.pkl', 'rb'))
streams = sorted(set([bits(c) for c in comp.values()] + [bits(c) for c in T.values()] + [bits(c) for c in D.values()]))
MAXL = 16

def decode_from(inv, b, i, maxsym=10**9):
    out = []; n = len(b)
    while i < n and len(out) < maxsym:
        for j in range(i + 1, min(n, i + MAXL) + 1):
            ch = inv.get(b[i:j])
            if ch is not None: break
        else:
            return out, i, None
        if ch == NUL:
            rest = b[j:]
            return out, None, (len(rest) < 8 and '1' not in rest)
        out.append(ch); i = j
    return out, (i if i < n else i), None

def prefix_free(cws, w):
    for v in cws:
        if v.startswith(w) or w.startswith(v): return False
    return True


import multiprocessing as mp
SAMPLE = 60
G = {}
def init(code_, st_):
    G['code'] = code_; G['st'] = st_
    globals()['code'] = code_
def evaluate(chunk):
    code_ = G['code']; st = G['st']
    inv = {v: kk for kk, v in code_.items()}
    used = set(code_)
    syms = [c for c in uni if c != NUL and c not in used] + [w for w in strings if w not in used]
    best = None
    for w, idx_all in chunk:
        idx = idx_all if len(idx_all) <= SAMPLE else random.Random(len(w)).sample(idx_all, SAMPLE)
        scale = len(idx_all) / len(idx)
        conts = []
        for si in idx:
            t0 = st[si][0]
            o, at, ok = decode_from(inv, streams[si], st[si][1] + len(w), 30)
            term = at is None and ok
            fail = -40.0 if (at is None and not ok) else 0.0
            conts.append((('' + t0)[-2:], o, term, fail, {}))
        for sym in syms:
            if len(sym) > 1 and all(ch in code_ for ch in sym) and sum(len(code_[ch]) for ch in sym) <= len(w):
                continue   # a string symbol must be shorter than spelling it out
            sc = 0.0
            for ctx, o, term, fail, memo in conts:
                a, c2 = lps(ctx, sym, len(w))
                h = memo.get(c2)
                if h is None:
                    h, c3 = lpseq(c2, o)
                    if term: h += lp(c3, NUL) + 6 + MARGIN + 6.0
                    memo[c2] = h
                sc += a + h + fail
            sc *= scale
            if best is None or sc > best[0]: best = (sc, sym, w, len(idx_all))
    return best

if __name__ == '__main__':
    out_name = sys.argv[3] if len(sys.argv) > 3 else 'hcode2.pkl'
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    for rnd in range(rounds):
        inv = {v: kk for kk, v in code.items()}
        st = []; full = 0
        for b in streams:
            o, at, ok = decode_from(inv, b, 0)
            st.append((''.join(o), at, ok)); full += at is None and ok
        kr = sum(2.0 ** -len(v) for v in code.values())
        print(f'round {rnd}: {len(code)} symbols, {full}/{len(streams)} full, kraft {kr:.4f}', flush=True)
        spots = collections.defaultdict(list)
        for si, ((t, at, ok), b) in enumerate(zip(st, streams)):
            if at is None: continue
            for L in range(2, MAXL + 1):
                if at + L > len(b): break
                spots[b[at:at + L]].append(si)
        cws = list(code.values())
        work = [(w, idx) for w, idx in spots.items() if len(idx) >= 8 and kr + 2.0 ** -len(w) <= 1.0 + 1e-9 and prefix_free(cws, w)]
        chunks = [work[k::48] for k in range(48)]
        with mp.Pool(12, initializer=init, initargs=(code, st)) as pool:
            res = [r for r in pool.map(evaluate, chunks) if r]
        best = max(res) if res else None
        if best is None or best[0] <= 0: print('no more'); break
        print(f'  add {best[1]!r} = {best[2]} (score {best[0]:.0f}, {best[3]} spots)', flush=True)
        code[best[1]] = best[2]
        pickle.dump(code, open(out_name, 'wb'))
    for ch, cw in sorted(code.items(), key=lambda kv: (len(kv[1]), kv[1])): print(repr(ch), cw)
