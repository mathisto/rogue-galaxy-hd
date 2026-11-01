"""Synthetic discs for the setup's tests. Nothing here is derived from the game: the files are made-up bytes, and the
"known disc" (what config/SCUS_974.90/hashes.json holds for the real one) is computed from them.
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'setup'))
sys.path.insert(0, str(REPO / 'tools'))

SECTOR = 2048
CNF = b'BOOT2 = cdrom0:\\SCUS_974.90;1\r\nVER = 1.01\r\nVMODE = NTSC\r\n'


def fake_files(serial_cnf=CNF):
    """{path: bytes} of a tiny made-up disc."""
    rnd = bytes((i * 7 + 3) % 251 for i in range(5000))
    return {
        'SYSTEM.CNF': serial_cnf,
        'SCUS_974.90': b'\x7fELF' + rnd,
        'BIN/ONE.BIN': rnd[:700],
        'DATA0_0.DAT': rnd * 3,          # "big": larger than the patched QUICK_LIMIT
        'MOVIE/A.MVI': rnd[:1234],
    }


def known_for(files, serial='SCUS-97490', iso_size=None, sectors=None, big=()):
    return {
        'version': 'SCUS_974.90', 'serial': serial, 'region': 'NTSC-U', 'elf': 'SCUS_974.90',
        'iso': {'size': iso_size, 'sectors': sectors, 'volume_starts': [0], 'crc32': '0', 'md5': '0', 'sha1': '0'},
        'files': {p: {'size': len(b), 'sha256': hashlib.sha256(b).hexdigest(), 'layers': [0]} for p, b in files.items()},
    }


def write_folder(root, files):
    for p, b in files.items():
        f = Path(root) / p
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b)


def build_iso(files):
    """A single-layer ISO9660 image holding files (paths with one level of folders at most). -> bytes."""
    names = sorted(files)
    folders = sorted({p.rsplit('/', 1)[0] for p in names if '/' in p})
    # layout: 16 PVD, 17 terminator, 18 root dir, then one sector per folder dir, then the files
    next_sector = 19
    dir_sector = {'': 18}
    for d in folders:
        dir_sector[d] = next_sector
        next_sector += 1
    file_sector = {}
    for p in names:
        file_sector[p] = next_sector
        next_sector += max(1, (len(files[p]) + SECTOR - 1) // SECTOR)
    total = next_sector

    def record(name, sector, size, is_dir):
        nm = name.encode('ascii')
        length = 33 + len(nm)
        length += length % 2
        rec = bytearray(length)
        rec[0] = length
        struct.pack_into('<I', rec, 2, sector)
        struct.pack_into('>I', rec, 6, sector)
        struct.pack_into('<I', rec, 10, size)
        struct.pack_into('>I', rec, 14, size)
        rec[25] = 2 if is_dir else 0
        rec[32] = len(nm)
        rec[33:33 + len(nm)] = nm
        return bytes(rec)

    img = bytearray(total * SECTOR)

    def put_dir(d):
        out = record('\x00', dir_sector[d], SECTOR, True) + record('\x01', dir_sector[''], SECTOR, True)
        for sub in folders:
            if d == '' and '/' not in sub:
                out += record(sub, dir_sector[sub], SECTOR, True)
        for p in names:
            parent, _, leaf = p.rpartition('/')
            if parent == d:
                out += record(leaf + ';1', file_sector[p], len(files[p]), False)
        img[dir_sector[d] * SECTOR:dir_sector[d] * SECTOR + len(out)] = out
    put_dir('')
    for d in folders:
        put_dir(d)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[40:72] = b'FAKEDISC'.ljust(32)
    struct.pack_into('<I', pvd, 80, total)
    struct.pack_into('>I', pvd, 84, total)
    pvd[156:156 + 34] = record('\x00', 18, SECTOR, True)[:34]
    img[16 * SECTOR:17 * SECTOR] = pvd
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    img[17 * SECTOR:18 * SECTOR] = term
    for p in names:
        b = files[p]
        img[file_sector[p] * SECTOR:file_sector[p] * SECTOR + len(b)] = b
    return bytes(img)


def scratch(prefix='rgtest-'):
    return tempfile.TemporaryDirectory(prefix=prefix)


def fake_program_root(tmp, known):
    """A stand-in for the program's folder: CMakeLists.txt (the version), config/SCUS_974.90/hashes.json, tools/hd."""
    root = Path(tmp) / 'program'
    (root / 'config' / 'SCUS_974.90').mkdir(parents=True)
    (root / 'tools' / 'hd').mkdir(parents=True)
    (root / 'CMakeLists.txt').write_text('project(x VERSION 9.8.7)\n')
    (root / 'config' / 'SCUS_974.90' / 'hashes.json').write_text(json.dumps(known))
    (root / 'tools' / 'hd' / 'tool.py').write_text('print(1)\n')
    return root
