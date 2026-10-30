#!/usr/bin/env python3
"""Rogue Galaxy Recompiled: builds the HD texture pack and the HD movies from YOUR disc.

Normally started by Build-HD-Pack.bat (which finds Python and the disc image). Nothing from the game is ever
distributed: this reads the disc you extracted with Install.bat (disc/), writes everything under work/hd/, puts the
finished pack in textures/SCUS-97490/replacements and the movies in textures/SCUS-97490/movies, and downloads two tools
(Real-ESRGAN, and FFmpeg if you have none) only after showing the address and asking.

    python tools/hd/build_hd_pack.py                 menu: [1] textures [2] movies [3] both
    python tools/hd/build_hd_pack.py --mode 3 --gentle
    python tools/hd/build_hd_pack.py --estimate      what it would do, how long, how much disk; changes nothing
    python tools/hd/build_hd_pack.py --mode 3 --limit 20 --limit-movies 2     a fast trial (see docs/HD-BUILD.md)

Steps (a finished step is skipped on the next run; the state is work/hd/state.json):
  textures: 1 extract the HD6/DAT archives (with the 512-byte offset fix)   2 decode TIM2 pictures
            3 decode IM3 menu art   4 compute the renderer's texture keys   5 merge and classify
            6 upscale (x4plus-anime; animevideov3 for effects, foliage, clouds; x4plus for stone floors)
            7 repair fp16 blackouts   8 build the pack   9 verify
  movies:   1 decrypt the MVI files and upscale each frame 2x with animevideov3, encode HEVC (watchdog: a stalled
            movie is killed and retried)   2 put the files in textures/SCUS-97490/movies   3 verify
"""
import argparse
import csv
import ctypes
import datetime
import hashlib
import json
import os
import queue
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
IS_WIN = os.name == 'nt'
NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000
IDLE = 0x00000040
SERIAL = 'SCUS-97490'

# The tools downloaded on request. A new pin needs the archive's SHA-256 (GitHub shows it on the release asset).
REALESRGAN = dict(
    name='Real-ESRGAN ncnn-vulkan 20220424 (Windows)', dir='realesrgan', exe='realesrgan-ncnn-vulkan.exe',
    url='https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip',
    sha256='abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d', mb=45,
    license='Real-ESRGAN is BSD-3-Clause (the models in its models/ folder come with the same release)',
    skip=('input.jpg', 'input2.jpg', 'onepiece_demo.mp4'))
FFMPEG = dict(
    name='FFmpeg 8.1.2 essentials build (gyan.dev, GPL: includes x265 and NVENC)', dir='ffmpeg', exe=os.path.join('bin', 'ffmpeg.exe'),
    url='https://github.com/GyanD/codexffmpeg/releases/download/8.1.2/ffmpeg-8.1.2-essentials_build.zip',
    sha256='db580001caa24ac104c8cb856cd113a87b0a443f7bdf47d8c12b1d740584a2ec', mb=105,
    license='FFmpeg is LGPL/GPL; this build is GPL v3 (a separate program run by the builder, not linked into the game)',
    only=('bin/ffmpeg.exe', 'bin/ffprobe.exe', 'LICENSE', 'README.txt'))

# Measured on the author's PC (Ryzen 9 7900X, RTX 5070 Ti, NVMe). Yours will differ; the GPU decides the upscale.
REFERENCE = dict(pictures=11200, movies=89)
GB_TEXTURES = 11     # extracted archives 2.3, decoded pictures 1.5, upscales about 6, working copies; the pack itself is hard links
GB_MOVIES = 11       # decrypted movies 1.6, HEVC results 7, frames in flight 1.5; the published files are hard links
EST_TEXT = ('about 20 to 30 minutes: about 5 of preparation, then about 15 to 25 of upscaling',
            "the author's RTX 5070 Ti")
EST_MOVIES = ('about 1.5 hours', "the author's RTX 5070 Ti; the 89 movies are about 5 hours of film at 30 fps")

LOG_PATH = None
CHILDREN = set()
STATE = None


# ---------------------------------------------------------------- output
def stamp():
    return datetime.datetime.now().strftime('%H:%M:%S')


def say(msg='', file_only=False):
    line = '%s %s' % (stamp(), msg) if msg else ''
    if not file_only:
        print(msg, flush=True)
    if LOG_PATH:
        with open(LOG_PATH, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


class BuildError(Exception):
    def __init__(self, step, what, hint=''):
        super().__init__(what)
        self.step, self.what, self.hint = step, what, hint


def human(sec):
    sec = int(sec)
    if sec < 90:
        return '%d s' % sec
    if sec < 5400:
        return '%d min' % round(sec / 60)
    return '%.1f h' % (sec / 3600)


def ask(question, default=False, yes=False):
    """Yes/no question; --yes answers yes, no console answers the default (no)."""
    if yes:
        return True
    if not sys.stdin or not sys.stdin.isatty():
        return default
    try:
        a = input('%s [%s] ' % (question, 'Y/n' if default else 'y/N')).strip().lower()
    except EOFError:
        return default
    return default if not a else a in ('y', 'yes', 's', 'sim')


# ---------------------------------------------------------------- state
class State:
    def __init__(self, path):
        self.path = path
        self.d = {}
        if os.path.exists(path):
            try:
                self.d = json.load(open(path, encoding='utf-8'))
            except Exception:
                self.d = {}

    def done(self, step):
        return bool(self.d.get(step, {}).get('done'))

    def mark(self, step, **kw):
        self.d[step] = dict(done=True, at=time.strftime('%Y-%m-%d %H:%M:%S'), **kw)
        self.save()

    def clear(self, step=None):
        if step is None or step == 'all':
            self.d = {}
        else:
            self.d.pop(step, None)
        self.save()

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, self.path)


