"""Command line (docs/INSTALLER.md):

    python -m rgsetup --source PATH --dest DIR [--preset faithful|recommended|everything]
                      [--components textures,movies,prompts_font] [--yes] [--estimate] [--verify] [--gentle]
                      [--temp DIR] [--lang en] [--format json|text] [--settings-file FILE]

A first install needs --source (an ISO, BIN/CUE, CHD or an extracted disc folder) and --dest. On a folder with
rg-install.json (resume after a cancel or a failure, or an update with a newer setup) only --dest is needed, and
--components / --preset add components. stdout: one JSON event per line (events.py); the log file has the same in words.
Exit code: 0 done, 1 failed, 2 bad command line, 3 cancelled (Ctrl+C, Ctrl+Break or SIGTERM).
"""
import argparse
import os
import signal
import sys
import time
from pathlib import Path

from . import disc as discmod
from . import download, hostcheck, plan as planmod, settings, state as statefile
from . import layout as layouts
from .events import Reporter
from .runner import Context, SetupError, run_steps
from .steps import (check_dest, BuildStep, ExtractStep, HdStep, MoviesStep, TexturesStep, all_steps, disc_current, pylibs_ok,
                    source_stamp, tool_ready, toolchain_key)

EXIT = {'ok': 0, 'failed': 1, 'usage': 2, 'cancelled': 3}
PRESET_NAMES = tuple(planmod.PRESETS)
STEP_IDS = [s.id for s in all_steps()]


class Parser(argparse.ArgumentParser):
    def error(self, message):
        rep = Reporter()
        rep.error('err.usage', {'detail': message})
        rep.emit('end', result='failed', seconds=0)
        sys.exit(2)


def build_parser():
    ap = Parser(prog='python -m rgsetup', description=__doc__.strip().splitlines()[0], add_help=True)
    ap.add_argument('--source', '--iso', dest='source', type=Path,
                    help='your disc: an .iso, .bin/.cue, .chd, or an extracted disc folder')
    ap.add_argument('--dest', type=Path, required=True, help='the install folder')
    ap.add_argument('--preset', choices=PRESET_NAMES, help='faithful (game only), recommended (default: game + HD textures '
                                                           '+ font and Xbox prompts) or everything (adds HD movies)')
    ap.add_argument('--components', help='comma list of: textures, movies, prompts_font (replaces the preset; on an '
                                         'existing install it adds to what is there)')
    ap.add_argument('--yes', action='store_true', help='answer yes to every question, downloads included')
    ap.add_argument('--estimate', action='store_true', help='check and report; change and download nothing')
    ap.add_argument('--verify', action='store_true', help='hash every kept output again (with --estimate: report only)')
    ap.add_argument('--gentle', action='store_true', help='lowest CPU and GPU priority: slower, the PC stays usable')
    ap.add_argument('--temp', type=Path, help='temp folder (default: work/setup in the install folder)')
    ap.add_argument('--lang', default='en', help='language of the log and --format text (a text/<lang>.json catalog)')
    ap.add_argument('--format', choices=('json', 'text'), default='json', help='json: events on stdout (default); '
                                                                              'text: the same in words')
    ap.add_argument('--settings-file', type=Path, help='the game settings.ini to write defaults to (default: the one the '
                                                       'game reads; RG_SETTINGS_FILE)')
    ap.add_argument('--clean-work', action='store_true', help='delete the HD builder work files when each HD step is done')
    ap.add_argument('--gpu', type=int, help='Vulkan device index for the upscaler (default: the best)')
    ap.add_argument('--tools-on-path', action='store_true', help='developers: build with the toolchain already on PATH')
    ap.add_argument('--redo', help='comma list of steps to run again: ' + ', '.join(STEP_IDS))
    # trials, maintainers and tests
    ap.add_argument('--limit', type=int, default=0, help='TRIAL: upscale only N textures (tools/hd --limit)')
    ap.add_argument('--limit-movies', type=int, default=0, help='TRIAL: only the N smallest movies')
    ap.add_argument('--only', help=argparse.SUPPRESS)
    ap.add_argument('--skip', help=argparse.SUPPRESS)
    return ap


