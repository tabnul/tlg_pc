"""Scene check: enters every start point of the game, the boy stands still for 10 s. Per point: FPS per
second (with the shader compiles of that second), a screenshot at the end, the fade and the mission scene
from Lua, and what went wrong if anything. Windows only (tasklist/taskkill, screen capture).

  python scene_check.py LABEL --install DIR [--mode launch|jump] [--points FILE] [--seconds N] [--save DIR]
                  [--env NAME=VALUE ...] [--exe PATH] [--display Fullscreen|Windowed|keep] [--pack off|NAME|keep]

--install: the game folder of a release package or a dev install ("Play The Last Guardian.exe",
  out/tlg-probe.exe, user/). Defaults to TLG_INSTALL. It needs a build with TLG_FRAME_STATS_MS.
--mode launch (default): one fresh launch per point from the start save: title screen, then CONTINUE
  retargeted to the point (~40 s a point). Every point starts from the same clean state, and
  a hang or a crash costs only that point.
--mode jump: one session; the first point is entered as above, the next ones the way the game's own random
  scene test switches (checkpoint + RestartStageFromMenu, ~20 s a point). State carries over from the
  previous points, as in a long play session. After a point that fails, the game is closed and the next
  point gets a fresh launch. logs/ then holds one log per session, named after its first point.
--points FILE: STAGE:AREA lines instead of all points (scenes.json next to this script; points.py rebuilds it).
--seconds N: how long each point runs after its load (default 10). 0: each point stays until you close the
  game window, then the next one opens (to look at points yourself; no screenshot then).
--exe: copy this exe over out/tlg-probe.exe first.
The start save is --save DIR (a CUSA*.memory folder), else start.save next to this script when there is one, else
a copy of your own save (any save that has a CONTINUE works: the point replaces its checkpoint; the rest of the
story state comes from the save). Without a save of your own the tool makes the save folder the game would use.
Output port/out/scene-check/LABEL: results.csv (one row per point, appended as it goes; running the same label
again resumes after the points already in it), seconds.json (per point [fps, worst frame ms, compiles] per
second), shots/, logs/. report.py turns one or two runs into a page.
Your save, display mode and texture pack setting are put back at the end, also after Ctrl+C."""
import argparse, csv, ctypes, glob, json, os, re, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "out", "scene-check"))
ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("label")
ap.add_argument("--install", default=os.environ.get("TLG_INSTALL"))
ap.add_argument("--mode", choices=("launch", "jump"), default="launch")
ap.add_argument("--points")
ap.add_argument("--seconds", type=float, default=10)
ap.add_argument("--save")
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--exe")
ap.add_argument("--display", default="Fullscreen")
ap.add_argument("--pack", default="off")
A = ap.parse_args()
if not A.install or not os.path.exists(os.path.join(A.install, "Play The Last Guardian.exe")):
    ap.error("--install: the game folder with 'Play The Last Guardian.exe' (or set TLG_INSTALL)")

EXE = "tlg-probe.exe"
OUT = os.path.join(ROOT, A.label)
CSV = os.path.join(OUT, "results.csv")
SECONDS = os.path.join(OUT, "seconds.json")
EXEC = os.path.join(OUT, "exec.lua")
LOGS = os.path.join(A.install, "out", "logs")
INI = os.path.join(A.install, "user", "tlg.ini")
SETTINGS = os.path.join(os.environ.get("APPDATA", ""), "tlg-launcher", "settings.json")
FIRST_SAVE = os.path.join(HERE, "start.save")
SCENES = os.path.join(HERE, "scenes.json")
MEASURE_S, LOAD_S = A.seconds, 120
FPS = re.compile(r"Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms .*?; (\d+) shader/pipeline compiles")
# The curse spell (rune screen) of these points waits for a button at the scene start.
SKIP = {("s04_FirstAntenna", "_050_SleepingTrico_001"): "curse spell SpellFlashBack1 in onEnter, waits for input",
        ("s13_FirstFlight", "_020_Care_Start"): "curse spell SpellFlashBack2 in onEnter (FF_c01b_BoyCareStart)",
        ("s13_FirstFlight", "_087_Soldier64_Entry"): "curse spell SpellChant_FF65_01 (FF_f01_Soldier64_Entry)"}
