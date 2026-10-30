"""Rogue Galaxy: the recomp renderer's texture identity for every TIM2 picture on the disc.

usage: python -I rg_texkeys.py <extracted_dir> <out_csv>

For every TIM2 picture in the extracted disc tree (the same scan as tim2_export.py) this computes
  * pixhash: tim2_export.py's dedupe hash (sha1 of the decoded RGBA + w,h), which names the
    upscaled PNGs (<set>/<pixhash>.png in the build's work folder);
  * texels/clut: the identity the recomp's GS texture cache (thirdparty/dq8-gfx
    sdlgpu_textures.cpp, GsTextureCache::expand and hashClutWindow) gives the texture once the
    game has uploaded it, bit for bit:
      texels = FNV-1a 64, seeded with hashStep(W), hashStep(H), hashStep(PSM) (each value as 8
               little-endian bytes, one FNV round a byte), then each row of raw texels as the
               cache reads it, 8 bytes a round (hash = (hash ^ word) * prime), the row's last
               (len % 8) bytes one a round. Rows: T8 = one index byte a texel (deswizzled);
               T4 = two indices a byte, low nibble first (= the TIM2's own bytes);
               CT32 = the four RGBA bytes as stored (alpha 0x80 = opaque).
      clut   = hashStep over cpsm, csm (0, CSM1), then each of the 16 (T4) or 256 (T8) palette
               entries as the texture indexes them (CSM1 resolved), the raw 32-bit word.
The identity depends only on the picture, its size and format, so it is computed straight from
the file; whether the game uploads the file unchanged is checked against a runtime log
(rg_texpack.py --validate).
"""
import csv
import hashlib
import os
import struct
import sys
import time

import numpy as np

# --- TIM2 decoding: copied from tim2_export.py (2026-10-27), so pixhash matches the upscaled set ---


