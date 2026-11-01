"""Resume keys: fresh install, resume, update and cancel are one rule (rgsetup/runner.py)."""
import io
import json
import tempfile
import unittest
from pathlib import Path

import setup_fakes  # noqa: F401
from rgsetup import events, state as statefile
from rgsetup.runner import Context, SetupError, Step, run_steps


class FakeLayout:
    def __init__(self, release='1.0.0'):
        self.release = release


class Recorder(Step):
    """A step with a settable key whose output can be 'deleted'."""
    def __init__(self, id, key='k', transient=False, always=False, log=None, fail=None, cancel=False, output=True):
        self.id, self._key, self.transient, self.always = id, key, transient, always
        self.log, self.fail, self.cancel, self.output = log if log is not None else [], fail, cancel, output
        self.verified = True
        self.verify_calls = 0

    def key(self, ctx):
        return f'{self._key}|{ctx.keys.get("a", "")}' if self.id != 'a' else self._key

    def current(self, ctx, record):
        return self.output

    def verify(self, ctx, record):
        self.verify_calls += 1
        return self.verified

    def run(self, ctx):
        self.log.append(self.id)
        if self.fail:
            raise SetupError(self.fail, why='test')
        if self.cancel:
            raise KeyboardInterrupt
        return {'extra': 1}


