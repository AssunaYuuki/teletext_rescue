# Writes src/ss_huffman.inc from a code pickle.
import pickle, sys
code = pickle.load(open(sys.argv[1], 'rb'))
if isinstance(code, tuple): code = code[0][0]
BS = chr(92)
def cstr(s):
    out = ''
    for ch in s:
        if ch == '\0': out += BS + '0'
        elif ch in ('"', BS): out += BS + ch
        else: out += ch
    return '"' + out + '"'
rows = sorted(code.items(), key=lambda kv: (len(kv[1]), kv[1]))
with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
    f.write('// StarSight Huffman code for compressed titles and descriptions.\n')
    f.write('// Recovered from the recordings themselves (WHYY 1998, WVIZ 1996): known\n')
    f.write('// short titles fixed the first symbols, the rest was grown symbol by\n')
    f.write('// symbol where decoding stopped; digits, Q and "," fill the last free\n')
    f.write('// branches (checked on 90210, 3000, 76ers, 1997...). A symbol may be a\n')
    f.write('// whole string ("The ").\n')
    f.write('// {codeword bits, symbol}; the symbol made of one NUL ends the text.\n')
    f.write('static const struct { const char *bits; const char *sym; } kSsHuffman[] = {\n')
    for sym, cw in rows:
        f.write('    {"%s", %s},\n' % (cw, cstr(sym)))
    f.write('};\n')
print(len(rows), 'symbols')