def cancel(signum, frame):
    raise KeyboardInterrupt


class SetupContext(Context):
    """The run's facts, beyond the step bookkeeping of runner.Context."""

    def __init__(self, layout, dest, temp, reporter, options, state=None):
        super().__init__(layout, dest, temp, reporter, options, state=state, verify=options.verify)
        self.known = discmod.load_known(layout.hashes_json)
        self.known_digest = discmod.known_digest(self.known)
        self.movie_count = sum(1 for p in self.known['files'] if p.upper().endswith('.MVI'))
        self.source_path = options.source
        self.source_kind = None
        self.host = {}
        self.components = ()
        self.skipped_components = []
        self.estimate = {}
        self.temp_need = 0
        self.downloads = []
        self.source_stamp = source_stamp(layout)

    # --- the source
    def identify_source(self, rep):
        """Validate what can be validated before extracting. Returns the info dict; SetupError for a problem."""
        kind, path, known = self.source_kind, self.source_path, self.known
        try:
            if kind == 'iso':
                return {'kind': kind, **discmod.identify_iso(path, known)}
            if kind == 'folder':
                return {'kind': kind, **discmod.validate_folder(path, known)}
            if kind == 'bin_cue':
                plan = discmod.bincue_plan(path, known)
                if plan[0] == 'iso':
                    return {'kind': kind, **discmod.identify_iso(plan[1], known)}
                _, bin_path, size, offset, start, count = plan
                with open(bin_path, 'rb') as f:        # the first sector only; the whole image after conversion
                    f.seek(start * size)
                    head = f.read(size)
                if size >= discmod.RAW_SECTOR and head[:12] != discmod.SYNC:
                    raise discmod.DiscProblem('err.disc_bin_sector', sector=start)
                if count * 2048 != known['iso']['size']:
                    raise discmod.DiscProblem('err.disc_bin_size', size=count * 2048, expected=known['iso']['size'])
                return {'kind': kind, 'serial': None, 'files': 0, 'hashed': 0}
            with open(path, 'rb') as f:                 # chd
                if f.read(8) != b'MComprHD':
                    raise discmod.DiscProblem('err.chd_unreadable', path=str(path), detail='not a CHD file')
            return {'kind': kind, 'serial': None, 'files': 0, 'hashed': 0}
        except discmod.DiscProblem as e:
            raise SetupError(e.id, **e.params) from None

    # --- chdman
    def chdman_path(self):
        env = os.environ.get('RG_CHDMAN')
        if env and Path(env).is_file():
            return Path(env)
        d = self.layout.tool_dir('chdman')
        exe = d / download.CHDMAN['exe']
        stamp = d / '.rg-sha256'
        if exe.is_file() and stamp.is_file() and stamp.read_text().strip() == download.CHDMAN['sha256']:
            return exe
        return None

    def fetch_chdman(self, rep):
        """Download MAME's release (pinned, SHA-256 verified), keep chdman.exe and the licence texts. The consent was
        given for this download in the plan (--yes)."""
        spec = download.CHDMAN
        lay = self.layout
        dl = self.temp / 'downloads'
        archive = download.fetch(spec, dl / Path(spec['url']).name, lambda d, t: rep.progress('source', 'download_chdman', d, t, 'bytes'))
        rep.progress('source', 'unpack_chdman', 0, 1, 'steps', force=True)
        target = lay.tool_dir('chdman')
        work = self.temp / 'chdman-unpack'
        download.patiently(lambda: __import__('shutil').rmtree(work, ignore_errors=True))
        work.mkdir(parents=True, exist_ok=True)
        if not download.unpack_sfx(archive, work, ('chdman.exe', 'COPYING')):
            raise SetupError('err.tool_missing', exe='chdman.exe', name=spec['name'], dir=str(target))
        target.mkdir(parents=True, exist_ok=True)
        import shutil
        shutil.copy2(work / 'chdman.exe', target / 'chdman.exe')
        for name in ('COPYING',):
            if (work / name).is_file():
                shutil.copy2(work / name, target / name)
        (target / '.rg-sha256').write_text(spec['sha256'] + '\n', encoding='ascii')
        shutil.rmtree(work, ignore_errors=True)
        download.patiently(lambda: archive.unlink(missing_ok=True))
        rep.progress('source', 'unpack_chdman', 1, 1, 'steps', force=True)
        return target / 'chdman.exe'


