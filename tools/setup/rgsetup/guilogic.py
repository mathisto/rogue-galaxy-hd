"""The setup window without Tk: the text catalog, the form, the event model and the CLI child process.

gui.py only draws; everything it decides lives here, so the tests run it headless. The window renders nothing but the
events of `python -m rgsetup` (docs/INSTALLER.md, "Events") and the ids of text/<lang>.json: it never reads the game
folder to learn what happened, and it never decides what the setup does.

    Catalog   the text of an id (text/<lang>.json, English where a translation lacks it)
    Form      what the player chose; builds the command lines and checks the entries
    Model     folds the events into step states, progress, plan, space, errors and the result
    Job       one run of the CLI as a child (own process group, no console), its lines in a queue; cancel = Ctrl+Break
"""
import ctypes
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import events, layout as layouts, plan as planmod

GiB = 1 << 30
COMPONENTS = planmod.COMPONENT_ORDER
PRESET_ORDER = tuple(planmod.PRESETS)


# ---- text ---------------------------------------------------------------------------------------------------------

class Catalog:
    """Window text: the same catalog as the log (events.load_catalog), so a sentence exists once."""

    def __init__(self, lang='en'):
        self.lang = lang
        self.texts = events.load_catalog(lang)

    def has(self, id):
        return id in self.texts

    def __call__(self, id, **params):
        return events.render(self.texts, id, params)


def gb(n):
    """Bytes as whole-or-tenth gigabytes the way the setup's own messages round (binary, labelled GB)."""
    v = n / GiB
    return f'{v:.0f}' if v >= 10 else f'{v:.1f}'


def minutes_text(cat, lo, hi):
    """'20 to 30 minutes', 'about 90 minutes', 'about 2 to 3.5 hours'."""
    if not hi:
        return cat('ui.time_none')
    if hi >= 120:
        half = lambda m: f'{round(m / 60 * 2) / 2:g}'
        if half(lo) == half(hi):
            return cat('ui.time_hours', n=half(hi))
        return cat('ui.time_hours_range', lo=half(lo), hi=half(hi))
    if lo == hi:
        return cat('ui.time_about', n=lo)
    return cat('ui.time_range', lo=lo, hi=hi)


def amount(cat, done, total, unit):
    if unit == 'bytes':
        return cat('ui.amount_bytes', done=f'{(done or 0) / 1e9:.1f}', total=f'{total / 1e9:.1f}')
    if unit == 'permille':
        return cat('ui.amount_percent', n=round((done or 0) / 10))
    id = f'ui.amount_{unit}'
    return cat(id if cat.has(id) else 'ui.amount_other', done=done or 0, total=total)


# ---- the CLI child ------------------------------------------------------------------------------------------------

CREATE_NEW_PROCESS_GROUP = 0x200
CREATE_NO_WINDOW = 0x08000000


def cli_python():
    """python.exe that runs `-m rgsetup`: the bundled one next to the window's pythonw, else the running interpreter."""
    exe = Path(sys.executable)
    if exe.name.lower() == 'pythonw.exe' and exe.with_name('python.exe').is_file():
        return exe.with_name('python.exe')
    return exe


_BREAK = """
import ctypes, sys, time
k = ctypes.windll.kernel32
pid = int(sys.argv[1])
k.FreeConsole()
if k.AttachConsole(pid):
    k.SetConsoleCtrlHandler(None, True)
    k.GenerateConsoleCtrlEvent(1, pid)
    time.sleep(1)
    k.FreeConsole()
else:
    sys.exit(1)
"""


def send_break(proc):
    """Ctrl+Break to proc's process group (it was started with CREATE_NEW_PROCESS_GROUP and a hidden console). The
    window has no console of its own (pythonw), so a short helper process attaches to the child's console and sends it."""
    if os.name != 'nt':
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            return True
        except OSError:
            return False
    r = subprocess.run([str(cli_python()), '-c', _BREAK, str(proc.pid)], creationflags=CREATE_NO_WINDOW, capture_output=True)
    return r.returncode == 0


