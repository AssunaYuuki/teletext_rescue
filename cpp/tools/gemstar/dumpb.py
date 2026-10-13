import sys, os
sys.argv = ['x']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gemtypeb.py')).read().replace('\nmain()\n', '\n'))
import os
words, exe, y, m = os.environ['W'], os.environ['G'], int(os.environ['Y']), int(os.environ['M'])
for st, fr, tag, msg, h, end in messages(words, exe, y, m):
    print(st, fr, 'tag', tag, h.hex(), len(msg), end)
    for i in range(0, len(msg), 32):
        c = msg[i:i+32]
        print('   %04x %-64s %s' % (i, c.hex(), ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))