# ---------------------------------------------------------------- child processes
def child_env(extra=None):
    env = dict(os.environ)
    libs = os.path.join(REPO, 'toolchain', 'pylibs')
    if os.path.isdir(libs):
        env['PYTHONPATH'] = libs + os.pathsep + env.get('PYTHONPATH', '')
    env['PYTHONUTF8'] = '1'
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONNOUSERSITE'] = '1'
    ff = os.path.join(REPO, 'toolchain', 'ffmpeg', 'bin')
    if os.path.isdir(ff):
        env['RG_FFMPEG_DIR'] = ff
    if extra:
        env.update(extra)
    return env


def run(cmd, label, gentle=False, forward=True, heartbeat=45, env=None, cwd=None):
    """Runs a child (logging all its output; showing it when forward). A silent child gets a line every
    `heartbeat` seconds. Returns (returncode, last lines). Only processes started here are ever killed."""
    say('  $ ' + ' '.join('"%s"' % c if ' ' in c else c for c in cmd), file_only=True)
    flags = 0
    if IS_WIN:
        flags = NO_WINDOW | (IDLE if gentle else BELOW_NORMAL)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                         errors='replace', env=env or child_env(), cwd=cwd or REPO, creationflags=flags)
    CHILDREN.add(p)
    q = queue.Queue()

    def reader():
        for ln in p.stdout:
            q.put(ln.rstrip('\r\n'))
        q.put(None)
    threading.Thread(target=reader, daemon=True).start()
    t0 = time.time()
    last = []
    quiet_since = time.time()
    try:
        while True:
            try:
                ln = q.get(timeout=heartbeat)
            except queue.Empty:
                say('  ... %s: still working, %s so far' % (label, human(time.time() - t0)))
                continue
            if ln is None:
                break
            last.append(ln)
            del last[:-25]
            if forward:
                say('  ' + ln)
            else:
                say('  ' + ln, file_only=True)
            quiet_since = time.time()
        p.wait()
    finally:
        CHILDREN.discard(p)
        if p.poll() is None:
            p.kill()
    return p.returncode, last


def tool(name, *args):
    return [sys.executable, '-X', 'utf8', os.path.join(HERE, name)] + list(args)


def kill_children():
    for p in list(CHILDREN):
        try:
            if p.poll() is None:
                p.kill()
        except Exception:
            pass


def keep_awake(on):
    if IS_WIN:
        try:
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | (0x00000001 if on else 0))
        except Exception:
            pass


# ---------------------------------------------------------------- csv / hash lists
def read_csv(path):
    with open(path, newline='', encoding='utf-8') as fh:
        return list(csv.DictReader(fh))


def read_hashes(path):
    out = set()
    with open(path, encoding='utf-8') as f:
        for ln in f:
            ln = ln.split('#')[0].strip()
            if ln and ln.lower() != 'hash':
                out.add(ln.split(',')[0].strip())
    return out


def link_or_copy(src, dst):
    if os.path.exists(dst):
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


# ---------------------------------------------------------------- environment checks
class Env:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work or os.environ.get('RG_HD_WORK') or os.path.join(REPO, 'work', 'hd'))
        self.disc = os.path.abspath(a.disc or os.path.join(REPO, 'disc'))
        self.out = os.path.abspath(a.out or os.path.join(REPO, 'textures', SERIAL))
        self.re_dir = os.path.join(REPO, 'toolchain', 'realesrgan')
        self.ff_dir = os.path.join(REPO, 'toolchain', 'ffmpeg', 'bin')
        self.gpu_index = a.gpu

    def p(self, *parts):
        return os.path.join(self.work, *parts)

    @property
    def re_exe(self):
        return os.path.join(self.re_dir, REALESRGAN['exe'])

    def disc_ok(self):
        return all(os.path.exists(os.path.join(self.disc, n)) for n in ('DATA0_0.DAT', 'DATA0_0.HD6', 'DATA0_1.DAT', 'DATA0_1.HD6'))

    def movies_ok(self):
        return os.path.isdir(os.path.join(self.disc, 'MOVIE'))


def find_ffmpeg(env):
    """-> directory holding ffmpeg and ffprobe, or None."""
    for d in (env.ff_dir,):
        if os.path.exists(os.path.join(d, 'ffmpeg.exe')) and os.path.exists(os.path.join(d, 'ffprobe.exe')):
            return d
    a, b = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if a and b:
        return os.path.dirname(a)
    return None


def list_gpus():
    """Names of the video adapters Windows knows (before any download)."""
    if not IS_WIN:
        return []
    try:
        r = subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                            '(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }) -join "|"'],
                           capture_output=True, text=True, timeout=30, creationflags=NO_WINDOW)
        return [n.strip() for n in r.stdout.strip().split('|') if n.strip()]
    except Exception:
        return []


def vulkan_devices(env):
    """Probes the real Vulkan devices through Real-ESRGAN's own start-up listing -> [(index, name)]."""
    if not os.path.exists(env.re_exe):
        return None
    try:
        r = subprocess.run([env.re_exe, '-i', os.path.join(env.work, 'no-such-input'), '-o', os.path.join(env.work, 'probe.png')],
                           capture_output=True, text=True, timeout=120, creationflags=NO_WINDOW if IS_WIN else 0)
    except Exception:
        return []
    seen = []
    for m in re.finditer(r'^\[(\d+) ([^\]]+)\]', (r.stdout or '') + (r.stderr or ''), re.M):
        if (int(m.group(1)), m.group(2)) not in seen:
            seen.append((int(m.group(1)), m.group(2)))
    return seen


def choose_gpu(env, devices):
    """A discrete NVIDIA/AMD device before an Intel or software one, unless --gpu says which."""
    if env.gpu_index is not None:
        return env.gpu_index
    def score(d):
        n = d[1].lower()
        if 'llvmpipe' in n or 'swiftshader' in n or 'microsoft basic' in n or 'warp' in n:
            return -1
        if 'nvidia' in n or 'radeon rx' in n or 'amd radeon' in n or 'geforce' in n:
            return 3
        if 'intel' in n and 'arc' in n:
            return 2
        return 1
    best = max(devices, key=score)
    return best[0] if score(best) > 0 else None


