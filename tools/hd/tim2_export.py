"""TIM2 scanner/decoder/exporter.
usage: python -I tim2_export.py <extracted_dir> <out_dir> [--limit N] [--no-png] [--legacy] [--rows rows.csv]
Scans every non-.mwv file for 'TIM2' blocks, decodes, dedupes by pixel hash, writes PNGs + manifest.csv.

2026-10-28 fix (research/12-tim2-decode-fix.md): about 1,060 of the 1,324 direct24/direct32 'pictures' are NOT images.
They are 64-byte header-only stubs (16-byte TIM2 header + 48-byte picture header, TEX0 = 0, TEX1 = 0xffffff8f00000000,
no CLUT, tot = 48 + w*h*4) that declare a runtime texture slot; the bytes after them are the next TIM2 header or a
TEX_ANIME script, not pixels. They are now detected (is_stub) and written to <out>/stubs.csv instead of PNGs.
Also: 8-bit data that is stored linear (not GS-swizzled) is detected by a flatness test (auto swizzle, idx8 only).
  --legacy     old behaviour (no stub filter, always unswizzle idx8), for reproducing the 2026-10-27 export
  --rows CSV   only decode pictures whose (file, offset) are in CSV (columns file,offset; offset = picture-header offset,
               as in manifest.csv); used to re-export just the previously bad rows"""
import os, sys, struct, hashlib, csv, time
import numpy as np
from PIL import Image

