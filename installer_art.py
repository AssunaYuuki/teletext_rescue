"""Картинки установщика в стиле программы: тёмный фон, янтарное светодиодное табло, значок «TR».

  python installer_art.py
Пишет installer/wizard_*.png (большая картинка слева, 164×314 на 100 %) и installer/small_*.png
(значок вверху страниц, 55×55) в масштабах 100/150/200 % — их подключает setup.iss.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'pages'))
import silent_radio as SR

OUT = os.path.join(HERE, 'installer')
BG, PANEL, AMBER, DIM, MUTED = (27, 28, 31), (10, 7, 3), (255, 178, 30), (52, 33, 10), (160, 163, 168)


def font(size, bold=False):
    for f in (('segoeuib.ttf' if bold else 'segoeui.ttf'), 'arial.ttf'):
        try: return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:/Windows'), 'Fonts', f), size)
        except OSError: pass
    return ImageFont.load_default()


def led_text(d, text, x0, y0, dot, rows=7):
    cols = SR.text_cols(text)
    for x, c in enumerate(cols):
        for y in range(rows):
            on = (c >> y) & 1
            cx, cy = x0 + x * dot, y0 + y * dot
            d.ellipse([cx + dot * 0.12, cy + dot * 0.12, cx + dot * 0.88, cy + dot * 0.88], fill=AMBER if on else DIM)
    return len(cols) * dot


def icon(size):
    im = Image.new('RGBA', (size, size), BG + (255,)); d = ImageDraw.Draw(im)
    m = max(1, size // 18)
    d.rounded_rectangle([m, m, size - m - 1, size - m - 1], radius=size // 6, fill=PANEL + (255,), outline=AMBER, width=max(1, size // 22))
    cols = SR.text_cols('TR'); dot = (size - 6 * m) / max(len(cols), 9)
    x0 = (size - len(cols) * dot) / 2; y0 = (size - 7 * dot) / 2
    led_text(d, 'TR', x0, y0, dot)
    return im


def wizard(scale):
    W, H = int(164 * scale), int(314 * scale)
    im = Image.new('RGB', (W, H), BG); d = ImageDraw.Draw(im)
    for y in range(H):                                   # лёгкий градиент сверху вниз
        k = y / H
        d.line([(0, y), (W, y)], fill=tuple(int(BG[i] * (1 - 0.45 * k)) for i in range(3)))
    ic = icon(int(64 * scale)).convert('RGB')
    im.paste(ic, ((W - ic.width) // 2, int(34 * scale)))
    # табло с названием
    dot = 2.6 * scale
    w1 = len(SR.text_cols('TELETEXT')) * dot; w2 = len(SR.text_cols('RESCUE')) * dot
    pw = max(w1, w2) + 14 * scale; ph = 7 * dot * 2 + 3 * dot + 14 * scale
    px, py = (W - pw) / 2, 128 * scale
    d.rounded_rectangle([px, py, px + pw, py + ph], radius=4 * scale, fill=PANEL, outline=(59, 59, 59), width=max(1, int(scale)))
    led_text(d, 'TELETEXT', (W - w1) / 2, py + 7 * scale, dot)
    led_text(d, 'RESCUE', (W - w2) / 2, py + 7 * scale + 10 * dot, dot)
    f = font(int(10 * scale)); fb = font(int(10 * scale), True)
    y = py + ph + 18 * scale
    for t in ('VBI  ·  NABTS', 'SILENT RADIO  ·  CAPTIONS'):
        tw = d.textlength(t, font=f); d.text(((W - tw) / 2, y), t, fill=MUTED, font=f); y += 15 * scale
    t = 'AssunaYuuki'; tw = d.textlength(t, font=fb)
    d.text(((W - tw) / 2, H - 30 * scale), t, fill=AMBER, font=fb)
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    for pct, sc in ((100, 1.0), (150, 1.5), (200, 2.0)):
        wizard(sc).save(os.path.join(OUT, 'wizard_%d.png' % pct))
        icon(int(55 * sc)).save(os.path.join(OUT, 'small_%d.png' % pct))
    print('written to', OUT)


if __name__ == '__main__':
    main()
