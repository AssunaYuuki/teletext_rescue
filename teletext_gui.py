"""Teletext Rescue — view, edit and export teletext, NABTS and other data recovered from VBI recordings.

Run:  python teletext_gui.py
File menu: open a .vbi recording, a .t42 / .t34 / .t33 packet stream or an existing project.
A project is a folder "<name>_teletext" next to the source file: pages.json (build),
extras.json (keys, flags, clock), quality.json, edits pages_edited.json (Save, Ctrl+S)
and the export (output.t42, output.html, html/).
"""
import copy, json, os, queue, subprocess, sys, threading, time, webbrowser, tkinter as tk
from tkinter import ttk, messagebox, simpledialog, filedialog
from PIL import Image, ImageDraw, ImageFont, ImageTk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages'))
import tt_export as X
import gui_parts as GP
GUI_STATE = os.path.join(X.PG, 'gui_state.json')       # последний открытый проект

# ------------------------------------------------------------------ отрисовка
COL = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
S = 1.5                                   # масштаб экрана
CW, CH = int(12 * S), int(20 * S)
try:
    FONT = ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:/Windows'), 'Fonts', 'courbd.ttf'), int(17 * S))
except OSError:
    FONT = ImageFont.load_default()
KEYS = [('#d22', 'red'), ('#2a2', 'green'), ('#cc2', 'yellow'), ('#2cc', 'cyan'), ('#999', 'index')]

import tt_level1 as L1
TV_GREY = (60, 60, 60)                    # «картинка» под страницей в режиме экрана ТВ
LAST = {'flash': False, 'box': False}     # что нашлось на последней отрисованной странице

