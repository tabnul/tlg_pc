# Scene check

The scene check visits every start point of the game by itself. Per point it loads the scene, lets the
boy stand still for 10 seconds and records:
- the FPS of every second, with the shader compiles in it;
- a screenshot at the end;
- a status that says what went wrong, if anything.

Use it to check a change across the whole game rather than on one route: run it before and after, then
compare the two runs on one page. It can also open a list of scenes one by one so you can look at them
yourself.

The tool lives in [port/tools/scene-check](../port/tools/scene-check).

## What you need
- Windows, and Python 3.8 or newer with Pillow (`pip install pillow`).
- A build of this fork that includes the scene check. The tool reads the per-second FPS lines that
  `TLG_FRAME_STATS_MS` turns on.
- The game set up in the launcher (CUSA03745 or CUSA03627 with update 01.03). We tested on the EU
  version, CUSA03745.

You don't need a save of your own, and you don't need to switch on the debug menu:
- **Save:** the tool comes with a start save (`start.save`). It continues at the first scene in Trico's
  cave (stage `s02_TricosCave`, start point `_010_Care_Step1`). Every point replaces that save's
  checkpoint, so the story state comes from the save. When you have never saved, the tool creates the
  save folder the game would use.
- **Debug menu:** *Level select and QA menu* under Debug in the launcher can be on or off. The tool
  doesn't use it and doesn't change it. When it is on, the tool closes the QA menu if it is open at the
  title screen.

## Steps
1. `python scene_check.py LABEL --install DIR` runs the check.
   - `DIR` is the game folder of the release package or dev install, the one with
     "Play The Last Guardian.exe".
   - Hands off while it runs: the game takes the focus for every screenshot.
2. `python report.py LABEL` writes `port\out\scene-check\LABEL\index.html`.
   `python report.py LABEL --base OTHER` writes `LABEL\vs-OTHER.html`, which puts both runs side by side.

The output of a run goes to `port\out\scene-check\LABEL`:
- `results.csv`: one row per point.
- `seconds.json`: FPS, worst frame and compiles per second.
- `shots\` and `logs\`.

Running the same label again resumes after the points already done. At the end, also after Ctrl+C, the
tool puts back your save, the launcher's display mode and the texture pack setting.

## The scene list
`scenes.json` lists the 222 start points the check visits, in story order. Each entry gives the stage,
the start point and its mission scene. The list comes from the game's own start points
(`StartAreas` in each stage's `regionsetting.lua`), the same ones the developers' QA menu offers:
- Only the story stages, `s01` to `s18`.
- Without the entries the game marks as test only, and without the ones that have no mission scene.
- Not the title screen's opening movie, and not the stage's "Bottom" fallback.

The tool skips three points, so a full run visits 219. Their rune scene waits for a button press at the
start:
- s04_FirstAntenna `_050_SleepingTrico_001`
- s13_FirstFlight `_020_Care_Start`
- s13_FirstFlight `_087_Soldier64_Entry`

The list was made from update 01.03. `python points.py` builds it again from your own game files
(`trico_boot.psarc` and the update's `trico_bootp.psarc`), for example after a game update. It reads the
game folder from the launcher, or from `--game DIR`.

## Options
| Option | What |
|--------|------|
| `--mode launch` | Default. A fresh launch per point, about 40 s each (about 2.5 hours for all). Every point starts from the same state, and a crash costs only that point. |
| `--mode jump` | One session, about 20 s a point. It switches scenes the way the game's own random scene test does (checkpoint plus restart from the pause menu), so state carries over as in a long play session. After a failed point the game restarts. |
| `--points FILE` | Only these points, one `STAGE:AREA` per line (for example `s03_Canyon:_030_InsideLake_001`). |
| `--seconds N` | How long each point runs after its load (default 10). With `0`, each point stays until you close the game window, then the next one opens, so you can look at a list of scenes yourself. Closing the window ends that game, so with `0` every point is a fresh launch. With `--mode jump`, give a number of seconds instead. |
| `--save DIR` | Start from this save (a `CUSA*.memory` folder) instead of `start.save`. |
| `--env NAME=VALUE` | Extra environment for the game, for example a setting you want to A/B. |
| `--exe PATH` | Copy this exe over `out\tlg-probe.exe` first. |
| `--display`, `--pack` | Display mode (default `Fullscreen`) and texture pack (default `off`) for the run, or `keep`. |

## Status
| Status | Meaning |
|--------|---------|
| `ok` | Loaded and measured. |
| `reload` | A new loading screen within the measured seconds (the boy was caught or fell, or the scene loads again by itself). |
| `black` | The screen stayed faded to black. |
| `crash` | The game closed. |
| `restart` | The game restarted itself (the ending does this). The FPS before it still counts. |
| `no load` | The load did not finish within 120 s. |
| `no title`, `no continue`, `warp error` | It never got into the point. |
| `no stats` | No FPS lines: the build lacks `TLG_FRAME_STATS_MS`. |

Warnings:
- `mission not seen`: the point's mission scene never started.
- `dark shot`: the screenshot is nearly black.

A status is a hint, not a verdict. Some scenes are dark or fade in late by design, and some load twice
on their own. Look at the screenshot, or open the scene with `--seconds 0`.

## How much a number means
We ran the same build twice over all points. Per point the two runs differ by about 3.5% at the median
and 8% at p90, while their averages over the whole run agree within 0.5%. So:
- a whole-run A/B shows a change of about 1%;
- a single point has to move by more than 8% to mean anything.

Look at the whole run, at the stage averages and at the per-second lines, not at a single number.

## Credits
The start points, and the way the game switches between them, come from the game's own QA tools. The
GoldHEN "Level Select + QA Menu" patch by @illusionyy and @DanielSvoboda made those reachable again.
Thank you both: this tool would not exist without that work.
