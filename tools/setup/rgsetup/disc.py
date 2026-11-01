"""The player's disc: what kind of input it is, whether it is the supported dump, and how to get an ISO from it.

Inputs: an ISO; a BIN/CUE pair (or a lone BIN); a CHD (converted to an ISO by chdman, which steps.py fetches after a
yes); an extracted disc folder. The one supported disc is Rogue Galaxy (USA), SCUS-97490: it is recognised by the boot
executable's serial in SYSTEM.CNF and by the sizes and SHA-256 hashes of config/SCUS_974.90/hashes.json (the facts of
the Redump dump, no game data). Anything else fails with one message that says what it is.

Nothing here writes to the input. Problems are DiscProblem(id, **params): ids are in text/en.json.
"""
import hashlib
import json
import os
import re
import sys
import zlib
from pathlib import Path

from .layout import ROOT

sys.path.insert(0, str(ROOT / 'tools'))
from disc.iso9660 import SECTOR, IsoImage  # noqa: E402

QUICK_LIMIT = 32 << 20     # files up to this size are hashed up front (setup.py verify's rule); the others by size
RAW_SECTOR = 2352

# Serial prefix -> where the disc was sold (PS2 SYSTEM.CNF names the boot file after the serial).
REGIONS = {
    'SCUS': 'USA', 'SLUS': 'USA', 'SCES': 'Europe', 'SLES': 'Europe', 'SCED': 'Europe', 'SLED': 'Europe',
    'SCPS': 'Japan', 'SLPS': 'Japan', 'SLPM': 'Japan', 'SCPM': 'Japan', 'SCAJ': 'Asia', 'SLAJ': 'Asia',
    'SCKA': 'Korea', 'SLKA': 'Korea', 'PBPX': 'Japan',
}


class DiscProblem(Exception):
    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


def load_known(hashes_path):
    return json.loads(Path(hashes_path).read_text(encoding='utf-8'))


def known_digest(known):
    """A fingerprint of what a valid disc is: the same for every valid dump, whichever way it reaches us."""
    h = hashlib.sha256()
    h.update(json.dumps({'iso': known['iso']['size'], 'files': known['files']}, sort_keys=True).encode())
    return h.hexdigest()


def sniff(path):
    """'iso' | 'bin_cue' | 'chd' | 'folder' for the player's input. DiscProblem when it is none of them."""
    p = Path(path)
    if not p.exists():
        raise DiscProblem('err.source_missing', path=str(p))
    if p.is_dir():
        return 'folder'
    ext = p.suffix.lower()
    if ext == '.iso':
        return 'iso'
    if ext in ('.cue', '.bin'):
        return 'bin_cue'
    if ext == '.chd':
        return 'chd'
    # a disc image with another name: look inside
    try:
        with open(p, 'rb') as f:
            if f.read(8) == b'MComprHD':
                return 'chd'
            f.seek(16 * SECTOR)
            if f.read(7) == b'\x01CD001\x01':
                return 'iso'
    except OSError:
        pass
    raise DiscProblem('err.source_type', path=str(p))


def serial_from_cnf(text):
    """('SCUS-97490', 'SCUS_974.90') from the BOOT2 line of SYSTEM.CNF, or None."""
    m = re.search(r'BOOT2?\s*=\s*cdrom0:\\?([A-Z]{4})_(\d{3})\.(\d{2})', text, re.I)
    if not m:
        return None
    a, b, c = m.groups()
    a = a.upper()
    return f'{a}-{b}{c}', f'{a}_{b}.{c}'


def region_of(serial):
    return REGIONS.get(serial[:4].upper(), 'unknown')


def unique_files(iso):
    files = {}
    for e in iso.entries():
        if not e.is_dir:
            files.setdefault(e.path, []).append(e)
    return files


def _hash_entry(iso, entry):
    h = hashlib.sha256()
    for chunk in iso.iter_chunks(entry):
        h.update(chunk)
    return h.hexdigest()


def _problem(problems):
    """One DiscProblem for a list of (kind, path) findings: how many, and the first few."""
    kinds = {}
    for kind, path in problems:
        kinds.setdefault(kind, []).append(path)
    first = '; '.join(f'{k}: {v[0]}' for k, v in kinds.items())
    return DiscProblem('err.disc_mismatch', count=len(problems), missing=len(kinds.get('missing', [])),
                       wrong=len(problems) - len(kinds.get('missing', [])), first=first)


def check_serial(cnf_text, known):
    """Raises DiscProblem for a disc of another game or region; returns the serial."""
    got = serial_from_cnf(cnf_text or '')
    if got is None:
        raise DiscProblem('err.disc_not_ps2')
    if got[0] != known['serial']:
        raise DiscProblem('err.disc_region', serial=got[0], region=region_of(got[0]), expected=known['serial'],
                          expected_region=known['region'])
    return got[0]


