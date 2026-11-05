# SPDX-License-Identifier: GPL-2.0-or-later
"""Practice starts (the launcher's Start at...): the player's saves are put back after the game closes.

begin() copies <user>/savedata to <user>/savedata.practice and only then writes the note
<user>/practice.json; end() puts the copy back. While the note exists the copy is the player's real
save, so a launcher that crashed or a PC that went off leaves the note behind and the next end()
(the launcher calls recover() at start) still puts the save back. Each step is a copy to a temporary
name or a rename, so a stop at any point leaves either the old or the new folder, never half of one:

  begin: copy savedata -> savedata.practice.copying, rename -> savedata.practice, write the note.
  end:   rename savedata -> savedata.played, savedata.practice -> savedata, delete the note,
         delete savedata.played. A stop between the renames: end() sees no savedata and finishes.
No Tk, no Qt.
"""
import json
from pathlib import Path
import shutil
import time

NOTE = 'practice.json'
COPY = 'savedata.practice'
PLAYED = 'savedata.played'


def _paths(user_dir):
    user_dir = Path(user_dir)
    return user_dir / 'savedata', user_dir / COPY, user_dir / NOTE, user_dir / PLAYED


def active(user_dir):
    """True while a practice start has not been put back yet."""
    return _paths(user_dir)[2].is_file()


def begin(user_dir, point=''):
    """Copies the saves aside before a practice start. Raises OSError when that fails (then the
    game must not start). Safe to call with a practice start left over: that one is put back first."""
    savedata, copy, note, _ = _paths(user_dir)
    if note.is_file():
        end(user_dir)
    shutil.rmtree(copy, ignore_errors=True)   # a leftover without a note is not the player's save
    had = savedata.is_dir()
    if had:
        temp = copy.with_name(COPY + '.copying')
        shutil.rmtree(temp, ignore_errors=True)
        shutil.copytree(savedata, temp)
        temp.rename(copy)
    temp_note = note.with_name(NOTE + '.tmp')
    temp_note.write_text(json.dumps({'point': point, 'had_saves': had, 'time': time.time()}), encoding='utf-8')
    temp_note.replace(note)


def end(user_dir):
    """Puts the saves from before the practice start back. Returns True when it did. Raises OSError
    when it cannot (the note stays, so the next call tries again)."""
    savedata, copy, note, played = _paths(user_dir)
    if not note.is_file():
        return False
    try:
        had = bool(json.loads(note.read_text(encoding='utf-8')).get('had_saves', True))
    except (OSError, ValueError):
        had = copy.is_dir()
    if had and not copy.is_dir():
        if savedata.is_dir():   # stopped after the swap, before the note was deleted
            note.unlink()
            shutil.rmtree(played, ignore_errors=True)
            return True
        raise OSError(f'{copy}: the saves from before the practice start are missing')
    shutil.rmtree(played, ignore_errors=True)
    if savedata.exists():
        savedata.rename(played)
    if had:
        copy.rename(savedata)
    note.unlink()
    shutil.rmtree(played, ignore_errors=True)
    return True


def recover(user_dir):
    """At launcher start: a practice start that was never put back (launcher crash, power off) is
    put back now. Returns True when it did. Call only while the game does not run."""
    return end(user_dir) if active(user_dir) else False
