# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec shared by packaging/linux/package.sh and packaging/windows/package.sh: one folder
# with the Tk launcher "$PORT_NAME". The scripts pass native paths in the environment; PyInstaller
# options are fixed here because a .spec file ignores most of the CLI.
#   TLG_SPEC_NAME   executable name           TLG_SPEC_ROOT   the port/ folder
#   TLG_SPEC_ICON   .ico (Windows only)       TLG_SPEC_HIDDEN space-separated modules for --script
#   TLG_SPEC_TK     launcher script (Tk)
import os
from pathlib import Path

name = os.environ['TLG_SPEC_NAME']
root = Path(os.environ['TLG_SPEC_ROOT'])
icon = os.environ.get('TLG_SPEC_ICON') or None
hidden = os.environ['TLG_SPEC_HIDDEN'].split()
paths = [str(root / 'scripts'), str(root / 'launcher')]

a = Analysis([str(root / 'launcher' / os.environ['TLG_SPEC_TK'])], pathex=paths, datas=[],
             hiddenimports=hidden, excludes=['PySide6', 'shiboken6'], noarchive=False)
exe = EXE(PYZ(a.pure), a.scripts, [], exclude_binaries=True, name=name, console=False, icon=icon,
          debug=False, strip=False, upx=False)
COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name=name)
