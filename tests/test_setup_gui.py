"""The setup window's logic (guilogic.py) and its text, headless; the screens themselves are `python -m rgsetup.gui --selftest`."""
import ast
import json
import re
import unittest
from pathlib import Path

import setup_fakes as fk
from rgsetup import gui, guilogic as gl, plan

PKG = fk.REPO / 'tools' / 'setup' / 'rgsetup'
CATALOG = json.loads((PKG / 'text' / 'en.json').read_text(encoding='utf-8'))


def literals(path, fn_names):
    """Every string literal passed first to one of the named calls (cat('ui.x'), self.cat('ui.x'))."""
    tree = ast.parse(path.read_text(encoding='utf-8'))
    out = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            name = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, 'id', '')
            if name in fn_names and node.args and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str):
                out.add(node.args[0].value)
    return out


class Text(unittest.TestCase):
    def test_every_window_id_is_in_the_catalog(self):
        ids = set()
        for f in ('gui.py', 'guilogic.py'):
            ids |= literals(PKG / f, {'cat', 'cat_'})
        missing = sorted(i for i in ids if not i.endswith('.') and i not in CATALOG)
        self.assertEqual(missing, [])

    def test_dynamic_ids_exist(self):
        for name in ('install', 'resume', 'update'):
            self.assertIn('ui.run.' + name, CATALOG)
        for c in plan.COMPONENT_ORDER:
            self.assertIn('component.' + c, CATALOG)
            self.assertIn('component.' + c + '.desc', CATALOG)
        for p in plan.PRESETS:
            self.assertIn('preset.' + p + '.desc', CATALOG)
        for r in ('current', 'not_needed', 'not_chosen'):
            self.assertIn('reason.' + r, CATALOG)
        for unit in ('bytes', 'percent', 'files', 'steps', 'tools', 'batches', 'movies', 'other'):
            self.assertIn('ui.amount_' + unit, CATALOG)

    def test_no_command_line_words_in_window_text(self):
        for k, v in CATALOG.items():
            if k.startswith('ui.'):
                self.assertNotRegex(v, r'--\w|\bCLI\b|\bargv\b|Traceback|\bexception\b', k)

    def test_times_and_sizes(self):
        cat = gl.Catalog('en')
        self.assertEqual(gl.minutes_text(cat, 20, 30), '20 to 30 minutes')
        self.assertEqual(gl.minutes_text(cat, 90, 90), 'about 90 minutes')
        self.assertEqual(gl.minutes_text(cat, 131, 211), 'about 2 to 3.5 hours')
        self.assertEqual(gl.minutes_text(cat, 0, 0), 'no waiting')
        self.assertEqual(gl.gb(6 * 2**30), '6.0')
        self.assertEqual(gl.gb(212 * 2**30), '212')


class FormLogic(unittest.TestCase):
    def test_commands(self):
        f = gl.Form(source='D:/rg.iso', dest='C:/RG')
        self.assertEqual(f.args('probe')[-3:], ['--estimate', '--preset', 'everything'])
        a = f.args('run')
        self.assertIn('--yes', a)
        self.assertEqual(a[a.index('--components') + 1], 'textures,prompts_font')
        self.assertEqual(a[a.index('--source') + 1], 'D:/rg.iso')
        self.assertNotIn('--estimate', a)
        f.set_preset('faithful')
        self.assertEqual(f.args('estimate')[f.args('estimate').index('--components') + 1], '')
        f.gentle = True
        self.assertIn('--gentle', f.args('run'))

    def test_presets_and_ticks(self):
        f = gl.Form()
        f.toggle('movies', True)
        self.assertEqual(f.preset, 'everything')
        f.toggle('textures', False)
        self.assertEqual(f.preset, 'custom')
        f.set_preset('faithful')
        self.assertEqual(f.parts, set())

    def test_problems(self):
        self.assertEqual(gl.Form(dest='').problems(True), ['ui.need_dest'])
        self.assertEqual(gl.Form(dest='C:/x').problems(False), ['ui.need_source'])
        self.assertEqual(gl.Form(dest='C:/x', source='Z:/nothere.iso').problems(False), ['ui.missing_source'])
        self.assertEqual(gl.Form(dest='C:/x').problems(True), [])


class Folding(unittest.TestCase):
    def events(self, *e):
        m = gl.Model()
        for x in e:
            m.apply(x)
        return m

    def test_run_and_end(self):
        m = self.events({'type': 'start', 'steps': ['check', 'extract'], 'mode': 'install', 'dest': 'C:/x', 'log': 'L'},
                        {'type': 'plan', 'run': ['check', 'extract'], 'skip': {}},
                        {'type': 'step', 'step': 'check', 'state': 'started'},
                        {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1},
                        {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 1, 'total': 4, 'unit': 'bytes'})
        self.assertEqual(m.states, {'check': 'done', 'extract': 'running'})
        self.assertEqual(m.fraction(), 0.25)
        m.apply({'type': 'end', 'result': 'ok', 'seconds': 3})
        m.finish(0)
        self.assertEqual(m.result, 'ok')

    def test_a_dead_process_is_failed_or_cancelled(self):
        m = self.events({'type': 'start', 'steps': ['check', 'extract']}, {'type': 'step', 'step': 'extract', 'state': 'started'})
        m.finish(-1)
        self.assertEqual((m.result, m.states['extract'], m.error[0]), ('failed', 'failed', 'ui.err_died'))
        m = self.events({'type': 'start', 'steps': ['check']})
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')

    def test_error_names_the_step_and_unavailable_parts(self):
        m = self.events({'type': 'start', 'steps': ['check', 'build']},
                        {'type': 'plan', 'run': [], 'skip': {}, 'skipped_components': [
                            {'id': 'textures', 'reason': 'warn.hd_no_gpu', 'params': {'devices': 'x'}}]},
                        {'type': 'error', 'id': 'err.space', 'params': {}, 'step': 'check'})
        self.assertEqual(m.states['check'], 'failed')
        self.assertIn('textures', m.unavailable())

    def test_log_lines_are_words(self):
        cat = gl.Catalog('en')
        line = gl.describe_event(cat, {'type': 'progress', 'step': 'build', 'phase': 'compile', 'done': 3, 'total': 9, 'unit': 'files'})
        self.assertEqual(line, 'Compiling the game  3 of 9 files')
        self.assertEqual(gl.describe_event(cat, {'type': 'step', 'step': 'extract', 'state': 'skipped', 'reason': 'current'}),
                         'Unpacking the disc: skipped (already done)')


class Selftest(unittest.TestCase):
    def test_every_screen_builds(self):
        import os
        if os.name != 'nt':
            self.skipTest('Tk screens are checked on Windows')
        import subprocess
        import sys
        r = subprocess.run([sys.executable, '-m', 'rgsetup.gui', '--selftest'], cwd=str(PKG.parent), capture_output=True,
                           text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
