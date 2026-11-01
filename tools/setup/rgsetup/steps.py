"""The setup's steps, in order (docs/INSTALLER.md):

    check         always, read-only: this PC, the folder, the disc, the space, the downloads
    source        (transient) CHD or BIN/CUE to an ISO in the temp folder; validate the disc image or folder
    extract       the disc into disc/ (setup.py extract; a folder is copied and hashed)
    toolchain     the pinned build tools into toolchain/ (the current download-the-toolchain flow)
    build         setup.py tools, recompile, build: the game
    hd_textures   tools/hd/build_hd_pack.py --mode 1 (HD textures)
    hd_movies     tools/hd/build_hd_pack.py --mode 2 (HD movies)
    prompts_font  settings.ini defaults for the Xbox prompts and the modern font; nothing is built
    finish        always: the summary, the temp folder removed

A step key fingerprints its inputs. A step whose recorded key is the current one, and whose output is still in place,
is skipped; otherwise it runs, and the HD builder and setup.py resume inside it with their own state. The sibling
project's installer (totk-pc-recomp/installer) is where this structure comes from.
"""
import csv
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import disc as discmod
from . import download, hostcheck, layout as layouts, plan as planmod, proc, settings
from .layout import SERIAL
from .runner import SetupError, Step

GiB = 1 << 30

# The names build_hd_pack.py reports with --events: they make the phase ids phase.<step>.stage_<name> (and
# download_<tool>, unpack_<tool>) that text/en.json must hold.
HD_STAGES = ('extract', 'tim2', 'im3', 'keys', 'merge', 'upscale', 'fp16', 'pack', 'verify', 'textures', 'movies', 'publish')
HD_TOOLS = ('realesrgan', 'ffmpeg')


# ---- fingerprints