NO_GPU_MESSAGE = '''
No usable Vulkan graphics card was found.

The upscaler (Real-ESRGAN ncnn-vulkan) runs on the graphics card through Vulkan: an NVIDIA, AMD or Intel Arc/Xe card
with a recent driver. The CPU cannot do this job in a sensible time. What to do:
  - update the graphics driver (nvidia.com/drivers, amd.com/support, intel.com/drivers), then run this again;
  - on a laptop with two graphics chips, make sure the faster one is used (Windows Settings > System > Display >
    Graphics), or pass --gpu N with the index this program lists;
  - without a suitable card you can still play: the game works without the HD pack and without the HD movies.
'''


# ---------------------------------------------------------------- downloads (only after the address is shown and a yes)
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def ensure_tool(spec, env, yes, why):
    """Makes sure toolchain/<dir> holds the tool; downloads (SHA-256 verified) after a yes. Returns True when ready."""
    base = os.path.join(REPO, 'toolchain', spec['dir'])
    exe = os.path.join(base, spec['exe'])
    stamp_file = os.path.join(base, '.rg-sha256')
    if os.path.exists(exe) and os.path.exists(stamp_file) and open(stamp_file).read().strip() == spec['sha256']:
        return True
    say()
    say('%s is needed %s.' % (spec['name'], why))
    say('  address : ' + spec['url'])
    say('  size    : about %d MB' % spec['mb'])
    say('  SHA-256 : ' + spec['sha256'] + ' (the download is refused if it differs)')
    say('  licence : ' + spec['license'])
    say('  goes to : ' + base + ' (nothing is installed in Windows)')
    if not ask('Download it now?', False, yes):
        raise BuildError('download', 'the download of %s was not accepted' % spec['name'],
                         'Run this again and answer y, or put the tool in %s yourself.' % base)
    dl = os.path.join(REPO, 'toolchain', 'downloads')
    os.makedirs(dl, exist_ok=True)
    zpath = os.path.join(dl, spec['dir'] + '.zip')
    if not (os.path.exists(zpath) and sha256_file(zpath) == spec['sha256']):
        part = zpath + '.part'
        req = urllib.request.Request(spec['url'], headers={'User-Agent': 'rogue-galaxy-recomp-hd-builder'})
        try:
            with urllib.request.urlopen(req, timeout=60) as r, open(part, 'wb') as out:
                total = int(r.headers.get('Content-Length') or 0)
                got, t0, shown = 0, time.time(), 0
                while True:
                    blk = r.read(1 << 20)
                    if not blk:
                        break
                    out.write(blk)
                    got += len(blk)
                    if time.time() - shown > 5:
                        shown = time.time()
                        say('  downloading %d of %d MB' % (got >> 20, total >> 20))
        except Exception as ex:
            raise BuildError('download', 'downloading %s failed: %s' % (spec['name'], ex),
                             'Check the internet connection and run this again (it starts over).')
        if sha256_file(part) != spec['sha256']:
            os.remove(part)
            raise BuildError('download', 'the file downloaded for %s is not the expected one (its SHA-256 differs)' % spec['name'],
                             'Run this again; if it repeats, something on this network alters downloads. Nothing was unpacked.')
        os.replace(part, zpath)
    say('  unpacking %s' % spec['name'])
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        top = z.namelist()[0].split('/')[0] if spec['dir'] == 'ffmpeg' else ''
        for info in z.infolist():
            if info.is_dir():
                continue
            rel = info.filename[len(top) + 1:] if top and info.filename.startswith(top + '/') else info.filename
            if os.path.basename(rel) in spec.get('skip', ()):
                continue
            if spec.get('only') and rel not in spec['only']:
                continue
            dst = os.path.normpath(os.path.join(base, rel))
            if not dst.startswith(base):
                continue   # never write outside the folder
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(info) as src, open(dst, 'wb') as out:
                shutil.copyfileobj(src, out)
    if not os.path.exists(exe):
        raise BuildError('download', '%s is missing after unpacking' % spec['exe'],
                         'An antivirus may have removed it: restore it from quarantine (or allow %s) and run this again.' % base)
    open(stamp_file, 'w').write(spec['sha256'] + '\n')
    try:
        os.remove(zpath)
    except OSError:
        pass
    return True


# ---------------------------------------------------------------- estimate / preflight
def free_gb(path):
    p = path
    while not os.path.exists(p):
        p = os.path.dirname(p)
    return shutil.disk_usage(p).free / 2 ** 30


def print_plan(env, mode, a):
    say('Rogue Galaxy HD builder')
    say('  folder        : ' + REPO)
    say('  work files    : ' + env.work)
    say('  pack goes to  : ' + os.path.join(env.out, 'replacements') + '   (textures)')
    say('                  ' + os.path.join(env.out, 'movies') + '   (movies)')
    say('  disc          : %s' % (env.disc if env.disc_ok() else env.disc + '  (NOT extracted: run Install.bat, or give --iso)'))
    gp = list_gpus()
    say('  graphics      : %s' % (', '.join(gp) if gp else 'none reported by Windows'))
    ff = find_ffmpeg(env)
    say('  Real-ESRGAN   : %s' % ('ready (toolchain/realesrgan)' if os.path.exists(env.re_exe) else 'not yet downloaded (asked before downloading, %d MB)' % REALESRGAN['mb']))
    say('  FFmpeg        : %s' % (ff if ff else 'not found (asked before downloading, %d MB)' % FFMPEG['mb']))
    need = 0
    if mode in (1, 3):
        need += GB_TEXTURES
    if mode in (2, 3):
        need += GB_MOVIES
    fr = free_gb(env.work)
    say()
    say('Disk space needed on the work/pack drive:')
    if mode in (1, 3):
        say('  textures : about %d GB (extracted archives 2.3, decoded pictures 1.5, upscales about 6; the pack is hard links)' % GB_TEXTURES)
    if mode in (2, 3):
        say('  movies   : about %d GB (decrypted movies 1.6, HEVC results 7, frames in flight; published files are hard links)' % GB_MOVIES)
    say('  total    : about %d GB, %.0f GB free here -> %s' % (need, fr, 'OK' if fr >= need + 3 else 'NOT ENOUGH'))
    say()
    say('Time (a guide, measured on the author\'s Ryzen 9 7900X + RTX 5070 Ti; the graphics card decides the upscale):')
    if mode in (1, 3):
        say('  textures : %s' % EST_TEXT[0])
    if mode in (2, 3):
        say('  movies   : %s' % EST_MOVIES[0])
    say('  An older or smaller graphics card takes several times longer. --gentle lowers the priority (slower, quieter).')
    if a.limit or a.limit_movies:
        say('  TRIAL RUN: %s, %s (the pack is then tiny).' % ('%d pictures' % a.limit if a.limit else 'all pictures', '%d movies' % a.limit_movies if a.limit_movies else 'all movies'))
    say()
    say('Steps (finished ones are skipped; state in %s):' % env.p('state.json'))
    names = [('extract', 'extract the archives'), ('tim2', 'decode TIM2 pictures'), ('im3', 'decode IM3 menu art'),
             ('keys', 'compute texture keys'), ('merge', 'merge and classify')]
    if mode in (1, 3):
        for k, label in names:
            say('  textures  %-8s %-26s %s' % (k, label, 'done' if STATE.done(k) else 'to do'))
        say('  textures  upscale / fp16 / pack / verify  (incremental: only missing pictures are processed)')
    if mode in (2, 3):
        st = env.p('movies', 'state.json')
        n = 0
        if os.path.exists(st):
            try:
                n = sum(1 for v in json.load(open(st))['files'].values() if v.get('status') == 'done')
            except Exception:
                pass
        say('  movies    %d of %d done' % (n, REFERENCE['movies']))