FIELDS = ["k", "stage", "area", "scene", "status", "warn", "entry", "launch_s", "load_s", "fps", "fps_min", "fps_max",
          "worst_ms", "compiles", "stalls", "shot", "fade", "missions"]
HEAD = ("local ls = loadstring or load\n"
        "local function L(s) local g = ls('--TLGLOG ' .. tostring(s)) if g then g() end end\n")
# At the title screen: close the QA menu if the level select opened it, make the next LoadCheckPoint load the
# point instead of the save's own scene, then CONTINUE as the game's aging test does.
WARP_LUA = HEAD + """L('warp boot STAGE SCENE')
if ControllerMenu and ControllerMenu.Tick then
  local ok, e = pcall(function()
    UnRegisterTick(ControllerMenu.Tick)
    DelayExec(function() GUI_DrawText(0, 0, 1, 1, "0x0", " ") end)
  end)
  if not ok then L('warp menu close error: ' .. tostring(e)) end
end
local orig = LoadCheckPoint
LoadCheckPoint = function(...)
  LoadCheckPoint = orig
  local r = orig(...)
  GameFlag.Stage, GameFlag.Scene = 'STAGE', 'SCENE'
  L('warp retarget ' .. GameFlag.Stage .. ' ' .. GameFlag.Scene)
  return r
end
local ok, e = pcall(GameMenu_AutoAsing_SelectTitleContinue)
if not ok then L('warp error: ' .. tostring(e)) end
"""
# The game's own random scene test (gamesetup.lua RandomNextScene): the target goes into the checkpoint,
# then the pause menu's restart loads it.
JUMP = HEAD + """L('warp jump STAGE SCENE')
local ok, e = pcall(function()
  if SpellSystem and SpellSystem:IsPlay() then SpellSystem:Exit() end
  local stage, scene = GameFlag.Stage, GameFlag.Scene
  GameFlag.Stage, GameFlag.Scene = 'STAGE', 'SCENE'
  SaveCheckPoint()
  GameFlag.Stage, GameFlag.Scene = stage, scene
  GameSetupTbl:RestartStageFromMenu()
end)
if not ok then L('warp error: ' .. tostring(e)) end
"""
STAT = HEAD + """local ok, e = pcall(function()
  local fc = FindObject('_fade_controller')
  L('warp stat stage=' .. tostring(GameFlag and GameFlag.Stage) .. ' scene=' .. tostring(GameFlag and GameFlag.Scene)
    .. ' fade=' .. (fc and string.format('%.2f', fc.SystemValue) or 'nil'))
end)
if not ok then L('warp stat error: ' .. tostring(e)) end
"""


def running():
    return EXE in subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {EXE}"], capture_output=True, text=True).stdout


class Game:
    """One launch through the launcher with our env; the game log's lines with the time we first saw them."""

    def __init__(self, env, tag):
        self.launched, self.tag, self.log, self.seen = time.time(), tag, None, []
        full = dict(os.environ, TLG_SKIP_LOGOS="1", TLG_FPS="unlocked", TLG_HDR="0", TLG_FRAME_STATS="1",
                    TLG_FRAME_STATS_MS="1000", TLG_LUA_EXEC=EXEC)
        full.update(env)
        subprocess.Popen([os.path.join(A.install, "Play The Last Guardian.exe")], cwd=A.install, env=full,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def lines(self):
        if not self.log:
            logs = []
            for p in glob.glob(os.path.join(LOGS, "run-*.log")):
                try:
                    if os.path.getmtime(p) >= self.launched - 1:
                        logs.append((os.path.getmtime(p), p))
                except OSError:
                    continue  # the game deletes old logs while we scan
            if not logs:
                return self.seen
            self.log = max(logs)[1]
        with open(self.log, errors="replace") as f:
            new = f.read().splitlines()[len(self.seen):]
        now = time.monotonic()
        self.seen += [(now, l) for l in new]
        return self.seen

    def wait(self, seconds, until=lambda ls: False):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if until(self.lines()):
                return True
            if time.time() - self.launched > 30 and not running():
                return False
            time.sleep(0.2)
        return False

    def close(self):
        # the ending restarts the game (LoadExec): a new process can start just after the kill, so keep at it a while
        for _ in range(8):
            if running():
                subprocess.run(["taskkill", "/F", "/IM", EXE], capture_output=True)
            time.sleep(1)
        if self.log and os.path.exists(self.log):
            shutil.copy(self.log, os.path.join(OUT, "logs", self.tag + ".log"))


def shot(path):
    """Captures the game window's screen area (PrintWindow is black on Vulkan); returns its mean brightness."""
    from PIL import ImageGrab, ImageStat
    u = ctypes.windll.user32
    u.SetProcessDPIAware()
    pid = re.search(rf'"{re.escape(EXE)}","(\d+)"', subprocess.run(
        ["tasklist", "/FO", "CSV", "/NH", "/FI", f"IMAGENAME eq {EXE}"], capture_output=True, text=True).stdout)
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(h, _):
        p = ctypes.c_ulong()
        u.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(p))
        if pid and p.value == int(pid[1]) and u.IsWindowVisible(ctypes.c_void_p(h)):
            found.append(h)
        return True
    u.EnumWindows(each, 0)
    if not found:
        return -1
    h = ctypes.c_void_p(found[0])
    u.SetForegroundWindow(h)
    time.sleep(0.5)
    r = (ctypes.c_long * 4)()
    u.GetWindowRect(h, ctypes.byref(r))
    im = ImageGrab.grab(bbox=tuple(r), all_screens=True)
    im.save(path)
    return round(sum(ImageStat.Stat(im.convert("L")).mean), 1)