def bpp_conv(raw, kind, n):
    """raw bytes -> (n,4) uint8 RGBA. kind: 1=16bit 2=24bit 3=32bit. alpha scaled 0x80->255."""
    if kind == 3:
        a = np.frombuffer(raw[:n*4], np.uint8).reshape(-1, 4).copy()
        a[:, 3] = np.minimum(255, a[:, 3].astype(np.int32) * 255 // 128).astype(np.uint8)
        return a
    if kind == 2:
        a = np.frombuffer(raw[:n*3], np.uint8).reshape(-1, 3)
        return np.concatenate([a, np.full((len(a), 1), 255, np.uint8)], 1)
    v = np.frombuffer(raw[:n*2], '<u2').astype(np.uint32)
    r = (v & 31) * 255 // 31; g = ((v >> 5) & 31) * 255 // 31; b = ((v >> 10) & 31) * 255 // 31
    al = np.where(v & 0x8000, 255, 0)
    return np.stack([r, g, b, al], 1).astype(np.uint8)

def unswizzle_clut(p):
    p = p.copy()
    for base in range(0, 256, 32):
        a = p[base+8:base+16].copy(); b = p[base+16:base+24].copy()
        p[base+8:base+16] = b; p[base+16:base+24] = a
    return p

def unswizzle8(idx, w, h):
    """PSMT8 data stored as PSMCT32 GS-memory layout -> linear rows (standard PS2 8-bit unswizzle)."""
    y, x = np.mgrid[0:h, 0:w]
    block = (y & ~0xf) * w + (x & ~0xf) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    posy = (((y & ~3) >> 1) + (y & 1)) & 7
    col = posy * w * 2 + ((x + swap) & 7) * 4
    bn = ((y >> 1) & 1) + ((x >> 2) & 2)
    return idx[(block + col + bn)].reshape(-1)

def roughness(px):
    """mean abs neighbour difference over opaque pixels (low = natural image, about 85 = white noise)"""
    a = px[..., :3].astype(np.int16); m = px[..., 3] > 0
    v = np.abs(np.diff(a, axis=1)).mean(2)[m[:, 1:] & m[:, :-1]]
    return float(v.mean()) if len(v) else 0.0

def prefer_linear(pl, px):
    """True if the linear decode `pl` looks like the real image and the GS-unswizzled one `px` does not."""
    d = flatness(pl) - flatness(px)
    return d > 0.10 or (d > 0.05 and roughness(pl) < 0.85 * roughness(px))

def is_stub(b, ip, base_sz):
    """True if the 'pixel data' of a picture is really something else: header-only stub (see module doc).
    Returns a reason string or None."""
    d = b[ip:ip+64]
    if d[:4] == b'TIM2': return 'stub_next_tim2'          # next TIM2 header begins right after the picture header
    if d[:2] == bytes((13, 10)) or b'TEX_ANIME' in d[:64]: return 'stub_script'   # CRLF / TEX_ANIME text script follows
    reg = b[ip:ip+base_sz]
    j = reg.find(b'TIM2', 4)
    while j >= 0:                                        # a *valid* TIM2 header inside the pixel area = overlap
        if parse_at(b, ip + j) is not None: return 'stub_overlaps_tim2'
        j = reg.find(b'TIM2', j + 4)
    return None

def flatness(px):
    """fraction of horizontal neighbour pairs with identical RGBA. A correctly un-swizzled picture has many flat runs;
    the wrong swizzle turns them into a checker/dither pattern (idx8 only; linear-vs-swizzled test)."""
    return float((px[:, 1:] == px[:, :-1]).all(2).mean())

def parse_at(b, off, strict=True):
    """Returns list of dict per picture or None. strict=False (im3_export.py) tolerates the Level-5 'total size' field
    that some tools wrote as if the picture were 32bpp (frame.img, event/item/*.img): sizes come from imsz/clsz."""
    if b[off:off+4] != b'TIM2' or off + 16 > len(b): return None
    ver, fmt, npic = struct.unpack_from('<BBH', b, off+4)
    if npic == 0 or npic > 64 or ver not in (3, 4): return None
    pos = off + (16 if fmt == 0 else 128)
    out = []
    for _ in range(npic):
        if pos + 48 > len(b): return None
        tot, clsz, imsz, hsz, ccol, pfmt, mips, ctype, itype, w, h = struct.unpack_from('<IIIHHBBBBHH', b, pos)
        if hsz < 48 or hsz > 256 or w == 0 or h == 0 or w > 2048 or h > 2048 or itype not in (1,2,3,4,5): return None
        if not strict:
            if pos + hsz + imsz + clsz > len(b) + 16: return None
            tot = hsz + imsz + clsz
        elif tot < hsz + imsz + clsz or pos + tot > len(b) + 16: return None
        bits = {1:16, 2:24, 3:32, 4:4, 5:8}[itype]
        if imsz < w*h*bits//8: return None
        ip = pos + hsz
        # (mip levels follow the base level; only the base is decoded)
        base_sz = w*h*bits//8
        img = b[ip:ip+base_sz]
        clut = b[ip+imsz:ip+imsz+clsz] if clsz else b''
        tex0 = struct.unpack_from('<Q', b, pos+24)[0]
        out.append(dict(ip=ip, base_sz=base_sz, psm=(tex0>>20)&63, off=pos, total=tot, w=w, h=h, itype=itype, ctype=ctype, ccol=ccol, mips=mips, img=img, clut=clut, size=tot))
        pos += tot
    return out, pos - off

def decode(p, legacy=False):
    w, h, it = p['w'], p['h'], p['itype']
    n = w*h
    if it in (1,2,3):
        px = bpp_conv(p['img'], it, n)
        return px.reshape(h, w, 4), 'direct%d' % {1:16,2:24,3:32}[it]
    idx = np.frombuffer(p['img'], np.uint8)
    if it == 4:
        idx = np.stack([idx & 15, idx >> 4], 1).reshape(-1)
    idx = idx[:n]
    lin = idx
    if it == 5:
        idx = unswizzle8(idx, w, h)
    ck = p['ctype'] & 0x3f
    if ck not in (1, 2, 3) or not p['clut']: return None
    csz = {1:2, 2:3, 3:4}[ck]
    ncol = min(len(p['clut']) // csz, 256)
    pal = bpp_conv(p['clut'], ck, ncol)
    if it == 5 and ncol >= 256 and not (p['ctype'] & 0x80):
        pal = unswizzle_clut(pal)
    if len(pal) < 256:
        pal = np.concatenate([pal, np.zeros((256-len(pal), 4), np.uint8)])
    px = pal[idx].reshape(h, w, 4)
    if it == 5 and not legacy and w >= 8 and h >= 8:
        # some 8-bit pictures are stored linear, not GS-swizzled (e.g. menu\rc\transport_menu_rc_en.pac)
        pl = pal[lin].reshape(h, w, 4)
        if prefer_linear(pl, px):     # ~70 of 7,041 idx8 in the 2026-10-27 export
            return pl, 'idx8lin'
    return px, ('idx4' if it == 4 else 'idx8')

def main():
    src, out = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[sys.argv.index('--limit')+1]) if '--limit' in sys.argv else None
    nopng = '--no-png' in sys.argv
    legacy = '--legacy' in sys.argv
    only = None
    if '--rows' in sys.argv:
        only = set((r['file'], int(r['offset'])) for r in csv.DictReader(open(sys.argv[sys.argv.index('--rows')+1], encoding='utf-8')))
        onlyfiles = set(f for f, _ in only)
    stubs = []
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    seen = {}; rows = []; total = 0; nstub = 0; t0 = time.time(); fail = 0
    for dp, _, fs in os.walk(src):
        for f in sorted(fs):
            if f.lower().endswith('.mwv') or f.lower().endswith('.snd'): continue
            path = os.path.join(dp, f)
            if only is not None and os.path.relpath(path, src) not in onlyfiles: continue
            b = open(path, 'rb').read()
            i = b.find(b'TIM2')
            while i >= 0:
                r = parse_at(b, i)
                if r:
                    pics, span = r
                    for k, p in enumerate(pics):
                        rel = os.path.relpath(path, src)
                        if only is not None and (rel, p['off']) not in only: continue
                        why = None if legacy else is_stub(b, p['ip'], p['base_sz'])
                        if why:
                            total += 1; nstub += 1
                            stubs.append([rel, p['off'], p['w'], p['h'], p['itype'], p['size'], why]); continue
                        try: d = decode(p, legacy)
                        except Exception: d = None
                        total += 1
                        if d is None: fail += 1; continue
                        px, kind = d
                        hsh = hashlib.sha1(px.tobytes() + struct.pack('<HH', p['w'], p['h'])).hexdigest()[:16]
                        alpha = int(((px[..., 3] > 0) & (px[..., 3] < 255)).any())
                        trans = int((px[..., 3] == 0).any())
                        if hsh in seen: seen[hsh] += 1; continue
                        seen[hsh] = 1
                        if not nopng:
                            Image.fromarray(px, 'RGBA').save(os.path.join(out, 'png', hsh + '.png'))
                        rows.append([hsh, p['psm'], os.path.relpath(path, src), i + (p['off'] - i), p['size'], p['w'], p['h'], kind, p['ccol'], p['mips'], alpha, trans, hex(p['ctype'])])
                    i = b.find(b'TIM2', i + 4)
                else:
                    i = b.find(b'TIM2', i + 4)
            if limit and len(rows) >= limit: break
        if limit and len(rows) >= limit: break
    with open(os.path.join(out, 'manifest.csv'), 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh); w.writerow(['hash','psm','file','offset','size','w','h','kind','clut_colors','mips','semi_alpha','has_zero_alpha','clut_type'])
        for r in rows: w.writerow(r)
    with open(os.path.join(out, 'stubs.csv'), 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh); w.writerow(['file','offset','w','h','itype','declared_size','reason']); w.writerows(stubs)
    print('stubs', nstub)
    print('total pictures', total, 'decode-fail', fail, 'unique', len(rows), 'secs', round(time.time()-t0))
if __name__ == "__main__":
    main()