def preflight(env, mode, a):
    """Blocking checks. Raises BuildError with what to do."""
    if not env.disc_ok():
        raise BuildError('disc', 'the disc is not extracted: %s has no DATA0_0.DAT / DATA0_1.DAT' % env.disc,
                         'Run Install.bat first (it extracts your disc image into disc/), or run Build-HD-Pack.bat with your .iso '
                         'dropped on it / --iso "path\\to\\Rogue Galaxy (USA).iso".')
    if mode in (2, 3) and not env.movies_ok():
        raise BuildError('disc', 'disc/MOVIE is missing', 'Run Install.bat first.')
    need = (GB_TEXTURES if mode in (1, 3) else 0) + (GB_MOVIES if mode in (2, 3) else 0)
    fr = free_gb(env.work)
    if fr < need + 3 and not a.skip_space_check:
        raise BuildError('space', 'only %.0f GB free for the work files; this needs about %d GB' % (fr, need),
                         'Free some space, or use --work "D:\\somewhere" to put the work files on another drive '
                         '(the pack itself is hard links when it is on the same drive).')


def prepare_tools(env, mode, a):
    gp = list_gpus()
    if gp and not any(re.search(r'nvidia|amd|radeon|intel|arc', n, re.I) for n in gp):
        say('Warning: Windows lists no NVIDIA, AMD or Intel graphics card (%s).' % ', '.join(gp))
    ensure_tool(REALESRGAN, env, a.yes, 'to upscale the pictures and the movies')
    devs = vulkan_devices(env)
    if not devs:
        raise BuildError('gpu', 'no Vulkan graphics device was found by the upscaler', NO_GPU_MESSAGE.strip())
    env.gpu_index = choose_gpu(env, devs)
    if env.gpu_index is None:
        raise BuildError('gpu', 'only software Vulkan devices were found: %s' % ', '.join(n for _, n in devs), NO_GPU_MESSAGE.strip())
    say('Graphics for the upscale: [%d] %s' % (env.gpu_index, dict(devs)[env.gpu_index]))
    if len(devs) > 1:
        say('  (others: %s; --gpu N chooses)' % ', '.join('[%d] %s' % d for d in devs if d[0] != env.gpu_index))
    if mode in (2, 3):
        if not find_ffmpeg(env):
            ensure_tool(FFMPEG, env, a.yes, 'to read and write the movies (no ffmpeg/ffprobe was found on PATH)')
        ff = find_ffmpeg(env)
        os.environ['RG_FFMPEG_DIR'] = ff
        say('FFmpeg: ' + ff)


# ---------------------------------------------------------------- textures
def step(name, label, env, fn, force=False):
    if STATE.done(name) and not force:
        say('[skip] %s (done earlier; --redo %s to repeat)' % (label, name))
        return
    say('[....] %s' % label)
    t0 = time.time()
    fn()
    dt = time.time() - t0
    STATE.mark(name, secs=round(dt))
    say('[done] %s in %s' % (label, human(dt)))


def must(rc, last, step_name, hint=''):
    if rc != 0:
        raise BuildError(step_name, 'the step stopped (exit code %s). Last output: %s' % (rc, ' | '.join(last[-4:])), hint)


def stage_extract(env, a):
    out = env.p('extracted')

    def go():
        shutil.rmtree(out, ignore_errors=True)
        rc, last = run(tool('extract.py', env.disc, out), 'extract', a.gentle)
        must(rc, last, 'extract', 'Is disc/DATA0_0.DAT complete? Run Install.bat again to re-extract the disc. Free disk space is the usual cause.')
    return go


def stage_tim2(env, a):
    def go():
        shutil.rmtree(env.p('tim2'), ignore_errors=True)
        rc, last = run(tool('tim2_export.py', env.p('extracted'), env.p('tim2')), 'tim2 export', a.gentle)
        must(rc, last, 'tim2', 'Python needs numpy and Pillow: run Build-HD-Pack.bat, which installs them after asking.')
    return go


def stage_im3(env, a):
    def go():
        shutil.rmtree(env.p('im3'), ignore_errors=True)
        rc, last = run(tool('im3_export.py', env.disc, env.p('im3'), '--scope', 'all'), 'im3 export', a.gentle)
        must(rc, last, 'im3')
    return go


def stage_keys(env, a):
    def go():
        rc, last = run(tool('rg_texkeys.py', env.p('extracted'), env.p('keys.csv')), 'texture keys', a.gentle)
        must(rc, last, 'keys')
    return go


