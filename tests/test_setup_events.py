import io
import json
import time
import unittest

import setup_fakes  # noqa: F401  (puts tools/setup on sys.path)
from rgsetup import events


class EventModel(unittest.TestCase):
    def reporter(self, **kw):
        out = io.StringIO()
        return events.Reporter(out=out, **kw), out

    def lines(self, out):
        return [json.loads(x) for x in out.getvalue().splitlines()]

    def test_one_json_object_per_line_with_type_and_time(self):
        rep, out = self.reporter()
        rep.emit('start', release='1.0', mode='install', dest='x', log='l', steps=['check'])
        rep.step('check', 'started')
        rep.step('check', 'done', seconds=1.5)
        rep.step('source', 'skipped', reason='current')
        rep.warning('warn.hd_no_vulkan', devices='-')
        rep.error('err.space', {'need': 1, 'free': 0, 'drive': 'C:'}, step='check')
        rep.emit('end', result='ok', seconds=2)
        got = self.lines(out)
        self.assertEqual([e['type'] for e in got], ['start', 'step', 'step', 'step', 'warning', 'error', 'end'])
        for e in got:
            self.assertIn('t', e)
            self.assertIsInstance(e['t'], float)
        self.assertEqual(got[3], {'type': 'step', 't': got[3]['t'], 'step': 'source', 'state': 'skipped', 'reason': 'current'})
        self.assertEqual(got[4]['id'], 'warn.hd_no_vulkan')
        self.assertEqual(got[5]['step'], 'check')
        self.assertEqual(got[5]['params']['drive'], 'C:')

    def test_error_outside_a_step_has_no_step_key(self):
        rep, out = self.reporter()
        rep.error('err.usage', {'detail': 'x'})
        self.assertNotIn('step', self.lines(out)[0])

    def test_progress_is_throttled_but_first_and_last_always_pass(self):
        rep, out = self.reporter()
        for i in range(1, 100):
            rep.progress('extract', 'copy', i, 100, 'bytes')
        rep.progress('extract', 'copy', 100, 100, 'bytes')
        got = self.lines(out)
        self.assertEqual(got[0]['done'], 1)
        self.assertEqual(got[-1]['done'], 100)
        self.assertLess(len(got), 10)
        self.assertEqual(got[0], {'type': 'progress', 't': got[0]['t'], 'step': 'extract', 'phase': 'copy', 'done': 1,
                                  'total': 100, 'unit': 'bytes'})

    def test_progress_phases_are_throttled_separately(self):
        rep, out = self.reporter()
        rep.progress('build', 'tools', 1, 10, 'files')
        rep.progress('build', 'compile', 1, 10, 'files')
        self.assertEqual(len(self.lines(out)), 2)

    def test_progress_without_a_total(self):
        rep, out = self.reporter()
        rep.progress('hd_movies', 'movies')
        e = self.lines(out)[0]
        self.assertNotIn('total', e)
        self.assertNotIn('done', e)

    def test_log_file_gets_words_not_json(self):
        import tempfile, pathlib
        with tempfile.TemporaryDirectory() as d:
            log = pathlib.Path(d) / 'sub' / 'x.log'
            rep, out = self.reporter(log_path=log)
            rep.step('extract', 'started')
            rep.error('err.space', {'need': 9, 'free': 1, 'drive': 'D:'})
            rep.emit('end', result='failed', seconds=3)
            rep.close()
            text = log.read_text(encoding='utf-8')
        self.assertIn('Unpacking the disc', text)
        self.assertIn('not enough free space on D:', text)
        self.assertIn('[err.space]', text)
        self.assertNotIn('{"type"', text)

    def test_text_format_prints_words_on_stdout(self):
        rep, out = self.reporter(text=True)
        rep.step('build', 'started')
        rep.progress('build', 'compile', 5, 10, 'files')
        rep.emit('end', result='ok', seconds=1)
        text = out.getvalue()
        self.assertIn('== Building the game', text)
        self.assertIn('Compiling the game 5/10 files', text)
        self.assertNotIn('"type"', text)

    def test_render_falls_back_to_the_id(self):
        self.assertEqual(events.render({}, 'err.nope'), 'err.nope')
        self.assertIn('x', events.render({'a': 'needs {b}'}, 'a', {'x': 1}))

    def test_a_translation_overrides_and_english_fills_the_gaps(self):
        import pathlib
        p = events.TEXT_DIR / 'zz.json'
        p.write_text(json.dumps({'step.check': 'Pruefen'}), encoding='utf-8')
        try:
            cat = events.load_catalog('zz')
        finally:
            p.unlink()
        self.assertEqual(cat['step.check'], 'Pruefen')
        self.assertEqual(cat['step.extract'], 'Unpacking the disc')


if __name__ == '__main__':
    unittest.main()
