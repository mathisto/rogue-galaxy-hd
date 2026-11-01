#!/usr/bin/env python3
"""Assemble the release folder of docs/INSTALLER.md ("The release") and, with --zip, pack it.

    python tools/make_release.py [--out build/release] [--zip] [--worktree | --ref REF] [--disc DISC_FOLDER] [--no-check]

The result is RogueGalaxy-Setup/ (and RogueGalaxy-Setup-<version>-windows.zip beside it): everything the setup needs to
turn the player's own disc into the game without downloading anything for the game itself. Real-ESRGAN, FFmpeg and
chdman stay downloads (pinned, SHA-256, asked first, only for the HD parts or a CHD disc).

    RogueGalaxy-Setup.exe   the window (tools/setup/launcher/setup_launcher.c, no console): starts
                            toolchain\\python\\pythonw.exe -m rgsetup.gui
    toolchain/              the pinned build tools of rgsetup/data/toolchain.json, unpacked and stamped as the setup
                            leaves them (git, python with tkinter, cmake, ninja, llvm-mingw), and pylibs/ (numpy and
                            Pillow, installed with the bundled python's pip)
    deps/                   the sources CMake would download (rgsetup/data/deps.json), each at its commit, no .git
    (the source tree)       src, cmake, config, patches, thirdparty (dq8-gfx, PS2Recomp, imgui), tools, docs, setup.py,
                            CMakeLists.txt, README.md, LICENSE, THIRD-PARTY-NOTICES.md, CHANGELOG.md (tools/release/source_zip.py)
    Setup-Console.cmd, README-FIRST.txt, VERSION, REVISION, release.json

Needs network on the machine that builds the release (the pinned downloads, cached in <out>/../release-cache), git, and
Python 3.11 with Pillow only for tools/setup/make_icon.py (the icon is checked in). tools/check_content.py runs over the
folder before anything is zipped: disc-derived bytes or the builder's personal paths fail the build.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'setup'))
sys.path.insert(0, str(ROOT / 'tools' / 'release'))
sys.path.insert(0, str(ROOT / 'tools'))

import check_content  # noqa: E402
import source_zip  # noqa: E402
from rgsetup import download  # noqa: E402

FOLDER = 'RogueGalaxy-Setup'
GITHUB_LIMIT = 2 * 1024 ** 3
# Left out of the release: tests, the batch installers (the window does their work), the developer's session notes, and
# files of the repository itself.
SKIP_PREFIXES = ('tests/', 'tools/install/', '.github/', 'tools/hd/build-hd-pack.ps1', 'docs/SESSION-NOTES')
SKIP_NAMES = {'Install.bat', 'Build-HD-Pack.bat', '.gitignore', '.gitattributes', '.gitmodules'}

CONSOLE_CMD = r"""@echo off
rem The setup without its window: the same program, its progress in words. Add options as the first line of
rem docs\INSTALLER.md shows, for example:  Setup-Console.cmd --source "D:\Rogue Galaxy (USA).iso" --preset recommended
setlocal
cd /d "%~dp0tools\setup"
"%~dp0toolchain\python\python.exe" -X utf8 -m rgsetup --dest "%~dp0." --format text %*
exit /b %ERRORLEVEL%
"""

README_FIRST = """Rogue Galaxy Recompiled {version}

1. Unpack this whole folder somewhere with a short path and plenty of room, for example C:\\RogueGalaxy
   (about 20 GB for the game, 40 GB or more with the HD parts; not inside OneDrive).
2. Double-click RogueGalaxy-Setup.exe. Windows may say it does not know the publisher: the program is not signed
   (see docs\\SHARING.md). Choose "More info", then "Run anyway", if you trust where you got it.
3. Choose your own disc image (an .iso, .bin/.cue or .chd of the USA release, or a folder with its files) and what to
   add. The window lists every download before it starts and waits for your yes.
4. When it says the game is ready, press Play. Later, "Rogue Galaxy.exe" in this folder starts it.

To update: unpack the newer release over this folder (your disc/, build/, textures/ and saves are not in the zip) and
run RogueGalaxy-Setup.exe again; it continues from what is finished.

