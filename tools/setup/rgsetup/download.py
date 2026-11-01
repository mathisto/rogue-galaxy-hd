"""Pinned downloads: an address, a size and a SHA-256 are fixed here (or in data/toolchain.json); a file whose hash
differs is deleted and nothing is unpacked. Nothing is fetched without the player's yes (cli.py asks, or --yes).

The tools the HD builder downloads itself (Real-ESRGAN, FFmpeg) are pinned in tools/hd/build_hd_pack.py; plan.py reads
them from there. This module holds chdman (needed only for a CHD disc image) and the build toolchain.
"""
import hashlib
import json
import os
import shutil
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

from .runner import SetupError

DATA = Path(__file__).resolve().parent / 'data'

# chdman is not released on its own: it ships in MAME's Windows release, a 7-Zip self-extracting archive. The setup
# unpacks it with the Windows tar (libarchive) and keeps chdman.exe and the licence texts only.
CHDMAN = {
    'name': 'chdman from MAME 0.289 (x64 Windows release)',
    'url': 'https://github.com/mamedev/mame/releases/download/mame0289/mame0289b_x64.exe',
    'bytes': 87626249,
    'sha256': 'a1aa7912168c9d1b05e611906bc21b8b9be3935822aead36d12a1da363150b7d',
    'license': 'chdman is BSD-3-Clause; MAME as a whole is GPL-2.0-or-later. A separate program the installer runs '
               '(it is not linked into the game and is not redistributed here)',
    'dest': os.path.join('toolchain', 'chdman'),
    'exe': 'chdman.exe',
}


def sha256_file(path, progress=None):
    h = hashlib.sha256()
    total = os.path.getsize(path)
    done = 0
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
            done += len(blk)
            if progress:
                progress(done, total)
    return h.hexdigest()


def patiently(action, tries=60, delay=0.5):
    """An antivirus looks at the files just written and holds them meanwhile: Windows then refuses to delete or rename
    them. Try again for half a minute before giving up (install.ps1 learned this on 2026-10-27)."""
    for attempt in range(tries):
        try:
            return action()
        except OSError:
            if attempt == tries - 1:
                raise
            time.sleep(delay)


def fetch(spec, target, progress=None, user_agent='rogue-galaxy-recomp-setup'):
    """Download spec['url'] to target (a file), refusing anything whose SHA-256 differs. Returns target.
    progress(done_bytes, total_bytes). SetupError('err.download_failed' | 'err.download_hash')."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and sha256_file(target) == spec['sha256']:
        return target
    part = target.with_name(target.name + '.part')
    patiently(lambda: part.unlink(missing_ok=True))
    req = urllib.request.Request(spec['url'], headers={'User-Agent': user_agent})
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(part, 'wb') as out:
            total = int(r.headers.get('Content-Length') or spec.get('bytes') or 0)
            done = 0
            while True:
                blk = r.read(1 << 20)
                if not blk:
                    break
                out.write(blk)
                done += len(blk)
                if progress:
                    progress(done, total)
    except OSError as ex:
        raise SetupError('err.download_failed', name=spec['name'], detail=str(ex)) from None
    if sha256_file(part) != spec['sha256']:
        patiently(lambda: part.unlink(missing_ok=True))
        raise SetupError('err.download_hash', name=spec['name'])
    patiently(lambda: os.replace(part, target))
    return target


def common_prefix(names):
    """The single top folder every name sits in ('mame/'), or ''. Archives that hold everything in one folder give what
    is inside it."""
    prefix = ''
    while True:
        deeper = [n for n in names if '/' in n[len(prefix):]]
        if not deeper:
            return prefix
        nxt = deeper[0][:len(prefix) + deeper[0][len(prefix):].index('/') + 1]
        if any(not (n + '/').startswith(nxt) for n in names):
            return prefix
        prefix = nxt


def unpack(archive, dest, only=None):
    """Unpack a .zip or .tar.gz into dest, dropping a single top folder, never writing outside dest.
    only: a predicate on the relative name (after the drop) choosing what to keep."""
    dest = Path(dest)
    archive = Path(archive)
    if archive.suffix == '.zip':
        z = zipfile.ZipFile(archive)
        members = [(i.filename, i) for i in z.infolist()]
        opener = z.open
    else:
        t = tarfile.open(archive)
        members = [(i.name, i) for i in t.getmembers()]
        opener = t.extractfile
    names = [n.replace('\\', '/').rstrip('/') for n, _ in members]
    prefix = common_prefix(names)
    root = dest.resolve()
    for (raw, info), name in zip(members, names):
        rel = name[len(prefix):]
        is_dir = info.is_dir() if hasattr(info, 'is_dir') else False
        if not rel or (only and not only(rel)):
            continue
        out = (dest / rel).resolve()
        if root != out and root not in out.parents:
            continue    # never write outside the folder
        if is_dir:
            out.mkdir(parents=True, exist_ok=True)
            continue
        if hasattr(info, 'isfile') and not info.isfile():
            continue
        out.parent.mkdir(parents=True, exist_ok=True)
        with opener(info) as src, open(out, 'wb') as f:
            shutil.copyfileobj(src, f)
    return dest


def load_toolchain():
    return json.loads((DATA / 'toolchain.json').read_text(encoding='utf-8'))['tools']


def unpack_sfx(archive, dest, names):
    """Extract just `names` from a 7-Zip self-extracting .exe with the Windows tar (libarchive reads them), without
    running it. Returns True when every name is there afterwards."""
    sysroot = os.environ.get('SystemRoot', r'C:\Windows')
    tar = os.path.join(sysroot, 'System32', 'tar.exe')
    if not os.path.isfile(tar):
        tar = shutil.which('tar') or tar
    import subprocess
    r = subprocess.run([tar, '-xf', str(archive), '-C', str(dest), *names], capture_output=True,
                       creationflags=0x08000000 if os.name == 'nt' else 0)
    return r.returncode == 0 and all((Path(dest) / n).is_file() for n in names)
