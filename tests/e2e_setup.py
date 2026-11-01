#!/usr/bin/env python3
"""End to end, with a real disc: a clean clone of this repository, the setup run on it, a reduced HD build.

    python tests/e2e_setup.py --scratch D:\\scratch --iso "Rogue Galaxy (USA).iso" [--chd rg.chd] [--cancel] [--cleanup]

Needs your own disc image; it is only read. Everything it writes is under --scratch (a clone of this repository's
current branch, the unpacked disc, the HD work files): about 25 GB, removed again with --cleanup. The game is not built
(--skip toolchain,build: a build is 20+ minutes and 9 GB); the HD parts are trials (--limit 6, --limit-movies 1) with
--gentle, so the graphics card is used for a minute or two at the lowest priority. Not part of the unit tests:
`python -m unittest discover -s tests -p "test_setup_*.py"` runs those (synthetic discs, seconds).

What it checks (each line printed PASS or FAIL):
  1. --estimate --verify on the real source writes nothing.
  2. A run: check, source (CHD to ISO with a downloaded chdman when --chd), extract, HD textures and movies (trial),
     prompts_font into a scratch settings file; the events are well formed and in order; exit code 0.
  3. The disc in the clone matches config/SCUS_974.90/hashes.json (--verify re-hashes it, read only).
  4. A second run with only --dest skips every finished step.
  5. With --cancel: Ctrl+Break during a re-extraction ends with exit 3; the next run finishes it.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(('PASS ' if ok else 'FAIL ') + name + (f'  [{detail}]' if detail and not ok else ''), flush=True)


def run_setup(clone, args, env, timeout=7200, cancel_after=None):
    """Run python -m rgsetup in the clone; returns (exit code, events)."""
    cmd = [sys.executable, '-X', 'utf8', '-m', 'rgsetup', '--dest', str(clone), *map(str, args)]
    p = subprocess.Popen(cmd, cwd=str(clone / 'tools' / 'setup'), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, encoding='utf-8', creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
    events = []
    t0 = time.time()
    sent = False
    for line in p.stdout:
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
        e = events[-1]
        if e['type'] in ('step', 'error', 'warning', 'end', 'space'):
            print('   ', json.dumps(e)[:220], flush=True)
        if cancel_after and not sent and e['type'] == 'progress' and e['step'] == 'extract' and time.time() - t0 > cancel_after:
            p.send_signal(signal.CTRL_BREAK_EVENT)
            sent = True
    p.wait(timeout=timeout)
    err = p.stderr.read()
    if err.strip():
        print('    stderr:', err[-400:])
    return p.returncode, events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scratch', required=True, type=Path)
    ap.add_argument('--iso', required=True, type=Path)
    ap.add_argument('--chd', type=Path)
    ap.add_argument('--cancel', action='store_true')
    ap.add_argument('--cleanup', action='store_true')
    ap.add_argument('--no-movies', action='store_true')
    a = ap.parse_args()
    scratch = a.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    clone = scratch / 'clone'
    settings = scratch / 'settings.ini'
    source = a.chd or a.iso
    iso_before = (a.iso.stat().st_size, a.iso.stat().st_mtime_ns)
    env = dict(os.environ, RG_SETTINGS_FILE=str(settings), PYTHONUTF8='1')
    env.pop('RG_SETUP_FAKE_VULKAN', None)

    if not clone.exists():
        subprocess.run(['git', 'clone', '-q', '--no-hardlinks', '--branch', subprocess.run(
            ['git', '-C', str(REPO), 'rev-parse', '--abbrev-ref', 'HEAD'], capture_output=True, text=True).stdout.strip(),
            str(REPO), str(clone)], check=True)
    check('clean clone has no disc, toolchain or work folder', not any((clone / d).exists() for d in ('disc', 'toolchain', 'work', 'textures', 'build')))

    components = 'textures,prompts_font' if a.no_movies else 'textures,movies,prompts_font'
    common = ['--components', components, '--yes', '--gentle', '--limit', '6', '--limit-movies', '1',
              '--skip', 'toolchain,build', '--settings-file', settings]

    # 1. estimate + verify on the clone: nothing written
    before = sorted(str(p) for p in clone.rglob('*') if '.git' not in p.parts)
    rc, ev = run_setup(clone, ['--source', source, '--estimate', '--verify', *common], env)
    after = sorted(str(p) for p in clone.rglob('*') if '.git' not in p.parts)
    check('estimate exits 0', rc == 0, rc)
    check('estimate writes nothing into the folder', before == after)
    check('estimate has start, plan, space and end', [e['type'] for e in ev if e['type'] in ('start', 'plan', 'space', 'end')] == ['start', 'plan', 'space', 'end'])
    plan = next(e for e in ev if e['type'] == 'plan')
    print('    plan: run', plan['run'], '; downloads', [d['id'] for d in plan['downloads']], '; disk need GiB', round(plan['disk']['need'] / 2**30, 1), flush=True)

    # 2. the run
    t0 = time.time()
    rc, ev = run_setup(clone, ['--source', source, *common], env)
    secs = time.time() - t0
    check('the run exits 0', rc == 0, rc)
    check('the run ends with ok', ev and ev[-1] == {**ev[-1], 'type': 'end', 'result': 'ok'})
    started = [e['step'] for e in ev if e['type'] == 'step' and e['state'] == 'started']
    done = [e['step'] for e in ev if e['type'] == 'step' and e['state'] == 'done']
    check('steps started in order', started == [s for s in ['check', 'source', 'extract', 'hd_textures', 'hd_movies', 'prompts_font', 'finish']
                                                if s in started] and 'extract' in started and 'hd_textures' in started, started)
    check('every started step is done', started == done, (started, done))
    check('no errors', not [e for e in ev if e['type'] == 'error'], [e for e in ev if e['type'] == 'error'][:1])
    if source.suffix.lower() == '.chd':
        check('chdman was downloaded and used', any(e['type'] == 'progress' and e['phase'] == 'download_chdman' for e in ev)
              and (clone / 'toolchain' / 'chdman' / 'chdman.exe').is_file())
    check('the disc is in the clone', (clone / 'disc' / 'manifest.json').is_file() and (clone / 'disc' / 'SCUS_974.90').is_file())
    check('trial texture pack written', (clone / 'textures' / 'SCUS-97490' / 'pack-manifest.csv').is_file())
    if not a.no_movies:
        check('a trial movie written', any((clone / 'textures' / 'SCUS-97490' / 'movies').rglob('*.mkv')) if (clone / 'textures' / 'SCUS-97490' / 'movies').is_dir() else False)
    check('settings defaults written to the scratch file only', settings.is_file() and 'button_prompts = 1' in settings.read_text())
    check('state file records the finished steps', (clone / 'rg-install.json').is_file() and
          set(json.loads((clone / 'rg-install.json').read_text())['steps']) >= {'extract', 'hd_textures', 'prompts_font'})
    print(f'    run took {secs / 60:.1f} min', flush=True)

    # 3. verify read only
    rc, ev = run_setup(clone, ['--estimate', '--verify'], env)
    check('verify of the unpacked disc passes (no warn.output_changed)', rc == 0 and not [e for e in ev if e['type'] == 'warning' and e['id'] == 'warn.output_changed'])

    # 4. second run
    rc, ev = run_setup(clone, ['--skip', 'toolchain,build', '--settings-file', settings, '--yes', '--gentle'], env)
    skipped = {e['step']: e.get('reason') for e in ev if e['type'] == 'step' and e['state'] == 'skipped'}
    started = [e['step'] for e in ev if e['type'] == 'step' and e['state'] == 'started']
    check('the second run needs only --dest and does no work', rc == 0 and started == ['check', 'finish'] and skipped.get('extract') == 'current', (rc, started, skipped))
    check('the second run is a resume', ev[0]['mode'] == 'resume', ev[0].get('mode'))

    # 5. cancel
    if a.cancel:
        rc, ev = run_setup(clone, ['--source', source, '--redo', 'extract', '--skip', 'toolchain,build,hd_textures,hd_movies', '--settings-file', settings, '--yes'],
                           env, cancel_after=8)
        check('Ctrl+Break during the extraction exits 3', rc == 3 and ev[-1]['result'] == 'cancelled', (rc, ev[-1:]))
        state = json.loads((clone / 'rg-install.json').read_text())
        check('the state still records the earlier steps', 'extract' in state['steps'])
        rc, ev = run_setup(clone, ['--source', source, '--skip', 'toolchain,build', '--settings-file', settings, '--yes', '--gentle', '--components', components,
                                   '--limit', '6', '--limit-movies', '1'], env)
        check('the next run finishes the job', rc == 0, rc)
        rc, ev = run_setup(clone, ['--estimate', '--verify'], env)
        check('the disc is whole after the cancel and the rerun', rc == 0 and not [e for e in ev if e['type'] == 'warning'])

    check('the source image was not touched', iso_before == (a.iso.stat().st_size, a.iso.stat().st_mtime_ns))
    freed = 0
    if a.cleanup:
        for p in clone.rglob('*'):
            try:
                if p.is_file():
                    freed += p.stat().st_size
            except OSError:
                pass

        def onerror(func, path, exc):
            os.chmod(path, 0o700)
            func(path)
        shutil.rmtree(clone, onerror=onerror)
        settings.unlink(missing_ok=True)
        print(f'cleaned up: freed {freed / 2**30:.1f} GiB', flush=True)
    failed = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
