# Карта качества ленты по минутам записи -> quality.json
#   принято   — доля строк VBI с телетекстом, из которых получен пакет;
#   нечитаемо — строки с сигналом, из которых пакет не получен;
#   ошибки    — доля байт с нарушенной чётностью в принятых пакетах.
#   python quality.py [--vbi capture.vbi --t42 поток.t42 --lines line_pkt.npy --lpf 32 --extras extras.json --out quality.json]
import json, os, sys, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
VBI = arg('--vbi'); T42 = arg('--t42'); LINES = arg('--lines')
if not (VBI and T42 and LINES):
    sys.exit('usage: quality.py --vbi recording.vbi --t42 stream.t42 --lines line_pkt.npy [--lpf 32] [--extras extras.json] [--out quality.json]')
LPF = int(arg('--lpf', 32)); SPL = int(arg('--spl', 2048))
EXTRAS = arg('--extras', os.path.join(os.path.dirname(T42), 'extras.json'))
OUT = arg('--out', os.path.join(os.path.dirname(T42), 'quality.json'))

v = np.memmap(VBI, dtype=np.uint8, mode='r')
v = v[:len(v) // (LPF * SPL) * LPF * SPL].reshape(-1, LPF, SPL)
st = np.frombuffer(open(T42, 'rb').read(), dtype=np.uint8).reshape(-1, 42)
lp = np.load(LINES)
FPM = 25 * 60                                                         # кадров в минуте
ex = json.load(open(EXTRAS, encoding='utf-8')) if os.path.exists(EXTRAS) else {}
clk = ex.get('_meta', {}).get('clock')
bad = (np.unpackbits(st[:, 2:, None], axis=2).sum(2) % 2 == 0)        # (пакет, байт): ошибка чётности
# строки, где вообще бывает телетекст (сигнал хотя бы в 2% кадров)
smp = np.asarray(v[::max(1, len(v) // 300)], dtype=np.float32).std(2)
DATA = [l for l in range(LPF) if (smp[:, l] > 20).mean() > 0.02] or list(range(LPF))

def hms(s): s = int(round(s)); return f'{s // 3600:02}:{s // 60 % 60:02}:{s % 60:02}'
rows = []
for m0 in range(0, len(v), FPM):
    m1 = min(len(v), m0 + FPM)
    sig = np.asarray(v[m0:m1], dtype=np.float32)[:, DATA].std(2) > 20
    got = lp[m0:m1][:, DATA] >= 0
    k = lp[m0:m1][lp[m0:m1] >= 0]
    exp = sig | got                                                   # строки, где телетекст был
    r = {'minute': m0 // FPM, 'from_s': round(m0 / 25, 1), 'to_s': round(m1 / 25, 1),
         'received': round(100 * float(got.sum() / max(1, exp.sum())), 2),
         'unreadable': round(100 * float((sig & ~got).sum() / max(1, exp.sum())), 2),
         'parity': round(100 * float(bad[k].mean()), 3) if len(k) else None}
    if clk and len(k):                    # часы подогнаны по номеру пакета / 650 (см. extract_extras)
        r['air'] = f"{hms(clk[0] + clk[1] * k.min() / 650)}–{hms(clk[0] + clk[1] * k.max() / 650)}"
    rows.append(r)
json.dump({'stream': os.path.basename(T42), 'minutes': rows}, open(OUT, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
for r in rows:
    print('min {0:2} ({1}): received {2:5.1f}%  unreadable {3:4.1f}%  errors {4:.3f}%'.format(r['minute'], r.get('air', ''), r['received'], r['unreadable'], r['parity'] or 0))
