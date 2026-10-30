"""Flag upscaled outputs whose mean colour (alpha-weighted) drifts from the input: catches model blackouts.
Usage: python -I -X utf8 tools/hd/rg_upscale_colorcheck.py <set dir> <src png dir> [threshold=25]
Writes <set dir>/_colordrift.csv (hash, drift, in_mean, out_mean), sorted by drift. The build uses it for the fp16
blackouts: realesrgan-x4plus-anime under ncnn fp16 returns black or dark pictures for ~130 bright textures."""
import sys, csv
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import numpy as np
from PIL import Image

SRCD = sys.argv[2] if len(sys.argv) > 2 else None


def mean(im):
    a = np.array(im.convert('RGBA')).astype(np.float32)
    w = a[..., 3:] / 255
    s = w.sum()
    return (a[..., :3] * w).sum((0, 1)) / s if s > 0 else np.zeros(3)


def one(args):
    h, d = args
    i = Image.open(Path(SRCD) / f'{h}.png')
    o = Image.open(Path(d) / f'{h}.png')
    o = o.resize((max(1, o.width // 4), max(1, o.height // 4)), Image.BOX)
    mi, mo = mean(i), mean(o)
    return h, float(np.abs(mi - mo).max()), mi.round(1).tolist(), mo.round(1).tolist()


def main():
    d = sys.argv[1]
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 25
    hs = [p.stem for p in Path(d).glob('*.png')]
    with ProcessPoolExecutor(8) as ex:
        res = sorted(ex.map(one, [(h, d) for h in hs], chunksize=32), key=lambda r: -r[1])
    with open(Path(d) / '_colordrift.csv', 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['hash', 'drift', 'in_mean', 'out_mean'])
        w.writerows(res)
    print('over', thr, ':', sum(1 for r in res if r[1] > thr), 'of', len(res))
    for r in res[:12]:
        print(r)


if __name__ == '__main__':
    main()
