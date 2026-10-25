#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: record a route in an installed port and replay it (src/runtime_pad.c BB_PAD_RECORD /
BB_PAD_REPLAY), from the same save and the same moment every run.

    python tools/sotc_route.py --install DIR --record out/routes/walk.txt
    python tools/sotc_route.py --install DIR --replay out/routes/walk.txt [--env NAME=VALUE ...]

--record copies the install's save next to the route (walk.save/), starts the game through the
launcher (--play), gets past the title by itself and starts recording 1 s after gameplay begins
(the second "st_gameflow() starting game" in the log). Play; F9 or closing the window ends it.
--replay puts that save back (the install's own save is restored afterwards), starts the replay at
the same moment and closes the game when it has finished. The run's log is user/last_run.log;
SOTC_FPS, SOTC_FRAME_LOG and SOTC_STALL_MS=40 are always set (tools/spike_report.py reads them).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

GAME_START = 'st_gameflow() starting game'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--install', required=True, help='the port folder (launcher/, user/)')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--record')
    mode.add_argument('--replay')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--timeout', type=float, default=3600)
    args = parser.parse_args()
    install = Path(args.install)
    user = install / 'user'
    saves = user / 'savedata' / '1' / 'CUSA08809'
    route = Path(args.record or args.replay).resolve()
    snapshot = route.with_suffix('.save')
    if subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe'], capture_output=True, text=True).stdout.count('bb-probe.exe'):
        print('INVALID: the game is running')
        return 1
    restore = None
    if args.record:
        route.parent.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(snapshot, ignore_errors=True)
        shutil.copytree(saves, snapshot)
    else:
        if not route.is_file() or not snapshot.is_dir():
            print(f'INVALID: need {route} and {snapshot}')
            return 1
        restore = user / 'savedata' / '1' / f'CUSA08809.route-backup-{time.strftime("%Y%m%d-%H%M%S")}'
        shutil.copytree(saves, restore)
        shutil.rmtree(saves)
        shutil.copytree(snapshot, saves)
    pad = user / 'route-pad.txt'
    pad.write_text('')
    env = dict(os.environ, BB_PAD_FILE=str(pad), SOTC_FPS='1', SOTC_FRAME_LOG='1', SOTC_STALL_MS='40',
               SOTC_SAVE_PROMPT='auto')
    env['BB_PAD_RECORD' if args.record else 'BB_PAD_REPLAY'] = str(route)
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    log = user / 'last_run.log'
    launched = time.time()
    launcher = subprocess.Popen([sys.executable, str(install / 'launcher' / 'sotc_launcher_win.py'), '--play'],
                                cwd=install, env=env)
    try:
        return drive(args, pad, log, launched, launcher)
    finally:
        if restore:
            # the game may still be writing the save: wait for it to exit first
            for _ in range(60):
                if not running():
                    break
                time.sleep(1)
            shutil.rmtree(saves, ignore_errors=True)
            shutil.copytree(restore, saves)
            shutil.rmtree(restore)
            print('your save is restored')


def running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe'], capture_output=True, text=True).stdout
    return 'bb-probe.exe' in out


def drive(args, pad, log, launched, launcher):
    start = time.monotonic()

    def text():
        try:
            if log.stat().st_mtime < launched - 1:
                return ''
            return log.read_text(encoding='utf-8', errors='replace')
        except OSError:
            return ''

    def wait(seconds, until=lambda t: False):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            t = text()
            if until(t):
                return t
            if time.monotonic() - start > 20 and not running():
                return None
            time.sleep(0.2)
        return text()

    def starts(t):
        return t.count(GAME_START) if t else 0

    def hold(button, seconds, until):
        pad.write_text(button)
        t = wait(seconds, until)
        pad.write_text('')
        return t

    t = wait(120, lambda t: 'pad opened' in t)
    if not t:
        print('INVALID: the game did not start')
        return 1
    # Opening cinematic ("hold X to skip"), title, menu (Continue is selected): hold and tap X
    # until the save starts loading.
    t = wait(8)
    while t is not None and starts(t) < 1 and time.monotonic() - start < 240:
        t = hold('cross', 3, lambda t: starts(t) >= 1)
        if t is not None and starts(t) < 1:
            t = wait(1.5, lambda t: starts(t) >= 1)
    if t is None or starts(t) < 1:
        print('INVALID: the save did not load')
        return 1
    # Gameplay begins at the second start; a cutscene before it is skipped with X held.
    t = wait(25, lambda t: starts(t) >= 2)
    while t is not None and starts(t) < 2 and time.monotonic() - start < 360:
        t = hold('cross', 5, lambda t: starts(t) >= 2)
        if t is not None and starts(t) < 2:
            t = wait(2, lambda t: starts(t) >= 2)
    if t is None or starts(t) < 2:
        print('INVALID: gameplay did not begin')
        return 1
    wait(1)
    if args.record:
        pad.write_text('record')
        print('READY: recording. Play the route; F9 stops it, or close the window.', flush=True)
        while running() and time.monotonic() - start < args.timeout:
            time.sleep(1)
        lines = len(Path(args.record).read_text().splitlines()) if Path(args.record).is_file() else 0
        print(f'recorded {lines} pad changes to {args.record}')
        return 0 if lines else 1
    pad.write_text('replay')
    print('replaying', flush=True)
    t = wait(args.timeout, lambda t: 'pad replay finished' in t)
    if t is None:
        print('INVALID: the game ended during the replay')
        return 1
    wait(3)
    subprocess.run(['taskkill', '/F', '/IM', 'bb-probe.exe'], capture_output=True)
    launcher.wait(30)
    print(f'replay done, log {log}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
