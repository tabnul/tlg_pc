# SPDX-License-Identifier: GPL-2.0-or-later
"""The switch table of the Qt launcher (launcher/backports_table.py) only offers what THIS port's
game really reads: every tlg.ini key is parsed by gpu/shim/tlg_settings.cpp and every TLG_* switch
is read somewhere in port/src or port/gpu. The switches inFAMOUS has and The Last Guardian does not
must never come back."""
import re
import sys
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import backports_table as bt  # noqa: E402
import tlg_run  # noqa: E402
from qt import gfxlogic  # noqa: E402

DEAD = ('TLG_OCCLUSION_MODE', 'TLG_LDS_MULTI', 'TLG_LDS_READLANE', 'TLG_LANE_DIVERGENCE', 'TLG_VERBOSE_LOG',
        'TLG_RENDERER', 'TLG_NATIVE_PASSES', 'TLG_WQM', 'TLG_LDS_SMALL', 'TLG_DYNAMIC_STRIDE', 'TLG_BC_TINY',
        'TLG_NO_MSAA', 'TLG_RFL_ALL', 'TLG_BARRIER_MERGE', 'TLG_MOUSE_PAD')


def sources():
    """Text of the port's own C/C++ (third-party trees left out)."""
    out = []
    for folder in (ROOT / 'src', ROOT / 'gpu'):
        for path in folder.rglob('*'):
            if path.suffix in ('.c', '.cpp', '.h', '.hpp') and 'third_party' not in path.parts and path.is_file():
                out.append(path.read_text(encoding='utf-8', errors='replace'))
    return '\n'.join(out)


SOURCES = sources() if (ROOT / 'gpu' / 'shim').is_dir() else ''
SETTINGS_CPP = (ROOT / 'gpu' / 'shim' / 'tlg_settings.cpp')


@unittest.skipUnless(SOURCES, 'GPU sources not present')
class TableMatchesTheSourcesTest(unittest.TestCase):
    def test_every_ini_key_is_parsed_by_the_game(self):
        text = SETTINGS_CPP.read_text(encoding='utf-8')
        for item in bt.ITEMS:
            if item.store == 'ini':
                self.assertRegex(text, r'"%s"' % re.escape(item.key), item.id)

    def test_every_environment_switch_is_read_by_the_game(self):
        for item in bt.ITEMS:
            if item.pass_env:
                self.assertIn(f'"{item.env}"', SOURCES, item.id)
        for name in ('TLG_READBACKS', 'TLG_STICK_DEADZONE', 'TLG_LANGUAGE', 'TLG_KEY_MAP', 'TLG_MODS_DIR'):
            self.assertIn(f'"{name}"', SOURCES, name)    # what tlg_run.build_env sets from the launcher

    def test_dead_switches_are_gone_from_the_launcher(self):
        ids = {i.id for i in bt.ITEMS} | {i.env for i in bt.ITEMS}
        launcher = '\n'.join(p.read_text(encoding='utf-8') for p in (ROOT / 'launcher').rglob('*')
                             if p.suffix in ('.py', '.qml', '.json') and p.is_file())
        for name in DEAD:
            self.assertNotIn(name, ids)
            self.assertNotIn(f'"{name}"', SOURCES, f'{name} came back in the game: list it here')
            self.assertNotIn(name.replace('TLG_', 'ISS_'), launcher)
        self.assertNotIn('TLG_OCCLUSION_MODE', launcher)


