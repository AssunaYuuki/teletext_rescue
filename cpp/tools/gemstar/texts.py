import sys, os, re, collections
sys.argv = ['x']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gemtypeb.py')).read().replace('\nmain()\n', '\n'))
words, exe, y, m = os.environ['W'], os.environ['G'], int(os.environ['Y']), int(os.environ['M'])
types = collections.Counter(); seen = set()
for st, fr, tag, msg, h, end in messages(words, exe, y, m):
    types[(msg[0], msg[1])] += 1
    for t in re.findall(rb'[ -~]{6,}', msg):
        if t not in seen: seen.add(t); print(hex(msg[0]), t.decode())
print(types)
