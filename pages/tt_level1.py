# Разбор страницы телетекста уровня 1 в клетки экрана — ETSI EN 300 706 §12.2
# (таблица 26, атрибуты «set-at» и «set-after») и §15.3 (ESC — второй набор G0).
#   cells(rows, t, t2=None, over=None) -> [Cell], has_flash, has_box
# rows: {'0'..'24': [40 кодов]}; t — основной набор (96 символов, charsets.table),
# t2 — второй набор для ESC (None — ESC ничего не делает, как в decode-orc по умолчанию);
# over — поправки X/26 {'r,c': символ} (уже отобранные tt_export.over_fits).
#
# Атрибуты:
#   set-at (действуют на свою же клетку): 09 steady, 0C normal size, 18 conceal,
#     19 contiguous, 1A separated, 1C black background, 1D new background, 1E hold mosaics;
#   set-after (с следующей клетки): 00–07 цвет текста, 08 flash, 0A end box, 0B start box,
#     0D double height, 0E double width, 0F double size, 10–17 цвет мозаики, 1B ESC,
#     1F release mosaics.
#   Коды цвета снимают conceal; смена размера сбрасывает удерживаемую мозаику.
#   Рамка (box) начинается двумя кодами 0B подряд и кончается двумя 0A подряд.
#   Двойная высота: ряд r+1 не показывается, фон клеток ряда r тянется на два ряда.
#   Двойная ширина: символ занимает 2 клетки, следующая клетка не рисуется.
from dataclasses import dataclass

@dataclass
class Cell:
    r: int; c: int
    text: str = ' '          # символ (для мозаики — пусто)
    mosaic: int = 0          # код мозаики (0x20..0x7f) или 0
    sep: bool = False
    fg: int = 7; bg: int = 0
    flash: bool = False; conceal: bool = False
    w: int = 1; h: int = 1   # размер символа в клетках
    bh: int = 1              # высота фона (2 во всём ряду с двойной высотой)
    box: bool = False

def cells(rows, t, t2=None, over=None):
    out = []; has_flash = has_box = False
    skip_row = False
    for r in range(25):
        if skip_row: skip_row = False; continue
        b = rows.get(str(r)) or rows.get(r)
        if not b: continue
        fg, bg, mos, sep = 7, 0, False, False
        flash = conceal = hold = box = False
        held, held_sep = 0x20, False
        w, h = 1, 1; cs = t; prev = None; skip_col = False; row_dh = False
        row_cells = []
        for c in range(40):
            ch = b[c] & 0x7f
            if ch < 0x20:
                # --- set-at
                if ch == 0x09: flash = False
                elif ch == 0x0C:
                    if (w, h) != (1, 1): held = 0x20
                    w, h = 1, 1
                elif ch == 0x18: conceal = True
                elif ch == 0x19: sep = False
                elif ch == 0x1A: sep = True
                elif ch == 0x1C: bg = 0
                elif ch == 0x1D: bg = fg
                elif ch == 0x1E: hold = True
                if not skip_col:
                    cell = Cell(r, c, fg=fg, bg=bg, flash=flash, conceal=conceal, w=1, h=h, box=box)
                    if hold and mos and held != 0x20: cell.mosaic, cell.sep, cell.text = held, held_sep, ''
                    row_cells.append(cell)
                skip_col = False
                # --- set-after
                if ch <= 0x07: fg = ch; mos = False; conceal = False; held = 0x20
                elif ch == 0x08: flash = True; has_flash = True
                elif ch == 0x0A and prev == 0x0A: box = False
                elif ch == 0x0B and prev == 0x0B: box = True; has_box = True
                elif ch in (0x0D, 0x0E, 0x0F):
                    nw, nh = {0x0D: (1, 2), 0x0E: (2, 1), 0x0F: (2, 2)}[ch]
                    if r >= 23 and nh == 2: nh = 1             # в рядах 23–24 двойной высоты нет
                    if (nw, nh) != (w, h): held = 0x20
                    w, h = nw, nh
                    if h == 2: row_dh = True
                elif 0x10 <= ch <= 0x17: fg = ch - 0x10; mos = True; conceal = False
                elif ch == 0x1B and t2: cs = t2 if cs is t else t
                elif ch == 0x1F: hold = False
                prev = ch
                continue
            prev = ch
            if skip_col: skip_col = False; continue
            cell = Cell(r, c, fg=fg, bg=bg, flash=flash, conceal=conceal, w=w, h=h, box=box, sep=sep)
            ov = (over or {}).get(f'{r},{c}')
            if mos and (ch & 0x20) and not ov:
                cell.mosaic, cell.text = ch, ''; held, held_sep = ch, sep
            else:
                cell.text = ov or cs[ch - 0x20]
            row_cells.append(cell)
            if w == 2: skip_col = True
        if row_dh:                                       # фон всех клеток ряда — на два ряда
            for cell in row_cells: cell.bh = 2
            skip_row = True
        out += row_cells
    return out, has_flash, has_box

def row_texts(rows, t, t2=None, over=None):
    """Текст ряда (как видит зритель: без мозаики и управляющих кодов) — {ряд: строка}."""
    cs, _, _ = cells(rows, t, t2, over)
    grid = {}
    for cell in cs:
        line = grid.setdefault(cell.r, [' '] * 40)
        if not cell.mosaic: line[cell.c] = cell.text or ' '
    return {r: ''.join(v).rstrip() for r, v in grid.items()}
