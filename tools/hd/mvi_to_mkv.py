#!/usr/bin/env python3
"""Rogue Galaxy (PS2, SCUS-97490) .MVI -> Matroska (MPEG-2 video copied bit for bit + FLAC PCM).

Usage (python -I mvi_to_mkv.py ...):
  mvi_to_mkv.py IN.MVI OUT.mkv            convert one movie
  mvi_to_mkv.py --all MOVIE_DIR OUT_DIR   convert every MOVIE/**/*.MVI (keeps the relative path)
  mvi_to_mkv.py --pss IN.MVI OUT.pss      only decrypt (standard MPEG-PS, plays in ffplay/VLC, no audio there)
  mvi_to_mkv.py --info IN.MVI             print the stream layout as JSON
  mvi_to_mkv.py --from-iso GAME.iso DIR   copy all 89 MOVIE/**.MVI from both DVD layers into DIR/
  options: --catalog FILE.json (with --all), --force, --ffmpeg PATH

Format (solved; see research/16-hd-cutscenes.md):
  * The file is N 2048-byte sectors plus a 4-byte trailer. Every sector is XORed with the
    same 2048-byte keystream: 512 little-endian words, w[0] = 0x98293255, w[i+1] = w[i] * 0x02E90EDD
    (mod 2^32). The trailer is XORed with w[0] and decrypts to 00 00 01 B9 (program end).
  * Plaintext = MPEG-2 program stream ("PSS"): video PES 0xE0 (MPEG-2 MP@ML 512x336, 29.97 fps,
    progressive, TMPGEnc 2.5), audio in private stream 1 (0xBD) sub-stream 0xFF with a 4-byte
    sub-stream header. The audio bytes form one Sony "SShd"/"SSbd" stream: a 0x28-byte header
    (SShd: type 1 = s16le PCM, rate, channels, interleave 0x200) then the SSbd body, channel blocks
    of `interleave` bytes alternating L,R. Private stream 2 (0xBE) packets are padding/nav and skipped.
  * Display aspect: a 512x336 frame is drawn 512x336 inside the PS2's 512x448 4:3 screen (black bars),
    i.e. it is a 16:9 picture (SAR 7:6). The bitstream's 4:3 flag is wrong; we write 16:9 into both
    the bitstream (mpeg2_metadata) and the container. TITLE01/TITLE02 are 512x448 at 30.000 fps and
    fill the 4:3 screen: they keep 4:3.
"""
import argparse, json, os, struct, subprocess, sys, tempfile, wave, shutil

SEC = 2048
K0 = 0x98293255
MUL = 0x02E90EDD