class TableTest(unittest.TestCase):
    def test_items_are_bilingual_and_unique(self):
        self.assertEqual(len({i.id for i in bt.ITEMS}), len(bt.ITEMS))
        for item in bt.ITEMS:
            self.assertTrue(all(item.name) and all(item.what), item.id)
            self.assertIn(item.store, ('ini', 'launcher'))
            if item.vtype in ('int', 'float'):
                self.assertLessEqual(item.lo, float(item.default), item.id)
                self.assertGreaterEqual(item.hi, float(item.default), item.id)
            if item.vtype == 'choice':
                self.assertIn(item.default, [c[0] for c in item.choices], item.id)
        self.assertEqual(bt.BACKPORTS, [])
        self.assertEqual(bt.ADVANCED, [])

    def test_states_round_trip_and_defaults_store_nothing(self):
        ini = {'fsr1': '1', 'rcas_strength': '75', 'color_contrast': '1.20', 'overlay_corner': '9'}
        states = bt.read_states(ini, {'deferred_readback': True, 'background_warmup': True})
        self.assertEqual(states['menu:fsr1'], '1')
        self.assertIsNone(states['menu:rcas_strength'])          # equal to the default
        self.assertEqual(states['menu:color_contrast'], '1.2')
        self.assertIsNone(states['menu:overlay_corner'])         # invalid choice
        self.assertEqual(states['TLG_DEFERRED_READBACK'], '1')
        self.assertIsNone(states['TLG_BACKGROUND_WARMUP'])
        updates = bt.ini_updates(states)
        self.assertEqual(updates['fsr1'], '1')
        self.assertIsNone(updates['rcas_strength'])
        self.assertNotIn('deferred_readback', updates)           # kept in settings.json

    def test_environment_only_for_changes_and_outside_values_win(self):
        states = {i.id: None for i in bt.ITEMS}
        self.assertEqual(bt.environment(states), {})
        states['TLG_DEFERRED_READBACK'] = '1'
        states['TLG_READBACK_LINEAR'] = '0'
        states['menu:fsr1'] = '1'                                 # tlg.ini, never the environment
        self.assertEqual(bt.environment(states), {'TLG_DEFERRED_READBACK': '1', 'TLG_READBACK_LINEAR': '0'})
        self.assertEqual(bt.environment(states, {'TLG_READBACK_LINEAR': '1'}), {'TLG_DEFERRED_READBACK': '1'})
        self.assertEqual(bt.changes(states), 2)                   # menu preferences do not count

    def test_save_experimental_writes_ini_and_settings(self):
        import os
        import tempfile
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {'TLG_USER_DIR': tmp}):
            settings = dict(tlg_run.DEFAULT_SETTINGS)
            states = {i.id: None for i in bt.ITEMS}
            states.update({'menu:smaa': '1', 'TLG_BLOCK1D': '1'})
            (Path(tmp) / 'tlg.ini').write_text('# tlg settings\nmenu_tab=2\nsmaa=0\n', encoding='utf-8')
            tlg_run.save_experimental(settings, states)
            text = (Path(tmp) / 'tlg.ini').read_text(encoding='utf-8')
            self.assertIn('smaa=1', text)
            self.assertIn('menu_tab=2', text)                     # the game's own keys stay
            self.assertIs(True, settings['block1d'])
            self.assertEqual(tlg_run.experimental_states(settings)['TLG_BLOCK1D'], '1')
            env = tlg_run.build_env(dict(settings, game_dir=''))
            self.assertEqual(env['TLG_BLOCK1D'], '1')


class PresetTest(unittest.TestCase):
    def test_presets_only_use_existing_items_and_launcher_defaults_match(self):
        ids = {i.id for i in bt.ITEMS}
        for preset_id, settings, items in gfxlogic.PRESETS:
            self.assertTrue(set(items) <= ids, preset_id)
            self.assertTrue(set(settings) <= set(tlg_run.DEFAULT_SETTINGS), preset_id)
        self.assertTrue(set(gfxlogic.PRESET_ITEMS) <= ids)
        for key, value in gfxlogic.SETTING_DEFAULTS.items():
            self.assertEqual(tlg_run.DEFAULT_SETTINGS[key], value, key)

    def test_budget_follows_the_tk_table(self):
        self.assertEqual(gfxlogic.suggested_budget_mb(6), 5600)
        self.assertEqual(gfxlogic.suggested_budget_mb(8), 7000)
        self.assertEqual(gfxlogic.suggested_budget_mb(12), 10500)
        self.assertEqual(gfxlogic.suggested_budget_mb(24), 21500)
        self.assertFalse(gfxlogic.budget_is_too_high(5600, 6144))
        self.assertTrue(gfxlogic.budget_is_too_high(6000, 6144))


if __name__ == '__main__':
    unittest.main()
