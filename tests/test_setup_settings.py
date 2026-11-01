import os
import tempfile
import unittest
from pathlib import Path

import setup_fakes  # noqa: F401
from rgsetup import settings

DEFAULTS = {'button_prompts': '1', 'font_style': '1'}


class Defaults(unittest.TestCase):
    def test_the_only_keys_written_are_the_two(self):
        self.assertEqual(settings.PROMPTS_FONT, DEFAULTS)

    def test_a_missing_file_gets_a_game_section_with_the_two_lines(self):
        new, added = settings.apply_defaults('', DEFAULTS)
        self.assertEqual(new, '[game]\nbutton_prompts = 1\nfont_style = 1\n')
        self.assertEqual(added, ['button_prompts', 'font_style'])

    def test_keys_the_player_set_are_never_overwritten(self):
        text = '[game]\nfast_forward_speed = 4\nbutton_prompts = 0\nfont_style = 0\n'
        new, added = settings.apply_defaults(text, DEFAULTS)
        self.assertEqual(new, text)
        self.assertEqual(added, [])

    def test_only_the_absent_key_is_added_inside_game_before_the_next_section(self):
        text = '[window]\nwidth = 1280\n\n[game]\nfast_forward_speed = 4\nfont_style = 0\n\n[sound]\nvolume = 80\n'
        new, added = settings.apply_defaults(text, DEFAULTS)
        self.assertEqual(added, ['button_prompts'])
        self.assertEqual(new, '[window]\nwidth = 1280\n\n[game]\nfast_forward_speed = 4\nfont_style = 0\n'
                              'button_prompts = 1\n\n[sound]\nvolume = 80\n')

    def test_a_key_in_another_section_does_not_count(self):
        text = '[interface]\nbutton_prompts = 0\n'
        new, added = settings.apply_defaults(text, DEFAULTS)
        self.assertEqual(added, ['button_prompts', 'font_style'])
        self.assertTrue(new.startswith('[interface]\nbutton_prompts = 0\n'))
        self.assertTrue(new.endswith('[game]\nbutton_prompts = 1\nfont_style = 1\n'))

    def test_line_endings_and_everything_else_are_kept_byte_for_byte(self):
        text = '; comment\r\n[window]\r\nwidth = 1\r\n\r\n[game]\r\nsixty_fps = 1\r\n'
        new, _ = settings.apply_defaults(text, DEFAULTS)
        self.assertEqual(new, '; comment\r\n[window]\r\nwidth = 1\r\n\r\n[game]\r\nsixty_fps = 1\r\n'
                              'button_prompts = 1\r\nfont_style = 1\r\n')

    def test_present_keys_ignores_comments_and_other_sections(self):
        text = '[game]\n; button_prompts = 1\n# font_style = 1\nsixty_fps = 1\n[x]\nfont_style = 0\n'
        self.assertEqual(settings.present_keys(text), {'sixty_fps': '1'})

    def test_the_game_parser_accepts_the_result(self):
        # The game reads "key = value" lines of [game] (ui_settings.cpp parseSettings): numbers 0..2 and 0..1.
        new, _ = settings.apply_defaults('', DEFAULTS)
        got = settings.present_keys(new)
        self.assertEqual({k: int(v) for k, v in got.items()}, {'button_prompts': 1, 'font_style': 1})

    def test_write_defaults_creates_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix='rgtest-') as d:
            path = Path(d) / 'deep' / 'settings.ini'
            self.assertEqual(settings.write_defaults(path, DEFAULTS), ['button_prompts', 'font_style'])
            first = path.read_bytes()
            self.assertEqual(settings.write_defaults(path, DEFAULTS), [])
            self.assertEqual(path.read_bytes(), first)
            self.assertFalse(list(path.parent.glob('*.setup-tmp')))

    def test_an_unchanged_file_is_not_even_rewritten(self):
        with tempfile.TemporaryDirectory(prefix='rgtest-') as d:
            path = Path(d) / 'settings.ini'
            path.write_bytes(b'[game]\r\nbutton_prompts = 2\r\nfont_style = 0\r\n')
            os.utime(path, (1_000_000_000, 1_000_000_000))
            self.assertEqual(settings.write_defaults(path, DEFAULTS), [])
            self.assertEqual(path.stat().st_mtime, 1_000_000_000)

    def test_path_resolution(self):
        old = os.environ.get('RG_SETTINGS_FILE')
        try:
            os.environ['RG_SETTINGS_FILE'] = 'X:/a/b.ini'
            self.assertEqual(settings.settings_path(), Path('X:/a/b.ini'))
            self.assertEqual(settings.settings_path('Y:/c.ini'), Path('Y:/c.ini'))
            del os.environ['RG_SETTINGS_FILE']
            self.assertTrue(str(settings.settings_path()).endswith(os.path.join('RogueGalaxy', 'Recompiled', 'settings.ini')))
        finally:
            if old is not None:
                os.environ['RG_SETTINGS_FILE'] = old


if __name__ == '__main__':
    unittest.main()
