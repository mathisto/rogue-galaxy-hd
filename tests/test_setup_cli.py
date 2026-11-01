"""The command line end to end, on a stand-in program folder and synthetic discs (never the game)."""
import contextlib
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import setup_fakes as fk
from rgsetup import cli, disc, settings, state as statefile


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.files = fk.fake_files()
        self.image = fk.build_iso(self.files)
        self.known = fk.known_for(self.files, iso_size=len(self.image), sectors=len(self.image) // 2048)
        self.root = fk.fake_program_root(self.t, self.known)
        # the HD builder module carries the pinned downloads; the stand-in folder needs it for the plan
        shutil.copy(fk.REPO / 'tools' / 'hd' / 'build_hd_pack.py', self.root / 'tools' / 'hd' / 'build_hd_pack.py')
        self.ini = self.t / 'settings.ini'
        self.env = {'RG_SETUP_FAKE_VULKAN': 'Test GPU:discrete'}
        self.old = {k: os.environ.get(k) for k in ('RG_SETUP_FAKE_VULKAN',)}
        os.environ.update(self.env)
        self.addCleanup(self.restore)
        old = disc.QUICK_LIMIT
        disc.QUICK_LIMIT = 3000
        self.addCleanup(setattr, disc, 'QUICK_LIMIT', old)

    def restore(self):
        for k, v in self.old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def run_cli(self, *args, dest=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(['--dest', str(dest or self.root), '--settings-file', str(self.ini), *map(str, args)], root=self.root)
        evs = [json.loads(x) for x in out.getvalue().splitlines()]
        return code, evs

    def only(self, evs, type):
        return [e for e in evs if e['type'] == type]

    # --- shape of a run
    def test_a_run_emits_start_plan_steps_and_end_in_order(self):
        code, evs = self.run_cli('--preset', 'faithful', '--components', 'prompts_font', '--skip', 'extract,build,toolchain,source')
        self.assertEqual(code, 0, evs)
        self.assertEqual(evs[0]['type'], 'start')
        self.assertEqual(evs[1]['type'], 'plan')
        self.assertEqual(evs[-1], {'type': 'end', 't': evs[-1]['t'], 'result': 'ok', 'seconds': evs[-1]['seconds']})
        start = evs[0]
        self.assertEqual(start['steps'], ['check', 'source', 'extract', 'toolchain', 'build', 'hd_textures', 'hd_movies',
                                          'prompts_font', 'finish'])
        self.assertEqual(start['release'], '9.8.7')
        plan = evs[1]
        self.assertEqual(plan['components'], ['prompts_font'])
        self.assertEqual(plan['region'], 'SCUS-97490')
        self.assertIn('prompts_font', plan['run'])
        for key in ('skip', 'downloads', 'disk', 'time', 'skipped_components', 'preset'):
            self.assertIn(key, plan)
        done = [e['step'] for e in evs if e['type'] == 'step' and e['state'] == 'done']
        self.assertEqual(done, ['check', 'prompts_font', 'finish'])
        self.assertEqual(self.only(evs, 'space')[0]['same_disk'], True)

    def test_prompts_font_writes_only_its_two_keys_and_the_state_records_it(self):
        self.ini.write_text('[game]\nsixty_fps = 1\n')
        code, evs = self.run_cli('--components', 'prompts_font', '--skip', 'extract,build,toolchain,source')
        self.assertEqual(code, 0)
        self.assertEqual(settings.present_keys(self.ini.read_text()), {'sixty_fps': '1', 'button_prompts': '1', 'font_style': '1'})
        st = statefile.load(self.root)
        self.assertEqual(st['components'], ['prompts_font'])
        self.assertIn('prompts_font', st['steps'])
        self.assertEqual(st['steps']['prompts_font']['added'], ['button_prompts', 'font_style'])

    def test_second_run_needs_only_dest_and_skips_what_is_done(self):
        self.run_cli('--components', 'prompts_font', '--skip', 'extract,build,toolchain,source')
        self.ini.write_text('[game]\nbutton_prompts = 0\nfont_style = 0\n')      # the player's own choice in the menu
        code, evs = self.run_cli('--skip', 'extract,build,toolchain,source')
        self.assertEqual(code, 0)
        self.assertEqual(self.only(evs, 'start')[0]['mode'], 'resume')
        skipped = {e['step']: e['reason'] for e in evs if e['type'] == 'step' and e['state'] == 'skipped'}
        self.assertEqual(skipped['prompts_font'], 'current')
        self.assertEqual(self.ini.read_text(), '[game]\nbutton_prompts = 0\nfont_style = 0\n')      # not overwritten

    def test_components_added_later_are_added_to_what_is_there(self):
        self.run_cli('--components', 'prompts_font', '--skip', 'extract,build,toolchain,source')
        code, evs = self.run_cli('--components', 'textures', '--estimate')
        self.assertEqual(self.only(evs, 'plan')[0]['components'], ['textures', 'prompts_font'])

    def test_a_newer_release_is_an_update(self):
        self.run_cli('--components', 'prompts_font', '--skip', 'extract,build,toolchain,source')
        (self.root / 'CMakeLists.txt').write_text('project(x VERSION 9.9.0)\n')
        code, evs = self.run_cli('--skip', 'extract,build,toolchain,source')
        self.assertEqual(self.only(evs, 'start')[0]['mode'], 'update')
        self.assertEqual(statefile.load(self.root)['release'], '9.9.0')

    # --- the checks
    def test_estimate_changes_nothing(self):
        before = sorted(p.relative_to(self.root) for p in self.root.rglob('*'))
        iso = self.t / 'x.iso'
        iso.write_bytes(self.image)
        code, evs = self.run_cli('--estimate', '--source', iso, '--preset', 'everything')
        self.assertEqual(code, 0, evs)
        after = sorted(p.relative_to(self.root) for p in self.root.rglob('*'))
        self.assertEqual(before, after)
        self.assertFalse(self.ini.exists())
        self.assertTrue(self.only(evs, 'plan'))
        self.assertTrue(self.only(evs, 'space'))
        self.assertEqual([e for e in evs if e['type'] == 'step' and e['state'] == 'started'][0]['step'], 'check')
        self.assertEqual(len([e for e in evs if e['type'] == 'step' and e['state'] == 'started']), 1)

    def test_the_plan_lists_downloads_with_url_size_hash_and_licence(self):
        old_path = os.environ['PATH']
        os.environ['PATH'] = os.path.join(os.environ['SystemRoot'], 'System32')      # no ffmpeg on the PATH
        self.addCleanup(os.environ.__setitem__, 'PATH', old_path)
        code, evs = self.run_cli('--estimate', '--preset', 'everything', '--skip', 'extract,build,toolchain,source')
        plan = self.only(evs, 'plan')[0]
        ids = {d['id'] for d in plan['downloads']}
        self.assertIn('realesrgan', ids)
        self.assertIn('ffmpeg', ids)
        for d in plan['downloads']:
            self.assertTrue(d['url'].startswith('https://'))
            self.assertGreater(d['bytes'], 0)
            self.assertTrue(d['license'])
            self.assertTrue(d['dest'])

    def test_downloads_need_a_yes(self):
        code, evs = self.run_cli('--components', 'textures', '--skip', 'extract,build,toolchain,source')
        self.assertEqual(code, 1)
        err = self.only(evs, 'error')[0]
        self.assertEqual(err['id'], 'err.consent_needed')
        self.assertIn('Real-ESRGAN', err['params']['names'])
        self.assertFalse([e for e in evs if e['type'] == 'step' and e['state'] == 'started'])

    def test_no_vulkan_leaves_hd_out_and_the_rest_installs(self):
        os.environ['RG_SETUP_FAKE_VULKAN'] = 'none'
        code, evs = self.run_cli('--preset', 'everything', '--skip', 'extract,build,toolchain,source')
        self.assertEqual(code, 0, evs)
        plan = self.only(evs, 'plan')[0]
        self.assertEqual(plan['components'], ['prompts_font'])
        self.assertEqual({s['id'] for s in plan['skipped_components']}, {'textures', 'movies'})
        self.assertEqual(plan['skipped_components'][0]['reason'], 'warn.hd_no_vulkan')
        self.assertIn('warn.hd_no_vulkan', [e['id'] for e in self.only(evs, 'warning')])
        self.assertEqual(plan['downloads'], [])

    def test_only_a_software_device_is_not_a_graphics_card(self):
        os.environ['RG_SETUP_FAKE_VULKAN'] = 'software'
        code, evs = self.run_cli('--components', 'textures', '--estimate', '--skip', 'extract,build,toolchain,source')
        plan = self.only(evs, 'plan')[0]
        self.assertEqual(plan['components'], [])
        self.assertEqual(plan['skipped_components'][0]['reason'], 'warn.hd_no_gpu')

    def test_a_folder_with_other_files_is_refused(self):
        other = self.t / 'photos'
        other.mkdir()
        (other / 'cat.jpg').write_bytes(b'x')
        code, evs = self.run_cli('--estimate', dest=other)
        self.assertEqual(code, 1)
        self.assertEqual(self.only(evs, 'error')[0]['id'], 'err.dest_foreign')

    def test_an_empty_other_folder_is_accepted_by_the_check_but_not_for_a_real_install_yet(self):
        empty = self.t / 'empty'
        empty.mkdir()
        code, evs = self.run_cli('--estimate', '--skip', 'extract', dest=empty)
        self.assertEqual(code, 0, evs)
        self.assertIn('warn.dest_not_program_folder', [e['id'] for e in self.only(evs, 'warning')])
        code, evs = self.run_cli('--components', 'prompts_font', dest=empty)
        self.assertEqual(code, 1)
        self.assertEqual(self.only(evs, 'error')[0]['id'], 'err.dest_not_program_folder')

    def test_the_disc_is_required_when_it_must_be_extracted(self):
        code, evs = self.run_cli('--preset', 'faithful', '--skip', 'build,toolchain')
        self.assertEqual(code, 1)
        self.assertEqual(self.only(evs, 'error')[0]['id'], 'err.source_required')

    def test_an_estimate_without_a_disc_warns_and_goes_on(self):
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--skip', 'build,toolchain')
        self.assertEqual(code, 0, evs)
        self.assertIn('warn.no_source', [e['id'] for e in self.only(evs, 'warning')])

    def test_a_folder_from_before_the_setup_is_adopted_not_redone(self):
        fk.write_folder(self.root / 'disc', self.files)
        (self.root / 'disc' / 'manifest.json').write_text(json.dumps({'version': 'SCUS_974.90', 'files': {}}))
        code, evs = self.run_cli('--estimate', '--skip', 'build,toolchain')
        self.assertEqual(code, 0, evs)
        self.assertEqual(self.only(evs, 'start')[0]['adopted'], ['extract'])
        plan = self.only(evs, 'plan')[0]
        self.assertEqual(plan['skip']['extract'], 'current')
        self.assertNotIn('extract', plan['run'])
        self.assertFalse((self.root / statefile.NAME).exists())          # an estimate records nothing
        # a real run records the adoption, and a damaged disc/ is not adopted
        (self.root / 'disc' / 'BIN' / 'ONE.BIN').write_bytes(b'short')
        code, evs = self.run_cli('--estimate', '--skip', 'build,toolchain')
        self.assertEqual(self.only(evs, 'start')[0]['adopted'], [])
        self.assertIn('extract', self.only(evs, 'plan')[0]['run'])

    def test_a_good_synthetic_iso_passes_the_check_and_a_bad_one_gives_one_message(self):
        iso = self.t / 'x.iso'
        iso.write_bytes(self.image)
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--source', iso)
        self.assertEqual(code, 0, evs)
        bad = bytearray(self.image)
        i = bad.find(bytes(self.files['BIN/ONE.BIN'][:40])) + 100
        bad[i] ^= 0xFF
        iso.write_bytes(bytes(bad))
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--source', iso)
        self.assertEqual(code, 1)
        errs = self.only(evs, 'error')
        self.assertEqual([e['id'] for e in errs], ['err.disc_mismatch'])
        self.assertEqual(errs[0]['step'], 'check')
        self.assertEqual(self.only(evs, 'end')[0]['result'], 'failed')

    def test_a_folder_source_is_validated_by_hashes(self):
        folder = self.t / 'dump'
        fk.write_folder(folder, self.files)
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--source', folder)
        self.assertEqual(code, 0, evs)
        (folder / 'MOVIE' / 'A.MVI').unlink()
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--source', folder)
        self.assertEqual(code, 1)
        e = self.only(evs, 'error')[0]
        self.assertEqual((e['id'], e['params']['missing']), ('err.disc_mismatch', 1))

    def test_a_disc_of_another_region_says_so(self):
        files = fk.fake_files(b'BOOT2 = cdrom0:\\SCES_545.67;1\r\n')
        folder = self.t / 'pal'
        fk.write_folder(folder, files)
        code, evs = self.run_cli('--estimate', '--preset', 'faithful', '--source', folder)
        e = self.only(evs, 'error')[0]
        self.assertEqual(e['id'], 'err.disc_region')
        self.assertEqual((e['params']['serial'], e['params']['region']), ('SCES-54567', 'Europe'))

    # --- usage errors
    def test_bad_components_and_steps_exit_2(self):
        code, evs = self.run_cli('--components', 'models')
        self.assertEqual(code, 2)
        self.assertEqual(self.only(evs, 'error')[0]['id'], 'err.usage')
        code, evs = self.run_cli('--redo', 'nonsense')
        self.assertEqual(code, 2)

    def test_argparse_errors_exit_2_with_a_json_error(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                cli.main(['--preset', 'bogus', '--dest', str(self.root)], root=self.root)
        self.assertEqual(cm.exception.code, 2)
        lines = [json.loads(x) for x in out.getvalue().splitlines()]
        self.assertEqual(lines[0]['type'], 'error')
        self.assertEqual(lines[0]['id'], 'err.usage')
        self.assertEqual(lines[-1]['type'], 'end')

    def test_text_format_prints_words(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(['--dest', str(self.root), '--format', 'text', '--estimate', '--preset', 'faithful',
                             '--skip', 'extract'], root=self.root)
        self.assertEqual(code, 0)
        self.assertIn('Installing: the game only.', out.getvalue())
        self.assertIn('Rogue Galaxy Recompiled 9.8.7 setup', out.getvalue())
        self.assertNotIn('"type"', out.getvalue())

    def test_unreadable_state_is_one_clear_error(self):
        (self.root / statefile.NAME).write_text('{nope')
        code, evs = self.run_cli('--estimate')
        self.assertEqual(code, 1)
        self.assertEqual(self.only(evs, 'error')[0]['id'], 'err.state_unreadable')


@unittest.skipUnless(os.name == 'nt', 'Ctrl+Break is how the Windows window cancels')
class Cancel(unittest.TestCase):
    def test_ctrl_break_ends_with_exit_3_and_a_cancelled_end_event(self):
        env = dict(os.environ, RG_SETUP_TEST_SLEEP='60', PYTHONIOENCODING='utf-8')
        cmd = [sys.executable, '-m', 'rgsetup', '--dest', str(fk.REPO), '--estimate', '--preset', 'faithful', '--skip', 'extract']
        p = subprocess.Popen(cmd, cwd=str(fk.REPO / 'tools' / 'setup'), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            # wait for the check step to start, so the signal arrives while it runs
            seen = []
            for line in p.stdout:
                seen.append(json.loads(line))
                if seen[-1].get('type') == 'step' and seen[-1].get('state') == 'started':
                    break
            p.send_signal(signal.CTRL_BREAK_EVENT)
            rest = p.stdout.read()
            p.wait(timeout=60)
        finally:
            if p.poll() is None:
                p.kill()
        seen += [json.loads(x) for x in rest.splitlines()]
        self.assertEqual(p.returncode, 3)
        self.assertEqual(seen[-1]['type'], 'end')
        self.assertEqual(seen[-1]['result'], 'cancelled')
        self.assertFalse([e for e in seen if e['type'] == 'error'])

    def test_no_arguments_exit_2(self):
        r = subprocess.run([sys.executable, '-m', 'rgsetup'], cwd=str(fk.REPO / 'tools' / 'setup'), capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        first = json.loads(r.stdout.splitlines()[0])
        self.assertEqual(first['id'], 'err.usage')


class ProcCancel(unittest.TestCase):
    def test_a_cancel_kills_the_child_and_only_it(self):
        import _thread
        import threading
        import time
        from rgsetup import proc
        started = []
        t = threading.Timer(1.5, _thread.interrupt_main)
        t.start()
        t0 = time.time()
        with self.assertRaises(KeyboardInterrupt):
            proc.run([sys.executable, '-c', 'import time,sys; print("up", flush=True); time.sleep(60)'], on_line=started.append)
        t.cancel()
        self.assertLess(time.time() - t0, 20)
        self.assertEqual(started, ['up'])


if __name__ == '__main__':
    unittest.main()
