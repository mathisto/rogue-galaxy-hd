#!/usr/bin/env python3
"""Fail when a folder holds bytes from the disc, or a path that belongs to the person who built it.

    python tools/check_content.py DIR [--disc DISC_FOLDER] [--extra TEXT ...]    a release folder (make_release.py runs this)
    python tools/check_content.py                                                every file git tracks (or could commit)

What it refuses, in a release folder (and, with no argument, in the repository):

  * a file named or typed like the game: disc images (.iso .bin .cue .chd), the disc's own archives and movies
    (.dat .hd6 .mvi .tm2 .im3), the game's executable (SCUS_974.90, SLUS/SCUS-style names), `disc/`, `mc0/`, `mc1/`,
    `textures/` and `build/` folders, memory-card and save files;
  * a file whose size and SHA-256 are those of one of the disc's 124 files (config/SCUS_974.90/hashes.json: facts about
    the dump, no game data), at least 4 KiB long, whatever it is called;
  * with --disc (the unpacked disc, read only): a 32-byte run taken from the inside of each large disc file found
    anywhere in a file, so a copy cut out of an archive, or pasted into a bigger file, is caught too;
  * a personal path: the builder's user name or home folder, the folders of this checkout and of the scratch folders,
    e-mail addresses given with --extra, in any file (as ASCII or UTF-16, text or binary).

It prints one line per hit and exits 1; 0 when clean. Standard library only.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HASHES = ROOT / 'config' / 'SCUS_974.90' / 'hashes.json'

BLOCKED_SUFFIXES = {'.iso', '.bin', '.cue', '.chd', '.dat', '.hd6', '.mvi', '.tm2', '.im3', '.elf', '.mcd', '.mcr', '.ps2',
                    '.sav', '.part'}
BLOCKED_NAMES = {'scus_974.90', 'scus_974.90.elf', 'system.cnf', 'rg-install.json', 'settings.ini', 'install.log'}
TOP_FOLDERS = {'disc', 'mc0', 'mc1', 'textures', 'build', 'work'}     # the install folder's own, at the top
ANY_FOLDERS = {'__pycache__', '.git'}
# Pinned third-party files (the bundled tools and the downloaded sources): their names may end in .dat or .bin, which
# means nothing there; the size, hash, byte-run and personal-path checks still apply to them.
THIRD_PARTY = ('toolchain/', 'deps/')
MIN_HASH_SIZE = 4096
NEEDLE = 32
NEEDLE_FILES = 14          # the largest disc files give the needles
NEEDLES_PER_FILE = 3
SCAN_MAX = 1 << 30         # a file bigger than this is only checked by name, size and hash


def known_files():
    """{size: [sha256]} of the disc's files, from the checked-in facts."""
    try:
        data = json.loads(HASHES.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    out = {}
    for info in data.get('files', {}).values():
        if info['size'] >= MIN_HASH_SIZE:
            out.setdefault(info['size'], set()).add(info['sha256'])
    return out


def needles_from_disc(disc):
    """32-byte runs from inside the biggest disc files (never from the start, which is generic headers)."""
    disc = Path(disc)
    files = sorted((p for p in disc.rglob('*') if p.is_file() and p.stat().st_size > (1 << 20)),
                   key=lambda p: -p.stat().st_size)[:NEEDLE_FILES]
    out = {}
    for p in files:
        size = p.stat().st_size
        with open(p, 'rb') as f:
            for i in range(NEEDLES_PER_FILE):
                f.seek(size * (2 * i + 1) // (2 * NEEDLES_PER_FILE + 1))
                chunk = f.read(NEEDLE)
                if len(chunk) == NEEDLE and len(set(chunk)) > 8:       # skip zero runs and padding
                    out[chunk] = p.name
    return out


def personal_patterns(extra=()):
    """Byte patterns of what identifies the builder: user name, home, this checkout, scratch folders, extras."""
    names = set()
    for var in ('USERNAME', 'USER', 'LOGNAME'):
        if os.environ.get(var):
            names.add(os.environ[var])
    home = Path.home()
    texts = {str(home), home.name, str(ROOT), str(ROOT).replace('\\', '/'), *names, *extra}
    texts |= {t.replace('\\', '/') for t in list(texts)}
    texts = {t for t in texts if len(t) >= 4}
    pats = {}
    for t in texts:
        pats[t.encode('utf-8')] = t
        pats[t.encode('utf-16-le')] = t
    return pats


def list_files(args):
    if args.folder:
        base = Path(args.folder)
        return base, [p for p in sorted(base.rglob('*')) if p.is_file()]
    out = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT, check=True,
                         capture_output=True).stdout
    return ROOT, [ROOT / p for p in out.decode('utf-8').split('\0') if p and (ROOT / p).is_file()]


def problems_for(path, base, sizes, needles, personal, repo_mode=False):
    found = []
    rel = path.relative_to(base)
    parts = [x.lower() for x in rel.parts]
    if not repo_mode and ((len(parts) > 1 and parts[0] in TOP_FOLDERS) or any(x in ANY_FOLDERS for x in parts[:-1])):
        found.append('inside a folder that holds game data, a build or a repository')
    third = not repo_mode and rel.as_posix().startswith(THIRD_PARTY)
    if path.suffix.lower() in BLOCKED_SUFFIXES and not third:
        found.append(f'a {path.suffix} file (game data or a disc image)')
    if path.name.lower() in BLOCKED_NAMES and not third:
        found.append('named like a game file or a personal install file')
    size = path.stat().st_size
    if size in sizes:
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for blk in iter(lambda: f.read(8 << 20), b''):
                h.update(blk)
        if h.hexdigest() in sizes[size]:
            found.append('is byte for byte one of the disc\'s files')
    if size <= SCAN_MAX and (needles or personal):
        with open(path, 'rb') as f:
            data = f.read()
        for needle, name in needles.items():
            if needle in data:
                found.append(f'holds bytes of the disc file {name}')
                break
        for pat, text in personal.items():
            if pat in data:
                found.append(f'holds a personal path or name ({text!r})')
                break
    return found


def check(args):
    base, files = list_files(args)
    sizes = known_files()
    needles = needles_from_disc(args.disc) if args.disc else {}
    personal = personal_patterns(args.extra or ())
    bad = []
    for p in files:
        try:
            for problem in problems_for(p, base, sizes, needles, personal, repo_mode=not args.folder):
                bad.append(f'{p.relative_to(base)}: {problem}')
        except OSError as e:
            bad.append(f'{p.relative_to(base)}: cannot be read ({e})')
    return len(files), bad, len(needles)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('folder', nargs='?', help='a release folder; without it, the files git tracks')
    ap.add_argument('--disc', help='the unpacked disc, read only: also look for runs of its bytes')
    ap.add_argument('--extra', nargs='*', help='more text that must not appear (an e-mail address, a name)')
    args = ap.parse_args(argv)
    n, bad, needles = check(args)
    for line in bad:
        print(line)
    if bad:
        print(f'\n{len(bad)} problem(s) in {n} files. Game data and personal paths never go into a release.')
        return 1
    print(f'check_content: {n} files, no hit ({needles} disc byte runs looked for)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
