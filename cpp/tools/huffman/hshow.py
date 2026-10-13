import pickle, sys, random
code = pickle.load(open(sys.argv[1], 'rb'))
if isinstance(code, tuple): code = code[0][0]
inv = {v: k for k, v in code.items()}
def dec(b):
    out = ''; i = 0
    while i < len(b):
        for j in range(i + 1, min(len(b), i + 18) + 1):
            if b[i:j] in inv: break
        else: return out + '{' + b[i:i+16] + '}', False
        ch = inv[b[i:j]]
        if ch == '\0': return out, '1' not in b[j:] and len(b) - j < 8
        out += ch; i = j
    return out + '{}', False
comp, plain = pickle.load(open('titles98.pkl', 'rb'))
D, T = pickle.load(open('extra.pkl', 'rb'))
which = sys.argv[2] if len(sys.argv) > 2 else 't'
src = list(comp.values()) if which == 't' else list(D.values())
random.seed(1)
for c in random.sample(src, int(sys.argv[3]) if len(sys.argv) > 3 else 40):
    t, ok = dec(''.join(format(x, '08b') for x in c))
    print('OK ' if ok else '.. ', t)