class Harness(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.dest = Path(self.tmp.name) / 'inst'

    def go(self, steps, release='1.0.0', verify=False, **kw):
        out = io.StringIO()
        rep = events.Reporter(out=out)
        ctx = Context(FakeLayout(release), self.dest, self.dest / 't', rep, None, state=statefile.load(self.dest), verify=verify)
        result = run_steps(steps, ctx, **kw)
        evs = [json.loads(x) for x in out.getvalue().splitlines()]
        return result, ctx, evs

    def states(self, evs):
        return {(e['step'], e['state']) for e in evs if e['type'] == 'step'}


class Resume(Harness):
    def test_a_fresh_run_runs_everything_and_records_it(self):
        log = []
        result, ctx, evs = self.go([Recorder('a', log=log), Recorder('b', log=log)])
        self.assertEqual(result, 'ok')
        self.assertEqual(log, ['a', 'b'])
        data = statefile.load(self.dest)
        self.assertEqual(set(data['steps']), {'a', 'b'})
        self.assertEqual(data['steps']['b']['extra'], 1)
        self.assertEqual(data['release'], '1.0.0')
        self.assertEqual(data['format'], 1)

    def test_a_second_run_skips_finished_steps(self):
        self.go([Recorder('a'), Recorder('b')])
        log = []
        result, ctx, evs = self.go([Recorder('a', log=log), Recorder('b', log=log)])
        self.assertEqual(log, [])
        self.assertIn(('a', 'skipped'), self.states(evs))
        skipped = [e for e in evs if e['type'] == 'step' and e['state'] == 'skipped']
        self.assertTrue(all(e['reason'] == 'current' for e in skipped))
        self.assertEqual(ctx.mode, 'resume')

    def test_a_changed_key_runs_that_step_and_the_ones_after_that_read_it(self):
        self.go([Recorder('a', key='1'), Recorder('b')])
        log = []
        self.go([Recorder('a', key='2', log=log), Recorder('b', log=log)])
        self.assertEqual(log, ['a', 'b'])         # b's key includes a's

    def test_a_changed_key_of_a_late_step_leaves_the_early_ones(self):
        self.go([Recorder('a'), Recorder('b', key='1')])
        log = []
        self.go([Recorder('a', log=log), Recorder('b', key='2', log=log)])
        self.assertEqual(log, ['b'])

    def test_missing_output_runs_the_step_again_even_with_the_same_key(self):
        self.go([Recorder('a')])
        log = []
        self.go([Recorder('a', log=log, output=False)])
        self.assertEqual(log, ['a'])

    def test_a_failure_keeps_what_finished_and_the_next_run_starts_there(self):
        log = []
        result, ctx, evs = self.go([Recorder('a', log=log), Recorder('b', log=log, fail='err.space'), Recorder('c', log=log)])
        self.assertEqual(result, 'failed')
        self.assertEqual(log, ['a', 'b'])
        err = [e for e in evs if e['type'] == 'error'][0]
        self.assertEqual((err['id'], err['step'], err['params']), ('err.space', 'b', {'why': 'test'}))
        self.assertEqual(set(statefile.load(self.dest)['steps']), {'a'})
        log2 = []
        result, _, _ = self.go([Recorder('a', log=log2), Recorder('b', log=log2), Recorder('c', log=log2)])
        self.assertEqual((result, log2), ('ok', ['b', 'c']))

    def test_a_cancel_is_reported_as_cancelled_and_keeps_finished_steps(self):
        log = []
        result, _, evs = self.go([Recorder('a', log=log), Recorder('b', log=log, cancel=True), Recorder('c', log=log)])
        self.assertEqual(result, 'cancelled')
        self.assertEqual(log, ['a', 'b'])
        self.assertEqual(set(statefile.load(self.dest)['steps']), {'a'})
        self.assertFalse([e for e in evs if e['type'] == 'error'])

    def test_an_unexpected_exception_becomes_an_internal_error_event(self):
        class Boom(Recorder):
            def run(self, ctx):
                raise ZeroDivisionError('x')
        result, _, evs = self.go([Boom('a')])
        self.assertEqual(result, 'failed')
        err = [e for e in evs if e['type'] == 'error'][0]
        self.assertEqual(err['id'], 'err.internal')
        self.assertEqual(err['params']['exception'], 'ZeroDivisionError')

    def test_always_steps_run_every_time_and_are_not_recorded(self):
        log = []
        self.go([Recorder('chk', always=True, log=log)])
        self.go([Recorder('chk', always=True, log=log)])
        self.assertEqual(log, ['chk', 'chk'])
        self.assertNotIn('chk', statefile.load(self.dest)['steps'])


class Transient(Harness):
    def test_a_transient_step_runs_only_when_the_next_one_does(self):
        self.go([Recorder('a'), Recorder('t', transient=True), Recorder('x')])
        log = []
        _, _, evs = self.go([Recorder('a', log=log), Recorder('t', transient=True, log=log, output=False),
                             Recorder('x', log=log)])
        self.assertEqual(log, [])
        self.assertIn(('t', 'skipped'), self.states(evs))
        reasons = {e['step']: e['reason'] for e in evs if e['type'] == 'step' and e['state'] == 'skipped'}
        self.assertEqual(reasons['t'], 'not_needed')
        log = []
        self.go([Recorder('a', log=log), Recorder('t', transient=True, log=log, output=False), Recorder('x', key='new', log=log)])
        self.assertEqual(log, ['t', 'x'])


class UpdateAndVerify(Harness):
    def test_a_newer_release_is_an_update_and_verifies_kept_output(self):
        self.go([Recorder('a'), Recorder('b')], release='1.0.0')
        a, b = Recorder('a'), Recorder('b')
        result, ctx, _ = self.go([a, b], release='1.1.0')
        self.assertEqual(ctx.mode, 'update')
        self.assertEqual((a.verify_calls, b.verify_calls), (1, 1))      # the other release's output is hash-checked
        self.assertEqual(statefile.load(self.dest)['release'], '1.1.0')

    def test_same_release_does_not_verify_without_the_flag(self):
        self.go([Recorder('a')])
        a = Recorder('a')
        self.go([a])
        self.assertEqual(a.verify_calls, 0)
        a = Recorder('a')
        self.go([a], verify=True)
        self.assertEqual(a.verify_calls, 1)

    def test_a_failed_verification_runs_the_step_again_and_warns(self):
        self.go([Recorder('a')])
        log = []
        a = Recorder('a', log=log)
        a.verified = False
        result, _, evs = self.go([a], verify=True)
        self.assertEqual(log, ['a'])
        self.assertEqual([e['id'] for e in evs if e['type'] == 'warning'], ['warn.output_changed'])

    def test_force_runs_a_step_even_when_current(self):
        self.go([Recorder('a'), Recorder('b')])
        log = []
        out = io.StringIO()
        ctx = Context(FakeLayout(), self.dest, self.dest / 't', events.Reporter(out=out), None, state=statefile.load(self.dest))
        ctx.force = {'b'}
        run_steps([Recorder('a', log=log), Recorder('b', log=log)], ctx)
        self.assertEqual(log, ['b'])

    def test_only_and_skip_leave_steps_out(self):
        log = []
        self.go([Recorder('a', log=log), Recorder('b', log=log), Recorder('c', log=log)], only={'b'})
        self.assertEqual(log, ['b'])
        log.clear()
        self.go([Recorder('a', log=log), Recorder('b', log=log), Recorder('c', log=log)], skip={'a'})
        self.assertEqual(log, ['c'])      # a is left out, b is current


class EstimateMode(Harness):
    def test_report_only_runs_nothing_but_read_only_steps_and_writes_no_state(self):
        class Check(Recorder):
            read_only = True
        log = []
        result, _, evs = self.go([Check('chk', always=True, log=log), Recorder('a', log=log)], report_only=True)
        self.assertEqual(result, 'ok')
        self.assertEqual(log, ['chk'])
        self.assertFalse(self.dest.exists())


class StateFile(Harness):
    def test_unreadable_and_newer_files(self):
        self.dest.mkdir(parents=True)
        (self.dest / statefile.NAME).write_text('{not json')
        with self.assertRaises(statefile.StateError) as cm:
            statefile.load(self.dest)
        self.assertEqual(cm.exception.id, 'err.state_unreadable')
        (self.dest / statefile.NAME).write_text(json.dumps({'format': 99, 'steps': {}}))
        with self.assertRaises(statefile.StateError) as cm:
            statefile.load(self.dest)
        self.assertEqual(cm.exception.id, 'err.state_newer')

    def test_save_is_atomic_and_never_leaves_the_temp_name(self):
        self.dest.mkdir(parents=True)
        statefile.save(self.dest, statefile.empty('1.0.0'))
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()), [statefile.NAME])

    def test_nothing_secret_or_disc_derived_is_stored(self):
        self.go([Recorder('a')])
        text = (self.dest / statefile.NAME).read_text()
        for word in ('sha256', 'password', 'key"'):
            self.assertNotIn(word, text.lower().replace('"key"', ''))


if __name__ == '__main__':
    unittest.main()
