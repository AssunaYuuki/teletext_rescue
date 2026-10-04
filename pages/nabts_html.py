"""Страницы NABTS/NAPLPS в HTML с показом «как на экране приёмника».

Для каждой страницы nabts.Player прогоняется заранее; запоминается, какие клетки экрана
в какой момент получили какие «чернила» (по кадрам 1/50 с), когда стирался экран,
менялись карта цветов и процессы мигания. В браузере nabts_player.js проигрывает это:
рисование во времени, перекраска при смене карты цветов, мигание.

  python pages/nabts_html.py поток.t33 [папка]
Каждая страница — один самостоятельный файл <запись>.html (только экран с анимацией).
Папка по умолчанию — «<поток>_nabts_html» рядом с потоком.
"""
import html, json, os, sys

import nabts as N

FPS = 50
PLAYER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nabts_player.js')


class _LogCells(list):
    """Клетки экрана, которые помнят записи с прошлого снимка (Rasteriser пишет и через
    Surface.put, и прямо в cells)."""
    def __setitem__(self, i, v):
        list.__setitem__(self, i, v); self.log[i] = v


class _LogSurface(N.Surface):
    def __init__(self, w, h):
        super().__init__(w, h)
        self.cells = _LogCells(self.cells); self.cells.log = {}
        self.log = self.cells.log

    def put(self, c, r, ink):
        if 0 <= c < self.w and 0 <= r < self.h:
            self.cells[r * self.w + c] = ink


class _RecPlayer(N.Player):
    def _new_surface(self):
        self.surf = _LogSurface(self.gw, self.gh)
        self.ras = N.Rasteriser(self.surf, self.gw, self.gh)


def _rgb(c):
    return list(N._rgb(c))


def _runs(log):
    """{клетка: чернила} -> [начало, длина, чернила, ...] по подряд идущим клеткам."""
    out = []
    for i in sorted(log):
        k = log[i]
        if out and out[-3] + out[-2] == i and out[-1] == k:
            out[-2] += 1
        else:
            out += [i, 1, k]
    return out


def _blink(b):
    out = []
    for e in b:
        if not e:
            out.append(None); continue
        to, to_col, on, off, delay, start = e
        out.append([to, _rgb(to_col) if to < 0 else None, on, off, delay, round(start, 3)])
    return out


def page_data(page, grid=(256, 200)):
    """Показ страницы -> словарь для nabts_player.js."""
    pl = _RecPlayer(page, grid)
    steps = []
    times = sorted({-(-int(round(e[1] * FPS * 1000)) // 1000) for e in pl.events})   # кадр, где событие видно
    for q in times:
        surf, mp, bl = pl.surf, pl.map, pl.blink
        pl.advance(q / FPS + 1e-9)
        st = [round(q / FPS, 3), 1 if pl.surf is not surf else 0,
              [_rgb(c) for c in pl.map] if pl.map is not mp else None,
              _blink(pl.blink) if pl.blink is not bl else None,
              _runs(pl.surf.log)]
        pl.surf.log.clear()
        steps.append(st)
    if not pl.done():                                    # на всякий случай — остаток сразу
        surf = pl.surf; pl.advance(None)
        steps.append([round(pl.end, 3), 1 if pl.surf is not surf else 0, [_rgb(c) for c in pl.map],
                      _blink(pl.blink), _runs(pl.surf.log)])
    inks = []
    for s in pl.inks:
        if s[0] == 'c':
            inks.append(['c', _rgb(s[1])])
        elif s[0] == 'm':
            inks.append(['m', s[1] % 16])
        else:
            inks.append(['d', _rgb(s[1]), s[2] % 16])
    return dict(w=pl.gw, h=pl.gh, end=round(pl.end, 3), map=[_rgb(c) for c in N.DEFAULT_MAP],
                inks=inks, steps=steps)


def _name(r):
    return '%03X-%s-v%d' % (r.channel, r.addr_text, r.version)


PAGE = '''<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
<style>html,body{{margin:0;height:100%;background:#000}}
body{{display:flex;align-items:center;justify-content:center}}
canvas{{width:min(100vw,133.333vh);aspect-ratio:4/3;image-rendering:pixelated;display:block;cursor:pointer}}</style>
</head><body><canvas id="screen" title="Click to replay"></canvas>
<script>
{js}
NabtsPlayer.start({data}, document.getElementById('screen'), document.getElementById('screen'), null);
</script></body></html>
'''


def page_html(r, grid=(256, 200)):
    """Страница -> один самостоятельный HTML: только экран, рисуется как на приёмнике
    (щелчок — показать заново)."""
    with open(PLAYER, encoding='utf-8') as f:
        js = f.read()
    return PAGE.format(title=html.escape(N.record_label(r)), js=js,
                       data=json.dumps(page_data(r.page, grid), separators=(',', ':')))


def save_page(r, path, grid=(256, 200)):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(page_html(r, grid))


def export(recs, out, grid=(256, 200), progress=None):
    """Каждая страница recs (после nabts.interpret) -> out/<запись>.html. -> число страниц."""
    os.makedirs(out, exist_ok=True)
    pages = [r for r in recs if r.page]
    for k, r in enumerate(pages):
        save_page(r, os.path.join(out, _name(r) + '.html'), grid)
        if progress:
            progress(k + 1, len(pages))
    return len(pages)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '_nabts_html'
    recs, summ = N.read_t33(src)
    N.interpret(recs)
    n = export(recs, out, progress=lambda i, k: print('PROGRESS %d %d' % (i, k), flush=True))
    print('Done: %d pages in %s' % (n, out))


if __name__ == '__main__':
    main()