class Job:
    """`python -m rgsetup ...` running; its stdout lines arrive in self.lines. poll() gives the exit code once the
    process ended and its output is drained."""

    def __init__(self, args, cwd=None, env=None):
        self.command = [str(cli_python()), '-X', 'utf8', '-m', 'rgsetup', *map(str, args)]
        self.lines = queue.Queue()
        e = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1', PYTHONUNBUFFERED='1')
        e.update(env or {})
        flags = (CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW) if os.name == 'nt' else 0
        group = {'creationflags': flags} if os.name == 'nt' else {'start_new_session': True}
        self.proc = subprocess.Popen(self.command, cwd=str(cwd or layouts.ROOT / 'tools' / 'setup'), env=e,
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **group)
        self.cancelled_at = None
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for raw in iter(self.proc.stdout.readline, b''):
            self.lines.put(raw.decode('utf-8', errors='replace'))
        self.proc.stdout.close()

    def cancel(self):
        if self.cancelled_at is None and self.proc.poll() is None:
            self.cancelled_at = time.monotonic()
            threading.Thread(target=send_break, args=(self.proc,), daemon=True).start()

    def poll(self):
        code = self.proc.poll()
        if code is not None and not self.reader.is_alive() and self.lines.empty():
            return code
        return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()


def parse_line(line):
    """The event dict of a stdout line, or None when it is not an event (noise, a traceback line)."""
    line = line.strip()
    if not line.startswith('{'):
        return None
    try:
        e = json.loads(line)
    except ValueError:
        return None
    return e if isinstance(e, dict) and isinstance(e.get('type'), str) else None


# ---- the form -----------------------------------------------------------------------------------------------------

@dataclass
class Form:
    source: str = ''
    dest: str = ''
    preset: str = 'recommended'             # faithful | recommended | everything | custom
    parts: set = field(default_factory=lambda: set(planmod.PRESETS['recommended']))
    gentle: bool = False

    def set_preset(self, name):
        self.preset = name
        if name in planmod.PRESETS:
            self.parts = set(planmod.PRESETS[name])

    def toggle(self, part, on):
        (self.parts.add if on else self.parts.discard)(part)
        self.preset = planmod.preset_of(self.parts)

    def components_arg(self):
        return ','.join(c for c in COMPONENTS if c in self.parts)

    def args(self, mode):
        """CLI arguments. mode: 'probe' (what can this PC and folder do), 'estimate' (check my choices) or 'run'."""
        a = ['--dest', self.dest.strip(), '--lang', 'en']
        if mode == 'probe':
            a += ['--estimate', '--preset', 'everything']
            return a
        if self.source.strip():
            a += ['--source', self.source.strip()]
        a += ['--components', self.components_arg()]
        if self.gentle:
            a.append('--gentle')
        a.append('--estimate' if mode == 'estimate' else '--yes')
        return a

    def problems(self, installed):
        """Ids of what is missing, in screen order. installed: the disc is already unpacked in the folder."""
        found = []
        if not self.dest.strip():
            found.append('ui.need_dest')
        if not installed:
            s = self.source.strip()
            if not s:
                found.append('ui.need_source')
            elif not Path(s).exists():
                found.append('ui.missing_source')
        return found


# ---- events -> state ----------------------------------------------------------------------------------------------

PENDING, RUNNING, DONE, SKIPPED, FAILED = 'pending', 'running', 'done', 'skipped', 'failed'


