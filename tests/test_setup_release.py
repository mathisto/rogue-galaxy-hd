"""The release's content scanner (tools/check_content.py) on synthetic folders: nothing here is game data."""
import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

import setup_fakes as fk  # noqa: F401  (puts tools/ on sys.path)
import check_content as cc


class Args:
    def __init__(self, folder, disc=None, extra=None):
        self.folder, self.disc, self.extra = str(folder), disc, extra


class Scanner(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rgscan-')
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.rel = self.t / 'rel'
        self.rel.mkdir()
        self.disc = self.t / 'disc'
        (self.disc / 'DIR').mkdir(parents=True)
        rnd = bytes((i * 31 + i // 7) % 253 for i in range(3 << 20))
        (self.disc / 'DIR' / 'BIG.DAT').write_bytes(rnd)
        self.rnd = rnd
        # the "known disc": one 8 KiB file's size and hash
        self.known = self.t / 'hashes.json'
        self.piece = rnd[1000:1000 + 8192]
        self.known.write_text(json.dumps({'files': {'A.BIN': {'size': len(self.piece), 'sha256': hashlib.sha256(self.piece).hexdigest()}}}))
        self.old = cc.HASHES
        cc.HASHES = self.known
        self.addCleanup(lambda: setattr(cc, 'HASHES', self.old))

    def scan(self, **kw):
        n, bad, needles = cc.check(Args(self.rel, **kw))
        return bad

    def test_clean_folder_passes(self):
        (self.rel / 'tools').mkdir()
        (self.rel / 'tools' / 'a.py').write_text('print(1)\n')
        (self.rel / 'toolchain').mkdir()
        (self.rel / 'toolchain' / 'x.dat').write_bytes(b'third party, not the game')
        self.assertEqual(self.scan(disc=str(self.disc)), [])

    def test_game_file_types_and_install_folders_fail(self):
        (self.rel / 'disc').mkdir()
        (self.rel / 'disc' / 'readme.txt').write_text('x')
        (self.rel / 'game.iso').write_bytes(b'x')
        (self.rel / 'MOVIE.MVI').write_bytes(b'x')
        (self.rel / 'rg-install.json').write_text('{}')
        bad = ' | '.join(self.scan())
        for name in ('disc', 'game.iso', 'MOVIE.MVI', 'rg-install.json'):
            self.assertIn(name, bad)

    def test_a_disc_file_under_another_name_fails_by_hash(self):
        (self.rel / 'innocent.txt').write_bytes(self.piece)
        self.assertIn('byte for byte', ' '.join(self.scan()))

    def test_a_cut_out_piece_fails_with_the_disc_given(self):
        pos = len(self.rnd) * 3 // 7
        (self.rel / 'bigger.bin.txt').write_bytes(b'header' * 100 + self.rnd[pos - 50:pos + 100] + b'tail')
        self.assertEqual(self.scan(), [])                       # without the disc it cannot be known
        self.assertIn('BIG.DAT', ' '.join(self.scan(disc=str(self.disc))))

    def test_personal_names_and_paths_fail_in_ascii_and_utf16(self):
        user = os.environ.get('USERNAME') or os.environ.get('USER') or 'someone'
        (self.rel / 'a.txt').write_text(f'built by {user} here')
        (self.rel / 'b.bin.txt').write_bytes(f'C:\\Users\\{user}\\x'.encode('utf-16-le'))
        (self.rel / 'c.txt').write_text('mail me at nobody.example')
        bad = ' | '.join(self.scan(extra=['nobody.example']))
        self.assertIn('a.txt', bad)
        self.assertIn('b.bin.txt', bad)
        self.assertIn('c.txt', bad)

    def test_the_repository_itself_is_clean(self):
        class R:
            folder, disc, extra = None, None, None
        n, bad, _ = cc.check(R)
        self.assertEqual(bad, [])
        self.assertGreater(n, 100)


if __name__ == '__main__':
    unittest.main()
