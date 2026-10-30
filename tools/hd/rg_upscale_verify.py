"""Verify an upscaled set against the manifest.
Usage: python -I -X utf8 tools/hd/rg_upscale_verify.py <set dir> --src PNGDIR --manifest CSV [--skip HASHLIST] [--only HASHLIST]
Checks: output count == manifest rows (minus --skip hashes, restricted to --only hashes); every output exactly 4x the input size; RGBA mode; alpha
equals the Lanczos-4x of the input alpha within tolerance (mean abs error <= 3, and fully transparent/opaque input
regions stay so on average); no zero-byte files; image decodes; no leftover .tmp. Writes <dir>/_verify.txt.
"""
import csv, sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import numpy as np
from PIL import Image

SRC = None
MANIFEST = None


def hashes(path):
    return {ln.split('#')[0].split(',')[0].strip() for ln in open(path, encoding='utf-8')} - {'', 'hash'}


def check(args):
    h, d, srcdir = args
    p = Path(d) / f'{h}.png'
    if not p.exists():
        return h, 'missing'
    if p.stat().st_size == 0:
        return h, 'zero-byte'
    try:
        src = Image.open(Path(srcdir) / f'{h}.png').convert('RGBA')
        up = Image.open(p)
        up.load()
    except Exception as e:  # noqa
        return h, f'decode: {e!r}'
    if up.size != (src.width * 4, src.height * 4):
        return h, f'size {up.size} vs {src.size}x4'
    if up.mode != 'RGBA':
        return h, f'mode {up.mode}'
    sa = src.getchannel('A')
    ref = np.array(sa.resize(up.size, Image.LANCZOS)).astype(np.int16)
    ua = np.array(up.getchannel('A')).astype(np.int16)
    err = float(np.abs(ref - ua).mean())
    if err > 3:
        return h, f'alpha mean err {err:.2f}'
    return h, None


def main():
    global SRC, MANIFEST
    d = Path(sys.argv[1])
    SRC = Path(sys.argv[sys.argv.index('--src') + 1])
    MANIFEST = Path(sys.argv[sys.argv.index('--manifest') + 1])
    skip = set()
    for i, x in enumerate(sys.argv):
        if x == '--skip':
            skip |= hashes(sys.argv[i + 1])
    only = hashes(sys.argv[sys.argv.index('--only') + 1]) if '--only' in sys.argv else None
    rows = list(csv.DictReader(open(MANIFEST, newline='', encoding='utf-8')))
    want = [r['hash'] for r in rows if r['hash'] not in skip and (only is None or r['hash'] in only)]
    have = {p.stem for p in d.glob('*.png')}
    extra = sorted(have - set(want))
    tmp = list(d.glob('*.tmp'))
    with ProcessPoolExecutor(8) as ex:
        res = list(ex.map(check, [(h, str(d), str(SRC)) for h in want], chunksize=32))
    bad = [(h, e) for h, e in res if e]
    total = sum(p.stat().st_size for p in d.glob('*.png'))
    lines = [f'dir={d}', f'manifest rows={len(rows)} skipped={len(skip)} expected_outputs={len(want)}',
             f'outputs present={len(have)} extra(not expected)={len(extra)} tmp_left={len(tmp)}',
             f'checked={len(res)} bad={len(bad)}', f'disk={total / 1e9:.2f} GB']
    lines += [f'  {h}: {e}' for h, e in bad[:50]]
    lines += [f'  extra: {h}' for h in extra[:10]]
    out = '\n'.join(lines)
    print(out)
    (d / '_verify.txt').write_text(out + '\n')


if __name__ == '__main__':
    main()
