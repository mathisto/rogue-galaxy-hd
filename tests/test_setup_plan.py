import unittest

import setup_fakes  # noqa: F401
from rgsetup import plan

GiB = 1 << 30


class Presets(unittest.TestCase):
    def test_the_three_presets(self):
        self.assertEqual(plan.PRESETS['faithful'], ())
        self.assertEqual(plan.PRESETS['recommended'], ('textures', 'prompts_font'))
        self.assertEqual(plan.PRESETS['everything'], ('textures', 'movies', 'prompts_font'))

    def test_a_fresh_install_follows_the_preset_or_the_components(self):
        self.assertEqual(plan.resolve_components('faithful'), ())
        self.assertEqual(plan.resolve_components('everything'), ('textures', 'movies', 'prompts_font'))
        self.assertEqual(plan.resolve_components(None), plan.PRESETS['recommended'])        # the default
        self.assertEqual(plan.resolve_components('everything', 'movies'), ('movies',))      # components replace the preset
        self.assertEqual(plan.resolve_components(None, 'prompts_font,textures'), ('textures', 'prompts_font'))  # canonical order

    def test_an_existing_install_only_gains_components(self):
        have = ('textures', 'prompts_font')
        self.assertEqual(plan.resolve_components(None, None, have), have)
        self.assertEqual(plan.resolve_components(None, 'movies', have), ('textures', 'movies', 'prompts_font'))
        self.assertEqual(plan.resolve_components('everything', None, ()), ('textures', 'movies', 'prompts_font'))
        self.assertEqual(plan.resolve_components('faithful', None, have), have)              # never removes one

    def test_unknown_component(self):
        with self.assertRaises(plan.PlanError):
            plan.parse_components('textures,models')
        self.assertEqual(plan.parse_components(' textures , '), ('textures',))
        self.assertIsNone(plan.parse_components(None))

    def test_preset_of(self):
        self.assertEqual(plan.preset_of(()), 'faithful')
        self.assertEqual(plan.preset_of(('prompts_font', 'textures')), 'recommended')
        self.assertEqual(plan.preset_of(('movies',)), 'custom')

    def test_no_3d_models_component(self):
        self.assertEqual(set(plan.COMPONENTS), {'textures', 'movies', 'prompts_font'})


class Numbers(unittest.TestCase):
    def test_hd_disk_adds_when_work_is_kept(self):
        peak, kept = plan.hd_disk(['textures', 'movies'], clean_work=False)
        self.assertEqual(peak, 22 * GiB)
        self.assertEqual(kept, 22 * GiB)

    def test_hd_disk_with_clean_work_peaks_at_one_plus_the_finished(self):
        peak, kept = plan.hd_disk(['textures', 'movies'], clean_work=True)
        self.assertEqual(peak, 6 * GiB + 11 * GiB)          # textures' result stays while the movies are built
        self.assertEqual(kept, 6 * GiB + 7 * GiB)

    def test_hd_disk_of_nothing(self):
        self.assertEqual(plan.hd_disk([]), (0, 0))

    def test_estimate_of_a_whole_install(self):
        e = plan.estimate(('textures', 'movies', 'prompts_font'))
        self.assertEqual(e['need'], (7 + 9 + 22 + 3) * GiB)
        self.assertEqual(e['final'], (7 + 9 + 22) * GiB)
        self.assertEqual(e["minutes_min"], 1 + 20 + 20 + 90)
        self.assertEqual(e["minutes_max"], 6 + 60 + 30 + 90)

    def test_estimate_counts_only_what_is_left(self):
        e = plan.estimate(('textures', 'movies', 'prompts_font'), extract=False, build=False, hd_pending=['movies'])
        self.assertEqual(e['need'], (11 + 3) * GiB)
        e = plan.estimate(('prompts_font',), extract=False, build=False)
        self.assertEqual(e['need'], 0)
        self.assertEqual((e['minutes_min'], e['minutes_max']), (0, 0))

    def test_faithful_needs_no_hd_space(self):
        e = plan.estimate(())
        self.assertEqual(e['need'], (7 + 9 + 3) * GiB)
        self.assertEqual(e['peak_hd'], 0)

    def test_conversion_adds_time(self):
        a = plan.estimate((), conversion=False)
        b = plan.estimate((), conversion=True)
        self.assertGreater(b['minutes_max'], a['minutes_max'])

    def test_each_component_reports_its_cost_for_the_window(self):
        e = plan.estimate(())
        self.assertEqual(set(e['per_component']), {'textures', 'movies', 'prompts_font'})
        self.assertGreater(e['per_component']['movies']['minutes_max'], e['per_component']['textures']['minutes_max'])
        self.assertEqual(e['per_component']['prompts_font']['peak'], 0)

    def test_numbers_match_the_hd_builder_where_it_has_them(self):
        hd = plan.hd_module(str(setup_fakes.REPO))
        self.assertEqual(plan.COMPONENTS['textures']['peak'], hd.GB_TEXTURES * GiB)
        self.assertEqual(plan.COMPONENTS['movies']['peak'], hd.GB_MOVIES * GiB)

    def test_download_specs_come_from_the_builder(self):
        from rgsetup import download
        specs = plan.download_specs(str(setup_fakes.REPO), download.CHDMAN)
        self.assertEqual(set(specs), {'realesrgan', 'ffmpeg', 'pylibs', 'chdman'})
        for key in ('realesrgan', 'ffmpeg', 'chdman'):
            self.assertRegex(specs[key]['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(specs[key]['url'].startswith('https://'))
            self.assertTrue(specs[key]['license'])


if __name__ == '__main__':
    unittest.main()