def sha(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(str(p).encode('utf-8'))
        h.update(b'\1')
    return h.hexdigest()


def tools_fingerprint(layout):
    """SHA-256 over the HD builder's code and data lists: a changed class, model or list makes the HD steps run again."""
    h = hashlib.sha256()
    base = layout.root / 'tools' / 'hd'
    for p in sorted(base.rglob('*')):
        if p.is_file() and '__pycache__' not in p.parts and p.suffix in ('.py', '.txt'):
            h.update(p.relative_to(base).as_posix().encode() + b'\0' + hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


SOURCE_DIRS = ('src', 'cmake', 'config', 'patches', 'thirdparty/dq8-gfx', 'thirdparty/PS2Recomp/ps2xRuntime',
               'thirdparty/PS2Recomp/ps2xRecomp')
SOURCE_FILES = ('CMakeLists.txt', 'setup.py')


def source_stamp(layout):
    """(fingerprint, newest mtime) of the files the game is built from: path, size and time, no content."""
    h = hashlib.sha256()
    newest = 0.0
    entries = []
    for rel in SOURCE_FILES:
        entries.append(layout.root / rel)
    for rel in SOURCE_DIRS:
        base = layout.root / rel
        for folder, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ('.git', '__pycache__')]
            entries.extend(Path(folder) / f for f in files)
    for p in sorted(entries):
        try:
            st = p.stat()
        except OSError:
            continue
        newest = max(newest, st.st_mtime)
        h.update(f'{p.relative_to(layout.root).as_posix()}|{st.st_size}|{int(st.st_mtime)}\n'.encode())
    return h.hexdigest(), newest


def toolchain_key():
    return sha('toolchain', json.dumps(download.load_toolchain(), sort_keys=True))


def progress_writer(rep, step, phase, unit='bytes'):
    return lambda done, total: rep.progress(step, phase, done, total, unit)


# ---- check

def check_dest(ctx, warn=False):
    """The install folder: empty, ours, or (for now) the program's own. Raises SetupError; with warn, a read-only run
    on another folder gets a warning instead of an error."""
    opt, lay = ctx.options, ctx.layout
    if ctx.host['dest_kind'] == 'foreign':
        raise SetupError('err.dest_foreign', dest=str(ctx.dest))
    if not lay.in_place:
        # Phase 2 gives the install folder a layout of its own; until then the game is built where the program is.
        if opt.estimate or opt.verify:
            if warn:
                ctx.reporter.warning('warn.dest_not_program_folder', dest=str(ctx.dest), program=str(lay.root))
        else:
            raise SetupError('err.dest_not_program_folder', dest=str(ctx.dest), program=str(lay.root))


class CheckStep(Step):
    id = 'check'
    always = True
    read_only = True

    def key(self, ctx):
        return ''

    def run(self, ctx):
        rep, lay, opt = ctx.reporter, ctx.layout, ctx.options
        host = ctx.host
        if os.environ.get('RG_SETUP_TEST_SLEEP'):       # test hook: a step that takes a while, for the cancel test
            end = time.monotonic() + float(os.environ['RG_SETUP_TEST_SLEEP'])
            while time.monotonic() < end:
                time.sleep(0.1)
        if not hostcheck.python_ok():
            raise SetupError('err.python_old', version='.'.join(map(str, sys.version_info[:3])))
        if not host['windows64']:
            raise SetupError('err.windows_64')
        if not host['cpu']['sse41']:
            raise SetupError('err.cpu_sse41')

        check_dest(ctx, warn=True)
        if ctx.runs.get('build') and not opt.tools_on_path:
            problem = layouts.path_problem(lay.root)
            if problem:
                raise SetupError(problem[0], path=str(lay.root), **problem[1])

        # the disc
        if ctx.runs.get('extract'):
            if not ctx.source_path:
                if not opt.estimate:
                    raise SetupError('err.source_required')
                rep.warning('warn.no_source')       # an estimate before the disc is chosen: the plan assumes a valid one
                ctx.shared['disc'] = None
            else:
                self.validate_source(ctx, rep)
        elif ctx.source_path and (opt.verify or opt.estimate):
            self.validate_source(ctx, rep)

        # the graphics card
        for item in ctx.skipped_components:
            rep.warning(item['reason'], **(item.get('params') or {}))

        # the space
        need = ctx.estimate['need']
        temp_need = ctx.temp_need
        free = hostcheck.free_bytes(ctx.dest)
        same = hostcheck.drive_of(ctx.dest) == hostcheck.drive_of(ctx.temp)
        total_need = need + (temp_need if same else 0)
        tfree = free if same else hostcheck.free_bytes(ctx.temp)
        rep.emit('space', dest=str(ctx.dest), need=total_need, free=free, temp=str(ctx.temp), temp_need=temp_need,
                 temp_free=tfree, same_disk=same)
        if free < total_need:
            raise SetupError('err.space', need=round(total_need / GiB, 1), free=round(free / GiB, 1), drive=str(ctx.dest.drive or ctx.dest))
        if not same and tfree < temp_need:
            raise SetupError('err.space_temp', need=round(temp_need / GiB, 1), free=round(tfree / GiB, 1), temp=str(ctx.temp))
        return {}

    def validate_source(self, ctx, rep):
        rep.progress('check', 'disc', 0, 1, 'files', force=True)
        ctx.shared['disc'] = ctx.identify_source(rep)
        rep.progress('check', 'disc', 1, 1, 'files', force=True)


# ---- source

class SourceStep(Step):
    id = 'source'
    transient = True

    def key(self, ctx):
        return sha('source', ctx.known_digest, ctx.source_kind or '')

    def current(self, ctx, record):
        return False     # its output is in the temp folder (or nothing): it runs whenever extract does

    def run(self, ctx):
        rep, kind = ctx.reporter, ctx.source_kind
        if not ctx.source_path:
            raise SetupError('err.source_required')
        known = ctx.known
        src = Path(ctx.source_path)
        try:
            if kind == 'iso':
                ctx.shared['iso'] = src
            elif kind == 'bin_cue':
                plan = discmod.bincue_plan(src, known)
                if plan[0] == 'iso':
                    ctx.shared['iso'] = plan[1]
                else:
                    out = ctx.temp / 'disc.iso'
                    out.unlink(missing_ok=True)
                    discmod.convert_bincue(plan, out, progress_writer(rep, 'source', 'convert'))
                    ctx.shared['iso'] = out
                    ctx.shared['temp_iso'] = out
            elif kind == 'chd':
                ctx.shared['iso'] = self.from_chd(ctx, src)
                ctx.shared['temp_iso'] = ctx.shared['iso']
            if kind == 'folder':
                if 'disc' not in ctx.shared:
                    ctx.shared['disc'] = ctx.identify_source(rep)
            else:
                rep.progress('source', 'validate', 0, 1, 'files', force=True)
                info = discmod.identify_iso(ctx.shared['iso'], known)
                ctx.shared['disc'] = {'kind': kind, **info}
                if ctx.options.verify:
                    got = discmod.verify_image_hash(ctx.shared['iso'], known, progress_writer(rep, 'source', 'image_hash'))
                    ctx.shared['image_hashes'] = {k: got[k] for k in ('crc32', 'sha1')}
                rep.progress('source', 'validate', 1, 1, 'files', force=True)
        except discmod.DiscProblem as e:
            raise SetupError(e.id, **e.params) from None
        d = ctx.shared['disc']
        ctx.state['region'] = known['serial']
        ctx.state['inputs']['disc'] = {'kind': kind, 'name': src.name, 'size': src.stat().st_size if src.is_file() else None,
                                       'serial': d['serial'], 'validated': True, 'digest': ctx.known_digest,
                                       **ctx.shared.get('image_hashes', {})}
        return {'kind': kind}

    def from_chd(self, ctx, src):
        rep = ctx.reporter
        exe = ctx.chdman_path()
        if not exe:
            exe = ctx.fetch_chdman(rep)
        out = ctx.temp / 'disc.iso'
        out.unlink(missing_ok=True)
        log = ctx.temp / 'logs' / 'source-chdman.log'
        info_lines = []
        rc, _ = proc.run([exe, 'info', '-i', str(src)], on_line=info_lines.append, log_path=log)
        if rc != 0:
            raise SetupError('err.chd_unreadable', path=str(src), detail=' | '.join(info_lines[-3:]))
        text = '\n'.join(info_lines)
        is_dvd = 'DVD' in text and ('Tag=\'DVD \'' in text or "Tag='DVD" in text or 'DVD' in text.split('Metadata')[-1])
        rep.progress('source', 'chd', 0, 1, 'files', force=True)

        def percent(line):
            m = re.search(r'(\d+(?:\.\d+)?)%\s+complete', line)
            if m:
                rep.progress('source', 'chd', int(float(m.group(1)) * 10), 1000, 'permille')
        if is_dvd:
            rc, last = proc.run([exe, 'extractdvd', '-i', str(src), '-o', str(out), '-f'], on_line=percent, log_path=log)
            if rc != 0:
                raise SetupError('err.chd_extract', detail=' | '.join(last[-3:]))
            return out
        cue, binf = ctx.temp / 'disc.cue', ctx.temp / 'disc.bin'
        rc, last = proc.run([exe, 'extractcd', '-i', str(src), '-o', str(cue), '-ob', str(binf), '-f'], on_line=percent, log_path=log)
        if rc != 0:
            raise SetupError('err.chd_extract', detail=' | '.join(last[-3:]))
        plan = discmod.bincue_plan(cue, ctx.known)
        if plan[0] == 'iso':
            binf.replace(out)
        else:
            discmod.convert_bincue(plan, out, progress_writer(rep, 'source', 'convert'))
            binf.unlink(missing_ok=True)
        cue.unlink(missing_ok=True)
        return out


# ---- extract

def disc_current(layout, known):
    """disc/ holds the extracted disc: the manifest, every file of hashes.json present with its size."""
    man = layout.disc / 'manifest.json'
    if not man.is_file():
        return False
    try:
        data = json.loads(man.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    if data.get('version') != layouts.VERSION or not (layout.disc / known['elf']).is_file():
        return False
    for path, info in known['files'].items():
        p = layout.disc / path
        try:
            if p.stat().st_size != info['size']:
                return False
        except OSError:
            return False
    return True


def disc_rehash(layout, known, progress=None):
    """Every file of disc/ against hashes.json (~6.3 GiB read). Returns the list of paths that differ."""
    bad = []
    total = sum(i['size'] for i in known['files'].values())
    done = 0
    for path, info in sorted(known['files'].items()):
        p = layout.disc / path
        h = hashlib.sha256()
        try:
            with open(p, 'rb') as f:
                for blk in iter(lambda: f.read(8 << 20), b''):
                    h.update(blk)
                    done += len(blk)
                    if progress:
                        progress(done, total)
        except OSError:
            bad.append(path)
            continue
        if h.hexdigest() != info['sha256']:
            bad.append(path)
    return bad


class ExtractStep(Step):
    id = 'extract'

    def key(self, ctx):
        return sha('extract', ctx.known_digest)

    def current(self, ctx, record):
        return disc_current(ctx.layout, ctx.known)

    def verify(self, ctx, record):
        bad = disc_rehash(ctx.layout, ctx.known, progress_writer(ctx.reporter, 'extract', 'verify'))
        if bad:
            ctx.reporter.log(f'disc/ differs from hashes.json: {bad[:5]}')
        return not bad

    def run(self, ctx):
        rep, lay = ctx.reporter, ctx.layout
        total = sum(i['size'] for i in ctx.known['files'].values())
        disc = ctx.shared.get('disc') or {}
        if ctx.source_kind == 'folder':
            self.copy_folder(ctx, disc, total)
        else:
            iso = ctx.shared['iso']
            log = ctx.temp / 'logs' / 'extract-setup.log'

            def on_line(line):
                m = re.match(r'\s*(\d+)%', line)
                if m:
                    rep.progress('extract', 'copy', int(m.group(1)) * total // 100, total, 'bytes')
                elif line.startswith('PROBLEM'):
                    rep.log(line)
            rc, last = proc.run(lay.setup_py_command('extract', '--iso', str(iso), '--out', str(lay.disc), '--force'),
                                on_line=on_line, log_path=log, cwd=lay.root, env=self._env(), gentle=ctx.options.gentle)
            if rc != 0:
                raise SetupError('err.extract_failed', detail=' | '.join(last[-3:]))
            rep.progress('extract', 'copy', total, total, 'bytes', force=True)
        temp_iso = ctx.shared.get('temp_iso')
        if temp_iso:
            Path(temp_iso).unlink(missing_ok=True)
        return {'files': len(ctx.known['files'])}

    def _env(self):
        env = dict(os.environ)
        env['PYTHONUTF8'] = '1'
        env['PYTHONUNBUFFERED'] = '1'
        return env

    def copy_folder(self, ctx, disc, total):
        """Copy the validated folder into disc/, hashing every byte again while copying, then write the manifest the
        way setup.py extract does (the layer starts are not known for a folder, so no sector numbers)."""
        rep, lay, known = ctx.reporter, ctx.layout, ctx.known
        mapping = disc['map']
        out = lay.disc
        out.mkdir(parents=True, exist_ok=True)
        (out / 'manifest.json').unlink(missing_ok=True)
        manifest = {'version': layouts.VERSION, 'image': None, 'files': {}}
        done = 0
        problems = []
        for path, info in sorted(known['files'].items()):
            src = mapping[path]
            dest = out / path
            in_place = src.resolve() == dest.resolve()
            h = hashlib.sha256()
            if in_place:
                with open(src, 'rb') as f:
                    for blk in iter(lambda: f.read(8 << 20), b''):
                        h.update(blk)
                        done += len(blk)
                        rep.progress('extract', 'copy', done, total, 'bytes')
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                part = dest.with_name(dest.name + '.part')
                with open(src, 'rb') as f, open(part, 'wb') as g:
                    for blk in iter(lambda: f.read(8 << 20), b''):
                        h.update(blk)
                        g.write(blk)
                        done += len(blk)
                        rep.progress('extract', 'copy', done, total, 'bytes')
                os.replace(part, dest)
            if h.hexdigest() != info['sha256']:
                problems.append(path)
            manifest['files'][path] = {'size': info['size'], 'sha256': h.hexdigest(),
                                       'copies': [{'layer': layer} for layer in info['layers']]}
        if problems:
            raise SetupError('err.extract_hash', count=len(problems), first=problems[0])
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        rep.progress('extract', 'copy', total, total, 'bytes', force=True)


# ---- toolchain

def tool_ready(layout, tool):
    d = layout.tool_dir(tool['name'])
    stamp = d / '.rg-sha256'
    try:
        return (d / tool['exe']).is_file() and stamp.read_text().strip() == tool['sha256']
    except OSError:
        return False


class ToolchainStep(Step):
    id = 'toolchain'

    def wanted(self, ctx):
        return not ctx.options.tools_on_path

    def key(self, ctx):
        return toolchain_key()

    def current(self, ctx, record):
        return all(tool_ready(ctx.layout, t) for t in download.load_toolchain())

    def run(self, ctx):
        rep, lay = ctx.reporter, ctx.layout
        tools = download.load_toolchain()
        dl = lay.toolchain / 'downloads'
        for i, tool in enumerate(tools):
            rep.progress('toolchain', 'tools', i, len(tools), 'tools', force=True)
            if tool_ready(lay, tool):
                continue
            file = dl / (tool['name'] + tool['ext'])
            spec = {'name': tool['label'], 'url': tool['url'], 'sha256': tool['sha256'], 'bytes': tool['mb'] * 1_000_000}
            download.fetch(spec, file, progress_writer(rep, 'toolchain', 'download'))
            rep.progress('toolchain', 'unpack', i, len(tools), 'tools', force=True)
            d = lay.tool_dir(tool['name'])
            download.patiently(lambda d=d: shutil.rmtree(d) if d.exists() else None)
            d.mkdir(parents=True, exist_ok=True)
            download.unpack(file, d)
            if not (d / tool['exe']).is_file():
                raise SetupError('err.tool_missing', exe=tool['exe'], name=tool['label'], dir=str(lay.toolchain))
            (d / '.rg-sha256').write_text(tool['sha256'] + '\n', encoding='ascii')
            download.patiently(lambda file=file: file.unlink(missing_ok=True))
        shutil.rmtree(dl, ignore_errors=True)
        rep.progress('toolchain', 'tools', len(tools), len(tools), 'tools', force=True)
        return {}


# ---- build

class BuildStep(Step):
    id = 'build'

    def key(self, ctx):
        stamp, _ = ctx.source_stamp
        return sha('build', ctx.layout.release, stamp, ctx.keys.get('extract', ''), '' if ctx.options.tools_on_path else toolchain_key())

    def current(self, ctx, record):
        lay = ctx.layout
        return lay.game_exe.is_file() and lay.launcher.is_file()

    def verify(self, ctx, record):
        lay = ctx.layout
        return lay.game_exe.is_file() and lay.game_exe.stat().st_size > 1_000_000

    def run(self, ctx):
        rep, lay = ctx.reporter, ctx.layout
        running = hostcheck.game_processes(lay.dest)
        if running:
            raise SetupError('err.game_running', pids=', '.join(map(str, running)))
        env = layouts.build_env(lay, ctx.options.tools_on_path)
        log = ctx.temp / 'logs' / 'build-setup.log'
        if not ((lay.root / 'thirdparty' / 'PS2Recomp' / 'CMakeLists.txt').is_file()
                and (lay.root / 'thirdparty' / 'imgui' / 'imgui.h').is_file()):
            if not (lay.root / '.git').exists():
                raise SetupError('err.sources_missing')
            rep.progress('build', 'submodules', 0, 1, 'steps', force=True)
            git = lay.tool_dir('git') / 'cmd' / 'git.exe'
            rc, last = proc.run([str(git) if git.is_file() else 'git', '-C', str(lay.root), 'submodule', 'update', '--init',
                                 '--recursive'], log_path=log, env=env)
            if rc != 0:
                raise SetupError('err.submodules', detail=' | '.join(last[-3:]))
        phases = [('tools', ['tools']), ('recompile', ['recompile', '--disc', str(lay.disc)]), ('compile', ['build'])]
        for name, args in phases:
            rep.progress('build', name, 0, 1, 'steps', force=True)

            def on_line(line, name=name):
                m = re.match(r'\[(\d+)/(\d+)\]', line)      # ninja: [12/896] Building ...
                if m:
                    rep.progress('build', name, int(m.group(1)), int(m.group(2)), 'files')
            rc, last = proc.run(lay.setup_py_command(*args), on_line=on_line, log_path=log, cwd=lay.root, env=env,
                                gentle=ctx.options.gentle)
            if rc != 0:
                raise SetupError('err.build_failed', phase=name, detail=' | '.join(last[-3:]), log=str(log))
            rep.progress('build', name, 1, 1, 'steps', force=True)
        if not lay.game_exe.is_file() or not lay.launcher.is_file():
            raise SetupError('err.build_no_exe', exe=str(lay.game_exe))
        return {'exe_size': lay.game_exe.stat().st_size}


# ---- HD

def pylibs_ok(layout):
    env = dict(os.environ, PYTHONPATH=str(layout.toolchain / 'pylibs'), PYTHONNOUSERSITE='1')
    r = subprocess.run([str(layout.python()), '-I', '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import numpy, PIL',
                        str(layout.toolchain / 'pylibs')], capture_output=True, env=env, creationflags=proc.NO_WINDOW)
    return r.returncode == 0


class HdStep(Step):
    component = ''
    mode = 0

    def wanted(self, ctx):
        return self.component in ctx.components

    def key(self, ctx):
        o = ctx.options
        return sha(self.id, ctx.keys.get('extract', ''), tools_fingerprint(ctx.layout),
                   o.limit if self.component == 'textures' else o.limit_movies)

    def hd_args(self, ctx):
        lay, o = ctx.layout, ctx.options
        args = ['--mode', str(self.mode), '--yes', '--events', '--disc', str(lay.disc), '--work', str(lay.hd_work),
                '--out', str(lay.textures)]
        if o.gentle:
            args.append('--gentle')
        if o.gpu is not None:
            args += ['--gpu', str(o.gpu)]
        if o.clean_work:
            args.append('--clean-work')
        return args

    def run(self, ctx):
        rep, lay = ctx.reporter, ctx.layout
        step = self.id
        if not (lay.disc / 'manifest.json').is_file() and not disc_current(lay, ctx.known):
            raise SetupError('err.hd_needs_disc')
        self.ensure_deps(ctx)
        args = self.hd_args(ctx) + self.extra_args(ctx)
        log = ctx.temp / 'logs' / f'{step}.log'
        state = {'error': None, 'tools': set()}
        poll = self.make_poller(ctx)

        def on_line(line):
            if line.startswith('@@RG '):
                try:
                    ev = json.loads(line[5:])
                except ValueError:
                    return
                self.on_event(ctx, ev, state)
                return
            m = re.search(r'\bbatch (\d+)/(\d+)\b', line)
            if m:
                rep.progress(step, 'upscale', int(m.group(1)), int(m.group(2)), 'batches')

        env = self.child_env(ctx)
        rc, last = proc.run(lay.hd_command(*args), on_line=on_line, log_path=log, cwd=lay.root, env=env,
                            gentle=ctx.options.gentle, heartbeat=3, on_idle=poll)
        if rc != 0:
            err = state['error']
            if err:
                raise SetupError('err.hd_failed', step=err['step'], what=err['what'], hint=err.get('hint', ''), log=str(log))
            raise SetupError('err.hd_failed', step='?', what=' | '.join(last[-3:]), hint='', log=str(log))
        return self.record_fields(ctx)

    def extra_args(self, ctx):
        return []

    def child_env(self, ctx):
        env = dict(os.environ)
        env['PYTHONUTF8'] = '1'
        env['PYTHONUNBUFFERED'] = '1'
        return env

    def make_poller(self, ctx):
        return None

    def ensure_deps(self, ctx):
        pass

    def on_event(self, ctx, ev, state):
        rep, step = ctx.reporter, self.id
        kind = ev.get('ev')
        if kind == 'stage':
            if ev['state'] == 'started':
                rep.progress(step, 'stage_' + ev['name'], 0, 1, 'steps', force=True)
            elif ev['state'] in ('done', 'skipped'):
                rep.progress(step, 'stage_' + ev['name'], 1, 1, 'steps', force=True)
        elif kind == 'download':
            tool = ev['tool']
            if ev['state'] == 'progress':
                rep.progress(step, 'download_' + tool, ev['done'], ev['total'], 'bytes')
            elif ev['state'] == 'unpacking':
                rep.progress(step, 'unpack_' + tool, 0, 1, 'steps', force=True)
            elif ev['state'] == 'done':
                rep.progress(step, 'unpack_' + tool, 1, 1, 'steps', force=True)
        elif kind == 'error':
            state['error'] = ev

    def record_fields(self, ctx):
        return {}


class TexturesStep(HdStep):
    id = 'hd_textures'
    component = 'textures'
    mode = 1

    def extra_args(self, ctx):
        return ['--limit', str(ctx.options.limit)] if ctx.options.limit else []

    def ensure_deps(self, ctx):
        lay, rep = ctx.layout, ctx.reporter
        if pylibs_ok(lay):
            return
        rep.progress(self.id, 'deps', 0, 1, 'steps', force=True)
        libs = lay.toolchain / 'pylibs'
        req = lay.root / 'tools' / 'hd' / 'requirements.txt'
        rc, last = proc.run([str(lay.python()), '-m', 'pip', 'install', '--disable-pip-version-check', '--target', str(libs),
                             '-r', str(req)], log_path=ctx.temp / 'logs' / 'hd_textures-pip.log',
                            env=dict(os.environ, PYTHONNOUSERSITE='1'))
        if rc != 0 or not pylibs_ok(lay):
            raise SetupError('err.pip_failed', detail=' | '.join(last[-3:]))
        rep.progress(self.id, 'deps', 1, 1, 'steps', force=True)

    def current(self, ctx, record):
        man = ctx.layout.textures / 'pack-manifest.csv'
        rep = ctx.layout.replacements
        if not man.is_file() or not rep.is_dir():
            return False
        rows = read_pack_manifest(man)
        if not rows:
            return False
        random.seed(2)
        for r in random.sample(rows, min(40, len(rows))):
            if not (rep / r).is_file():
                return False
        return True

    def verify(self, ctx, record):
        rep = ctx.layout.replacements
        rows = read_pack_manifest(ctx.layout.textures / 'pack-manifest.csv')
        return bool(rows) and all((rep / r).is_file() for r in rows)

    def record_fields(self, ctx):
        rows = read_pack_manifest(ctx.layout.textures / 'pack-manifest.csv')
        return {'files': len(rows), 'trial': bool(ctx.options.limit)}


def read_pack_manifest(path):
    try:
        with open(path, newline='', encoding='utf-8') as f:
            return [r['path'] for r in csv.DictReader(f) if r.get('path')]
    except (OSError, KeyError):
        return []


class MoviesStep(HdStep):
    id = 'hd_movies'
    component = 'movies'
    mode = 2

    def extra_args(self, ctx):
        return ['--limit-movies', str(ctx.options.limit_movies)] if ctx.options.limit_movies else []

    def expected(self, ctx):
        return ctx.options.limit_movies or ctx.movie_count

    def current(self, ctx, record):
        d = ctx.layout.movies
        if not d.is_dir():
            return False
        n = sum(1 for p in d.rglob('*.mkv') if p.stat().st_size > 0)
        return n >= self.expected(ctx)

    def verify(self, ctx, record):
        return self.current(ctx, record)

    def make_poller(self, ctx):
        state_file = ctx.layout.hd_work / 'movies' / 'state.json'
        total = self.expected(ctx)
        last = {'n': -1, 't': 0.0}

        def poll():
            if time.time() - last['t'] < 3:
                return
            last['t'] = time.time()
            try:
                files = json.loads(state_file.read_text(encoding='utf-8')).get('files', {})
            except (OSError, ValueError):
                return
            n = sum(1 for v in files.values() if v.get('status') == 'done')
            if n != last['n']:
                last['n'] = n
                ctx.reporter.progress(self.id, 'movies', min(n, total), total, 'movies')
        return poll

    def record_fields(self, ctx):
        n = sum(1 for p in ctx.layout.movies.rglob('*.mkv')) if ctx.layout.movies.is_dir() else 0
        return {'movies': n, 'trial': bool(ctx.options.limit_movies)}


# ---- prompts_font

class PromptsFontStep(Step):
    id = 'prompts_font'

    def wanted(self, ctx):
        return 'prompts_font' in ctx.components

    def key(self, ctx):
        return sha('prompts_font', json.dumps(settings.PROMPTS_FONT, sort_keys=True))

    def current(self, ctx, record):
        return True      # written once; a later choice in the game's menu is the player's and is never overwritten

    def run(self, ctx):
        path = settings.settings_path(ctx.options.settings_file)
        added = settings.write_defaults(path, settings.PROMPTS_FONT)
        ctx.reporter.progress(self.id, 'write', 1, 1, 'files', force=True)
        if not added:
            ctx.reporter.log(f'{path}: button_prompts and font_style are already set; left as they are')
        return {'added': added}


# ---- finish

class FinishStep(Step):
    id = 'finish'
    always = True

    def run(self, ctx):
        rep, lay = ctx.reporter, ctx.layout
        present = {}
        if (lay.replacements.is_dir() and any(lay.replacements.iterdir())):
            present['textures'] = True
        if lay.movies.is_dir() and any(lay.movies.rglob('*.mkv')):
            present['movies'] = True
        ctx.state['components'] = [c for c in planmod.COMPONENT_ORDER if c in set(ctx.state.get('components', [])) | set(ctx.components)]
        # Whatever the temp folder holds besides the logs is scratch.
        if ctx.temp.is_dir():
            for child in ctx.temp.iterdir():
                if child.name != 'logs':
                    shutil.rmtree(child, ignore_errors=True) if child.is_dir() else child.unlink(missing_ok=True)
        rep.progress('finish', 'summary', 1, 1, 'steps', force=True)
        ctx.shared['present'] = present
        return {}


def all_steps():
    return [CheckStep(), SourceStep(), ExtractStep(), ToolchainStep(), BuildStep(), TexturesStep(), MoviesStep(),
            PromptsFontStep(), FinishStep()]
