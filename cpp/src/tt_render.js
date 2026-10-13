// Отрисовка страницы телетекста уровня 1 на canvas — тот же разбор атрибутов, что
// (ETSI EN 300 706 §12.2 табл. 26, §15.3 ESC).
// rows: {номер ряда: [40 кодов]}; opts: {reveal, cursor:{r,c},
//   over: {'r,c': символ} — поправки X/26 (уже подходящие к клеткам),
//   cs: 96 символов основного набора для кодов 0x20..0x7F (без него —
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

  // Знакогенератор SAA5050 (как в приёмниках и BBC Micro Mode 7): матрицы 5×9 из проекта
  // только знаки наборов; код:9 рядов в hex.
  const FONT_SRC = "20:000000000000000000;21:040404040400040000;22:0a0a0a000000000000;23:0a0a1f0a1f0a0a0000;24:0e15140e05150e0000;25:181902040813030000;26:0814140815120d0000;27:040404000000000000;28:020408080804020000;29:080402020204080000;2a:04150e040e15040000;2b:0004041f0404000000;2c:000000000004040800;2d:0000000e0000000000;2e:000000000000040000;2f:000102040810000000;30:040a1111110a040000;31:040c040404040e0000;32:0e11010608101f0000;33:1f01020601110e0000;34:02060a121f02020000;35:1f101e0101110e0000;36:0608101e11110e0000;37:1f0102040808080000;38:0e11110e11110e0000;39:0e11110f01020c0000;3a:000004000000040000;3b:000004000004040800;3c:020408100804020000;3d:00001f001f00000000;3e:080402010204080000;3f:0e1102040400040000;40:0e11171517100e0000;41:040a11111f11110000;42:1e11111e11111e0000;43:0e11101010110e0000;44:1e11111111111e0000;45:1f10101e10101f0000;46:1f10101e1010100000;47:0e11101013110f0000;48:1111111f1111110000;49:0e04040404040e0000;4a:0101010101110e0000;4b:111214181412110000;4c:1010101010101f0000;4d:111b15151111110000;4e:111119151311110000;4f:0e11111111110e0000;50:1e11111e1010100000;51:0e11111115120d0000;52:1e11111e1412110000;53:0e11100e01110e0000;54:1f0404040404040000;55:1111111111110e0000;56:1111110a0a04040000;57:1111111515150a0000;58:11110a040a11110000;59:11110a040404040000;5a:1f01020408101f0000;5b:0f08080808080f0000;5c:001008040201000000;5d:1e02020202021e0000;5e:040a11000000000000;5f:0000000000001f0000;60:080402000000000000;61:00000e010f110f0000;62:10101e1111111e0000;63:00000f1010100f0000;64:01010f1111110f0000;65:00000e111f100e0000;66:0204040e0404040000;67:00000f1111110f010e;68:10101e111111110000;69:04000c0404040e0000;6a:040004040404040408;6b:0808090a0c0a090000;6c:0c04040404040e0000;6d:00001a151515150000;6e:00001e111111110000;6f:00000e1111110e0000;70:00001e1111111e1010;71:00000f1111110f0101;72:00000b0c0808080000;73:00000f100e011e0000;74:04040e040404020000;75:0000111111110f0000;76:000011110a0a040000;77:0000111115150a0000;78:0000110a040a110000;79:0000111111110f010e;7a:00001f0204081f0000;7b:030404080404030000;7c:040404040404040000;7d:180404020404180000;7e:081502000000000000;a1:000004000404040404;a3:0609081c08081f0000;a4:0000110e0a0e110000;a7:0e11100e110e01110e;b0:060906000000000000;bc:080808080903050701;bd:101010101601020407;be:180418041903050701;bf:00000400040408110e;c4:0a000e111f11110000;c5:04000e111f11110000;c9:02041f101e101f0000;d6:0a000e1111110e0000;dc:0a00111111110e0000;df:0c1212161111161010;e0:08040e010f110f0000;e1:02040e010f110f0000;e2:040a0e010f110f0000;e4:0a000e010f110f0000;e5:04000e010f110f0000;e7:00000f1010100f0204;e8:08040e111f100e0000;e9:02040e111f100e0000;ea:040a0e111f100e0000;eb:0a000e111f100e0000;ec:0804000c04040e0000;ed:0204000c04040e0000;ee:040a000c04040e0000;ef:0a000c0404040e0000;f1:050a1e111111110000;f2:0804000e11110e0000;f3:0204000e11110e0000;f4:040a000e11110e0000;f6:000a000e11110e0000;f7:0004001f0004000000;f9:0804111111110f0000;fa:0204111111110f0000;fb:040a001111110f0000;fc:000a001111110f0000;fd:0204111111110f010e;10d:0a040f1010100f0000;11b:0a040e111f100e0000;159:05020b0c0808080000;161:0a040f100e011e0000;165:09091c080808040000;16f:0400111111110f0000;17e:0a041f0204081f0000;402:1e08080e0909090102;403:02041f101010100000;404:0609101c1009060000;406:0e04040404040e0000;407:0a000e0404040e0000;408:0101010101110e0000;409:0c1414171515170000;40a:1414141f1515170000;40b:1e08080e0909090000;40c:020411121812110000;40f:1111111111111f0400;410:0e1111111f11110000;411:1f10101f11111f0000;412:1e11111e11111e0000;413:1f1010101010100000;414:060a0a0a0a0a1f1100;415:1f10101e10101f0000;416:1515150e1515150000;417:0e11010601110e0000;418:111113151911110000;419:151113151911110000;41a:111214181412110000;41b:070909090909190000;41c:111b15151111110000;41d:1111111f1111110000;41e:0e11111111110e0000;41f:1f1111111111110000;420:1e11111e1010100000;421:0e11101010110e0000;422:1f0404040404040000;423:1111111f01011f0000;424:041f1515151f040000;425:11110a040a11110000;426:1212121212121f0100;427:1111111f0101010000;428:1515151515151f0000;429:1515151515151f0100;42a:1808080f09090f0000;42b:1111111d15151d0000;42c:1010101f11111f0000;42d:0c12010701120c0000;42e:1215151d1515120000;42f:0f11110f0509110000;430:00000e010f110f0000;431:0e101e1111111e0000;432:00001e111e111e0000;433:00001f101010100000;434:0000060a0a0a1f1100;435:00000e111f100e0000;436:000015150e15150000;437:00000e1106110e0000;438:000011131519110000;439:000411131519110000;43a:000011121c12110000;43b:000007090909190000;43c:0000111b1511110000;43d:000011111f11110000;43e:00000e1111110e0000;43f:00001f111111110000;440:00001e1111111e1010;441:00000e1110110e0000;442:00001f040404040000;443:0000111111110f010e;444:00040e1515150e0400;445:0000110a040a110000;446:0000121212121f0100;447:00001111110f010000;448:0000151515151f0000;449:0000151515151f0100;44a:000018080e090e0000;44b:000011111d151d0000;44c:000010101e111e0000;44d:00000c1206120c0000;44e:000012151d15120000;44f:00000f110f05190000;452:081e080e0909090102;453:02041f101010100000;454:00000c1218120c0000;456:04000c0404040e0000;457:0a000c0404040e0000;458:040004040404040408;459:00000c141615160000;45a:000014141e15160000;45b:081e080e0909090000;45c:020411121c12110000;2014:0000001f0000000000;2016:0a0a0a0a0a0a0a0000;2190:0004081f0804000000;2191:00040e150404000000;2192:0004021f0204000000;25a0:1f1f1f1f1f1f1f0000";
  const FONT = new Map(FONT_SRC.split(';').map(e => { const [k, v] = e.split(':');
    return [parseInt(k, 16), Array.from({length: 9}, (_, i) => parseInt(v.substr(2 * i, 2), 16))]; }));
  const ROUND = new Map();
  // клетка 12×20 со «сглаживанием» SAA5050: пустая точка получает угловую четверть там,
  // где по диагонали сходятся две закрашенные соседки -> прямоугольники [x, y, w] по рядам
  function rounded(code) {
    if (ROUND.has(code)) return ROUND.get(code);
    const g = FONT.get(code); let runs = null;
    if (g) {
      const px = (c, r) => c >= 1 && c < 6 && r >= 1 && r < 10 && ((g[r - 1] >> (5 - c)) & 1) === 1;
      const m = Array.from({length: 20}, () => new Array(12).fill(false));
      for (let r = 0; r < 10; r++) for (let c = 0; c < 6; c++) {
        const x = 2 * c, y = 2 * r;
        if (px(c, r)) { m[y][x] = m[y][x + 1] = m[y + 1][x] = m[y + 1][x + 1] = true; continue; }
        const w = px(c - 1, r), e = px(c + 1, r), n = px(c, r - 1), so = px(c, r + 1);
        if (w && n && !px(c - 1, r - 1)) m[y][x] = true;
        if (e && n && !px(c + 1, r - 1)) m[y][x + 1] = true;
        if (w && so && !px(c - 1, r + 1)) m[y + 1][x] = true;
        if (e && so && !px(c + 1, r + 1)) m[y + 1][x + 1] = true;
      }
      runs = [];
      for (let y = 0; y < 20; y++) for (let x = 0; x < 12;) {
        if (!m[y][x]) { x++; continue; }
        let x1 = x; while (x1 < 12 && m[y][x1]) x1++;
        runs.push([x, y, x1 - x]); x = x1;
      }
    }
    ROUND.set(code, runs);
    return runs;
  }

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
        g.save(); g.translate(x, y); g.scale(cell.w * CW / 12, cell.h * CH / 20);
        const runs = cell.text.length === 1 ? rounded(cell.text.codePointAt(0)) : null;
        if (runs) for (const [rx, ry, rw] of runs) g.fillRect(rx, ry, rw, 1);
        else { g.font = 'bold 17px "Courier New",monospace'; g.textBaseline = 'top'; g.fillText(cell.text, 0, 1); }
        g.restore();
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

  // мигание 0,75 Гц: 3/4 видно, 1/4 скрыто (как в приёмниках WST).
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