def identify_iso(path, known, progress=None):
    """Validate an ISO against hashes.json without writing: serial, layout, file set, sizes, the small files' hashes.
    Returns {'serial', 'files', 'hashed'}. DiscProblem otherwise."""
    try:
        iso = IsoImage(path)
    except (ValueError, OSError) as ex:
        raise DiscProblem('err.disc_not_iso', path=str(path), detail=str(ex)) from None
    with iso:
        if iso.size != known['iso']['size'] and not iso.volumes:
            raise DiscProblem('err.disc_not_iso', path=str(path), detail='')
        try:
            files = unique_files(iso)
        except (ValueError, OSError, IndexError) as ex:
            raise DiscProblem('err.disc_not_iso', path=str(path), detail=str(ex)) from None
        cnf = files.get('SYSTEM.CNF')
        if not cnf:
            raise DiscProblem('err.disc_not_ps2')
        serial = check_serial(iso.read_bytes(cnf[0]).decode('ascii', 'replace'), known)
        problems = []
        k = known['iso']
        if iso.size != k['size']:
            problems.append(('size', f'image is {iso.size} bytes, expected {k["size"]}'))
        if [v.base for v in iso.volumes] != k['volume_starts']:
            problems.append(('layout', 'the two disc layers start at other sectors'))
        expected = known['files']
        for p in sorted(set(expected) - set(files)):
            problems.append(('missing', p))
        done = 0
        small = [(p, i) for p, i in sorted(expected.items()) if p in files and i['size'] <= QUICK_LIMIT]
        for p, info in sorted(expected.items()):
            copies = files.get(p)
            if not copies:
                continue
            if copies[0].size != info['size']:
                problems.append(('size', p))
            elif [c.layer for c in copies] != info['layers']:
                problems.append(('layers', p))
            elif info['size'] <= QUICK_LIMIT:
                if _hash_entry(iso, copies[0]) != info['sha256']:
                    problems.append(('hash', p))
                done += 1
                if progress:
                    progress(done, len(small))
        if problems:
            raise _problem(problems)
    return {'serial': serial, 'files': len(expected), 'hashed': done}


def image_digests(path, progress=None):
    md5, sha1, sha256, crc = hashlib.md5(), hashlib.sha1(), hashlib.sha256(), 0
    total = os.path.getsize(path)
    done = 0
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            md5.update(chunk)
            sha1.update(chunk)
            sha256.update(chunk)
            crc = zlib.crc32(chunk, crc)
            done += len(chunk)
            if progress:
                progress(done, total)
    return {'crc32': f'{crc:08x}', 'md5': md5.hexdigest(), 'sha1': sha1.hexdigest(), 'sha256': sha256.hexdigest()}


def verify_image_hash(path, known, progress=None):
    """The whole image against Redump (--verify). DiscProblem('err.disc_image_hash')."""
    got = image_digests(path, progress)
    for algo in ('crc32', 'md5', 'sha1'):
        if got[algo] != known['iso'][algo]:
            raise DiscProblem('err.disc_image_hash', algo=algo, got=got[algo], expected=known['iso'][algo])
    return got


# ---- an extracted disc folder

def find_disc_root(path):
    """The folder that holds SYSTEM.CNF: path itself, or the only folder inside it that does (case-insensitive)."""
    p = Path(path)
    def has_cnf(d):
        try:
            return any(x.name.lower() == 'system.cnf' for x in d.iterdir() if x.is_file())
        except OSError:
            return False
    if has_cnf(p):
        return p
    subs = [d for d in p.iterdir() if d.is_dir() and has_cnf(d)] if p.is_dir() else []
    if len(subs) == 1:
        return subs[0]
    return None


def index_folder(root):
    """{lowercase relative path with '/': real Path} of every file under root (Windows paths are case-insensitive, and
    other dumping tools do not agree on case)."""
    out = {}
    base = Path(root)
    for folder, _dirs, files in os.walk(base):
        for name in files:
            full = Path(folder) / name
            rel = full.relative_to(base).as_posix()
            if ';' in name:      # a ";1" version suffix some tools leave
                rel = rel[:rel.rindex(';')]
            out[rel.lower()] = full
    return out


def validate_folder(path, known, progress=None):
    """Check an extracted disc folder: the serial, every expected file present with the right size, and the hashes of
    the files up to QUICK_LIMIT. One DiscProblem says how many files are missing or wrong and names the first.
    Returns {'root', 'serial', 'files', 'hashed', 'map': {expected path: real Path}}."""
    root = find_disc_root(path)
    if root is None:
        raise DiscProblem('err.disc_folder_not_disc', path=str(path))
    found = index_folder(root)
    cnf = found.get('system.cnf')
    serial = check_serial(cnf.read_bytes().decode('ascii', 'replace'), known)
    problems, mapping = [], {}
    expected = known['files']
    todo = sorted(expected.items())
    small = [p for p, i in todo if i['size'] <= QUICK_LIMIT]
    done = 0
    for p, info in todo:
        real = found.get(p.lower())
        if real is None:
            problems.append(('missing', p))
            continue
        mapping[p] = real
        if real.stat().st_size != info['size']:
            problems.append(('size', p))
            continue
        if info['size'] <= QUICK_LIMIT:
            h = hashlib.sha256(real.read_bytes()).hexdigest()
            if h != info['sha256']:
                problems.append(('hash', p))
            done += 1
            if progress:
                progress(done, len(small))
    if problems:
        raise _problem(problems)
    return {'root': root, 'serial': serial, 'files': len(expected), 'hashed': done, 'map': mapping}


