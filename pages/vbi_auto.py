"""Открыть запись .vbi «как есть»: определить формат и службы в строках и сразу всё прочитать.

  python pages/vbi_auto.py запись.vbi

1. vbi_probe.py — формат записи и что идёт в каждой строке;
2. по найденному — всё в одну папку «<запись>_vbi»:
   WST PAL телетекст (bt8x8)  -> teletext/ (проект страниц; decode_vbi.py);
   NABTS                      -> <запись>.t33 (nabts_slicer.py; не-cx23885 записи
                                 пересчитываются во временный дамп 27 МГц);
   Silent Radio               -> silentradio/ (табло, тексты; silent_radio.py);
   CC (строка 21)             -> cc_<строка>.txt;
   зашифрованная рассылка 5,73 Мбит/с (WTTW: строки 19, 20, 22)
                              -> datacast/ (пакеты, адреса, расписание; datacast.py);
   испытательные сигналы — АЧХ по multiburst в отчёт (vits.py).
3. Отчёт: report.txt и report.json (его читает программа, чтобы открыть результаты).
Готовые результаты повторно не пересчитываются (кроме --again).
"""
import json, os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vbi_probe as VP


def step(t):
    print('STEP ' + t, flush=True)


def run(args):
    """Запуск декодера с передачей его строк STEP/PROGRESS наверх."""
    p = subprocess.Popen([sys.executable, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding='utf-8', errors='replace', cwd=HERE,
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    for line in p.stdout:
        print(line.rstrip(), flush=True)
    return p.wait()


# ------------------------------------------------------------------ CC (EIA-608)
CC_SPECIAL = {0x2A: 'á', 0x5C: 'é', 0x5E: 'í', 0x5F: 'ó', 0x60: 'ú', 0x7B: 'ç', 0x7C: '÷', 0x7D: 'Ñ', 0x7E: 'ñ', 0x7F: '█'}


def cc_text(R, row, parity):
    """Текст субтитров: печатные пары, управляющие коды 14 2C/2F/2D — разрыв строки."""
    out = []; last = None; good = 0
    for u in range(parity or 0, R.n, 2 if parity is not None else 1):
        b = VP.cc_slice(R.line(u, row), R.fs)
        if b is None:
            continue
        if any(bin(x).count('1') % 2 == 0 for x in b):    # нечётность нарушена
            continue
        good += 1
        c1, c2 = b[0] & 0x7F, b[1] & 0x7F
        if c1 == 0 and c2 == 0:
            continue
        if 0x10 <= c1 < 0x20:                             # управляющий код (передаётся дважды)
            if (c1, c2) == last:
                last = None; continue
            last = (c1, c2)
            if c1 in (0x14, 0x1C) and c2 in (0x2C, 0x2F, 0x2D, 0x20, 0x25, 0x26, 0x27, 0x29):
                if out and out[-1] != '\n': out.append('\n')
            continue
        last = None
        for c in (c1, c2):
            if c >= 0x20:
                out.append(CC_SPECIAL.get(c, chr(c)))
    return ''.join(out), good


# ------------------------------------------------------------------ NABTS из любого формата
def nabts(src, res, R, lines, again, rep_dir):
    out = os.path.join(rep_dir, os.path.splitext(os.path.basename(src))[0] + '.t33')
    if os.path.exists(out) and not again:
        return out
    if res['format'] == 'cx23885':
        rows = sorted({L['row'] for L in lines})
        rc = run([os.path.join(HERE, 'nabts_slicer.py'), src, '--out', out, '--lines', ','.join(str(10 + r) for r in rows)])
        return out if rc == 0 and os.path.exists(out) else None
    # пересчёт строк с NABTS в дамп cx23885 (27 МГц, 1440 отсчётов, слоты строк 10–21)
    rows = sorted({L['row'] for L in lines})[:12]
    tmp = os.path.join(tempfile.gettempdir(), 'vbi_auto_nabts_{0}.vbi'.format(os.getpid()))
    step('Resampling the NABTS lines to 27 MHz')
    o = np.memmap(tmp, np.uint8, 'w+', shape=(R.n, 12, 1440))
    t27 = np.arange(1440) / 27e6 * R.fs + int(R.ns * 0.1)
    for u in range(R.n):
        for k, r in enumerate(rows):
            y = R.line(u, r)
            o[u, k] = np.clip(np.interp(t27, np.arange(len(y)), y) * (255.0 / max(1.0, y.max())), 0, 255)
        if u % 2000 == 0:
            print('PROGRESS {0} {1}'.format(u, R.n), flush=True)
    o.flush(); del o
    try:
        rc = run([os.path.join(HERE, 'nabts_slicer.py'), tmp, '--out', out, '--lo', '0', '--hi', '300',
                  '--lines', ','.join(str(10 + k) for k in range(len(rows)))])
    finally:
        try: os.remove(tmp)
        except OSError: pass
    return out if rc == 0 and os.path.exists(out) else None


def main():
    src = os.path.abspath(sys.argv[1]); again = '--again' in sys.argv
    base = os.path.splitext(src)[0]
    rep_dir = base + '_vbi'; os.makedirs(rep_dir, exist_ok=True)
    step('Looking at the recording: format and what each line carries')
    res = VP.probe(src)
    if res['format'] is None:
        print('Unknown recording format', flush=True)
        json.dump({'file': src, 'format': None, 'results': []}, open(os.path.join(rep_dir, 'report.json'), 'w'))
        sys.exit(2)
    R = VP.Rec(src, res['format'])
    by = {}
    for L in res['lines']:
        by.setdefault(L['kind'], []).append(L)
    results = []

    if 'WST PAL teletext' in by and res['format'].startswith('bt8x8'):
        out = os.path.join(rep_dir, 'teletext')
        if not os.path.exists(os.path.join(out, 'pages.json')) or again:
            step('Decoding WST teletext into pages')
            run([os.path.join(HERE, 'decode_vbi.py'), src, '--out', out, '--lpf', '32'])
        if os.path.exists(os.path.join(out, 'pages.json')):
            results.append({'service': 'WST teletext', 'kind': 'teletext_project', 'path': out})

    if 'NABTS' in by:
        step('Reading NABTS')
        out = nabts(src, res, R, by['NABTS'], again, rep_dir)
        if out:
            results.append({'service': 'NABTS', 'kind': 't33', 'path': out})

    if 'Silent Radio' in by:
        step('Reading Silent Radio (LED sign service)')
        import silent_radio as SR
        L = max(by['Silent Radio'], key=lambda l: l.get('read') or 0)
        out = os.path.join(rep_dir, 'silentradio')
        if not os.path.exists(os.path.join(out, 'index.html')) or again:
            data, line = SR.read_bytes(src, row=L['row'], parity=L['parity'])
            SR.save(src, out, data, line)
        results.append({'service': 'Silent Radio', 'kind': 'html', 'path': os.path.join(out, 'index.html')})

    dkey = [k for k in by if k.startswith('encrypted datacast')]
    if dkey:
        import datacast as DC
        out = os.path.join(rep_dir, 'datacast')
        if not os.path.exists(os.path.join(out, 'index.html')) or again:
            DC.export(src, out, [(L['row'], L['parity']) for L in by[dkey[0]]])
        results.append({'service': 'Encrypted datacast (packets, addresses, schedule)', 'kind': 'html',
                        'path': os.path.join(out, 'index.html')})

    if 'CC (line 21)' in by:
        for L in by['CC (line 21)']:
            step('Reading CC captions, line {0}'.format(L['tv_line']))
            txt, n = cc_text(R, L['row'], L['parity'])
            p = os.path.join(rep_dir, 'cc_line{0}{1}.txt'.format(L['tv_line'], '' if L['parity'] is None else 'AB'[L['parity']]))
            open(p, 'w', encoding='utf-8').write(txt)
            results.append({'service': 'CC captions (line {0})'.format(L['tv_line']), 'kind': 'text', 'path': p,
                            'note': '{0} intact byte pairs'.format(n)})

    # испытательные сигналы: АЧХ записи по multiburst
    step('Measuring test signals (VITS)')
    import vits as VT
    vt = VT.analyse(src, log=lambda *a: None, res=res)
    vt_text = VT.response_text(vt)

    # отчёт
    lines = []
    lines.append('Recording: ' + src)
    lines.append('Format: {0} — {1} {2}s'.format(res['format'], res['units'], res['unit']))
    lines.append('')
    lines.append('What each line carries:')
    for L in res['lines']:
        if L['kind'] == 'empty': continue
        lines.append('  line {0:3}{1:9} {2}{3}'.format(L['tv_line'], '' if L['parity'] is None else ' field ' + 'AB'[L['parity']],
                                                      L['kind'], '' if L.get('read') is None else ' (read {0:.0%})'.format(L['read'])))
    if vt_text:
        lines.append(''); lines += vt_text
    lines.append('')
    lines.append('Results:' if results else 'Nothing that the program can decode was found.')
    for r in results:
        lines.append('  {0}: {1}{2}'.format(r['service'], r['path'], ' — ' + r['note'] if r.get('note') else ''))
    open(os.path.join(rep_dir, 'report.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    json.dump({'file': src, 'format': res['format'], 'probe': res, 'results': results, 'vits': vt, 'vits_text': vt_text},
              open(os.path.join(rep_dir, 'report.json'), 'w', encoding='utf-8'), indent=1)
    print('\n'.join(lines), flush=True)
    print('REPORT ' + os.path.join(rep_dir, 'report.json'), flush=True)


if __name__ == '__main__':
    main()