def write_exec(text):
    with open(EXEC, "w", newline="\n") as f:
        f.write(text + f"-- {time.time()}\n")  # a new chunk runs each time the file changes


def sfo_value(path, key):
    """A text value from the game's param.sfo."""
    data = open(path, "rb").read()
    keys, values, count = struct.unpack_from("<III", data, 8)
    for i in range(count):
        name, _, size, _, at = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        if data[keys + name:data.index(b"\0", keys + name)].decode() == key:
            return data[values + at:values + at + size].rstrip(b"\0").decode()
    return None


def save_dir():
    """The game's memory save in the install (savedata/<user>/<serial>.memory); the start save goes in its place.
    Without a save yet: the folder the game will use, named from its param.sfo the way the port names it."""
    found = glob.glob(os.path.join(A.install, "user", "savedata", "*", "CUSA*.memory"))
    if found:
        return found[0]
    game = json.load(open(SETTINGS, encoding="utf-8")).get("game_dir") if os.path.exists(SETTINGS) else None
    sfo = os.path.join(game or "", "sce_sys", "param.sfo")
    if not os.path.exists(sfo):
        sys.exit("no save in the install and no game folder set in the launcher: start the game once")
    title = sfo_value(sfo, "INSTALL_DIR_SAVEDATA") or sfo_value(sfo, "TITLE_ID")
    return os.path.join(A.install, "user", "savedata", "1", f"{title}.memory")


def launch(stage, scene, env, tag, row):
    """Fresh launch from the start save: title screen, then CONTINUE retargeted to the point. Returns the game and
    the index of the log line the point's load follows (None when it never got there; row["status"] says why)."""
    shutil.rmtree(SAVE, ignore_errors=True)
    shutil.copytree(START, SAVE)
    write_exec("-- warp\n")
    game = Game(env, tag)
    if not game.wait(LOAD_S, lambda ls: any("opA_010 titleScreen" in l for _, l in ls)):
        row["status"] = "no title"
        return game, None
    game.wait(1)
    write_exec(WARP_LUA.replace("STAGE", stage).replace("SCENE", scene))
    if not game.wait(20, lambda ls: any("Lua: warp retarget" in l or "Lua: warp error" in l for _, l in ls)):
        row["status"] = "no continue"
        return game, None
    seen = next(i for i, (_, l) in enumerate(game.seen) if "Lua: warp retarget" in l or "Lua: warp error" in l)
    if "warp error" in game.seen[seen][1]:
        row["status"] = "warp error"
        return game, None
    return game, seen


