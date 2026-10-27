from paths import ROOT
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run


def param_sfo(title='CUSA03745', version='01.03'):
    keys, values, entries = bytearray(), bytearray(), []
    for key, value in [('TITLE_ID', title), ('APP_VER', version)]:
        encoded = value.encode() + b'\0'
        size = (len(encoded) + 3) & ~3
        entries.append(struct.pack('<HHIII', len(keys), 0x204, len(encoded), size, len(values)))
        keys += key.encode() + b'\0'
        values += encoded + b'\0' * (size - len(encoded))
    key_offset = 20 + 16 * len(entries)
    value_offset = (key_offset + len(keys) + 3) & ~3
    return (struct.pack('<4sIIII', b'\0PSF', 0x101, key_offset, value_offset, len(entries)) +
            b''.join(entries) + keys + b'\0' * (value_offset - key_offset - len(keys)) + values)


class JapaneseLanguageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.game = Path(self.tmp.name)
        (self.game / 'sce_sys').mkdir()
        for name in ('trico_boot.psarc', 'trico_bootp.psarc', 'trico_frf.psarc', 'trico_frfp.psarc'):
            (self.game / name).write_bytes(b'untouched archive fixture')
        self.metadata()
        self.settings = {'game_dir': str(self.game), 'game_language': 'ja'}

    def metadata(self, title='CUSA03745', version='01.03'):
        (self.game / 'sce_sys/param.sfo').write_bytes(param_sfo(title, version))

    def test_verified_layout_offers_explicit_japanese_choice(self):
        self.assertTrue(tlg_run.japanese_fallback_available(self.game))
        self.assertIn('ja', tlg_run.game_language_choices(dict(self.settings, game_language='auto')))
        self.assertEqual(tlg_run.resolve_game_language(self.settings, {}), 'ja')
        self.assertNotIn('ja', tlg_run.game_languages(self.game))  # native engine table unchanged

    def test_japanese_emits_internal_french_id_and_guest_mask(self):
        self.assertEqual(tlg_run.game_language_environment(self.settings, {}),
                         {'TLG_LANGUAGE': '2', 'TLG_JAPANESE_FALLBACK': '1'})

    def test_ordinary_french_keeps_both_french_archives_visible(self):
        s = dict(self.settings, game_language='fr')
        self.assertEqual(tlg_run.game_language_environment(s, {'TLG_JAPANESE_FALLBACK': '1'}),
                         {'TLG_LANGUAGE': '2', 'TLG_JAPANESE_FALLBACK': '0'})

    def test_other_languages_do_not_inherit_a_stale_mask(self):
        for choice, code in [('en', '1'), ('de', '4')]:
            self.assertEqual(tlg_run.game_language_environment(dict(self.settings, game_language=choice),
                                                               {'TLG_JAPANESE_FALLBACK': '1'}),
                             {'TLG_LANGUAGE': code, 'TLG_JAPANESE_FALLBACK': '0'})

    def test_automatic_does_not_silently_opt_into_experimental_fallback(self):
        self.assertEqual(tlg_run.game_language_environment(dict(self.settings, game_language='auto'),
                                                           {'LANG': 'ja_JP.UTF-8'}),
                         {'TLG_LANGUAGE': '1', 'TLG_JAPANESE_FALLBACK': '0'})

    def test_other_regions_and_versions_are_not_advertised(self):
        for title, version in [('CUSA03627', '01.03'), ('CUSA04936', '01.03'),
                               ('CUSA04943', '01.03'), ('CUSA99999', '01.03'),
                               ('CUSA03745', '01.00'), ('CUSA03745', '01.04')]:
            self.metadata(title, version)
            self.assertFalse(tlg_run.japanese_fallback_available(self.game))
            self.assertNotIn('ja', tlg_run.game_language_choices(dict(self.settings, game_language='auto')))
            self.assertEqual(tlg_run.game_language_environment(self.settings, {}),
                             {'TLG_LANGUAGE': '1', 'TLG_JAPANESE_FALLBACK': '0'})

    def test_missing_boot_archive_or_bad_metadata_fails_closed(self):
        (self.game / 'trico_bootp.psarc').unlink()
        self.assertFalse(tlg_run.japanese_fallback_available(self.game))
        (self.game / 'sce_sys/param.sfo').write_bytes(b'broken')
        self.assertFalse(tlg_run.japanese_fallback_available(self.game))
        with patch.object(tlg_run.shader_cache, 'title_id', return_value='CUSA03745'), \
                patch.object(tlg_run.mod_patches, 'game_app_version', side_effect=struct.error('broken')):
            self.assertFalse(tlg_run.japanese_fallback_available(self.game))

    def test_explicit_engine_override_is_not_combined_with_wrong_mask(self):
        self.assertEqual(tlg_run.game_language_environment(self.settings, {'TLG_LANGUAGE': '1'}),
                         {'TLG_LANGUAGE': '1', 'TLG_JAPANESE_FALLBACK': '0'})

    def test_preexisting_manual_workaround_does_not_need_french_archives_restored(self):
        for name in ('trico_frf.psarc', 'trico_frfp.psarc'):
            (self.game / name).unlink()
        self.assertTrue(tlg_run.japanese_fallback_available(self.game))
        self.assertEqual(tlg_run.game_language_environment(self.settings, {})['TLG_JAPANESE_FALLBACK'], '1')

    def test_build_env_uses_the_same_language_policy(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(tlg_run, 'window_icon', return_value=None), \
                patch.object(tlg_run, 'ngs2_module', return_value=None), \
                patch.object(tlg_run, 'user_dir', return_value=self.game / 'unused-user'), \
                patch.object(tlg_run, 'probe_env', return_value={}):
            settings = dict(tlg_run.DEFAULT_SETTINGS, **self.settings)
            env = tlg_run.build_env(settings)
            self.assertEqual(env['TLG_LANGUAGE'], '2')
            self.assertEqual(env['TLG_JAPANESE_FALLBACK'], '1')

    def test_environment_planning_does_not_modify_game_or_saves(self):
        before = {str(p.relative_to(self.game)): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        tlg_run.game_language_environment(self.settings, {})
        after = {str(p.relative_to(self.game)): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
