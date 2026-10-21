# SPDX-License-Identifier: GPL-2.0-or-later
"""Export and import of configuration files (launcher/preset_share.py). The pure parts need no Qt;
the controller test at the end skips without PySide6. Nothing here touches the real settings.json:
`paths` points the launcher config at a throwaway folder, and the controller test uses its own."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import backports_table  # noqa: E402
import preset_share  # noqa: E402
from qt import gfxlogic, logic  # noqa: E402

try:
    from qt.backend import Backend
    from qt.graphics import Graphics
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

DEFAULTS = dict(logic.tlg_run.DEFAULT_SETTINGS)


def t(key, **fmt):
    en, _ = logic.tk_text().get(key, (key, key))
    return en.format(**fmt) if fmt else en


def custom_states():
    """A player's choices: a few menu values of each type, a start-up switch off and one on."""
    states = {i.id: None for i in backports_table.ITEMS}
    states.update({'menu:fsr1': '1', 'menu:rcas_strength': '80', 'menu:color_contrast': '1.2', 'menu:fps_limit': '60',
                   'menu:overlay_corner': '3', 'TLG_BACKGROUND_WARMUP': '0', 'TLG_DEFERRED_READBACK': '1'})
    return states


def custom_settings():
    return dict(DEFAULTS, vsync=False, show_fps=True, gpu_readbacks='1', frame_rate='60',
                display_mode='Fullscreen', vram_budget_mb=9000, game_dir='/secret/path', ui_language='pt_BR')


class Base(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, data, name='x.tlg-preset.json'):
        path = self.dir / name
        path.write_text(data if isinstance(data, str) else json.dumps(data), encoding='utf-8')
        return path

    def valid(self, **over):
        data = {'format': 'tlg-preset', 'version': 1, 'game': 'CUSA03627', 'name': 'n', 'what': 'w',
                'exported': '2026-10-16', 'port_version': 'v1', 'settings': {'vsync': False},
                'items': {'menu:fsr1': '1'}}
        data.update(over)
        return data


class ExportTests(Base):
    def test_file_layout_and_machine_items_stay_out(self):
        preset = preset_share.export_preset(custom_settings(), custom_states(), ' My setup ', 'Notes', 'v9', '2026-10-16')
        for key in ('format', 'version', 'game', 'name', 'what', 'exported', 'port_version', 'settings', 'items'):
            self.assertIn(key, preset)
        self.assertEqual((preset['format'], preset['version'], preset['game'], preset['name']),
                         ('tlg-preset', 1, 'CUSA03627', 'My setup'))
        self.assertEqual(preset['settings'], {'vsync': False, 'show_fps': True, 'gpu_readbacks': '1', 'frame_rate': '60'})
        text = json.dumps(preset)
        for secret in ('vram_budget_mb', 'display_mode', 'game_dir', 'ui_language', '/secret/path', 'savedata'):
            self.assertNotIn(secret, text)
        self.assertEqual(preset['items']['menu:color_contrast'], '1.2')
        self.assertEqual(preset['items']['TLG_BACKGROUND_WARMUP'], '0')
        self.assertEqual(preset['items']['TLG_READBACK_LINEAR'],
                         backports_table.effective(gfxlogic.item_by_id('TLG_READBACK_LINEAR'), {}))
        self.assertIn('menu:overlay_corner', preset['items'])

    def test_an_amd_workaround_the_player_chose_is_exported(self):
        states = custom_states()
        states['TLG_BLOCK1D'] = '1'
        self.assertEqual(preset_share.export_preset({}, states)['items']['TLG_BLOCK1D'], '1')

    def test_write_adds_the_suffix_and_default_name_is_safe(self):
        path = preset_share.write_preset(preset_share.export_preset({}, {}, 'a'), self.dir / 'mine')
        self.assertTrue(str(path).endswith('.tlg-preset.json'))
        self.assertEqual(preset_share.write_preset({'a': 1}, self.dir / 'x.json').name, 'x.json')
        self.assertTrue(preset_share.default_filename('Rec/ommended: 1').endswith('.tlg-preset.json'))
        self.assertNotIn('/', preset_share.default_filename('a/b'))


