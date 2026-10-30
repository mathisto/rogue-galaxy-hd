#!/usr/bin/env python3
"""Rogue Galaxy HD cutscene batch: MVI -> lossless intermediate -> Real-ESRGAN -> HEVC 10-bit MKV.

Resumable, one movie at a time, bounded scratch disk, low priority, no console windows, keeps the PC
awake (not the display) only while it runs. Run with:  python -I tools/hd/rg_movies_batch.py [options]

  --list                 catalogue of all movies (frames, duration, status)
  --status               summary, measured throughput, ETA for what is left
  (no flag)              process every pending movie (skips done ones; resumes after a crash/reboot)
  --only NAME[,NAME..]   just these (match on file stem or relative path, case-insensitive, e.g. W_ARISA)
  --limit N              stop after N movies this run
  --retry-failed         also re-run movies marked failed (default: failed ones are skipped)
  --redo                 re-run movies even if done (with --only)
  --verify               full check of every output in hd/ against the source (frames, duration, aspect, audio)
  --gentle               idle priority, 1 Real-ESRGAN thread set, 20 s pause between movies (slower, quieter)
  --stall-timeout S      kill and retry a movie when no frame moves anywhere for S seconds (default 180)
  --retries N            how many more times a stalled movie is tried (default 2)

Normally build_hd_pack.py runs this (Build-HD-Pack.bat). Layout, with --work defaulting to
<repo>/work/hd/movies (or %RG_HD_WORK%/movies):
  --movie-root          the *.MVI inputs (89), default <repo>/disc/MOVIE, the folder Install.bat extracts
  src/<rel>.mkv         lossless intermediate (MPEG-2 copied, FLAC)        [made on demand]
  hd/<rel>.mkv          result: HEVC Main10 1920x1080 (16:9) / 1440x1080 (4:3), FLAC copied
  tmp/                  frame chunks in flight (deleted as they are consumed; <= ~3 chunks)
  state.json, batch.log per-file state and the log
"""
import argparse, json, os, queue, shutil, signal, subprocess, sys, threading, time, ctypes, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mvi_to_mkv  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
DEF_WORK = os.path.join(os.environ.get('RG_HD_WORK') or os.path.join(REPO, 'work', 'hd'), 'movies')
DEF_RE = os.path.join(REPO, 'toolchain', 'realesrgan')


def find_exe(name):
    """ffmpeg / ffprobe: %RG_FFMPEG_DIR% (build_hd_pack.py sets it to toolchain/ffmpeg/bin), then PATH."""
    d = os.environ.get('RG_FFMPEG_DIR')
    if d:
        for ext in ('.exe', ''):
            if os.path.exists(os.path.join(d, name + ext)):
                return os.path.join(d, name + ext)
    return shutil.which(name) or name


FFMPEG = find_exe('ffmpeg')
FFPROBE = find_exe('ffprobe')
IS_WIN = os.name == 'nt'
BELOW_NORMAL = 0x00004000
IDLE = 0x00000040
NO_WINDOW = 0x08000000

STOP = threading.Event()
CHILDREN = set()
LOG = None


def log(msg):
    line = '%s %s' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), msg)
    print(line, flush=True)
    if LOG:
        with open(LOG, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def popen(cmd, gentle, **kw):
    flags = (NO_WINDOW | (IDLE if gentle else BELOW_NORMAL)) if IS_WIN else 0
    p = subprocess.Popen(cmd, creationflags=flags, **kw)
    CHILDREN.add(p)
    return p


def reap(p):
    CHILDREN.discard(p)


def kill_children():
    for p in list(CHILDREN):
        try:
            if p.poll() is None:
                p.kill()  # exact PIDs we started, nothing else
        except Exception:
            pass


def keep_awake(on):
    if IS_WIN:
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | (ES_SYSTEM_REQUIRED if on else 0))


# ---------------------------------------------------------------- state
class State:
    def __init__(self, path):
        self.path = path
        self.d = json.load(open(path)) if os.path.exists(path) else {'files': {}}

    def get(self, rel):
        return self.d['files'].setdefault(rel, {'status': 'pending'})

    def save(self):
        tmp = self.path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, self.path)


def find_movies(root):
    out = []
    for dp, _, fs in os.walk(root):
        for fn in sorted(fs):
            if fn.upper().endswith('.MVI'):
                p = os.path.join(dp, fn)
                out.append((os.path.relpath(p, root)[:-4], p))
    return sorted(out)


