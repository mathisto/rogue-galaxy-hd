#!/usr/bin/env python3
"""Verify Rogue Galaxy movie outputs (intermediate src/*.mkv or upscaled hd/*.mkv).

Usage: python -I rg_movies_verify.py DIR [--ref SRC_DIR] [--json OUT.json] [--only NAME] [--expect-height H]

For every *.mkv under DIR (recursive): ffprobe streams; one full decode (video + audio) with
`-v error` (any decoder message = error); counts decoded video frames and audio duration.
Checks: exactly 1 video stream, >=1 audio stream, no decode errors, |audio - video| <= 0.10 s,
display aspect 16:9, and with --ref the frame count equals the reference (src) file's frame count and
the duration matches within 0.05 s. Exit code 1 if any file fails.
"""
import argparse, json, os, subprocess, sys, shutil



def _exe(name):
    d = os.environ.get('RG_FFMPEG_DIR')   # build_hd_pack.py points this at toolchain/ffmpeg/bin
    if d and os.path.exists(os.path.join(d, name + '.exe')):
        return os.path.join(d, name + '.exe')
    return shutil.which(name) or name


FFMPEG = _exe('ffmpeg')
FFPROBE = _exe('ffprobe')


def probe(path):
    r = subprocess.run([FFPROBE, '-v', 'error', '-print_format', 'json', '-show_streams', '-show_format', path],
                       capture_output=True, text=True)
    return json.loads(r.stdout or '{}')


def decode(path):
    """Full decode. Returns (video_frames, audio_seconds, video_seconds, error_text)."""
    cmd = [FFMPEG, '-nostdin', '-hide_banner', '-v', 'error', '-i', path,
           '-map', '0:v:0', '-f', 'null', '-', '-map', '0:a?', '-f', 'null', '-',
           '-progress', 'pipe:1', '-nostats']
    # progress applies to the last output; use separate passes for clarity instead
    v = subprocess.run([FFMPEG, '-nostdin', '-hide_banner', '-v', 'error', '-i', path, '-map', '0:v:0',
                        '-f', 'null', '-', '-progress', 'pipe:1', '-nostats'], capture_output=True, text=True)
    frames, vsec = 0, 0.0
    for line in v.stdout.splitlines():
        if line.startswith('frame='):
            frames = int(line.split('=')[1])
        elif line.startswith('out_time_us='):
            try:
                vsec = int(line.split('=')[1]) / 1e6
            except ValueError:
                pass
    a = subprocess.run([FFMPEG, '-nostdin', '-hide_banner', '-v', 'error', '-i', path, '-map', '0:a:0',
                        '-f', 'null', '-', '-progress', 'pipe:1', '-nostats'], capture_output=True, text=True)
    asec = 0.0
    for line in a.stdout.splitlines():
        if line.startswith('out_time_us='):
            try:
                asec = int(line.split('=')[1]) / 1e6
            except ValueError:
                pass
    err = (v.stderr + a.stderr).strip()
    return frames, asec, vsec, err


def check(path, ref=None, expect_h=None):
    pr = probe(path)
    st = pr.get('streams', [])
    vs = [s for s in st if s.get('codec_type') == 'video']
    as_ = [s for s in st if s.get('codec_type') == 'audio']
    res = {'file': path, 'ok': True, 'problems': []}
    if len(vs) != 1:
        res['problems'].append('video streams=%d' % len(vs))
    if not as_:
        res['problems'].append('no audio')
    if vs:
        v = vs[0]
        res.update(codec=v.get('codec_name'), width=v.get('width'), height=v.get('height'),
                   dar=v.get('display_aspect_ratio'), sar=v.get('sample_aspect_ratio'),
                   fps=v.get('avg_frame_rate'), pix_fmt=v.get('pix_fmt'))
        dar = v.get('display_aspect_ratio')
        want = ref.get('dar') if ref else ('4:3' if v.get('width', 0) * 3 == v.get('height', 0) * 4 * 1
                                         or (v.get('width'), v.get('height')) == (512, 448) else None)
        if dar not in ('16:9', '4:3') or (want and dar != want):
            res['problems'].append('DAR %s (want %s)' % (dar, want or '16:9/4:3'))
        if expect_h and v.get('height') != expect_h:
            res['problems'].append('height %s != %s' % (v.get('height'), expect_h))
    res['audio'] = [{'codec': a.get('codec_name'), 'rate': a.get('sample_rate'), 'channels': a.get('channels'),
                     'lang': a.get('tags', {}).get('language')} for a in as_]
    frames, asec, vsec, err = decode(path)
    res.update(frames=frames, video_s=round(vsec, 3), audio_s=round(asec, 3))
    if err:
        res['problems'].append('decode errors: ' + err[:300].replace('\n', ' | '))
    if frames == 0:
        res['problems'].append('no frames')
    if as_ and abs(asec - vsec) > 0.10:
        res['problems'].append('audio %.3fs vs video %.3fs' % (asec, vsec))
    if ref:
        if ref.get('frames') != frames:
            res['problems'].append('frames %d != src %s' % (frames, ref.get('frames')))
        if abs(ref.get('video_s', 0) - vsec) > 0.05:
            res['problems'].append('duration %.3f != src %.3f' % (vsec, ref.get('video_s', 0)))
    res['ok'] = not res['problems']
    return res


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('dir'); ap.add_argument('--ref'); ap.add_argument('--json'); ap.add_argument('--only')
    ap.add_argument('--expect-height', type=int)
    a = ap.parse_args(argv)
    refmap = {}
    if a.ref and os.path.isfile(a.ref):
        for r in json.load(open(a.ref)):
            refmap[os.path.normcase(r['rel'])] = r
    out = []
    for dp, _, fs in os.walk(a.dir):
        for fn in sorted(fs):
            if not fn.lower().endswith('.mkv'):
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, a.dir)
            if a.only and a.only.upper() not in rel.upper():
                continue
            r = check(p, refmap.get(os.path.normcase(rel)), a.expect_height)
            r['rel'] = rel
            out.append(r)
            print('%-4s %-22s %4sx%-4s %6d fr %8.3fs v %8.3fs a  %s' % (
                'OK' if r['ok'] else 'FAIL', rel, r.get('width'), r.get('height'), r['frames'],
                r['video_s'], r['audio_s'], '; '.join(r['problems'])), flush=True)
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
    bad = [r for r in out if not r['ok']]
    print('%d files, %d failed, %d frames total, %.1f s total' % (
        len(out), len(bad), sum(r['frames'] for r in out), sum(r['video_s'] for r in out)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
