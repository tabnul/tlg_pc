# SPDX-License-Identifier: MIT
"""Benchmark sweep behind the launcher's Debug section: the game visits a list of start points on
its own (TLG_START_POINT warps there right after the title screen), stands still for a fixed number
of seconds and records the FPS of every second. The results use the same columns and files as the
developers' scene check (tools/scene-check), so a report from any PC can be compared directly.

Everything lands in one folder under <user data>/benchmark/ and is zipped at the end:
  results.csv   one row per scene (status, load time, FPS avg/min/max, worst frame, compiles, ...)
  seconds.json  the per-second FPS samples per scene (what the comparisons are made from)
  meta.json     build version, OS, CPU, GPU, RAM, the bench parameters and settings snapshots
  logs/         the game log of every scene, recorded at the advanced logging level (a full
                219-scene run zips to roughly 5 MB)

The player's own saves are copied aside before the run and put back afterwards (practice_save);
every scene starts from a bundled clean save (scene-check/start.save), so the run never touches
or depends on the player's progress."""

import csv
import json

import re
import shutil
import sys
import threading
import time
import zipfile
from pathlib import Path

import practice_save
import report
import savemanager
import start_points
import tlg_run

FPS_LINE = re.compile(r"Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms .*?; (\d+) shader/pipeline compiles")
START_LINE = re.compile(r"Fun: start at (\S+) (\S+) \((\S+)\)")
# Same columns as tools/scene-check/scene_check.py, so its report.py reads our results.csv as-is.
FIELDS = ["k", "stage", "area", "scene", "status", "warn", "entry", "launch_s", "load_s", "fps", "fps_min", "fps_max",
          "worst_ms", "compiles", "stalls", "shot", "fade", "mb_s", "top_write", "changed_pct", "missions"]
# The curse spell (rune screen) of these points waits for a button at the scene start (scene check skips them too).
SKIP = {("s04_FirstAntenna", "_050_SleepingTrico_001"),
        ("s13_FirstFlight", "_020_Care_Start"),
        ("s13_FirstFlight", "_087_Soldier64_Entry")}
EXAMPLE_POINTS = ("s04_FirstAntenna:_090_GardenTerrace_001",
                  "s10_Pit:_040_Hungry_start_001",
                  "s14_CondorClash:_000_Start",
                  "s03_Canyon:_030_InsideLake_001")
WARP_WAIT_S = 120      # process start -> "Fun: start at" (title screen + the 1 s warp trigger)
LOAD_WAIT_S = 150      # warp -> end of the blocking load overlay
EXIT_WAIT_S = 15
OVERHEAD_S = 35        # per-scene launch + load, for the time estimate

# The game runs with the launcher's own settings (graphics, frame rate, display...), recorded in
# meta.json. Only what would break the measurement is forced: boot logos skipped, the per-second
# frame stats on, frame generation off (generated frames would inflate the numbers) and the end
# credits' own 30 FPS cap lifted (a capped reading says nothing).
BENCH_ENV = {"TLG_SKIP_LOGOS": "1", "TLG_FG": "0",
             "TLG_FRAME_STATS": "1", "TLG_FRAME_STATS_MS": "1000"}
BENCH_INI = {"credits_fps": "unlocked"}


def default_points():
    """Every start point, without the ones that wait for input."""
    return [p for p in start_points.POINTS if p not in SKIP]


def parse_points(text):
    """One 'stage:start point' per line (comments after # and blank lines ignored);
    returns (points, unknown lines)."""
    known = set(start_points.POINTS) - SKIP
    points, bad = [], []
    for raw in text.splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        stage, _, area = line.replace(' ', ':').partition(':')
        point = (stage, area)
        if point in known:
            if point not in points:
                points.append(point)
        else:
            bad.append(line)
    return points, bad


def estimate_minutes(count, seconds):
    return max(1, round(count * (seconds + OVERHEAD_S) / 60))


def save_template():
    """The bundled clean save's folder (scene-check/start.save): one subfolder per save slot."""
    if tlg_run.FROZEN:
        bundled = Path(getattr(sys, '_MEIPASS', '')) / 'scene-check' / 'start.save'
        if bundled.is_dir():
            return bundled
    return tlg_run.PORT_DIR / 'tools' / 'scene-check' / 'start.save'


