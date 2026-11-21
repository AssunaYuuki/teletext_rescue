// Проигрыватель табло Silent Radio (112×15 светодиодов). Данные — DATA:
// zones[z] = [{seq, title, text, frames, copies, intact, time, at}], кадр —
// {f: столбцы, t: секунды, fx: переход (число SrFx или "wipe"), ft: секунды перехода},
// {scroll: столбцы, row, speed} — бегущая строка, {scroll2: [{cols,row}], speed} — строки едут.
// Бит 0 столбца — верхняя точка. Интерфейс строится здесь же (страница — только DATA и этот файл).
(function () {
  'use strict';
  var W = 112, H = 15, M = 0x7fff;
  var FX = ['cut', 'wipe right', 'wipe left', 'wipe out', 'wipe in', 'slide left', 'slide right',
            'scroll up', 'scroll down', 'dissolve', 'split', 'pan'];
  var COLOURS = {amber: [255, 170, 40], red: [255, 50, 30], green: [90, 255, 80], white: [235, 240, 255]};

  // ---------------------------------------------------------------- page
  var css = [
    ':root{color-scheme:dark}',
    'body{background:#0d0e10;color:#c9ccd2;font:14px/1.4 system-ui,Segoe UI,sans-serif;margin:0;padding:16px}',
    '#srp{max-width:1400px;margin:0 auto}',
    '#srp h1{font-size:18px;margin:0 0 2px;color:#eee;font-weight:600}',
    '#srp .sub{color:#8a8f98;font-size:12px;margin-bottom:12px}',
    '#housing{background:linear-gradient(#26282c,#16171a);border-radius:10px;padding:14px;box-shadow:0 6px 24px #000a,inset 0 1px 0 #fff1}',
    '#sign{display:block;width:100%;height:auto;background:#050505;border-radius:4px;image-rendering:auto}',
    '#bar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:12px 0 8px}',
    '#bar button,#bar select,#bar input[type=search]{background:#1d1f23;color:#dde;border:1px solid #33363c;border-radius:5px;padding:5px 10px;font:inherit}',
    '#bar button:hover{background:#2a2d33}',
    '#bar label{color:#9aa;display:flex;align-items:center;gap:4px}',
    '#bar .sp{flex:1}',
    '#prog{display:flex;gap:2px;height:8px;margin:4px 0 10px;cursor:pointer}',
    '#prog div{flex:1;background:#24272c;border-radius:2px;overflow:hidden;position:relative}',
    '#prog div i{position:absolute;left:0;top:0;bottom:0;background:#e0902a;width:0}',
    '#cap{color:#f0b24a;min-height:1.4em;font-size:15px;margin-bottom:2px}',
    '#meta{color:#7d838c;font-size:12px;min-height:1.3em}',
    '#list{max-height:42vh;overflow:auto;border:1px solid #26292e;border-radius:6px;margin-top:10px;position:relative}',
    '#list table{border-collapse:collapse;width:100%}',
    '#list td{padding:4px 8px;border-bottom:1px solid #1b1d21;white-space:nowrap;cursor:pointer}',
    '#list td.t{white-space:normal;color:#d5d7db;width:100%}',
    '#list td.n{color:#7d838c;font-variant-numeric:tabular-nums}',
    '#list tr:hover td{background:#17191c}',
    '#list tr.on td{background:#3b2a14;color:#fff}',
    '#list .z{display:inline-block;min-width:18px;text-align:center;border-radius:3px;background:#2b2f36;color:#cfd3da;font-size:11px;padding:0 4px}',
    '#help{color:#6c727b;font-size:12px;margin-top:8px}'
  ].join('\n');
  var st = document.createElement('style'); st.textContent = css; document.head.appendChild(st);
  [].slice.call(document.body.children).forEach(function (e) { if (e.tagName != 'SCRIPT') e.remove(); });
  var root = document.createElement('div'); root.id = 'srp';
  root.innerHTML =
    '<h1>Silent Radio</h1><div class="sub" id="src"></div>' +
    '<div id="housing"><canvas id="sign"></canvas></div>' +
    '<div id="bar">' +
    '<button id="prev" title="Previous item (←)">&#x23EE;</button>' +
    '<button id="back" title="Previous picture (,)">&#x25C0;</button>' +
    '<button id="play" title="Play / pause (space)">Pause</button>' +
    '<button id="fwd" title="Next picture (.)">&#x25B6;</button>' +
    '<button id="next" title="Next item (→)">&#x23ED;</button>' +
    '<select id="zone" title="Zone"></select>' +
    '<select id="speed" title="Speed"><option value="0.5">0.5×</option><option value="1" selected>1×</option>' +
    '<option value="2">2×</option><option value="4">4×</option></select>' +
    '<select id="colour" title="LED colour"></select>' +
    '<label><input type="checkbox" id="loop" checked> loop</label>' +
    '<span class="sp"></span><input type="search" id="find" placeholder="find text…">' +
    '</div>' +
    '<div id="prog"></div><div id="cap"></div><div id="meta"></div>' +
    '<div id="list"><table><tbody id="rows"></tbody></table></div>' +
    '<div id="help">Space — play/pause · ← → — item · , . — picture · click the bar to jump to a picture.</div>';
  document.body.appendChild(root);
  var $ = function (id) { return document.getElementById(id); };

  // ---------------------------------------------------------------- data
  var all = [];
  Object.keys(DATA.zones).sort().forEach(function (z) {
    DATA.zones[z].forEach(function (it, i) {
      it.zone = z; it.order = all.length;
      if (it.at == null) it.at = i;
      all.push(it);
    });
  });
  $('src').textContent = (DATA.source || '') + (DATA.line ? ' · line ' + DATA.line : '') +
    ' · ' + all.length + ' items in ' + Object.keys(DATA.zones).length + ' zone(s)';
  var zsel = $('zone');
  var zopt = [['*', 'all zones (' + all.length + ')']];
  Object.keys(DATA.zones).sort().forEach(function (z) { zopt.push([z, 'zone ' + z + ' (' + DATA.zones[z].length + ')']); });
  zopt.forEach(function (o) { var e = document.createElement('option'); e.value = o[0]; e.textContent = o[1]; zsel.appendChild(e); });
  Object.keys(COLOURS).forEach(function (c) { var e = document.createElement('option'); e.value = c; e.textContent = c; $('colour').appendChild(e); });
  var store = {get: function (k) { try { return localStorage.getItem('srp.' + k); } catch (e) { return null; } },
               set: function (k, v) { try { localStorage.setItem('srp.' + k, v); } catch (e) {} }};
  if (store.get('colour') && COLOURS[store.get('colour')]) $('colour').value = store.get('colour');
  if (store.get('speed')) $('speed').value = store.get('speed');

  // ---------------------------------------------------------------- frames
  function fxOf(f) { return typeof f.fx == 'string' ? (f.fx == 'wipe' ? 1 : 0) : (f.fx | 0); }
  function fxTime(f) { var x = fxOf(f); return x || f.fx2 != null ? (f.ft || 0.45) : 0; }
  function lines(f) { return f.scroll ? [{cols: f.scroll, row: f.row}] : (f.scroll2 || []); }
  function dur(f) {
    if (f.f) return Math.max(f.t || 2, fxTime(f));
    var n = 0; lines(f).forEach(function (l) { n = Math.max(n, l.cols.length); });
    return (W + n) / (f.speed || 36);
  }
  function hash(x, y) {
    var h = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663)) >>> 0;
    h ^= h >>> 13; h = Math.imul(h, 0x5bd1e995) >>> 0; h ^= h >>> 15;
    return (h & 0xffff) / 65536;
  }
  function frameAt(f, t, prev) {               // the sign t seconds into frame f, prev = the picture before
    var out = new Array(W).fill(0), x;
    if (!f.f) {
      var off = Math.floor(t * (f.speed || 36));
      lines(f).forEach(function (l) {
        for (x = 0; x < W; x++) { var k = x - W + off; if (k >= 0 && k < l.cols.length) out[x] |= (l.cols[k] << l.row) & M; }
      });
      return out;
    }
    var img = [], old = [];
    for (x = 0; x < W; x++) { img[x] = (f.f[x] || 0) & M; old[x] = (prev[x] || 0) & M; }
    var fx = fxOf(f), ft = fxTime(f);
    if ((!fx && f.fx2 == null) || t >= ft) return img;
    var k = Math.max(0, t / ft);
    var two = f.fx2 != null && f.fx2 != fx;
    out = trans(fx, f.fa | 0, k, img, old, 0, two ? 8 : H);
    if (two) {                                   // the bottom text line comes in its own way
      var bot = trans(f.fx2, f.fa | 0, k, img, old, 8, H - 8);
      for (x = 0; x < W; x++) out[x] = (out[x] & 0xff) | (bot[x] & 0x7f00);
    }
    return out;
  }
  function trans(fx, fa, k, img, old, lo, rows) {  // one transition, k = 0..1 of the way; rolling stays in rows lo..
    lo = lo || 0; rows = rows || H;
    var out = new Array(W).fill(0), x, s = Math.round(k * W), r = Math.round(k * rows), B = (1 << rows) - 1;
    for (x = 0; x < W; x++) {
      var d = Math.abs(2 * x + 1 - W);
      switch (fx) {
        case 1: out[x] = x < s ? img[x] : old[x]; break;
        case 2: out[x] = x >= W - s ? img[x] : old[x]; break;
        case 3: out[x] = d < s ? img[x] : old[x]; break;
        case 4: out[x] = d >= W - s ? img[x] : old[x]; break;
        case 5: out[x] = x < W - s ? old[x + s] : img[x - (W - s)]; break;
        case 6: out[x] = x >= s ? old[x - s] : img[x + W - s]; break;
        case 7: out[x] = (((((old[x] >> lo) & B) >> r) | (((img[x] >> lo) & B) << (rows - r))) & B) << lo & M; break;
        case 8: out[x] = (((((old[x] >> lo) & B) << r) | (((img[x] >> lo) & B) >> (rows - r))) & B) << lo & M; break;
        case 9: {
          var v = 0;
          for (var y = 0; y < H; y++) v |= (((hash(x, y) < k ? img[x] : old[x]) >> y) & 1) << y;
          out[x] = v; break;
        }
        case 10: {
          var top = x < s ? img[x + W - s] : old[x - s];
          var bot = x >= W - s ? img[x - (W - s)] : old[x + s];
          out[x] = (top & 0xff) | (bot & (M & ~0xff)); break;
        }
        case 11: {                                // the window moves f.fa columns over the sign's memory
          var dd = fa, mm = Math.round(k * dd);
          if (dd > 0) out[x] = x + mm < W ? old[x + mm] : img[x + mm - dd];
          else out[x] = x + mm >= 0 ? old[x + mm] : img[x + mm - dd];
          break;
        }
        default: out[x] = img[x];
      }
    }
    return out;
  }

  // ---------------------------------------------------------------- the sign
  var cv = $('sign'), cx = cv.getContext('2d');
  var led = document.createElement('canvas'), lx = led.getContext('2d');
  var lum = new Float32Array(W * H), P = 10, PAD = 6;
  function resize() {
    var dpr = window.devicePixelRatio || 1;
    var css = Math.max(320, cv.parentNode.clientWidth - 28);
    P = Math.max(3, Math.floor(css * dpr / (W + 2 * PAD / 10)));
    cv.width = led.width = W * P + 2 * PAD; cv.height = led.height = H * P + 2 * PAD;
  }
  window.addEventListener('resize', resize); resize();
  var glow = 'filter' in cx;
  function paint(cols, dt) {
    var c = COLOURS[$('colour').value] || COLOURS.amber;
    var rise = dt ? 1 - Math.exp(-dt / 0.012) : 1, fall = dt ? 1 - Math.exp(-dt / 0.06) : 1;   // the first picture at once
    lx.fillStyle = '#060606'; lx.fillRect(0, 0, led.width, led.height);
    var rad = P * 0.4, x, y, i;
    for (x = 0; x < W; x++) {
      var col = cols[x] || 0;
      for (y = 0; y < H; y++) {
        i = x * H + y;
        var on = (col >> y) & 1;
        lum[i] += ((on ? 1 : 0) - lum[i]) * (on ? rise : fall);
        var l = lum[i], b = 0.09 + 0.91 * l;
        lx.fillStyle = 'rgb(' + Math.round(c[0] * b) + ',' + Math.round(c[1] * b * (0.55 + 0.45 * l)) + ',' +
                       Math.round(c[2] * b * (0.4 + 0.6 * l)) + ')';
        lx.beginPath(); lx.arc(PAD + x * P + P / 2, PAD + y * P + P / 2, rad, 0, 6.2832); lx.fill();
      }
    }
    cx.globalCompositeOperation = 'source-over'; cx.filter = 'none'; cx.globalAlpha = 1;
    cx.drawImage(led, 0, 0);
    if (glow) {
      cx.globalCompositeOperation = 'lighter'; cx.globalAlpha = 0.55; cx.filter = 'blur(' + Math.max(2, P * 0.7) + 'px)';
      cx.drawImage(led, 0, 0);
      cx.globalCompositeOperation = 'source-over'; cx.filter = 'none'; cx.globalAlpha = 1;
    }
  }

  // ---------------------------------------------------------------- playing
  var items = [], it = 0, fr = 0, el = 0, playing = true, prev = new Array(W).fill(0), shown = prev;
  function select() {
    var z = zsel.value, q = $('find').value.trim().toLowerCase();
    items = all.filter(function (x) {
      return (z == '*' || x.zone == z) && (!q || (x.text || []).join(' ').toLowerCase().indexOf(q) >= 0);
    });
    items.sort(function (a, b) { return a.at - b.at || a.order - b.order; });
    var rows = $('rows'); rows.innerHTML = '';
    items.forEach(function (x, i) {
      var tr = document.createElement('tr');
      var t = (x.text && x.text.length ? x.text : [x.title || '']).join('  ·  ');
      tr.innerHTML = '<td class="n"></td><td><span class="z"></span></td><td class="n"></td><td class="n"></td><td class="t"></td>';
      tr.children[0].textContent = x.time || '';
      tr.children[1].firstChild.textContent = x.zone;
      tr.children[2].textContent = x.seq;
      tr.children[3].textContent = x.copies > 1 ? '×' + x.copies : '';
      tr.children[4].textContent = t;
      tr.onclick = function () { go(i); };
      rows.appendChild(tr);
    });
    go(0);
  }
  function go(i, keepPrev) {
    if (!items.length) { $('cap').textContent = 'nothing matches'; $('prog').innerHTML = ''; return; }
    it = (i + items.length) % items.length; fr = 0; el = 0;
    if (!keepPrev) prev = shown;
    var rows = $('rows').children;
    for (var k = 0; k < rows.length; k++) rows[k].className = k == it ? 'on' : '';
    var L = $('list'), r = rows[it];           // scroll the list only, not the page
    if (r) {
      if (r.offsetTop < L.scrollTop) L.scrollTop = r.offsetTop;
      else if (r.offsetTop + r.offsetHeight > L.scrollTop + L.clientHeight) L.scrollTop = r.offsetTop + r.offsetHeight - L.clientHeight;
    }
    var item = items[it], pg = $('prog'); pg.innerHTML = '';
    item.frames.forEach(function (f, k) {
      var d = document.createElement('div'); d.style.flex = String(Math.max(0.3, dur(f)));
      d.title = (f.w || '') + ' — ' + (f.f ? FX[fxOf(f)] || '' : 'crawl') + ', ' + dur(f).toFixed(1) + ' s';
      d.appendChild(document.createElement('i'));
      d.onclick = function () { seek(k); };
      pg.appendChild(d);
    });
    $('cap').textContent = (item.text || []).join('  ·  ');
    meta();
  }
  function meta() {
    var item = items[it]; if (!item) return;
    var f = item.frames[fr] || {};
    $('meta').textContent = 'zone ' + item.zone + ' · packet ' + item.seq + (item.time ? ' · at ' + item.time : '') +
      ' · received ' + (item.copies || 1) + '×' + (item.intact != null ? ' (' + item.intact + ' whole)' : '') +
      ' · picture ' + (fr + 1) + '/' + item.frames.length + ' · ' + (f.f ? FX[fxOf(f)] : 'crawl');
  }
  function seek(k) {                            // to picture k: the one before it is what is on the sign
    var item = items[it]; prev = new Array(W).fill(0);
    for (var j = 0; j < k; j++) prev = frameAt(item.frames[j], dur(item.frames[j]), prev);
    fr = k; el = 0; meta();
  }
  function advance() {
    var item = items[it], f = item.frames[fr];
    prev = frameAt(f, dur(f), prev); el = 0; fr++;
    if (fr >= item.frames.length) {
      if (it + 1 < items.length || $('loop').checked) go(it + 1, true); else { fr = item.frames.length - 1; el = dur(f); playing = false; $('play').textContent = 'Play'; }
    }
    meta();
  }
  var last = 0;
  function tick(now) {
    var dt = last ? Math.min(0.1, (now - last) / 1000) : 0; last = now;
    if (items.length) {
      if (playing) {
        el += dt * parseFloat($('speed').value);
        var guard = 0;
        while (el >= dur(items[it].frames[fr]) && guard++ < 50) {
          var rest = el - dur(items[it].frames[fr]); advance(); el = rest;
          if (!playing) break;
        }
      }
      var item = items[it], f = item.frames[fr];
      shown = frameAt(f, Math.min(el, dur(f)), prev);
      var bars = $('prog').children;
      for (var k = 0; k < bars.length; k++)
        bars[k].firstChild.style.width = (k < fr ? 100 : k > fr ? 0 : Math.min(100, 100 * el / dur(f))) + '%';
    }
    paint(shown, dt);
    requestAnimationFrame(tick);
  }

  // ---------------------------------------------------------------- controls
  function toggle() { playing = !playing; $('play').textContent = playing ? 'Pause' : 'Play'; }
  function step(d) {                            // one picture on or back, paused
    if (!items.length) return;
    if (playing) toggle();
    if (d > 0) advance();
    else if (fr > 0) seek(fr - 1);
    else { go(it - 1); seek(Math.max(0, items[it].frames.length - 1)); }
    var f = items[it].frames[fr];
    el = f.f ? fxTime(f) : 0;                   // a picture whole, a crawl from its start
  }
  $('play').onclick = toggle;
  $('prev').onclick = function () { go(it - 1); };
  $('next').onclick = function () { go(it + 1); };
  $('back').onclick = function () { step(-1); };
  $('fwd').onclick = function () { step(1); };
  zsel.onchange = select;
  $('find').oninput = select;
  $('speed').onchange = function () { store.set('speed', this.value); };
  $('colour').onchange = function () { store.set('colour', this.value); };
  document.addEventListener('keydown', function (e) {
    if (e.target.tagName == 'INPUT' && e.target.type == 'search') return;
    if (e.key == ' ') { toggle(); e.preventDefault(); }
    else if (e.key == 'ArrowLeft') go(it - 1);
    else if (e.key == 'ArrowRight') go(it + 1);
    else if (e.key == ',') step(-1);
    else if (e.key == '.') step(1);
  });
  select();
  // index.html#9BAB/2/0.3 — packet 9BAB, its 3rd picture, 0.3 s into it, paused
  (function () {
    var m = /^#([0-9A-Fa-f-]+)(?:\/(\d+))?(?:\/([\d.]+))?/.exec(location.hash || '');
    if (!m) return;
    for (var i = 0; i < items.length; i++)
      if (String(items[i].seq).toUpperCase() == m[1].toUpperCase()) {
        go(i); seek(Math.min(items[i].frames.length - 1, parseInt(m[2] || '0', 10)));
        el = parseFloat(m[3] || '0'); playing = false; $('play').textContent = 'Play';
        break;
      }
  })();
  requestAnimationFrame(tick);
})();
