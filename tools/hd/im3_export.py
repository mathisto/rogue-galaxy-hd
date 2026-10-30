"""IM3 / TIM2 UI texture exporter for Rogue Galaxy (PS2, USA).  Reads the game archive directly (read-only), so it is
immune to the 512-byte extraction offset bug (see research/11-menu-art-and-numerals.md: HD6 offsets whose value is
== 512 mod 2048 are really 2048-aligned; extract.py started those files 512 bytes late, cutting the IM3/TIM2 headers).

usage (python -I -X utf8):
  im3_export.py <game dir> <out dir> [--scope ui|all] [--survey] [--no-png] [--only SUBSTR] [--prev earlier-manifest.csv]
Writes <out>/png/<hash>.png (RGBA, same hash scheme as tim2_export.py: sha1(rgba + w + h)[:16]), <out>/cfg/*.txt (texture
animation scripts and other text entries), <out>/manifest.csv (one row per texture occurrence) and <out>/unique.csv.
Each row also carries gs_key = the renderer's replacement-pack key <texels>_<clut>_<W>x<H>_psm<NN> computed the way
GsTextureCache does (FNV-1a), so a pack can be named without running the game; verify with RG_TEXTURE_LOG=1.
Reuses tim2_export.parse_at/decode (TIM2) and hd6.parse (index).
"""
import os, sys, re, csv, struct, hashlib, time, argparse
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hd6
import tim2_export as T

STUBS = []   # header-only stub pictures dropped by emit() (tim2_export.is_stub)
M64 = (1 << 64) - 1
PRIME = 1099511628211
SEED = 1469598103934665603


def fnv_step(h, v):
    for i in range(8):
        h = ((h ^ ((v >> (8 * i)) & 0xff)) * PRIME) & M64
    return h


def fnv_bytes(h, data):
    n = len(data) & ~7
    if n:
        for (w,) in struct.iter_unpack('<Q', data[:n]):
            h = ((h ^ w) * PRIME) & M64
    for c in data[n:]:
        h = ((h ^ c) * PRIME) & M64
    return h


def gs_identity(idx_rows, kind, w, h, pal32, psm_override=None):
    """idx_rows: (h,w) uint8 logical indices (idx8/idx4) or (h,w,4) RGBA raw (direct32).
    pal32: list of 256 (or 16) raw palette words (r | g<<8 | b<<16 | a<<24, alpha in GS 0..0x80 units).
    Returns (texels_hash, clut_hash, psm)."""
    psm = psm_override or {'idx8': 19, 'idx4': 20, 'direct32': 0, 'direct24': 1, 'direct16': 2}[kind]
    t = fnv_step(fnv_step(fnv_step(SEED, w), h), psm)
    if kind == 'idx8':
        for y in range(h):
            t = fnv_bytes(t, idx_rows[y].tobytes())
    elif kind == 'idx4':
        for y in range(h):
            r = idx_rows[y]
            packed = (r[0::2] | (np.pad(r, (0, w & 1))[1::2][:len(r[0::2])] << 4)).astype(np.uint8)
            t = fnv_bytes(t, packed.tobytes())
    else:
        for y in range(h):
            t = fnv_bytes(t, idx_rows[y].tobytes())
    c = 0
    if pal32 is not None:
        c = fnv_step(fnv_step(SEED, 0), 0)         # cpsm=CT32 (0), csm=CSM1 (0)
        for v in pal32:
            c = fnv_step(c, v)
    return t, c, psm


BLOCK32 = np.array([[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29],
                    [10, 11, 14, 15, 26, 27, 30, 31]])
