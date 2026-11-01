"""The setup window: python -m rgsetup.gui   (RogueGalaxy-Setup.exe starts it without a console)

A front end over the CLI (docs/INSTALLER.md). It collects the disc, the folder and the parts, asks the CLI what that
would do (`--estimate`), shows the answer for a yes (every download with address, size, SHA-256 and licence), then runs
the CLI with `--yes` and draws its events. Decisions live in guilogic.py; the words are ids of text/en.json.
The structure follows the sibling project's installer window (totk-pc-recomp/installer/gui.py, MIT, same author).

    --selftest [--shots DIR] [--scale 1.5]   build every screen with sample events and close by itself
    --dest DIR                               the install folder (default: the folder this setup runs from)
"""
import argparse
import ctypes
import json
import os
import queue
import shutil
import subprocess
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, font as tkfont, messagebox, ttk

from . import layout as layouts, plan as planmod, state as statefile
from .guilogic import (DONE, FAILED, PENDING, RUNNING, SKIPPED, Catalog, Form, Job, Model, amount, describe_event,
                       dpi_aware, gb, minutes_text, parse_line)

POLL_MS = 100
SETUP_EXE = 'RogueGalaxy-Setup.exe'
GLYPH = {PENDING: '\u25cb', RUNNING: '\u25cf', DONE: '\u2713', SKIPPED: '\u2013', FAILED: '\u2715'}
USER_DATA = {'disc', 'build', 'textures', 'mc0', 'mc1', 'work', 'rg-install.json', 'rg-install.json.tmp', 'install.log',
             '.git', '.scratch'}

# palette: a calm light surface, the game's navy for the one accent; every pair is 4.5:1 or better
BG, PANEL, FG, MUTED = '#f4f5f8', '#ffffff', '#1a2233', '#566073'
ACCENT, ACCENT_FG, OK, BAD = '#1d4a8f', '#ffffff', '#1b7a4b', '#b3261e'
WARN_BG, WARN_FG, RULE = '#fff3d1', '#5c4300', '#d5d9e2'