def probe_video(path):
    r = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'v:0', '-count_packets', '-show_entries',
                        'stream=width,height,avg_frame_rate,nb_read_packets,display_aspect_ratio:format=duration',
                        '-of', 'json', path], capture_output=True, text=True,
                       creationflags=NO_WINDOW if IS_WIN else 0)
    j = json.loads(r.stdout or '{}')
    s = (j.get('streams') or [{}])[0]
    return {'w': s.get('width'), 'h': s.get('height'), 'fps': s.get('avg_frame_rate'),
            'frames': int(s.get('nb_read_packets') or 0), 'dar': s.get('display_aspect_ratio'),
            'dur': float(j.get('format', {}).get('duration') or 0)}


# ---------------------------------------------------------------- png stream splitting
PNG_SIG = b'\x89PNG\r\n\x1a\n'


def read_pngs(stream):
    """Yield whole PNG files from a concatenated image2pipe stream."""
    while True:
        sig = stream.read(8)
        if not sig:
            return
        if sig != PNG_SIG:
            raise RuntimeError('bad PNG stream')
        parts = [sig]
        while True:
            hdr = stream.read(8)
            ln = int.from_bytes(hdr[:4], 'big')
            body = stream.read(ln + 4)
            parts += [hdr, body]
            if hdr[4:8] == b'IEND':
                break
        yield b''.join(parts)


class StallError(RuntimeError):
    """No frame moved anywhere (decoder, Real-ESRGAN, encoder) for --stall-timeout seconds."""