class Model:
    """What the window shows, folded from events. apply() takes one event; nothing else mutates it."""

    def __init__(self):
        self.steps, self.states = [], {}
        self.mode = self.release = self.log_path = self.dest = ''
        self.estimate = False
        self.current = self.phase = None
        self.done = self.total = self.unit = None
        self.plan = None
        self.space = None
        self.warnings = []            # (id, params)
        self.error = None             # (id, params, step)
        self.result = None
        self.seconds = None
        self.exit_code = None
        self.reasons = {}

    def apply(self, e):
        kind = e['type']
        if kind == 'start':
            self.__init__()
            self.steps = list(e.get('steps') or [])
            self.states = {s: PENDING for s in self.steps}
            self.mode, self.release = e.get('mode', ''), e.get('release', '')
            self.log_path, self.dest = e.get('log', ''), e.get('dest', '')
            self.estimate = bool(e.get('estimate'))
        elif kind == 'plan':
            self.plan = e
            for s, reason in (e.get('skip') or {}).items():
                if s in self.states and self.states[s] == PENDING:
                    self.states[s] = SKIPPED
                    self.reasons[s] = reason
        elif kind == 'step':
            s, state = e.get('step'), e.get('state')
            if s not in self.states:
                self.states[s] = PENDING
                self.steps.append(s)
            if state == 'started':
                self.states[s], self.current = RUNNING, s
                self.phase = self.done = self.total = self.unit = None
            elif state == 'done':
                self.states[s] = DONE
                if self.current == s:
                    self.current = None
            elif state == 'skipped':
                self.states[s] = SKIPPED
                self.reasons[s] = e.get('reason')
        elif kind == 'progress':
            if self.states.get(e.get('step')) == PENDING:
                self.states[e['step']] = RUNNING
            self.current = e.get('step', self.current)
            self.phase = e.get('phase')
            self.done, self.total, self.unit = e.get('done'), e.get('total'), e.get('unit')
        elif kind == 'space':
            self.space = e
        elif kind == 'warning':
            self.warnings.append((e.get('id', ''), e.get('params') or {}))
        elif kind == 'error':
            self.error = (e.get('id', ''), e.get('params') or {}, e.get('step'))
            if e.get('step') in self.states:
                self.states[e['step']] = FAILED
        elif kind == 'end':
            self.result = e.get('result')
            self.seconds = e.get('seconds')
            self.current = None

    def fraction(self):
        """0..1 of the current phase, or None when its size is unknown."""
        if self.total:
            return max(0.0, min(1.0, (self.done or 0) / self.total))
        return None

    def finish(self, code):
        """The process ended. No end event (it died) counts as failed, or cancelled on exit code 3."""
        self.exit_code = code
        if self.result is None:
            self.result = 'cancelled' if code == 3 else 'failed'
        if self.result == 'failed' and self.error is None:
            self.error = ('ui.err_died', {'code': code}, self.current)
        for s, st in self.states.items():
            if st == RUNNING:
                self.states[s] = FAILED if self.result == 'failed' else PENDING

    # --- what the plan says, ready for the screens
    def todo_steps(self):
        """Steps a player sees: the ones that will run, without the always-on check and finish."""
        if not self.plan:
            return []
        return [s for s in self.plan.get('run', []) if s not in ('check', 'finish', 'source')]

    def per_component(self):
        return ((self.plan or {}).get('disk') or {}).get('per_component') or {}

    def unavailable(self):
        """{component id: (reason id, params)} for HD parts this PC cannot do."""
        return {i['id']: (i['reason'], i.get('params') or {}) for i in (self.plan or {}).get('skipped_components', [])}


def describe_event(cat, e):
    """One log line for an event, in words (the window's log)."""
    kind = e['type']
    if kind == 'step':
        text = f'{cat("step." + e["step"])}: {cat("state." + e["state"])}'
        if 'seconds' in e:
            text += f' ({e["seconds"]:.1f} s)'
        elif e.get('reason'):
            text += f' ({cat("reason." + e["reason"])})'
        return text
    if kind == 'progress':
        text = cat(f'phase.{e["step"]}.{e["phase"]}')
        if e.get('total'):
            text += '  ' + amount(cat, e.get('done'), e['total'], e.get('unit'))
        return text
    if kind in ('warning', 'error'):
        return f'{cat("ui.log_" + kind)}: {cat(e["id"], **(e.get("params") or {}))}'
    if kind == 'space':
        return cat('ui.log_space', need=gb(e.get('need', 0)), free=gb(e.get('free', 0)))
    if kind == 'end':
        return cat('result.' + str(e.get('result')))
    if kind == 'start':
        return cat('msg.start', release=e.get('release', ''), dest=e.get('dest', ''))
    return ''


def host_scale_ok():
    return os.name == 'nt'


def dpi_aware():
    """Per-monitor DPI awareness before Tk exists, so text and the window are sharp at 125 to 200%."""
    if os.name != 'nt':
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass
