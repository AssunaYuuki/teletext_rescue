// Показ страницы NAPLPS, как на экране приёмника:
// клетки получают «чернила» в свои моменты, стирание экрана, смена карты цветов
// перекрашивает нарисованное, процессы мигания чередуют цвета.
var NabtsPlayer = (function () {
  function start(P, canvas, replayBtn, animBox) {
    var w = P.w, h = P.h;
    canvas.width = w; canvas.height = h;
    var ctx = canvas.getContext('2d');
    var img = ctx.createImageData(w, h);
    var ids, map, blink, i, t0, raf = null, lastKey = null, dirty;

    function colour(ink, T) {
      var a, col;
      if (ink[0] === 'c') return ink[1];
      if (ink[0] === 'm') { a = ink[1]; col = map[a]; } else { a = ink[2]; col = ink[1]; }
      var b = blink[a];
      if (b && b[2] && b[3]) {
        var ph = T - b[5] - 0.1 * b[4];
        if (ph >= 0 && (ph % (0.1 * (b[2] + b[3]))) >= 0.1 * b[2])
          col = b[0] >= 0 ? map[b[0] % 16] : b[1];
      }
      return col;
    }

    function hasBlink() {
      for (var k = 0; k < 16; k++) if (blink[k] && blink[k][2] && blink[k][3]) return true;
      return false;
    }

    function advance(T) {
      var S = P.steps;
      while (i < S.length && S[i][0] <= T) {
        var s = S[i++];
        if (s[1]) ids.fill(0);
        if (s[2]) map = s[2];
        if (s[3]) blink = s[3];
        var r = s[4];
        for (var k = 0; k < r.length; k += 3) ids.fill(r[k + 2], r[k], r[k] + r[k + 1]);
        dirty = true;
      }
    }

    function paint(T) {
      var lut = P.inks.map(function (ink) { return colour(ink, T); });
      var key = lut.join(';');
      if (!dirty && key === lastKey) return;
      lastKey = key; dirty = false;
      var d = img.data;
      for (var r = 0; r < h; r++) {
        var src = r * w, dst = (h - 1 - r) * w * 4;            // строка 0 — низ экрана
        for (var c = 0; c < w; c++) {
          var col = lut[ids[src + c]], o = dst + c * 4;
          d[o] = col[0]; d[o + 1] = col[1]; d[o + 2] = col[2]; d[o + 3] = 255;
        }
      }
      ctx.putImageData(img, 0, 0);
    }

    function frame() {
      raf = null;
      var T = (performance.now() - t0) / 1000;
      advance(T);
      paint(T);
      if (i < P.steps.length || hasBlink()) raf = requestAnimationFrame(frame);
    }

    function play() {
      if (raf) cancelAnimationFrame(raf);
      ids = new Uint16Array(w * h); map = P.map; blink = new Array(16).fill(null);
      i = 0; lastKey = null; dirty = true;
      var anim = !animBox || animBox.checked;
      t0 = performance.now() - (anim ? 0 : P.end * 1000);
      if (!anim) advance(Infinity);
      frame();
    }

    if (replayBtn) replayBtn.onclick = play;
    if (animBox) animBox.onchange = play;
    play();
  }
  return { start: start };
})();
