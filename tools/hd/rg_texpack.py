"""Builds the Rogue Galaxy recomp's HD texture pack from the AI-upscaled set, named by the renderer's own keys.

usage (build_hd_pack.py runs the build for you):
  python -I -X utf8 rg_texpack.py build  --keys KEYS.csv --out PACKDIR --upscaled UPSCALED_ROOT [--no-flagged] [--copy]
                                           [--stubs stubs.csv] [--actions old-set-actions.csv] [--clean] [--wildcard-short]
                                           [--extra MANIFEST.csv=UPSCALED_ROOT[|PRIMARY|FALLBACK]]
  python -I -X utf8 rg_texpack.py validate --keys KEYS.csv --pack PACKDIR LOG [LOG...]

KEYS.csv comes from rg_texkeys.py (one row per TIM2 picture and layout, with the renderer's texel and palette hashes
and the dedupe pixel hash that names the upscaled PNGs). PACKDIR is the game's
textures\\SCUS-97490\\replacements (or any folder given to the game as RG_TEXTURE_PACK); the pack is user-derived
content and must never be committed (the repository ignores /textures/).

build:
  * one file per renderer key, <texels>_<clut>_<W>x<H>_psm<NN>.png, in a sub-folder per disc folder (map, chara...);
  * a picture's upscale: <upscaled>\\x4plus-anime-fixed\\<pixhash>.png if the decoder fix made one, else
    x4plus-anime\\<pixhash>.png. By default (since 2026-10-28) the recommended set of flagged-classes.csv wins:
    animevideov3(-fixed) for foliage cut-outs, effects (additive glows, sparks) and smooth clouds, x4plus (the general
    model) for the hand-picked stone_floor tiles (paving, cobbles, brick). --no-flagged builds the all-x4plus-anime
    pack of before (--fallback-for-flagged is accepted and does nothing);
  * --actions (an old-set-actions.csv from the 2026-10-28 decoder fix; not needed for a build made from scratch
    with the fixed exporters) is applied first: old upscales of header-only stubs, of mis-decoded linear idx8
    pictures and of still-undecodable ones are never used; --stubs (tim2_export.py stubs.csv) rows are skipped;
  * files are hard links to the upscales (same volume; no space used) unless --copy;
  * idx8 pictures with fewer than 256 palette entries cannot have their palette hash computed (the rest of the
    CLUT is whatever video memory holds); with --wildcard-short they get the any-palette name (16 zeros) when
    their indices belong to that one picture only;
  * atlases (--native NAME, default item_icon: the icon sheet a TEX_ANIME slot animates and work_icon copies from)
    get the disc's own picture beside the upscale as native_<key>.png;
  * PACKDIR\\..\\pack-manifest.csv lists every file with its source, set and pixel hash; --clean first removes the
    files a previous manifest lists (nothing else).

validate: reads game logs made with RG_TEXTURE_LOG=1 and reports how many texture identities the game built
(whole textures, "[texrep] texture ... built|now <texels>_<clut>") are in the pack, how many part lookups hit, and
which misses look like render targets (texels 0) or palette-only differences.
"""
import argparse
import csv
import os
import re
import shutil
import sys



def read_csv(path):
    with open(path, newline='', encoding='utf-8') as fh:
        return list(csv.DictReader(fh))


def key_name(r):
    return '%s_%s_%sx%s_psm%02x.png' % (r['texels'], r['clut'] or '0' * 16, r['w'], r['h'], int(r['psm']))


