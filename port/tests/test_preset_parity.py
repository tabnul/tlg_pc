# SPDX-License-Identifier: GPL-2.0-or-later
"""The graphics presets exist twice: in the launcher (launcher/preset_share.py GAME_STOCK,
GAME_RECOMMENDED) and in the in-game menu (gpu/shim/tlg_settings.cpp GraphicsText, StockLines,
with the defaults in tlg_settings.h as Recommended). These tests read the C++ sources and fail
when the two drift apart: a look setting missing from one side, or a Stock or default value that
differs."""
import math
import re
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import preset_share  # noqa: E402

CPP = (ROOT / 'gpu/shim/tlg_settings.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'gpu/shim/tlg_settings.h').read_text(encoding='utf-8')


def graphics_text_body():
    start = CPP.index('std::string GraphicsText(const Values& v) {')
    return CPP[start:CPP.index('\n}\n', start)]


def preset_members():
    """{tlg.ini key: Values member} for every key GraphicsText writes (texture_pack: None)."""
    members = dict(re.findall(r'add(?:_float)?\("(\w+)", v\.(\w+)\)', graphics_text_body()))
    effects = CPP[CPP.index('EffectKeys[] = {'):]
    effects = effects[:effects.index('};')]
    members.update(re.findall(r'\{"(\w+)", &Values::(\w+)\}', effects))
    if 'texture_pack=' in graphics_text_body():
        members['texture_pack'] = None
    return members


def stock_lines():
    start = CPP.index('constexpr const char* StockLines =')
    text = ''.join(re.findall(r'"([^"]*)"', CPP[start:CPP.index(';', start)]))
    return dict(line.split('=', 1) for line in text.split('\\n') if '=' in line)


def same(a, b):
    try:
        return math.isclose(float(a), float(b), abs_tol=0.005)
    except ValueError:
        return a == b


class PresetParityTests(unittest.TestCase):
    def test_both_sides_cover_the_same_settings(self):
        self.assertEqual(set(preset_members()), set(preset_share.GAME_RECOMMENDED))

    def test_stock_is_the_same_on_both_sides(self):
        cpp = stock_lines()
        for key, value in cpp.items():
            with self.subTest(key=key):
                self.assertTrue(same(preset_share.GAME_STOCK[key], value),
                                f'{key}: launcher {preset_share.GAME_STOCK[key]}, in-game {value}')
        for key, value in preset_share.GAME_STOCK.items():
            if not same(value, preset_share.GAME_RECOMMENDED[key]):
                with self.subTest(key=key):
                    self.assertIn(key, cpp, f'{key}: launcher Stock sets {value}, in-game Stock keeps the default')

    def test_recommended_is_the_in_game_default(self):
        for key, member in preset_members().items():
            if member is None:
                continue
            with self.subTest(key=key):
                found = re.search(r'\b' + member + r'\{([^}]*)\}', HEADER)
                self.assertIsNotNone(found, f'no default for Values::{member}')
                value = found.group(1).rstrip('f').replace('true', '1').replace('false', '0')
                self.assertTrue(same(preset_share.GAME_RECOMMENDED[key], value),
                                f'{key}: launcher {preset_share.GAME_RECOMMENDED[key]}, in-game default {value}')


class DeckPreset(unittest.TestCase):
    def test_values_exist(self):
        import tlg_run
        self.assertIn(preset_share.DECK_SETTINGS['resolution'], tlg_run.RESOLUTIONS)
        self.assertIn(preset_share.DECK_SETTINGS['frame_rate'], tlg_run.FRAME_RATES)

    def test_look_is_stock(self):
        # The button ends in the Stock look: the preset list shows Stock after it, not Custom.
        self.assertTrue(preset_share.game_preset_matches(preset_share.DECK_INI, preset_share.GAME_STOCK))


if __name__ == '__main__':
    unittest.main()