def _key():
    k, out = K0, bytearray()
    for _ in range(SEC // 4):
        out += struct.pack('<I', k)
        k = (k * MUL) & 0xFFFFFFFF
    return bytes(out)


KEY = _key()
CHUNK_SECTORS = 512  # 1 MiB per XOR
KEY_CHUNK = int.from_bytes(KEY * CHUNK_SECTORS, 'little')


def decrypt(data: bytes) -> bytes:
    if len(data) < SEC + 4 or (len(data) - 4) % SEC:
        raise ValueError('bad MVI length %d (expected N*2048 + 4)' % len(data))
    body_len = len(data) - 4
    out = bytearray()
    step = SEC * CHUNK_SECTORS
    for off in range(0, body_len, step):
        blk = data[off:min(off + step, body_len)]
        kk = KEY_CHUNK if len(blk) == step else int.from_bytes(KEY * (len(blk) // SEC), 'little')
        out += (int.from_bytes(blk, 'little') ^ kk).to_bytes(len(blk), 'little')
    out += (int.from_bytes(data[-4:], 'little') ^ K0).to_bytes(4, 'little')
    if out[:4] != b'\x00\x00\x01\xba':
        raise ValueError('decrypted data does not start with a pack header')
    return bytes(out)


def _pts(b, i):
    return (((b[i] >> 1) & 7) << 30) | (b[i + 1] << 22) | ((b[i + 2] >> 1) << 15) | (b[i + 3] << 7) | (b[i + 4] >> 1)


def demux(ps: bytes):
    """Walk the program stream. Returns dict with audio substreams (bytes), first PTS values, counts."""
    p, n = 0, len(ps)
    audio = {}
    first_pts = {}
    counts = {}
    end_code = False
    while p + 4 <= n:
        if ps[p:p + 3] != b'\x00\x00\x01':
            raise ValueError('program stream desync at 0x%X' % p)
        sid = ps[p + 3]
        if sid == 0xBA:
            if (ps[p + 4] & 0xC0) != 0x40:
                raise ValueError('MPEG-1 pack header at 0x%X (unexpected)' % p)
            p += 14 + (ps[p + 13] & 7)
            continue
        if sid == 0xB9:
            end_code = True
            p += 4
            break
        ln = struct.unpack_from('>H', ps, p + 4)[0]
        counts[sid] = counts.get(sid, 0) + 1
        if sid in (0xE0, 0xBD):
            flags, hl = ps[p + 7], ps[p + 8]
            if (flags & 0x80) and sid not in first_pts:
                first_pts[sid] = _pts(ps, p + 9)
            pay = p + 9 + hl
            if sid == 0xBD:
                sub = ps[pay]
                key = 'bd%02x' % sub
                if (flags & 0x80) and key not in first_pts:
                    first_pts[key] = _pts(ps, p + 9)
                audio.setdefault(sub, bytearray()).extend(ps[pay + 4:p + 6 + ln])
        p += 6 + ln
    return {'audio': audio, 'first_pts': first_pts, 'counts': counts, 'end_code': end_code,
            'trailing': n - p}


def parse_sshd(a: bytes):
    if a[:4] != b'SShd':
        raise ValueError('audio does not start with SShd: %r' % a[:8])
    hsize = struct.unpack_from('<I', a, 4)[0]
    typ, rate, ch, inter = struct.unpack_from('<IIII', a, 8)
    q = 8 + hsize
    if a[q:q + 4] != b'SSbd':
        raise ValueError('no SSbd after SShd')
    bsize = struct.unpack_from('<I', a, q + 4)[0]
    body = a[q + 8:q + 8 + bsize]
    return {'type': typ, 'rate': rate, 'channels': ch, 'interleave': inter, 'declared': bsize,
            'body': body}


def deinterleave(body: bytes, ch: int, inter: int) -> bytes:
    frame = inter * ch
    nfull = len(body) // frame
    body = body[:nfull * frame]  # a partial final frame cannot be split per channel safely
    out = bytearray(len(body))
    # per frame: ch blocks of `inter` bytes -> interleave samples (2 bytes each)
    import array
    for f in range(nfull):
        base = f * frame
        chans = [array.array('h', body[base + c * inter: base + (c + 1) * inter]) for c in range(ch)]
        mix = array.array('h', bytes(inter * ch))
        for c in range(ch):
            mix[c::ch] = chans[c]
        out[base:base + frame] = mix.tobytes()
    return bytes(out)


def display_aspect(height):
    """512x336 movies are drawn letterboxed inside the 4:3 512x448 screen -> 16:9 picture.
    512x448 movies (the title overlay's TITLE01/02) fill the 4:3 screen -> 4:3."""
    return '16:9' if height <= 336 else '4:3'


def ffmpeg_exe(opt):
    if opt:
        return opt
    d = os.environ.get('RG_FFMPEG_DIR')   # build_hd_pack.py points this at toolchain/ffmpeg/bin
    if d and os.path.exists(os.path.join(d, 'ffmpeg.exe')):
        return os.path.join(d, 'ffmpeg.exe')
    return shutil.which('ffmpeg') or 'ffmpeg'


def info_for(path):
    data = open(path, 'rb').read()
    ps = decrypt(data)
    dm = demux(ps)
    i = ps.find(b'\x00\x00\x01\xb3')
    w = (ps[i + 4] << 4) | (ps[i + 5] >> 4)
    h = ((ps[i + 5] & 15) << 8) | ps[i + 6]
    fr = {1: '24000/1001', 2: '24', 3: '25', 4: '30000/1001', 5: '30'}.get(ps[i + 7] & 15, '?')
    res = {'file': path, 'bytes': len(data), 'sectors': (len(data) - 4) // SEC,
           'width': w, 'height': h, 'frame_rate': fr, 'dar': display_aspect(h),
           'counts': {'0x%02X' % k: v for k, v in dm['counts'].items()},
           'first_pts': {(k if isinstance(k, str) else '0x%02X' % k): v for k, v in dm['first_pts'].items()},
           'end_code': dm['end_code'], 'audio': []}
    for sub, a in sorted(dm['audio'].items()):
        h = parse_sshd(bytes(a))
        res['audio'].append({'substream': '0x%02X' % sub, 'type': h['type'], 'rate': h['rate'],
                             'channels': h['channels'], 'interleave': h['interleave'],
                             'declared_bytes': h['declared'], 'got_bytes': len(h['body']),
                             'seconds': len(h['body']) / (h['rate'] * h['channels'] * 2)})
    return res, ps, dm


def convert(src, dst, ffmpeg=None, force=False, keep_pss=False):
    if os.path.exists(dst) and not force:
        return {'skipped': True, 'out': dst}
    res, ps, dm = info_for(src)
    os.makedirs(os.path.dirname(os.path.abspath(dst)) or '.', exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix='mvi_', dir=os.path.dirname(os.path.abspath(dst)))
    try:
        pss = os.path.join(tmpdir, 'v.pss')
        with open(pss, 'wb') as f:
            f.write(ps)
        cmd = [ffmpeg_exe(ffmpeg), '-hide_banner', '-nostdin', '-loglevel', 'error', '-y',
               '-fflags', '+genpts', '-f', 'mpeg', '-i', pss]
        maps = ['-map', '0:v:0']
        meta = []
        vpts = dm['first_pts'].get(0xE0, 0)
        for idx, (sub, a) in enumerate(sorted(dm['audio'].items())):
            h = parse_sshd(bytes(a))
            if h['type'] != 1:
                raise ValueError('unsupported SShd type %d' % h['type'])
            wav = os.path.join(tmpdir, 'a%02x.wav' % sub)
            with wave.open(wav, 'wb') as w:
                w.setnchannels(h['channels']); w.setsampwidth(2); w.setframerate(h['rate'])
                w.writeframes(deinterleave(h['body'], h['channels'], h['interleave']))
            apts = dm['first_pts'].get('bd%02x' % sub, vpts)
            off = (apts - vpts) / 90000.0
            cmd += ['-itsoffset', '%.6f' % off, '-i', wav]
            maps += ['-map', '%d:a:0' % (idx + 1)]
            meta += ['-metadata:s:a:%d' % idx, 'language=eng',
                     '-metadata:s:a:%d' % idx, 'title=PCM sub-stream 0x%02X' % sub]
            res['audio'][idx]['offset_s'] = off
        dar = display_aspect(res['height'])
        cmd += maps + ['-c:v', 'copy', '-bsf:v', 'mpeg2_metadata=display_aspect_ratio=' + dar.replace(':', '/'),
                       '-aspect', dar, '-c:a', 'flac', '-compression_level', '8'] + meta + \
               ['-metadata', 'title=' + os.path.splitext(os.path.basename(src))[0], dst]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('ffmpeg failed: ' + r.stderr[-2000:])
        if keep_pss:
            shutil.copyfile(pss, os.path.splitext(dst)[0] + '.pss')
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    res['out'] = dst
    return res


def iso_movies(iso):
    """All MOVIE/**.MVI on both layers of the DVD9 image: [(relpath, abs_lba, size)], distinct paths.
    Layer 1's descriptor sits at the layer break; its directory LBAs are relative to break-16."""
    f = open(iso, 'rb')

    def rd(lba, n):
        f.seek(lba * SEC); return f.read(n)

    def walk(base, rec, path, out):
        lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        data, p = rd(base + lba, size), 0
        while p < size:
            ln = data[p]
            if ln == 0:
                p = (p // SEC + 1) * SEC; continue
            r = data[p:p + ln]; name = r[33:33 + r[32]]
            if name not in (b'\0', b'\1'):
                nm = name.decode('latin1').split(';')[0]
                if r[25] & 2:
                    walk(base, r, path + nm + '/', out)
                else:
                    out.append((path + nm, base + struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]))
            p += ln
    pvd0 = rd(16, SEC)
    l0 = struct.unpack_from('<I', pvd0, 80)[0]
    files = []
    walk(0, pvd0[156:190], '', files)
    for brk in range(l0 - 16, l0 + 17):  # layer-1 descriptor near the break
        d = rd(brk, SEC)
        if d[1:6] == b'CD001' and d[0] == 1:
            walk(brk - 16, d[156:190], '', files)
            break
    seen, out = set(), []
    for p, lba, size in files:
        if p.upper().startswith('MOVIE/') and p.upper().endswith('.MVI') and p not in seen:
            seen.add(p); out.append((p, lba, size))
    return f, out


def extract_iso(iso, outdir):
    f, mv = iso_movies(iso)
    for p, lba, size in mv:
        dst = os.path.join(outdir, *p.split('/')[1:])
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        f.seek(lba * SEC)
        with open(dst + '.part', 'wb') as o:
            left = size
            while left:
                b = f.read(min(left, 8 << 20)); o.write(b); left -= len(b)
        os.replace(dst + '.part', dst)
    print('%d MVI files under %s' % (len(mv), outdir))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src'); ap.add_argument('dst', nargs='?')
    ap.add_argument('--all', action='store_true'); ap.add_argument('--pss', action='store_true')
    ap.add_argument('--info', action='store_true'); ap.add_argument('--force', action='store_true')
    ap.add_argument('--catalog'); ap.add_argument('--ffmpeg')
    ap.add_argument('--from-iso', action='store_true', help='src = ISO image, dst = dir: copy every MOVIE/**.MVI '
                    'from both disc layers (89 distinct files; the PC-extracted MOVIE folder has only layer 0)')
    a = ap.parse_args(argv)
    if a.from_iso:
        extract_iso(a.src, a.dst); return 0
    if a.info:
        print(json.dumps(info_for(a.src)[0], indent=1)); return 0
    if a.pss:
        open(a.dst, 'wb').write(decrypt(open(a.src, 'rb').read())); return 0
    if a.all:
        out = []
        for dp, _, fs in os.walk(a.src):
            for fn in sorted(fs):
                if fn.upper().endswith('.MVI'):
                    p = os.path.join(dp, fn)
                    rel = os.path.relpath(p, a.src)
                    dst = os.path.join(a.dst, os.path.splitext(rel)[0] + '.mkv')
                    try:
                        r = convert(p, dst, a.ffmpeg, a.force); r['rel'] = rel
                        print('ok  ', rel, flush=True)
                    except Exception as e:  # keep going; report
                        r = {'rel': rel, 'error': str(e)}; print('FAIL', rel, e, flush=True)
                    out.append(r)
        if a.catalog:
            json.dump(out, open(a.catalog, 'w'), indent=1)
        return 1 if any('error' in r for r in out) else 0
    r = convert(a.src, a.dst, a.ffmpeg, a.force)
    print(json.dumps(r, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