def stage_merge(env, a):
    """One folder of pictures (hard links) and one manifest for every later step, plus the classification."""
    def go():
        png = env.p('all', 'png')
        shutil.rmtree(env.p('all'), ignore_errors=True)
        os.makedirs(png)
        rows, seen, tim2_rows = [], set(), []
        for r in read_csv(env.p('tim2', 'manifest.csv')):
            h = r['hash']
            if h in seen:
                continue
            seen.add(h)
            row = dict(hash=h, file=r['file'], offset=r['offset'], w=r['w'], h=r['h'], kind=r['kind'],
                       has_zero_alpha=r['has_zero_alpha'], src='tim2')
            rows.append(row)
            tim2_rows.append(row)
            link_or_copy(env.p('tim2', 'png', h + '.png'), os.path.join(png, h + '.png'))
        for r in read_csv(env.p('im3', 'unique.csv')):
            h = r['hash']
            if not h or h in seen:
                continue
            seen.add(h)
            rows.append(dict(hash=h, file=r['file'], offset=r['off'], w=r['w'], h=r['h'], kind=r['kind'],
                             has_zero_alpha=r['has_zero_alpha'], src='im3'))
            link_or_copy(env.p('im3', 'png', h + '.png'), os.path.join(png, h + '.png'))
        cols = ['hash', 'file', 'offset', 'w', 'h', 'kind', 'has_zero_alpha', 'src']
        for name, rs in (('manifest.csv', rows), ('manifest-tim2.csv', tim2_rows)):
            with open(env.p('all', name), 'w', newline='', encoding='utf-8') as fh:
                wr = csv.DictWriter(fh, cols)
                wr.writeheader()
                wr.writerows(rs)
        say('  %d distinct pictures (%d TIM2, %d IM3-only)' % (len(rows), len(tim2_rows), len(rows) - len(tim2_rows)))
        rc, last = run(tool('rg_flag_classes.py', '--manifest', env.p('all', 'manifest-tim2.csv'), '--png', png,
                            '--stone', os.path.join(HERE, 'data', 'stone-floor.txt'),
                            '--skip', os.path.join(HERE, 'data', 'skip-hashes.txt'),
                            '--out', env.p('upscaled', 'flagged-classes.csv')), 'classify', a.gentle)
        must(rc, last, 'merge')
    return go


def plan_models(env):
    """-> {model set: [hashes]} following the pack builder's rules (rg_texpack.py): the recommended set of
    flagged-classes.csv for TIM2 pictures, animevideov3 for IM3 pictures under effect/, x4plus-anime for the rest."""
    skip = read_hashes(os.path.join(HERE, 'data', 'skip-hashes.txt'))
    flagged = {r['hash']: r['recommended_set'] for r in read_csv(env.p('upscaled', 'flagged-classes.csv'))}
    plan = {'x4plus-anime': [], 'animevideov3': [], 'x4plus': []}
    for r in read_csv(env.p('all', 'manifest.csv')):
        h = r['hash']
        if h in skip:
            continue
        rec = flagged.get(h)
        if rec is None and r['src'] == 'im3':
            top = r['file'].replace('/', '\\').split('\\')
            if len(top) > 2 and top[1].lower() == 'effect':
                rec = 'animevideov3'
        if rec not in ('animevideov3', 'x4plus'):
            rec = 'x4plus-anime'
        plan[rec].append(h)
    return plan


