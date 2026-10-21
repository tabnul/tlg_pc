"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

# The launcher keeps its settings in ~/.config/tlg-launcher (APPDATA on Windows) and its data-folder
# pointer in ~/.config/tlg-pc, outside TLG_USER_DIR: without this, a test that saves settings would
# overwrite the player's real files (game folder, language). Every test module imports this one, so
# they all get a throwaway folder; a test that needs its own still patches XDG_CONFIG_HOME/APPDATA.
import atexit as _atexit
import os as _os
import shutil as _shutil
import tempfile as _tempfile

_CONFIG = _tempfile.mkdtemp(prefix='tlg-test-config-')
_os.environ['XDG_CONFIG_HOME'] = _CONFIG
_os.environ['APPDATA'] = _CONFIG
_atexit.register(_shutil.rmtree, _CONFIG, True)

# Same for the data folder (saves, tlg.ini, mods): tlg_run.user_dir() falls back to <port>/user, the
# real one in a source checkout. A test that needs the fallback patches tlg_run.PORT_DIR too.
_USER = _tempfile.mkdtemp(prefix='tlg-test-user-')
_os.environ['TLG_USER_DIR'] = _USER
_atexit.register(_shutil.rmtree, _USER, True)


def _real_user_untouched(path=ROOT / 'user' / 'tlg.ini', before=None):
    """Loud warning when a test changed the real tlg.ini of this checkout (it happened once)."""
    try:
        now = path.stat().st_mtime_ns
    except OSError:
        now = None
    if before != now:
        import sys as _sys
        print(f'\n*** A TEST CHANGED THE REAL {path} ***\n', file=_sys.stderr)


try:
    _BEFORE = (ROOT / 'user' / 'tlg.ini').stat().st_mtime_ns
except OSError:
    _BEFORE = None
_atexit.register(_real_user_untouched, before=_BEFORE)