def build(args):
    up = args.upscaled
    rows = read_csv(args.keys)
    drop_old = set()
    actions = read_csv(args.actions) if args.actions and os.path.exists(args.actions) else []
    for a in actions:
        if a['action'] != 'keep_unchanged':
            drop_old.add(a['old_hash'])
    stubs = set()
    if args.stubs and os.path.exists(args.stubs):
        for s in read_csv(args.stubs):
            stubs.add((s['file'], int(s['offset'])))
    flagged = {}
    fpath = os.path.join(up, 'flagged-classes.csv')
    if os.path.exists(fpath):
        for f in read_csv(fpath):
            flagged[f['hash']] = (f['classes'], f['recommended_set'])

    def listing(name):
        d = os.path.join(up, name)
        return set(os.path.splitext(n)[0] for n in os.listdir(d) if n.endswith('.png')) if os.path.isdir(d) else set()

    sets = {n: listing(n) for n in ('x4plus-anime', 'x4plus-anime-fixed', 'x4plus-anime-extra', 'animevideov3',
                                    'animevideov3-fixed', 'animevideov3-extra', 'x4plus')}
    for n in ('x4plus-anime', 'animevideov3', 'x4plus'):
        sets[n] -= drop_old
    use_flagged = not args.no_flagged

    def source(pix):
        """-> (set name, path) or None"""
        order = ['x4plus-anime-fixed', 'x4plus-anime-extra', 'x4plus-anime']
        cls = flagged.get(pix)
        if cls and cls[1] == 'review-decode-first':
            return None
        if use_flagged and cls and cls[1] == 'animevideov3':
            order = ['animevideov3-fixed', 'animevideov3-extra', 'animevideov3'] + order
        elif use_flagged and cls and cls[1] == 'x4plus' and pix not in sets['x4plus-anime-fixed']:
            # stone_floor: the general model, made from the same native picture (a decoder-fixed picture keeps its
            # fixed upscale instead).
            order = ['x4plus'] + order
        for n in order:
            if pix in sets[n]:
                return n, os.path.join(up, n, pix + '.png')
        return None

    # Indices that belong to one picture only, for --wildcard-short.
    tex_pix = {}
    for r in rows:
        if r['texels']:
            tex_pix.setdefault((r['texels'], r['w'], r['h'], r['psm']), set()).add(r['pixhash'])

    planned = {}   # name -> (row, set, path)
    conflicts = 0
    counts = dict(rows=len(rows), no_hash=0, stub=0, short_clut=0, no_upscale=0, keyed=0)
    for r in rows:
        if (r['file'], int(r['offset'])) in stubs:
            counts['stub'] += 1
            continue
        if not r['texels'] or int(r['w']) > 1024 or int(r['h']) > 1024:
            # (the GS addresses at most 1024x1024: a taller picture is only ever drawn in parts)
            counts['no_hash'] += 1
            continue
        if r['clut'] == 'short':
            if not args.wildcard_short or len(tex_pix[(r['texels'], r['w'], r['h'], r['psm'])]) != 1:
                counts['short_clut'] += 1
                continue
            r = dict(r, clut='0' * 16)
        src = source(r['pixhash'])
        if not src:
            counts['no_upscale'] += 1
            continue
        name = key_name(r)
        if name in planned:
            if planned[name][0]['pixhash'] != r['pixhash']:
                conflicts += 1
            continue
        planned[name] = (r, src[0], src[1])
    # Extra sets (the IM3 menu art and the pictures found inside IM3 containers: im3_export.py unique.csv + upscales):
    # rows with a gs_key (and gs_key_t4hl) name and a pixel hash under <root>\x4plus-anime.
    extra_count = 0
    natives_for = set(args.native or ['item_icon'])
    for spec in args.extra or []:
        manifest, root = spec.split('=', 1)
        primary, fallback = 'x4plus-anime', 'animevideov3'
        if '|' in root:
            root, primary, fallback = root.split('|')
        for r in read_csv(manifest):
            names = [k for k in (r.get('gs_key'), r.get('gs_key_t4hl')) if k]
            order = [primary]
            cls = flagged.get(r['hash'])
            top = r['file'].replace('/', '\\').split('\\')
            effect = len(top) > 2 and top[1].lower() == 'effect'   # flag_classes' effect_additive rule
            if use_flagged and (r.get('flag') == 'animevideov3' or effect or (cls and cls[1] == 'animevideov3')):
                order.insert(0, fallback)
            path = next((os.path.join(root, n, r['hash'] + '.png') for n in order
                         if os.path.exists(os.path.join(root, n, r['hash'] + '.png'))), None)
            if not path:
                continue
            for k in names:
                # The other manifests write the PSM in decimal (psm19 = T8, psm20 = T4, psm36 = T4HL); the
                # renderer's names are hex (psm13, psm14, psm24).
                head, psm = k.lower().rsplit('_psm', 1)
                k = '%s_psm%02x' % (head, int(psm, 10) if args.extra_psm_decimal else int(psm, 16))
                name = k + '.png'
                if name in planned:
                    continue
                parts = k.split('_')
                w, h = parts[2].split('x')
                if int(w) > 1024 or int(h) > 1024:
                    continue
                row = dict(file=r['file'].replace('/', os.sep), offset=r.get('off', ''), w=w, h=h,
                           kind=r.get('kind', ''), layout=r.get('layout', ''), pixhash=r['hash'])
                native = os.path.join(os.path.dirname(manifest), 'png', r['hash'] + '.png')
                if r.get('name') in natives_for and os.path.exists(native):
                    row['native'] = native
                planned[name] = (row, 'extra:' + os.path.basename(os.path.dirname(os.path.dirname(path))) + '/' +
                                 os.path.basename(os.path.dirname(path)), path)
                extra_count += 1
    counts['keyed'] = len(planned)
    print('from other sets:', extra_count)

    out = args.out
    manifest_path = os.path.join(os.path.dirname(os.path.abspath(out)), 'pack-manifest.csv')
    if args.clean and os.path.exists(manifest_path):
        removed = 0
        for m in read_csv(manifest_path):
            p = os.path.join(out, m['path'])
            if os.path.isfile(p):
                os.remove(p)
                removed += 1
        print('removed', removed, 'files of the previous pack')
    os.makedirs(out, exist_ok=True)
    linked = copied = kept = natives = 0
    by_set = {}
    with open(manifest_path, 'w', newline='', encoding='utf-8') as fh:
        wr = csv.writer(fh)
        wr.writerow(['path', 'set', 'pixhash', 'file', 'offset', 'w', 'h', 'kind', 'layout'])
        for name, (r, setname, path) in sorted(planned.items()):
            top = r['file'].replace('/', '\\').split('\\')
            sub = top[1] if len(top) > 2 else 'misc'
            rel = os.path.join(sub, name)
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst):
                if os.path.samefile(dst, path):
                    kept += 1
                else:
                    os.remove(dst)
            if not os.path.exists(dst):
                if args.copy:
                    shutil.copyfile(path, dst)
                    copied += 1
                else:
                    try:
                        os.link(path, dst)
                        linked += 1
                    except OSError:
                        shutil.copyfile(path, dst)
                        copied += 1
            by_set[setname] = by_set.get(setname, 0) + 1
            wr.writerow([rel, setname, r['pixhash'], r['file'], r['offset'], r['w'], r['h'], r['kind'], r['layout']])
            if r.get('native'):
                # The atlas's own picture beside its upscale: the renderer composes the HD picture of a texture the
                # game changes in place (TEX_ANIME slots) or fills with its icons (work_icon) from the two.
                nrel = os.path.join(sub, 'native_' + name)
                ndst = os.path.join(out, nrel)
                if not (os.path.exists(ndst) and os.path.samefile(ndst, r['native'])):
                    if os.path.exists(ndst):
                        os.remove(ndst)
                    try:
                        os.link(r['native'], ndst)
                    except OSError:
                        shutil.copyfile(r['native'], ndst)
                    natives += 1
                wr.writerow([nrel, 'native', r['pixhash'], r['file'], r['offset'], r['w'], r['h'], r['kind'],
                             r['layout']])
    print('pictures', counts['rows'], '| pack files', counts['keyed'], '| by set', by_set)
    print('skipped: stubs', counts['stub'], '| no hash', counts['no_hash'], '| short palette', counts['short_clut'],
          '| no upscale', counts['no_upscale'], '| name conflicts', conflicts)
    print('linked', linked, 'copied', copied, 'already there', kept, '| atlas native pictures added', natives, '->', out)
    print('manifest', manifest_path)


