// Отрисовка страницы телетекста уровня 1 на canvas — тот же разбор атрибутов, что
// pages/tt_level1.py (ETSI EN 300 706 §12.2 табл. 26, §15.3 ESC).
// rows: {номер ряда: [40 кодов]}; opts: {reveal, cursor:{r,c},
//   over: {'r,c': символ} — поправки X/26 (уже подходящие к клеткам),
//   cs: 96 символов основного набора для кодов 0x20..0x7F (charsets.py; без него —
//       латиница с немецким вариантом), cs2: второй набор для ESC (или нет),
//   flashOn: фаза мигания (true — мигающие символы видны),
//   tv: как на экране ТВ — видно только содержимое рамок (новости, субтитры)}.
// Возвращает ссылки на страницы [{r,c,p}] со свойствами .flash и .box —
// есть ли на странице мигание и рамки.
const TT = (() => {
  const COL = ['#000', '#f00', '#0f0', '#ff0', '#00f', '#f0f', '#0ff', '#fff'];
  const DE = {0x23:'#',0x24:'$',0x40:'§',0x5b:'Ä',0x5c:'Ö',0x5d:'Ü',0x5e:'^',0x5f:'_',0x60:'°',0x7b:'ä',0x7c:'ö',0x7d:'ü',0x7e:'ß',0x7f:'■'};
  const DEF = Array.from({length: 96}, (_, i) => DE[i + 0x20] || String.fromCharCode(i + 0x20)).join('');
  const CW = 12, CH = 20, TVGREY = '#3c3c3c';
  let CS = null;                                       // набор последней отрисованной страницы

  function cells(rows, cs, cs2, over) {
    const out = []; let hasFlash = false, hasBox = false, skipRow = false;
    for (let r = 0; r < 25; r++) {
      if (skipRow) { skipRow = false; continue; }
      const b = rows[r]; if (!b) continue;
      let fg = 7, bg = 0, mos = false, sep = false, flash = false, conceal = false, hold = false, box = false;
      let held = 0x20, heldSep = false, w = 1, h = 1, set = cs, prev = -1, skipCol = false, rowDH = false;
      const row = [];
      for (let c = 0; c < 40; c++) {
        const ch = b[c] & 0x7f;
        if (ch < 0x20) {
          // set-at
          if (ch == 0x09) flash = false;
          else if (ch == 0x0c) { if (w != 1 || h != 1) held = 0x20; w = 1; h = 1; }
          else if (ch == 0x18) conceal = true; else if (ch == 0x19) sep = false; else if (ch == 0x1a) sep = true;
          else if (ch == 0x1c) bg = 0; else if (ch == 0x1d) bg = fg; else if (ch == 0x1e) hold = true;
          if (!skipCol) {
            const cell = {r, c, text: ' ', mosaic: 0, sep: false, fg, bg, flash, conceal, w: 1, h, bh: 1, box};
            if (hold && mos && held != 0x20) { cell.mosaic = held; cell.sep = heldSep; cell.text = ''; }
            row.push(cell);
          }
          skipCol = false;
          // set-after
          if (ch <= 7) { fg = ch; mos = false; conceal = false; held = 0x20; }
          else if (ch == 0x08) { flash = true; hasFlash = true; }
          else if (ch == 0x0a && prev == 0x0a) box = false;
          else if (ch == 0x0b && prev == 0x0b) { box = true; hasBox = true; }
          else if (ch >= 0x0d && ch <= 0x0f) {
            let nw = ch == 0x0d ? 1 : 2, nh = ch == 0x0e ? 1 : 2;
            if (r >= 23) nh = 1;
            if (nw != w || nh != h) held = 0x20;
            w = nw; h = nh; if (h == 2) rowDH = true;
          }
          else if (ch >= 0x10 && ch <= 0x17) { fg = ch - 0x10; mos = true; conceal = false; }
          else if (ch == 0x1b && cs2) set = set === cs ? cs2 : cs;
          else if (ch == 0x1f) hold = false;
          prev = ch; continue;
        }
        prev = ch;
        if (skipCol) { skipCol = false; continue; }
        const cell = {r, c, text: '', mosaic: 0, sep, fg, bg, flash, conceal, w, h, bh: 1, box};
        const ov = over && over[r + ',' + c];
        if (mos && (ch & 0x20) && !ov) { cell.mosaic = ch; held = ch; heldSep = sep; }
        else cell.text = ov || set[ch - 0x20];
        row.push(cell);
        if (w == 2) skipCol = true;
      }
      if (rowDH) { for (const cell of row) cell.bh = 2; skipRow = true; }
      out.push(...row);
    }
    return {cells: out, hasFlash, hasBox};
  }

  function render(g, rows, opts = {}) {
    const W = 40 * CW, H = 25 * CH;
    CS = opts.cs || DEF;
    g.fillStyle = opts.tv ? TVGREY : '#000'; g.fillRect(0, 0, W, H);
    const {cells: cs, hasFlash, hasBox} = cells(rows, CS, opts.cs2 || null, opts.over);
    const flashOn = opts.flashOn !== false;
    for (const cell of cs) {
      if (opts.tv && !cell.box) continue;
      const x = cell.c * CW, y = cell.r * CH, w = cell.w * CW, h = cell.h * CH;
      g.fillStyle = COL[cell.bg]; g.fillRect(x, y, Math.max(w, CW), Math.max(h, cell.bh * CH));
      if ((cell.conceal && !opts.reveal) || (cell.flash && !flashOn)) continue;
      g.fillStyle = COL[cell.fg];
      if (cell.mosaic) {
        const ch = cell.mosaic, bits = [ch & 1, ch & 2, ch & 4, ch & 8, ch & 16, ch & 64], cw = w / 2, chh = h / 3;
        for (let i = 0; i < 6; i++) if (bits[i]) {
          const cx = x + (i % 2) * cw, cy = y + Math.floor(i / 2) * chh;
          if (cell.sep) g.fillRect(cx + 1, cy + 1, cw - 2, chh - 2); else g.fillRect(cx, cy, cw, Math.ceil(chh));
        }
      } else if (cell.text && cell.text != ' ') {
        g.save(); g.translate(x, y); g.scale(cell.w, cell.h);
        g.font = 'bold 17px "Courier New",monospace'; g.textBaseline = 'top';
        g.fillText(cell.text, 0, 1); g.restore();
      }
    }
    // номера страниц в тексте — ссылки
    const links = [];
    for (let r = 1; r < 25; r++) {
      const b = rows[r]; if (!b) continue;
      const s = Array.from(b, x => (x & 0x7f) >= 0x20 ? String.fromCharCode(x & 0x7f) : ' ').join('');
      for (const m of s.matchAll(/(?<!\d)([1-8]\d\d)(?!\d)/g)) links.push({r, c: m.index, p: m[1]});
    }
    if (opts.cursor) {
      const {r, c} = opts.cursor;
      g.strokeStyle = '#f80'; g.lineWidth = 2; g.strokeRect(c * CW + 1, r * CH + 1, CW - 2, CH - 2);
    }
    links.flash = hasFlash; links.box = hasBox;
    return links;
  }

  // мигание 0,75 Гц: 3/4 видно, 1/4 скрыто (как в decode-orc и приёмниках WST).
  // draw(flashOn) рисует страницу и возвращает результат render; animate(draw) — функция
  // «перерисовать с начала», сама запускает таймер, если на странице есть мигание.
  function animate(draw) {
    let on = true, timer = null;
    const tick = () => { on = !on; const l = draw(on); timer = l && l.flash ? setTimeout(tick, on ? 1000 : 333) : null; };
    return () => { clearTimeout(timer); on = true; const l = draw(true); timer = l && l.flash ? setTimeout(tick, 1000) : null; };
  }

  // символ с клавиатуры -> код телетекста (в наборе последней отрисованной страницы)
  function charCode(k) {
    const set = CS || DEF; const i = set.indexOf(k);
    return k.length == 1 && i >= 0 && k != '■' ? i + 0x20 : null;
  }

  // цветные кнопки FLOF: красная, зелёная, жёлтая, голубая
  const KEYS = [['#d22', 'красная'], ['#2a2', 'зелёная'], ['#cc2', 'жёлтая'], ['#2cc', 'голубая']];
  return {render, animate, charCode, CW, CH, COL, KEYS};
})();