# ---- BIN/CUE

MODES = {  # mode -> (bytes per sector in the file, offset of the 2048 data bytes in a sector)
    'MODE1/2048': (2048, 0), 'MODE1/2352': (2352, 16), 'MODE2/2352': (2352, 24), 'MODE2/2048': (2048, 0),
    'MODE2/2336': (2336, 8),
}
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def parse_cue(path):
    """The first data track of a CUE sheet -> (bin path, sector size in the file, data offset, start frame, end frame
    or None). DiscProblem('err.disc_cue') for a sheet this cannot read."""
    path = Path(path)
    try:
        text = path.read_text(encoding='utf-8-sig', errors='replace')
    except OSError as ex:
        raise DiscProblem('err.disc_cue', detail=str(ex)) from None
    file, tracks = None, []
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r'FILE\s+(?:"([^"]+)"|(\S+))\s+\S+', s, re.I)
        if m:
            file = path.with_name(m.group(1) or m.group(2))
            continue
        m = re.match(r'TRACK\s+(\d+)\s+(\S+)', s, re.I)
        if m:
            tracks.append({'file': file, 'mode': m.group(2).upper(), 'index': None})
            continue
        m = re.match(r'INDEX\s+01\s+(\d+):(\d+):(\d+)', s, re.I)
        if m and tracks:
            mm, ss, ff = (int(x) for x in m.groups())
            tracks[-1]['index'] = (mm * 60 + ss) * 75 + ff
    if not tracks or tracks[0]['file'] is None or tracks[0]['index'] is None:
        raise DiscProblem('err.disc_cue', detail='no TRACK / INDEX 01 found')
    first = tracks[0]
    if first['mode'] == 'AUDIO' or first['mode'] not in MODES:
        raise DiscProblem('err.disc_cue', detail=f'the first track is {first["mode"]}, not data')
    size, offset = MODES[first['mode']]
    nxt = next((t['index'] for t in tracks[1:] if t['file'] == first['file'] and t['index'] is not None), None)
    if not first['file'].is_file():
        raise DiscProblem('err.disc_cue', detail=f'{first["file"].name} is missing')
    return first['file'], size, offset, first['index'], nxt


def bin_layout(bin_path, known):
    """(sector size in the file, data offset) for a lone BIN, from its size against the supported disc's."""
    size = os.path.getsize(bin_path)
    sectors = known['iso']['sectors']
    if size == sectors * SECTOR:
        return 2048, 0
    if size == sectors * RAW_SECTOR:
        return RAW_SECTOR, 16
    raise DiscProblem('err.disc_bin_size', size=size, expected=known['iso']['size'])


def bincue_plan(path, known):
    """What to do with a BIN/CUE: ('iso', bin) when the BIN already is the ISO (2048-byte sectors from the start), or
    ('convert', bin, sector size, data offset, first sector, sector count)."""
    p = Path(path)
    if p.suffix.lower() == '.cue':
        bin_path, size, offset, start, end = parse_cue(p)
    else:
        bin_path, (size, offset), start, end = p, bin_layout(p, known), 0, None
    total = os.path.getsize(bin_path) // size
    count = (end if end is not None else total) - start
    if size == 2048 and offset == 0 and start == 0:
        return ('iso', bin_path)
    return ('convert', bin_path, size, offset, start, count)


def convert_bincue(plan, out_iso, progress=None):
    """Write the 2048-byte data sectors of a BIN to out_iso. DiscProblem('err.disc_bin_sector') when a raw sector does
    not start with the CD sync pattern (a damaged or mis-described image)."""
    _, bin_path, size, offset, start, count = plan
    out_iso = Path(out_iso)
    out_iso.parent.mkdir(parents=True, exist_ok=True)
    part = out_iso.with_name(out_iso.name + '.part')
    batch = 2048     # sectors per read
    with open(bin_path, 'rb') as src, open(part, 'wb') as dst:
        src.seek(start * size)
        done = 0
        while done < count:
            n = min(batch, count - done)
            buf = src.read(n * size)
            if len(buf) < n * size:
                n = len(buf) // size
                if n == 0:
                    break
            for i in range(n):
                sector = buf[i * size:(i + 1) * size]
                if size >= RAW_SECTOR and sector[:12] != SYNC:
                    raise DiscProblem('err.disc_bin_sector', sector=start + done + i)
                dst.write(sector[offset:offset + 2048])
            done += n
            if progress:
                progress(done * 2048, count * 2048)
    os.replace(part, out_iso)
    return out_iso
