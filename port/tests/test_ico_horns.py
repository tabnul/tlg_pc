# SPDX-License-Identifier: GPL-2.0-or-later
"""The ICO horn models (launcher/ico_horns.py) are built on the player's PC: every machine must get
the same bytes as the archive that was approved in game, and the generator must not read any file."""
import builtins
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import ico_horns  # noqa: E402

APPROVED = 'f05255c40cf08d74ca92b5731123a9cbbab4f1940ad404f07b574ef9bec8b1c0'


class IcoHorns(unittest.TestCase):
    def test_same_bytes_everywhere(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'trico_other.psarc'
            self.assertEqual(ico_horns.write(str(path)), APPROVED)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), APPROVED)

    def test_reads_no_file(self):
        real, opened = builtins.open, []

        def watched(file, mode='r', *args, **kwargs):
            opened.append((str(file), mode))
            return real(file, mode, *args, **kwargs)

        builtins.open = watched
        try:
            files = ico_horns.build()
        finally:
            builtins.open = real
        self.assertEqual(opened, [])
        self.assertIn('game/assets/props/level/icohorn_rx/icohorn_rx.bod', files)


if __name__ == '__main__':
    unittest.main()
