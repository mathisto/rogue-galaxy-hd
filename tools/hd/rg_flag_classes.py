"""Write flagged-classes.csv: manifest rows in the classes the 2026-10-27 trial found risky for
realesrgan-x4plus-anime, with a recommended model per row (animevideov3, or x4plus for the stone floors). The pack
builder and build_hd_pack.py's upscale plan both read it.
Usage: python -I -X utf8 tools/hd/rg_flag_classes.py --manifest work/hd/all/manifest.csv --png work/hd/all/png
          --stone tools/hd/data/stone-floor.txt --skip tools/hd/data/skip-hashes.txt --out work/hd/upscaled/flagged-classes.csv
          [--decode-suspect]
--decode-suspect brings back the old rule that marked every kept direct32/direct24 picture "review-decode-first"
(before the stub filter of tim2_export.py those were mostly mis-decoded; with it they are real images).
Classes: effect_additive (path under effect\\, trial: x4plus-anime saturates glow cores / changes additive brightness),
foliage_cutout (map\\ or insect\\map with fully transparent pixels, or leaf/tree/vine/grass/flower/bush/plant in the
path; trial: posterised blobs, hard dark edges), smooth_cloud (opaque-ish low-detail textures incl. .sky: directional
streaks), stone_floor (hand-picked paving/cobble/brick tiles listed in data/stone-floor.txt; recommended set
x4plus, the general realesrgan-x4plus model, in upscaled/x4plus), decode_suspect (kept direct32/direct24: partly
mis-decoded smears pass the noise filter; review first).
"""
import argparse
import csv
from pathlib import Path
import numpy as np
from PIL import Image

PNG = None
BS = chr(92)
FOLIAGE = ('leaf', 'tree', 'vine', 'grass', 'flower', 'bush', 'plant', 'weed', 'moss', 'palm', 'branch')


def smooth_metric(h):
    a = np.array(Image.open(PNG / f'{h}.png').convert('RGBA'))
    m = a[..., 3] > 0
    g = a[..., :3].astype(np.float32).mean(-1)
    if m.sum() < 64:
        return None
    nd = (np.abs(np.diff(g, axis=0))[:, :-1].mean() + np.abs(np.diff(g, axis=1))[:-1, :].mean()) / 2
    return float(nd), float(g[m].std())


def main():
    global PNG
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True, help='CSV of the TIM2 pictures: hash,file,offset,w,h,kind,has_zero_alpha')
    ap.add_argument('--png', required=True, help='folder of the <hash>.png pictures')
    ap.add_argument('--stone', default='', help='stone-floor.txt (hand-picked hashes)')
    ap.add_argument('--skip', action='append', default=[], help='hash list to leave out')
    ap.add_argument('--out', required=True)
    ap.add_argument('--decode-suspect', action='store_true')
    a = ap.parse_args()
    PNG = Path(a.png)
    rows = list(csv.DictReader(open(a.manifest, newline='', encoding='utf-8')))
    skip = set()
    for f in a.skip:
        skip |= {ln.split('#')[0].split(',')[0].strip() for ln in open(f, encoding='utf-8')} - {'', 'hash'}
    stone = set()
    if a.stone and Path(a.stone).exists():
        stone = {ln.strip() for ln in open(a.stone, encoding='utf-8') if ln.strip() and not ln.startswith('#')}
    out = []
    for r in rows:
        h = r['hash']
        if h in skip:
            continue
        f = r['file'].lower()
        parts = f.split(BS)
        top = parts[1] if len(parts) > 2 else ''
        classes, rec = [], 'x4plus-anime'
        if top == 'effect':
            classes.append('effect_additive')
            rec = 'animevideov3'
        if (top in ('map', 'insect') and r['has_zero_alpha'] == '1') or any(k in f for k in FOLIAGE):
            classes.append('foliage_cutout')
            rec = 'animevideov3'
        w, hh = int(r['w']), int(r['h'])
        if min(w, hh) >= 32 and r['kind'] not in ('idx4',) and (top == 'map' and f.endswith('.sky') or r['has_zero_alpha'] == '0'):
            m = smooth_metric(h)
            if m and m[0] < 3.0 and m[1] > 6:
                classes.append('smooth_cloud')
                rec = 'animevideov3'
        if h in stone:
            # Paving, cobbles, flagstones and brick (hand-picked, data/stone-floor.txt): x4plus-anime draws a
            # 'cracked mosaic' of invented lines and flattens mortar; the general x4plus model keeps the stones.
            classes.append('stone_floor')
            rec = 'x4plus'
        if a.decode_suspect and r['kind'].startswith('direct'):
            classes.append('decode_suspect')
            rec = 'review-decode-first'
        if classes:
            out.append([h, r['file'], r['offset'], w, hh, r['kind'], '+'.join(classes), rec])
    dst = Path(a.out)
    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(dst, 'w', newline='', encoding='utf-8') as fh:
        wr = csv.writer(fh)
        wr.writerow(['hash', 'file', 'offset', 'w', 'h', 'kind', 'classes', 'recommended_set'])
        wr.writerows(out)
    from collections import Counter
    print('rows', len(out), Counter(c for o in out for c in o[6].split('+')), Counter(o[7] for o in out))


if __name__ == '__main__':
    main()
