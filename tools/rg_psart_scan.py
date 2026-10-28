"""Find PlayStation button art in the exported UI textures (NCC, multi-scale).

  THRESH=0.78 DIFF=0.15 py -3.12 -I tools/rg_psart_scan.py <ui/all> <gaiji2.png> scan.csv [hash,...]
  py -3.12 -I tools/rg_psart_emit.py scan.csv <ui/all>/unique.csv src/button_art_table.inc tools/rg_psart_exclude.txt

<ui/all> is im3_export.py's UI export (research 11: png/<hash>.png, unique.csv
with gs_key); gaiji2.png the game's gaiji sheet exported the same way. The
templates are gaiji2's eight icons (the game's own style), the inner part of
each (so the background never counts), at nine scales; a hit needs a
normalised correlation over THRESH and a mean difference under DIFF. Output:
hash,name,cat,w,h,kind,x,y,bw,bh,score (the icon's box in the picture's
texels). About 40 minutes for the 529 UI textures. Only boxes come out.
"""
import csv, sys
import numpy as np
from PIL import Image

src, gaiji, out = sys.argv[1], sys.argv[2], sys.argv[3]
import os
THRESH = float(os.environ.get('THRESH', '0.84'))
DIFF = float(os.environ.get('DIFF', '0.10'))
G = Image.open(gaiji).convert('RGBA')
cells = {'L2': (0, 0, 32, 20), 'L1': (32, 0, 32, 20), 'R1': (64, 0, 32, 20), 'R2': (96, 0, 32, 20),
         'circle': (0, 20, 22, 22), 'triangle': (22, 20, 22, 22), 'square': (44, 20, 22, 22), 'cross': (66, 20, 22, 22)}


def flat(img):
    """RGBA -> float RGB over mid grey."""
    a = np.asarray(img.convert('RGBA'), dtype=np.float32) / 255.0
    return a[..., :3] * a[..., 3:4] + 0.5 * (1 - a[..., 3:4])


# EXTRA="kind=crop.png;..." adds templates cut from other pictures (the
# START/SELECT pictograms, the d-pad, the sticks: kept outside the
# repository, they are the game's art); ONLY_EXTRA=1 drops gaiji2's eight.
extra = [e.split('=', 1) for e in os.environ.get('EXTRA', '').split(';') if e]
sources = [] if os.environ.get('ONLY_EXTRA') == '1' else \
    [(kind, G.crop((x, y, x + w, y + h))) for kind, (x, y, w, h) in cells.items()]
sources += [(kind, Image.open(path).convert('RGBA')) for kind, path in extra]
SCALES = [float(v) for v in os.environ.get('SCALES', '0.55,0.65,0.75,0.85,1.0,1.15,1.3,1.45,1.6').split(',')]
templates = []
for kind, crop in sources:
    # trim transparent margins
    bb = crop.getchannel('A').point(lambda v: 255 if v > 16 else 0).getbbox()
    crop = crop.crop(bb)
    for s in SCALES:
        tw, th = max(6, round(crop.width * s)), max(6, round(crop.height * s))
        full = flat(crop.resize((tw, th), Image.LANCZOS))
        # The inner part only (all opaque), so the background never counts.
        fx, fy = (0.16, 0.16) if crop.width == crop.height else (0.1, 0.18)
        ix, iy = round(tw * fx), round(th * fy)
        t = full[iy:th - iy, ix:tw - ix]
        templates.append((kind, s, t, ix, iy, tw, th))


def ncc(img, t):
    H, W, _ = img.shape
    th, tw, _ = t.shape
    if th > H or tw > W:
        return None
    n = th * tw
    total = np.zeros((H - th + 1, W - tw + 1), np.float32)
    var_t = 0.0
    num = 0.0
    den_i = 0.0
    for c in range(3):
        I = img[..., c]
        T = t[..., c] - t[..., c].mean()
        F = np.fft.rfft2(I)
        K = np.fft.rfft2(T[::-1, ::-1], s=I.shape)
        corr = np.fft.irfft2(F * K, s=I.shape)[th - 1:, tw - 1:]
        S = np.pad(I, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        S2 = np.pad(I * I, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        box = lambda A: A[th:, tw:] - A[:-th, tw:] - A[th:, :-tw] + A[:-th, :-tw]
        s1, s2 = box(S), box(S2)
        num = num + corr
        den_i = den_i + np.maximum(s2 - s1 * s1 / n, 0)
        var_t += (T * T).sum()
    return num / (np.sqrt(den_i * var_t) + 1e-6)


rows = list(csv.DictReader(open(src + '/unique.csv', encoding='utf-8')))
if len(sys.argv) > 4:
    rows = [r for r in rows if r['hash'] in sys.argv[4].split(',')]
found = []
for i, r in enumerate(rows):
    if r['cat'] in ('font',):
        continue
    try:
        img = flat(Image.open(f"{src}/png/{r['hash']}.png"))
    except Exception:
        continue
    hits = []
    for kind, s, t, ix, iy, fw, fh in templates:
        m = ncc(img, t)
        if m is None:
            continue
        th, tw, _ = t.shape
        ys, xs = np.where(m > THRESH)
        for y, x in zip(ys, xs):
            win = img[y:y + th, x:x + tw]
            if win.std() < 0.08 or np.abs(win - t).mean() > DIFF:
                continue
            hits.append((float(m[y, x]), kind, int(x) - ix, int(y) - iy, fw, fh))
    hits.sort(reverse=True)
    kept = []
    for h in hits:
        sc, kind, x, y, w, hh = h
        if any(abs(x - k[2]) < max(w, k[4]) * 0.6 and abs(y - k[3]) < max(hh, k[5]) * 0.6 for k in kept):
            continue
        kept.append(h)
    for sc, kind, x, y, w, hh in kept:
        found.append([r['hash'], r['name'], r['cat'], r['w'], r['h'], kind, x, y, w, hh, round(sc, 3)])
    if i % 50 == 0:
        print(i, len(found), flush=True)
with open(out, 'w', newline='') as f:
    wr = csv.writer(f)
    wr.writerow(['hash', 'name', 'cat', 'w', 'h', 'kind', 'x', 'y', 'bw', 'bh', 'score'])
    wr.writerows(found)
print('done', len(found))