def detect_host(layout, dest, kind):
    return {
        'windows64': hostcheck.is_64bit_windows() or os.environ.get('RG_SETUP_ANY_OS') == '1',
        'cpu': hostcheck.cpu_features(),
        'dest_kind': kind,
        'vulkan': hostcheck.vulkan_devices(),
    }


# ---- adoption: an install made before the setup existed (Install.bat, Build-HD-Pack.bat) has no rg-install.json

def adopt_existing(ctx):
    """With no state file, the outputs already in place count as done for the steps whose own check passes. They are
    recorded with adopted=true when the run finishes; --redo and --verify examine them again."""
    lay, o = ctx.layout, ctx.options
    if ctx.state['steps'] or not lay.in_place:
        return
    adopted = []
    for s in ctx.steps:
        if s.always or s.id in ('source',) or s.id not in ('extract', 'toolchain', 'build', 'hd_textures', 'hd_movies', 'prompts_font'):
            continue
        if s.id == 'toolchain' and o.tools_on_path:
            continue
        if s.id in ('hd_textures', 'hd_movies') and (o.limit or o.limit_movies):
            continue
        try:
            ok = s.current(ctx, {})
            if s.id == 'build' and ok:
                ok = lay.game_exe.stat().st_mtime >= ctx.source_stamp[1] - 2
            if s.id == 'prompts_font':
                have = settings.present_keys(settings.settings_path(o.settings_file).read_text(encoding='utf-8')
                                             if settings.settings_path(o.settings_file).is_file() else '')
                ok = all(k in have for k in settings.PROMPTS_FONT)
        except OSError:
            ok = False
        if ok:
            ctx.state['steps'][s.id] = {'key': ctx.keys[s.id], 'release': lay.release, 'adopted': True, 'finished': None}
            adopted.append(s.id)
    if adopted:
        ctx.reporter.log(f'adopted from the folder as it is: {", ".join(adopted)}')
        ctx.adopted = adopted


def adopted_components(ctx):
    got = []
    for c, step in (('textures', 'hd_textures'), ('movies', 'hd_movies'), ('prompts_font', 'prompts_font')):
        if step in ctx.state['steps'] and ctx.state['steps'][step].get('adopted'):
            got.append(c)
    return got


# ---- the plan

def pending_downloads(ctx):
    """What would be downloaded, for the steps that will run."""
    lay = ctx.layout
    runs = ctx.runs
    specs = {}

    def spec(key):
        if not specs:
            specs.update(planmod.download_specs(str(lay.root), download.CHDMAN))
        return specs[key]
    out = []

    def add(spec, needed_by):
        out.append({'id': spec['id'], 'name': spec['name'], 'url': spec['url'], 'bytes': spec['bytes'],
                    'sha256': spec['sha256'], 'license': spec['license'], 'dest': spec['dest'], 'needed_by': needed_by})
    if runs.get('toolchain'):
        for t in download.load_toolchain():
            if not tool_ready(lay, t):
                add({'id': t['name'], 'name': t['label'], 'url': t['url'], 'bytes': t['mb'] * 1_000_000,
                     'sha256': t['sha256'], 'license': t['license'], 'dest': os.path.join('toolchain', t['name'])}, 'build')
    if runs.get('source') and ctx.source_kind == 'chd' and ctx.chdman_path() is None:
        add(spec('chdman'), 'source')
    hd = [s for s in ('hd_textures', 'hd_movies') if runs.get(s)]
    if hd:
        hdm = planmod.hd_module(str(lay.root))
        if not (lay.tool_dir('realesrgan') / hdm.REALESRGAN['exe']).is_file():
            add(spec('realesrgan'), hd[0])
        if runs.get('hd_movies') and not (lay.tool_dir('ffmpeg') / 'bin' / 'ffmpeg.exe').is_file():
            import shutil
            if not (shutil.which('ffmpeg') and shutil.which('ffprobe')):
                add(spec('ffmpeg'), 'hd_movies')
    if runs.get('hd_textures') and not pylibs_ok(lay):
        add(spec('pylibs'), 'hd_textures')
    return out


