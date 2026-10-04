"""Оформление и окна программы: тёмная тема, стартовый экран, недавние файлы,
окно «Что в записи» (карта строк VBI и результаты), табло Silent Radio прямо в программе."""
import json, os, threading, time, webbrowser, tkinter as tk
from tkinter import ttk

# ------------------------------------------------------------------ тема
BG, PANEL, FIELD, LINE = '#1b1c1f', '#24262b', '#2d3036', '#3a3d44'
FG, MUTED, DIM = '#e3e3e3', '#a0a3a8', '#7a7d83'
ACCENT, ACCENT2, SEL = '#ffa31a', '#ffbe5c', '#5a3d10'
FONT = ('Segoe UI', 10)
SERVICE_COL = {                      # цвет службы на карте строк
    'teletext': '#3fa7ff', 'NABTS': '#7c6cff', 'Silent Radio': '#ffa31a', 'CC': '#3ccf7a',
    'datacast': '#ff5c8a', 'VPS': '#c8c84a', 'WSS': '#c8c84a', 'test': '#555a63', 'data': '#b07cff',
    'picture': '#3a3d44', 'empty': '#2a2c31'}


def service_key(kind):
    k = kind.lower()
    if k.startswith('encrypted datacast'): return 'datacast'
    if 'silent radio' in k: return 'Silent Radio'
    if k.startswith('nabts'): return 'NABTS'
    if 'wst' in k or 'teletext' in k: return 'teletext'
    if k.startswith('cc'): return 'CC'
    if k.startswith('vps'): return 'VPS'
    if k.startswith('wss'): return 'WSS'
    if k.startswith('test'): return 'test'
    if k.startswith('data'): return 'data'
    if k.startswith('empty'): return 'empty'
    return 'picture'


def dark_titlebar(win):
    """Тёмный заголовок окна Windows 10/11 (DWMWA_USE_IMMERSIVE_DARK_MODE); на других системах — ничего."""
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
        v = ctypes.c_int(1)
        for attr in (20, 19):                     # 20 — Windows 11 / 10 20H1+, 19 — старые сборки 10
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), 4) == 0:
                break
        try:                                      # цвет полосы заголовка (Windows 11)
            c = ctypes.c_int(0x1F1C1B)            # BGR от BG
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(c), 4)
        except Exception:
            pass
    except Exception:
        pass