def log_samples(env, log_path, keys_path=None):
    """Pictures a game run showed: RG_TEXTURE_LOG lines -> pixel hashes (through keys.csv and the IM3 keys)."""
    line = re.compile(r'\[texrep\] texture tbp0=\w+ tbw=\d+ psm=\w+ \d+x\d+ (?:built|now) (\w{16})_(\w{16})')
    wanted = set()
    with open(log_path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = line.search(ln)
            if m:
                wanted.add(m.group(1))
    by_texel = {}
    for r in read_csv(env.p('keys.csv')):
        if r['texels'] in wanted:
            by_texel.setdefault(r['texels'], set()).add(r['pixhash'])
    for r in read_csv(env.p('im3', 'unique.csv')):
        if r['hash'] and r['gs_key'] and r['gs_key'].split('_')[0] in wanted:
            by_texel.setdefault(r['gs_key'].split('_')[0], set()).add(r['hash'])
    out = []
    for t in wanted:
        out += sorted(by_texel.get(t, ()))
    return out


def limit_plan(env, plan, n, sample_log):
    """A trial run: n pictures, those a game run showed first (--sample-log), then a few of every model, then small ones."""
    sizes = {r['hash']: int(r['w']) * int(r['h']) for r in read_csv(env.p('all', 'manifest.csv'))}
    chosen = []
    if sample_log:
        shown = log_samples(env, sample_log)
        say('  the sample log names %d pictures' % len(shown))
        have = set(h for hs in plan.values() for h in hs)
        chosen += [h for h in shown if h in have and sizes.get(h, 0) <= 256 * 256]
    chosen = list(dict.fromkeys(chosen))[:n]
    for model in ('x4plus', 'animevideov3', 'x4plus-anime'):
        for h in sorted(plan[model], key=lambda x: sizes.get(x, 0))[:max(2, n // 6)]:
            if len(chosen) < n and h not in chosen:
                chosen.append(h)
    pool = sorted((h for hs in plan.values() for h in hs), key=lambda x: sizes.get(x, 0))
    for h in pool:
        if len(chosen) >= n:
            break
        if h not in chosen:
            chosen.append(h)
    keep = set(chosen)
    return {m: [h for h in hs if h in keep] for m, hs in plan.items()}


MODELS = {'x4plus-anime': 'realesrgan-x4plus-anime', 'animevideov3': 'realesr-animevideov3', 'x4plus': 'realesrgan-x4plus'}


def upscale_set(env, a, setname, hashes, label):
    if not hashes:
        return
    d = env.p('upscaled', setname)
    only = env.p('upscaled', 'plan-%s.txt' % setname)
    with open(only, 'w') as f:
        f.write('\n'.join(hashes) + '\n')
    todo = sum(1 for h in hashes if not os.path.exists(os.path.join(d, h + '.png')))
    if todo == 0:
        say('  %s: all %d pictures already upscaled' % (label, len(hashes)))
        return
    say('  %s: %d pictures to upscale (%d already done)' % (label, todo, len(hashes) - todo))
    cmd = tool('rg_upscale_batch.py', '--model', MODELS[setname], '--out', d, '--src', env.p('all', 'png'),
               '--manifest', env.p('all', 'manifest.csv'), '--only', only,
               '--skip', os.path.join(HERE, 'data', 'skip-hashes.txt'), '--realesrgan', env.re_dir, '--gpu', str(env.gpu_index))
    if a.gentle:
        cmd.append('--gentle')
    rc, last = run(cmd, label, a.gentle, heartbeat=60)
    must(rc, last, 'upscale', 'Update the graphics driver; try --gentle; check free disk space. Run again to continue: finished pictures are kept.')
    failed = os.path.join(d, '_failed.txt')
    if os.path.exists(failed):
        n = len([ln for ln in open(failed) if ln.strip()])
        say('  note: %d pictures failed in %s (listed in %s); the pack is built without them' % (n, label, failed))


def stage_upscale(env, a):
    plan = plan_models(env)
    if a.limit:
        plan = limit_plan(env, plan, a.limit, a.sample_log)
    say('  plan: %s' % ', '.join('%d %s' % (len(v), k) for k, v in plan.items()))
    env.plan = plan
    # the stone floors and the flagged classes first (small), then the big general set
    upscale_set(env, a, 'x4plus', plan['x4plus'], 'stone floors (x4plus)')
    upscale_set(env, a, 'animevideov3', plan['animevideov3'], 'effects, foliage, clouds (animevideov3)')
    upscale_set(env, a, 'x4plus-anime', plan['x4plus-anime'], 'everything else (x4plus-anime)')


def stage_fp16(env, a):
    """realesrgan-x4plus-anime under ncnn fp16 returns black or dark pictures for ~130 bright textures: find them by
    colour drift and use the animevideov3 upscale instead (the set the pack builder reads first for them)."""
    d = env.p('upscaled', 'x4plus-anime')
    if not os.path.isdir(d) or not any(n.endswith('.png') for n in os.listdir(d)):
        return
    rc, last = run(tool('rg_upscale_colorcheck.py', d, env.p('all', 'png'), '25'), 'colour check', a.gentle)
    must(rc, last, 'fp16')
    bad = [r['hash'] for r in read_csv(os.path.join(d, '_colordrift.csv')) if float(r['drift']) > 25]
    if not bad:
        say('  no blackouts')
        return
    say('  %d pictures came out too dark or off-colour; redoing them with animevideov3' % len(bad))
    bset = env.p('upscaled', 'animevideov3')
    only = env.p('upscaled', 'plan-fp16.txt')
    open(only, 'w').write('\n'.join(bad) + '\n')
    cmd = tool('rg_upscale_batch.py', '--model', MODELS['animevideov3'], '--out', bset, '--src', env.p('all', 'png'),
               '--manifest', env.p('all', 'manifest.csv'), '--only', only, '--realesrgan', env.re_dir, '--gpu', str(env.gpu_index))
    if a.gentle:
        cmd.append('--gentle')
    rc, last = run(cmd, 'fp16 fallback', a.gentle)
    must(rc, last, 'fp16')
    subst = []
    for h in bad:
        src = os.path.join(bset, h + '.png')
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(d, h + '.png'))
            subst.append(h)
    with open(env.p('upscaled', 'primary-substituted-animevideov3.csv'), 'w') as f:
        f.write('hash\n' + '\n'.join(subst) + '\n')
    say('  replaced %d (the rest keep the x4plus-anime picture)' % len(subst))


def stage_pack(env, a):
    out = os.path.join(env.out, 'replacements')
    cmd = tool('rg_texpack.py', 'build', '--keys', env.p('keys.csv'), '--out', out, '--upscaled', env.p('upscaled'),
               '--stubs', env.p('tim2', 'stubs.csv'), '--extra', '%s=%s' % (env.p('im3', 'unique.csv'), env.p('upscaled')),
               '--native', 'item_icon', '--clean')
    rc, last = run(cmd, 'pack build', a.gentle)
    must(rc, last, 'pack')


def stage_verify_textures(env, a):
    out = os.path.join(env.out, 'replacements')
    man = os.path.join(env.out, 'pack-manifest.csv')
    if not os.path.exists(man):
        raise BuildError('verify', 'the pack manifest was not written', 'Run again; see the log.')
    rows = read_csv(man)
    files = [r['path'] for r in rows]
    missing = [p for p in files if not os.path.exists(os.path.join(out, p))]
    from PIL import Image
    name = re.compile(r'^(native_)?[0-9a-f]{16}_[0-9a-f]{16}_(\d+)x(\d+)_psm[0-9a-f]{2}\.png$')
    bad_name = [p for p in files if not name.match(os.path.basename(p))]
    random.seed(1)
    sample = random.sample(files, min(60, len(files)))
    bad_img = []
    for p in sample:
        m = name.match(os.path.basename(p))
        try:
            with Image.open(os.path.join(out, p)) as im:
                w, h = int(m.group(2)), int(m.group(3))
                if m.group(1):
                    ok = im.size == (w, h)           # the atlas's own picture
                else:
                    ok = im.size[0] % w == 0 and im.size[1] % h == 0 and im.size[0] // w == im.size[1] // h >= 1
                if not ok:
                    bad_img.append('%s (%s)' % (p, im.size))
        except Exception as ex:
            bad_img.append('%s (%s)' % (p, ex))
    kinds = {}
    for r in rows:
        kinds[r['set']] = kinds.get(r['set'], 0) + 1
    say('  pack: %d files in %s' % (len(files), out))
    say('  by source: ' + ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
    say('  checked: names %s, files present %s, %d sampled pictures %s' % (
        'ok' if not bad_name else 'BAD %d' % len(bad_name), 'ok' if not missing else 'MISSING %d' % len(missing),
        len(sample), 'ok (right size)' if not bad_img else 'BAD %d' % len(bad_img)))
    if missing or bad_name or bad_img:
        raise BuildError('verify', 'the pack failed its check: %s' % (missing[:3] or bad_name[:3] or bad_img[:3]),
                         'Run again with --redo pack; see the log for the pack builder\'s output.')
    if not files:
        raise BuildError('verify', 'the pack is empty', 'No picture was upscaled: see the lines above.')


def build_textures(env, a):
    say()
    say('== HD textures ==')
    step('extract', 'Extracting the game archives (with the 512-byte offset fix)', env, stage_extract(env, a))
    step('tim2', 'Decoding the TIM2 pictures', env, stage_tim2(env, a))
    step('im3', 'Decoding the IM3 menu art', env, stage_im3(env, a))
    step('keys', 'Computing the renderer\'s texture keys', env, stage_keys(env, a))
    step('merge', 'Merging the pictures and classifying them', env, stage_merge(env, a))
    say('[....] Upscaling')
    t0 = time.time()
    stage_upscale(env, a)
    say('[done] Upscaling in %s' % human(time.time() - t0))
    say('[....] Checking for black upscales (fp16)')
    stage_fp16(env, a)
    say('[....] Building the pack')
    stage_pack(env, a)
    say('[....] Verifying the pack')
    stage_verify_textures(env, a)
    say('[done] HD textures: %s' % os.path.join(env.out, 'replacements'))


# ---------------------------------------------------------------- movies
def movie_names(env, n):
    """The n smallest movies (a trial run): 'folder\\NAME' as rg_movies_batch --only wants them."""
    root = os.path.join(env.disc, 'MOVIE')
    items = []
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.upper().endswith('.MVI'):
                items.append((os.path.getsize(os.path.join(dp, f)), os.path.relpath(os.path.join(dp, f), root)[:-4]))
    items.sort()
    return [r for _, r in items[:n]]


def build_movies(env, a):
    say()
    say('== HD movies ==')
    work = env.p('movies')
    os.makedirs(work, exist_ok=True)
    cmd = tool('rg_movies_batch.py', '--work', work, '--movie-root', os.path.join(env.disc, 'MOVIE'),
               '--realesrgan', env.re_dir, '--gpu', str(env.gpu_index), '--stall-timeout', str(a.stall_timeout))
    if a.gentle:
        cmd.append('--gentle')
    if a.limit_movies:
        cmd += ['--only', ','.join(movie_names(env, a.limit_movies))]
    if a.retry_failed:
        cmd.append('--retry-failed')
    say('[....] Upscaling the movies (each movie: decrypt, upscale 2x frame by frame, encode HEVC)')
    say('       log: ' + os.path.join(work, 'batch.log'))
    t0 = time.time()
    env2 = child_env({'RG_FFMPEG_DIR': find_ffmpeg(env)})
    if a.test_stall:
        env2['RG_MOVIES_TEST_STALL'] = '1'
    rc, last = run(cmd, 'movies', a.gentle, heartbeat=120, env=env2)
    if rc not in (0, None):
        raise BuildError('movies', 'the movie step stopped (exit code %s). Last output: %s' % (rc, ' | '.join(last[-3:])),
                         'Run again to continue (finished movies are kept). A movie that keeps failing is listed in %s; '
                         'try --gentle, update the graphics driver, check free disk space.' % os.path.join(work, 'batch.log'))
    say('[done] Upscaling the movies in %s' % human(time.time() - t0))
    # publish
    st = json.load(open(os.path.join(work, 'state.json'))) if os.path.exists(os.path.join(work, 'state.json')) else {'files': {}}
    dest = os.path.join(env.out, 'movies')
    wanted = None
    if a.limit_movies:
        wanted = set(m.upper() for m in movie_names(env, a.limit_movies))
    n = 0
    failed = []
    for rel, v in sorted(st['files'].items()):
        if wanted is not None and rel.upper() not in wanted:
            continue
        if v.get('status') == 'done':
            src = os.path.join(work, 'hd', rel + '.mkv')
            if os.path.exists(src):
                dst = os.path.join(dest, rel + '.mkv')
                if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src)):
                    if os.path.exists(dst):
                        os.remove(dst)
                    link_or_copy(src, dst)
                n += 1
        elif v.get('status') == 'failed':
            failed.append((rel, v.get('error', '')))
    say('[done] %d movies in %s' % (n, dest))
    verify_movies(env, a, dest, st, wanted)
    if failed:
        for rel, err in failed:
            say('  FAILED %s: %s' % (rel, err[:200]))
        raise BuildError('movies', '%d movies failed' % len(failed),
                         'Run again with --retry-failed (try --gentle). Log: %s' % os.path.join(work, 'batch.log'))


def verify_movies(env, a, dest, st, wanted):
    ffprobe = os.path.join(find_ffmpeg(env), 'ffprobe')
    bad, count = [], 0
    for rel, v in sorted(st['files'].items()):
        if v.get('status') != 'done' or (wanted is not None and rel.upper() not in wanted):
            continue
        p = os.path.join(dest, rel + '.mkv')
        if not os.path.exists(p):
            bad.append((rel, 'missing'))
            continue
        r = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-count_packets', '-show_entries',
                            'stream=codec_name,width,height,nb_read_packets', '-of', 'json', p],
                           capture_output=True, text=True, creationflags=NO_WINDOW if IS_WIN else 0)
        try:
            s = json.loads(r.stdout)['streams'][0]
        except Exception:
            bad.append((rel, 'unreadable'))
            continue
        ok = s['codec_name'] == 'hevc' and int(s['nb_read_packets']) == int(v.get('frames', -1)) and s['height'] == 1080
        if not ok:
            bad.append((rel, 'expected %s frames of 1080p HEVC, found %s' % (v.get('frames'), s)))
        count += 1
    say('  checked %d movies: HEVC, 1080 high, frame count equals the disc movie%s' % (count, '' if not bad else '; PROBLEMS: %s' % bad[:3]))
    if bad:
        raise BuildError('verify', '%d movies failed the check: %s' % (len(bad), bad[:3]), 'Run again with --redo movies.')