def make_plan_event(ctx, steps):
    runs = ctx.runs
    hd_pending = [c for c in ctx.components if c in ('textures', 'movies') and runs.get(planmod.COMPONENTS[c]['step'])]
    convert = bool(runs.get('extract') and ctx.source_kind in ('chd', 'bin_cue') and
                   (ctx.source_kind == 'chd' or discmod.bincue_plan(ctx.source_path, ctx.known)[0] == 'convert'))
    est = planmod.estimate(ctx.components, extract=bool(runs.get('extract')), build=bool(runs.get('build')),
                           conversion=convert, hd_pending=hd_pending, clean_work=ctx.options.clean_work)
    ctx.estimate = est
    ctx.temp_need = (ctx.known['iso']['size'] if convert else 0) + planmod.DOWNLOAD_BYTES
    ctx.downloads = pending_downloads(ctx)
    return {
        'components': list(ctx.components),
        'preset': planmod.preset_of(ctx.components),
        'skipped_components': ctx.skipped_components,
        'region': ctx.known['serial'],
        'source': {'kind': ctx.source_kind},
        'run': [s.id for s in steps if ctx.runs[s.id]],
        'skip': {s.id: ctx.reasons[s.id] for s in steps if not ctx.runs[s.id]},
        'downloads': ctx.downloads,
        'disk': {'need': est['need'], 'game': est['game'], 'peak_hd': est['peak_hd'], 'final': est['final'],
                 'per_component': est['per_component'], 'temp_need': ctx.temp_need},
        'time': {'minutes_min': est['minutes_min'], 'minutes_max': est['minutes_max'],
                 'note_id': 'msg.time_note'},
    }


def log_dir_for(args, layout, kind):
    """Where the human log goes. A read-only run (--estimate), or a folder that is not (yet) ours, gets nothing written
    in it: not even a log."""
    if args.estimate or kind not in ('in_place', 'ours'):
        import tempfile
        return Path(tempfile.gettempdir()) / 'rg-setup-logs'
    return (args.temp or layout.default_temp) / 'logs'


def ask_yes(question):
    try:
        return input(question + ' [y/N] ').strip().lower() in ('y', 'yes')
    except EOFError:
        return False