def visit(game, k, stage, area, scene, env):
    """One point: a fresh launch (game None) or a jump in the running game, then MEASURE_S standing still.
    Returns the game, the row and the per-second [fps, worst ms, compiles] list."""
    tag = f"{k:03d}_{stage}_{area}"
    row = dict(k=k, stage=stage, area=area, scene=scene, status="ok", warn="", entry="jump" if game else "launch")
    secs = []
    entered = time.monotonic()
    if game:
        seen = len(game.lines())
        write_exec(JUMP.replace("STAGE", stage).replace("SCENE", scene))
    else:
        game, seen = launch(stage, scene, env, tag, row)
        if seen is None:
            return game, row, secs
    started = time.monotonic()

    def loaded(ls):
        new = [l for _, l in ls[seen:]]
        if any("Lua: warp error" in l for l in new):
            return True
        i = next((j for j, l in enumerate(new) if "blocking overlay started" in l), None)
        return i is not None and any("blocking overlay ended" in l for l in new[i:])
    if not game.wait(LOAD_S, loaded):
        row["status"] = "crash" if not running() else "no load"
        return game, row, secs
    if any("Lua: warp error" in l for _, l in game.seen[seen:]):
        row["status"] = "warp error"
        return game, row, secs
    t0 = time.monotonic()
    row["load_s"] = round(t0 - started, 1)
    row["launch_s"] = round(t0 - entered, 1)
    end = len(game.seen)
    stop = t0 + (MEASURE_S or float("inf")) + 0.5
    if MEASURE_S:
        # standing still; a new loading screen in it means a reload (caught, game over)
        reload = game.wait(MEASURE_S, lambda ls: any("blocking overlay started" in l for _, l in ls[end:]))
    else:  # --seconds 0: the user looks around until they close the game
        while running():
            game.wait(2)
        reload = any("blocking overlay started" in l for _, l in game.lines()[end:])
    restarted = lambda: any("sceSystemServiceLoadExec" in l for _, l in game.lines()[end:])
    if any("Fault: exception" in l for _, l in game.lines()[end:]) or MEASURE_S and not running() and not restarted():
        row["status"] = "crash"
        return game, row, secs
    fade, row["shot"] = None, -1
    if MEASURE_S:
        row["shot"] = shot(os.path.join(OUT, "shots", tag + ".png"))
        write_exec(STAT)
        game.wait(5, lambda ls: any("Lua: warp stat" in l for _, l in ls[end:]))
        stat = next((l for _, l in game.seen[end:] if "Lua: warp stat" in l), "")
        fade = re.search(r"fade=([\d.]+)", stat)
    row["fade"] = fade[1] if fade else ""
    for t, l in game.seen[end:]:
        if t <= stop and (m := FPS.match(l)):
            secs.append([float(m[1]), float(m[2]), int(m[3])])
    row["stalls"] = sum(1 for t, l in game.seen[end:] if t <= stop and l.startswith("Stall:"))
    missions = [l[13:] for _, l in game.seen[seen:] if l.startswith("Lua: mission ") and "_Worker" not in l]
    row["missions"] = " | ".join(missions)
    if secs:
        row.update(fps=round(sum(s[0] for s in secs) / len(secs), 1), fps_min=min(s[0] for s in secs),
                   fps_max=max(s[0] for s in secs), worst_ms=max(s[1] for s in secs),
                   compiles=sum(s[2] for s in secs))
    warns = []
    if restarted():
        row["status"] = "restart"
    elif reload:
        row["status"] = "reload"
    elif fade and float(fade[1]) < 0.05 and 0 <= row["shot"] < 12:
        row["status"] = "black"
    elif not secs:
        row["status"] = "no stats"
    if not any(scene in m for m in missions):
        warns.append("mission not seen")
    if 0 <= row["shot"] < 12 and row["status"] == "ok":
        warns.append("dark shot")
    row["warn"] = ", ".join(warns)
    return game, row, secs


def set_display(mode):
    text = open(SETTINGS, encoding="utf-8").read()
    old = re.search(r'"display_mode":\s*"(\w+)"', text)[1]
    open(SETTINGS, "w", encoding="utf-8").write(re.sub(r'("display_mode":\s*)"\w+"', rf'\1"{mode}"', text))
    return old


