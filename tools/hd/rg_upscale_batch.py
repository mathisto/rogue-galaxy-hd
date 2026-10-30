"""Resumable production batch upscaler for the exported Rogue Galaxy textures (realesrgan-ncnn-vulkan, 4x).

Usage (build_hd_pack.py runs this for you):
  python -I -X utf8 tools/hd/rg_upscale_batch.py --model realesrgan-x4plus-anime --out work/hd/upscaled/x4plus-anime
        --src work/hd/all/png --manifest work/hd/all/manifest.csv --realesrgan toolchain/realesrgan
        [--jobs 3:4:4] [--workers 8] [--batch-mp 4.0] [--limit N] [--only HASHLIST] [--skip HASHLIST] [--gentle]
Per texture <hash>.png (RGBA, from --src):
  * hashes listed in --skip files are excluded; textures with min(w,h) <= 8 px are written as 4x NEAREST (no model);
  * others: bleed colour under alpha 0 -> RGB through the model (directory mode, batches of ~--batch-mp megapixels,
    tile size 0/auto for max side <= 256, -t 512 above) -> recombine with a Lanczos-4x alpha -> <out>/<hash>.png.
Resumable: an output that already exists is skipped (outputs are written to .tmp then renamed). Per-batch timings and
failures go to <out>/_log.txt; failed files are retried one by one with a smaller tile, then listed in <out>/_failed.txt.
Only the current batch (and the next one, prepared in parallel) is ever on disk in <out>/_work.
"""
import argparse, csv, os, shutil, subprocess, sys, time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import numpy as np
from PIL import Image

SRC = None
EXE = None
MODELS = None
SCALE = 4
GENTLE = False
GPU = 0
RUN_TIMEOUT = 1800   # seconds for one ncnn run (a batch is a few MP); a hung run is killed and its files retried singly
IDLE_PRIORITY = 0x00000040
NO_WINDOW = 0x08000000


def read_hashes(path):
    """A skip/only list: one hash per line (# comments), or a CSV whose first column is the hash."""
    out = set()
    for ln in open(path, encoding='utf-8'):
        ln = ln.split('#')[0].strip()
        if ln and ln.lower() != 'hash':
            out.add(ln.split(',')[0].strip())
    return out


def bleed(rgb, alpha, iters=64):
    """Fill RGB under alpha==0 with nearby opaque colour (iterative 8-neighbour average); same as upscale_trial.py."""
    rgb = rgb.astype(np.float32)
    known = alpha > 0
    if known.all() or not known.any():
        return rgb.round().astype(np.uint8)
    h, w = known.shape
    for _ in range(iters):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        kp = np.pad(known, 1)
        rp = np.pad(rgb * known[..., None], ((1, 1), (1, 1), (0, 0)))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                acc += rp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
                cnt += kp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
        fill = (~known) & (cnt > 0)
        rgb[fill] = acc[fill] / cnt[fill][..., None]
        known = known | fill
    return rgb.round().astype(np.uint8)


def prep_one(args):
    h, src, dst = args
    im = Image.open(src).convert('RGBA')
    a = np.array(im)
    rgb = bleed(a[..., :3], a[..., 3]) if (a[..., 3] == 0).any() else a[..., :3]
    Image.fromarray(rgb).save(dst / f'{h}.png', compress_level=1)
    return h


def post_one(args):
    h, src, up, out = args
    try:
        im = Image.open(src).convert('RGBA')
        u = Image.open(up).convert('RGB')
        if u.size != (im.width * SCALE, im.height * SCALE):
            return h, f'size mismatch {u.size} vs {im.size}x{SCALE}'
        a = im.getchannel('A')
        u.putalpha(a.resize(u.size, Image.LANCZOS))
        tmp = out / f'{h}.png.tmp'
        u.save(tmp, format='PNG', compress_level=6)
        os.replace(tmp, out / f'{h}.png')
        os.remove(up)
        return h, None
    except Exception as e:  # noqa
        return h, repr(e)


