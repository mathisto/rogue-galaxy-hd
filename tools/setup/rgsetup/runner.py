"""Which steps run, in what order, and the bookkeeping around them: fresh install, resume and update are one rule.

Each step has a key: a fingerprint of everything its output depends on, computed before anything runs (a step's key
includes the keys of the steps it reads from, so a change upstream reaches every step below it). A step is skipped
when rg-install.json records it with the same key and its output still matches the record (Step.current, cheap).
Otherwise it runs. A transient step (its output lives in the temp folder and only the next step reads it) runs only
when the next step runs. `always` steps (check, finish) run every time.

Update mode is the same rule with a newer release: a step whose key does not mention the release (the disc's
extraction) keeps its output, and the build, whose key does, runs again (incrementally). Before keeping the output of a
step recorded by another release (or with --verify), Step.verify (expensive: every hash) is called; a mismatch makes
the step run again.

A failed or cancelled run leaves the finished steps recorded; the next run starts at the first step that is not.
"""
import datetime
import traceback

from . import state as statefile


class SetupError(Exception):
    """A failure with a message id (text/en.json) and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


class Step:
    id = ''
    transient = False
    always = False
    read_only = False     # may run under --estimate (it changes nothing)

    def key(self, ctx):
        """Fingerprint of the step's inputs; may read ctx.keys of earlier steps."""
        return ''

    def wanted(self, ctx):
        """False when the options leave this step out altogether (reported as skipped: not_chosen)."""
        return True

    def current(self, ctx, record):
        """True when the recorded output is still in place (cheap checks only)."""
        return True

    def verify(self, ctx, record):
        """True when the recorded output passes the full check (hashes); called before keeping another release's
        output, and with --verify."""
        return True

    def run(self, ctx):
        """Do the work. Returns fields to record with the step."""
        return {}


class Context:
    def __init__(self, layout, dest, temp, reporter, options, state=None, verify=False):
        self.layout, self.dest, self.temp, self.reporter, self.options = layout, dest, temp, reporter, options
        self.fresh = state is None
        self.state = state or statefile.empty(layout.release)
        self.verify = verify
        self.mode = 'install' if state is None else 'resume' if state.get('release') == layout.release else 'update'
        self.keys, self.runs, self.reasons, self.steps, self.forced = {}, {}, {}, [], set()
        self.shared = {}      # what steps tell later steps of this run (the validated source, the components ...)
        self.announce = None  # called with the planned steps once the plan is final (cli.py emits the plan event)
        self.force = set()    # step ids that must run again (--redo)
        self.only, self.skip = None, None     # maintainer and test options: run just these steps / leave these out

    def record(self, step_id):
        return self.state['steps'].get(step_id)

    def save(self):
        self.dest.mkdir(parents=True, exist_ok=True)
        statefile.save(self.dest, self.state)

    def compute_keys(self):
        for s in self.steps:
            self.keys[s.id] = s.key(self)

    def plan(self, forced=()):
        """Fill keys, runs and reasons. forced: step ids that must run (and those of --redo)."""
        forced = set(forced) | self.force
        self.compute_keys()
        for s in self.steps:
            rec = self.record(s.id)
            if not s.wanted(self):
                self.runs[s.id], self.reasons[s.id] = False, 'not_chosen'
            elif s.always or s.id in forced:
                self.runs[s.id], self.reasons[s.id] = True, ''
            else:
                cur = bool(rec and rec.get('key') == self.keys[s.id] and s.current(self, rec))
                self.runs[s.id], self.reasons[s.id] = (not cur), ('current' if cur else '')
        for i in range(len(self.steps) - 2, -1, -1):
            s, after = self.steps[i], self.steps[i + 1]
            if s.transient and self.runs[s.id] and not self.runs[after.id] and self.reasons[after.id] != 'not_chosen':
                self.runs[s.id], self.reasons[s.id] = False, 'not_needed'
        for s in self.steps:
            if (self.only and s.id not in self.only and not s.always) or (self.skip and s.id in self.skip):
                self.runs[s.id], self.reasons[s.id] = False, 'not_chosen'
        self.forced = forced

    def verify_kept(self):
        """Full check of every kept step output that another release recorded (or of all with --verify). A step that
        fails it is planned to run again. Returns the ids that failed."""
        failed = []
        for s in self.steps:
            rec = self.record(s.id)
            if self.runs[s.id] or self.reasons[s.id] != 'current' or s.always:
                continue
            if not (self.verify or rec.get('release') != self.layout.release):
                continue
            self.reporter.log(f'verifying kept output of {s.id}')
            if not s.verify(self, rec):
                failed.append(s.id)
        if failed:
            for f in failed:
                self.reporter.warning('warn.output_changed', step=f)
            self.plan(self.forced | set(failed))
        return failed


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def run_steps(steps, ctx, only=None, skip=None, report_only=False):
    """Run the plan. Returns 'ok', 'failed' or 'cancelled'; events and the log say why.
    only/skip: step ids (maintainer and test options). report_only (--estimate): plan and report; only read-only
    steps run, and nothing is recorded."""
    rep = ctx.reporter
    ctx.steps = steps
    current = None
    try:
        ctx.only, ctx.skip = only, skip
        ctx.plan()
        ctx.verify_kept()
        if ctx.announce:
            ctx.announce(steps)
        for s in steps:
            if report_only and not s.read_only:
                if not ctx.runs[s.id]:
                    rep.step(s.id, 'skipped', reason=ctx.reasons[s.id])
                continue
            if not ctx.runs[s.id]:
                rep.step(s.id, 'skipped', reason=ctx.reasons[s.id])
                continue
            current = s.id
            rep.step(s.id, 'started')
            t0 = rep.elapsed()
            fields = s.run(ctx) or {}
            seconds = round(rep.elapsed() - t0, 1)
            if not s.always and not report_only:
                ctx.state['steps'][s.id] = {'key': ctx.keys[s.id], 'release': ctx.layout.release,
                                            'finished': now_utc(), 'seconds': seconds, **fields}
                ctx.save()
            rep.step(s.id, 'done', seconds=seconds)
            current = None
        if not report_only:
            ctx.state['release'] = ctx.layout.release
            ctx.save()
        return 'ok'
    except SetupError as e:
        rep.error(e.id, e.params, step=current)
        return 'failed'
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)}, step=current)
        return 'failed'
    except KeyboardInterrupt:
        rep.log(f'cancelled during {current}')
        return 'cancelled'
    except Exception as e:
        rep.log(traceback.format_exc())
        rep.error('err.internal', {'exception': type(e).__name__, 'detail': str(e)}, step=current)
        return 'failed'