def render(rows, over=None, reveal=False, cursor=None, t=None, t2=None, flash_on=True, tv=False):
    """Страница уровня 1 -> PIL Image (разбор атрибутов — tt_level1, EN 300 706 §12.2).
    flash_on — фаза мигания; tv — как на экране ТВ: видно только то, что в рамках
    (новостные вставки и субтитры), остальное — «картинка»."""
    t = t or X.page_table()
    im = Image.new('RGB', (40 * CW, 25 * CH), TV_GREY if tv else (0, 0, 0)); d = ImageDraw.Draw(im)
    cs, LAST['flash'], LAST['box'] = L1.cells(rows, t, t2, over)
    for cell in cs:
        x, y = cell.c * CW, cell.r * CH
        if tv and not cell.box: continue
        w, h = cell.w * CW, cell.h * CH
        d.rectangle([x, y, x + max(w, CW) - 1, y + max(h, cell.bh * CH) - 1], fill=COL[cell.bg])
        if (cell.conceal and not reveal) or (cell.flash and not flash_on): continue
        if cell.mosaic:
            ch = cell.mosaic; bits = [ch & 1, ch & 2, ch & 4, ch & 8, ch & 16, ch & 64]; cw = w / 2; chh = h / 3
            g = 1 if cell.sep else 0
            for i in range(6):
                if bits[i]:
                    cx, cy = x + (i % 2) * cw, y + (i // 2) * chh
                    d.rectangle([int(cx) + g, int(cy) + g, int(cx + cw) - 1 - g, int(cy + chh) - 1 - g], fill=COL[cell.fg])
        elif cell.text and cell.text != ' ':
            if (cell.w, cell.h) == (1, 1):
                d.text((x, y + 1), cell.text, font=FONT, fill=COL[cell.fg])
            else:
                g = Image.new('RGB', (CW, CH), COL[cell.bg]); ImageDraw.Draw(g).text((0, 1), cell.text, font=FONT, fill=COL[cell.fg])
                im.paste(g.resize((w, h), Image.NEAREST), (x, y))
    if cursor:
        r, c = cursor
        d.rectangle([c * CW, r * CH, c * CW + CW - 1, r * CH + CH - 1], outline=(255, 136, 0), width=2)
    return im

# ------------------------------------------------------------------ приложение
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.geometry('1320x920'); self.minsize(1000, 760)
        GP.apply_theme(self)
        ico = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon.ico')
        if not os.path.exists(ico): ico = os.path.join(X.PG, 'icon.ico')     # в собранной версии — рядом с данными
        try: self.iconbitmap(default=ico)              # значок всех окон программы
        except tk.TclError: pass
        self.pages = {}; self.ids = []; self.cur = None; self.dirty = False; self.edited = set(); self.undo = []
        self.start = None
        self.build_ui()
        self.update_idletasks(); self.pan.sashpos(0, 470)    # ширина списка страниц
        last = (X.load_json(GUI_STATE, {}) or {}).get('last')
        if not (last and X.is_project(last) and self.load_project(last, quiet=True)):
            self.title('Teletext Rescue')
            self.show_start()
            self.status('Open a recording or stream: “File” menu (.vbi, .t42, .t34, .t33) or an existing project')
        self.protocol('WM_DELETE_WINDOW', self.on_close)

    # ---------- проекты
    def load_project(self, path, quiet=False):
        """Open the project in folder path. -> success."""
        prev = X.PROJ
        X.set_project(path)
        try:
            pages, edited, src = X.load_state()
            if not pages: raise FileNotFoundError('the project has no pages')
        except (FileNotFoundError, ValueError) as e:
            X.set_project(prev)
            if not quiet: messagebox.showerror('Could not open', str(e), parent=self)
            return False
        self.pages = pages; self.edited = set(edited); self.dirty = False; self.undo = []
        self.clk = X.clock()
        self.ids = sorted(self.pages); self.cur = '100' if '100' in self.pages else self.ids[0]
        self.ver = 0; self.cursor = [1, 0]; self.kpage = None
        self.title(X.TITLE + f'  —  {X.PROJ}')
        self.cs_var.set(X.CHARSET); self.cs2_var.set(X.CHARSET2 or ''); self.full_cache = {}
        self.fill_list(); self.open(self.cur); self.build_quality()
        self.status('Loaded: {0}, {1} pages'.format(src, len(self.ids)))
        st = X.load_json(GUI_STATE, {}) or {}
        st['last'] = X.PROJ
        try: json.dump(st, open(GUI_STATE, 'w', encoding='utf-8'), ensure_ascii=False)
        except OSError: pass
        GP.recent_add(GUI_STATE, X.PROJ, 'project')
        self.hide_start()
        return True

    # ---------- стартовый экран и недавние файлы
    def show_start(self):
        if self.start: self.start.destroy()
        self.start = GP.StartScreen(self, self, GP.recent_list(GUI_STATE))
        self.start.place(relx=0, rely=0, relwidth=1, relheight=1)
    def hide_start(self):
        if self.start: self.start.destroy(); self.start = None
    def open_recent(self, r):
        if r['kind'] == 'project':
            if self.can_leave(): self.load_project(r['path'])
        elif r['kind'] == 'vbi': self.open_vbi(r['path'])
        else: self.open_stream(r['path'])
    def fill_recent(self):
        self.rmenu.delete(0, 'end')
        rec = GP.recent_list(GUI_STATE)
        if not rec: self.rmenu.add_command(label='(empty)', state='disabled')
        for r in rec:
            self.rmenu.add_command(label='{0}   —   {1}'.format(os.path.basename(r['path']), os.path.dirname(r['path'])),
                                   command=lambda r=r: self.open_recent(r))
        self.rmenu.add_separator(); self.rmenu.add_command(label='Start screen', command=self.show_start)

    def set_charset2(self):
        X.set_charset2(self.cs2_var.get()); self.fill_list(); self.draw()
        self.status('second set (ESC): ' + (self.cs2_var.get() or 'none') + ' — saved in the project')

    def set_charset(self):
        """Основной набор символов проекта (как настройка декодера в decode-orc)."""
        X.set_charset(self.cs_var.get()); self.fill_list(); self.draw()
        self.status('character set: {0} — saved in the project; run Export to update the HTML'.format(X.CS.CHARSETS[X.CHARSET]))

    def can_leave(self):
        """Перед сменой проекта: предложить сохранить правки."""
        if self.cur: self.set_flofs()
        if not self.dirty: return True
        r = messagebox.askyesnocancel('Unsaved edits', 'Save changes to the current project?', parent=self)
        if r is None: return False
        if r: self.save()
        return True

    def menu_project(self):
        if not self.can_leave(): return
        d = filedialog.askdirectory(parent=self, title='Project folder (with pages.json)')
        if not d: return
        if not X.is_project(d): messagebox.showerror('Not a project', 'There is no pages.json in the folder {0}'.format(d), parent=self); return
        self.load_project(d)
    def menu_t42(self):
        if not self.can_leave(): return
        f = filedialog.askopenfilename(parent=self, title='Teletext stream .t42 / .t34 / .t33',
                                       filetypes=[('Teletext stream', '*.t42 *.t34 *.t33'), ('All files', '*.*')])
        if f: self.open_stream(f)
    def open_stream(self, f):
        GP.recent_add(GUI_STATE, f, 'stream')
        if f.lower().endswith('.t33'): self.open_t33(f); return
        size = os.path.getsize(f); k = 34 if f.lower().endswith('.t34') else 42
        if size < k or size % k:
            messagebox.showerror('Not a stream', 'The file size is not a multiple of {0} bytes — this is not a .t{1} packet stream.'.format(k, k), parent=self); return
        out = self.project_dir(f, '{0:,} packets'.format(size // k).replace(',', ' ') + (' (525 lines, 32-character rows)' if k == 34 else ''))
        if out: self.run_build('Building pages from ' + os.path.basename(f),
                               [os.path.join(X.PG, 'build_project.py'), '--t42', f, '--out', out], out)
    def menu_t33(self, f=None):
        f = f or filedialog.askopenfilename(parent=self, title='NABTS stream .t33 (ExtraVision, NBC Teletext)',
                                            filetypes=[('NABTS stream', '*.t33'), ('All files', '*.*')])
        if f: self.open_t33(f)
    def open_t33(self, f):
        """NABTS — другой сервис (страницы NAPLPS — рисунки, а не сетка 40×25), поэтому своё окно."""
        size = os.path.getsize(f)
        if size < 33 or size % 33:
            messagebox.showerror('Not a stream', 'The file size is not a multiple of 33 bytes — this is not a NABTS .t33 packet stream.', parent=self); return
        NabtsWindow(self, f)
    def menu_vbi(self):
        """Запись .vbi: формат и службы в строках определяются сами (pages/vbi_auto.py),
        всё найденное читается и открывается; вопросы — только если формат неизвестен."""
        if not self.can_leave(): return
        f = filedialog.askopenfilename(parent=self, title='VBI recording', filetypes=[('VBI recording', '*.vbi'), ('All files', '*.*')])
        if f: self.open_vbi(f)
    def open_vbi(self, f):
        GP.recent_add(GUI_STATE, f, 'vbi')
        args = [os.path.join(X.PG, 'vbi_auto.py'), f]
        rep = os.path.join(os.path.splitext(f)[0] + '_vbi', 'report.json')
        if os.path.exists(rep):                          # запись уже разбиралась
            when = time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(rep)))
            r = messagebox.askyesnocancel('Already decoded',
                                          '{0} was already decoded ({1}).\n\nYes — open the results\nNo — decode the recording again'.format(os.path.basename(f), when),
                                          parent=self)
            if r is None: return
            if r: self.vbi_results(f); return
            args.append('--again')
        Runner(self, 'Opening ' + os.path.basename(f), args, lambda: self.vbi_results(f))
    def vbi_results(self, f):
        rep = os.path.splitext(f)[0] + '_vbi'
        try: R = json.load(open(os.path.join(rep, 'report.json'), encoding='utf-8'))
        except (OSError, ValueError): R = {'format': None, 'results': []}
        if R.get('format') is None:
            self.vbi_manual(f); return
        for r in R['results']:                          # главное открывается сразу, остальное — из окна записи
            if r['kind'] in ('teletext_project', 't33') or r['service'] == 'Silent Radio':
                self.open_result(r)
        GP.RecordingWindow(self, R, self.open_result)
    def open_result(self, r):
        p = r['path']
        if r['kind'] == 'teletext_project' and X.is_project(p):
            if self.can_leave(): self.load_project(p)
        elif r['kind'] == 't33': self.open_t33(p)
        elif r['service'] == 'Silent Radio' and os.path.exists(os.path.join(os.path.dirname(p), 'packets.json')):
            GP.SignWindow(self, os.path.dirname(p))
        elif r['kind'] == 'html': webbrowser.open('file:///' + p.replace(os.sep, '/'))
        elif os.path.exists(p): os.startfile(p)
    def vbi_manual(self, f):
        """Формат не распознан: как раньше — bt8x8 с вопросом о числе строк."""
        size = os.path.getsize(f); lpf = 32
        if size % (lpf * 2048):
            lpf = simpledialog.askinteger('Recording format', 'The recording format was not recognised.\nHow many VBI lines of 2048 samples per frame?', parent=self, minvalue=1, maxvalue=64, initialvalue=32)
            if not lpf: return
        n = size // (lpf * 2048)
        out = self.project_dir(f, '{0} frames (~{1} min of recording); decoding will take about {2} min'.format(n, max(1, round(n / 25 / 60)), vbi_minutes(n)))
        if out: self.run_build('Decoding ' + os.path.basename(f),
                               [os.path.join(X.PG, 'decode_vbi.py'), f, '--out', out, '--lpf', str(lpf)], out)
    def project_dir(self, src, info):
        """Папка проекта рядом с исходным файлом. -> путь для сборки или None
        (если открыт готовый проект или пользователь отказался)."""
        out = os.path.splitext(src)[0] + '_teletext'
        if X.is_project(out):
            r = messagebox.askyesnocancel('Project already exists', 'A project has already been built for this file:\n{0}\n\nYes — open it\nNo — build it again'.format(out), parent=self)
            if r is None: return None
            if r: self.load_project(out); return None
            ed = os.path.join(out, 'pages_edited.json')
            if os.path.exists(ed):                       # правки не теряем: откладываем в сторону
                os.replace(ed, os.path.join(out, time.strftime('pages_edited.%Y%m%d-%H%M%S.json')))
        elif not messagebox.askokcancel('New project', '{0}: {1}.\n\nThe project will be created in the folder\n{2}\nThe source file is not changed.'.format(os.path.basename(src), info, out), parent=self):
            return None
        return out
    def run_build(self, title, args, out):
        def done():
            if X.is_project(out): self.load_project(out)
            else: messagebox.showerror('Build', 'The build finished, but no pages were produced.', parent=self)
        Runner(self, title, args, done)

    # ---------- интерфейс
    def build_ui(self):
        mb = tk.Menu(self); fm = tk.Menu(mb, tearoff=0); mb.add_cascade(label='File', menu=fm)
        fm.add_command(label='Open stream .t42 / .t34…', command=self.menu_t42)
        fm.add_command(label='Open NABTS .t33 (ExtraVision)…', command=self.menu_t33)
        fm.add_command(label='Open .vbi recording…', command=self.menu_vbi)
        fm.add_command(label='Open project (folder)…', command=self.menu_project)
        self.rmenu = tk.Menu(fm, tearoff=0, postcommand=self.fill_recent)
        fm.add_cascade(label='Recent', menu=self.rmenu)
        fm.add_separator()
        fm.add_command(label='Save', accelerator='Ctrl+S', command=self.save)
        fm.add_command(label='Export → output.t42 + HTML', command=self.export)
        fm.add_command(label='Export complete pages only → output_full.t42 + HTML', command=lambda: self.export(full=True))
        fm.add_command(label='Export subtitles .srt…', command=self.export_srt)
        fm.add_command(label='Open export folder', command=lambda: os.startfile(X.OUTDIR))
        fm.add_separator(); fm.add_command(label='Exit', command=self.on_close)
        cm = tk.Menu(mb, tearoff=0); mb.add_cascade(label='Character set', menu=cm)
        self.cs_var = tk.StringVar(value='latin')
        for key, name in X.CS.CHARSETS.items():
            cm.add_radiobutton(label=name, value=key, variable=self.cs_var, command=self.set_charset)
        cm.add_separator(); cm.add_command(label='Second set (ESC code):', state='disabled')
        self.cs2_var = tk.StringVar(value='')
        for key, name in [('', 'none'), ('latin', 'Latin'), ('cyr2', 'Cyrillic (Russian)'),
                          ('cyr1', 'Cyrillic (Serbian)'), ('cyr3', 'Cyrillic (Ukrainian)')]:
            cm.add_radiobutton(label='   ' + name, value=key, variable=self.cs2_var, command=self.set_charset2)
        self.config(menu=mb)
        top = ttk.Frame(self, padding=(8, 6)); top.pack(fill='x')
        ttk.Button(top, text='Save (Ctrl+S)', command=self.save).pack(side='left')
        ttk.Button(top, text='Export → output.t42 + HTML', command=self.export).pack(side='left', padx=6)
        ttk.Button(top, text='Undo (Ctrl+Z)', command=self.do_undo).pack(side='left')
        ttk.Button(top, text='Open html/index.html', command=self.open_html).pack(side='left', padx=6)
        self.msg = ttk.Label(top, foreground=GP.MUTED); self.msg.pack(side='left', padx=12)

        pan = ttk.PanedWindow(self, orient='horizontal'); pan.pack(fill='both', expand=True)
        # список страниц
        left = ttk.Frame(pan, padding=(8, 0)); pan.add(left, weight=1)
        self.flt = tk.StringVar(); self.flt.trace_add('write', lambda *a: self.fill_list())
        e = ttk.Entry(left, textvariable=self.flt); e.pack(fill='x', pady=(0, 4))
        ttk.Label(left, text='search by number or title', foreground=GP.DIM).pack(anchor='w')
        cols = ('p', 't', 'tx', 'v', 'n')
        self.tree = ttk.Treeview(left, columns=cols, show='headings', selectmode='browse')
        for c, t, w in zip(cols, ('Page', 'Section', 'Received', 'Versions', 'Note'), (44, 190, 62, 54, 80)):
            self.tree.heading(c, text=t, command=lambda c=c: self.sort_list(c))
            self.tree.column(c, width=w, anchor='e' if c in ('tx', 'v') else 'w', stretch=c == 't')
        self.tree.tag_configure('svc', foreground=GP.ACCENT); self.tree.tag_configure('edit', foreground='#5aa9ff')
        self.tree.tag_configure('del', foreground='#999')
        sb = ttk.Scrollbar(left, command=self.tree.yview); self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side='left', fill='both', expand=True); sb.pack(side='left', fill='y')
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.tree.selection() and self.open(self.tree.selection()[0]))
        self.sort_key = 'p'
        self.pan = pan

        nb = ttk.Notebook(pan); pan.add(nb, weight=4); self.nb = nb
        # вкладка «Страница»
        pg = ttk.Frame(nb, padding=8); nb.add(pg, text='Page')
        bar = ttk.Frame(pg); bar.pack(fill='x')
        ttk.Button(bar, text='◀ page', width=7, command=lambda: self.step(-1)).pack(side='left')
        self.pnum = tk.StringVar(); pe = ttk.Entry(bar, textvariable=self.pnum, width=5, justify='center')
        pe.pack(side='left', padx=4); pe.bind('<Return>', lambda e: self.open(self.pnum.get().strip()))
        ttk.Button(bar, text='page ▶', width=7, command=lambda: self.step(1)).pack(side='left')
        ttk.Separator(bar, orient='vertical').pack(side='left', fill='y', padx=10)
        ttk.Button(bar, text='◀', width=3, command=lambda: self.step_ver(-1)).pack(side='left')
        self.vinfo = ttk.Label(bar, width=58, anchor='center'); self.vinfo.pack(side='left')
        ttk.Button(bar, text='▶', width=3, command=lambda: self.step_ver(1)).pack(side='left')
        self.reveal = tk.BooleanVar(); ttk.Checkbutton(bar, text='concealed text', variable=self.reveal, command=self.draw).pack(side='left', padx=10)
        self.fullv = tk.BooleanVar(); ttk.Checkbutton(bar, text='complete page', variable=self.fullv,
                                                      command=lambda: (setattr(self, 'ver', 0), self.draw())).pack(side='left')
        self.tv = tk.BooleanVar(); ttk.Checkbutton(bar, text='as on a TV screen', variable=self.tv, command=self.draw).pack(side='left', padx=10)
        self.flash_on = True; self.flash_job = None
        self.note = ttk.Label(pg, foreground=GP.ACCENT); self.note.pack(anchor='w', pady=(4, 0))

        box = ttk.Frame(pg); box.pack(fill='both', expand=True, pady=4)
        self.screen = tk.Label(box, bd=0, takefocus=1, cursor='xterm', highlightthickness=2, highlightcolor=GP.ACCENT, highlightbackground=GP.BG, bg=GP.BG)
        self.screen.place(relx=0.5, rely=0.5, anchor='center')
        self.fit = (480, 500)                          # размер экрана подстраивается под окно
        box.bind('<Configure>', lambda e: self.on_resize(e.width, e.height))
        self.screen.bind('<Button-1>', self.on_click); self.screen.bind('<Key>', self.on_key)

        keys = ttk.Frame(pg); keys.pack()
        ttk.Label(keys, text='Colour keys (FLOF):').pack(side='left', padx=(0, 6))
        self.kvars = []
        for i, (col, name) in enumerate(KEYS):
            v = tk.StringVar(); self.kvars.append(v)
            en = tk.Entry(keys, textvariable=v, width=5, justify='center', bg=col, fg='black', font=('Consolas', 11, 'bold'), relief='flat')
            en.pack(side='left', padx=2)
            en.bind('<Return>', lambda e, i=i: self.set_flof(i)); en.bind('<FocusOut>', lambda e, i=i: self.set_flof(i))
            en.bind('<Double-Button-1>', lambda e, v=v: self.open(v.get().strip()))
        ttk.Label(keys, text='double-click — go to', foreground=GP.DIM).pack(side='left', padx=6)

        acts = ttk.Frame(pg); acts.pack(pady=(8, 2))
        for t, f in (('Clear row', self.row_clear), ('Delete row', self.row_delete), ('Row from another version…', self.row_from),
                     ('Delete version', self.ver_delete), ('Copy version', self.ver_copy), ('Delete / restore page', self.page_toggle)):
            ttk.Button(acts, text=t, command=f).pack(side='left', padx=2)
        pal = ttk.Frame(pg); pal.pack(pady=4)
        names = ['', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white']
        for i in range(1, 8):
            tk.Button(pal, text='T', width=2, bg='#%02x%02x%02x' % COL[i], command=lambda i=i: self.insert(i)).pack(side='left', padx=1)
        ttk.Label(pal, text=' text   ').pack(side='left')
        for i in range(1, 8):
            tk.Button(pal, text='M', width=2, bg='#%02x%02x%02x' % COL[i], command=lambda i=i: self.insert(0x10 + i)).pack(side='left', padx=1)
        ttk.Label(pal, text=' mosaic').pack(side='left')
        pal2 = ttk.Frame(pg); pal2.pack()
        for t, code in (('Background = colour', 0x1d), ('Black background', 0x1c), ('Double height', 0x0d), ('Normal height', 0x0c),
                        ('Contiguous mosaic', 0x19), ('Separated mosaic', 0x1a), ('Concealed', 0x18)):
            ttk.Button(pal2, text=t, command=lambda c=code: self.insert(c)).pack(side='left', padx=1)
        self.stat = ttk.Label(pg, foreground=GP.MUTED); self.stat.pack(pady=(6, 0))
        ttk.Label(pg, foreground=GP.DIM, text='Click a character and type. Arrows — move, Backspace/Delete — erase, Enter — next row, PageUp/PageDown — neighbouring pages.').pack()

        # вкладка «Качество ленты»
        self.qframe = ttk.Frame(nb, padding=8); nb.add(self.qframe, text='Tape quality')

        self.bind_all('<Control-s>', lambda e: self.save()); self.bind_all('<Control-z>', lambda e: self.do_undo())
        self.bind_all('<Control-S>', lambda e: self.save()); self.bind_all('<Control-Z>', lambda e: self.do_undo())

    def build_quality(self):
        q = self.qframe
        for w in q.winfo_children(): w.destroy()
        Q = X.load_json(X.QUALITY)
        if not Q:
            ttk.Label(q, text='No quality map: it is built when a project is built from a .vbi recording.').pack(); return
        D = Q['minutes']
        ttk.Label(q, text='Tape quality per minute of {0} (broadcast time in brackets)'.format(X.NAME), font=('Segoe UI', 11, 'bold')).pack(anchor='w')
        row = ttk.Frame(q); row.pack(fill='x', pady=8)
        for d in D: d['missed'] = round(100 - d['received'], 2)
        metrics = [('missed', 'Teletext lines not received, %'), ('unreadable', 'Of these unreadable (signal present), %'),
                   ('parity', 'Parity errors, % of bytes')]
        self.qtip = tk.Label(q, bg=GP.FIELD, fg=GP.FG, relief='solid', bd=1, font=('Segoe UI', 9))
        for key, title in metrics:
            f = ttk.Frame(row); f.pack(side='left', fill='both', expand=True, padx=6)
            ttk.Label(f, text=title).pack(anchor='w')
            cv = tk.Canvas(f, width=200, height=180, bg=GP.PANEL, highlightthickness=0); cv.pack(fill='both', expand=True)
            cv.bind('<Configure>', lambda e, cv=cv, key=key, title=title: self.bar_chart(cv, D, key, title, e.width, e.height))
        cols = ('m', 'air', 'r', 'u', 'p')
        tv = ttk.Treeview(q, columns=cols, show='headings', height=12)
        for c, t in zip(cols, ('Recording minute', 'Broadcast', 'Lines received, %', 'Unreadable, %', 'Parity errors, %')):
            tv.heading(c, text=t); tv.column(c, width=150, anchor='e' if c != 'air' else 'w')
        for d in D:
            tv.insert('', 'end', values=(d['minute'], d.get('air', ''), f"{d['received']:.1f}", f"{d['unreadable']:.1f}", f"{(d['parity'] or 0):.3f}"))
        tv.pack(fill='x', pady=6)
        ttk.Label(q, foreground=GP.DIM, text='Received — share of teletext VBI lines that yielded a packet; unreadable — signal present but no decoder got a packet; errors — bytes with bad parity in received packets.').pack(anchor='w')

    def bar_chart(self, cv, D, key, title, W, H):
        cv.delete('all'); L, B, T, R = 40, 24, 10, 8
        vals = [d[key] or 0 for d in D]; n = len(vals); bw = (W - L - R) / n
        lo, hi = 0, (max(vals) * 1.15 or 1)            # столбики всегда от нуля
        y = lambda v: T + (H - T - B) * (1 - (v - lo) / (hi - lo))
        for i in range(5):
            v = lo + (hi - lo) * i / 4; yy = y(v)
            cv.create_line(L, yy, W - R, yy, fill=GP.LINE)
            cv.create_text(L - 4, yy, text=f'{v:.2f}' if hi < 2 else f'{v:.1f}', anchor='e', fill=GP.MUTED, font=('Segoe UI', 8))
        for i, v in enumerate(vals):
            x0 = L + i * bw + 1; x1 = L + (i + 1) * bw - 1
            it = cv.create_rectangle(x0, y(v), x1, H - B, fill=GP.ACCENT, outline='')
            cv.create_text((x0 + x1) / 2, H - B + 9, text=str(D[i]['minute']), fill=GP.MUTED, font=('Segoe UI', 8))
            hit = cv.create_rectangle(L + i * bw, T, L + (i + 1) * bw, H - B, fill='', outline='')
            for obj in (it, hit):
                cv.tag_bind(obj, '<Enter>', lambda e, d=D[i]: self.show_tip(e, 'minute {0} ({1})\n{2}: {3:.3f}'.format(d['minute'], d.get('air', ''), title, d[key] or 0)))
                cv.tag_bind(obj, '<Leave>', lambda e: self.qtip.place_forget())
        cv.create_line(L, H - B, W - R, H - B, fill=GP.MUTED)

    def show_tip(self, e, text):
        self.qtip.config(text=text)
        x = e.widget.winfo_rootx() - self.qtip.master.winfo_rootx() + e.x + 12
        y = e.widget.winfo_rooty() - self.qtip.master.winfo_rooty() + e.y - 30
        self.qtip.place(x=x, y=y)

    # ---------- список страниц
    def fill_list(self):
        if not hasattr(self, 'tree'): return
        f = self.flt.get().strip().lower()
        self.tree.delete(*self.tree.get_children())
        items = []
        for p in self.ids:
            pg = self.pages[p]; title = X.page_title(pg)
            if f and f not in p and f not in title.lower(): continue
            note = 'service' if pg.get('boxed') else ''
            if pg.get('deleted'): note = 'deleted'
            items.append((p, title, pg.get('tx', 0), len(pg['versions']), note))
        k = {'p': 0, 't': 1, 'tx': 2, 'v': 3, 'n': 4}[self.sort_key]
        items.sort(key=lambda r: r[k], reverse=k in (2, 3))
        for it in items:
            pg = self.pages[it[0]]
            tag = 'del' if pg.get('deleted') else 'edit' if it[0] in self.edited else 'svc' if pg.get('boxed') else ''
            self.tree.insert('', 'end', iid=it[0], values=it, tags=(tag,))
    def sort_list(self, c): self.sort_key = c; self.fill_list()

    # ---------- навигация
    def open(self, p):
        if p not in self.pages: self.status('there is no page {0}'.format(p)); return
        self.set_flofs()                               # применить ввод в полях кнопок до переключения
        self.cur = p; self.ver = 0; self.cursor = [1, 0]
        if self.tree.exists(p) and self.tree.selection() != (p,):
            self.tree.selection_set(p); self.tree.see(p)
        self.draw(); self.screen.focus_set()
    def step(self, d):
        if not self.cur: return
        i = (self.ids.index(self.cur) + d) % len(self.ids); self.open(self.ids[i])
    def view(self):
        """Страница, как она показана: обычная или (галочка «полная страница») — по одной
        полной странице на подстраницу, собранной из всех версий (X.full_versions)."""
        pg = self.pages[self.cur]
        if not self.fullv.get(): return pg
        key = (self.cur, id(pg), len(pg['versions']))
        if key not in self.full_cache: self.full_cache[key] = {**pg, 'versions': X.full_versions(pg)}
        return self.full_cache[key]
    def step_ver(self, d):
        if not self.cur: return
        n = len(self.view()['versions']); self.ver = (self.ver + d) % n; self.draw()
    def snap(self):
        V = self.view()['versions']; return V[min(self.ver, len(V) - 1)]
    def editable(self):
        if self.fullv.get():
            self.status('the complete page is assembled from versions — untick “complete page” to edit'); return False
        return True

    def on_resize(self, w, h):
        k = max(0.4, min((w - 8) / (40 * CW), (h - 8) / (25 * CH)))
        fit = (int(40 * CW * k), int(25 * CH * k))
        if fit != self.fit: self.fit = fit; self.draw()
    def draw(self, keep_flash=False):
        if not self.cur: return
        pg = self.view(); s = self.snap(); t = X.page_table(pg)
        if not keep_flash: self.flash_on = True
        im = render(s['rows'], X.fitted(s['rows'], X.overlay(pg, s), t), self.reveal.get(), self.cursor, t,
                    X.page_table2(pg), self.flash_on, self.tv.get())
        if self.flash_job: self.after_cancel(self.flash_job); self.flash_job = None
        if LAST['flash']:                              # мигание 0,75 Гц: 3/4 видно, 1/4 скрыто
            self.flash_job = self.after(1000 if self.flash_on else 333, self.flash_tick)
        if im.size != self.fit: im = im.resize(self.fit, Image.LANCZOS)
        self.photo = ImageTk.PhotoImage(im); self.screen.config(image=self.photo)
        self.pnum.set(self.cur)
        self.vinfo.config(text=X.version_label(pg, min(self.ver, len(pg['versions']) - 1), self.clk))
        if not LAST['box'] and self.tv.get(): self.status('this page has no boxes — on a TV screen its text is not shown')
        note = X.service_note(self.cur, pg)
        if pg.get('deleted'): note = 'page deleted (will not be exported)'
        self.note.config(text=note)
        F = pg.get('flof') or []
        for i, v in enumerate(self.kvars): v.set(F[i] if i < len(F) and F[i] else '')
        self.kpage = self.cur                          # страница, к которой относятся поля кнопок
        r, c = self.cursor; b = s['rows'].get(str(r))
        self.stat.config(text='row {0}, column {1}'.format(r, c) + (', code 0x{0:02x}'.format(b[c]) if b else ', no row') +
                         ' · received {0} times, subpages {1}'.format(pg.get('tx', 0), pg.get('subpages', 1)))

    def flash_tick(self):
        self.flash_job = None; self.flash_on = not self.flash_on; self.draw(keep_flash=True)

    # ---------- правка
    def push(self):
        self.undo.append((self.cur, copy.deepcopy(self.pages[self.cur])))
        del self.undo[:-300]
    def touch(self):
        self.full_cache = {}
        self.dirty = True; self.edited.add(self.cur); self.status('there are unsaved changes')
        if self.tree.exists(self.cur): self.tree.item(self.cur, tags=('edit',))
    def row(self, r):
        s = self.snap()
        if str(r) not in s['rows']: s['rows'][str(r)] = [0x20] * 40
        return s['rows'][str(r)]
    def set_char(self, code):
        if not self.editable(): return False
        r, c = self.cursor
        if r == 0 and c < 8: self.status('the first 8 positions of the header are filled automatically'); return False
        self.push(); self.row(r)[c] = code; self.touch(); return True
    def move(self, dr, dc):
        r, c = self.cursor; c += dc; r += dr
        if c > 39: c = 0; r += 1
        if c < 0: c = 39; r -= 1
        self.cursor = [max(0, min(24, r)), c]; self.draw()
    def on_click(self, e):
        if not self.cur: return
        self.screen.focus_set()
        self.cursor = [min(24, int(e.y * 25 / self.fit[1])), min(39, int(e.x * 40 / self.fit[0]))]; self.draw()
    def on_key(self, e):
        if not self.cur: return
        k = e.keysym
        if e.state & 0x4: return                       # Ctrl+… обрабатываются отдельно
        if k == 'Left': self.move(0, -1)
        elif k == 'Right': self.move(0, 1)
        elif k == 'Up': self.move(-1, 0)
        elif k == 'Down': self.move(1, 0)
        elif k == 'Home': self.cursor[1] = 0; self.draw()
        elif k == 'End': self.cursor[1] = 39; self.draw()
        elif k == 'Return': self.cursor = [min(24, self.cursor[0] + 1), 0]; self.draw()
        elif k == 'Prior': self.step(-1)
        elif k == 'Next': self.step(1)
        elif k == 'BackSpace': self.move(0, -1); self.set_char(0x20); self.draw()
        elif k == 'Delete': self.set_char(0x20); self.draw()
        elif e.char and len(e.char) == 1 and e.char.isprintable():
            ch = e.char
            code = X.CS.reverse(X.page_table(self.pages[self.cur])).get(ch)
            if code is None: self.status("the character “{0}” is not in this page's character set".format(ch)); return 'break'
            if self.set_char(code): self.move(0, 1)
        else: return
        return 'break'
    def insert(self, code):
        if not self.cur: return
        if self.set_char(code): self.move(0, 1)
        self.screen.focus_set()
    def row_clear(self):
        if not self.cur: return
        if not self.editable(): return False
        if self.cursor[0] == 0: return
        self.push(); self.snap()['rows'][str(self.cursor[0])] = [0x20] * 40; self.touch(); self.draw()
    def row_delete(self):
        if not self.cur: return
        if not self.editable(): return False
        if self.cursor[0] == 0: self.status('the header cannot be deleted'); return
        self.push(); self.snap()['rows'].pop(str(self.cursor[0]), None); self.touch(); self.draw()
    def row_from(self):
        if not self.cur: return
        if not self.editable(): return False
        V = self.pages[self.cur]['versions']
        v = simpledialog.askinteger('Row from another version', 'Version number (1–{0}) to take row {1} from:'.format(len(V), self.cursor[0]), parent=self, minvalue=1, maxvalue=len(V))
        if not v: return
        src = V[v - 1]['rows'].get(str(self.cursor[0]))
        if not src: self.status('that version does not have this row'); return
        self.push(); self.snap()['rows'][str(self.cursor[0])] = list(src); self.touch(); self.draw()
    def ver_delete(self):
        if not self.cur: return
        if not self.editable(): return False
        V = self.pages[self.cur]['versions']
        if len(V) < 2: self.status('the last version cannot be deleted — delete the page instead'); return
        self.push(); V.pop(min(self.ver, len(V) - 1)); self.ver = max(0, self.ver - 1); self.touch(); self.draw()
    def ver_copy(self):
        if not self.cur: return
        if not self.editable(): return False
        self.push(); V = self.pages[self.cur]['versions']; V.insert(self.ver + 1, copy.deepcopy(self.snap())); self.ver += 1; self.touch(); self.draw()
    def page_toggle(self):
        if not self.cur: return
        self.push(); pg = self.pages[self.cur]; pg['deleted'] = not pg.get('deleted'); self.touch(); self.fill_list(); self.draw()
    def set_flofs(self):
        if not self.cur: return
        for i in range(5): self.set_flof(i)
    def set_flof(self, i):
        p = getattr(self, 'kpage', None)
        if p is None or p not in self.pages: return
        v = self.kvars[i].get().strip(); pg = self.pages[p]
        F = list(pg.get('flof') or [None] * 5); F += [None] * (5 - len(F))
        if v and not (len(v) == 3 and v.isdigit() and v[0] in '12345678'):
            self.status('page number: three digits, 100–899'); self.kvars[i].set(F[i] or ''); return
        if (F[i] or '') == v: return
        self.undo.append((p, copy.deepcopy(pg))); F[i] = v or None; pg['flof'] = F if any(F) else None
        self.dirty = True; self.edited.add(p); self.status('there are unsaved changes')
    def do_undo(self):
        if not self.undo: self.status('nothing to undo'); return
        p, data = self.undo.pop(); self.pages[p] = data
        if p != self.cur: self.open(p)
        else: self.draw()
        self.touch()

    # ---------- сохранение и экспорт
    def status(self, t): self.msg.config(text=t)
    def save(self):
        if not self.cur: return
        X.save_state(self.pages, self.edited); self.dirty = False
        self.status('saved to {0} (pages changed: {1})'.format(X.rel(X.EDITED), len(self.edited)))
    def export(self, full=False):
        if not self.cur: return
        tag = '_full' if full else ''
        what = ' (complete pages only — one per subpage)' if full else ''
        if not messagebox.askyesno('Export', 'Write output{0}.t42, output{1}.html and the folder html{2}/{3} to\n{4}?\nExisting files will be replaced.'.format(tag, tag, tag, what, X.OUTDIR), parent=self): return
        if self.edited: self.save()
        self.status('exporting…'); self.config(cursor='watch')
        pages = copy.deepcopy(self.pages)
        def work():
            try: res = X.export_all(pages, full)
            except Exception as ex: res = 'export error: {0}: {1}'.format(type(ex).__name__, ex)
            self.after(0, lambda: (self.status(res), self.config(cursor='')))
        threading.Thread(target=work, daemon=True).start()
    def export_srt(self):
        if not self.cur: return
        """Субтитры со страницы телетекста (как export_subtitles в decode-orc): титр — переданная
        страница с флагом C6, снимается заголовком с C4 или без C6; время — по полям записи."""
        import subtitles as SUB
        try: c6 = SUB.subtitle_pages(X.STREAM) if X.STREAM and os.path.exists(X.STREAM) else {}
        except Exception: c6 = {}
        hint = ('Pages with the “subtitle” flag (C6): ' + ', '.join(f'{k} ({v})' for k, v in list(c6.items())[:6])
                if c6 else 'The stream has no pages with the “subtitle” flag (C6).')
        page = simpledialog.askstring('Subtitles .srt', hint + '\n\nSubtitle page number:',
                                      initialvalue=next(iter(c6), '888'), parent=self)
        if not page: return
        page = page.strip().upper()
        if len(page) != 3 or page[0] not in '12345678' or any(ch not in '0123456789ABCDEF' for ch in page[1:]):
            messagebox.showerror('Subtitles .srt', 'Page number: three characters, the first is the magazine 1–8 (e.g. 888).', parent=self); return
        self.status('subtitles from page {0}…'.format(page)); self.config(cursor='watch')
        def work():
            try:
                path, k = SUB.export(page)
                res = 'subtitles: {0} captions from page {1} → {2}'.format(k, page, X.rel(path)) if k else \
                      'page {0} has no subtitles (no transmitted pages with the C6 flag) — an empty {1} was written'.format(page, X.rel(path))
            except Exception as ex: res = 'subtitle export error: {0}: {1}'.format(type(ex).__name__, ex)
            self.after(0, lambda: (self.status(res), self.config(cursor='')))
        threading.Thread(target=work, daemon=True).start()
    def open_html(self):
        if not self.cur: return
        p = os.path.join(X.OUTDIR, 'html', 'index.html')
        if os.path.exists(p): os.startfile(p)
        else: self.status('html/index.html does not exist yet — run Export first')
    def on_close(self):
        if self.dirty:
            r = messagebox.askyesnocancel('Exit', 'Save changes before exiting?', parent=self)
            if r is None: return
            if r: self.save()
        self.destroy()

# ------------------------------------------------------------------ сборка в фоне
def vbi_minutes(frames):
    """Оценка времени декодирования .vbi: видеокарта ~0,01 с на кадр + ~1 мин подготовки
    (дольше, если запись незнакомая и дочитывается декодером decode-orc); процессор ~0,4 с на кадр."""
    try:
        import vbidecode_gpu
        gpu = vbidecode_gpu.available()
    except Exception:
        gpu = False
    return max(1, round((frames * 0.012 + 60) / 60 if gpu else frames * 0.4 / 60))

class ReportWindow(tk.Toplevel):
    """Что найдено в записи .vbi (отчёт pages/vbi_auto.py)."""
    def __init__(self, app, name, text):
        super().__init__(app); self.title('What is in ' + name); self.geometry('760x480')
        t = tk.Text(self, font=('Consolas', 9), wrap='none'); t.pack(fill='both', expand=True)
        t.insert('end', text); t.config(state='disabled')
        ttk.Button(self, text='Close', command=self.destroy).pack(anchor='e', padx=8, pady=6)

class Runner(tk.Toplevel):
    """Окно хода сборки: запускает скрипт, показывает строки STEP/PROGRESS; «Отмена» — остановить."""
    def __init__(self, app, title, args, on_done):
        super().__init__(app); self.title(title); self.geometry('640x360'); self.transient(app)
        self.on_done = on_done; self.q = queue.Queue(); self.finished = False
        f = ttk.Frame(self, padding=10); f.pack(fill='both', expand=True)
        self.stepl = ttk.Label(f, text='starting…', font=('Segoe UI', 10, 'bold')); self.stepl.pack(anchor='w')
        self.bar = ttk.Progressbar(f, mode='indeterminate'); self.bar.pack(fill='x', pady=6); self.bar.start(15)
        self.eta = ttk.Label(f, foreground=GP.MUTED); self.eta.pack(anchor='w')
        self.log = tk.Text(f, height=12, font=('Consolas', 9), wrap='word'); self.log.pack(fill='both', expand=True, pady=6)
        self.btn = ttk.Button(f, text='Cancel', command=self.cancel); self.btn.pack(anchor='e')
        self.protocol('WM_DELETE_WINDOW', self.cancel)
        env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUNBUFFERED='1')
        self.proc = subprocess.Popen([sys.executable, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding='utf-8', errors='replace', env=env,
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        threading.Thread(target=self.reader, daemon=True).start()
        self.after(100, self.poll)
    def reader(self):
        for line in self.proc.stdout: self.q.put(line.rstrip())
        self.q.put(None)
    def poll(self):
        if self.finished: return
        try:
            while True:
                line = self.q.get_nowait()
                if line is None: self.finish(self.proc.wait()); return
                self.handle(line)
        except queue.Empty: pass
        self.after(100, self.poll)
    def handle(self, line):
        if line.startswith('PROGRESS '):
            p = line.split(maxsplit=3)
            try: done, tot = int(p[1]), int(p[2])
            except (IndexError, ValueError): return
            if str(self.bar['mode']) != 'determinate': self.bar.stop(); self.bar.config(mode='determinate')
            self.bar.config(maximum=max(1, tot), value=done)
            el = time.time() - getattr(self, 't_step', time.time())
            left = el * (tot - done) / done if done else 0
            self.eta.config(text='{0:.0%}   ·   {1} of {2}   ·   {3:.0f} s elapsed{4}  '.format(
                done / max(1, tot), done, tot, el, ', about {0:.0f} s left'.format(left) if done and el > 3 else '')
                + (p[3] if len(p) > 3 else ''))
        elif line.startswith('STEP '):
            self.t_step = time.time()
            self.stepl.config(text=line[5:]); self.eta.config(text='')
            self.bar.config(mode='indeterminate', value=0); self.bar.start(15); self.add(line[5:])
        else: self.add(line)
    def add(self, t): self.log.insert('end', t + '\n'); self.log.see('end')
    def finish(self, rc):
        self.finished = True; self.bar.stop()
        if rc == 0:
            self.destroy(); self.on_done()
        else:
            self.stepl.config(text='error (code {0}) — details below'.format(rc))
            self.btn.config(text='Close', command=self.destroy); self.protocol('WM_DELETE_WINDOW', self.destroy)
    def cancel(self):
        if self.finished: self.destroy(); return
        if not messagebox.askyesno('Cancel', 'Stop the build?', parent=self): return
        self.finished = True
        if sys.platform == 'win32':                    # вместе с рабочими процессами декодера
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], capture_output=True,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        else: self.proc.kill()
        self.destroy()

# ------------------------------------------------------------------ NABTS (.t33)
NABTS_GRIDS = {'256 × 200 (as the receiver)': (256, 200), '512 × 400': (512, 400), '768 × 600': (768, 600)}

class NabtsWindow(tk.Toplevel):
    """Поток NABTS .t33 (CBS ExtraVision, NBC Teletext): каталог записей и страницы NAPLPS.
    Разбор — pages/nabts.py (порт decode-orc nabts_sink)."""
    def __init__(self, app, path):
        super().__init__(app); self.path = path
        self.title('NABTS — ' + os.path.basename(path)); self.geometry('1300x860')
        self.recs = []; self.shown = None; self.img = None; self.pimg = None; self.q = queue.Queue()
        self.player = None; self.job = None; self.t0 = 0.0; self.T = None; self.key = None
        top = ttk.Frame(self, padding=(8, 6)); top.pack(fill='x')
        ttk.Label(top, text='Receiver:').pack(side='left')
        self.grid_var = tk.StringVar(value=next(iter(NABTS_GRIDS)))
        cb = ttk.Combobox(top, textvariable=self.grid_var, values=list(NABTS_GRIDS), state='readonly', width=24)
        cb.pack(side='left', padx=4); cb.bind('<<ComboboxSelected>>', lambda e: self.reinterpret())
        self.play_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text='Draw as on screen', variable=self.play_var,
                        command=self.show).pack(side='left', padx=(12, 4))
        ttk.Button(top, text='▶ Replay', command=self.show).pack(side='left')
        ttk.Button(top, text='Save PNG…', command=self.save_png).pack(side='left', padx=(12, 4))
        ttk.Button(top, text='Save all pages (PNG + text)…', command=self.save_all).pack(side='left')
        ttk.Button(top, text='Save all pages as HTML…', command=self.save_all_html).pack(side='left', padx=4)
        self.msg = ttk.Label(top, foreground=GP.MUTED); self.msg.pack(side='left', padx=12)
        pan = ttk.PanedWindow(self, orient='horizontal'); pan.pack(fill='both', expand=True)
        lf = ttk.Frame(pan); pan.add(lf, weight=1)
        cols = ('rec', 'type', 'seen', 'flags', 'title')
        self.tree = ttk.Treeview(lf, columns=cols, show='headings', selectmode='browse')
        for c, t, w in zip(cols, ('Record', 'Type', 'Received', 'Flags', 'Title'), (150, 70, 60, 120, 260)):
            self.tree.heading(c, text=t); self.tree.column(c, width=w, stretch=(c == 'title'))
        sb = ttk.Scrollbar(lf, command=self.tree.yview); self.tree.config(yscrollcommand=sb.set)
        self.tree.pack(side='left', fill='both', expand=True); sb.pack(side='left', fill='y')
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.show())
        rf = ttk.Frame(pan); pan.add(rf, weight=2)
        self.cv = tk.Canvas(rf, bg='#202020', highlightthickness=0); self.cv.pack(fill='both', expand=True)
        self.cv.bind('<Configure>', lambda e: self.draw())
        self.info = ttk.Label(rf, foreground='#333', padding=(6, 2)); self.info.pack(fill='x')
        self.text = tk.Text(rf, height=12, font=('Consolas', 10), wrap='none'); self.text.pack(fill='x')
        self.protocol('WM_DELETE_WINDOW', self.close)
        self.status('parsing the stream…')
        threading.Thread(target=self.load, daemon=True).start()
        self.after(100, self.poll)

    def status(self, t): self.msg.config(text=t)

    def load(self):
        try:
            import nabts
            recs, summ = nabts.read_t33(self.path, lambda i, n: self.q.put(('progress', i, n)))
            nabts.interpret(recs, NABTS_GRIDS[self.grid_var.get()])
            self.q.put(('done', recs, summ))
        except Exception as e:
            self.q.put(('error', repr(e)))

    def poll(self):
        try:
            while True:
                m = self.q.get_nowait()
                if m[0] == 'progress':
                    self.status('parsing the stream… {0}%'.format(100 * m[1] // max(1, m[2])))
                elif m[0] == 'error':
                    self.status('error'); messagebox.showerror('NABTS', m[1], parent=self); return
                else:
                    self.loaded(m[1], m[2]); return
        except queue.Empty:
            pass
        self.after(100, self.poll)

    def loaded(self, recs, summ):
        import nabts
        self.recs = recs
        g = summ['groups']
        pages = sum(1 for r in recs if r.page)
        self.status('packets {0:,}, groups assembled {1:,}, records {2}, pages {3}'.format(summ['packets'], g['complete'], len(recs), pages).replace(',', ' '))
        if summ['foreign']:
            self.status(self.msg['text'] + ';  not teletext: ' + ', '.join(
                'channel {0:03X} type {1} ({2})'.format(ch, t, n) for (ch, t), n in summ['foreign'].items()))
        tnames = {0: 'page', 1: 'one-off', 2: 'application', 3: 'priority'}
        for i, r in enumerate(recs):
            title = next((l.strip() for l in (getattr(r, 'text', '') or '').split('\n') if l.strip()), '')
            if r.chain_pos:
                title = '[continuation {0} of {1}] '.format(r.chain_pos, nabts.address_text(r.chain_base)) + title
            if r.purpose:
                title = f'({r.purpose}) ' + title
            self.tree.insert('', 'end', iid=str(i), values=(nabts.record_label(r), tnames.get(r.type, r.type),
                                                           r.seen, nabts.flags_text(r), title))
        first = next((i for i, r in enumerate(recs) if r.page), 0)
        if recs:
            self.tree.selection_set(str(first)); self.tree.see(str(first))

    def reinterpret(self):
        if not self.recs: return
        import nabts
        self.status('redrawing…'); self.update_idletasks()
        nabts.interpret(self.recs, NABTS_GRIDS[self.grid_var.get()])
        self.status('receiver ' + self.grid_var.get()); self.show()

    def current(self):
        sel = self.tree.selection()
        return self.recs[int(sel[0])] if sel else None

    def page_image(self, r):
        import nabts
        return nabts.render_page(r.page, NABTS_GRIDS[self.grid_var.get()]) if r.page else None

    def close(self):
        self.stop(); self.destroy()

    def stop(self):
        if self.job: self.after_cancel(self.job); self.job = None

    def show(self):
        """Показ записи. Как на экране приёмника: страница рисуется по ходу программы
        NAPLPS (паузы WAIT, смена карты цветов), затем работают процессы мигания."""
        import nabts
        self.stop()
        r = self.current()
        if r is None: return
        self.player = nabts.Player(r.page, NABTS_GRIDS[self.grid_var.get()]) if r.page else None
        self.img = None
        if self.player:
            if self.play_var.get():
                self.t0 = time.monotonic(); self.T = 0.0
                self.player.advance(0.0)
            else:
                self.player.advance(None); self.T = self.player.end
                self.t0 = time.monotonic() - self.T
            self.key = None
            self.frame()
        self.text.delete('1.0', 'end')
        if r.page:
            self.text.insert('1.0', r.text)
        elif r.type == 2:
            self.text.insert('1.0', 'Application record (CEA-516 §7.2.2 functions), not a page:\n\n' +
                             bytes(b & 0x7F for b in r.data).decode('latin-1').replace('\r', '\n'))
        else:
            self.text.insert('1.0', 'The record has no data to display.')
        info = ('{0} · received {1} times, intact {2}'.format(nabts.record_label(r), r.seen, r.intact_n)
                + (', voted from {0} copies'.format(r.copies_voted) if r.copies_voted else '')
                + ' · {0} bytes'.format(len(r.data))
                + (' · on-screen display {0:.1f} s'.format(r.page.end) if r.page else ''))
        if r.chain_pos:
            info += ' · continuation #{0} of chain {1} (drawn over the previous ones)'.format(r.chain_pos, nabts.address_text(r.chain_base))
        self.info.config(text=info)
        self.draw()

    def frame(self):
        """Очередной кадр показа; пока страница рисуется — 20 кадров в секунду, потом
        только смены фазы мигания."""
        self.job = None
        pl = self.player
        if pl is None: return
        self.T = time.monotonic() - self.t0
        new = pl.advance(self.T)
        key = pl.blink_key(self.T)
        if new or key != self.key or self.img is None:
            self.key = key
            self.img = pl.image(self.T); self.draw()
        if not pl.done():
            self.job = self.after(50, self.frame)
        elif pl.has_blink:
            self.job = self.after(100, self.frame)

    def draw(self):
        self.cv.delete('all')
        if self.img is None: return
        W, H = self.cv.winfo_width(), self.cv.winfo_height()
        w = min(W, H * 4 // 3); h = w * 3 // 4                # экран 4:3, как у телевизора
        if w < 10: return
        self.pimg = ImageTk.PhotoImage(self.img.resize((w, h), Image.NEAREST))
        self.cv.create_image(W // 2, H // 2, image=self.pimg)

    def png_size(self):
        return (1536, 1152)                                  # как «Save PNG» в decode-orc

    def save_png(self):
        import nabts
        r = self.current()
        if r is None or not r.page:
            messagebox.showinfo('PNG', 'This record has no page.', parent=self); return
        f = filedialog.asksaveasfilename(parent=self, defaultextension='.png', filetypes=[('PNG', '*.png')],
                                         initialfile='%03X-%s-v%d.png' % (r.channel, r.addr_text, r.version))
        if f:
            self.page_image(r).resize(self.png_size(), Image.NEAREST).save(f); self.status('saved: ' + f)

    def save_all(self):
        import nabts
        d = filedialog.askdirectory(parent=self, title='Folder for pages',
                                    initialdir=os.path.dirname(self.path))
        if not d: return
        out = os.path.join(d, os.path.splitext(os.path.basename(self.path))[0] + '_nabts')
        os.makedirs(out, exist_ok=True)
        txt = []; n = 0
        for r in self.recs:
            name = '%03X-%s-v%d' % (r.channel, r.addr_text, r.version)
            txt.append('=== {0}  type {1}  received {2}  {3} {4}'.format(nabts.record_label(r), r.type, r.seen, nabts.flags_text(r), r.purpose))
            if r.page:
                self.page_image(r).resize(self.png_size(), Image.NEAREST).save(os.path.join(out, name + '.png'))
                txt.append(r.text); n += 1
                if n % 10 == 0: self.status('saved {0}…'.format(n)); self.update_idletasks()
            txt.append('')
        open(os.path.join(out, 'records.txt'), 'w', encoding='utf-8').write('\n'.join(txt))
        self.status('saved {0} pages to {1}'.format(n, out))
        os.startfile(out)

    def save_all_html(self):
        """Все страницы — каждая отдельным HTML (только экран с анимацией)."""
        import nabts_html
        if not any(r.page for r in self.recs):
            messagebox.showinfo('HTML', 'There are no pages to save.', parent=self); return
        d = filedialog.askdirectory(parent=self, title='Folder for the HTML pages',
                                    initialdir=os.path.dirname(self.path))
        if not d: return
        def prog(i, n):
            if i % 5 == 0 or i == n:
                self.status('HTML: {0} of {1} pages…'.format(i, n)); self.update_idletasks()
        n = nabts_html.export(self.recs, d, NABTS_GRIDS[self.grid_var.get()], prog)
        self.status('saved {0} pages to {1}'.format(n, d))
        os.startfile(d)

def run_script():
    """«teletext_gui скрипт.py аргументы» — выполнить инструмент из pages/. Так окно хода работы и
    vbi_auto запускают декодеры; в собранном Teletext Rescue.exe отдельного python нет —
    sys.executable указывает на сам .exe, и он выполняет скрипт здесь."""
    import runpy
    script = os.path.abspath(sys.argv[1])
    sys.argv = sys.argv[1:]
    sys.path.insert(0, os.path.dirname(script))
    if sys.stdout is None:                             # оконная сборка без консоли: вывод — в канал родителя
        try: sys.stdout = sys.stderr = open(1, 'w', encoding='utf-8', errors='replace', closefd=False)
        except OSError: sys.stdout = sys.stderr = open(os.devnull, 'w')
    runpy.run_path(script, run_name='__main__')


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()                   # рабочие процессы datacast в собранной версии
    if len(sys.argv) > 1 and sys.argv[1].lower().endswith('.py'):
        run_script(); sys.exit(0)
    if sys.platform == 'win32':
        try:                                           # чёткий вывод на экранах с масштабом 125–200%
            import ctypes; ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    App().mainloop()
