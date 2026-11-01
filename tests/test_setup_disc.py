import hashlib
import os
import tempfile
import unittest
from pathlib import Path

import setup_fakes as fk
from rgsetup import disc


def small_limit(n=3000):
    """The fake disc's DATA0_0.DAT is bigger than this, so it is checked by size only, as the real big files are."""
    old = disc.QUICK_LIMIT
    disc.QUICK_LIMIT = n
    return old


class FolderValidation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.files = fk.fake_files()
        self.known = fk.known_for(self.files)
        self.root = Path(self.tmp.name) / 'dump'
        fk.write_folder(self.root, self.files)
        self.old = small_limit()
        self.addCleanup(setattr, disc, 'QUICK_LIMIT', self.old)

    def test_a_complete_folder_passes(self):
        info = disc.validate_folder(self.root, self.known)
        self.assertEqual(info['serial'], 'SCUS-97490')
        self.assertEqual(info['files'], 5)
        self.assertEqual(set(info['map']), set(self.files))
        self.assertEqual(info['hashed'], 3)       # the two big files (the ELF here too) are checked by size

    def test_wrong_case_and_version_suffix_are_tolerated(self):
        (self.root / 'bin').rename(self.root / 'bin_tmp')
        (self.root / 'bin_tmp').rename(self.root / 'Bin')
        (self.root / 'Bin' / 'ONE.BIN').rename(self.root / 'Bin' / 'one.bin;1')
        self.assertEqual(disc.validate_folder(self.root, self.known)['files'], 5)

    def test_the_disc_inside_one_subfolder_is_found(self):
        outer = Path(self.tmp.name) / 'outer'
        outer.mkdir()
        fk.write_folder(outer / 'Rogue Galaxy', self.files)
        info = disc.validate_folder(outer, self.known)
        self.assertEqual(info['root'], outer / 'Rogue Galaxy')

    def test_two_candidate_subfolders_are_not_guessed(self):
        outer = Path(self.tmp.name) / 'outer'
        fk.write_folder(outer / 'a', self.files)
        fk.write_folder(outer / 'b', self.files)
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(outer, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_folder_not_disc')

    def test_not_a_disc_folder(self):
        empty = Path(self.tmp.name) / 'empty'
        empty.mkdir()
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(empty, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_folder_not_disc')

    def test_partial_dump_gives_one_message_with_counts(self):
        (self.root / 'MOVIE' / 'A.MVI').unlink()
        (self.root / 'BIN' / 'ONE.BIN').write_bytes(b'wrong')
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(self.root, self.known)
        e = cm.exception
        self.assertEqual(e.id, 'err.disc_mismatch')
        self.assertEqual(e.params['count'], 2)
        self.assertEqual(e.params['missing'], 1)
        self.assertEqual(e.params['wrong'], 1)
        self.assertIn('MOVIE/A.MVI', e.params['first'])

    def test_a_changed_byte_in_a_small_file_is_caught_by_hash(self):
        data = bytearray(self.files['BIN/ONE.BIN'])
        data[100] ^= 1
        (self.root / 'BIN' / 'ONE.BIN').write_bytes(bytes(data))
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(self.root, self.known)
        self.assertIn('hash: BIN/ONE.BIN', cm.exception.params['first'])

    def test_a_truncated_big_file_is_caught_by_size(self):
        (self.root / 'DATA0_0.DAT').write_bytes(self.files['DATA0_0.DAT'][:-1])
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(self.root, self.known)
        self.assertIn('size: DATA0_0.DAT', cm.exception.params['first'])

    def test_another_region_is_named(self):
        files = fk.fake_files(b'BOOT2 = cdrom0:\\SLES_545.67;1\r\n')
        root = Path(self.tmp.name) / 'pal'
        fk.write_folder(root, files)
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(root, fk.known_for(files))
        self.assertEqual(cm.exception.id, 'err.disc_region')
        self.assertEqual(cm.exception.params['serial'], 'SLES-54567')
        self.assertEqual(cm.exception.params['region'], 'Europe')
        self.assertEqual(cm.exception.params['expected'], 'SCUS-97490')

    def test_a_cnf_without_a_boot_line_is_not_ps2(self):
        files = fk.fake_files(b'hello\r\n')
        root = Path(self.tmp.name) / 'x'
        fk.write_folder(root, files)
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.validate_folder(root, fk.known_for(files))
        self.assertEqual(cm.exception.id, 'err.disc_not_ps2')

    def test_progress_counts_the_hashed_files(self):
        seen = []
        disc.validate_folder(self.root, self.known, lambda d, t: seen.append((d, t)))
        self.assertEqual(seen[-1], (3, 3))


class IsoValidation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.files = fk.fake_files()
        self.image = fk.build_iso(self.files)
        self.known = fk.known_for(self.files, iso_size=len(self.image), sectors=len(self.image) // 2048)
        self.path = Path(self.tmp.name) / 'fake.iso'
        self.path.write_bytes(self.image)
        self.old = small_limit()
        self.addCleanup(setattr, disc, 'QUICK_LIMIT', self.old)

    def test_the_synthetic_iso_is_valid(self):
        info = disc.identify_iso(self.path, self.known)
        self.assertEqual(info['serial'], 'SCUS-97490')
        self.assertEqual(info['files'], 5)

    def test_a_flipped_byte_in_a_file_fails(self):
        bad = bytearray(self.image)
        idx = bad.find(bytes(fk.fake_files()['BIN/ONE.BIN'][:40])) + 100
        bad[idx] ^= 0xFF
        self.path.write_bytes(bytes(bad))
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.identify_iso(self.path, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_mismatch')

    def test_a_truncated_image_fails_by_size(self):
        self.path.write_bytes(self.image[:-4096])      # the last file (SYSTEM.CNF here) loses its sector
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.identify_iso(self.path, self.known)
        self.assertIn(cm.exception.id, ('err.disc_mismatch', 'err.disc_not_ps2'))

    def test_not_an_iso(self):
        self.path.write_bytes(b'\0' * 100000)
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.identify_iso(self.path, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_not_iso')

    def test_another_regions_iso(self):
        files = fk.fake_files(b'BOOT2 = cdrom0:\\SLPM_667.12;1\r\n')
        path = Path(self.tmp.name) / 'jp.iso'
        path.write_bytes(fk.build_iso(files))
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.identify_iso(path, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_region')
        self.assertEqual(cm.exception.params['region'], 'Japan')

    def test_whole_image_hash(self):
        self.known['iso'].update({'crc32': format(__import__('zlib').crc32(self.image), '08x'),
                                  'md5': hashlib.md5(self.image).hexdigest(), 'sha1': hashlib.sha1(self.image).hexdigest()})
        disc.verify_image_hash(self.path, self.known)
        self.known['iso']['md5'] = '0' * 32
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.verify_image_hash(self.path, self.known)
        self.assertEqual(cm.exception.id, 'err.disc_image_hash')


class Sniffing(unittest.TestCase):
    def test_kinds(self):
        with tempfile.TemporaryDirectory(prefix='rgtest-') as t:
            t = Path(t)
            for name in ('a.iso', 'b.ISO', 'c.cue', 'd.bin', 'e.chd'):
                (t / name).write_bytes(b'x')
            kinds = {n: disc.sniff(t / n) for n in ('a.iso', 'b.ISO', 'c.cue', 'd.bin', 'e.chd')}
            self.assertEqual(kinds, {'a.iso': 'iso', 'b.ISO': 'iso', 'c.cue': 'bin_cue', 'd.bin': 'bin_cue', 'e.chd': 'chd'})
            self.assertEqual(disc.sniff(t), 'folder')
            (t / 'noext').write_bytes(b'MComprHD' + b'\0' * 100)
            self.assertEqual(disc.sniff(t / 'noext'), 'chd')
            (t / 'blob').write_bytes(b'\0' * 100)
            with self.assertRaises(disc.DiscProblem) as cm:
                disc.sniff(t / 'blob')
            self.assertEqual(cm.exception.id, 'err.source_type')
            with self.assertRaises(disc.DiscProblem) as cm:
                disc.sniff(t / 'nothere.iso')
            self.assertEqual(cm.exception.id, 'err.source_missing')

    def test_serials_and_regions(self):
        self.assertEqual(disc.serial_from_cnf('BOOT2 = cdrom0:\\SCUS_974.90;1'), ('SCUS-97490', 'SCUS_974.90'))
        self.assertEqual(disc.serial_from_cnf('BOOT2=cdrom0:SLES_545.67;1'), ('SLES-54567', 'SLES_545.67'))
        self.assertIsNone(disc.serial_from_cnf('nothing'))
        self.assertEqual(disc.region_of('SCUS-97490'), 'USA')
        self.assertEqual(disc.region_of('SLPS-25000'), 'Japan')
        self.assertEqual(disc.region_of('ABCD-00000'), 'unknown')

    def test_the_real_hashes_file_describes_the_usa_disc(self):
        known = disc.load_known(fk.REPO / 'config' / 'SCUS_974.90' / 'hashes.json')
        self.assertEqual(known['serial'], 'SCUS-97490')
        self.assertEqual(len(known['files']), 124)
        self.assertRegex(disc.known_digest(known), r'^[0-9a-f]{64}$')


class BinCue(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgtest-')
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.iso = fk.build_iso(fk.fake_files())

    def raw(self, mode_offset=16, size=2352):
        out = bytearray()
        for i in range(len(self.iso) // 2048):
            sector = bytearray(size)
            sector[:12] = disc.SYNC
            sector[mode_offset:mode_offset + 2048] = self.iso[i * 2048:(i + 1) * 2048]
            out += sector
        return bytes(out)

    def known(self):
        return {'iso': {'size': len(self.iso), 'sectors': len(self.iso) // 2048}}

    def test_mode1_2352_is_converted_to_the_same_iso(self):
        (self.t / 'g.bin').write_bytes(self.raw())
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')
        plan = disc.bincue_plan(self.t / 'g.cue', self.known())
        self.assertEqual(plan[0], 'convert')
        out = disc.convert_bincue(plan, self.t / 'out' / 'g.iso')
        self.assertEqual(out.read_bytes(), self.iso)

    def test_mode2_2352_data_starts_at_24(self):
        (self.t / 'g.bin').write_bytes(self.raw(24))
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n')
        out = disc.convert_bincue(disc.bincue_plan(self.t / 'g.cue', self.known()), self.t / 'g.iso')
        self.assertEqual(out.read_bytes(), self.iso)

    def test_mode1_2048_bin_is_already_the_iso(self):
        (self.t / 'g.bin').write_bytes(self.iso)
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n')
        plan = disc.bincue_plan(self.t / 'g.cue', self.known())
        self.assertEqual(plan, ('iso', self.t / 'g.bin'))

    def test_lone_bin_is_recognised_by_its_size(self):
        (self.t / 'raw.bin').write_bytes(self.raw())
        plan = disc.bincue_plan(self.t / 'raw.bin', self.known())
        self.assertEqual(plan[0], 'convert')
        self.assertEqual(plan[2:4], (2352, 16))
        (self.t / 'iso.bin').write_bytes(self.iso)
        self.assertEqual(disc.bincue_plan(self.t / 'iso.bin', self.known())[0], 'iso')
        (self.t / 'odd.bin').write_bytes(b'\0' * 12345)
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.bincue_plan(self.t / 'odd.bin', self.known())
        self.assertEqual(cm.exception.id, 'err.disc_bin_size')

    def test_audio_tracks_after_the_data_track_are_left_out(self):
        (self.t / 'g.bin').write_bytes(self.raw() + b'\0' * 2352 * 10)
        n = len(self.iso) // 2048
        mm, ss, ff = n // 75 // 60, n // 75 % 60, n % 75
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n'
                                      f'TRACK 02 AUDIO\nINDEX 01 {mm:02d}:{ss:02d}:{ff:02d}\n')
        plan = disc.bincue_plan(self.t / 'g.cue', self.known())
        self.assertEqual(plan[5], n)
        self.assertEqual(disc.convert_bincue(plan, self.t / 'g.iso').read_bytes(), self.iso)

    def test_a_damaged_sector_is_reported(self):
        raw = bytearray(self.raw())
        raw[2352 * 3 + 5] = 0x12        # a sector whose sync pattern is broken
        (self.t / 'g.bin').write_bytes(bytes(raw))
        (self.t / 'g.cue').write_text('FILE "g.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n')
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.convert_bincue(disc.bincue_plan(self.t / 'g.cue', self.known()), self.t / 'g.iso')
        self.assertEqual(cm.exception.id, 'err.disc_bin_sector')
        self.assertEqual(cm.exception.params['sector'], 3)
        self.assertFalse((self.t / 'g.iso').exists())      # no half-written ISO under the real name

    def test_cue_problems(self):
        (self.t / 'a.cue').write_text('nonsense\n')
        with self.assertRaises(disc.DiscProblem) as cm:
            disc.parse_cue(self.t / 'a.cue')
        self.assertEqual(cm.exception.id, 'err.disc_cue')
        (self.t / 'b.cue').write_text('FILE "missing.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n')
        with self.assertRaises(disc.DiscProblem):
            disc.parse_cue(self.t / 'b.cue')
        (self.t / 'c.bin').write_bytes(b'x')
        (self.t / 'c.cue').write_text('FILE "c.bin" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
        with self.assertRaises(disc.DiscProblem):
            disc.parse_cue(self.t / 'c.cue')


if __name__ == '__main__':
    unittest.main()