class Scroller(tk.Frame):
    """A frame whose content scrolls when the window cannot be as tall as the content (a small or zoomed screen). The
    scrollbar only shows then; fit() asks for the height through limit()."""

    def __init__(self, parent):
        super().__init__(parent, bg=BG)
        self.canvas = tk.Canvas(self, bg=BG, highlightthickness=0, borderwidth=0)
        self.bar = ttk.Scrollbar(self, orient='vertical', command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.canvas.pack(side='left', fill='both', expand=True)
        self.inner = tk.Frame(self.canvas, bg=BG)
        self.window = self.canvas.create_window(0, 0, window=self.inner, anchor='nw')
        self.canvas.bind('<Configure>', lambda e: self.canvas.itemconfigure(self.window, width=e.width))
        self.inner.bind('<Configure>', lambda e: self.canvas.configure(scrollregion=self.canvas.bbox('all')))
        self.bind_all('<MouseWheel>', self.wheel, add='+')

    def wheel(self, e):
        if self.bar.winfo_ismapped() and str(self.winfo_toplevel().focus_displayof()) != 'None':
            self.canvas.yview_scroll(int(-e.delta / 120) * 2, 'units')

    def limit(self, cap):
        """Height the canvas asks for: all of the content, or cap with a scrollbar."""
        need = self.inner.winfo_reqheight()
        self.canvas.configure(height=min(need, cap), width=self.inner.winfo_reqwidth())
        if need > cap:
            self.bar.pack(side='right', fill='y')
        else:
            self.bar.pack_forget()
            self.canvas.yview_moveto(0)


class App:
    def __init__(self, root, layout, cat=None, scale=None, dest=None, start_probe=True):
        self.root, self.layout = root, layout
        self.cat = cat or Catalog('en')
        self.k = scale or max(1.0, float(root.tk.call('tk', 'scaling')) * 72 / 96)
        if scale:
            root.tk.call('tk', 'scaling', scale * 96 / 72)
        self.form = Form(dest=str(dest or layout.dest))
        self.job = self.probe_job = self.review_job = None
        self.probe = Model()
        self.model = Model()
        self.review = Model()
        self.installed_parts = []
        self.is_installed = False
        self.closing = False
        self.log_entries = []
        self.parts_open = False
        self.fitted = None
        self.screen = None
        self.drop_ok = False

        root.title(self.cat('ui.title'))
        root.configure(bg=BG)
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        root.report_callback_exception = self.on_exception
        ico = layout.root / 'tools' / 'setup' / 'rgsetup' / 'data' / 'setup.ico'
        if os.name == 'nt' and ico.is_file():
            try:
                root.iconbitmap(default=str(ico))
            except tk.TclError:
                pass
        self.styles()

        self.top = tk.Frame(root, bg=BG)
        self.top.pack(fill='x', padx=self.s(24), pady=(self.s(18), 0))
        tk.Label(self.top, text=self.cat('ui.title'), bg=BG, fg=FG, font=self.f_title).pack(side='left')
        tk.Label(self.top, text=self.cat('ui.version', release=layout.release), bg=BG, fg=MUTED,
                 font=self.f_small).pack(side='left', padx=(self.s(10), 0), pady=(self.s(6), 0))
        self.body = tk.Frame(root, bg=BG)
        self.body.pack(fill='both', expand=True, padx=self.s(24), pady=self.s(14))
        self.frames = {'form': self.build_form(), 'consent': self.build_consent(), 'run': self.build_run(),
                      'finish': self.build_finish()}
        self.refresh_form()
        self.show('form')
        root.bind('<Return>', self.on_return)
        self.source_entry.focus_set()
        if start_probe:
            self.start_probe()
            root.after(300, self.enable_drop)

    # ---- look ---------------------------------------------------------------------------------------------------

    def s(self, n):
        return int(round(n * self.k))

    def styles(self):
        r = self.root
        family = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'
        for name in ('TkDefaultFont', 'TkTextFont', 'TkMenuFont', 'TkHeadingFont'):
            tkfont.nametofont(name).configure(family=family, size=10)
        tkfont.nametofont('TkFixedFont').configure(family='Consolas' if os.name == 'nt' else 'DejaVu Sans Mono', size=9)
        self.f_title = tkfont.Font(family=family, size=17, weight='bold')
        self.f_head = tkfont.Font(family=family, size=12, weight='bold')
        self.f_body = tkfont.Font(family=family, size=10)
        self.f_bold = tkfont.Font(family=family, size=10, weight='bold')
        self.f_small = tkfont.Font(family=family, size=9)
        st = ttk.Style(r)
        st.theme_use('clam')
        st.configure('.', background=BG, foreground=FG, font=self.f_body, bordercolor=RULE, focuscolor=ACCENT)
        st.configure('TButton', padding=(self.s(14), self.s(6)), background=PANEL, foreground=FG, bordercolor='#aab1c0',
                     relief='flat', borderwidth=1)
        st.map('TButton', background=[('active', '#e8ecf5'), ('disabled', '#eceef3')], foreground=[('disabled', '#7a8294')])
        st.configure('Accent.TButton', background=ACCENT, foreground=ACCENT_FG, bordercolor=ACCENT, font=self.f_bold)
        st.map('Accent.TButton', background=[('active', '#163a73'), ('disabled', '#9fb0cc')],
               foreground=[('disabled', '#ffffff')])
        st.configure('Link.TButton', background=BG, foreground=ACCENT, bordercolor=BG, padding=(0, self.s(2)))
        st.map('Link.TButton', background=[('active', BG)], foreground=[('active', '#163a73')])
        st.configure('TEntry', fieldbackground=PANEL, bordercolor='#aab1c0', padding=self.s(5))
        st.configure('TCheckbutton', background=BG, foreground=FG, padding=self.s(2))
        st.configure('TRadiobutton', background=BG, foreground=FG, padding=self.s(2))
        st.map('TCheckbutton', background=[('active', BG)], foreground=[('disabled', '#7a8294')])
        st.map('TRadiobutton', background=[('active', BG)], foreground=[('disabled', '#7a8294')])
        st.configure('Horizontal.TProgressbar', troughcolor='#dfe3ec', background=ACCENT, bordercolor='#dfe3ec',
                     lightcolor=ACCENT, darkcolor=ACCENT, thickness=self.s(10))

    @property
    def wrap(self):
        return self.s(660)

    def label(self, parent, text='', *, font=None, fg=FG, bg=BG, wrap=True, anchor='w', **kw):
        w = tk.Label(parent, text=text, font=font or self.f_body, fg=fg, bg=bg, anchor=anchor, justify='left', **kw)
        if wrap:
            w.configure(wraplength=self.wrap)
        return w

    def button(self, parent, text, command, style='TButton', **kw):
        return ttk.Button(parent, text=text, command=command, style=style, **kw)

    def footer(self, frame):
        bar = tk.Frame(frame, bg=BG)
        bar.pack(side='bottom', fill='x', pady=(self.s(14), 0))   # packed first: pack shrinks the text, never the buttons
        tk.Frame(frame, bg=RULE, height=1).pack(side='bottom', fill='x')
        return bar

    # ---- screens ------------------------------------------------------------------------------------------------

    def show(self, name):
        for f in self.frames.values():
            f.pack_forget()
        self.frames[name].pack(fill='both', expand=True)
        self.screen = name
        self.fit()

    def fit(self):
        """Size the window to its largest screen, so a screen switch neither cuts text nor moves the buttons. It only
        grows; a size the player dragged it to stays."""
        self.root.update_idletasks()
        cap = int(self.root.winfo_screenheight() * 0.88) - self.s(24 + 80 + 18 + 14 * 2 + 40 + 90)
        self.scroller.limit(max(cap, self.s(260)))
        self.root.update_idletasks()
        sizes = [(f.winfo_reqwidth(), f.winfo_reqheight()) for f in self.frames.values()]
        chrome = self.root.winfo_reqheight() - self.body.winfo_reqheight()
        w = max(self.root.winfo_reqwidth(), max(a for a, _ in sizes) + 2 * self.s(24))
        h = chrome + max(b for _, b in sizes) + 2 * self.s(14)
        if (w, h) == self.fitted:
            return
        self.fitted = (w, h)
        self.root.minsize(w, h)
        self.root.geometry(f'{max(w, self.root.winfo_width() if self.root.winfo_ismapped() else 0)}x'
                           f'{max(h, self.root.winfo_height() if self.root.winfo_ismapped() else 0)}')

    # ---- form ---------------------------------------------------------------------------------------------------

    def build_form(self):
        outer = tk.Frame(self.body, bg=BG)
        bar = self.footer(outer)
        self.scroller = Scroller(outer)
        self.scroller.pack(fill='both', expand=True)
        f = self.scroller.inner
        self.review_button = self.button(bar, self.cat('ui.review'), self.do_review, 'Accent.TButton')
        self.review_button.pack(side='right')
        self.checking = self.label(bar, '', fg=MUTED, wrap=False)
        self.checking.pack(side='right', padx=(0, self.s(12)))

        self.label(f, self.cat('ui.form.heading'), font=self.f_head).pack(anchor='w')
        self.label(f, self.cat('ui.form.intro'), fg=MUTED).pack(anchor='w', pady=(self.s(2), self.s(12)))

        self.var_source = tk.StringVar()
        self.var_dest = tk.StringVar(value=self.form.dest)
        self.var_source.trace_add('write', lambda *_: self.on_form_edit())

        self.label(f, self.cat('ui.disc_label'), font=self.f_bold).pack(anchor='w')
        row = tk.Frame(f, bg=BG)
        row.pack(fill='x', pady=(self.s(4), 0))
        row.columnconfigure(0, weight=1)
        self.source_entry = ttk.Entry(row, textvariable=self.var_source)
        self.source_entry.grid(row=0, column=0, sticky='ew')
        self.button(row, self.cat('ui.choose_file'), self.pick_file).grid(row=0, column=1, padx=(self.s(8), 0))
        self.button(row, self.cat('ui.choose_folder'), self.pick_folder).grid(row=0, column=2, padx=(self.s(6), 0))
        self.disc_help = self.label(f, self.cat('ui.disc_help'), fg=MUTED, font=self.f_small)
        self.disc_help.pack(anchor='w', pady=(self.s(3), self.s(12)))

        self.label(f, self.cat('ui.dest_label'), font=self.f_bold).pack(anchor='w')
        row = tk.Frame(f, bg=BG)
        row.pack(fill='x', pady=(self.s(4), 0))
        row.columnconfigure(0, weight=1)
        self.dest_entry = ttk.Entry(row, textvariable=self.var_dest, state='readonly')
        self.dest_entry.grid(row=0, column=0, sticky='ew')
        self.button(row, self.cat('ui.change'), self.pick_dest).grid(row=0, column=1, padx=(self.s(8), 0))
        self.label(f, self.cat('ui.dest_help'), fg=MUTED, font=self.f_small).pack(anchor='w', pady=(self.s(3), 0))
        self.space_label = self.label(f, '', fg=MUTED, font=self.f_small)
        self.space_label.pack(anchor='w', pady=(0, self.s(10)))

        self.label(f, self.cat('ui.parts_label'), font=self.f_bold).pack(anchor='w')
        self.preset_var = tk.StringVar(value=self.form.preset)
        self.preset_widgets = {}
        for name in planmod.PRESETS:
            r = tk.Frame(f, bg=BG)
            r.pack(fill='x', pady=(self.s(2), 0))
            rb = ttk.Radiobutton(r, text=self.cat('preset.' + name), value=name, variable=self.preset_var,
                                 command=lambda n=name: self.choose_preset(n), width=15)
            rb.pack(side='left')
            d = self.label(r, self.cat('preset.' + name + '.desc'), fg=MUTED, font=self.f_small, wrap=False)
            d.pack(side='left')
            self.preset_widgets[name] = (rb, d)
        self.gpu_note = self.label(f, '', fg=BAD, font=self.f_small)
        self.parts_toggle = self.button(f, self.cat('ui.parts_custom'), self.toggle_parts, 'Link.TButton')
        self.parts_toggle.pack(anchor='w', pady=(self.s(6), 0))
        self.parts_box = tk.Frame(f, bg=BG)
        self.part_vars, self.part_widgets = {}, {}
        for c in planmod.COMPONENT_ORDER:
            var = tk.BooleanVar(value=c in self.form.parts)
            self.part_vars[c] = var
            r = tk.Frame(self.parts_box, bg=BG)
            r.pack(fill='x', pady=(self.s(3), 0), padx=(self.s(4), 0))
            r.columnconfigure(0, weight=1)
            cb = ttk.Checkbutton(r, text=self.cat('component.' + c), variable=var,
                                 command=lambda c=c: self.toggle_part(c))
            cb.grid(row=0, column=0, sticky='w')
            cost = self.label(r, '', fg=MUTED, font=self.f_small, wrap=False)
            cost.grid(row=0, column=1, sticky='e')
            desc = self.label(r, '', fg=MUTED, font=self.f_small)
            desc.grid(row=1, column=0, columnspan=2, sticky='w', padx=(self.s(26), 0))
            desc.configure(wraplength=self.wrap - self.s(30))
            self.part_widgets[c] = (cb, cost, desc)
        self.note = self.label(f, '', fg=WARN_FG, bg=WARN_BG, padx=self.s(10), pady=self.s(7))
        self.error_label = self.label(f, '', fg=BAD, font=self.f_bold)
        return outer

    def on_exception(self, kind, value, tb):
        """A bug in the window: the traceback goes to a file (pythonw has no console), and the player is told where."""
        import tempfile
        import traceback
        path = Path(tempfile.gettempdir()) / 'rg-setup-logs' / 'window-error.log'
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, 'a', encoding='utf-8') as f:
                f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + ''.join(traceback.format_exception(kind, value, tb)) + chr(10))
        except OSError:
            pass
        messagebox.showerror(self.cat('ui.title'), self.cat('ui.err_window', path=str(path)), parent=self.root)

    def on_return(self, _event=None):
        """Enter does the screen's main thing, as the highlighted button says."""
        if self.screen == 'form' and self.review_job is None:
            self.do_review()
        elif self.screen == 'consent' and 'disabled' not in self.start_button.state():
            self.start()

    def on_form_edit(self):
        self.form.source = self.var_source.get()
        self.error_label.configure(text='')

    def installed_text(self):
        names = [self.cat('component.' + c) for c in self.installed_parts]
        return ', '.join(names) if names else self.cat('ui.installed_none')

    def refresh_form(self):
        cat, probe = self.cat, self.probe
        per = probe.per_component()
        gone = probe.unavailable()
        for c, (cb, cost, desc) in self.part_widgets.items():
            info = per.get(c)
            mins = minutes_text(cat, info['minutes_min'], info['minutes_max']) if info else ''
            if info and info.get('final'):
                cost.configure(text=cat('ui.part_cost', disk=gb(info['peak']), time=mins))
            else:
                cost.configure(text=cat('ui.part_cost_none'))
            installed = c in self.installed_parts
            blocked = c in gone and not installed
            if blocked:
                reason_id, params = gone[c]
                desc.configure(text=cat('ui.part_unavailable'), fg=BAD)         # the reason is under the presets, once
            else:
                desc.configure(text='', fg=MUTED)
            if installed:
                cost.configure(text=cat('ui.part_installed'))
                self.part_vars[c].set(True)
            if installed:
                cb.state(['disabled'])
            elif blocked:
                self.part_vars[c].set(False)
                self.form.parts.discard(c)
                cb.state(['disabled'])
            else:
                cb.state(['!disabled'])
        for name, (rb, d) in self.preset_widgets.items():
            blocked = any(c in gone and c not in self.installed_parts for c in planmod.PRESETS[name])
            rb.state(['disabled' if blocked else '!disabled'])
            d.configure(text=cat('ui.part_unavailable') if blocked else cat('preset.' + name + '.desc'), fg=BAD if blocked else MUTED)
        left_out = [c for c in gone if c not in self.installed_parts]
        if left_out:
            reason_id, params = gone[left_out[0]]
            self.gpu_note.configure(text=cat('ui.part_no_vulkan' if reason_id == 'warn.hd_no_vulkan' else 'ui.part_no_gpu', **params))
            self.gpu_note.pack(anchor='w', padx=(self.s(4), 0), pady=(self.s(4), 0), before=self.parts_toggle)
        else:
            self.gpu_note.pack_forget()
        if self.form.preset not in planmod.PRESETS or any(
                c in gone and c not in self.installed_parts for c in planmod.PRESETS.get(self.form.preset, ())):
            fallback = next((n for n in ('recommended', 'faithful') if not any(
                c in gone for c in planmod.PRESETS[n] if c not in self.installed_parts)), 'faithful')
            if self.form.preset in planmod.PRESETS:
                self.form.set_preset(fallback)
        self.preset_var.set(self.form.preset if self.form.preset in planmod.PRESETS else '')
        for c, var in self.part_vars.items():
            if c not in self.installed_parts:
                var.set(c in self.form.parts)
        sp = probe.space
        if sp:
            self.space_label.configure(text=cat('ui.space_line', free=gb(sp['free']),
                                                drive=Path(sp['dest']).drive or sp['dest']))
        else:
            self.space_label.configure(text='')
        if self.is_installed:
            self.note.configure(text=cat('ui.installed_note', parts=self.installed_text()))
            self.note.pack(fill='x', pady=(self.s(10), 0))
            self.disc_help.configure(text=cat('ui.disc_installed'))
        else:
            self.note.pack_forget()
            self.disc_help.configure(text=cat('ui.disc_help'))
        self.error_label.pack_forget()
        if self.error_label.cget('text'):
            self.error_label.pack(anchor='w', pady=(self.s(8), 0))
        busy = self.review_job is not None
        self.review_button.state(['disabled' if busy else '!disabled'])
        self.checking.configure(text=cat('ui.checking') if busy else '')
        self.fit()

    def show_form_error(self, text):
        self.error_label.configure(text=text)
        self.error_label.pack(anchor='w', pady=(self.s(8), 0))
        self.fit()

    def choose_preset(self, name):
        self.form.set_preset(name)
        self.refresh_form()

    def toggle_part(self, c):
        self.form.toggle(c, self.part_vars[c].get())
        self.refresh_form()

    def toggle_parts(self):
        self.parts_open = not self.parts_open
        if self.parts_open:
            self.parts_box.pack(fill='x', after=self.parts_toggle)
        else:
            self.parts_box.pack_forget()
        self.parts_toggle.configure(text=self.cat('ui.parts_custom_hide' if self.parts_open else 'ui.parts_custom'))
        self.fit()

    def pick_file(self):
        types = [(self.cat('ui.type_images'), '*.iso *.bin *.cue *.chd'), (self.cat('ui.type_all'), '*.*')]
        p = filedialog.askopenfilename(title=self.cat('ui.pick_file'), filetypes=types)
        if p:
            self.var_source.set(os.path.normpath(p))

    def pick_folder(self):
        p = filedialog.askdirectory(title=self.cat('ui.pick_folder'))
        if p:
            self.var_source.set(os.path.normpath(p))

    def set_source(self, path):
        self.var_source.set(os.path.normpath(path))

    # ---- the folder: the build happens where the setup runs, so another folder means a copy of the setup there ------

    def pick_dest(self):
        p = filedialog.askdirectory(title=self.cat('ui.pick_dest'), mustexist=False)
        if not p:
            return
        self.move_to(os.path.normpath(p))

    def move_to(self, dest):
        cat = self.cat
        if Path(dest).resolve() == self.layout.dest:
            return
        problem = layouts.path_problem(dest)
        if problem:
            self.show_form_error(cat(problem[0], path=dest, **problem[1]))
            return
        d = Path(dest)
        if d.exists() and any(d.iterdir()) and not (d / statefile.NAME).is_file():
            self.show_form_error(cat('ui.move.nonempty', dest=dest))
            return
        size = sum(f.stat().st_size for f in self.program_files())
        if not messagebox.askyesno(cat('ui.move.title'), cat('ui.move.text', dest=dest, gb=gb(size)),
                                   icon='question', parent=self.root):
            return
        self.show_form_error(cat('ui.move.copying', dest=dest))
        self.error_label.configure(fg=MUTED)
        self.root.update()
        try:
            self.copy_program(d)
        except OSError as e:
            self.error_label.configure(fg=BAD)
            self.show_form_error(cat('ui.move.failed', dest=dest, detail=str(e)))
            return
        exe = d / SETUP_EXE
        if exe.is_file():
            subprocess.Popen([str(exe)], cwd=str(d), creationflags=0x00000008 if os.name == 'nt' else 0)
        else:
            subprocess.Popen([sys.executable, '-m', 'rgsetup.gui'], cwd=str(d / 'tools' / 'setup'))
        self.root.destroy()

    def program_files(self):
        root = self.layout.root
        for folder, dirs, files in os.walk(root):
            if Path(folder) == root:
                dirs[:] = [x for x in dirs if x not in USER_DATA]
                files = [x for x in files if x not in USER_DATA and x != 'Rogue Galaxy.exe']
            dirs[:] = [x for x in dirs if x != '__pycache__']
            for name in files:
                yield Path(folder) / name

    def copy_program(self, dest):
        root = self.layout.root
        for src in self.program_files():
            target = dest / src.relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)

    # ---- drag and drop (WM_DROPFILES; Tk has none) ---------------------------------------------------------------

    def enable_drop(self):
        if os.name != 'nt':
            return
        try:
            from ctypes import wintypes
            user32, shell32 = ctypes.windll.user32, ctypes.windll.shell32
            hwnd = user32.GetParent(self.root.winfo_id()) or self.root.winfo_id()
            WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
            user32.SetWindowLongPtrW.restype = ctypes.c_void_p
            user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
            user32.CallWindowProcW.restype = ctypes.c_ssize_t
            user32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            shell32.DragQueryFileW.argtypes = [ctypes.c_void_p, wintypes.UINT, wintypes.LPWSTR, wintypes.UINT]
            shell32.DragFinish.argtypes = [ctypes.c_void_p]
            user32.GetWindowLongPtrW.restype = ctypes.c_void_p
            user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
            old = user32.GetWindowLongPtrW(hwnd, -4)

            def proc(h, msg, wp, lp):
                if msg == 0x233:        # WM_DROPFILES
                    buf = ctypes.create_unicode_buffer(1024)
                    shell32.DragQueryFileW(wp, 0, buf, 1024)
                    shell32.DragFinish(wp)
                    path = buf.value
                    self.root.after(0, lambda: self.dropped(path))
                    return 0
                return user32.CallWindowProcW(old, h, msg, wp, lp)
            self._wndproc = WNDPROC(proc)       # kept alive for the window's life
            user32.SetWindowLongPtrW(hwnd, -4, ctypes.cast(self._wndproc, ctypes.c_void_p))
            shell32.DragAcceptFiles(hwnd, True)
            self.drop_ok = True
        except Exception:
            self.drop_ok = False

    def dropped(self, path):
        if self.screen == 'form' and path:
            self.set_source(path)

    # ---- probing and reviewing -----------------------------------------------------------------------------------

    def start_probe(self):
        if self.probe_job:
            self.probe_job.kill()
        try:
            self.probe_job = Job(self.form.args('probe'))
        except OSError as e:
            self.show_form_error(self.cat('ui.err_start', detail=str(e)))
            return
        self.probe_model = Model()
        self.root.after(POLL_MS, self.poll_probe)

    def poll_probe(self):
        job = self.probe_job
        if not job:
            return
        drain(job, self.probe_model.apply)
        code = job.poll()
        if code is None:
            self.root.after(POLL_MS, self.poll_probe)
            return
        self.probe_job = None
        m = self.probe_model
        if m.plan:
            self.probe = m
        elif m.error:
            self.show_form_error(self.cat(m.error[0], **m.error[1]))
        self.read_installed()
        self.refresh_form()

    def read_installed(self):
        try:
            s = statefile.load(self.form.dest)
        except statefile.StateError:
            s = None
        self.is_installed = bool(s and 'extract' in s.get('steps', {}))
        self.installed_parts = [c for c in planmod.COMPONENT_ORDER if s and c in (s.get('components') or [])]
        self.is_installed = self.is_installed or bool(self.probe.plan and self.probe.plan.get('skip', {}).get('extract') == 'current')

    def do_review(self):
        problems = self.form.problems(self.is_installed)
        if problems:
            self.show_form_error(' '.join(self.cat(p) for p in problems))
            return
        self.error_label.configure(text='', fg=BAD)
        self.form.source = self.var_source.get()
        try:
            self.review_job = Job(self.form.args('estimate'))
        except OSError as e:
            self.show_form_error(self.cat('ui.err_start', detail=str(e)))
            return
        self.review = Model()
        self.refresh_form()
        self.root.after(POLL_MS, self.poll_review)

    def poll_review(self):
        job = self.review_job
        if not job:
            return
        drain(job, self.review.apply)
        code = job.poll()
        if code is None:
            self.root.after(POLL_MS, self.poll_review)
            return
        self.review_job = None
        m = self.review
        if m.result != 'ok' or not m.plan:
            err = m.error or ('ui.err_died', {'code': code}, None)
            self.refresh_form()
            self.show_form_error(self.cat(err[0], **err[1]))
            return
        self.fill_consent()
        self.refresh_form()
        self.show('consent')

    # ---- consent ------------------------------------------------------------------------------------------------

    def build_consent(self):
        f = tk.Frame(self.body, bg=BG)
        bar = self.footer(f)
        self.start_button = self.button(bar, self.cat('ui.consent.start'), self.start, 'Accent.TButton')
        self.start_button.pack(side='right')
        self.button(bar, self.cat('ui.back'), lambda: self.show('form')).pack(side='right', padx=(0, self.s(8)))
        self.label(f, self.cat('ui.consent.heading'), font=self.f_head).pack(anchor='w')
        self.consent_dest = self.label(f, '', fg=MUTED)
        self.consent_dest.pack(anchor='w', pady=(self.s(2), self.s(10)))
        self.consent_parts = self.label(f, '')
        self.consent_parts.pack(anchor='w')
        self.consent_disk = self.label(f, '')
        self.consent_disk.pack(anchor='w', pady=(self.s(4), 0))
        self.consent_time = self.label(f, '')
        self.consent_time.pack(anchor='w', pady=(self.s(4), 0))
        self.consent_author = self.label(f, self.cat('ui.time_author'), fg=MUTED, font=self.f_small)
        self.consent_author.pack(anchor='w')
        self.consent_dl_head = self.label(f, '', font=self.f_bold)
        self.consent_dl_head.pack(anchor='w', pady=(self.s(14), self.s(4)))
        box = tk.Frame(f, bg=PANEL, highlightbackground=RULE, highlightthickness=1)
        self.consent_box = box
        self.dl_text = tk.Text(box, height=9, width=1, wrap='char', bg=PANEL, fg=FG, relief='flat', font=self.f_body,
                               padx=self.s(10), pady=self.s(8), borderwidth=0, highlightthickness=0, cursor='arrow')
        sc = ttk.Scrollbar(box, command=self.dl_text.yview)
        self.dl_text.configure(yscrollcommand=sc.set)
        self.dl_text.tag_configure('name', font=self.f_bold, spacing1=self.s(6))
        self.dl_text.tag_configure('small', font=self.f_small, foreground=MUTED, lmargin1=self.s(12), lmargin2=self.s(12))
        sc.pack(side='right', fill='y')
        self.dl_text.pack(side='left', fill='both', expand=True)
        self.gentle_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text=self.cat('ui.consent.gentle'), variable=self.gentle_var,
                        command=lambda: setattr(self.form, 'gentle', self.gentle_var.get())).pack(anchor='w', pady=(self.s(10), 0))
        return f

    def fill_consent(self):
        cat, m = self.cat, self.review
        plan = m.plan
        self.consent_dest.configure(text=cat('ui.consent.installing', dest=m.dest))
        todo = m.todo_steps()
        comps = plan.get('components') or []
        names = ', '.join([cat('ui.installed_none')] + [cat('component.' + c) for c in comps]) if comps else cat('component.none')
        self.consent_parts.configure(text=cat('ui.consent.parts', parts=names))
        d = plan.get('disk') or {}
        t = plan.get('time') or {}
        if not todo:
            self.consent_disk.configure(text=cat('ui.consent.nothing_new'))
            self.consent_time.configure(text='')
            self.consent_author.pack_forget()
        else:
            self.consent_disk.configure(text=cat('ui.consent.disk', need=gb(d.get('need', 0)), final=gb(d.get('final', 0))))
            self.consent_time.configure(text=cat('ui.consent.time', time=minutes_text(cat, t.get('minutes_min', 0), t.get('minutes_max', 0))))
            self.consent_author.pack(anchor='w', after=self.consent_time)
        dls = plan.get('downloads') or []
        self.dl_text.configure(state='normal')
        self.dl_text.delete('1.0', 'end')
        self.consent_box.pack_forget()
        if dls:
            self.consent_dl_head.configure(text=cat('ui.consent.downloads'))
            self.consent_box.pack(fill='x', after=self.consent_dl_head)
            for dl in dls:
                self.dl_text.insert('end', cat('ui.consent.download_row', name=dl['name'], mb=round(dl['bytes'] / 1e6),
                                               license=dl['license']) + '\n', 'name')
                self.dl_text.insert('end', cat('ui.consent.download_url', url=dl['url']) + '\n', 'small')
                self.dl_text.insert('end', (cat('ui.consent.download_sha', sha256=dl['sha256']) if dl.get('sha256')
                                            else cat('ui.consent.download_pip')) + '\n', 'small')
            self.start_button.configure(text=cat('ui.consent.start_download'))
        else:
            self.consent_dl_head.configure(text=cat('ui.consent.no_downloads') if todo else '')
            self.start_button.configure(text=cat('ui.consent.start'))
        self.dl_text.configure(state='disabled')
        self.start_button.state(['!disabled' if todo else 'disabled'])
        self.fit()

    # ---- run ----------------------------------------------------------------------------------------------------

    def build_run(self):
        f = tk.Frame(self.body, bg=BG)
        bar = self.footer(f)
        self.cancel_button = self.button(bar, self.cat('ui.cancel'), self.cancel)
        self.cancel_button.pack(side='right')
        self.log_toggle = self.button(bar, self.cat('ui.show_log'), self.toggle_log, 'Link.TButton')
        self.log_toggle.pack(side='left')
        self.run_head = self.label(f, '', font=self.f_head)
        self.run_head.pack(anchor='w')
        self.label(f, self.cat('ui.run.safe'), fg=MUTED, font=self.f_small).pack(anchor='w', pady=(self.s(2), self.s(10)))
        self.steps_box = tk.Frame(f, bg=BG)
        self.steps_box.pack(fill='x')
        self.step_rows = {}
        self.phase_label = self.label(f, '', font=self.f_bold)
        self.phase_label.pack(anchor='w', pady=(self.s(12), 0))
        self.bar = ttk.Progressbar(f, mode='determinate', maximum=1000)
        self.bar.pack(fill='x', pady=(self.s(5), self.s(6)))
        self.run_space = self.label(f, '', fg=MUTED, font=self.f_small)
        self.run_space.pack(anchor='w')
        self.warn_label = self.label(f, '', fg=WARN_FG, font=self.f_small)
        self.warn_label.pack(anchor='w', pady=(self.s(4), 0))
        self.log_frame = tk.Frame(f, bg=BG)
        self.log = tk.Text(self.log_frame, height=9, width=1, wrap='word', state='disabled', bg=PANEL, fg=FG,
                           font='TkFixedFont', relief='flat', highlightbackground=RULE, highlightthickness=1,
                           padx=self.s(6), pady=self.s(4))
        sc = ttk.Scrollbar(self.log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=sc.set)
        sc.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        self.log_open = False
        return f

    def start(self):
        self.form.source = self.var_source.get()
        self.begin(self.form.args('run'))

    def begin(self, args):
        self.model = Model()
        self.log_entries = []
        self.render_log()
        self.cancel_button.state(['!disabled'])
        self.cancel_button.configure(text=self.cat('ui.cancel'))
        try:
            self.job = Job(args)
        except OSError as e:
            self.model.error = ('ui.err_start', {'detail': str(e)}, None)
            self.model.finish(1)
            self.refresh_finish()
            self.show('finish')
            return
        self.build_step_rows([])
        self.show('run')
        self.refresh_run()
        self.root.after(POLL_MS, self.poll)

    def build_step_rows(self, steps):
        for w in self.steps_box.winfo_children():
            w.destroy()
        self.step_rows = {}
        for s in steps:
            row = tk.Frame(self.steps_box, bg=BG)
            row.pack(fill='x', pady=self.s(1))
            glyph = tk.Label(row, width=2, bg=BG, font=self.f_bold)
            glyph.pack(side='left')
            name = tk.Label(row, bg=BG, anchor='w', text=self.cat('step.' + s))
            name.pack(side='left')
            note = tk.Label(row, bg=BG, fg=MUTED, font=self.f_small)
            note.pack(side='left', padx=(self.s(8), 0))
            self.step_rows[s] = (glyph, name, note)

    def visible_steps(self):
        m = self.model
        if not m.plan:
            return ['check'] if m.steps else []
        run = set(m.plan.get('run', []))
        return [s for s in m.steps if (s in run or s == 'check') and s != 'finish']

    def poll(self):
        if not self.job:
            return
        drain(self.job, self.feed_event, self.feed_raw)
        code = self.job.poll()
        self.refresh_run()
        if code is None:
            self.root.after(POLL_MS, self.poll)
            return
        self.model.finish(code)
        self.job = None
        self.refresh_finish()
        self.show('finish')
        if self.closing:
            self.root.destroy()

    def feed_event(self, e):
        self.model.apply(e)
        self.log_entries.append(e)
        self.append_log(describe_event(self.cat, e))

    def feed_raw(self, line):
        if line.strip():
            self.log_entries.append(line.rstrip())
            self.append_log(line.rstrip())

    def refresh_run(self):
        cat, m = self.cat, self.model
        want = self.visible_steps()
        if want != list(self.step_rows):
            self.build_step_rows(want)
        for s, (glyph, name, note) in self.step_rows.items():
            st = m.states.get(s, PENDING)
            color = {PENDING: MUTED, RUNNING: ACCENT, DONE: OK, SKIPPED: MUTED, FAILED: BAD}[st]
            glyph.configure(text=GLYPH[st], fg=color)
            name.configure(font=self.f_bold if st == RUNNING else self.f_body, fg=FG if st != PENDING else MUTED)
            extra = ''
            if st == FAILED:
                extra = cat('ui.state_failed')
            elif st == SKIPPED:
                extra = cat('reason.' + m.reasons.get(s, 'current')) if m.reasons.get(s) in ('current', 'not_needed', 'not_chosen') else ''
            note.configure(text=extra)
        mode = m.mode if m.mode in ('install', 'resume', 'update') else 'install'
        self.run_head.configure(text=cat('ui.run.' + mode))
        if m.current and m.phase:
            text = cat(f'phase.{m.current}.{m.phase}')
            if m.total:
                text += '   ' + amount(cat, m.done, m.total, m.unit)
        elif m.current:
            text = cat('step.' + m.current)
        else:
            text = ''
        self.phase_label.configure(text=text)
        frac = m.fraction()
        if frac is None:
            if str(self.bar.cget('mode')) != 'indeterminate':
                self.bar.configure(mode='indeterminate')
                self.bar.start(15)
        else:
            if str(self.bar.cget('mode')) == 'indeterminate':
                self.bar.stop()
                self.bar.configure(mode='determinate')
            self.bar.configure(value=frac * 1000)
        sp = m.space
        self.run_space.configure(text=cat('ui.space_line', free=gb(sp['free']), drive=Path(sp['dest']).drive or sp['dest']) if sp else '')
        self.warn_label.configure(text='\n'.join(cat(i, **p) for i, p in m.warnings))
        self.fit()

    def toggle_log(self):
        self.log_open = not self.log_open
        if self.log_open:
            self.log_frame.pack(fill='both', expand=True, pady=(self.s(8), 0))
        else:
            self.log_frame.pack_forget()
        self.log_toggle.configure(text=self.cat('ui.hide_log' if self.log_open else 'ui.show_log'))
        self.fit()

    def render_log(self):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        for entry in self.log_entries:
            text = entry if isinstance(entry, str) else describe_event(self.cat, entry)
            if text:
                self.log.insert('end', text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def append_log(self, text):
        if text:
            self.log.configure(state='normal')
            self.log.insert('end', text + '\n')
            self.log.see('end')
            self.log.configure(state='disabled')

    def cancel(self):
        if self.job:
            self.cancel_button.state(['disabled'])
            self.cancel_button.configure(text=self.cat('ui.cancelling'))
            self.job.cancel()

    def on_close(self):
        if self.job and self.screen == 'run':
            if not messagebox.askyesno(self.cat('ui.confirm.title'), self.cat('ui.confirm.text'), parent=self.root):
                return
            self.closing = True
            self.cancel()
            return
        for j in (self.probe_job, self.review_job):
            if j:
                j.kill()
        self.root.destroy()

    # ---- finish -------------------------------------------------------------------------------------------------

    def build_finish(self):
        f = tk.Frame(self.body, bg=BG)
        bar = self.footer(f)
        self.close_button = self.button(bar, self.cat('ui.close'), self.on_close)
        self.close_button.pack(side='right')
        self.play_button = self.button(bar, self.cat('ui.play'), self.play, 'Accent.TButton')
        self.play_button.pack(side='right', padx=(0, self.s(8)))
        self.retry_button = self.button(bar, self.cat('ui.retry'), self.retry, 'Accent.TButton')
        self.folder_button = self.button(bar, self.cat('ui.open_folder'), self.open_folder)
        self.folder_button.pack(side='right', padx=(0, self.s(8)))
        self.finish_head = self.label(f, '', font=self.f_title)
        self.finish_head.pack(anchor='w')
        self.finish_text = self.label(f, '')
        self.finish_text.pack(anchor='w', pady=(self.s(10), 0))
        self.finish_more = self.label(f, '', fg=MUTED)
        self.finish_more.pack(anchor='w', pady=(self.s(8), 0))
        self.finish_steps = tk.Frame(f, bg=BG)
        self.finish_steps.pack(fill='x', pady=(self.s(14), 0))
        return f

    def refresh_finish(self):
        cat, m = self.cat, self.model
        result = m.result or 'ok'
        if result == 'ok':
            ran = any(m.states.get(s) == DONE for s in m.steps if s not in ('check', 'finish'))
            self.finish_head.configure(text=cat('ui.done.heading'), fg=OK)
            self.finish_text.configure(text=cat('ui.done.text', dest=m.dest) if ran or not m.plan else cat('ui.done.nothing'))
            self.finish_more.configure(text='')
        elif result == 'cancelled':
            self.finish_head.configure(text=cat('ui.cancelled.heading'), fg=FG)
            self.finish_text.configure(text=cat('ui.cancelled.text'))
            self.finish_more.configure(text='')
        else:
            err = m.error or ('ui.err_died', {'code': m.exit_code}, None)
            self.finish_head.configure(text=cat('ui.failed.heading'), fg=BAD)
            self.finish_text.configure(text=cat(err[0], **err[1]))
            more = cat('ui.failed.next')
            if m.log_path:
                more += '\n' + cat('ui.failed.log', path=m.log_path)
            self.finish_more.configure(text=more)
        for w in self.finish_steps.winfo_children():
            w.destroy()
        for s in self.visible_steps():
            st = m.states.get(s, PENDING)
            color = {PENDING: MUTED, RUNNING: ACCENT, DONE: OK, SKIPPED: MUTED, FAILED: BAD}[st]
            tk.Label(self.finish_steps, text=f'{GLYPH[st]}  {cat("step." + s)}', bg=BG, fg=color, anchor='w',
                     font=self.f_body).pack(anchor='w')
        self.play_button.pack_forget()
        self.retry_button.pack_forget()
        if result == 'ok':
            self.play_button.pack(side='right', padx=(0, self.s(8)), before=self.folder_button)
        else:
            self.retry_button.pack(side='right', padx=(0, self.s(8)), before=self.folder_button)
        self.fit()

    def retry(self):
        self.start_probe()
        self.show('form')

    def play(self):
        exe = self.layout.launcher
        if exe.is_file():
            subprocess.Popen([str(exe)], cwd=str(self.layout.dest), creationflags=0x00000008 if os.name == 'nt' else 0)
            self.root.destroy()

    def open_folder(self):
        dest = self.model.dest or self.form.dest
        if dest and Path(dest).is_dir() and os.name == 'nt':
            os.startfile(dest)


def drain(job, on_event, on_raw=None):
    while True:
        try:
            line = job.lines.get_nowait()
        except queue.Empty:
            return
        e = parse_line(line)
        if e is not None:
            on_event(e)
        elif on_raw:
            on_raw(line)


# ---- screenshots and the self test -------------------------------------------------------------------------------

def grab(root, path):
    """The window's pixels without bringing it to the front: PrintWindow, so a window placed off screen (or behind a
    game) is captured the same. Needs Pillow; only the developer's --shots uses it."""
    try:
        from PIL import Image
    except ImportError:         # the release keeps Pillow in toolchain/pylibs, for the HD builder
        sys.path.append(str(layouts.ROOT / 'toolchain' / 'pylibs'))
        from PIL import Image
    from ctypes import wintypes
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    hwnd = user32.GetParent(root.winfo_id()) or root.winfo_id()
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    hdc = user32.GetDC(0)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)

    class BMI(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('w', wintypes.LONG), ('h', wintypes.LONG), ('planes', wintypes.WORD),
                    ('bits', wintypes.WORD), ('comp', wintypes.DWORD), ('img', wintypes.DWORD), ('x', wintypes.LONG),
                    ('y', wintypes.LONG), ('used', wintypes.DWORD), ('imp', wintypes.DWORD)]
    bmi = BMI(ctypes.sizeof(BMI), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(0, hdc)
    Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(path)


def quiet_window(root):
    """For screenshots: fully transparent, never activated, clicks pass through: the window paints (so PrintWindow
    sees it) but cannot take the player's focus from a game or catch a keystroke."""
    root.update()
    user32 = ctypes.windll.user32
    hwnd = user32.GetParent(root.winfo_id()) or root.winfo_id()
    user32.GetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.SetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
    style = user32.GetWindowLongW(hwnd, -20)
    user32.SetWindowLongW(hwnd, -20, style | 0x08000000 | 0x80000 | 0x20 | 0x80)   # NOACTIVATE, LAYERED, TRANSPARENT, TOOLWINDOW
    root.attributes('-alpha', 0.0)


SAMPLE_PROBE = [
    {'type': 'start', 'release': '0.1.0', 'mode': 'install', 'dest': 'C:\\RogueGalaxy', 'steps': ['check', 'source', 'extract',
     'toolchain', 'build', 'hd_textures', 'hd_movies', 'prompts_font', 'finish'], 'estimate': True},
    {'type': 'plan', 'components': ['textures', 'movies', 'prompts_font'], 'preset': 'everything', 'skipped_components': [],
     'run': ['check', 'source', 'extract', 'build', 'hd_textures', 'hd_movies', 'prompts_font', 'finish'], 'skip': {'toolchain': 'current'},
     'downloads': [], 'disk': {'need': 40 * 2**30, 'game': 16 * 2**30, 'peak_hd': 11 * 2**30, 'final': 36 * 2**30,
                               'per_component': {'textures': {'peak': 11 * 2**30, 'final': 6 * 2**30, 'minutes_min': 20, 'minutes_max': 30},
                                                 'movies': {'peak': 11 * 2**30, 'final': 7 * 2**30, 'minutes_min': 90, 'minutes_max': 90},
                                                 'prompts_font': {'peak': 0, 'final': 0, 'minutes_min': 0, 'minutes_max': 0}},
                               'temp_need': 0}, 'time': {'minutes_min': 131, 'minutes_max': 211}},
    {'type': 'space', 'dest': 'C:\\RogueGalaxy', 'need': 40 * 2**30, 'free': 212 * 2**30, 'temp': 'C:\\RogueGalaxy\\work\\setup',
     'temp_need': 0, 'temp_free': 0, 'same_disk': True},
]
SAMPLE_NO_GPU = [dict(e) for e in SAMPLE_PROBE]
SAMPLE_NO_GPU[1] = dict(SAMPLE_PROBE[1], components=['prompts_font'], skipped_components=[
    {'id': 'textures', 'reason': 'warn.hd_no_vulkan', 'params': {'devices': '-'}},
    {'id': 'movies', 'reason': 'warn.hd_no_vulkan', 'params': {'devices': '-'}}])
SAMPLE_REVIEW = [
    SAMPLE_PROBE[0], dict(SAMPLE_PROBE[1], components=['textures', 'prompts_font'], preset='recommended', downloads=[
        {'id': 'realesrgan', 'name': 'Real-ESRGAN ncnn Vulkan 0.2.0', 'url': 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip',
         'bytes': 45_000_000, 'sha256': 'a3f1c0de5b7e9a2d4c6b8e0f1a3c5e7092b4d6f8a0c2e4f6a8b0d2c4e6f8a0b1', 'license': 'BSD-3-Clause', 'dest': 'toolchain\\realesrgan', 'needed_by': 'hd_textures'},
        {'id': 'pylibs', 'name': 'numpy and Pillow (pip, from pypi.org)', 'url': 'https://pypi.org/', 'bytes': 40_000_000, 'sha256': '',
         'license': 'BSD-3-Clause (numpy), MIT-CMU (Pillow)', 'dest': 'toolchain\\pylibs', 'needed_by': 'hd_textures'}]),
    SAMPLE_PROBE[2]]
SAMPLE_RUN = [
    SAMPLE_PROBE[0] | {'estimate': False, 'log': 'C:\\RogueGalaxy\\work\\setup\\logs\\setup.log'},
    SAMPLE_REVIEW[1] | {'run': ['check', 'source', 'extract', 'build', 'hd_textures', 'prompts_font', 'finish'], 'skip': {'toolchain': 'current', 'hd_movies': 'not_chosen'}},
    {'type': 'step', 'step': 'check', 'state': 'started'},
    SAMPLE_PROBE[2] | {'estimate': False},
    {'type': 'warning', 'id': 'warn.hd_no_gpu', 'params': {'devices': 'Basic Display'}},
    {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.2},
    {'type': 'step', 'step': 'source', 'state': 'started'},
    {'type': 'step', 'step': 'source', 'state': 'done', 'seconds': 1.4},
    {'type': 'step', 'step': 'extract', 'state': 'started'},
    {'type': 'step', 'step': 'extract', 'state': 'done', 'seconds': 17.1},
    {'type': 'step', 'step': 'build', 'state': 'started'},
    {'type': 'progress', 'step': 'build', 'phase': 'compile', 'done': 412, 'total': 2203, 'unit': 'files'},
]


def selftest(layout, shots=None, scale=None):
    """Every screen with sample events, no CLI child; the window closes itself. Returns 0 when nothing raised and the
    checks hold."""
    dpi_aware()
    root = tk.Tk()
    root.geometry('+40+40')
    if shots and os.name == 'nt':
        quiet_window(root)
    app = App(root, layout, scale=scale, start_probe=False)
    fails = []
    root.report_callback_exception = lambda kind, value, tb: fails.append(repr(value))
    shots_dir = Path(shots) if shots else None
    if shots_dir:
        shots_dir.mkdir(parents=True, exist_ok=True)
    tag = f'{int((scale or app.k) * 100)}'

    def shot(name):
        root.update()
        if shots_dir:
            grab(root, shots_dir / f'{name}-{tag}.png')

    def check(name, ok):
        if not ok:
            fails.append(name)

    def fold(events):
        m = Model()
        for e in events:
            m.apply(e)
        return m

    def run_all():
        try:
            app.probe = fold(SAMPLE_PROBE)
            app.refresh_form()
            app.show('form')
            shot('1-form')
            app.toggle_parts()
            app.var_source.set('D:\\discs\\Rogue Galaxy (USA).iso')
            shot('2-form-parts')
            check('parts open', app.parts_open)
            app.probe = fold(SAMPLE_NO_GPU)
            app.form.set_preset('recommended')
            app.refresh_form()
            shot('3-form-no-gpu')
            check('textures greyed', 'disabled' in app.part_widgets['textures'][0].state())
            check('recommended not offered', 'disabled' in app.preset_widgets['recommended'][0].state())
            app.show_form_error(app.cat('err.disc_region', serial='SCES-54567', region='Europe', expected_region='USA', expected='SCUS-97490'))
            shot('4-form-error')
            app.error_label.configure(text='')
            app.probe = fold(SAMPLE_PROBE)
            app.installed_parts = ['textures', 'prompts_font']
            app.is_installed = True
            app.refresh_form()
            shot('5-form-installed')
            check('installed part disabled', 'disabled' in app.part_widgets['textures'][0].state())
            app.installed_parts, app.is_installed = [], False
            app.refresh_form()
            app.review = fold(SAMPLE_REVIEW)
            app.fill_consent()
            app.show('consent')
            shot('6-consent')
            check('consent lists 2 downloads', app.dl_text.get('1.0', 'end').count('From ') == 2)
            app.model = Model()
            app.build_step_rows([])
            for e in SAMPLE_RUN:
                app.feed_event(e)
            app.show('run')
            app.refresh_run()
            shot('7-run')
            app.toggle_log()
            shot('8-run-log')
            app.toggle_log()
            app.model.finish(0)
            for e in [{'type': 'step', 'step': s, 'state': 'done', 'seconds': 1} for s in ('build', 'hd_textures', 'prompts_font')] + [{'type': 'end', 'result': 'ok', 'seconds': 99}]:
                app.model.apply(e)
            app.refresh_finish()
            app.show('finish')
            shot('9-finish-ok')
            app.model = fold(SAMPLE_RUN[:3] + [{'type': 'error', 'id': 'err.space', 'step': 'check', 'params': {'need': 38.0, 'free': 21.4, 'drive': 'C:'}},
                                               {'type': 'end', 'result': 'failed', 'seconds': 3}])
            app.model.finish(1)
            app.refresh_finish()
            shot('10-finish-failed')
            app.model = fold(SAMPLE_RUN[:4])
            app.model.finish(3)
            app.refresh_finish()
            shot('11-finish-cancelled')
            # the log follows the catalog: every event kind renders to words, none to its raw id
            text = ' '.join(describe_event(app.cat, e) for e in SAMPLE_RUN)
            check('log in words', 'step.' not in text and 'phase.' not in text)
            test_drop(app, check)
        except Exception as exc:     # a screen that cannot be built is a failed self test, not a hang
            import traceback
            traceback.print_exc()
            fails.append(repr(exc))
        root.destroy()

    root.after(300, run_all)
    root.after(90000, root.destroy)
    root.mainloop()
    for f in fails:
        print('FAIL', f)
    print('selftest', 'FAILED' if fails else 'ok', f'(scale {tag}%)')
    return 1 if fails else 0


def test_drop(app, check):
    """Post a WM_DROPFILES with one path to the window and see it land in the disc field."""
    if os.name != 'nt':
        return
    from ctypes import wintypes
    app.enable_drop()
    check('drop enabled', app.drop_ok)
    if not app.drop_ok:
        return
    app.show('form')
    app.var_source.set('')
    path = 'D:\\dropped\\disc.iso'
    data = path.encode('utf-16-le') + b'\0\0\0\0'
    head = (20).to_bytes(4, 'little') + b'\0' * 12 + (1).to_bytes(4, 'little')   # DROPFILES: offset 20, pt, fNC, fWide=1
    k32, u32 = ctypes.windll.kernel32, ctypes.windll.user32
    k32.GlobalAlloc.restype = ctypes.c_void_p
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]
    k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    h = k32.GlobalAlloc(0x42, len(head) + len(data))     # GHND
    p = k32.GlobalLock(h)
    ctypes.memmove(p, head + data, len(head) + len(data))
    k32.GlobalUnlock(h)
    hwnd = u32.GetParent(app.root.winfo_id()) or app.root.winfo_id()
    u32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p]
    u32.SendMessageW(hwnd, 0x233, h, 0)
    app.root.update()
    app.root.update()
    check('drop reaches the field', app.var_source.get() == os.path.normpath(path))