def bpp_conv(raw, kind, n):
    if kind == 3:
        a = np.frombuffer(raw[:n * 4], np.uint8).reshape(-1, 4).copy()
        a[:, 3] = np.minimum(255, a[:, 3].astype(np.int32) * 255 // 128).astype(np.uint8)
        return a
    if kind == 2:
        a = np.frombuffer(raw[:n * 3], np.uint8).reshape(-1, 3)
        return np.concatenate([a, np.full((len(a), 1), 255, np.uint8)], 1)
    v = np.frombuffer(raw[:n * 2], '<u2').astype(np.uint32)
    r = (v & 31) * 255 // 31; g = ((v >> 5) & 31) * 255 // 31; b = ((v >> 10) & 31) * 255 // 31
    al = np.where(v & 0x8000, 255, 0)
    return np.stack([r, g, b, al], 1).astype(np.uint8)


def unswizzle_clut(p):
    p = p.copy()
    for base in range(0, 256, 32):
        a = p[base + 8:base + 16].copy(); b = p[base + 16:base + 24].copy()
        p[base + 8:base + 16] = b; p[base + 16:base + 24] = a
    return p


def unswizzle8_index(w, h):
    y, x = np.mgrid[0:h, 0:w]
    block = (y & ~0xf) * w + (x & ~0xf) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    posy = (((y & ~3) >> 1) + (y & 1)) & 7
    col = posy * w * 2 + ((x + swap) & 7) * 4
    bn = ((y >> 1) & 1) + ((x >> 2) & 2)
    return (block + col + bn).reshape(-1)


_UNSWZ = {}


def unswizzle8(idx, w, h):
    key = (w, h)
    if key not in _UNSWZ:
        _UNSWZ[key] = unswizzle8_index(w, h)
    return idx[_UNSWZ[key]]


def parse_at(b, off):
    if b[off:off + 4] != b'TIM2' or off + 16 > len(b): return None
    ver, fmt, npic = struct.unpack_from('<BBH', b, off + 4)
    if npic == 0 or npic > 64 or ver not in (3, 4): return None
    pos = off + (16 if fmt == 0 else 128)
    out = []
    for _ in range(npic):
        if pos + 48 > len(b): return None
        tot, clsz, imsz, hsz, ccol, pfmt, mips, ctype, itype, w, h = struct.unpack_from('<IIIHHBBBBHH', b, pos)
        if hsz < 48 or hsz > 256 or w == 0 or h == 0 or w > 2048 or h > 2048 or itype not in (1, 2, 3, 4, 5): return None
        if tot < hsz + imsz + clsz or pos + tot > len(b) + 16: return None
        bits = {1: 16, 2: 24, 3: 32, 4: 4, 5: 8}[itype]
        if imsz < w * h * bits // 8: return None
        ip = pos + hsz
        base_sz = w * h * bits // 8
        img = b[ip:ip + base_sz]
        clut = b[ip + imsz:ip + imsz + clsz] if clsz else b''
        tex0 = struct.unpack_from('<Q', b, pos + 24)[0]
        out.append(dict(psm=(tex0 >> 20) & 63, tex0=tex0, off=pos, total=tot, w=w, h=h, itype=itype, ctype=ctype,
                        ccol=ccol, mips=mips, img=img, clut=clut, size=tot))
        pos += tot
    return out, pos - off


def decode(p, linear=False):
    """-> (rgba hxwx4, kind, raw_rows (h, rowbytes) uint8 as the GS cache hashes them, palette words or None).
    linear: an 8-bit picture's indices in file order instead of GS-swizzled (tim2_export.py's prefer_linear
    case, about 1% of idx8 pictures; which one the game uses is settled by what matches at runtime)."""
    w, h, it = p['w'], p['h'], p['itype']
    n = w * h
    if it in (1, 2, 3):
        px = bpp_conv(p['img'], it, n)
        raw = None
        if it == 3:
            raw = np.frombuffer(p['img'][:n * 4], np.uint8).reshape(h, w * 4)
        return px.reshape(h, w, 4), 'direct%d' % {1: 16, 2: 24, 3: 32}[it], raw, None
    idx = np.frombuffer(p['img'], np.uint8)
    if it == 4:
        packed = idx[:n // 2]
        idx = np.stack([idx & 15, idx >> 4], 1).reshape(-1)
    idx = idx[:n]
    if it == 5 and not linear:
        idx = unswizzle8(idx, w, h)
    ck = p['ctype'] & 0x3f
    if ck not in (1, 2, 3) or not p['clut']: return None
    csz = {1: 2, 2: 3, 3: 4}[ck]
    ncol = min(len(p['clut']) // csz, 256)
    pal = bpp_conv(p['clut'], ck, ncol)
    words = None
    if ck == 3:
        words = np.frombuffer(p['clut'][:ncol * 4], '<u4').copy()
    if it == 5 and ncol >= 256 and not (p['ctype'] & 0x80):
        pal = unswizzle_clut(pal)
        if words is not None:
            words = unswizzle_clut(words)
    if len(pal) < 256:
        pal = np.concatenate([pal, np.zeros((256 - len(pal), 4), np.uint8)])
    rgba = pal[idx].reshape(h, w, 4)
    if it == 4:
        raw = packed.reshape(h, w // 2) if w % 2 == 0 else None
    else:
        raw = idx.reshape(h, w)
    return rgba, ('idx4' if it == 4 else 'idx8'), raw, words


# --- the renderer's hashes (sdlgpu_textures.cpp) ---

SEED = np.uint64(1469598103934665603)
PRIME = np.uint64(1099511628211)
GS_PSM = {'idx8': 0x13, 'idx4': 0x14, 'direct32': 0x00, 'direct24': 0x01, 'direct16': 0x02}


def hash_step(h, value):
    """hashStep: 8 little-endian bytes of value, one FNV-1a round each. h: uint64 array."""
    v = np.asarray(value, dtype=np.uint64)
    for byte in range(8):
        h = (h ^ ((v >> np.uint64(byte * 8)) & np.uint64(0xff))) * PRIME
    return h


def hash_rows(h, rows):
    """hashBytes per row, for a batch: rows (N, H, B) uint8, h (N,) uint64."""
    n, height, b = rows.shape
    whole = b // 8
    if whole:
        words = np.ascontiguousarray(rows[:, :, :whole * 8]).view('<u8').astype(np.uint64)  # (N,H,whole)
    for y in range(height):
        for k in range(whole):
            h = (h ^ words[:, y, k]) * PRIME
        for t in range(whole * 8, b):
            h = (h ^ rows[:, y, t].astype(np.uint64)) * PRIME
    return h


def texel_hashes(rows, w, h, psm):
    n = rows.shape[0]
    hv = np.full(n, SEED, dtype=np.uint64)
    with np.errstate(over='ignore'):
        hv = hash_step(hv, np.full(n, w, np.uint64))
        hv = hash_step(hv, np.full(n, h, np.uint64))
        hv = hash_step(hv, np.full(n, psm, np.uint64))
        return hash_rows(hv, rows)


def clut_hashes(words, cpsm=0, csm=0):
    """words (N, E) uint32 palette entries in index order (16 or 256)."""
    n = words.shape[0]
    hv = np.full(n, SEED, dtype=np.uint64)
    with np.errstate(over='ignore'):
        hv = hash_step(hv, np.full(n, cpsm, np.uint64))
        hv = hash_step(hv, np.full(n, csm, np.uint64))
        for e in range(words.shape[1]):
            hv = hash_step(hv, words[:, e].astype(np.uint64))
    return hv


def scan(src):
    """Yields one dict per picture."""
    for dp, _, fs in os.walk(src):
        for f in sorted(fs):
            if f.lower().endswith('.mwv') or f.lower().endswith('.snd'):
                continue
            path = os.path.join(dp, f)
            b = open(path, 'rb').read()
            i = b.find(b'TIM2')
            while i >= 0:
                r = parse_at(b, i)
                if r:
                    pics, _span = r
                    for k, p in enumerate(pics):
                        p['file'] = os.path.relpath(path, src)
                        p['tim2'] = i
                        p['index'] = k
                        yield p
                i = b.find(b'TIM2', i + 4)


def main():
    src, out = sys.argv[1], sys.argv[2]
    t0 = time.time()
    recs = []
    batches = {}   # (h, rowbytes, w, psm) -> list of (rec index, rows)
    cbatches = {}  # entries -> list of (rec index, words)
    for p in scan(src):
      for layout in ('gs', 'linear') if p['itype'] == 5 else ('gs',):
        try:
            d = decode(p, linear=layout == 'linear')
        except Exception:
            d = None
        if d is None:
            continue
        rgba, kind, raw, words = d
        w, h = p['w'], p['h']
        pix = hashlib.sha1(rgba.tobytes() + struct.pack('<HH', w, h)).hexdigest()[:16]
        psm = GS_PSM[kind]
        rec = dict(file=p['file'], tim2=p['tim2'], index=p['index'], offset=p['off'], w=w, h=h, kind=kind,
                   layout=layout, psm=psm, tex0psm=p['psm'], ctype=p['ctype'], ccol=p['ccol'], pixhash=pix,
                   texels='', clut='')
        ri = len(recs)
        recs.append(rec)
        if raw is not None:
            batches.setdefault((h, raw.shape[1], w, psm), []).append((ri, raw))
        if words is not None and kind in ('idx4', 'idx8'):
            e = 16 if kind == 'idx4' else 256
            if len(words) >= e:
                cbatches.setdefault(e, []).append((ri, words[:e]))
            elif kind == 'idx8':
                rec['clut'] = 'short'  # fewer than 256 entries: the rest of the CLUT is whatever VRAM holds
    print('pictures', len(recs), 'decode', round(time.time() - t0, 1), 's', file=sys.stderr)
    for (h, b, w, psm), items in batches.items():
        rows = np.stack([r for _, r in items])
        hv = texel_hashes(rows, w, h, psm)
        for (ri, _), v in zip(items, hv):
            recs[ri]['texels'] = '%016x' % int(v)
    for e, items in cbatches.items():
        words = np.stack([wd for _, wd in items])
        hv = clut_hashes(words)
        for (ri, _), v in zip(items, hv):
            recs[ri]['clut'] = '%016x' % int(v)
    print('hashed', round(time.time() - t0, 1), 's', file=sys.stderr)
    with open(out, 'w', newline='', encoding='utf-8') as fh:
        wr = csv.DictWriter(fh, fieldnames=list(recs[0].keys()))
        wr.writeheader()
        wr.writerows(recs)


if __name__ == '__main__':
    main()