# ---------------------------------------------------------------- one movie
def upscale_movie(a, src_mkv, out_mkv, info, tmpdir):
    stall_flag = os.path.join(a.work, '.stall-test-done')
    shutil.rmtree(tmpdir, ignore_errors=True)  # leftovers of a crashed run
    os.makedirs(tmpdir, exist_ok=True)
    fps = info['fps']
    four3 = info['h'] >= 448
    out_h = a.height
    out_w = (out_h * 4 // 3) if four3 else (out_h * 16 // 9)
    out_w -= out_w % 2
    dar = '4:3' if four3 else '16:9'
    part = out_mkv + '.part.mkv'
    os.makedirs(os.path.dirname(out_mkv), exist_ok=True)

    dec = popen([FFMPEG, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', src_mkv, '-map', '0:v:0',
                 '-vf', 'scale=in_color_matrix=bt601:in_range=tv:flags=accurate_rnd+full_chroma_int,format=rgb24',
                 '-f', 'image2pipe', '-c:v', 'png', '-compression_level', '1', '-'],
                a.gentle, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    vf = ('scale=%d:%d:flags=lanczos+accurate_rnd+full_chroma_int:out_color_matrix=bt709:out_range=tv,'
          'setsar=1,format=p010le' % (out_w, out_h))
    if a.encoder == 'hevc_nvenc':
        venc = ['-c:v', 'hevc_nvenc', '-preset', 'p7', '-tune', 'hq', '-rc', 'vbr', '-cq', str(a.cq), '-b:v', '0',
                '-maxrate', '60M', '-bufsize', '120M', '-profile:v', 'main10', '-spatial-aq', '1',
                '-temporal-aq', '1', '-rc-lookahead', '32', '-bf', '3', '-b_ref_mode', 'middle', '-g', '60']
    else:
        venc = ['-c:v', 'libx265', '-preset', 'slow', '-crf', str(a.cq), '-profile:v', 'main10',
                '-x265-params', 'keyint=60:min-keyint=1:aq-mode=3:log-level=error']
    enc = popen([FFMPEG, '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
                 '-f', 'image2pipe', '-framerate', fps, '-c:v', 'png', '-i', '-', '-i', src_mkv,
                 '-map', '0:v:0', '-map', '1:a', '-vf', vf] + venc +
                ['-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-color_range', 'tv',
                 '-aspect', dar, '-c:a', 'copy', '-map_metadata', '1',
                 '-metadata', 'comment=Real-ESRGAN %s x%d, %s' % (a.model, a.scale, a.encoder), part],
                a.gentle, stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    q = queue.Queue(maxsize=1)  # chunks upscaled but not yet encoded (bounds disk)
    err = []
    counts = {'dec': 0, 'enc': 0}
    t_gpu = [0.0]
    cur = {'re': None, 'cout': None}   # the Real-ESRGAN process and output folder of the chunk in flight

    def producer():
        try:
            k = 0
            pngs = read_pngs(dec.stdout)
            done = False
            while not done and not STOP.is_set():
                cin = os.path.join(tmpdir, 'in%05d' % k)
                cout = os.path.join(tmpdir, 'out%05d' % k)
                os.makedirs(cin, exist_ok=True); os.makedirs(cout, exist_ok=True)
                n = 0
                for png in pngs:
                    with open(os.path.join(cin, '%07d.png' % (counts['dec'] + n)), 'wb') as f:
                        f.write(png)
                    n += 1
                    if n >= a.chunk:
                        break
                else:
                    done = True
                if n == 0:
                    shutil.rmtree(cin, ignore_errors=True); shutil.rmtree(cout, ignore_errors=True)
                    break
                counts['dec'] += n
                t0 = time.time()
                cmd = [os.path.join(a.realesrgan, 'realesrgan-ncnn-vulkan.exe'), '-i', cin, '-o', cout,
                       '-n', a.model, '-s', str(a.scale), '-m', os.path.join(a.realesrgan, 'models'),
                       '-f', 'png', '-j', '1:1:1' if a.gentle else '2:2:2'] + (['-g', str(a.gpu)] if a.gpu is not None else [])
                if os.environ.get('RG_MOVIES_TEST_STALL') and k == 0 and not os.path.exists(stall_flag):
                    # test aid for the watchdog: the first chunk of the first try "hangs" like the real stall seen once
                    open(stall_flag, 'w').close()
                    cmd = [sys.executable, '-c', 'import time; time.sleep(3600)']
                cur['cout'] = cout
                re = popen(cmd, a.gentle, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                cur['re'] = re
                _, e = re.communicate(); reap(re); cur['re'] = None
                t_gpu[0] += time.time() - t0
                got = len([x for x in os.listdir(cout) if x.endswith('.png')])
                if re.returncode != 0 or got != n:
                    raise RuntimeError('realesrgan chunk %d: rc=%s, %d/%d frames: %s' % (
                        k, re.returncode, got, n, e.decode(errors='replace')[-400:]))
                shutil.rmtree(cin, ignore_errors=True)
                q.put(cout)
                k += 1
        except Exception as ex:  # noqa
            err.append('producer: %s' % ex)
        finally:
            q.put(None)

    def consumer():
        try:
            while True:
                cout = q.get()
                if cout is None:
                    break
                for fn in sorted(os.listdir(cout)):
                    if STOP.is_set():
                        raise RuntimeError('stopped')
                    p = os.path.join(cout, fn)
                    with open(p, 'rb') as f:
                        enc.stdin.write(f.read())
                    os.remove(p)
                    counts['enc'] += 1
                shutil.rmtree(cout, ignore_errors=True)
        except Exception as ex:  # noqa
            err.append('consumer: %s' % ex)
            while q.get() is not None:  # drain so the producer can finish
                pass

    tp = threading.Thread(target=producer, daemon=True)
    tc = threading.Thread(target=consumer, daemon=True)
    tp.start(); tc.start()
    last = time.time()
    stalled = False
    seen, seen_at = None, time.time()
    while tc.is_alive():
        tc.join(5)
        if time.time() - last > 60:
            last = time.time()
            log('    ... %d/%d frames encoded' % (counts['enc'], info['frames']))
        # Watchdog: one progress number made of the frames decoded, upscaled (files in the chunk's output folder) and
        # encoded. If it does not move for --stall-timeout seconds (a hang was seen once: Real-ESRGAN idle while the
        # pipe stalled), kill this movie's processes (ours only) and let the caller retry the movie.
        try:
            up = len(os.listdir(cur['cout'])) if cur['cout'] else 0
        except OSError:
            up = 0
        now = (counts['dec'], counts['enc'], up)
        if now != seen:
            seen, seen_at = now, time.time()
        elif a.stall_timeout and time.time() - seen_at > a.stall_timeout and not stalled:
            stalled = True
            log('    STALL: no frame moved for %d s (decoded %d, upscaled %d in this chunk, encoded %d of %d): '
                'stopping the processes of this movie' % (a.stall_timeout, counts['dec'], up, counts['enc'], info['frames']))
            for p in (dec, enc, cur['re']):
                try:
                    if p is not None and p.poll() is None:
                        p.kill()
                except Exception:
                    pass
    tp.join()
    try:
        enc.stdin.close()
    except Exception:
        pass
    if err or STOP.is_set():
        for p in (dec, enc):
            if p.poll() is None:
                p.kill()
    enc_err = enc.stderr.read().decode(errors='replace'); enc.wait(); reap(enc)
    dec_err = dec.stderr.read().decode(errors='replace'); dec.wait(); reap(dec)
    shutil.rmtree(tmpdir, ignore_errors=True)
    if STOP.is_set():
        raise KeyboardInterrupt
    if stalled:
        if os.path.exists(part):
            os.remove(part)
        raise StallError('stalled for %d s at %d/%d frames' % (a.stall_timeout, counts['enc'], info['frames']))
    if err:
        raise RuntimeError('; '.join(err))
    if enc.returncode or dec.returncode:
        raise RuntimeError('ffmpeg rc dec=%s enc=%s: %s %s' % (dec.returncode, enc.returncode,
                                                               dec_err[-300:], enc_err[-300:]))
    if counts['enc'] != info['frames']:
        raise RuntimeError('encoded %d frames, source has %d' % (counts['enc'], info['frames']))
    chk = probe_video(part)
    if chk['frames'] != info['frames'] or abs(chk['dur'] - info['dur']) > 0.1:
        raise RuntimeError('output check failed: %s vs %s' % (chk, info))
    os.replace(part, out_mkv)
    return {'gpu_s': round(t_gpu[0], 1), 'out_w': out_w, 'out_h': out_h}


# ---------------------------------------------------------------- main
def main(argv=None):
    global LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', default=DEF_WORK)
    ap.add_argument('--movie-root', help='default: <repo>/disc/MOVIE')
    ap.add_argument('--realesrgan', default=DEF_RE)
    ap.add_argument('--model', default='realesr-animevideov3')
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--height', type=int, default=1080)
    ap.add_argument('--encoder', default='hevc_nvenc', choices=['hevc_nvenc', 'libx265'])
    ap.add_argument('--cq', type=int, default=18)
    ap.add_argument('--chunk', type=int, default=300)
    ap.add_argument('--list', action='store_true'); ap.add_argument('--status', action='store_true')
    ap.add_argument('--only'); ap.add_argument('--limit', type=int)
    ap.add_argument('--retry-failed', action='store_true'); ap.add_argument('--redo', action='store_true')
    ap.add_argument('--verify', action='store_true'); ap.add_argument('--gentle', action='store_true')
    ap.add_argument('--gpu', type=int, default=None, help='Vulkan device index (ncnn -g); default: ncnn picks')
    ap.add_argument('--stall-timeout', type=int, default=180); ap.add_argument('--retries', type=int, default=2)
    a = ap.parse_args(argv)
    root = a.movie_root or os.path.join(REPO, 'disc', 'MOVIE')
    os.makedirs(a.work, exist_ok=True)
    LOG = os.path.join(a.work, 'batch.log')
    st = State(os.path.join(a.work, 'state.json'))
    movies = find_movies(root)
    if not movies:
        print('no .MVI under %s (Install.bat extracts them into disc/MOVIE; or pass --movie-root)' % root); return 2
    want = None
    if a.only:
        want = [w.strip().upper() for w in a.only.split(',') if w.strip()]
        movies = [(r, p) for r, p in movies if any(w == os.path.basename(r).upper() or w == r.upper().replace('/', '\\')
                                                    for w in want)]
        if not movies:
            print('no movie matches --only %s' % a.only); return 2

    if a.list or a.status:
        tot_f = left_f = 0; done_f = 0; gpu = wall = 0.0; ns = {}
        for rel, p in movies:
            s = st.get(rel)
            fr = s.get('frames')
            if fr is None:
                src = os.path.join(a.work, 'src', rel + '.mkv')
                if os.path.exists(src):
                    fr = probe_video(src)['frames']
            tot_f += fr or 0
            ns[s['status']] = ns.get(s['status'], 0) + 1
            if s['status'] == 'done':
                done_f += fr or 0; wall += s.get('wall_s', 0)
            else:
                left_f += fr or 0
            if a.list:
                print('%-8s %-18s %6s fr %7.1f s  %s' % (s['status'], rel, fr or '?', (fr or 0) / 29.97,
                                                           s.get('error', '')[:80]))
        rate = done_f / wall if wall else None
        print('%d movies: %s; frames total %d, left %d' % (len(movies), ns, tot_f, left_f))
        if rate:
            print('measured %.1f frames/s over %d done frames -> ETA for the rest %.1f h' % (
                rate, done_f, left_f / rate / 3600))
        return 0

    if a.verify:
        cmd = [sys.executable, '-I', os.path.join(HERE, 'rg_movies_verify.py'), os.path.join(a.work, 'hd'),
               '--ref', os.path.join(a.work, 'src', 'verify.json'), '--json', os.path.join(a.work, 'hd_verify.json'),
               '--expect-height', str(a.height)]
        if a.only and len(want) == 1:
            cmd += ['--only', want[0]]
        if not os.path.exists(os.path.join(a.work, 'src', 'verify.json')):
            subprocess.run([sys.executable, '-I', os.path.join(HERE, 'rg_movies_verify.py'),
                            os.path.join(a.work, 'src'), '--json', os.path.join(a.work, 'src', 'verify.json')])
        return subprocess.run(cmd).returncode

    # preflight: a systemic problem should stop the run, not fail 89 movies one by one
    exe = os.path.join(a.realesrgan, 'realesrgan-ncnn-vulkan.exe')
    model = os.path.join(a.realesrgan, 'models', '%s%s.param' % (a.model, '-x%d' % a.scale if 'animevideov3' in a.model else ''))
    for need in (exe, model):
        if not os.path.exists(need):
            log('preflight: missing %s' % need); return 2
    if a.encoder == 'hevc_nvenc':
        t = subprocess.run([FFMPEG, '-v', 'error', '-f', 'lavfi', '-i', 'color=black:s=1920x1080:d=0.1', '-c:v',
                            'hevc_nvenc', '-profile:v', 'main10', '-pix_fmt', 'p010le', '-f', 'null', '-'],
                           capture_output=True, text=True, creationflags=NO_WINDOW if IS_WIN else 0)
        if t.returncode:
            log('preflight: hevc_nvenc unavailable (%s); using libx265' % t.stderr.strip()[-200:])
            a.encoder = 'libx265'

    def on_sig(*_):
        STOP.set()
    signal.signal(signal.SIGINT, on_sig)
    if hasattr(signal, 'SIGBREAK'):
        signal.signal(signal.SIGBREAK, on_sig)
    keep_awake(True)
    free = shutil.disk_usage(a.work).free / 2**30
    log('batch start: %d candidates, model %s x%d -> %dp, %s cq %d, chunk %d%s, free %.0f GiB' % (
        len(movies), a.model, a.scale, a.height, a.encoder, a.cq, a.chunk, ', gentle' if a.gentle else '', free))
    n_done = 0
    fails_in_row = 0
    try:
        for rel, mvi in movies:
            if STOP.is_set():
                break
            s = st.get(rel)
            out = os.path.join(a.work, 'hd', rel + '.mkv')
            if s['status'] == 'done' and os.path.exists(out) and not a.redo:
                continue
            if s['status'] == 'failed' and not (a.retry_failed or a.only):
                continue
            if a.limit is not None and n_done >= a.limit:
                break
            if shutil.disk_usage(a.work).free < 10 * 2**30:
                log('STOP: less than 10 GiB free on %s' % a.work); break
            t0 = time.time()
            try:
                src = os.path.join(a.work, 'src', rel + '.mkv')
                if not os.path.exists(src):
                    mvi_to_mkv.convert(mvi, src)
                info = probe_video(src)
                s.update(status='running', frames=info['frames'], started=time.strftime('%Y-%m-%d %H:%M:%S'))
                s.pop('error', None); st.save()
                log('[%s] %d frames %dx%d @%s' % (rel, info['frames'], info['w'], info['h'], info['fps']))
                for attempt in range(a.retries + 1):
                    try:
                        r = upscale_movie(a, src, out, info, os.path.join(a.work, 'tmp', rel.replace(os.sep, '_')))
                        break
                    except StallError as ex:
                        log('[%s] %s%s' % (rel, ex, '; trying again (%d of %d)' % (attempt + 1, a.retries)
                                           if attempt < a.retries else '; giving up on this movie'))
                        if attempt >= a.retries:
                            raise
                wall = time.time() - t0
                s.update(status='done', wall_s=round(wall, 1), fps=round(info['frames'] / wall, 2),
                         out_bytes=os.path.getsize(out), **r)
                st.save(); n_done += 1
                log('[%s] done in %.0f s (%.1f fps), %.1f MB' % (rel, wall, info['frames'] / wall,
                                                                  s['out_bytes'] / 2**20))
            except KeyboardInterrupt:
                s.update(status='pending'); st.save()
                log('[%s] interrupted; will restart from the beginning of this movie' % rel)
                break
            except Exception as ex:  # keep going with the next movie
                s.update(status='failed', error=str(ex)[:500]); st.save()
                log('[%s] FAILED: %s' % (rel, ex))
                fails_in_row += 1
                if fails_in_row >= 3:
                    log('STOP: three movies failed in a row; something systemic (see the errors above)')
                    break
            else:
                fails_in_row = 0
                for f in (out + '.part.mkv',):
                    if os.path.exists(f):
                        os.remove(f)
            if a.gentle and not STOP.is_set():
                time.sleep(20)
    finally:
        kill_children()
        keep_awake(False)
        shutil.rmtree(os.path.join(a.work, 'tmp'), ignore_errors=True)
    log('batch end: %d movies done this run' % n_done)
    return 0


if __name__ == '__main__':
    sys.exit(main())
