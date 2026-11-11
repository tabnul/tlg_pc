"""Packed game images (.zar, issue #72): the game folder as one ZArchive file.

The runtime reads a .zar directly as /app0 (port/src/runtime_file.c). The launcher and the
prepare scripts read a few small files themselves (eboot.bin, sce_module/, sce_sys/), so for a
.zar these are extracted once to out/zar/<name>/ and the scripts get that folder instead.
Everything else (the psarc archives, many GB) stays inside the .zar.

Idea and format from @R-V-L's SpartanPort .zar build (R-V-L/gow3_pc) and Exzap's ZArchive.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

FROZEN = bool(getattr(sys, 'frozen', False))
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
OUT_DIR = PORT_DIR / 'out'
PROBE = OUT_DIR / ('tlg-probe.exe' if os.name == 'nt' else 'tlg-probe')
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# The small files the launcher and scripts read; the rest is only read by the game.
EXTRACT_DIRS = ('sce_sys/', 'sce_module/')
EXTRACT_FILES = ('eboot.bin',)
_entries = {}


def is_zar(game_dir):
    return bool(game_dir) and str(game_dir).lower().endswith('.zar') and Path(game_dir).is_file()


def _env():
    env = os.environ.copy()
    if os.name == 'nt':  # a source build finds its DLLs in MSYS2 CLANG64
        toolchain = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if toolchain.is_dir():
            env['PATH'] = str(toolchain) + os.pathsep + env.get('PATH', '')
    elif (OUT_DIR / 'lib').is_dir():
        env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, [str(OUT_DIR / 'lib'), env.get('LD_LIBRARY_PATH')]))
    return env


def _run(args):
    return subprocess.run([str(PROBE), *args], capture_output=True, text=True, env=_env(),
                          creationflags=NO_WINDOW)


class EntryFile:
    """A read-only file object over one entry of a .zar (seek, tell, read, close), served by
    tlg-probe --zar-serve; the Soundtrack page reads the psarcs inside a packed game with it."""

    def __init__(self, zar, name):
        self.name, self.position = name, 0
        self.process = subprocess.Popen([str(PROBE), '--zar-serve', str(zar)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, env=_env(), creationflags=NO_WINDOW)

    def seek(self, offset, whence=0):
        self.position = offset if whence == 0 else self.position + offset
        return self.position

    def tell(self):
        return self.position

    def read(self, size=-1):
        if size is None or size < 0:
            size = 1 << 62
        self.process.stdin.write(f'{self.position} {size} {self.name}\n'.encode('utf-8'))
        self.process.stdin.flush()
        head = self.process.stdout.read(8)
        if len(head) != 8:
            raise OSError(f'{self.name}: archive read failed')
        data = self.process.stdout.read(int.from_bytes(head, 'little'))
        self.position += len(data)
        return data

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class EntryPath:
    """Path-like stand-in for a file inside a .zar: name, stat() and open('rb')."""

    def __init__(self, zar, name, size):
        self.zar, self.name, self.size = Path(zar), name, size

    def stat(self):
        st = self.zar.stat()
        return os.stat_result((0o100444, 0, 0, 1, 0, 0, self.size, st.st_atime, st.st_mtime, st.st_ctime))

    def is_file(self):
        return True

    def open(self, mode='rb'):
        return EntryFile(self.zar, self.name)


def game_path(game_dir, name):
    """Path of a file of the game: a real path in a folder, an EntryPath inside a .zar."""
    if is_zar(game_dir):
        for entry, size in entries(game_dir).items():
            if entry.lower() == name.lower():
                return EntryPath(game_dir, entry, size)
        raise FileNotFoundError(name)
    return Path(game_dir) / name


def root_files(game_dir, pattern):
    """Files directly in the game (folder or .zar) matching a glob pattern, sorted by name."""
    import fnmatch
    if is_zar(game_dir):
        return sorted((EntryPath(game_dir, name, size) for name, size in entries(game_dir).items()
                       if '/' not in name and fnmatch.fnmatch(name.lower(), pattern.lower())),
                      key=lambda p: p.name)
    return sorted(p for p in Path(game_dir).glob(pattern) if p.is_file())


def entries(zar):
    """{path: size} of the files in the archive (directories left out); cached per file version."""
    zar = Path(zar)
    stat = zar.stat()
    key = (str(zar), stat.st_size, stat.st_mtime_ns)
    if key not in _entries:
        result = _run(['--zar-list', str(zar)])
        if result.returncode:
            raise RuntimeError(f'{zar.name}: {result.stderr.strip() or "not a ZArchive"}')
        found = {}
        for line in result.stdout.splitlines():
            kind, size, name = line.split(' ', 2)
            if kind == 'F':
                found[name] = int(size)
        _entries[key] = found
    return _entries[key]


def has_file(game_dir, name):
    """Whether the game (folder or .zar) has the file name (relative, case-insensitive in a .zar)."""
    if is_zar(game_dir):
        try:
            return name.lower() in {path.lower() for path in entries(game_dir)}
        except (OSError, RuntimeError, ValueError):
            return False
    return (Path(game_dir) / name).is_file()


def files_dir(game_dir):
    """A real folder with the game's small files: the game folder itself, or for a .zar its
    extracted copy (made on first use, again when the .zar changes). None-safe for ''."""
    if not is_zar(game_dir):
        return game_dir
    zar = Path(game_dir)
    stat = zar.stat()
    target = OUT_DIR / 'zar' / zar.stem
    stamp = target / '.source'
    tag = f'{zar.resolve()}|{stat.st_size}|{stat.st_mtime_ns}'
    try:
        if stamp.read_text(encoding='utf-8') == tag:
            return str(target)
    except OSError:
        pass
    shutil.rmtree(target, ignore_errors=True)
    for name in entries(zar):
        if name in EXTRACT_FILES or name.lower().startswith(EXTRACT_DIRS):
            out = target / name
            out.parent.mkdir(parents=True, exist_ok=True)
            result = _run(['--zar-extract', str(zar), name, str(out)])
            if result.returncode:
                raise RuntimeError(f'{zar.name}: cannot extract {name}: {result.stderr.strip()}')
    target.mkdir(parents=True, exist_ok=True)
    stamp.write_text(tag, encoding='utf-8')
    return str(target)


def safe_files_dir(game_dir):
    """files_dir for display checks (title, icon, version): the .zar itself when extraction fails."""
    try:
        return files_dir(game_dir)
    except (OSError, RuntimeError, ValueError):
        return game_dir


def pack(folder, zar, progress=print):
    """Packs a game folder into zar with tlg-probe; progress gets each "Pack: ..." line.
    Raises RuntimeError on failure (the probe removes the partial file)."""
    process = subprocess.Popen([str(PROBE), '--zar-pack', str(folder), str(zar)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, env=_env(), creationflags=NO_WINDOW)
    last = ''
    for line in process.stdout:
        last = line.strip()
        progress(last)
    if process.wait():
        raise RuntimeError(last or 'packing failed')
