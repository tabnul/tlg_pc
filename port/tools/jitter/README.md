# DLAA jitter session kit

Tools for hunting jitter-only artifacts with DLAA, without rebuilding. Needs a dev build of this branch
(`cd port && TLG_LTO=OFF bash build.sh`). The method and the known rules are in the DLAA debugging docs.

## Start
Run `port\tools\jitter\start.cmd` (optionally with a render size, e.g. `start.cmd 1920x1080`). It starts the
game with jitter debugging on, plus a minimized "jitter watch" window that saves a screenshot for every F6
to `port\out\jitter\shots\markN.png`. Use the QA menu (L1+L3) to jump between stages.

## Keys
| Key | What |
|-----|------|
| F7  | Jitter on/off (A/B, not saved). |
| F6  | Mark: logs the draw map and suspect draws of this frame, writes a frame capture, takes a screenshot. |
| F8  | DLAA on/off (saved), to compare with the game's own SMAA. |

## Per area
1. Look at the boy, Trico, water, light effects, foliage and distant geometry while moving the camera.
2. See blocks, tearing, flicker or swimming edges? Press F7.
   - Gone with jitter off: a jitter bug. Turn jitter back on, frame it and press F6.
   - Still there with jitter off: not a jitter bug (DLSS or the game).
3. Shimmer on grass or hair only while moving is the DLSS limit; skip it.

"Jitter suspect" log lines are noisy and mean nothing without a visible artifact.

## Live bisection (`port\out\jitter\control.txt`)
- `follow`: unshifted draws that depth-test a jittered depth get jittered too.
- `nomesh` / `nodecal`: turn the existing rules off.
- `not <ps>` / `only <ps>` / `<lo> <hi>` draw ranges: narrow down to one shader.
- `skip <ps>`: drop that shader's draws, to see what it draws.
- `map`: log the draw map on the next frame.

Empty the file to go back to normal.

## Live Lua (`port\out\jitter\lua_exec.lua`)
Runs every time the file is saved. The starting copy logs the stage, the fade and the loading state, which
helps with black screens after a stage switch.

## Lab tab (Insert, last tab)
Graphics tuning tests, all live:
- Texture LOD bias (under DLAA), anisotropic filtering, full-resolution fog/particles/AO and the wind
  animation frame skip. These are saved in `tlg.ini`; "Reset all" puts them back.
- Rows from `port\out\jitter\lab.txt` (a copy of `lab.txt` here; edit it while the game runs). Each
  row scales, offsets or sets one property of the game's own objects (fog, LOD, far plane, shadow
  splits). The right column is the game's value. These are not saved.

Full-resolution effects, per shader (in `control.txt`): `halfonly <ps>` / `halfnot <ps>` limit which
passes are drawn at full resolution, to find the one that breaks. They work while DLAA is on.
