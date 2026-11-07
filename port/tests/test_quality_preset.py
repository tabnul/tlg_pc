# SPDX-License-Identifier: GPL-2.0-or-later
"""The Quality row (tlg.ini preset): render size per output, the menu list, and the render size
derived on one start never pinning the next ("Apply and restart" inherits the environment)."""
import os
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run  # noqa: E402


class QualityPresetTests(unittest.TestCase):
    def test_render_size_per_output(self):
        table = {
            '1920x1080': ['1280x720', '1130x636', '960x540', '640x360', '2880x1620', '3840x2160'],
            '3840x1600': ['2560x1066', '2258x942', '1920x800', '1280x534', '5760x2400', '7680x3200'],
            '2560x1440': ['1706x960', '1506x848', '1280x720', '854x480', '3840x2160', '5120x2880'],
            '3840x2160': ['2560x1440', '2258x1270', '1920x1080', '1280x720', '5760x3240', '7680x4320'],
        }
        for output, sizes in table.items():
            self.assertIsNone(tlg_run.render_resolution(output, 0))
            self.assertEqual([tlg_run.render_resolution(output, p) for p in range(1, 7)], sizes, output)

    def test_preset_from_ini_and_environment(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('TLG_UPSCALE_PRESET', None)
            self.assertEqual(tlg_run.quality_preset({'preset': '3'}), 3)
            self.assertEqual(tlg_run.quality_preset({'preset': '9'}), 6)
            self.assertEqual(tlg_run.quality_preset({'preset': 'x'}), 0)
            self.assertEqual(tlg_run.quality_preset({}), 0)
            os.environ['TLG_UPSCALE_PRESET'] = '2'
            self.assertEqual(tlg_run.quality_preset({'preset': '3'}), 2)

    def test_menu_lists_supersampling_only_while_selected(self):
        self.assertEqual(len(tlg_run.menu_preset_keys(0)), tlg_run.MENU_PRESETS)
        self.assertEqual(len(tlg_run.menu_preset_keys(4)), tlg_run.MENU_PRESETS)
        self.assertEqual(tlg_run.menu_preset_keys(5), tlg_run.PRESET_KEYS)
        self.assertTrue(tlg_run.supersampling(5) and tlg_run.supersampling(6))
        self.assertFalse(tlg_run.supersampling(4))

    def test_render_size_is_derived_again_on_restart(self):
        settings = dict(tlg_run.DEFAULT_SETTINGS)
        with mock.patch.dict(os.environ, {}, clear=False):
            for key in ('TLG_RENDER_RESOLUTION', 'TLG_DERIVED'):
                os.environ.pop(key, None)
            tlg_run.applied_render_resolution = '2560x1066'
            env = tlg_run.build_env(settings)
            self.assertEqual(env['TLG_RENDER_RESOLUTION'], '2560x1066')
            self.assertEqual(env['TLG_DERIVED'], 'TLG_RENDER_RESOLUTION')
            # The restarted launcher inherits both and must drop the derived value before
            # preparing the next start (prepare() does), so the new preset decides.
            os.environ.update(env)
            for key in os.environ.pop('TLG_DERIVED', '').split(','):
                os.environ.pop(key, None)
            self.assertNotIn('TLG_RENDER_RESOLUTION', os.environ)
            tlg_run.applied_render_resolution = None
            env = tlg_run.build_env(settings)
            self.assertNotIn('TLG_RENDER_RESOLUTION', env)
            self.assertNotIn('TLG_DERIVED', env)
        tlg_run.applied_render_resolution = None


if __name__ == '__main__':
    unittest.main()