# ---------------------------------------------------------------- main
def menu(env, a):
    say()
    say('What do you want to build?')
    say('  [1] HD textures only   (%s)' % EST_TEXT[0])
    say('  [2] HD movies only     (%s)' % EST_MOVIES[0])
    say('  [3] Both')
    say('  [E] Show the plan and the estimates only')
    say('  [Q] Quit')
    say('  (times are for the author\'s RTX 5070 Ti; the graphics card decides)')
    while True:
        try:
            c = input('Choose 1, 2, 3, E or Q: ').strip().lower()
        except EOFError:
            return None
        if c in ('1', '2', '3'):
            return int(c)
        if c == 'e':
            return 'e'
        if c in ('q', ''):
            return None


def main(argv=None):
    global LOG_PATH, STATE
    ap = argparse.ArgumentParser(description='Builds the HD texture pack and HD movies from your own disc.')
    ap.add_argument('--mode', type=int, choices=(1, 2, 3), help='1 textures, 2 movies, 3 both (asked when omitted)')
    ap.add_argument('--iso', help='your disc image, used only when disc/ is not extracted yet')
    ap.add_argument('--disc', help='the extracted disc folder (default: disc/)')
    ap.add_argument('--work', help='work folder (default: work/hd, or %%RG_HD_WORK%%)')
    ap.add_argument('--out', help='textures/SCUS-97490 folder to write to (default: the repository\'s)')
    ap.add_argument('--gentle', action='store_true', help='lowest CPU/GPU priority, fewer threads: slower, the PC stays usable')
    ap.add_argument('--yes', action='store_true', help='answer yes to every question, downloads included (you have read the addresses in docs/HD-BUILD.md)')
    ap.add_argument('--estimate', action='store_true', help='show the plan, disk and time estimates; change nothing')
    ap.add_argument('--redo', metavar='STEP', help='forget a finished step (extract tim2 im3 keys merge, or all) so it runs again')
    ap.add_argument('--gpu', type=int, default=None, help='Vulkan device index for the upscaler (default: the best one)')
    ap.add_argument('--limit', type=int, default=0, help='TRIAL: upscale only N pictures')
    ap.add_argument('--sample-log', help='TRIAL: an RG_TEXTURE_LOG output; its pictures are chosen first by --limit')
    ap.add_argument('--limit-movies', type=int, default=0, help='TRIAL: only the N smallest movies')
    ap.add_argument('--stall-timeout', type=int, default=180, help='seconds without a frame moving before a movie is killed and retried')
    ap.add_argument('--retry-failed', action='store_true', help='also retry movies that failed before')
    ap.add_argument('--skip-space-check', action='store_true')
    ap.add_argument('--clean-work', action='store_true', help='when done, delete the work files (the pack and movies stay)')
    ap.add_argument('--test-stall', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args(argv)

    env = Env(a)
    STATE = State(env.p('state.json'))
    if not a.estimate:     # an estimate changes nothing, not even a log
        os.makedirs(env.work, exist_ok=True)
        LOG_PATH = env.p('build.log')
        say('==== %s  %s' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), ' '.join(sys.argv[1:])), file_only=True)
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    if a.redo:
        STATE.clear(a.redo)
        say('Forgot: %s' % a.redo)

    mode = a.mode
    if mode is None and not a.estimate:
        say('Rogue Galaxy Recompiled: HD pack builder')
        say('Builds the HD textures and HD cutscenes from your own disc. Nothing from the game is distributed;')
        say('the results stay on this PC (see docs/SHARING.md).')
        if not (sys.stdin and sys.stdin.isatty()):
            say('No console to ask on: pass --mode 1|2|3 (see --help).')
            return 2
        c = menu(env, a)
        if c is None:
            return 0
        if c == 'e':
            a.estimate = True
            mode = 3
        else:
            mode = c
    if mode is None:
        mode = 3
    try:
        print_plan(env, mode, a)
        if a.estimate:
            say()
            say('Estimate only: nothing was changed or downloaded.')
            return 0
        say()
        preflight(env, mode, a)
        if not ask('Start now?', True, a.yes):
            say('Nothing was done.')
            return 0
        keep_awake(True)
        prepare_tools(env, mode, a)
        t0 = time.time()
        if mode in (1, 3):
            build_textures(env, a)
        if mode in (2, 3):
            build_movies(env, a)
        say()
        say('Finished in %s. Start the game: HD textures and HD movies are on by default when the folders are not empty' % human(time.time() - t0))
        say('(F1 > Game > Text and prompts > Texture replacements; F1 > Game > Convenience > HD movies).')
        say('Log: ' + LOG_PATH)
        if a.clean_work or (sys.stdin and sys.stdin.isatty() and not a.yes and not a.limit and not a.limit_movies and
                            ask('Delete the work files (about %d GB) to free space? The pack and movies stay, but a later change needs a full rebuild.' % (
                                (GB_TEXTURES if mode in (1, 3) else 0) + (GB_MOVIES if mode in (2, 3) else 0)), False)):
            for d in ('extracted', 'tim2', 'im3', 'all', 'upscaled', 'movies'):
                if d == 'movies' and mode == 1:
                    continue
                shutil.rmtree(env.p(d), ignore_errors=True)
            STATE.clear('all')
            say('Work files deleted.')
        return 0
    except BuildError as e:
        say()
        say('STOPPED at step "%s": %s' % (e.step, e.what))
        if e.hint:
            say('What to do: ' + e.hint)
        say('Finished steps are kept: run Build-HD-Pack.bat again to continue. Full log: ' + LOG_PATH)
        return 1
    except KeyboardInterrupt:
        say()
        say('Interrupted. Finished steps are kept; run again to continue.')
        return 130
    finally:
        kill_children()
        keep_awake(False)


if __name__ == '__main__':
    sys.exit(main())