def main(argv=None, root=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    t0 = time.monotonic()
    layout = layouts.Layout(args.dest, root=root or layouts.ROOT)
    dest = layout.dest
    temp = (args.temp.resolve() if args.temp else layout.default_temp)
    kind = layouts.classify_dest(dest, layout.root)
    args.dest_kind = kind      # as it was before the log file was created in it
    log = log_dir_for(args, layout, kind) / time.strftime('setup-%Y%m%d-%H%M%S.log')
    rep = Reporter(log_path=log, text=(args.format == 'text'), lang=args.lang)
    result = 'failed'
    try:
        result = run(args, layout, dest, temp, log, rep)
    except KeyboardInterrupt:
        result = 'cancelled'
    finally:
        rep.emit('end', result='failed' if result == 'usage' else result, seconds=round(time.monotonic() - t0, 1))
        rep.close()
    return EXIT[result]


def run(args, layout, dest, temp, log, rep):
    try:
        for opt, ids in (('only', args.only), ('skip', args.skip), ('redo', args.redo)):
            bad = [x for x in (ids or '').split(',') if x and x not in STEP_IDS]
            if bad:
                rep.error('err.usage', {'detail': f'--{opt}: unknown step {", ".join(bad)}'})
                return 'usage'
        try:
            asked = planmod.parse_components(args.components)
        except planmod.PlanError as e:
            rep.error('err.usage', {'detail': f'--components: unknown component {e}; use textures, movies, prompts_font'})
            return 'usage'
        try:
            state = statefile.load(dest)
        except statefile.StateError as e:
            rep.error(e.id, {'path': str(e.path)})
            return 'failed'
        ctx = SetupContext(layout, dest, temp, rep, args, state=state)
        # the disc input
        if args.source:
            try:
                ctx.source_kind = discmod.sniff(args.source)
            except discmod.DiscProblem as e:
                rep.error(e.id, e.params)
                return 'failed'
        steps = all_steps()
        ctx.steps = steps
        ctx.host = detect_host(layout, dest, args.dest_kind)
        ctx.force = set(x for x in (args.redo or '').split(',') if x)
        ctx.compute_keys()
        adopt_existing(ctx)
        # what the folder already has: the recorded components, or (a folder from before the setup) what is in place
        recorded = None
        if state:
            recorded = state.get('components') or []
        elif ctx.state['steps']:
            recorded = adopted_components(ctx)
        chosen = planmod.resolve_components(args.preset, asked, recorded)
        # the graphics card: HD components need one; without it they are left out, the game is not
        ctx.components, ctx.skipped_components = filter_for_gpu(ctx, chosen)
        rep.emit('start', release=layout.release, mode=ctx.mode, dest=str(dest), log=str(log),
                 steps=[s.id for s in steps], estimate=args.estimate, verify=args.verify, lang=args.lang,
                 adopted=sorted(k for k, v in ctx.state['steps'].items() if v.get('adopted')))

        def announce(planned):
            check_dest(ctx)
            event = make_plan_event(ctx, planned)
            rep.emit('plan', **event)
            if ctx.downloads and not args.estimate and not args.yes:
                if args.format == 'text' and sys.stdin and sys.stdin.isatty():
                    if not ask_yes(f'Download {", ".join(d["name"] for d in ctx.downloads)} now?'):
                        raise SetupError('err.consent_needed', names=', '.join(d['name'] for d in ctx.downloads))
                else:
                    raise SetupError('err.consent_needed', names=', '.join(d['name'] for d in ctx.downloads))
        ctx.announce = announce
        ctx.state['components'] = sorted(set(ctx.state.get('components') or []) | set(ctx.components)) if not args.estimate else ctx.state.get('components', [])
        result = run_steps(steps, ctx, only=set((args.only or '').split(',')) - {''} or None,
                           skip=set((args.skip or '').split(',')) - {''} or None, report_only=args.estimate)
        return result
    except SetupError as e:
        rep.error(e.id, e.params)
        return 'failed'


def filter_for_gpu(ctx, chosen):
    """(components to install, [{'id', 'reason', 'params'}] for those left out). HD components need a usable Vulkan
    graphics card; one already built (recorded) stays, whatever this PC has."""
    devices = ctx.host['vulkan']
    gpu = hostcheck.usable_gpu(devices)
    keep, skipped = [], []
    for c in chosen:
        spec = planmod.COMPONENTS[c]
        built = spec['step'] in ctx.state['steps']
        if spec['gpu'] and gpu is None and not built:
            reason = 'warn.hd_no_vulkan' if devices is None else 'warn.hd_no_gpu'
            skipped.append({'id': c, 'reason': reason, 'params': {'devices': ', '.join(d['name'] for d in devices or []) or '-'}})
        else:
            keep.append(c)
    if gpu is not None and gpu['type'] == 'integrated':
        ctx.reporter.log('only an integrated graphics device was found: the HD steps will be slow')
    return tuple(keep), skipped
