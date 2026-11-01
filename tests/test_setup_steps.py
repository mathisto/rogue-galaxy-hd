"""Steps on their own: the folder copy, the BIN/CUE conversion step, the HD builder's events, the keys, the game check."""
import io
import json
import os
import shutil
import subprocess
import tempfile
import time
import types
import unittest
from pathlib import Path

import setup_fakes as fk
from rgsetup import disc, events, hostcheck, steps
from rgsetup.layout import Layout
from rgsetup.runner import SetupError


class Stub:
    """Just enough of cli.SetupContext for a step."""
    def __init__(self, tmp, files, **opts):
        self.image = fk.build_iso(files)
        self.known = fk.known_for(files, iso_size=len(self.image), sectors=len(self.image) // 2048)
        self.root = fk.fake_program_root(tmp, self.known)
        self.layout = Layout(self.root, root=self.root)
        self.dest = self.layout.dest
        self.temp = Path(tmp) / 'temp'
        self.out = io.StringIO()
        self.reporter = events.Reporter(out=self.out)
        self.options = types.SimpleNamespace(verify=False, gentle=False, limit=0, limit_movies=0, gpu=None, clean_work=False,
                                             tools_on_path=False, **opts)
        self.shared, self.keys = {}, {}
        self.state = {'steps': {}, 'inputs': {}, 'components': []}
        self.source_kind = None
        self.source_path = None
        self.known_digest = disc.known_digest(self.known)
        self.components = ()
        self.movie_count = 1

    def events(self):
        return [json.loads(x) for x in self.out.getvalue().splitlines()]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.files = fk.fake_files()
        self.old = disc.QUICK_LIMIT
        disc.QUICK_LIMIT = 3000
        self.addCleanup(setattr, disc, 'QUICK_LIMIT', self.old)


class FolderCopy(Base):
    def test_a_folder_is_copied_hashed_and_gets_a_manifest(self):
        ctx = Stub(self.t, self.files)
        folder = self.t / 'dump'
        fk.write_folder(folder, self.files)
        ctx.source_kind, ctx.source_path = 'folder', folder
        ctx.shared['disc'] = disc.validate_folder(folder, ctx.known)
        total = sum(i['size'] for i in ctx.known['files'].values())
        steps.ExtractStep().copy_folder(ctx, ctx.shared['disc'], total)
        for p, b in self.files.items():
            self.assertEqual((ctx.layout.disc / p).read_bytes(), b)
        man = json.loads((ctx.layout.disc / 'manifest.json').read_text())
        self.assertEqual(set(man['files']), set(self.files))
        self.assertEqual(man['version'], 'SCUS_974.90')
        self.assertTrue(steps.disc_current(ctx.layout, ctx.known))
        self.assertEqual(steps.disc_rehash(ctx.layout, ctx.known), [])
        self.assertFalse(list((ctx.layout.disc).rglob('*.part')))
        self.assertEqual(ctx.events()[-1]['done'], total)

    def test_a_file_that_changes_while_copying_is_caught_and_no_manifest_is_written(self):
        ctx = Stub(self.t, self.files)
        folder = self.t / 'dump'
        fk.write_folder(folder, self.files)
        info = disc.validate_folder(folder, ctx.known)
        # a big file (checked by size only up front) with the right size and wrong content
        (folder / 'DATA0_0.DAT').write_bytes(bytes(len(self.files['DATA0_0.DAT'])))
        total = sum(i['size'] for i in ctx.known['files'].values())
        with self.assertRaises(SetupError) as cm:
            steps.ExtractStep().copy_folder(ctx, info, total)
        self.assertEqual(cm.exception.id, 'err.extract_hash')
        self.assertEqual(cm.exception.params['first'], 'DATA0_0.DAT')
        self.assertFalse((ctx.layout.disc / 'manifest.json').exists())

    def test_the_cheap_check_notices_a_missing_or_short_file(self):
        ctx = Stub(self.t, self.files)
        fk.write_folder(ctx.layout.disc, self.files)
        (ctx.layout.disc / 'manifest.json').write_text(json.dumps({'version': 'SCUS_974.90'}))
        self.assertTrue(steps.disc_current(ctx.layout, ctx.known))
        (ctx.layout.disc / 'MOVIE' / 'A.MVI').write_bytes(b'x')
        self.assertFalse(steps.disc_current(ctx.layout, ctx.known))
        (ctx.layout.disc / 'MOVIE' / 'A.MVI').unlink()
        self.assertFalse(steps.disc_current(ctx.layout, ctx.known))

    def test_the_hash_check_finds_a_flipped_byte_of_the_same_size(self):
        ctx = Stub(self.t, self.files)
        fk.write_folder(ctx.layout.disc, self.files)
        data = bytearray(self.files['BIN/ONE.BIN'])
        data[3] ^= 1
        (ctx.layout.disc / 'BIN' / 'ONE.BIN').write_bytes(bytes(data))
        self.assertEqual(steps.disc_rehash(ctx.layout, ctx.known), ['BIN/ONE.BIN'])


class SourceStepRun(Base):
    def raw(self, iso):
        out = bytearray()
        for i in range(len(iso) // 2048):
            out += disc.SYNC + b'\x00\x00\x00\x01' + iso[i * 2048:(i + 1) * 2048] + bytes(288)
        return bytes(out)

    def test_bin_cue_becomes_a_validated_iso_in_the_temp_folder(self):
        ctx = Stub(self.t, self.files)
        (self.t / 'g.bin').write_bytes(self.raw(ctx.image))
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n')
        ctx.source_kind, ctx.source_path = 'bin_cue', self.t / 'g.cue'
        out = steps.SourceStep().run(ctx)
        self.assertEqual(out, {'kind': 'bin_cue'})
        self.assertEqual(ctx.shared['iso'].read_bytes(), ctx.image)
        self.assertEqual(ctx.shared['temp_iso'], ctx.temp / 'disc.iso')
        self.assertEqual(ctx.state['region'], 'SCUS-97490')
        self.assertTrue(ctx.state['inputs']['disc']['validated'])
        self.assertNotIn('path', json.dumps(ctx.state['inputs']))        # the state never keeps a path to the image
        self.assertTrue([e for e in ctx.events() if e['phase'] == 'convert'])

    def test_a_bin_that_is_another_disc_is_refused_with_the_region(self):
        other = fk.fake_files(b'BOOT2 = cdrom0:\\SLUS_000.00;1\r\n')
        ctx = Stub(self.t, self.files)
        iso = fk.build_iso(other)
        (self.t / 'o.bin').write_bytes(self.raw(iso))
        (self.t / 'o.cue').write_text('FILE "o.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n')
        ctx.known['iso']['size'] = len(iso)
        ctx.known['iso']['sectors'] = len(iso) // 2048
        ctx.source_kind, ctx.source_path = 'bin_cue', self.t / 'o.cue'
        with self.assertRaises(SetupError) as cm:
            steps.SourceStep().run(ctx)
        self.assertEqual(cm.exception.id, 'err.disc_region')
        self.assertEqual(cm.exception.params['region'], 'USA')       # SLUS is USA, but not the supported serial
        self.assertEqual(cm.exception.params['serial'], 'SLUS-00000')

    def test_source_is_transient_and_never_current(self):
        ctx = Stub(self.t, self.files)
        s = steps.SourceStep()
        self.assertTrue(s.transient)
        self.assertFalse(s.current(ctx, {}))


class HdEvents(Base):
    def test_builder_events_become_progress(self):
        ctx = Stub(self.t, self.files)
        step = steps.TexturesStep()
        st = {'error': None}
        for ev in ({'ev': 'stage', 'name': 'tim2', 'state': 'started'}, {'ev': 'stage', 'name': 'tim2', 'state': 'done'},
                   {'ev': 'download', 'tool': 'realesrgan', 'state': 'progress', 'done': 5, 'total': 10},
                   {'ev': 'download', 'tool': 'realesrgan', 'state': 'unpacking'}, {'ev': 'download', 'tool': 'realesrgan', 'state': 'done'}):
            step.on_event(ctx, ev, st)
        got = [(e['phase'], e.get('done'), e.get('total')) for e in ctx.events()]
        self.assertEqual(got, [('stage_tim2', 0, 1), ('stage_tim2', 1, 1), ('download_realesrgan', 5, 10),
                               ('unpack_realesrgan', 0, 1), ('unpack_realesrgan', 1, 1)])
        step.on_event(ctx, {'ev': 'error', 'step': 'gpu', 'what': 'no Vulkan', 'hint': 'update the driver'}, st)
        self.assertEqual(st['error']['step'], 'gpu')

    def test_every_event_name_the_builder_prints_has_catalog_text(self):
        names = json.loads((fk.REPO / 'tools' / 'setup' / 'rgsetup' / 'text' / 'en.json').read_text(encoding='utf-8'))
        for sid in ('hd_textures', 'hd_movies'):
            for n in steps.HD_STAGES:
                self.assertIn(f'phase.{sid}.stage_{n}', names)

    def test_the_hd_keys_follow_the_tools_the_extraction_and_the_trial_limit(self):
        ctx = Stub(self.t, self.files)
        hd = ctx.root / 'tools' / 'hd'
        ctx.keys['extract'] = 'E1'
        k1 = steps.TexturesStep().key(ctx)
        self.assertEqual(k1, steps.TexturesStep().key(ctx))
        (hd / 'tool.py').write_text('print(2)\n')                       # the builder's code changed
        k2 = steps.TexturesStep().key(ctx)
        self.assertNotEqual(k1, k2)
        ctx.keys['extract'] = 'E2'
        self.assertNotEqual(k2, steps.TexturesStep().key(ctx))
        ctx.options.limit = 6
        k3 = steps.TexturesStep().key(ctx)
        ctx.options.limit = 0
        self.assertNotEqual(k3, steps.TexturesStep().key(ctx))           # a trial never counts as the real run
        self.assertNotEqual(steps.TexturesStep().key(ctx), steps.MoviesStep().key(ctx))

    def test_a_build_key_follows_the_release_the_sources_and_the_toolchain(self):
        ctx = Stub(self.t, self.files)
        ctx.keys['extract'] = 'E'
        ctx.source_stamp = ('S1', 0)
        k = steps.BuildStep().key(ctx)
        ctx.source_stamp = ('S2', 0)
        self.assertNotEqual(k, steps.BuildStep().key(ctx))
        ctx.source_stamp = ('S1', 0)
        ctx.layout.release = '9.9.9'
        self.assertNotEqual(k, steps.BuildStep().key(ctx))

    def test_the_extract_key_ignores_the_input_and_the_release(self):
        ctx = Stub(self.t, self.files)
        ctx.source_kind = 'iso'
        a = steps.ExtractStep().key(ctx)
        ctx.source_kind = 'chd'
        ctx.layout.release = '2.0.0'
        self.assertEqual(a, steps.ExtractStep().key(ctx))      # any valid copy of the disc is the same disc


class Pack(Base):
    def test_the_pack_check_samples_the_manifest(self):
        ctx = Stub(self.t, self.files)
        step = steps.TexturesStep()
        lay = ctx.layout
        self.assertFalse(step.current(ctx, {}))
        lay.replacements.mkdir(parents=True)
        names = [f'a{i:03d}.png' for i in range(60)]
        for n in names:
            (lay.replacements / n).write_bytes(b'x')
        (lay.textures / 'pack-manifest.csv').write_text('set,path\n' + ''.join(f'x,{n}\n' for n in names))
        self.assertTrue(step.current(ctx, {}))
        self.assertTrue(step.verify(ctx, {}))
        (lay.replacements / names[7]).unlink()
        self.assertFalse(step.verify(ctx, {}))           # --verify checks every file, the cheap check a sample
        self.assertEqual(steps.read_pack_manifest(lay.textures / 'nope.csv'), [])

    def test_the_movie_check_counts_files(self):
        ctx = Stub(self.t, self.files)
        step = steps.MoviesStep()
        ctx.movie_count = 3
        self.assertFalse(step.current(ctx, {}))
        for n in ('00/A', '01/B'):
            p = ctx.layout.movies / (n + '.mkv')
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b'x')
        self.assertFalse(step.current(ctx, {}))
        (ctx.layout.movies / '01' / 'C.mkv').write_bytes(b'x')
        self.assertTrue(step.current(ctx, {}))
        ctx.options.limit_movies = 1
        self.assertTrue(step.current(ctx, {}))


class PromptsFont(Base):
    def test_run_writes_only_to_the_given_settings_file(self):
        ctx = Stub(self.t, self.files, settings_file=self.t / 's' / 'settings.ini')
        out = steps.PromptsFontStep().run(ctx)
        self.assertEqual(out['added'], ['button_prompts', 'font_style'])
        self.assertEqual((self.t / 's' / 'settings.ini').read_text(), '[game]\nbutton_prompts = 1\nfont_style = 1\n')


class Build(Base):
    def setUp(self):
        super().setUp()
        from rgsetup import proc
        self.proc = proc
        self.calls = []
        self.old_run, self.old_gp = proc.run, hostcheck.game_processes

        def fake_run(command, on_line=None, log_path=None, env=None, cwd=None, gentle=False, **kw):
            self.calls.append((list(map(str, command)), env, cwd, gentle))
            if on_line:
                on_line('[10/10] Linking CXX executable x')
            return 0, []
        proc.run = fake_run
        self.addCleanup(setattr, proc, 'run', self.old_run)
        self.addCleanup(setattr, hostcheck, 'game_processes', self.old_gp)

    def ctx(self):
        ctx = Stub(self.t, self.files)
        for p in ('thirdparty/PS2Recomp/CMakeLists.txt', 'thirdparty/imgui/imgui.h'):
            f = ctx.root / p
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text('x')
        ctx.layout.game_exe.parent.mkdir(parents=True, exist_ok=True)
        ctx.layout.game_exe.write_bytes(b'x' * 10)
        ctx.layout.launcher.write_bytes(b'x')
        return ctx

    def test_the_build_runs_tools_recompile_build_in_the_toolchain_environment(self):
        hostcheck.game_processes = lambda d: []
        ctx = self.ctx()
        out = steps.BuildStep().run(ctx)
        self.assertEqual(out, {'exe_size': 10})
        subs = [c[0][3:] for c in self.calls]
        self.assertEqual(subs, [['tools'], ['recompile', '--disc', str(ctx.layout.disc)], ['build']])
        for command, env, cwd, gentle in self.calls:
            self.assertEqual(command[1], '-u')
            self.assertTrue(command[2].endswith('setup.py'))
            self.assertEqual(cwd, ctx.layout.root)
            self.assertTrue(env['PATH'].startswith(str(ctx.layout.toolchain / 'llvm-mingw' / 'bin')))
            self.assertNotIn('CC', env)
            self.assertEqual(env['PYTHONUTF8'], '1')
        compile_events = [e for e in ctx.events() if e['phase'] == 'compile' and e.get('total') == 10]
        self.assertEqual((compile_events[0]['done'], compile_events[0]['unit']), (10, 'files'))

    def test_the_recorded_key_is_the_one_of_the_tree_the_build_leaves(self):
        # the build applies patches/ to thirdparty/PS2Recomp: a second run on the same folder must not see a new key
        hostcheck.game_processes = lambda d: []
        ctx = self.ctx()
        (ctx.root / 'src').mkdir(exist_ok=True)
        (ctx.root / 'src' / 'a.cpp').write_text('one')
        ctx.source_stamp = steps.source_stamp(ctx.layout)
        before = steps.BuildStep().key(ctx)
        patched = ctx.root / 'src' / 'a.cpp'
        step = steps.BuildStep()
        original = self.proc.run

        def patching_run(command, **kw):
            patched.write_text('patched by the build')
            return original(command, **kw)
        self.proc.run = patching_run
        step.run(ctx)
        self.assertNotEqual(ctx.keys['build'], before)
        self.assertEqual(ctx.keys['build'], step.key(ctx))            # what the next run will compute

    def test_a_running_game_stops_the_build_before_anything_is_touched(self):
        hostcheck.game_processes = lambda d: [4242]
        ctx = self.ctx()
        with self.assertRaises(SetupError) as cm:
            steps.BuildStep().run(ctx)
        self.assertEqual(cm.exception.id, 'err.game_running')
        self.assertEqual(self.calls, [])

    def test_a_failed_phase_names_the_phase_and_the_log(self):
        hostcheck.game_processes = lambda d: []
        ctx = self.ctx()
        self.proc.run = lambda command, **kw: (1, ['boom'])
        with self.assertRaises(SetupError) as cm:
            steps.BuildStep().run(ctx)
        self.assertEqual((cm.exception.id, cm.exception.params['phase']), ('err.build_failed', 'tools'))

    def test_the_current_check_wants_both_executables(self):
        ctx = self.ctx()
        self.assertTrue(steps.BuildStep().current(ctx, {}))
        ctx.layout.launcher.unlink()
        self.assertFalse(steps.BuildStep().current(ctx, {}))

    def test_empty_framework_folders_without_git_are_explained(self):
        hostcheck.game_processes = lambda d: []
        ctx = self.ctx()
        (ctx.root / 'thirdparty' / 'imgui' / 'imgui.h').unlink()
        with self.assertRaises(SetupError) as cm:
            steps.BuildStep().run(ctx)
        self.assertEqual(cm.exception.id, 'err.sources_missing')

    def test_the_toolchain_step_knows_when_it_is_ready(self):
        from rgsetup import download
        ctx = Stub(self.t, self.files)
        step = steps.ToolchainStep()
        self.assertFalse(step.current(ctx, {}))
        for t in download.load_toolchain():
            d = ctx.layout.tool_dir(t['name'])
            (d / t['exe']).parent.mkdir(parents=True, exist_ok=True)
            (d / t['exe']).write_bytes(b'x')
            (d / '.rg-sha256').write_text(t['sha256'] + '\n')
        self.assertTrue(step.current(ctx, {}))
        (ctx.layout.tool_dir('cmake') / '.rg-sha256').write_text('0' * 64)       # a different pin: fetch again
        self.assertFalse(step.current(ctx, {}))


@unittest.skipUnless(os.name == 'nt', 'Windows processes')
class GameCheck(Base):
    def test_a_game_by_path_under_the_install_folder_is_found_and_the_toolchain_is_not(self):
        build = self.t / 'build' / 'game-dev'
        build.mkdir(parents=True)
        exe = build / 'rg-test.exe'
        shutil.copy(Path(os.environ['SystemRoot']) / 'System32' / 'PING.EXE', exe)
        other = self.t / 'toolchain' / 'python'
        other.mkdir(parents=True)
        tool = other / 'python.exe'
        shutil.copy(Path(os.environ['SystemRoot']) / 'System32' / 'PING.EXE', tool)
        flags = subprocess.CREATE_NO_WINDOW
        p1 = subprocess.Popen([str(exe), '-n', '40', '127.0.0.1'], stdout=subprocess.DEVNULL, creationflags=flags)
        p2 = subprocess.Popen([str(tool), '-n', '40', '127.0.0.1'], stdout=subprocess.DEVNULL, creationflags=flags)
        try:
            time.sleep(1.5)
            found = hostcheck.game_processes(self.t)
            self.assertIn(p1.pid, found)
            self.assertNotIn(p2.pid, found)
            self.assertEqual(hostcheck.game_processes(self.t / 'elsewhere'), [])
        finally:
            for p in (p1, p2):
                p.kill()
                p.wait()

    def test_a_game_of_that_name_running_from_another_folder_is_not_in_the_way(self):
        # the owner plays one copy while another is set up: only the one under the install folder blocks a relink
        other = self.t / 'player' / 'build' / 'game' / 'src'
        other.mkdir(parents=True)
        exe = other / 'roguegalaxy.exe'
        shutil.copy(Path(os.environ['SystemRoot']) / 'System32' / 'PING.EXE', exe)
        mine = self.t / 'install'
        mine.mkdir()
        p = subprocess.Popen([str(exe), '-n', '40', '127.0.0.1'], stdout=subprocess.DEVNULL,
                             creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            time.sleep(1.5)
            self.assertIn(p.pid, hostcheck.game_processes(self.t / 'player'))
            self.assertNotIn(p.pid, hostcheck.game_processes(mine))
        finally:
            p.kill()
            p.wait()


class Space(Base):
    def test_not_enough_space_is_an_error_naming_the_numbers(self):
        from rgsetup import cli
        ctx = Stub(self.t, self.files)
        ctx.runs = {}
        ctx.host = {'windows64': True, 'cpu': {'sse41': True}, 'dest_kind': 'in_place'}
        ctx.options.estimate = False
        ctx.options.verify = False
        ctx.skipped_components = []
        ctx.estimate = {'need': 100 << 30}
        ctx.temp_need = 1 << 30
        old = hostcheck.free_bytes
        hostcheck.free_bytes = lambda path: 10 << 30
        try:
            with self.assertRaises(SetupError) as cm:
                steps.CheckStep().run(ctx)
        finally:
            hostcheck.free_bytes = old
        self.assertEqual(cm.exception.id, 'err.space')
        self.assertEqual(cm.exception.params['free'], 10.0)
        sp = [e for e in ctx.events() if e['type'] == 'space'][0]
        self.assertEqual(sp['need'], 101 << 30)          # same drive: the temp need is included


if __name__ == '__main__':
    unittest.main()
