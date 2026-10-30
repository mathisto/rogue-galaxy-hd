"""Completes the HD texture pack from textures the game showed with no replacement (the recomp's learn mode).

usage: python -I -X utf8 rg_texlearn.py LEARN_DIR PACK_DIR --source PNGDIR=UPSCALEDDIR[,UPSCALEDDIR...] [--source ...]
                                          --realesrgan DIR [--work DIR] [--model realesrgan-x4plus-anime]
                                          [--no-upscale] [--max-side 512] [--include-direct] [--actions CSV]

LEARN_DIR is what the game wrote with RG_TEXTURE_LEARN=LEARN_DIR: one PNG per texture the GS texture cache built and
found no replacement for, named by its key (<texels>_<clut>_<W>x<H>_psm<NN>.png, alpha 255 = the GS's 0x80), exactly
the name a pack file needs. For each one:
  1. a disc picture with the same pixels (a --source folder: work/hd/all/png) that already has an
     upscale: the upscale is linked into PACK_DIR\\learned\\ under the learned name (no GPU work);
  2. otherwise (pictures the disc export missed or packed differently: the export's 512-byte offset bug, sheets
     composed at runtime) the learned picture itself is upscaled 4x with the same model and alpha handling as the
     main set (rg_upscale_batch.py: colour bled under alpha 0, Lanczos alpha) into --work and linked in.
Skipped: frame copies and other direct-colour 512x512 textures (psm 00/01/02 unless --include-direct), pictures over
--max-side (texture atlases the game refills, such as the 1024x1024 item icon sheet), and pictures of one colour.
Re-runnable: what is already in the pack or the work folder is kept. Then F8 in the game (or a restart) reads it.
"""
import argparse
import csv
import hashlib
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def pixkey(path):
    im = np.asarray(Image.open(path).convert('RGBA'))
    return hashlib.sha1(im.tobytes() + bytes(str(im.shape), 'ascii')).hexdigest(), im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('learn')
    ap.add_argument('pack')
    ap.add_argument('--source', action='append', required=True, metavar='PNGDIR=UPSCALEDDIR[,UPSCALEDDIR]',
                    help='a folder of the disc pictures (<hash>.png) and the folders holding their upscales, best first')
    ap.add_argument('--realesrgan', required=True)
    ap.add_argument('--actions', default='')
    ap.add_argument('--work', default='', help='work folder (default: beside PACK_DIR, learn-work)')
    ap.add_argument('--model', default='realesrgan-x4plus-anime')
    ap.add_argument('--no-upscale', action='store_true')
    ap.add_argument('--max-side', type=int, default=512)
    ap.add_argument('--include-direct', action='store_true')
    ap.add_argument('--exclude', default='', help='file of learned names never to place')
    a = ap.parse_args()

    if not a.work:
        a.work = os.path.join(os.path.dirname(os.path.abspath(a.pack)), 'learn-work')
    drop = set()
    if a.actions and os.path.exists(a.actions):
        drop = {r['old_hash'] for r in csv.DictReader(open(a.actions, encoding='utf-8')) if r['action'] != 'keep_unchanged'}
    SOURCES = [(x.split('=', 1)[0], x.split('=', 1)[1].split(',')) for x in a.source]

    excluded = set(l.strip() for l in open(a.exclude)) if a.exclude else set()
    learned = []
    for n in sorted(os.listdir(a.learn)):
        if not n.endswith('.png'):
            continue
        parts = n[:-4].split('_')
        if len(parts) != 4:
            continue
        w, h = map(int, parts[2].split('x'))
        psm = int(parts[3][3:], 16)
        if max(w, h) > a.max_side or (psm in (0, 1, 2) and not a.include_direct) or max(w, h) <= 16:
            continue
        # 4-bit pages of 512 or more across are the game's text pages (fonts, names, the loading recap's lines):
        # the modern font draws those, and an AI upscale of letters is what the user did not like.
        if psm in (0x14, 0x24, 0x2c) and w >= 512:
            continue
        if n in excluded:
            continue
        learned.append((n, w, h))
    print('learned textures to place:', len(learned))

    # Disc pictures of the same sizes, by pixel content.
    sizes = {(w, h) for _, w, h in learned}
    index = {}
    for src, ups in SOURCES:
        if not os.path.isdir(src):
            continue
        for f in os.listdir(src):
            stem = f[:-4]
            if stem in drop:
                continue
            up = next((os.path.join(u, f) for u in ups if os.path.exists(os.path.join(u, f))), None)
            if not up:
                continue
            try:
                with Image.open(os.path.join(src, f)) as im:
                    if im.size not in sizes:
                        continue
            except OSError:
                continue
            k, _ = pixkey(os.path.join(src, f))
            index.setdefault(k, up)

    out = os.path.join(a.pack, 'learned')
    os.makedirs(out, exist_ok=True)
    work_png = os.path.join(a.work, 'png')
    os.makedirs(work_png, exist_ok=True)
    matched = upscaled = kept = flat = 0
    todo = []
    for n, w, h in learned:
        dst = os.path.join(out, n)
        if os.path.exists(dst):
            kept += 1
            continue
        k, im = pixkey(os.path.join(a.learn, n))
        if (im == im[0, 0]).all():
            flat += 1
            continue
        # Noise (a render target or a buffer read as a texture): neighbours differ wildly.
        g = im[..., :3].astype(np.int16)
        if np.abs(np.diff(g, axis=1)).mean() > 40 and np.abs(np.diff(g, axis=0)).mean() > 40:
            flat += 1
            continue
        if k in index:
            os.link(index[k], dst)
            matched += 1
            continue
        stem = n[:-4]
        Image.fromarray(im, 'RGBA').save(os.path.join(work_png, n))
        todo.append((stem, w, h))
    print('already in the pack', kept, '| matched a disc picture', matched, '| one colour or noise', flat,
          '| to upscale', len(todo))
    if todo and not a.no_upscale:
        manifest = os.path.join(a.work, 'manifest.csv')
        with open(manifest, 'w', newline='', encoding='utf-8') as fh:
            wr = csv.writer(fh)
            wr.writerow(['hash', 'w', 'h'])
            for stem, w, h in todo:
                wr.writerow([stem, w, h])
        upout = os.path.join(a.work, a.model.replace('realesrgan-', '').replace('realesr-', ''))
        cmd = [sys.executable, '-I', '-X', 'utf8', os.path.join(HERE, 'rg_upscale_batch.py'), '--model', a.model,
               '--out', upout, '--src', work_png, '--manifest', manifest, '--realesrgan', a.realesrgan, '--workers', '4']
        print(' '.join(cmd))
        subprocess.run(cmd, check=False)
        for stem, w, h in todo:
            src = os.path.join(upout, stem + '.png')
            dst = os.path.join(out, stem + '.png')
            if os.path.exists(src) and not os.path.exists(dst):
                os.link(src, dst)
                upscaled += 1
        print('upscaled and linked', upscaled, 'of', len(todo))


if __name__ == '__main__':
    main()