COL32 = np.array([[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29],
                  [18, 19, 22, 23, 26, 27, 30, 31], [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
                  [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]])


def unswizzle_ct32(words, w, h):
    """PSMCT32 GS block layout (as uploaded raw) -> linear (h,w) uint32.  words: flat uint32, w multiple of 64, h of 32."""
    y, x = np.mgrid[0:h, 0:w]
    page = (y >> 5) * (w >> 6) + (x >> 6)
    blk = BLOCK32[(y >> 3) & 3, (x >> 3) & 7]
    col = COL32[y & 7, x & 7]
    return words[page * 2048 + blk * 64 + col].reshape(h, w)


def roughness(px):
    p = px[..., :3].astype(np.int16)
    return float(np.abs(np.diff(p, axis=1)).mean() + np.abs(np.diff(p, axis=0)).mean())


def raw_decode(p, flag=-1):
    """Like tim2_export.decode but flag-aware (IM3 entry flag w28: 0 = pixel rows linear, 1 = GS-swizzled data, see
    Tim2RegisterPicture 0x159940 which only swizzles 8bpp when the flag is set) and returns logical indices and the
    raw palette words for gs_identity.  flag -1 (loose TIM2): pick the smoother of the two layouts.
    Returns (rgba, kind, raw, pal32, layout)."""
    w, h, it = p['w'], p['h'], p['itype']
    n = w * h
    if it in (1, 2, 3):
        px = T.bpp_conv(p['img'], it, n).reshape(h, w, 4)
        kind = 'direct%d' % {1: 16, 2: 24, 3: 32}[it]
        raw = np.frombuffer(p['img'][:n * 4], np.uint8).reshape(h, w, 4) if it == 3 else None
        layout = 'linear'
        if it == 3 and flag != 0 and w % 64 == 0 and h % 32 == 0:
            words = np.frombuffer(p['img'][:n * 4], '<u4')
            lin = unswizzle_ct32(words, w, h)
            raw2 = lin.view(np.uint8).reshape(h, w, 4)
            px2 = T.bpp_conv(raw2.tobytes(), 3, n).reshape(h, w, 4)
            if roughness(px2) < roughness(px) * 0.8:
                px, raw, layout = px2, raw2, 'ct32-swizzled'
        return px, kind, raw, None, layout
    idx = np.frombuffer(p['img'], np.uint8)
    if it == 4:
        idx = np.stack([idx & 15, idx >> 4], 1).reshape(-1)
    idx = idx[:n]
    ck = p['ctype'] & 0x3f
    if ck not in (1, 2, 3) or not p['clut']:
        return None
    csz = {1: 2, 2: 3, 3: 4}[ck]
    ncol = min(len(p['clut']) // csz, 256)
    rawpal = np.frombuffer(p['clut'][:ncol * 4], np.uint8).reshape(-1, 4).copy() if ck == 3 else None
    pal = T.bpp_conv(p['clut'], ck, ncol)
    if it == 5 and ncol >= 256 and not (p['ctype'] & 0x80):
        pal = T.unswizzle_clut(pal)
        if rawpal is not None:
            rawpal = T.unswizzle_clut(rawpal)
    if len(pal) < 256:
        pal = np.concatenate([pal, np.zeros((256 - len(pal), 4), np.uint8)])
        if rawpal is not None:
            rawpal = np.concatenate([rawpal, np.zeros((256 - len(rawpal), 4), np.uint8)])
    layout = 'linear'
    if it == 5:
        swz = (flag == 1)
        if flag not in (0, 1):
            swz = roughness(pal[T.unswizzle8(idx, w, h).reshape(h, w)]) < roughness(pal[idx.reshape(h, w)])
        if swz:
            idx = T.unswizzle8(idx, w, h); layout = 'psmt8-swizzled'
    px = pal[idx].reshape(h, w, 4)
    pal32 = None
    if rawpal is not None:
        k = 16 if it == 4 else 256
        pal32 = [int(a) | int(b) << 8 | int(c) << 16 | int(d) << 24 for a, b, c, d in rawpal[:k]]
    return px, ('idx4' if it == 4 else 'idx8'), idx.reshape(h, w), pal32, layout


# ---------------------------------------------------------------- archive access (corrected offsets)
def archive_files(game, n):
    base = os.path.join(game, f'DATA0_{n}')
    strs, ents = hd6.parse(base + '.HD6')
    dat = open(base + '.DAT', 'rb')
    size = os.path.getsize(base + '.DAT')
    for name, off, sz in ents:
        if not name or sz == 0:
            continue
        true_off = off & ~2047            # the low 2 bits of the 512-unit field are not address bits
        yield name.replace('\\', '/').lstrip('/'), true_off, sz, off, dat


def read_file(dat, off, sz):
    dat.seek(off)
    return dat.read(sz)


def ui_category(p):
    q = p.lower()
    if q.startswith('menu/'):
        if '/help_board/' in q: return 'menu/help_board'
        if '/fac_help/' in q: return 'menu/fac_help'
        if os.path.basename(q).startswith('item_icon'): return 'menu/item_icon'
        if os.path.basename(q).startswith('icon_all'): return 'menu/icon_all'
        if q.startswith('menu/rc/'): return 'menu/rc'
        return 'menu/other'
    if q.startswith('meswin/'): return 'font'
    if q.startswith('tgsrc/'): return 'title_loading'
    if q.startswith('rc/'): return 'rc'
    if q.startswith('event/area_master_name/') or q.startswith('event/boss_name/') or q.startswith('event/extreme_dunjon/'):
        return 'banner'
    if q.startswith('event/item/'): return 'item_picture'
    if q.startswith('map/warp/'): return 'warp_map'
    if q.startswith('map/caption/'): return 'map_caption'
    if q.endswith('minimap.img'): return 'minimap'
    if q in ('chara/frame.img', 'chara/dc3title.img', 'effect/condition.img', 'effect/dbfont.img',
             'effect/battle_effect00.img', 'font16.img'):
        return 'hud'
    return None


def find_im3(b):
    """Yield (offset, [entries]) for every valid IM3 header in b; entry = dict(name, a, size, flag, grp, h30, d38)."""
    for m in re.finditer(rb'IM3\x00', b):
        o = m.start()
        if o + 16 > len(b):
            continue
        h, n, z = struct.unpack_from('<III', b, o + 4)
        if h != 0x10 or n == 0 or n > 512 or z != 0:
            continue
        ents = []
        ok = True
        for i in range(n):
            e = o + 0x10 + i * 0x40
            if e + 0x40 > len(b): ok = False; break
            name = b[e:e + 0x20].split(b'\0')[0].decode('latin1')
            w20, w24, w28, w2c, h30, w34, d38 = struct.unpack_from('<IIIIIIQ', b, e + 0x20)
            if w20 != 0x40 or o + w24 > len(b) + 16: ok = False; break
            ents.append(dict(name=name, a=o + w24, size=w34, flag=w28, grp=w2c, h30=h30, d38=d38))
        if ok:
            yield o, ents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game'); ap.add_argument('out')
    ap.add_argument('--scope', default='ui'); ap.add_argument('--survey', action='store_true')
    ap.add_argument('--no-png', action='store_true'); ap.add_argument('--only', default='')
    ap.add_argument('--prev', default='', help='an earlier manifest.csv whose hashes are marked in_prev (optional)')
    ap.add_argument('--no-key', action='store_true')
    ap.add_argument('--no-stub-filter', action='store_true', help='keep header-only stub pictures (research/12); default drops them to <out>/stubs.csv')
    a = ap.parse_args()
    prev = set()
    if a.prev and os.path.exists(a.prev):
        prev = {r['hash'] for r in csv.DictReader(open(a.prev, encoding='utf-8'))}
    os.makedirs(os.path.join(a.out, 'png'), exist_ok=True)
    os.makedirs(os.path.join(a.out, 'cfg'), exist_ok=True)
    rows = []; seen = {}; t0 = time.time(); fails = []; cfgs = 0
    survey = {}
    for n in (0, 1):
        for path, off, sz, off_raw, dat in archive_files(a.game, n):
            low = path.lower()
            if low.endswith(('.mwv', '.snd', '.stb', '.map', '.mc', '.sky')): continue
            cat = ui_category(path)
            if a.scope == 'ui' and not cat and not a.survey: continue
            if a.only and a.only not in low: continue
            b = read_file(dat, off, sz)
            tag = 'DATA0_%d/%s' % (n, path)
            covered = []
            for o, ents in find_im3(b):
                for e in ents:
                    d = b[e['a']:e['a'] + 4]
                    covered.append((e['a'], e['a'] + e['size']))
                    if a.survey:
                        k = (os.path.dirname(path), 'IM3:' + ('tim2' if d == b'TIM2' else 'other'))
                        survey[k] = survey.get(k, 0) + 1
                    if cat is None and a.scope == 'ui': continue
                    if d != b'TIM2':
                        txt = b[e['a']:e['a'] + e['size']]
                        cfgs += 1
                        fn = '%s__%s.bin' % (tag.replace('/', '_'), re.sub(r'\W', '_', e['name']))
                        open(os.path.join(a.out, 'cfg', fn), 'wb').write(txt)
                        rows.append(dict(hash='', cat=cat or '', file=tag, im3_off=o, off=e['a'], name=e['name'], w=0, h=0,
                                         kind='cfg:' + ('text' if txt[:1].isascii() and b'\0' not in txt[:64] else 'bin'),
                                         size=e['size'], psm='', colors='', semi_alpha='', has_zero_alpha='', gs_key='', in_prev='', note='non-TIM2 IM3 entry'))
                        continue
                    r = T.parse_at(b, e['a'], strict=False)
                    if not r:
                        fails.append((tag, e['name'], 'parse')); continue
                    for k, pic in enumerate(r[0]):
                        emit(a, rows, seen, prev, fails, tag, cat, o, e['a'], e['name'] + ('' if k == 0 else '#%d' % k), pic, e['flag'], b)
            if a.survey:
                loose = 0
            # loose TIM2 (not inside an IM3 entry)
            i = b.find(b'TIM2')
            while i >= 0:
                if not any(s <= i < t for s, t in covered):
                    r = T.parse_at(b, i, strict=False)
                    if r:
                        for k, pic in enumerate(r[0]):
                            if a.survey:
                                k2 = (os.path.dirname(path), 'loose-TIM2'); survey[k2] = survey.get(k2, 0) + 1
                            elif cat or a.scope == 'all':
                                emit(a, rows, seen, prev, fails, tag, cat, -1, i, '' if k == 0 else '#%d' % k, pic, -1, b)
                i = b.find(b'TIM2', i + 4)
    if a.survey:
        for k, v in sorted(survey.items()): print(k, v)
        return
    with open(os.path.join(a.out, 'manifest.csv'), 'w', newline='', encoding='utf-8') as f:
        cols = ['hash', 'cat', 'file', 'im3_off', 'off', 'name', 'w', 'h', 'kind', 'size', 'psm', 'colors', 'semi_alpha',
                'has_zero_alpha', 'gs_key', 'gs_key_t4hl', 'in_prev', 'flag', 'layout', 'dups', 'note']
        w = csv.DictWriter(f, cols, extrasaction='ignore'); w.writeheader()
        for r in rows:
            r['dups'] = seen.get(r['hash'], 1) if r['hash'] else ''
            w.writerow(r)
    uniq = [r for r in rows if r['hash'] and r.get('first')]
    with open(os.path.join(a.out, 'unique.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, cols, extrasaction='ignore'); w.writeheader()
        for r in uniq: w.writerow(r)
    with open(os.path.join(a.out, 'stubs.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f); w.writerow(['file', 'offset', 'w', 'h', 'itype', 'reason']); w.writerows(STUBS)
    print('stubs', len(STUBS), end=' ')
    print('rows', len(rows), 'unique textures', len(uniq), 'cfg entries', cfgs, 'fails', len(fails), 'secs', round(time.time() - t0))
    for fl in fails[:20]: print('FAIL', fl)
    with open(os.path.join(a.out, 'fails.txt'), 'w', encoding='utf-8') as f:
        for fl in fails: f.write('\t'.join(map(str, fl)) + '\n')


def emit(a, rows, seen, prev, fails, tag, cat, im3off, off, name, pic, flag, b=None):
    if b is not None and not a.no_stub_filter:
        why = T.is_stub(b, pic['ip'], pic['base_sz'])
        if why:
            STUBS.append((tag, off, pic['w'], pic['h'], pic['itype'], why)); return
    try:
        d = raw_decode(pic, flag)
    except Exception as ex:
        fails.append((tag, name, 'decode ' + str(ex))); return
    if d is None:
        fails.append((tag, name, 'no clut')); return
    px, kind, raw, pal32, layout = d
    hsh = hashlib.sha1(px.tobytes() + struct.pack('<HH', pic['w'], pic['h'])).hexdigest()[:16]
    first = hsh not in seen
    seen[hsh] = seen.get(hsh, 0) + 1
    key = ''; key2 = ''
    if first and not a.no_png:
        Image.fromarray(px, 'RGBA').save(os.path.join(a.out, 'png', hsh + '.png'))
    if first and not a.no_key and raw is not None:
        try:
            t, c, psm = gs_identity(raw, kind, pic['w'], pic['h'], pal32)
            key = '%016x_%016x_%dx%d_psm%02d' % (t, c, pic['w'], pic['h'], psm)
            if kind == 'idx4':      # the game keeps some 4-bit art as T4HL in a 32-bit buffer (the font pages do): psm 36 key
                t2, c2, p2 = gs_identity(raw, kind, pic['w'], pic['h'], pal32, 36)
                key2 = '%016x_%016x_%dx%d_psm%02d' % (t2, c2, pic['w'], pic['h'], p2)
        except Exception:
            key = ''
    alpha = int(((px[..., 3] > 0) & (px[..., 3] < 255)).any()); trans = int((px[..., 3] == 0).any())
    rows.append(dict(hash=hsh, cat=cat or '', file=tag, im3_off=im3off, off=off, name=name, w=pic['w'], h=pic['h'], kind=kind,
                     size=pic['size'], psm=pic['psm'], colors=pic['ccol'], semi_alpha=alpha, has_zero_alpha=trans, gs_key=key, gs_key_t4hl=key2,
                     in_prev=int(hsh in prev), first=first, note='', flag=flag, layout=layout))


if __name__ == '__main__':
    main()
