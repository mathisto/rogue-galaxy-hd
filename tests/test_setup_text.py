"""The text catalog is complete: every message id the code can emit is in text/en.json, with the placeholders it fills."""
import ast
import json
import re
import string
import unittest
from pathlib import Path

import setup_fakes as fk
from rgsetup import events, plan, steps

PKG = fk.REPO / 'tools' / 'setup' / 'rgsetup'
CATALOG = json.loads((PKG / 'text' / 'en.json').read_text(encoding='utf-8'))
ID_RE = re.compile(r'^(?:err|warn|msg|reason|component|preset)\.[a-z0-9_.]+$')


def py_files():
    return sorted(PKG.glob('*.py'))


def placeholders(text):
    return {name for _, name, _, _ in string.Formatter().parse(text) if name}


class Collect(ast.NodeVisitor):
    """Every message id in the code, with the parameter names given where they are known."""
    def __init__(self):
        self.ids = {}            # id -> list of param-name sets (None: dynamic)
        self.progress = set()    # (step, phase)

    def add(self, id, params):
        self.ids.setdefault(id, []).append(params)

    def visit_Call(self, node):
        name = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, 'id', '')
        first = node.args[0] if node.args else None
        if name in ('SetupError', 'DiscProblem', 'warning', 'error') and isinstance(first, ast.Constant) and isinstance(first.value, str):
            if any(k.arg is None for k in node.keywords):
                params = None
            else:
                params = {k.arg for k in node.keywords}
            if name == 'error' and len(node.args) > 1 and isinstance(node.args[1], ast.Dict):
                params = {k.value for k in node.args[1].keys if isinstance(k, ast.Constant)}
            elif name == 'error' and len(node.args) > 1:
                params = None
            self.add(first.value, params)
        if name in ('progress', 'progress_writer') and len(node.args) >= 2:
            a, b = node.args[0], node.args[1]
            if name == 'progress_writer':
                a, b = node.args[1], node.args[2]
            if isinstance(b, ast.Constant) and isinstance(b.value, str):
                step = a.value if isinstance(a, ast.Constant) else None
                self.progress.add((step, b.value))
        self.generic_visit(node)

    def visit_Constant(self, node):
        if isinstance(node.value, str) and ID_RE.match(node.value):
            self.ids.setdefault(node.value, [])


def collect():
    c = Collect()
    for f in py_files():
        c.visit(ast.parse(f.read_text(encoding='utf-8'), str(f)))
    return c


class Catalog(unittest.TestCase):
    def test_every_id_used_in_the_code_is_in_the_catalog(self):
        c = collect()
        missing = sorted(i for i in c.ids if i not in CATALOG)
        self.assertEqual(missing, [])

    def test_each_message_has_the_placeholders_its_call_site_fills(self):
        c = collect()
        problems = []
        for id, sites in c.ids.items():
            if id not in CATALOG:
                continue
            need = placeholders(CATALOG[id])
            for params in sites:
                if params is not None and not need <= params:
                    problems.append(f'{id}: the text needs {sorted(need - params)} that a call does not pass ({sorted(params)})')
        self.assertEqual(problems, [])

    def test_every_step_has_a_title(self):
        for s in steps.all_steps():
            self.assertIn('step.' + s.id, CATALOG)

    def test_every_progress_phase_has_text(self):
        c = collect()
        step_ids = {s.id for s in steps.all_steps()}
        missing = []
        for step, phase in sorted(c.progress, key=str):
            if step in step_ids:
                if f'phase.{step}.{phase}' not in CATALOG:
                    missing.append(f'phase.{step}.{phase}')
            elif not any(f'phase.{sid}.{phase}' in CATALOG for sid in step_ids):    # progress(self.id, ...): some step
                missing.append(f'phase.<step>.{phase}')
        self.assertEqual(sorted(set(missing)), [])

    def test_the_builders_stage_names_have_text_for_both_hd_steps(self):
        for sid in ('hd_textures', 'hd_movies'):
            for stage in steps.HD_STAGES:
                self.assertIn(f'phase.{sid}.stage_{stage}', CATALOG)
            for tool in steps.HD_TOOLS:
                self.assertIn(f'phase.{sid}.download_{tool}', CATALOG)
                self.assertIn(f'phase.{sid}.unpack_{tool}', CATALOG)

    def test_the_builders_stage_names_are_the_ones_it_reports(self):
        src = (fk.REPO / 'tools' / 'hd' / 'build_hd_pack.py').read_text(encoding='utf-8')
        reported = set(re.findall(r"evt\(ev='stage', name='(\w+)'", src)) | set(re.findall(r"step\('(\w+)',", src))
        self.assertLessEqual(reported, set(steps.HD_STAGES), reported - set(steps.HD_STAGES))

    def test_every_component_preset_and_state_has_text(self):
        for c in plan.COMPONENTS:
            self.assertIn(f'component.{c}', CATALOG)
            self.assertIn(f'component.{c}.desc', CATALOG)
        for p in list(plan.PRESETS) + ['custom']:
            self.assertIn(f'preset.{p}', CATALOG)
        for p in plan.PRESETS:
            self.assertIn(f'preset.{p}.desc', CATALOG)
        for k in ('state.started', 'state.done', 'state.skipped', 'reason.current', 'reason.not_needed',
                  'reason.not_chosen', 'result.ok', 'result.failed', 'result.cancelled'):
            self.assertIn(k, CATALOG)

    def test_no_message_in_the_catalog_is_orphaned(self):
        c = collect()
        used = set(c.ids)
        dynamic = ('phase.', 'step.', 'component.', 'preset.')
        orphans = [i for i in CATALOG if not i.startswith('_') and i not in used and not i.startswith(dynamic)
                   and i not in ('state.started', 'state.done', 'state.skipped', 'result.ok', 'result.failed',
                                 'result.cancelled', 'reason.current', 'reason.not_needed', 'reason.not_chosen')]
        self.assertEqual(orphans, [])

    def test_player_text_has_no_code_words_or_em_dashes(self):
        for id, text in CATALOG.items():
            if id.startswith('_'):
                continue
            self.assertNotIn('—', text, id)
            self.assertTrue(text.strip() == text and text, id)

    def test_render_of_every_message_works_with_dummy_parameters(self):
        for id, text in CATALOG.items():
            if id.startswith('_'):
                continue
            params = {name: 'x' for name in placeholders(text)}
            self.assertNotIn('{', events.render(CATALOG, id, params), id)


if __name__ == '__main__':
    unittest.main()