def apply_theme(root):
    root.bind_class('Toplevel', '<Map>', lambda e: dark_titlebar(e.widget) if e.widget.winfo_toplevel() is e.widget else None, '+')
    root.after(50, lambda: dark_titlebar(root))
    st = ttk.Style(root)
    st.theme_use('clam')
    root.configure(bg=BG)
    st.configure('.', background=BG, foreground=FG, fieldbackground=FIELD, bordercolor=LINE,
                 lightcolor=PANEL, darkcolor=PANEL, troughcolor=PANEL, font=FONT,
                 selectbackground=SEL, selectforeground=FG, insertcolor=FG, focuscolor=ACCENT)
    st.configure('TFrame', background=BG)
    st.configure('Card.TFrame', background=PANEL)
    st.configure('TLabel', background=BG, foreground=FG)
    st.configure('Card.TLabel', background=PANEL, foreground=FG)
    st.configure('Muted.TLabel', foreground=MUTED)
    st.configure('CardMuted.TLabel', background=PANEL, foreground=MUTED)
    st.configure('Title.TLabel', font=('Segoe UI Semibold', 22), foreground=ACCENT)
    st.configure('H.TLabel', font=('Segoe UI Semibold', 12), foreground=FG)
    st.configure('CardH.TLabel', background=PANEL, font=('Segoe UI Semibold', 12), foreground=FG)
    st.configure('TButton', background=FIELD, foreground=FG, borderwidth=1, focusthickness=0, padding=(10, 5))
    st.map('TButton', background=[('pressed', SEL), ('active', LINE)], bordercolor=[('active', ACCENT)])
    st.configure('Accent.TButton', background=ACCENT, foreground='#1b1c1f', font=('Segoe UI Semibold', 11), padding=(16, 10))
    st.map('Accent.TButton', background=[('pressed', '#d98400'), ('active', ACCENT2)])
    st.configure('Big.TButton', font=('Segoe UI Semibold', 11), padding=(16, 10))
    st.configure('Link.TButton', background=PANEL, foreground=ACCENT2, borderwidth=0, padding=(4, 2), anchor='w')
    st.map('Link.TButton', background=[('active', LINE)])
    st.configure('TCheckbutton', background=BG, foreground=FG)
    st.map('TCheckbutton', background=[('active', BG)], indicatorcolor=[('selected', ACCENT), ('!selected', FIELD)])
    st.configure('TRadiobutton', background=BG, foreground=FG)
    st.configure('TEntry', fieldbackground=FIELD, foreground=FG, insertcolor=FG, padding=4)
    st.configure('TCombobox', fieldbackground=FIELD, background=FIELD, foreground=FG, arrowcolor=FG)
    st.map('TCombobox', fieldbackground=[('readonly', FIELD)], foreground=[('readonly', FG)])
    st.configure('TNotebook', background=BG, borderwidth=0, tabmargins=(8, 6, 8, 0))
    st.configure('TNotebook.Tab', background=PANEL, foreground=MUTED, padding=(16, 6), borderwidth=0)
    st.map('TNotebook.Tab', background=[('selected', BG)], foreground=[('selected', ACCENT)])
    st.configure('Treeview', background=PANEL, fieldbackground=PANEL, foreground=FG, rowheight=24, borderwidth=0)
    st.map('Treeview', background=[('selected', SEL)], foreground=[('selected', '#fff')])
    st.configure('Treeview.Heading', background=FIELD, foreground=MUTED, relief='flat', padding=(6, 4))
    st.map('Treeview.Heading', background=[('active', LINE)])
    st.configure('TScrollbar', background=FIELD, troughcolor=BG, arrowcolor=MUTED, borderwidth=0)
    st.configure('Horizontal.TProgressbar', background=ACCENT, troughcolor=PANEL, borderwidth=0, thickness=10)
    st.configure('TPanedwindow', background=BG)
    st.configure('Sash', sashthickness=6, background=LINE)
    st.configure('TSeparator', background=LINE)
    root.option_add('*Menu.background', PANEL); root.option_add('*Menu.foreground', FG)
    root.option_add('*Menu.activeBackground', SEL); root.option_add('*Menu.activeForeground', '#fff')
    root.option_add('*Menu.relief', 'flat'); root.option_add('*Menu.font', FONT)
    root.option_add('*Text.background', PANEL); root.option_add('*Text.foreground', FG)
    root.option_add('*Text.insertBackground', FG); root.option_add('*Text.relief', 'flat')
    root.option_add('*Listbox.background', PANEL); root.option_add('*Listbox.foreground', FG)
    root.option_add('*Toplevel.background', BG)
    root.option_add('*TCombobox*Listbox.background', PANEL); root.option_add('*TCombobox*Listbox.foreground', FG)


# ------------------------------------------------------------------ недавние файлы
def recent_list(state_path):
    try: d = json.load(open(state_path, encoding='utf-8'))
    except (OSError, ValueError): d = {}
    return [r for r in d.get('recent', []) if os.path.exists(r['path'])]


