"""Progress events: one JSON object per line on stdout (for the window and for tests), the same in words in a log file.

Every event has "type" and "t" (seconds since the start). The types (docs/INSTALLER.md has the full schema):

    start     release, mode (install | resume | update), dest, log, steps (the step ids in order), estimate, verify
    plan      what will happen: components, skipped_components, run, skip, downloads, disk, time (see plan.py)
    space     dest, need, free (bytes on the install drive), temp, temp_need, temp_free, same_disk
    step      step, state: started | done (seconds) | skipped (reason: current | not_needed | not_chosen)
    progress  step, phase, and done/total/unit (bytes, files, batches, movies ...) when the amount is known
    warning   id, params
    error     id, params, step (absent outside a step)
    end       result: ok | failed | cancelled, seconds

Text is never the payload. Ids name a step ("step.<id>"), a phase ("phase.<step>.<phase>") or a message ("err.*",
"warn.*", "msg.*"); text/en.json holds the English text with {param} placeholders, and a translator adds
text/<lang>.json. The log (and --format text) renders the events with the catalog.
"""
import json
import sys
import threading
import time
from pathlib import Path

TEXT_DIR = Path(__file__).resolve().parent / 'text'
PROGRESS_EVERY = 0.5   # seconds between two progress events of one phase


def load_catalog(lang='en'):
    """The catalog of lang, English filling the ids a translation lacks."""
    english = json.loads((TEXT_DIR / 'en.json').read_text(encoding='utf-8'))
    if lang and lang != 'en':
        path = TEXT_DIR / f'{lang}.json'
        if path.is_file():
            english.update(json.loads(path.read_text(encoding='utf-8')))
    return english


def render(catalog, id, params=None):
    """The text of id with its parameters, or the id itself when the catalog lacks it."""
    text = catalog.get(id, id)
    try:
        return text.format(**(params or {}))
    except (KeyError, IndexError, ValueError):
        return f'{text} {params}'


def human_bytes(n):
    n = float(n)
    for unit in ('bytes', 'KiB', 'MiB', 'GiB'):
        if n < 1024 or unit == 'GiB':
            return f'{n:.0f} {unit}' if unit == 'bytes' else f'{n:.1f} {unit}'
        n /= 1024


class Reporter:
    def __init__(self, out=None, log_path=None, text=False, lang='en'):
        self.out = out or sys.stdout
        self.text = text            # human lines on stdout instead of JSON
        self.t0 = time.monotonic()
        self.catalog = load_catalog(lang)
        self.lock = threading.Lock()
        self.log_file = None
        self.last_progress = {}
        self.log_path = log_path
        if log_path:
            Path(log_path).parent.mkdir(parents=True, exist_ok=True)
            self.log_file = open(log_path, 'a', encoding='utf-8', buffering=1)

    def elapsed(self):
        return time.monotonic() - self.t0

    def log(self, text):
        """A line for the log file only."""
        if self.log_file:
            with self.lock:
                self.log_file.write(f'[{self.elapsed():8.1f}] {text}\n')

    def emit(self, type, **fields):
        event = {'type': type, 't': round(self.elapsed(), 2), **fields}
        line = self.describe(event)
        with self.lock:
            if self.text:
                if line is not None and not (type == 'progress' and not fields.get('final')):
                    self.out.write(line + '\n')
            else:
                self.out.write(json.dumps(event, ensure_ascii=False) + '\n')
            self.out.flush()
        if line is not None:
            self.log(line)
        return event

    def describe(self, e):
        """One line of words for an event (None when it has nothing to say)."""
        c = self.catalog
        kind = e['type']
        if kind == 'step':
            name = render(c, 'step.' + e['step'])
            if e['state'] == 'started':
                return f'== {name}'
            extra = f' ({e["seconds"]:.1f} s)' if 'seconds' in e else f' ({render(c, "reason." + e["reason"])})' if 'reason' in e else ''
            return f'   {name}: {render(c, "state." + e["state"])}{extra}'
        if kind == 'progress':
            text = '   ' + render(c, f'phase.{e["step"]}.{e["phase"]}')
            if e.get('total'):
                unit = e.get('unit', '')
                if unit == 'bytes':
                    text += f' {human_bytes(e["done"])} / {human_bytes(e["total"])}'
                else:
                    text += f' {e["done"]}/{e["total"]} {unit}'.rstrip()
            return text
        if kind in ('warning', 'error'):
            return f'{kind.upper()}: {render(c, e["id"], e.get("params"))} [{e["id"]}]'
        if kind == 'space':
            return (f'disk: needs {human_bytes(e["need"])}, {human_bytes(e["free"])} free on the install drive'
                    + (f'; temp needs {human_bytes(e["temp_need"])}, {human_bytes(e["temp_free"])} free' if e.get('temp_need') else ''))
        if kind == 'end':
            return f'{render(c, "result." + e["result"])} ({e["seconds"]:.0f} s)'
        if kind == 'plan':
            return self.describe_plan(e)
        if kind == 'start':
            return render(c, 'msg.start', {'release': e['release'], 'dest': e['dest']})
        return json.dumps(e, ensure_ascii=False)

    def describe_plan(self, e):
        c = self.catalog
        lines = [render(c, 'msg.plan_components', {'components': ', '.join(render(c, 'component.' + x) for x in e.get('components', [])) or render(c, 'component.none')})]
        for s in e.get('skipped_components', []):
            lines.append('  - ' + render(c, 'component.' + s['id']) + ': ' + render(c, s['reason'], s.get('params')))
        for d in e.get('downloads', []):
            lines.append('  ' + render(c, 'msg.plan_download', {
                'name': d['name'], 'mb': round(d['bytes'] / 1e6), 'url': d['url'], 'sha256': d['sha256'],
                'license': d['license'], 'dest': d['dest']}))
        if e.get('run'):
            lines.append('  ' + render(c, 'msg.plan_run', {'steps': ', '.join(render(c, 'step.' + s) for s in e['run'])}))
        d = e.get('disk')
        if d:
            params = {'need': human_bytes(d['need']), 'peak': human_bytes(d['peak_hd']), 'final': human_bytes(d['final'])}
            lines.append('  ' + render(c, 'msg.plan_disk_hd' if d['peak_hd'] else 'msg.plan_disk', params))
        t = e.get('time')
        if t:
            lines.append('  ' + render(c, 'msg.plan_time', {'min': t['minutes_min'], 'max': t['minutes_max']}))
        return '\n'.join(lines)

    def step(self, step, state, **fields):
        return self.emit('step', step=step, state=state, **fields)

    def progress(self, step, phase, done=None, total=None, unit=None, force=False):
        """A progress event, at most one per PROGRESS_EVERY per phase unless it is the first or the last."""
        now = time.monotonic()
        key = (step, phase)
        last = self.last_progress.get(key)
        final = total is not None and done is not None and done >= total
        if last is not None and not final and not force and now - last < PROGRESS_EVERY:
            return None
        self.last_progress[key] = now
        fields = {'step': step, 'phase': phase}
        if done is not None:
            fields['done'] = done
        if total is not None:
            fields['total'] = total
        if unit:
            fields['unit'] = unit
        if self.text:
            fields['final'] = True       # text mode prints each one that passed the throttle
        ev = self.emit('progress', **fields)
        return ev

    def warning(self, id, **params):
        return self.emit('warning', id=id, params=params)

    def error(self, id, params=None, step=None):
        fields = {'id': id, 'params': params or {}}
        if step:
            fields['step'] = step
        return self.emit('error', **fields)

    def close(self):
        if self.log_file:
            self.log_file.close()
            self.log_file = None