def set_pack(pack):
    lines = open(INI, encoding="utf-8").read().splitlines()
    old = next((l.split("=", 1)[1] for l in lines if l.startswith("texture_pack=")), None)
    lines = [l for l in lines if not l.startswith("texture_pack=")] + [f"texture_pack={pack}"]
    with open(INI, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return old


def main():
    global SAVE, START, MEASURE_S
    if running():
        sys.exit("the game is running")
    if A.exe:
        shutil.copy(A.exe, os.path.join(A.install, "out", EXE))
    if not os.path.exists(SCENES):
        sys.exit("no scenes.json: run points.py first")
    pts = json.load(open(SCENES))
    order = [(p["stage"], p["area"]) for p in pts]
    index = {p: n for n, p in enumerate(order, 1)}  # k: the point's place in the full list
    scenes = {(p["stage"], p["area"]): p["scene"] for p in pts}
    # a points line is stage:area, or stage:area:seconds to measure that point for its own time
    lines = [l.strip().split(":") for l in open(A.points) if ":" in l] if A.points else [list(p) for p in order]
    todo = [tuple(l[:2]) for l in lines]
    own_s = [float(l[2]) if len(l) > 2 else A.seconds for l in lines]
    unknown = [":".join(p) for p in todo if p not in scenes]
    if unknown:
        sys.exit(f"not in scenes.json: {', '.join(unknown)}")
    own_s = [s for p, s in zip(todo, own_s) if p not in SKIP]
    todo = [p for p in todo if p not in SKIP]
    for d in ("shots", "logs"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    done, fields = set(), FIELDS
    if os.path.exists(CSV):
        done = {(r["stage"], r["area"]) for r in csv.DictReader(open(CSV))}
        fields = next(csv.reader(open(CSV)))
    secs_all = json.load(open(SECONDS)) if os.path.exists(SECONDS) else {}
    SAVE = save_dir()
    backup = os.path.join(OUT, "save_backup")
    if not os.path.exists(backup):  # empty when there is no save yet
        shutil.copytree(SAVE, backup) if os.path.isdir(SAVE) else os.makedirs(backup)
    START = A.save or (FIRST_SAVE if os.path.exists(FIRST_SAVE) else backup)
    if not os.path.isdir(START) or not os.listdir(START):
        sys.exit(f"no start save in {START}: give one with --save DIR")
    env = dict(e.split("=", 1) for e in A.env)
    old_display = set_display(A.display) if A.display != "keep" else None
    old_pack = set_pack(A.pack) if A.pack != "keep" else None
    print(f"out {OUT}: {len(todo)} points, {len(done)} done, mode {A.mode}, env {env or '-'}", flush=True)
    if not MEASURE_S:
        print("each point stays until you close the game window; then the next one opens", flush=True)
    t_start, game, visited = time.monotonic(), None, 0
    try:
        new = not os.path.exists(CSV)
        with open(CSV, "a", newline="") as f:
            w = csv.DictWriter(f, fields, extrasaction="ignore")
            if new:
                w.writeheader()
            for n, (stage, area) in enumerate(todo, 1):
                if (stage, area) in done:
                    continue
                k = index.get((stage, area), 0)
                MEASURE_S = own_s[n - 1]
                game, row, secs = visit(game if A.mode == "jump" else None, k, stage, area, scenes[(stage, area)], env)
                if A.mode == "launch" or row["status"] != "ok" or not running():
                    game.close()  # in jump mode the next point starts clean after a failure
                    game = None
                w.writerow(row)
                f.flush()
                key = f"{k:03d}_{stage}_{area}"
                while key in secs_all:  # a point listed twice (e.g. the same scene at start and end)
                    key += "+"
                secs_all[key] = secs
                json.dump(secs_all, open(SECONDS, "w"))
                visited += 1
                left = (time.monotonic() - t_start) / visited * (len(todo) - n) / 60
                print(f"{n:3}/{len(todo)} {k:03d} {stage} {area:32} {row['status']:10} {row.get('fps', ''):>5} fps"
                      f"  load {row.get('load_s', '')} s  {row['entry']}  {row['warn']}  (~{left:.0f} min left)", flush=True)
    finally:
        if game:
            game.close()
        if old_display:
            set_display(old_display)
        if old_pack is not None:
            set_pack(old_pack)
        shutil.rmtree(SAVE, ignore_errors=True)
        if os.listdir(backup):
            shutil.copytree(backup, SAVE)
        print("your save, display mode and texture pack are restored", flush=True)


main()