def recent_add(state_path, path, kind):
    try: d = json.load(open(state_path, encoding='utf-8'))
    except (OSError, ValueError): d = {}
    rec = [r for r in d.get('recent', []) if os.path.normcase(r['path']) != os.path.normcase(path)]
    rec.insert(0, {'path': path, 'kind': kind, 'time': time.strftime('%Y-%m-%d %H:%M')})
    d['recent'] = rec[:12]
    try: json.dump(d, open(state_path, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    except OSError: pass


# ------------------------------------------------------------------ стартовый экран
class StartScreen(ttk.Frame):
    """Пока проект не открыт: крупные действия, недавние файлы, что умеет программа."""
    def __init__(self, master, app, recent):
        super().__init__(master, padding=(40, 28, 40, 40))
        self.app = app
        top = ttk.Frame(self); top.pack(fill='x', pady=(0, 22))
        self.weather = LedBoard(top)
        self.weather.pack(side='right', anchor='n')
        head = ttk.Frame(top); head.pack(side='left', fill='x', expand=True, anchor='n')
        ttk.Label(head, text='Teletext Rescue', style='Title.TLabel').pack(anchor='w')
        ttk.Label(head, text='Teletext, NABTS, Silent Radio, captions and data\nhidden in the vertical blanking interval',
                  style='Muted.TLabel', justify='left').pack(anchor='w', pady=(0, 24))
        acts = ttk.Frame(head); acts.pack(anchor='w')
        ttk.Button(acts, text='📼  Open .vbi recording…', style='Accent.TButton', command=app.menu_vbi).pack(side='left')
        ttk.Button(acts, text='Open stream .t42 / .t34 / .t33…', style='Big.TButton', command=app.menu_t42).pack(side='left', padx=10)
        ttk.Button(acts, text='Open project…', style='Big.TButton', command=app.menu_project).pack(side='left')
        ttk.Label(head, text='A recording is examined automatically: the format and every VBI line are identified and '
                  'everything readable is decoded and opened.', style='Muted.TLabel', wraplength=560, justify='left').pack(anchor='w', pady=(10, 0))
        cols = ttk.Frame(self); cols.pack(fill='both', expand=True)
        rc = ttk.Frame(cols, style='Card.TFrame', padding=16); rc.pack(side='left', fill='both', expand=True, padx=(0, 12))
        ttk.Label(rc, text='Recent', style='CardH.TLabel').pack(anchor='w', pady=(0, 8))
        if not recent:
            ttk.Label(rc, text='Nothing yet — open a recording.', style='CardMuted.TLabel').pack(anchor='w')
        for r in recent[:8]:
            RecentCard(rc, r, lambda r=r: app.open_recent(r)).pack(fill='x', pady=3)
        sv = ttk.Frame(cols, style='Card.TFrame', padding=16); sv.pack(side='left', fill='both', expand=True)
        ttk.Label(sv, text='What it reads', style='CardH.TLabel').pack(anchor='w', pady=(0, 8))
        for key, text in (('teletext', 'WST teletext (PAL, 525-line) — pages, editor, HTML, subtitles'),
                          ('NABTS', 'NABTS / NAPLPS — CBS ExtraVision, NBC Teletext, animated pages'),
                          ('Silent Radio', 'Silent Radio — LED news sign service (line 21)'),
                          ('datacast', 'Encrypted datacast — packets, addresses, schedule'),
                          ('CC', 'CC captions (line 21)'),
                          ('test', 'Test signals (VITS), VPS / WSS detection')):
            row = ttk.Frame(sv, style='Card.TFrame'); row.pack(fill='x', pady=2)
            tk.Frame(row, bg=SERVICE_COL[key], width=12, height=12).pack(side='left', padx=(0, 8))
            ttk.Label(row, text=text, style='Card.TLabel').pack(side='left')
        ttk.Label(sv, text='\nFormats: bt8x8 PAL/NTSC, cx23885, 16-bit 4fsc (vhs-decode TBC)', style='CardMuted.TLabel').pack(anchor='w')


class RecentCard(tk.Frame):
    """Строка недавнего файла: значок вида, имя, папка и время; подсветка под мышью, щелчок — открыть."""
    ICON = {'vbi': ('📼', ACCENT), 'project': ('📄', '#3fa7ff'), 'stream': ('〰', '#7c6cff')}

    def __init__(self, master, r, command):
        super().__init__(master, bg=FIELD, cursor='hand2', padx=10, pady=6)
        ic, col = self.ICON.get(r['kind'], ('•', MUTED))
        tk.Frame(self, bg=col, width=4).pack(side='left', fill='y', padx=(0, 10))
        tk.Label(self, text=ic, bg=FIELD, fg=col, font=('Segoe UI Emoji', 14)).pack(side='left', padx=(0, 10))
        box = tk.Frame(self, bg=FIELD); box.pack(side='left', fill='x', expand=True)
        tk.Label(box, text=os.path.basename(r['path']), bg=FIELD, fg=FG, font=('Segoe UI Semibold', 10), anchor='w').pack(fill='x')
        tk.Label(box, text='{0}    {1}'.format(os.path.dirname(r['path']), r.get('time', '')), bg=FIELD, fg=DIM,
                 font=('Segoe UI', 8), anchor='w').pack(fill='x')
        self.all = [self] + list(self.winfo_children()) + list(box.winfo_children()) + [box]
        for w in self.all:
            w.bind('<Enter>', lambda e: self.paint(LINE)); w.bind('<Leave>', lambda e: self.paint(FIELD))
            w.bind('<Button-1>', lambda e: command())

    def paint(self, c):
        for w in self.all:
            if isinstance(w, tk.Frame) and w.cget('width') == 4: continue
            try: w.configure(bg=c)
            except tk.TclError: pass


class LedBoard(tk.Canvas):
    """Табло «время и температура» как в 80–90-е: янтарная точечная матрица 96×15, две строки
    шрифта 5×7, страницы сменяются шторкой (время и дата → город и температура со значком →
    погода, ветер, влажность), двоеточие часов мигает. Время — часы компьютера; погода — там,
    где сейчас компьютер (место по IP-адресу, wttr.in, без ключей), раз в 30 минут в фоне."""
    URL = 'https://wttr.in/?format=%l|%C|%t|%w|%h&m&lang=en'
    COLS, ROWS = 96, 15
    ON, OFF = '#ffb21e', '#2a1a08'
    EXTRA = {'°': [0x06, 0x09, 0x09, 0x06]}
    PAGE_S = 4.0

    # значки 13×13 (строки сверху вниз)
    ICONS = {
        'sun': ['......#......', '..#...#...#..', '...#.....#...', '.....###.....', '....#####....', '#..#######..#',
                '...#######...', '#..#######..#', '....#####....', '.....###.....', '...#.....#...', '..#...#...#..', '......#......'],
        'cloud': ['.............', '.............', '.....###.....', '...##...##...', '..#.......#..', '.##........#.',
                  '#...........#', '#...........#', '.###########.', '.............', '.............', '.............', '.............'],
        'partly': ['.#..#........', '..###........', '#######......', '..####.###...', '.#..##...##..', '...#.......#.',
                   '..#.........#', '.#..........#', '.###########.', '.............', '.............', '.............', '.............'],
        'rain': ['.....###.....', '...##...##...', '..#.......#..', '.#.........#.', '#...........#', '.###########.',
                 '.............', '..#...#...#..', '.#...#...#...', '.............', '...#...#...#.', '..#...#...#..', '.............'],
        'snow': ['.....###.....', '...##...##...', '..#.......#..', '.#.........#.', '#...........#', '.###########.',
                 '.............', '.#...#...#...', '..#...#...#.', '.#...#...#...', '.............', '...#...#...#.', '.............'],
        'storm': ['.....###.....', '...##...##...', '..#.......#..', '.#.........#.', '#...........#', '.#####.#####.',
                  '.....##......', '....##.......', '...#####.....', '.....##......', '....##.......', '....#........', '.............'],
        'fog': ['.............', '.............', '#############', '.............', '.###########.', '.............',
                '#############', '.............', '.###########.', '.............', '#############', '.............', '.............'],
    }

    def __init__(self, master, dot=4):
        super().__init__(master, width=self.COLS * dot + 24, height=self.ROWS * dot + 24, bg='#141414',
                         highlightthickness=2, highlightbackground='#3b3b3b')
        import silent_radio as SR
        self.SR = SR; self.dot = dot
        self.wx = None; self.page = 0; self.t_page = time.time(); self.shown = [0] * self.COLS; self.prev = [0] * self.COLS
        d = dot; o = 12
        self.create_rectangle(4, 4, self.COLS * d + 20, self.ROWS * d + 20, fill='#0a0703', outline='#000')
        self.dots = [[self.create_oval(o + x * d + 1, o + y * d + 1, o + x * d + d - 1, o + y * d + d - 1,
                                       fill=self.OFF, outline='') for y in range(self.ROWS)] for x in range(self.COLS)]
        threading.Thread(target=self.fetch, daemon=True).start()
        self.after(100, self.tick)

    def fetch(self):                                  # фоновый поток: только self.wx, окно не трогает
        import urllib.request
        while True:
            try:
                req = urllib.request.Request(self.URL, headers={'User-Agent': 'curl/8'})
                t = urllib.request.urlopen(req, timeout=20).read().decode('utf-8', 'replace').strip()
                p = (t.split('|') + [''] * 5)[:5]
                if not p[2]: raise ValueError(t)
                self.wx = {'place': p[0].split(',')[0].strip().upper(), 'cond': p[1].strip(), 'temp': p[2].strip(),
                           'wind': ''.join(c for c in p[3] if c.isalnum() or c in '/.'), 'hum': p[4].strip()}
                time.sleep(30 * 60)
            except Exception:
                time.sleep(120)

    # ---------- страницы
    def tcols(self, s):
        out = []
        for ch in s:
            out += list(self.EXTRA.get(ch) or self.SR.glyph(ch)) + [0]
        return out[:-1]

    def put_line(self, buf, s, row, x0=None, right=None):
        c = self.tcols(s)
        x = (self.COLS - len(c)) // 2 if x0 is None else x0
        if right is not None: x = right - len(c)
        for k, v in enumerate(c):
            if 0 <= x + k < self.COLS: buf[x + k] |= v << row

    def put_icon(self, buf, name, x0, y0=1):
        for y, line in enumerate(self.ICONS[name]):
            for x, ch in enumerate(line):
                if ch == '#' and 0 <= x0 + x < self.COLS and y0 + y < self.ROWS:
                    buf[x0 + x] |= 1 << (y0 + y)

    def icon_name(self, c):
        c = c.lower()
        if 'thunder' in c or 'storm' in c: return 'storm'
        if any(w in c for w in ('snow', 'sleet', 'ice', 'blizzard')): return 'snow'
        if any(w in c for w in ('rain', 'drizzle', 'shower')): return 'rain'
        if any(w in c for w in ('fog', 'mist', 'haze', 'smog', 'smoke')): return 'fog'
        if 'partly' in c or 'patchy' in c: return 'partly'
        if 'cloud' in c or 'overcast' in c: return 'cloud'
        return 'sun'

    def pages(self):
        return 4 if self.wx else 2

    def frame(self, page, colon):
        buf = [0] * self.COLS
        now = time.localtime()
        if page == 0:
            # двоеточие гаснет, но его столбцы остаются — цифры не сдвигаются
            hh, mm, dots = self.tcols(time.strftime('%H', now)), self.tcols(time.strftime('%M', now)), self.tcols(':')
            c = hh + [0] + (dots if colon else [0] * len(dots)) + [0] + mm
            x = (self.COLS - len(c)) // 2
            for k, v in enumerate(c): buf[x + k] |= v
            self.put_line(buf, time.strftime('%a %d %b', now).upper(), 8)
        elif page == self.pages() - 1:                 # название программы
            self.put_line(buf, 'TELETEXT', 0)
            self.put_line(buf, 'RESCUE', 8)
        elif page == 1:
            w = self.wx
            self.put_icon(buf, self.icon_name(w['cond']), 2)
            self.put_line(buf, w['place'][:12], 0, x0=19)
            self.put_line(buf, w['temp'].replace('+', ''), 8, x0=19)
        else:
            w = self.wx
            self.put_line(buf, w['cond'].upper()[:15], 0)
            self.put_line(buf, 'WIND {0}  {1}'.format(w['wind'].upper(), w['hum'])[:16], 8)
        return buf

    def tick(self):
        try:
            if not self.winfo_exists(): return
        except tk.TclError:
            return
        now = time.time()
        if now - self.t_page >= self.PAGE_S:
            self.prev = self.shown[:]; self.page = (self.page + 1) % self.pages(); self.t_page = now
        el = now - self.t_page
        target = self.frame(self.page % self.pages(), int(now * 2) % 2 == 0)
        k = self.COLS if el >= 0.5 else int(el / 0.5 * self.COLS)     # шторка слева направо
        cols = [target[x] if x < k else self.prev[x] for x in range(self.COLS)]
        for x in range(self.COLS):
            if cols[x] != self.shown[x]:
                d = cols[x] ^ self.shown[x]
                for y in range(self.ROWS):
                    if (d >> y) & 1:
                        self.itemconfigure(self.dots[x][y], fill=self.ON if (cols[x] >> y) & 1 else self.OFF)
                self.shown[x] = cols[x]
        self.after(40, self.tick)


# ------------------------------------------------------------------ окно «Что в записи»
class RecordingWindow(tk.Toplevel):
    """Карта строк VBI (какая служба где) и кнопки результатов (pages/vbi_auto.py report.json)."""
    def __init__(self, app, report, opener):
        super().__init__(app); self.configure(bg=BG)
        R = report; P = R.get('probe') or {}
        self.title('What is in ' + os.path.basename(R['file'])); self.geometry('980x640')
        f = ttk.Frame(self, padding=18); f.pack(fill='both', expand=True)
        ttk.Label(f, text=os.path.basename(R['file']), style='H.TLabel').pack(anchor='w')
        units = P.get('units', 0); unit = P.get('unit', 'field')
        secs = units / (59.94 if unit == 'field' else 29.97 if 'ntsc' in (R.get('format') or '') else 25)
        ttk.Label(f, text='{0} · {1} {2}s · {3:.0f} min {4:02.0f} s'.format(R.get('format'), units, unit, secs // 60, secs % 60),
                  style='Muted.TLabel').pack(anchor='w', pady=(0, 12))
        body = ttk.Frame(f); body.pack(fill='both', expand=True)
        # карта строк
        left = ttk.Frame(body, style='Card.TFrame', padding=12); left.pack(side='left', fill='both', expand=True)
        ttk.Label(left, text='VBI lines', style='CardH.TLabel').pack(anchor='w', pady=(0, 6))
        cv = tk.Canvas(left, bg=PANEL, highlightthickness=0); cv.pack(fill='both', expand=True)
        lines = [L for L in P.get('lines', [])]
        if R.get('vits_text'):
            ttk.Label(left, text='\n'.join(R['vits_text']), style='CardMuted.TLabel', justify='left').pack(side='bottom', anchor='w', pady=(8, 0))
        cv.bind('<Configure>', lambda e: self.draw_map(cv, lines, e.width))
        # результаты
        right = ttk.Frame(body, style='Card.TFrame', padding=12); right.pack(side='left', fill='y', padx=(12, 0))
        ttk.Label(right, text='Results', style='CardH.TLabel').pack(anchor='w', pady=(0, 8))
        if not R.get('results'):
            ttk.Label(right, text='Nothing the program can decode\nwas found in this recording.', style='CardMuted.TLabel').pack(anchor='w')
        for r in R.get('results', []):
            ttk.Button(right, text='▶  ' + r['service'], style='Big.TButton', command=lambda r=r: opener(r)).pack(fill='x', pady=3)
            ttk.Label(right, text=os.path.basename(r['path'].rstrip('\\/')) + ('  — ' + r['note'] if r.get('note') else ''),
                      style='CardMuted.TLabel').pack(anchor='w', pady=(0, 6))
        rep = os.path.join(os.path.splitext(R['file'])[0] + '_vbi', 'report.txt')
        if os.path.exists(rep):
            ttk.Button(right, text='Report (text)', command=lambda: os.startfile(rep)).pack(fill='x', pady=(12, 0))
        ttk.Button(right, text='Close', command=self.destroy).pack(side='bottom', fill='x')

    def draw_map(self, cv, lines, W):
        cv.delete('all')
        rows = {}
        for L in lines:
            rows.setdefault(L['tv_line'], []).append(L)
        y = 4; h = 26; x0 = 70
        for tvl in sorted(rows):
            Ls = sorted(rows[tvl], key=lambda L: (L['parity'] is None, L['parity'] or 0))
            cv.create_text(8, y + h / 2, text='line {0}'.format(tvl), anchor='w', fill=MUTED, font=('Segoe UI', 9))
            n = len(Ls); w = (W - x0 - 8) / max(1, n)
            for i, L in enumerate(Ls):
                key = service_key(L['kind'])
                x = x0 + i * w
                cv.create_rectangle(x + 1, y + 1, x + w - 2, y + h - 2, fill=SERVICE_COL[key], outline='')
                if L['kind'] != 'empty':
                    txt = L['kind'].split(' (')[0]
                    if L['parity'] is not None: txt = 'AB'[L['parity']] + ': ' + txt
                    if L.get('read') not in (None, 0): txt += '  · read {0:.0%}'.format(L['read'])
                    cv.create_text(x + 8, y + h / 2, text=txt, anchor='w', font=('Segoe UI', 9),
                                   fill='#111' if key in ('Silent Radio', 'CC', 'VPS', 'WSS', 'teletext') else FG)
            y += h
        cv.configure(scrollregion=(0, 0, W, y))


# ------------------------------------------------------------------ табло Silent Radio
class SignWindow(tk.Toplevel):
    """Проигрыватель табло (packets.json из pages/silent_radio.py) прямо в программе."""
    W, H, S = 112, 15, 8

    def __init__(self, app, folder):
        super().__init__(app); self.configure(bg=BG)
        self.S = max(4, min(8, (self.winfo_screenwidth() - 160) // self.W))   # точка табло по ширине экрана
        self.data = json.load(open(os.path.join(folder, 'packets.json'), encoding='utf-8'))
        self.title('Silent Radio — ' + self.data['source']); self.geometry('{0}x660'.format(self.W * self.S + 60))
        top = ttk.Frame(self, padding=(14, 10)); top.pack(fill='x')
        ttk.Label(top, text='Silent Radio', style='H.TLabel').pack(side='left')
        ttk.Label(top, text='  zone', style='Muted.TLabel').pack(side='left', padx=(16, 4))
        self.zone = tk.StringVar(value=sorted(self.data['zones'])[0])
        cb = ttk.Combobox(top, textvariable=self.zone, values=sorted(self.data['zones']), state='readonly', width=4)
        cb.pack(side='left'); cb.bind('<<ComboboxSelected>>', lambda e: self.select())
        self.pbtn = ttk.Button(top, text='⏸ Pause', command=self.toggle); self.pbtn.pack(side='left', padx=(16, 4))
        ttk.Button(top, text='⏮', width=3, command=lambda: self.go(self.it - 1)).pack(side='left')
        ttk.Button(top, text='⏭', width=3, command=lambda: self.go(self.it + 1)).pack(side='left', padx=4)
        ttk.Button(top, text='Open in browser', command=lambda: webbrowser.open(
            'file:///' + os.path.join(folder, 'index.html').replace(os.sep, '/'))).pack(side='right')
        frame = tk.Frame(self, bg='#111', padx=10, pady=10); frame.pack(padx=14)
        self.cv = tk.Canvas(frame, width=self.W * self.S, height=self.H * self.S, bg='#000', highlightthickness=0)
        self.cv.pack()
        self.dots = [[self.cv.create_oval(x * self.S + 1, y * self.S + 1, x * self.S + self.S - 1, y * self.S + self.S - 1,
                                          fill='#2a1606', outline='') for y in range(self.H)] for x in range(self.W)]
        self.cap = ttk.Label(self, text='', foreground=ACCENT2, wraplength=960, padding=(14, 6)); self.cap.pack(fill='x')
        lf = ttk.Frame(self, padding=(14, 0, 14, 14)); lf.pack(fill='both', expand=True)
        self.lst = ttk.Treeview(lf, columns=('t',), show='tree', selectmode='browse')
        self.lst.column('#0', width=70); self.lst.column('t', width=800)
        sb = ttk.Scrollbar(lf, command=self.lst.yview); self.lst.configure(yscrollcommand=sb.set)
        self.lst.pack(side='left', fill='both', expand=True); sb.pack(side='left', fill='y')
        self.lst.bind('<<TreeviewSelect>>', lambda e: self.lst.selection() and self.go(int(self.lst.selection()[0]), from_list=True))
        self.state = [0] * self.W; self.playing = True; self.job = None
        self.protocol('WM_DELETE_WINDOW', self.close)
        self.select()

    def close(self):
        if self.job: self.after_cancel(self.job)
        self.destroy()

    def select(self):
        self.items = self.data['zones'][self.zone.get()]
        self.lst.delete(*self.lst.get_children())
        for i, x in enumerate(self.items):
            self.lst.insert('', 'end', iid=str(i), text=x['seq'], values=(x['title'],))
        self.go(0)

    def toggle(self):
        self.playing = not self.playing
        self.pbtn.config(text='⏸ Pause' if self.playing else '▶ Play')
        if self.playing: self.t0 = time.time() - self.el; self.tick()

    def go(self, i, from_list=False):
        self.it = i % len(self.items); self.fr = 0; self.t0 = time.time(); self.el = 0
        if not from_list:
            self.lst.selection_set(str(self.it)); self.lst.see(str(self.it))
        self.cap.config(text='  ·  '.join(self.items[self.it]['text']))
        if self.job: self.after_cancel(self.job)
        self.tick()

    def dur(self, f):
        if 'f' in f: return f['t']
        n = len(f['scroll']) if 'scroll' in f else max(len(l['cols']) for l in f['scroll2'])
        return (self.W + n) / f['speed']

    def cols(self, f, el):
        if 'f' in f:
            if f.get('fx') == 'wipe' and el < 0.4:
                k = int(el / 0.4 * self.W)
                return [c if x < k else self.prev[x] for x, c in enumerate(f['f'])]
            return f['f']
        off = int(el * f['speed'])
        lines = [{'cols': f['scroll'], 'row': f['row']}] if 'scroll' in f else f['scroll2']
        out = [0] * self.W
        for l in lines:
            for x in range(self.W):
                k = x - self.W + off
                if 0 <= k < len(l['cols']): out[x] |= l['cols'][k] << l['row']
        return out

    def tick(self):
        if not self.playing: return
        item = self.items[self.it]; f = item['frames'][self.fr]
        self.el = time.time() - self.t0
        if self.el >= self.dur(f):
            if 'f' in f: self.prev = f['f']
            self.fr += 1; self.t0 = time.time(); self.el = 0
            if self.fr >= len(item['frames']):
                self.go(self.it + 1); return
            f = item['frames'][self.fr]
        if not hasattr(self, 'prev'): self.prev = [0] * self.W
        c = self.cols(f, self.el)
        for x in range(self.W):
            if c[x] != self.state[x]:
                d = c[x] ^ self.state[x]
                for y in range(self.H):
                    if (d >> y) & 1:
                        self.cv.itemconfigure(self.dots[x][y], fill='#ffa31a' if (c[x] >> y) & 1 else '#2a1606')
                self.state[x] = c[x]
        self.job = self.after(30, self.tick)
