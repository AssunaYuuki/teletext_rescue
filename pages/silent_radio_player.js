// Проигрыватель табло Silent Radio (112×15 светодиодов). Данные — DATA из silent_radio.py:
// zones[z] = [{seq, title, text, frames}], кадр — {f: столбцы, t: секунды} (бит 0 — верхняя точка),
// {scroll: столбцы, row, speed} — бегущая строка, {scroll2: [{cols,row}], speed} — две строки едут.
(function () {
  var W = 112, H = 15, S = 8;
  var cv = document.getElementById('sign'), cx = cv.getContext('2d');
  cv.width = W * S; cv.height = H * S;
  var zsel = document.getElementById('zone'), list = document.getElementById('list');
  var cap = document.getElementById('cap');
  document.getElementById('src').textContent = DATA.source + ', line ' + DATA.line;
  var zones = Object.keys(DATA.zones).sort();
  zones.forEach(function (z) {
    var o = document.createElement('option'); o.value = z;
    o.textContent = z + ' (' + DATA.zones[z].length + ')'; zsel.appendChild(o);
  });
  var items, it = 0, fr = 0, t0 = 0, playing = true;

  function draw(cols) {
    cx.fillStyle = '#000'; cx.fillRect(0, 0, cv.width, cv.height);
    for (var x = 0; x < W; x++) {
      var c = cols[x] || 0;
      for (var y = 0; y < H; y++) {
        cx.fillStyle = (c >> y) & 1 ? '#ffa31a' : '#2a1606';
        cx.beginPath(); cx.arc(x * S + S / 2, y * S + S / 2, S * 0.38, 0, 6.2832); cx.fill();
      }
    }
  }
  function scrollCols(lines, off) {          // окно табло на бегущий текст, сдвиг off точек
    var out = new Array(W).fill(0);
    lines.forEach(function (l) {
      for (var x = 0; x < W; x++) {
        var k = x - W + off;
        if (k >= 0 && k < l.cols.length) out[x] |= l.cols[k] << l.row;
      }
    });
    return out;
  }
  function dur(f) {
    if (f.f) return f.t;
    var n = f.scroll ? f.scroll.length : Math.max.apply(null, f.scroll2.map(function (l) { return l.cols.length; }));
    return (W + n) / f.speed;
  }
  function select(z) {
    items = DATA.zones[z]; list.innerHTML = '';
    items.forEach(function (x, i) {
      var d = document.createElement('div');
      d.textContent = x.seq + '  ' + x.title + (x.copies > 1 ? '  (×' + x.copies + ')' : '');
      d.onclick = function () { go(i); }; list.appendChild(d);
    });
    go(0);
  }
  function go(i) {
    it = (i + items.length) % items.length; fr = 0; t0 = performance.now();
    [].forEach.call(list.children, function (d, k) { d.className = k == it ? 'on' : ''; });
    list.children[it].scrollIntoView({block: 'nearest'});
    cap.textContent = items[it].text.join('  ·  ');
  }
  function tick(now) {
    if (items && items.length) {
      var item = items[it], f = item.frames[fr];
      if (!playing) t0 += now - (tick.last || now);
      var el = (now - t0) / 1000;
      if (el >= dur(f)) {
        t0 = now; fr++;
        if (fr >= item.frames.length) {
          if (it + 1 < items.length || document.getElementById('loop').checked) go(it + 1); else fr--;
        }
        f = items[it].frames[fr]; el = 0;
      }
      if (f.f && f.fx == 'wipe' && el < 0.4) {     // переход: вытеснение слева направо
        var k = Math.floor(el / 0.4 * W), prev = tick.prev || [];
        draw(f.f.map(function (c, x) { return x < k ? c : (prev[x] || 0); }));
      } else if (f.f) { draw(f.f); tick.prev = f.f; }
      else draw(scrollCols(f.scroll ? [{cols: f.scroll, row: f.row}] : f.scroll2, Math.floor(el * f.speed)));
    }
    tick.last = now; requestAnimationFrame(tick);
  }
  zsel.onchange = function () { select(zsel.value); };
  document.getElementById('play').onclick = function () {
    playing = !playing; this.textContent = playing ? 'Pause' : 'Play';
  };
  document.getElementById('prev').onclick = function () { go(it - 1); };
  document.getElementById('next').onclick = function () { go(it + 1); };
  select(zones[0]); requestAnimationFrame(tick);
})();