class RoundTripTests(Base):
    def test_every_preset_undoes_what_another_set(self):
        settings = dict(DEFAULTS)
        states = {i.id: None for i in backports_table.ITEMS}
        self.assertEqual(gfxlogic.matching_preset(settings, states), gfxlogic.DEFAULT_PRESET)
        self.assertEqual(backports_table.environment(states), {})
        gfxlogic.apply_targets(settings, states, gfxlogic.preset_targets('quality'))
        self.assertEqual(states['menu:smaa'], '1')
        self.assertEqual(gfxlogic.matching_preset(settings, states), 'quality')
        for preset_id in ('balanced', 'performance', 'ps4'):
            gfxlogic.apply_targets(settings, states, gfxlogic.preset_targets(preset_id))
            self.assertIsNone(states['menu:smaa'], preset_id)
            self.assertEqual(gfxlogic.matching_preset(settings, states), preset_id)

    def test_export_then_read_gives_the_same_values(self):
        settings, states = custom_settings(), custom_states()
        path = preset_share.write_preset(preset_share.export_preset(settings, states, 'rt'), self.dir / 'rt')
        preset, problems = preset_share.read_preset(path)
        self.assertEqual(problems, [])
        self.assertEqual(preset['profile'], 'rt')
        # apply to a fresh PC: the shared values come back, the machine ones stay as they are
        other_settings = dict(DEFAULTS, display_mode='Borderless', vram_budget_mb=5000)
        other_states = {i.id: None for i in backports_table.ITEMS}
        preset_share.apply_preset(preset, other_settings, other_states)
        for key in preset_share.EXPORT_SETTINGS:
            self.assertEqual(other_settings[key], settings[key], key)
        self.assertEqual((other_settings['display_mode'], other_settings['vram_budget_mb']), ('Borderless', 5000))
        for item in backports_table.ITEMS:
            self.assertEqual(backports_table.effective(item, other_states), backports_table.effective(item, states), item.id)
        self.assertEqual(preset_share.diff(preset, other_settings, other_states), [])

    def test_a_built_in_preset_exports_and_matches_again(self):
        settings, states = dict(DEFAULTS), {i.id: None for i in backports_table.ITEMS}
        gfxlogic.apply_targets(settings, states, gfxlogic.preset_targets('quality'))
        preset, problems = preset_share.parse_preset(preset_share.export_preset(settings, states, 'c'))
        self.assertEqual(problems, [])
        other_settings, other_states = dict(DEFAULTS), {i.id: None for i in backports_table.ITEMS}
        preset_share.apply_preset(preset, other_settings, other_states)
        self.assertEqual(gfxlogic.matching_preset(other_settings, other_states), 'quality')

    def test_diff_lists_only_what_changes_and_texts_render(self):
        preset, _ = preset_share.parse_preset(self.valid(items={'menu:fsr1': '1', 'menu:color_contrast': '1.3'},
                                                         settings={'vsync': False, 'gpu_readbacks': '0'}))
        changes = preset_share.diff(preset, dict(DEFAULTS), {i.id: None for i in backports_table.ITEMS})
        self.assertEqual(sorted((k, i) for k, i, _, _ in changes),
                         [('item', 'menu:color_contrast'), ('item', 'menu:fsr1'), ('setting', 'gpu_readbacks'),
                          ('setting', 'vsync')])
        body, lines = preset_share.summary_lines(preset, [], changes, t, False)
        self.assertIn('4', body)
        self.assertEqual(len(lines), 4)
        self.assertTrue(any('Contrast' in line and '1.3' in line for line in lines))