def live(layout, scenario, source=None, shots=None, preset='faithful', gentle=True, wait=5400):
    """Drive the real window against the real CLI, for the developer (no clicking needed, never takes the focus):

        review    fill the form, press Review, wait for the consent page (or the error), print what it shows
        run       review, then Start, wait for the finish page
        cancel    review, Start, press Cancel five seconds into the extraction, wait for the finish page

    Prints one JSON line with what the screens said. Exit 0 when the scenario ended as it should."""
    dpi_aware()
    root = tk.Tk()
    root.geometry('+40+40')
    if shots and os.name == 'nt':
        quiet_window(root)
    app = App(root, layout)
    out = {'scenario': scenario, 'steps': []}

    def boom(kind, value, tb):          # unattended: a bug ends the run, a dialog would hang it
        import traceback
        traceback.print_exception(kind, value, tb)
        out['exception'] = repr(value)
        finish(1)
    root.report_callback_exception = boom
    shots_dir = Path(shots) if shots else None
    if shots_dir:
        shots_dir.mkdir(parents=True, exist_ok=True)

    def shot(name):
        root.update()
        if shots_dir:
            grab(root, shots_dir / f'{scenario}-{name}.png')

    state = {'phase': 'probe', 't0': time.monotonic(), 'cancel_at': None}

    def tick():
        now = time.monotonic() - state['t0']
        ph = state['phase']
        if ph == 'probe':
            if app.probe_job is None:
                out['probe'] = {'installed': app.is_installed, 'unavailable': sorted(app.probe.unavailable()),
                                'per_component': sorted(app.probe.per_component()), 'free_gb': gb(app.probe.space['free']) if app.probe.space else None}
                shot('1-form')
                if source:
                    app.set_source(source)
                app.form.set_preset(preset)
                app.form.gentle = gentle
                app.refresh_form()
                app.review_button.invoke()          # the button's own command, as a click runs it
                state['phase'] = 'review'
        elif ph == 'review':
            if app.review_job is None:
                if app.screen == 'consent':
                    plan = app.review.plan
                    out['consent'] = {'components': plan['components'], 'run': plan['run'], 'downloads': [d['id'] for d in plan['downloads']],
                                      'need_gb': gb(plan['disk']['need']), 'text': app.consent_disk.cget('text'), 'time': app.consent_time.cget('text')}
                    shot('2-consent')
                    if scenario == 'review':
                        return finish(0)
                    app.start_button.invoke()
                    state['phase'] = 'run'
                else:
                    out['form_error'] = app.error_label.cget('text')
                    shot('2-form-error')
                    return finish(0 if scenario == 'review' else 1)
        elif ph == 'run':
            m = app.model
            if scenario == 'cancel' and state['cancel_at'] is None and m.states.get('extract') == RUNNING:
                state['cancel_at'] = now + 5
            if state['cancel_at'] and now >= state['cancel_at'] and app.job and app.job.cancelled_at is None:
                shot('3-running')
                app.cancel_button.invoke()
            if app.screen == 'finish':
                out['result'] = m.result
                out['exit'] = m.exit_code
                out['finish'] = app.finish_head.cget('text')
                out['steps'] = {s: m.states.get(s) for s in m.steps}
                shot('4-finish')
                want = 'cancelled' if scenario == 'cancel' else 'ok'
                return finish(0 if m.result == want else 1)
        if now > wait:
            out['timeout'] = True
            return finish(1)
        root.after(300, tick)

    def finish(code):
        out['code'] = code
        print(json.dumps(out), flush=True)
        root.destroy()
        sys.exit(code)

    root.after(300, tick)
    root.mainloop()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--shots', type=Path)
    ap.add_argument('--scale', type=float, help='pretend the display is at this scale (1.5 = 150%%)')
    ap.add_argument('--dest', type=Path)
    ap.add_argument('--live', choices=('review', 'run', 'cancel'), help='developer: drive the real window against the real CLI')
    ap.add_argument('--source', help='with --live: the disc to choose')
    ap.add_argument('--preset', default='faithful', choices=('faithful', 'recommended', 'everything'), help='with --live')
    args = ap.parse_args(argv)
    dpi_aware()
    layout = layouts.Layout(args.dest or layouts.ROOT)
    if args.selftest:
        return selftest(layout, args.shots, args.scale)
    if args.live:
        return live(layout, args.live, args.source, args.shots, args.preset)
    root = tk.Tk()
    App(root, layout, scale=args.scale, dest=args.dest)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
