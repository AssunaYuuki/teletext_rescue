import sys, os
sys.argv = ['x']
here = os.path.dirname(os.path.abspath(__file__))
exec(open(os.path.join(here, 'gemtypeb.py')).read().replace('\nmain()\n', '\n'))
sys.path.insert(0, here); from gemtext import Tables, Decoder
import pluscode
words, exe, y, m, dd = os.environ['W'], os.environ['G'], int(os.environ['Y']), int(os.environ['M']), os.environ['D']
dec = Decoder(Tables(exe, dd))
def vlen(msg, p):
    n = 0
    while True:
        b = msg[p]; p += 1; n += b
        if b != 0xff: return n, p
done = set()
for st, fr, tag, msg, h, end in messages(words, exe, y, m):
    if msg[0] != 0 or msg in done: continue
    done.add(msg)
    total = msg[1] | msg[2] << 8
    print('SIP tag', tag, 'len', total, 'hdr', msg[3:10].hex(), 'plus', msg[4:8].hex())
    code = bcd4 = int(msg[4:8].hex())
    yy, mm = y, m
    ch, date, hhmm, ln = pluscode.decode(yy, mm, code)
    if date % 1000000 < (y % 100) * 10000 + m * 100 + 31:   # earlier than the recording day: next month, as 252f:0367 does
        mm = m % 12 + 1; yy = y + (m == 12)
        ch, date, hhmm, ln = pluscode.decode(yy, mm, code)
    base = hhmm // 100 * 60 + hhmm % 100
    print('   header code', code, '-> ch', ch, 'date', date, 'time', hhmm, 'len', ln)
    p = 10
    try:
        while p < total:
            hdr = msg[p:p + 4]; p += 4
            strs = []
            for k in range(4):
                n, p = vlen(msg, p)
                strs.append(msg[p:p + n]); p += n
            bcd = lambda b: int(''.join('%x' % x for x in b) or '0') if all((x >> 4) < 10 and (x & 15) < 10 for x in b) else -1
            pc = bcd(strs[3])
            st = (hdr[0] & 0x3f) * 5 + base; du = (hdr[1] & 0x7f) * 5
            own = pluscode.decode(yy, mm, pc) if pc > 0 else None
            print('  %s ch%d %06d %02d:%02d +%dmin theme %02x misc %02x | own code %d -> %s | %s' % (hdr.hex(), ch, date, st // 60 % 24, st % 60, du, hdr[3], hdr[2], pc, own, ' / '.join(dec.decode(x).decode('latin1').replace('', ' | ') for x in strs[:3])))
            if msg[p] != 0: print('  !end', msg[p:p+4].hex()); break
            if not hdr[0] & 0x80: break
            p += 1
    except Exception as e:
        print('  err', e)