LINE = re.compile(r'\[texrep\] texture tbp0=(\w+) tbw=\d+ psm=(\w+) (\d+)x(\d+) (built|now) (\w{16})_(\w{16})')
LOOKUP = re.compile(r'\[texrep\] lookup tbp0=(\w+) psm=(\w+) (\d+)x(\d+) part (\d+)x(\d+): (\w{16})_(\w{16})')
BOUND = re.compile(r'\[texrep\] tbp0=(\w+) psm=(\w+) (\d+)x(\d+): (\S+) at (\d+)x over (\d+)x(\d+)')


def validate(args):
    rows = read_csv(args.keys)
    offline_tex = {}
    for r in rows:
        if r['texels']:
            offline_tex.setdefault(r['texels'], []).append(r)
    pack = set()
    for dp, _, fs in os.walk(args.pack):
        for f in fs:
            if f.endswith('.png'):
                pack.add(os.path.splitext(f)[0].lower())
    pack_tex = {p.split('_')[0] for p in pack}
    ids, lookups, bound = {}, set(), set()
    for log in args.logs:
        with open(log, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = LINE.search(line)
                if m:
                    tbp, psm, w, h, _, t, c = m.groups()
                    ids[(t, c, w, h, int(psm, 16))] = tbp
                    continue
                m = LOOKUP.search(line)
                if m:
                    lookups.add(m.groups()[4:])
                    continue
                m = BOUND.search(line)
                if m:
                    bound.add(m.group(5))
    total = len(ids)
    render = sum(1 for k in ids if k[0] == '0' * 16)
    direct = sum(1 for k in ids if k[0] != '0' * 16 and k[4] in (0, 1, 2))
    whole_in_pack = sum(1 for (t, c, w, h, psm) in ids
                        if '%s_%s_%sx%s_psm%02x' % (t, c, w, h, psm) in pack or
                        '%s_%s_%sx%s_psm%02x' % (t, '0' * 16, w, h, psm) in pack)
    tex_offline = sum(1 for k in ids if k[0] in offline_tex)
    tex_pack = sum(1 for k in ids if k[0] in pack_tex)
    part_hits = sum(1 for (pw, ph, t, c) in lookups
                    if any(n.startswith('%s_%s_%sx%s' % (t, c, pw, ph)) or n.startswith('%s_%s_%sx%s' % (t, '0' * 16, pw, ph))
                           for n in pack)) if len(lookups) < 5000 else -1
    print('distinct texture identities the game built:', total)
    print('  render-target / partial (texels 0, never replaceable):', render)
    print('  direct colour (psm 00/01/02, mostly frame copies):', direct)
    print('  texel hash computed offline from the disc:', tex_offline, '(%.1f%%)' % (100.0 * tex_offline / max(total, 1)))
    print('  whole texture named in the pack:', whole_in_pack, '(%.1f%%)' % (100.0 * whole_in_pack / max(total, 1)))
    print('  texel hash in the pack (any palette):', tex_pack)
    print('lookups the cache made (whole and parts):', len(lookups), '| hits in the pack:', part_hits)
    print('pictures bound (files and generated):', len(bound))
    if args.misses:
        with open(args.misses, 'w', encoding='utf-8') as fh:
            for (t, c, w, h, psm), tbp in sorted(ids.items()):
                name = '%s_%s_%sx%s_psm%02x' % (t, c, w, h, psm)
                if name in pack:
                    continue
                why = 'render-target' if t == '0' * 16 else ('palette-differs' if t in pack_tex else
                                                             ('offline-no-upscale' if t in offline_tex else 'unknown'))
                fh.write('%s\t%s\t%s\n' % (name, tbp, why))
        print('misses written to', args.misses)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--keys', required=True)
    b.add_argument('--out', required=True)
    b.add_argument('--upscaled', required=True, help='folder holding the model sets (x4plus-anime, animevideov3, x4plus) and flagged-classes.csv')
    b.add_argument('--stubs', default='', help='tim2_export.py stubs.csv (pictures that are not images)')
    b.add_argument('--actions', default='', help='old-set-actions.csv of the 2026-10-28 decoder fix (optional)')
    b.add_argument('--fallback-for-flagged', action='store_true',
                   help='(the default since 2026-10-28; kept so old command lines still work)')
    b.add_argument('--no-flagged', action='store_true',
                   help='ignore flagged-classes.csv\'s recommended sets: x4plus-anime everywhere (the pack before '
                   '2026-10-28)')
    b.add_argument('--wildcard-short', action='store_true')
    b.add_argument('--copy', action='store_true')
    b.add_argument('--clean', action='store_true')
    b.add_argument('--native', action='append', help='TIM2 name (the name column of an --extra manifest) whose disc picture '
                   'goes beside its upscale as native_<key>.png, for the atlas matching of the renderer (default: item_icon)')
    b.add_argument('--extra', action='append', help='MANIFEST.csv=UPSCALED_ROOT[|PRIMARY_SET|FALLBACK_SET] with '
                   'gs_key/hash columns')
    b.add_argument('--extra-psm-hex', dest='extra_psm_decimal', action='store_false',
                   help='the --extra manifests write psm in hex (default: decimal, as im3_export.py does)')
    v = sub.add_parser('validate')
    v.add_argument('--keys', required=True)
    v.add_argument('--pack', required=True)
    v.add_argument('--misses')
    v.add_argument('logs', nargs='+')
    args = ap.parse_args()
    build(args) if args.cmd == 'build' else validate(args)


if __name__ == '__main__':
    main()