def nearest_one(args):
    h, src, out = args
    im = Image.open(src).convert('RGBA')
    tmp = out / f'{h}.png.tmp'
    im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST).save(tmp, format='PNG')
    os.replace(tmp, out / f'{h}.png')
    return h


def run_exe(model, indir, outdir, jobs, tile, log):
    cmd = [str(EXE), '-i', str(indir), '-o', str(outdir), '-n', model, '-s', str(SCALE), '-m', str(MODELS), '-g', str(GPU),
           '-f', 'png', '-j', jobs, '-t', str(tile)]
    t = time.perf_counter()
    flags = (NO_WINDOW | (IDLE_PRIORITY if GENTLE else 0)) if os.name == 'nt' else 0
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, creationflags=flags, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        log(f'  exe timed out after {RUN_TIMEOUT}s and was stopped; its missing files are retried one by one')
        return time.perf_counter() - t
    dt = time.perf_counter() - t
    if p.returncode != 0:
        log(f'  exe rc={p.returncode} stderr tail: {p.stderr[-300:]!r}')
    return dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', default='2:4:4', help='ncnn -j load:proc:save threads')
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--batch-mp', type=float, default=4.0)
    ap.add_argument('--max-files', type=int, default=800)
    ap.add_argument('--limit', type=int, default=0, help='process only the first N pending textures (benchmark/test)')
    ap.add_argument('--only', default='', help='file with hashes (one per line) to restrict to')
    ap.add_argument('--src', required=True, help='dir of the input <hash>.png files')
    ap.add_argument('--manifest', required=True, help='CSV with hash,w,h columns (one row per picture)')
    ap.add_argument('--skip', action='append', default=[], help='file of hashes to leave out (repeatable)')
    ap.add_argument('--realesrgan', required=True, help='folder holding realesrgan-ncnn-vulkan.exe and models/')
    ap.add_argument('--gpu', type=int, default=0, help='Vulkan device index (ncnn -g)')
    ap.add_argument('--gentle', action='store_true', help='idle priority, one ncnn thread set, fewer workers')
    a = ap.parse_args()
    global SRC, EXE, MODELS, GENTLE, GPU
    GPU = a.gpu
    SRC = Path(a.src)
    EXE = Path(a.realesrgan) / ('realesrgan-ncnn-vulkan.exe' if os.name == 'nt' else 'realesrgan-ncnn-vulkan')
    MODELS = Path(a.realesrgan) / 'models'
    GENTLE = a.gentle
    if a.gentle:
        a.jobs = '1:1:1'
        a.workers = min(a.workers, 2)
    out = Path(a.out)
    work = out / '_work'
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / '_log.txt', 'a', encoding='utf-8')

    def log(msg):
        line = f'{time.strftime("%H:%M:%S")} {msg}'
        print(line, flush=True)
        logf.write(line + '\n')
        logf.flush()

    rows = list(csv.DictReader(open(a.manifest, newline='', encoding='utf-8')))
    skip = set()
    for f in a.skip:
        skip |= read_hashes(f)
    only = read_hashes(a.only) if a.only else None
    for tmp in out.glob('*.tmp'):
        tmp.unlink()
    shutil.rmtree(work, ignore_errors=True)
    done = {p.stem for p in out.glob('*.png')}
    tiny, todo = [], []
    for r in rows:
        h = r['hash']
        if h in skip or h in done or (only and h not in only):
            continue
        w, hh = int(r['w']), int(r['h'])
        (tiny if min(w, hh) <= 8 else todo).append((h, w, hh))
    todo.sort(key=lambda t: (max(t[1], t[2]) > 256, t[1] * t[2]))  # small first, big (tile 512) group last
    if a.limit:
        tiny, todo = tiny[:a.limit], todo[:a.limit]
    log(f'START model={a.model} jobs={a.jobs} workers={a.workers} pending={len(todo)} tiny={len(tiny)} '
        f'already_done={len(done)} skipped={len(skip)}')
    t0 = time.perf_counter()
    fails, total_mp, gpu_s = [], 0.0, 0.0
    pool = ProcessPoolExecutor(a.workers)

    for _ in pool.map(nearest_one, [(h, SRC / f'{h}.png', out) for h, _, _ in tiny], chunksize=32):
        pass
    if tiny:
        log(f'tiny nearest-4x written: {len(tiny)}')

    # build batches: homogeneous tile group, <= batch-mp megapixels and <= max-files files
    batches, cur, mp, big = [], [], 0.0, False
    for h, w, hh in todo:
        isbig = max(w, hh) > 256
        if cur and (isbig != big or mp + w * hh / 1e6 > a.batch_mp or len(cur) >= a.max_files):
            batches.append((big, cur))
            cur, mp = [], 0.0
        big = isbig
        cur.append((h, w, hh))
        mp += w * hh / 1e6
    if cur:
        batches.append((big, cur))

    def submit_prep(i):
        big, items = batches[i]
        d = work / f'b{i:05d}_in'
        d.mkdir(parents=True, exist_ok=True)
        return pool.map(prep_one, [(h, SRC / f'{h}.png', d) for h, _, _ in items], chunksize=8), d

    nxt = submit_prep(0) if batches else None
    post_pending = None
    ndone = 0
    for i, (big, items) in enumerate(batches):
        tb = time.perf_counter()
        gen, indir = nxt
        for _ in gen:  # wait for prep of this batch
            pass
        prep_s = time.perf_counter() - tb
        nxt = submit_prep(i + 1) if i + 1 < len(batches) else None
        if post_pending:
            post_pending()
            post_pending = None
        outdir = work / f'b{i:05d}_out'
        outdir.mkdir(parents=True, exist_ok=True)
        bmp = sum(w * hh for _, w, hh in items) / 1e6
        dt = run_exe(a.model, indir, outdir, a.jobs, 512 if big else 0, log)
        gpu_s += dt
        total_mp += bmp
        missing = [h for h, _, _ in items if not (outdir / f'{h}.png').exists()]
        for h in missing:  # retry singly with smaller tile
            one_in = work / f'retry_{h}'
            one_in.mkdir(exist_ok=True)
            shutil.copy(indir / f'{h}.png', one_in / f'{h}.png')
            run_exe(a.model, one_in, outdir, '1:1:1', 128, log)
            shutil.rmtree(one_in, ignore_errors=True)
            if not (outdir / f'{h}.png').exists():
                fails.append(h)
                log(f'  FAIL {h} (model output missing)')
        okitems = [(h, SRC / f'{h}.png', outdir / f'{h}.png', out) for h, _, _ in items if (outdir / f'{h}.png').exists()]
        pgen = pool.map(post_one, okitems, chunksize=4)

        def finish(pgen=pgen, indir=indir, outdir=outdir, i=i, n=len(items), bmp=bmp, dt=dt, prep_s=prep_s, nmiss=len(missing)):
            bad = [(h, e) for h, e in pgen if e]
            for h, e in bad:
                fails.append(h)
                log(f'  FAIL {h} post: {e}')
            shutil.rmtree(indir, ignore_errors=True)
            shutil.rmtree(outdir, ignore_errors=True)
            log(f'batch {i + 1}/{len(batches)} n={n} in={bmp:.2f}MP tile={"512" if big else "auto"} gpu={dt:.1f}s '
                f'({bmp / dt:.2f} MP/s in) prep_wait={prep_s:.1f}s retried={nmiss} post_fail={len(bad)}')
        post_pending = finish
        ndone += len(items)
    if post_pending:
        post_pending()
    wall = time.perf_counter() - t0
    shutil.rmtree(work, ignore_errors=True)
    if fails:
        with open(out / '_failed.txt', 'w') as fh:
            fh.write('\n'.join(fails) + '\n')
    log(f'END processed={ndone} tiny={len(tiny)} fails={len(fails)} wall={wall:.1f}s gpu_wall={gpu_s:.1f}s '
        f'input={total_mp:.2f}MP throughput={total_mp / max(wall, 1e-9):.3f} MP/s wall, {total_mp / max(gpu_s, 1e-9):.3f} MP/s gpu-only')


if __name__ == '__main__':
    main()