The game's files are never included. You need your own copy of the disc. README.md and docs\\ explain the rest.
"""


def git(*args, cwd=ROOT):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def version_info(worktree, ref):
    text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8') if worktree else git('show', f'{ref}:CMakeLists.txt')
    version = re.search(r'project\(RogueGalaxyRecomp\s+VERSION\s+(\d+\.\d+\.\d+)', text).group(1)
    revision = git('describe', '--tags', '--always', ref)
    dirty = bool(git('status', '--porcelain', '--untracked-files=no', '--ignore-submodules=dirty'))
    if worktree and dirty:
        revision += '-dirty'
    return version, revision


def mib(n):
    return f'{n / (1 << 20):.1f} MiB'


def say(*a):
    print(*a, flush=True)


def progress(label):
    last = [0.0]

    def cb(done, total):
        if time.time() - last[0] > 5 or done == total:
            last[0] = time.time()
            say(f'  {label}: {mib(done)} of {mib(total)}')
    return cb


def run(cmd, **kw):
    say('+', ' '.join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


# ---- the source tree

def write_source(out, worktree, ref, when):
    count = 0
    for name, data, _mtime in source_zip.iter_source(worktree, ref):
        if name.startswith(SKIP_PREFIXES) or name in SKIP_NAMES:
            continue
        if name.startswith(source_zip.NEVER) or len(data) > source_zip.LARGEST:
            sys.exit(f'refusing {name} ({len(data)} bytes): game data or a build has no place in a release')
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        os.utime(target, (when, when))
        count += 1
    return count


# ---- the toolchain

def build_toolchain(out, cache):
    tools = download.load_toolchain()
    done = []
    for t in tools:
        spec = {'name': t['label'], 'url': t['url'], 'sha256': t['sha256'], 'bytes': t['mb'] * 1_000_000}
        archive = download.fetch(spec, cache / (t['name'] + t['ext']), progress(t['label']))
        dest = out / 'toolchain' / t['name']
        say(f'unpack {t["label"]}')
        download.unpack(archive, dest)
        if not (dest / t['exe']).is_file():
            sys.exit(f'{t["label"]}: {t["exe"]} is missing after unpacking')
        (dest / '.rg-sha256').write_text(t['sha256'] + '\n', encoding='ascii')
        done.append({'name': t['name'], 'label': t['label'], 'url': t['url'], 'sha256': t['sha256'], 'license': t['license']})
    return done


def build_pylibs(out):
    """numpy and Pillow for the HD builder, installed with the bundled python so the HD parts need no pip run."""
    py = out / 'toolchain' / 'python' / 'python.exe'
    libs = out / 'toolchain' / 'pylibs'
    env = dict(os.environ, PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1')
    run([py, '-m', 'pip', 'install', '--disable-pip-version-check', '--no-compile', '--only-binary', ':all:',
         '--target', libs, '-r', ROOT / 'tools' / 'hd' / 'requirements.txt'], env=env)
    r = subprocess.run([str(py), '-I', '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import numpy, PIL; '
                        'print("numpy", numpy.__version__, "Pillow", PIL.__version__)', str(libs)],
                       capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit('pylibs do not import with the bundled python:\n' + r.stderr)
    say(r.stdout.strip())
    shutil.rmtree(libs / 'bin', ignore_errors=True)       # console scripts: their launchers hold the path of the machine that built them
    return r.stdout.strip()


# ---- deps/

def build_deps(out, cache, git_exe):
    deps = json.loads((ROOT / 'tools' / 'setup' / 'rgsetup' / 'data' / 'deps.json').read_text(encoding='utf-8'))['deps']
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0')
    done = []
    for d in deps:
        dest = out / 'deps' / d['name']
        mirror = cache / 'deps' / (d['name'] + '-' + d['commit'][:12])
        if not (mirror / '.complete').is_file():
            shutil.rmtree(mirror, ignore_errors=True)
            mirror.mkdir(parents=True)
            say(f'fetch {d["name"]} {d["tag"] or d["commit"][:10]}')
            run([git_exe, 'init', '-q'], cwd=mirror, env=env)
            run([git_exe, 'fetch', '-q', '--depth', '1', d['url'], d['commit']], cwd=mirror, env=env)
            run([git_exe, '-c', 'core.autocrlf=false', 'checkout', '-q', 'FETCH_HEAD'], cwd=mirror, env=env)
            got = subprocess.run([str(git_exe), 'rev-parse', 'HEAD'], cwd=mirror, capture_output=True, text=True).stdout.strip()
            if got != d['commit']:
                sys.exit(f'{d["name"]}: fetched {got}, pinned {d["commit"]}')
            (mirror / '.complete').write_text('')
        shutil.copytree(mirror, dest, ignore=shutil.ignore_patterns('.git', '.complete'))
        done.append({k: d[k] for k in ('name', 'url', 'tag', 'commit', 'license')})
    return done


# ---- the launcher

def build_launcher(out, work):
    llvm = out / 'toolchain' / 'llvm-mingw' / 'bin'
    work.mkdir(parents=True, exist_ok=True)
    res = work / 'setup_launcher.res'
    run([sys.executable, ROOT / 'src' / 'launcher' / 'make_resources.py', ROOT / 'tools' / 'setup' / 'rgsetup' / 'data' / 'setup.ico',
         ROOT / 'src' / 'launcher' / 'launcher.manifest', res])
    env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'COMSPEC', 'PATHEXT')}
    env['PATH'] = os.pathsep.join([str(llvm), r'C:\Windows\System32', r'C:\Windows'])
    exe = out / 'RogueGalaxy-Setup.exe'
    run([llvm / 'clang.exe', '-O2', '-mwindows', '-static', '-Wall', '-Wl,-s', ROOT / 'tools' / 'setup' / 'launcher' / 'setup_launcher.c',
         res, '-o', exe], env=env, cwd=work)
    return exe


# ---- checks

def clean_env(out):
    env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'COMSPEC', 'PATHEXT', 'LOCALAPPDATA', 'APPDATA', 'USERPROFILE')}
    env['PATH'] = os.pathsep.join([str(out / 'toolchain' / 'python'), r'C:\Windows\System32', r'C:\Windows'])
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PYTHONNOUSERSITE'] = '1'
    return env


def check_bundle(out):
    """The folder's own python runs the window's self test and the setup's read-only check, from the folder."""
    py = out / 'toolchain' / 'python' / 'python.exe'
    env = clean_env(out)
    r = subprocess.run([str(py), '-X', 'utf8', '-m', 'rgsetup.gui', '--selftest'], cwd=out / 'tools' / 'setup', env=env,
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        sys.exit(f'window self test failed ({r.returncode}):\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}')
    say('bundle: the window built every screen with the bundled python and closed')
    r = subprocess.run([str(py), '-X', 'utf8', '-m', 'rgsetup', '--dest', str(out), '--estimate', '--preset', 'faithful', '--format', 'json'],
                       cwd=out / 'tools' / 'setup', env=env, capture_output=True, text=True, timeout=300)
    events = [json.loads(x) for x in r.stdout.splitlines() if x.startswith('{')]
    plan = next((e for e in events if e['type'] == 'plan'), None)
    # a build folder this deep (build/release/RogueGalaxy-Setup) is refused by the setup's own path rule: that is the rule
    # working, not a fault of the release
    errors = [e['id'] for e in events if e['type'] == 'error' and not e['id'].startswith('err.path_')]
    if plan is None or errors:
        sys.exit(f'setup check failed ({r.returncode}) {errors}:\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}')
    downloads = [d['id'] for d in plan['downloads']]
    if downloads:
        sys.exit(f'the bundled toolchain is not recognised: the setup would download {downloads}')
    say('bundle: the setup recognises the toolchain (no download in the plan for the game); it runs: ' + ', '.join(plan['run']))


def scan(out, disc):
    class A:
        folder = str(out)
        extra = [e for e in (subprocess.run(['git', 'config', 'user.email'], cwd=ROOT, capture_output=True, text=True).stdout.strip(),) if e]

    A.disc = str(disc) if disc and Path(disc).is_dir() else None
    n, bad, needles = check_content.check(A)
    if bad:
        sys.exit('check_content found game data or personal paths in the release:\n' + '\n'.join(bad))
    say(f'check_content: {n} files, no hit ({needles} disc byte runs looked for)')
    return n, needles


# ---- the zip

def sizes(out):
    rows = []
    for p in sorted(out.iterdir()):
        n = sum(f.stat().st_size for f in p.rglob('*') if f.is_file()) if p.is_dir() else p.stat().st_size
        rows.append((p.name, n))
    for name, n in sorted(rows, key=lambda r: -r[1])[:12]:
        say(f'  {name:28} {mib(n):>12}')
    total = sum(n for _, n in rows)
    say(f'  {"total (unpacked)":28} {mib(total):>12}')
    return total


def write_zip(out, path, when):
    path.parent.mkdir(parents=True, exist_ok=True)
    stamp = time.gmtime(when)[:6]
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():
                info = zipfile.ZipInfo((Path(FOLDER) / p.relative_to(out)).as_posix(), stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                with open(p, 'rb') as src, z.open(info, 'w', force_zip64=True) as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    return path.stat().st_size


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'release', help='folder for RogueGalaxy-Setup/ and the zip')
    ap.add_argument('--zip', action='store_true', help='also pack RogueGalaxy-Setup-<version>-windows.zip')
    ap.add_argument('--worktree', action='store_true', help='the working tree instead of the commit (to try before committing)')
    ap.add_argument('--ref', default='HEAD')
    ap.add_argument('--disc', type=Path, default=ROOT / 'disc', help='the unpacked disc, read only, for the byte-run scan')
    ap.add_argument('--no-check', action='store_true', help='skip the bundled-python checks')
    ap.add_argument('--cache', type=Path, help='downloads (default: <out>/../release-cache)')
    args = ap.parse_args(argv)

    out_root = args.out.resolve()
    folder = out_root / FOLDER
    cache = (args.cache or out_root.parent / 'release-cache').resolve()
    version, revision = version_info(args.worktree, args.ref)
    stamp = f'{version}+{revision}'
    when = time.time() - 86400          # a day in the past: a zip's times have no zone, and a future input keeps ninja stale
    say(f'release {stamp} -> {folder}')
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)

    n = write_source(folder, args.worktree, args.ref, when)
    say(f'source tree: {n} files')
    toolchain = build_toolchain(folder, cache / 'dl')
    git_exe = folder / 'toolchain' / 'git' / 'cmd' / 'git.exe'
    pylibs = build_pylibs(folder)
    deps = build_deps(folder, cache, git_exe)
    build_launcher(folder, cache / 'launcher')
    (folder / 'Setup-Console.cmd').write_text(CONSOLE_CMD, encoding='ascii', newline='\r\n')
    (folder / 'README-FIRST.txt').write_text(README_FIRST.format(version=stamp), encoding='ascii', newline='\r\n')
    (folder / 'VERSION').write_text(stamp + '\n', encoding='ascii')
    (folder / 'REVISION').write_text(revision + '\n', encoding='ascii')
    (folder / 'release.json').write_text(json.dumps({
        'version': version, 'revision': revision, 'built': time.strftime('%Y-%m-%d', time.gmtime()),
        'toolchain': toolchain, 'pylibs': pylibs, 'deps': deps,
        'downloads_left': ['Real-ESRGAN (HD parts)', 'FFmpeg (HD movies, unless on PATH)', 'chdman (CHD discs only)']},
        indent=1), encoding='utf-8')
    for p in list(folder.rglob('__pycache__')):
        shutil.rmtree(p, ignore_errors=True)
    if not args.no_check:
        check_bundle(folder)
    for p in list(folder.rglob('__pycache__')):
        shutil.rmtree(p, ignore_errors=True)
    scan(folder, args.disc)
    total = sizes(folder)
    if args.zip:
        name = f'RogueGalaxy-Setup-{version}-{revision}-windows.zip'
        size = write_zip(folder, out_root / name, when)
        say(f'{out_root / name}: {mib(size)} ({size} bytes); GitHub allows {mib(GITHUB_LIMIT)} a file: '
            + ('under the limit' if size < GITHUB_LIMIT else 'OVER THE LIMIT'))
        say('sha256 ' + hashlib.sha256((out_root / name).read_bytes()).hexdigest())
        if size >= GITHUB_LIMIT:
            sys.exit(1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