def save_place(data, game_dir):
    """Where the game's memory save lives (savedata/<user>/<serial>.memory, a folder): the existing
    save's spot, else the one the game will use, named from the game's param.sfo."""
    for found in sorted((data / 'savedata').glob('*/*.memory')):
        return found.relative_to(data)
    sfo = savemanager.read_sfo((Path(game_dir) / 'sce_sys' / 'param.sfo').read_bytes())
    title = sfo.get('INSTALL_DIR_SAVEDATA') or sfo.get('TITLE_ID') or 'CUSA03745'
    return Path('savedata') / '1' / f'{title}.memory'


class Sweep:
    """Runs the whole bench on a worker thread. notify(kind, **data) is called with:
    'status' (text), 'point' (index/total/stage/area), 'row' (a finished scene's dict),
    'done' (zip path or None), 'error' (text). stop_event ends the run after the current scene;
    its second set (or kill) also kills the running game."""

    def __init__(self, settings, points, seconds, notify, stop_event):
        self.settings = dict(settings)
        self.points = list(points)
        self.seconds = max(5, int(seconds))
        self.notify = notify
        self.stop = stop_event
        self.process = None

    # ---- the run ----

    def run(self):
        out = None
        try:
            out = self._run()
        except Exception as error:  # noqa: BLE001  (the worker thread must never die silently)
            self.notify('error', text=f'{type(error).__name__}: {error}')
        self.notify('done', zip=out)

    def _run(self):
        data = tlg_run.user_dir()
        data.mkdir(parents=True, exist_ok=True)
        template = save_template()
        if not template.is_dir():
            raise RuntimeError(f'bundled start save missing: {template}')
        place = save_place(data, self.settings["game_dir"])
        stamp = time.strftime('%Y-%m-%d_%H-%M')
        out = data / 'benchmark' / f'run_{stamp}'
        (out / 'logs').mkdir(parents=True, exist_ok=True)

        self.notify('status', text='prepare')
        settings = dict(self.settings, level_select=False, debug_logging=True,
                        skip_logos=True)  # launcher settings, but full logs and no boot logos
        notes = []
        tlg_run.prepare(settings, log=lambda line: notes.append(line) if line.startswith('Frame rate') else None)

        ini = data / 'tlg.ini'
        before = {key: tlg_run.read_ini(ini).get(key) for key in BENCH_INI}
        practice_save.begin(data, 'benchmark')
        try:
            tlg_run.update_ini(ini, BENCH_INI)
            rows, seconds_map = [], {}
            for index, (stage, area) in enumerate(self.points):
                if self.stop.is_set():
                    break
                self.notify('point', index=index, total=len(self.points), stage=stage, area=area)
                self._place_save(data, template, place)
                row, samples, log = self._visit(index, stage, area, settings, notes)
                rows.append(row)
                if samples:
                    seconds_map[f'{index:03d}_{stage}_{area}'] = samples
                self._write(out, rows, seconds_map)
                try:
                    shutil.copy2(log, out / 'logs' / f'{index:03d}_{stage}_{area}.log')
                except OSError:
                    pass
                self.notify('row', row=row)
            self._meta(out, rows, data, ini)
            return self._zip(out)
        finally:
            restore = {key: value for key, value in before.items() if value is not None}
            drop = [key for key, value in before.items() if value is None]
            if restore:
                tlg_run.update_ini(ini, restore)
            if drop:
                self._drop_ini_keys(ini, drop)
            practice_save.end(data)

    def _place_save(self, data, template, place):
        """A fresh copy of the clean save at the game's own save spot."""
        savedata = data / 'savedata'
        if savedata.exists():
            shutil.rmtree(savedata)
        shutil.copytree(template, data / place)

    def _visit(self, index, stage, area, settings, notes):
        """One scene: launch, wait for the warp and the load, measure, close. Returns (row, samples, log)."""
        env = dict(BENCH_ENV, TLG_START_POINT=f'{stage}:{area}')
        budget = WARP_WAIT_S + LOAD_WAIT_S + self.seconds + 60
        started = time.time()
        process, log = tlg_run.launch(settings, seconds=budget, extra_env=env, header=notes)
        self.process = process
        row = dict.fromkeys(FIELDS, '')
        row.update(k=index, stage=stage, area=area, scene='', status='ok', entry='warp')
        samples, missions, stalls = [], [], 0
        try:
            import logtail
            tail = logtail.LogTail(log)
            phase, deadline = 'warp', started + WARP_WAIT_S
            measure_end = None
            while True:
                done = process.poll() is not None
                for line in tail.poll(final=done):
                    now = time.time()
                    match = START_LINE.search(line)
                    if match:
                        row['scene'] = match.group(3)
                        if phase == 'warp':
                            phase, deadline = 'load', now + LOAD_WAIT_S
                            row['launch_s'] = round(now - started, 1)
                    elif 'start point' in line and 'not in the list' in line:
                        row['status'] = 'warp error'
                    elif 'blocking overlay ended.' in line and phase == 'load':
                        phase = 'measure'
                        row['load_s'] = round(now - started - (row['launch_s'] or 0), 1)
                        measure_end = now + self.seconds
                    elif 'blocking overlay started.' in line and phase == 'measure':
                        row['status'] = 'reload'
                    elif 'Fault: exception' in line:
                        row['status'] = 'crash'
                    elif 'sceSystemServiceLoadExec' in line:
                        row['status'] = 'restart'
                    elif phase == 'measure':
                        if line.startswith('Lua: mission '):
                            missions.append(line.split()[-1])
                        elif line.startswith('Stall:'):
                            stalls += 1
                        else:
                            stats = FPS_LINE.search(line)
                            if stats:
                                samples.append([float(stats.group(1)), float(stats.group(2)),
                                                int(stats.group(3))])
                if done:
                    if row['status'] == 'ok' and phase != 'done':
                        row['status'] = 'crash'
                    break
                now = time.time()
                if phase == 'measure' and now >= measure_end:
                    phase = 'done'
                    break
                if phase in ('warp', 'load') and now > deadline:
                    row['status'] = 'no title' if phase == 'warp' else 'no load'
                    break
                if self.stop.is_set() and phase != 'measure':
                    row['status'] = 'stopped'
                    break
                time.sleep(0.2)
        finally:
            self._close(process)
            self.process = None
        if samples:
            fps = [s[0] for s in samples]
            row['fps'] = round(sum(fps) / len(fps), 1)
            row['fps_min'] = round(min(fps), 1)
            row['fps_max'] = round(max(fps), 1)
            row['worst_ms'] = round(max(s[1] for s in samples), 1)
            row['compiles'] = sum(s[2] for s in samples)
        elif row['status'] == 'ok':
            row['status'] = 'no stats'
        row['stalls'] = stalls
        row['missions'] = ' '.join(dict.fromkeys(missions))
        if row['status'] == 'ok' and row['scene'] and row['scene'] not in row['missions']:
            row['warn'] = 'mission not seen'
        return row, samples, log

    def _close(self, process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(EXIT_WAIT_S)
            except Exception:  # noqa: BLE001
                process.kill()
                process.wait(EXIT_WAIT_S)

    def kill_game(self):
        process = self.process
        if process and process.poll() is None:
            process.kill()

    # ---- output ----

    def _write(self, out, rows, seconds_map):
        with open(out / 'results.csv', 'w', newline='', encoding='utf-8') as handle:
            writer = csv.DictWriter(handle, FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        (out / 'seconds.json').write_text(json.dumps(seconds_map), encoding='utf-8')

    def _meta(self, out, rows, data, ini):
        gathered = {}
        try:
            gathered = report.gather([tlg_run.PORT_DIR], tlg_run.LOG_DIR, self.settings.get('game_dir', ''),
                                     savemanager.read_sfo)
        except Exception as error:  # noqa: BLE001
            gathered = {'error': str(error)}
        meta = {'kind': 'tlg launcher benchmark', 'when': time.strftime('%Y-%m-%d %H:%M'),
                'seconds_per_scene': self.seconds, 'points': len(self.points),
                'finished': sum(1 for r in rows if r['status'] == 'ok'),
                'system': gathered,
                'env': BENCH_ENV, 'ini_overrides': BENCH_INI,
                'tlg_ini': tlg_run.read_ini(ini),
                'settings': {k: v for k, v in self.settings.items()
                             if k not in ('game_dir', 'key_map', 'background')}}
        (out / 'meta.json').write_text(json.dumps(meta, indent=1, default=str), encoding='utf-8')

    def _zip(self, out):
        names = report.own_names()
        target = out.parent / f'tlg-benchmark-{out.name[4:]}.zip'
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted(out.rglob('*')):
                if not file.is_file():
                    continue
                rel = file.relative_to(out).as_posix()
                if file.suffix in ('.csv', '.json', '.log', '.txt'):
                    text = file.read_text(encoding='utf-8', errors='replace')
                    bundle.writestr(rel, report.redact(text, names)[0])
                else:
                    bundle.write(file, rel)
        return target

    @staticmethod
    def _drop_ini_keys(ini, keys):
        try:
            lines = ini.read_text(encoding='utf-8').splitlines()
        except OSError:
            return
        kept = [line for line in lines
                if line.split('=', 1)[0].strip() not in keys]
        ini.write_text('\n'.join(kept) + '\n', encoding='utf-8')
