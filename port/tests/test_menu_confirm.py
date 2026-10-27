from paths import ROOT
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import tlg_patches as t

sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run


def fixture_elf(instructions=t.MENU_EXPECTED):
    data = bytearray(t.TEXT_FILE_OFFSET + t.MENU_OFFSET + len(instructions))
    data[:4] = b'\x7fELF'
    data[t.TEXT_FILE_OFFSET + t.MENU_OFFSET:] = instructions
    return bytes(data)


class MenuConfirmTests(unittest.TestCase):
    def test_default_is_no_game_patch(self):
        self.assertEqual(t.menu_writes(None, 'cross'), [])

    def test_wrong_build_and_unknown_preference_fail_closed(self):
        for elf, preference in ((b'wrong build', 'circle'), (b'', 'circle'), (None, 'invalid')):
            with self.assertRaises(ValueError):
                t.menu_writes(elf, preference)

    def test_exact_conversion_and_ordinary_face_bits(self):
        elf = fixture_elf()
        with patch.object(t, 'MENU_ELF_SHA256', hashlib.sha256(elf).hexdigest()):
            writes = t.menu_writes(elf, 'circle')
        self.assertEqual([(at, data) for at, data, _ in writes],
                         [(t.MENU_OFFSET, bytes.fromhex('41c1e20441c1e504'))])
        for raw, ordinary, menu in ((0, 0, 0), (0x2000, 2, 0x20000),
                                     (0x4000, 1, 0x40000), (0x6000, 3, 0x60000)):
            # Ordinary Cross/Circle bits are produced by separate, untouched instructions.
            self.assertEqual(((raw & 0x4000) >> 14) | ((raw & 0x2000) >> 12), ordinary)
            self.assertEqual(((raw & 0x2000) << 4) | ((raw & 0x4000) << 4), menu)

    def test_instruction_guard_even_with_matching_hash(self):
        elf = fixture_elf(b'\xcc' * len(t.MENU_EXPECTED))
        with patch.object(t, 'MENU_ELF_SHA256', hashlib.sha256(elf).hexdigest()):
            with self.assertRaisesRegex(ValueError, 'instructions'):
                t.menu_writes(elf, 'circle')

    def test_30fps_can_apply_menu_patch_without_editing_original(self):
        elf = fixture_elf()
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(t, 'MENU_ELF_SHA256', hashlib.sha256(elf).hexdigest()):
            source = Path(tmp, 'eboot.bin')
            source.write_bytes(elf)
            mode, lines = t.prepare(tmp, tmp, '30', menu_confirm='circle')
            self.assertEqual(mode, '30')
            self.assertTrue(any('gameplay unchanged' in line for line in lines))
            blob = Path(tmp, 'patches.bin').read_bytes()
            self.assertEqual(struct.unpack_from('<8sQQ', blob), (b'G3PATCH2', 0, 1))
            self.assertEqual(struct.unpack_from('<QQ', blob, 24), (t.MENU_OFFSET, 8))
            self.assertEqual(blob[40:], bytes.fromhex('41c1e20441c1e504'))
            self.assertEqual(source.read_bytes(), elf)

    def test_unsupported_game_does_not_receive_menu_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'eboot.bin').write_bytes(b'unsupported')
            mode, lines = t.prepare(tmp, tmp, '30', menu_confirm='circle')
            self.assertEqual(mode, '30')
            self.assertTrue(any('game-menu patch not applied' in line for line in lines))
            self.assertEqual(struct.unpack('<8sQQ', Path(tmp, 'patches.bin').read_bytes()),
                             (b'G3PATCH2', 0, 0))

    def test_conflicting_mod_cannot_overwrite_menu_policy(self):
        elf = fixture_elf()
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(t, 'MENU_ELF_SHA256', hashlib.sha256(elf).hexdigest()), \
                patch.object(t, 'compile_mods', return_value=[(t.MENU_OFFSET + 3, b'\x05', 'conflict')]):
            Path(tmp, 'eboot.bin').write_bytes(elf)
            _, lines = t.prepare(tmp, tmp, '30', mods_dir='unused', menu_confirm='circle')
            self.assertTrue(any('overlap' in line for line in lines))
            self.assertEqual(struct.unpack_from('<Q', Path(tmp, 'patches.bin').read_bytes(), 16)[0], 1)

    def test_launcher_and_runtime_share_normalized_preference(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(tlg_run.menu_confirm({}), 'cross')
            self.assertEqual(tlg_run.menu_confirm({'menu_confirm': 'invalid'}), 'cross')
            with patch.object(tlg_run, 'window_icon', return_value=None), \
                    patch.object(tlg_run, 'ngs2_module', return_value=None), \
                    patch.object(tlg_run, 'user_dir', return_value=ROOT / 'unused-test-user'), \
                    patch.object(tlg_run, 'probe_env', return_value={}):
                settings = dict(tlg_run.DEFAULT_SETTINGS, menu_confirm='circle')
                self.assertEqual(tlg_run.build_env(settings)['TLG_MENU_CONFIRM'], 'circle')
        with patch.dict(os.environ, {'TLG_MENU_CONFIRM': 'cross'}):
            self.assertEqual(tlg_run.menu_confirm({'menu_confirm': 'circle'}), 'cross')

    def test_ini_preference_is_loaded(self):
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(tlg_run, 'user_dir', return_value=Path(tmp)), \
                patch.object(tlg_run, 'config_path', return_value=Path(tmp, 'settings.json')):
            Path(tmp, 'tlg.ini').write_text('menu_confirm=circle\n')
            self.assertEqual(tlg_run.load_settings()['menu_confirm'], 'circle')