class ValidationTests(Base):
    def read(self, data):
        return preset_share.read_preset(self.write(data))

    def codes(self, problems):
        return [p.code for p in problems]

    def test_broken_json_and_unreadable_files_never_raise(self):
        for text in ('{"format": ', '', 'not json', '[]', '"x"', 'null', '[' * 100000):
            preset, problems = self.read(text)
            self.assertIsNone(preset, text[:10])
            self.assertTrue(problems[0].fatal)
        binary = self.dir / 'b.json'
        binary.write_bytes(b'\xff\xfe\x00\x80')
        self.assertEqual(self.codes(preset_share.read_preset(binary)[1]), ['unreadable'])
        self.assertEqual(self.codes(preset_share.read_preset(self.dir / 'missing.json')[1]), ['unreadable'])
        self.assertEqual(self.codes(preset_share.read_preset(self.dir)[1]), ['unreadable'])
        big = self.write('x' * (preset_share.MAX_BYTES + 1), 'big.json')
        self.assertEqual(self.codes(preset_share.read_preset(big)[1]), ['too_big'])
        self.assertEqual(self.codes(self.read('{"a": 1')[1]), ['not_json'])

    def test_wrong_format_and_version(self):
        self.assertEqual(self.codes(self.read(self.valid(format='other'))[1]), ['format'])
        self.assertEqual(self.codes(self.read({'settings': {}})[1]), ['format'])
        for version in (0, '1', None, 1.5, True):
            self.assertEqual(self.codes(self.read(self.valid(version=version))[1]), ['version'], version)
        preset, problems = self.read(self.valid(version=2))
        self.assertIsNone(preset)
        self.assertEqual(self.codes(problems), ['newer'])
        self.assertEqual(self.codes(self.read(self.valid(settings={}, items={}))[1]), ['empty'])
        self.assertEqual(self.codes(self.read(self.valid(settings=[], items=3))[1]), ['empty'])

    def test_another_game_is_a_warning_and_still_applies(self):
        preset, problems = self.read(self.valid(game='CUSA99999'))
        self.assertIsNotNone(preset)
        self.assertEqual([(p.code, p.detail, p.fatal) for p in problems], [('game', 'CUSA99999', False)])
        self.assertEqual(self.codes(self.read(self.valid(game=None))[1]), ['game'])
        self.assertIn('CUSA99999', preset_share.problem_text(problems[0], t))

    def test_unknown_ids_are_ignored_and_listed(self):
        preset, problems = self.read(self.valid(items={'menu:fsr1': '1', 'BP99': '1', 'menu:nope': '3'},
                                                settings={'vsync': True, 'vram_budget_mb': 9999, 'display_mode': 'Fullscreen',
                                                          'game_dir': '/x'}))
        self.assertEqual(preset['items'], {'menu:fsr1': '1'})
        self.assertEqual(preset['settings'], {'vsync': True})
        self.assertEqual(sorted((p.code, p.detail) for p in problems),
                         [('unknown_item', 'BP99'), ('unknown_item', 'menu:nope'), ('unknown_setting', 'display_mode'),
                          ('unknown_setting', 'game_dir'), ('unknown_setting', 'vram_budget_mb')])
        self.assertFalse(any(p.fatal for p in problems))

    def test_out_of_range_and_wrong_type_values_are_refused_and_listed(self):
        bad = {'menu:rcas_strength': '101',         # int, hi 100
               'menu:color_contrast': 9,            # float, hi 1.5
               'menu:color_gamma': 'abc',
               'menu:fsr1': '2',                    # bool
               'menu:fps_limit': '45',              # choice
               'menu:cas_strength': 80.5,           # int, not whole
               'TLG_BLOCK1D': None, 'TLG_BACKGROUND_WARMUP': [1], 'TLG_STALE_READBACK': {'a': 1},
               'menu:overlay_scale': float('inf')}
        preset, problems = self.read(self.valid(items=dict(bad, **{'menu:rcas': '1'}),
                                                settings={'vsync': 'yes', 'gpu_readbacks': '7', 'frame_rate': 2}))
        self.assertEqual(preset['items'], {'menu:rcas': '1'})
        self.assertEqual(preset['settings'], {})
        refused = {p.detail.split(' = ')[0] for p in problems if p.code in ('bad_item', 'bad_setting')}
        self.assertEqual(refused, set(bad) | {'vsync', 'gpu_readbacks', 'frame_rate'})
        self.assertFalse(any(p.fatal for p in problems))

    def test_valid_values_of_every_type_are_accepted_and_normalised(self):
        preset, problems = self.read(self.valid(
            items={'menu:rcas_strength': 70, 'menu:color_contrast': '1.50', 'menu:fps_limit': 120, 'TLG_BLOCK1D': True,
                   'menu:overlay_scale': 100.0, 'menu:color_saturation': 2.0},
            settings={'vsync': 0, 'gpu_readbacks': 2, 'frame_rate': 'unlocked'}))
        self.assertEqual(problems, [])
        self.assertEqual(preset['items'], {'menu:rcas_strength': '70', 'menu:color_contrast': '1.5', 'menu:fps_limit': '120',
                                           'TLG_BLOCK1D': '1', 'menu:overlay_scale': '100', 'menu:color_saturation': '2'})
        self.assertEqual(preset['settings'], {'vsync': False, 'gpu_readbacks': '2', 'frame_rate': 'unlocked'})

    def test_every_problem_code_has_a_text_in_both_languages(self):
        for code in ('unreadable', 'too_big', 'not_json', 'format', 'version', 'newer', 'empty', 'game',
                     'unknown_item', 'unknown_setting', 'bad_item', 'bad_setting'):
            en, pt = preset_share.TEXT['ps_p_' + code]
            self.assertTrue(en and pt)
        for key, (en, pt) in preset_share.TEXT.items():
            self.assertTrue(en and pt, key)


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class ControllerTests(Base):
    """Import through Graphics: same path as the built-in presets, saved to the temporary folders only."""
    def setUp(self):
        super().setUp()
        patcher = mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.dir / 'cfg'),
                                               'TLG_USER_DIR': str(self.dir / 'user')})
        patcher.start()
        self.addCleanup(patcher.stop)
        probes = {'gpu': lambda: ('GPU', '1'), 'pad': lambda: 'pad', 'problems': lambda: [], 'cache': lambda: 0,
                  'save': lambda: None, 'news': lambda: [], 'version': lambda: 'v1',
                  'saves': {'running': lambda: False, 'scan': lambda: [], 'open': lambda p: None}}
        settings = dict(DEFAULTS, game_dir='/g', ui_language='en_US')
        self.backend = Backend(settings=settings, probes=probes)
        self.graphics = Graphics(self.backend, {'vram': lambda: 6144, 'version': lambda: 'v1'})
        self.ini = self.dir / 'user' / 'tlg.ini'

    def saved(self):
        return json.loads(logic.tlg_run.config_path().read_text())

    def test_export_writes_a_file_with_the_current_values(self):
        self.graphics.setValue('menu:fsr1', '1')
        self.graphics.setValue('s:vsync', '0')
        out = self.dir / 'out' / 'mine'
        self.graphics.exportPreset('Mine', 'desc', str(out))
        self.assertEqual(self.graphics.shareTone, 'good')
        data = json.loads((self.dir / 'out' / 'mine.tlg-preset.json').read_text(encoding='utf-8'))
        self.assertEqual((data['name'], data['what'], data['port_version']), ('Mine', 'desc', 'v1'))
        self.assertEqual((data['items']['menu:fsr1'], data['settings']['vsync']), ('1', False))
        self.assertNotIn('vram_budget_mb', data['settings'])
        self.graphics.exportPreset('Mine', '', str(self.dir / 'no' / 'such' / '\0bad'))   # unwritable: a message
        self.assertEqual(self.graphics.shareTone, 'bad')

    def test_import_asks_then_applies_through_the_preset_path(self):
        self.graphics.applyPreset('balanced')
        path = self.write(self.valid(name='Friend', items={'menu:fsr1': '1', 'menu:color_contrast': '1.3', 'BP99': '1'},
                                     settings={'vsync': False, 'gpu_readbacks': '1'}), 'friend.tlg-preset.json')
        self.graphics.importPreset(str(path))
        dialog = self.graphics.dialog
        self.assertTrue(dialog['open'])
        self.assertEqual([b['id'] for b in dialog['buttons']], ['apply', 'cancel'])
        self.assertIn('friend', dialog['body'])
        self.assertTrue(any('BP99' in line for line in dialog['lines']))        # the ignored id is listed
        self.assertNotIn('color_contrast', self.ini.read_text() if self.ini.exists() else '')   # nothing yet
        self.graphics.answer('cancel')
        self.assertEqual(self.saved().get('gpu_readbacks'), '2')
        self.graphics.importPreset(str(path))
        self.graphics.answer('apply')
        saved = self.saved()
        self.assertEqual((saved['vsync'], saved['gpu_readbacks']), (False, '1'))
        self.assertIn('color_contrast=1.3', self.ini.read_text())
        self.assertEqual(preset_share.diff(preset_share.read_preset(path)[0], self.backend.settings,
                                           self.backend.fix_states), [])
        self.assertIn('friend', self.graphics.presetText)                       # custom profile with the file name
        self.assertEqual(self.graphics.exportName, 'friend')
        self.graphics.setValue('menu:fps_limit', '120')
        self.assertNotIn('friend', self.graphics.presetText)

    def test_import_of_a_built_in_preset_export_is_recognised_and_bad_files_open_no_apply(self):
        self.graphics.applyPreset('quality')
        out = self.dir / 'cur.tlg-preset.json'
        self.graphics.exportPreset('Cur', '', str(out))
        self.graphics.applyPreset('performance')
        self.graphics.importPreset(str(out))
        self.assertEqual(self.graphics.dialog['body'].count('will change'), 1)
        self.graphics.answer('apply')
        self.assertIn('quality', self.graphics.presetText.lower())
        self.graphics.importPreset(str(out))                                     # same again: nothing to change
        self.assertEqual([b['id'] for b in self.graphics.dialog['buttons']], ['close'])
        self.graphics.answer('close')
        for bad in ('{"format": ', json.dumps(self.valid(version=3))):
            self.graphics.importPreset(str(self.write(bad, 'bad.json')))
            self.assertEqual([b['id'] for b in self.graphics.dialog['buttons']], ['close'])
            self.assertTrue(self.graphics.dialog['lines'])
            self.graphics.answer('close')


if __name__ == '__main__':
    unittest.main()
