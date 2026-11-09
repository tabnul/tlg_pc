# SPDX-License-Identifier: GPL-2.0-or-later
"""Language packs (launcher/langpack.py, gpu/shim/tlg_lang.h, docs/language-packs.md): the menu
text list matches the C++ source, every shipped pack passes the translator check, and switching
packs always comes back to the exact English texts."""
import json
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import langpack  # noqa: E402
import backports_table  # noqa: E402
import tlg_launcher  # noqa: E402

SHIPPED = sorted(p for p in langpack.SHIPPED_DIR.glob('*.json') if p.name != langpack.MENU_TEXTS.name)


class MenuTexts(unittest.TestCase):
    def test_menu_text_list_is_current(self):
        self.assertEqual(langpack.scan_menu(ROOT), langpack.menu_texts(),
                         'run: python launcher/langpack.py menu-scan')

    def test_no_old_two_language_calls_left(self):
        src = (ROOT / 'gpu' / 'shim' / 'tlg_overlay.cpp').read_text(encoding='utf-8')
        self.assertNotIn('TL(', src)
        self.assertNotIn('UiPortuguese', src)


class ShippedPacks(unittest.TestCase):
    def test_every_pack_passes_the_check(self):
        self.assertTrue(SHIPPED, 'no shipped packs')
        for path in SHIPPED:
            with self.subTest(pack=path.name):
                pack = langpack.read_pack(path)
                self.assertEqual(path.stem, pack['code'])
                self.assertTrue(pack.get('authors'), 'a pack names its authors')
                problems, done, total = langpack.check(pack)
                self.assertEqual(problems, [])
                self.assertGreater(done, total * 0.8, 'mostly untranslated')

    def test_printf_rule(self):
        self.assertEqual(langpack._printf('%d of %s, %.1f ms, 0x%08X, %zu'), ['%d', '%s', '%.1f', '%08X', '%zu'])
        self.assertEqual(langpack._printf('0% turns it off, 150%: sharper, 100%%'), [])


class Switching(unittest.TestCase):
    def tearDown(self):
        langpack.apply(langpack.ENGLISH)

    def snapshot(self):
        return (dict(tlg_launcher.TEXT), list(backports_table.ITEMS), dict(backports_table.READBACK_WHY))

    def test_round_trip_restores_english(self):
        langpack.apply(langpack.ENGLISH)
        before = self.snapshot()
        for path in SHIPPED:
            code = json.loads(path.read_text(encoding='utf-8'))['code']
            with self.subTest(pack=code):
                self.assertEqual(langpack.apply(code), code)
                self.assertNotEqual(self.snapshot()[0], before[0], 'pack changed nothing')
        langpack.apply(langpack.ENGLISH)
        self.assertEqual(self.snapshot(), before)

    def test_unknown_pack_is_english(self):
        self.assertEqual(langpack.apply('xx_NOPE'), langpack.ENGLISH)

    def test_broken_text_falls_back(self):
        en = langpack.english()['text']
        key = next(k for k, v in en.items() if '{' in v and langpack._fields(v))
        self.assertFalse(langpack._usable('no placeholder here', en[key]))
        self.assertTrue(langpack._usable(en[key], en[key]))


if __name__ == '__main__':
    unittest.main()
