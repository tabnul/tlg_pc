# Performance findings from the SotC port, to check in TLG

What we learned while chasing a steady 60 FPS in the Shadow of the Colossus port
(@Oussama268's SotC PC port, branch `perf/sotc`). Both ports share the same base: shadPS4
GPU code, the probe runtime and the launcher. Work through the items below **one at a time and
in this order**. Measure first; change code only after the user's OK.

The tools and patches are in `tools/sotc-perf/` (copied from SotC, still with SotC paths and
`SOTC_`/`BB_` names; adapt them before use).

## How we measured (use the same method)

- **Same route every run.** Record a pad route once (`TLG_PAD_RECORD=<file>`, F9 starts and
  stops), then replay it from the same save (`TLG_PAD_REPLAY`). `tools/sotc-perf/sotc_route.py`
  is our wrapper: it puts the route's save back, replays and restores the install's own save.
  In SotC the replay follows guest frames, not milliseconds, so it drifts less (see
  `spike_sampler.patch`, `bbgpu_guest_frames`). Pad replay must never be removed.
- **Several runs per variant.** One run of a moving scene varies by several FPS.
- **Metrics per run:**
  - average FPS
  - average FPS of the heavy stretch (SotC: frames with more than 9k commands)
  - number of frames over 20 ms and over 33 ms
  - worst frame
  
  `spk.py`, `fc.py` and `cmp.py` compute these from the frame log.
- **Never benchmark a build that carries diagnostics** (TLG already learned this). Every
  diagnostic must be off by default and cost nothing when off.

  The SotC `bench` skill wraps deploy, replay and these numbers in one command and keeps a
  `results.md` table. TLG now has its own port: `.claude/skills/bench` (`record`, `run`, `report`).
  The first run after a new build compiles shaders: leave it out of the comparison.

## 0. Before anything: fork check and sync the base

**The biggest lesson of the SotC day.** We spent about six hours (07:00–13:00, 2026-10-25) on
perf work on a base that was behind Oussama268's main. His 0.60.6-opt11 release (10-24) brought
more than all our work combined: on our pan30 benchmark, heavy-stretch FPS went 44.5 → 53.8
(+20%), avg 55 → 58, frames over 20 ms 421 → 281. A fork check at the start would have shown it.

So in TLG: run `/fork-scan` first (read its `last_scan.md` if it ran today). Bring in what
HayatoG/tlg_pc, the forks and the sister ports have, with the user's OK. Rebuild, take a new
baseline, and only then start on the items below. Do this again before each new perf item.

What opt11 changed. Check each one against TLG's code: same shadPS4 base, so they may port.
- **Hot readbacks on by default**, with a late re-check of the guest's writes right before
  applying (a86cc40, c029be2) and **each page written under its own region lock** (f9c26ba)
  instead of one big lock. Oussama measured +25% at the Shrine.
- **Fast bind on by default**: descriptor binds are reused (@Tadomikaari312-Hy's idea). This cuts
  draw-thread cost per draw, the very thing we had planned to profile next.
- **liverpool: skip `m_access` when a mapped queue has nothing queued** (0554703, a `has_work`
  atomic). Less lock traffic on the command-processor thread.
- **No disk read for shader-cache files that don't exist yet** (c828f3b): fewer stalls when
  shaders compile for the first time.

### TLG results for section 0 (2026-10-25, route `bench`, 60 FPS cap, fullscreen)

- 0d has_work (skip m_access for an empty queue): **dead end on TLG.** base 56.9 FPS / lowest 51.6 / 97 slow frames (mean of 3); has_work 56.1 / 50.2 / 122. The GPU thread is 0% idle here, so the skipped lock saves nothing. Kept in `git stash` ("sotc 0d has_work").
- 0e cache_storage MayExist: does not apply. TLG only calls Load for warm-up from existing keys; RegisterShaderBinary only saves.
- HDR on/off makes no performance difference (no HDR: 57.2 / 52.8 / 82, one run). Tests run with TLG_HDR=0.

## 1. PGO (Clang): the big one, about +14% in heavy scenes

SotC result on the same replay: heavy scenes 39 → 45 FPS, mid 51 → 55, frames over 33 ms 99 → 17.
The draw thread is the FPS limit there and gains most. The TLG bottleneck analysis (2026-10-23:
not GPU bound, the recording and command threads busy) points the same way.

TLG status: `build.sh` passes `-DTLG_PGO=off` on Windows, with the comment "GCC-only flags".
Only the GCC branch exists in `gpu/CMakeLists.txt`. SotC has a Clang branch:

```cmake
if (BB_PGO STREQUAL "generate" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE BB_PGO_CLANG)
elseif (BB_PGO STREQUAL "use" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fprofile-use=${BB_PGO_DIR}/bbgpu.profdata -Wno-profile-instr-unprofiled
                        -Wno-profile-instr-out-of-date -Wno-profile-instr-missing
                        -Wno-backend-plugin)
elseif (... the existing GCC branches ...)
```

`tools/sotc-perf/pgo_dump_bbgpu_cpp.txt` shows the shim part: with Clang the periodic profile
dump maps to `__llvm_profile_write_file`. In SotC the profile is written every 30 s, so a run
that gets killed still leaves a profile. The SotC equivalents are `tools/pgo_train.sh` and
`build.sh`, which pick up `pgo/bbgpu.profdata` when it's there.

Steps:
1. Port the Clang branch and the dump.
2. Build the instrumented exe in a separate build dir: `-DTLG_PGO=generate`.
3. Play or replay a heavy route with it. It runs slowly, so the replay drifts; the profile is
   still good.
4. Run `llvm-profdata merge -o pgo/tlggpu.profdata pgo/*.profraw`.
5. Rebuild with `use` and compare on the replay.

Commit the `.profdata` too. In SotC `pgo/` was ignored, so nobody's build actually used PGO.

Pitfall: `ninja -t clean` deletes the `.rc.res` files, and `llvm-windres` then fails with "Unable to
create temp file". Copy the `.res` files from the other build dir's `CMakeFiles` instead of
cleaning.

## 2. Periodic hitches: check our own diagnostics first

SotC had a 130–200 ms hitch every 60 s. The cause was our own memory report: it locked every
heap and walked the address space while every allocating thread waited. Worst frame went from
168 to 74 ms after making it opt-in. TLG had a similar case (the Lua `Each()` re-list every
~5 s). So when hitches are periodic, list every timer-driven report or scan first.

The tool that found it is the spike sampler (`spike_sampler.patch`, in `src/probe.c`).
`SOTC_SPIKE_OUT=<file>` samples the stack of every named thread every 2 ms for the whole run;
`spike_report.py` then shows what each thread did during the slow frames. `phase.py`,
`waits.py` and `prof.py` give busy share per thread over the frame, what a thread waits on, and
a per-thread function profile.

Also in SotC: large memory views are mapped in chunks (`view_chunks.patch`, `runtime_memory.c`),
which removed the spikes when a mapping got carved up. Check whether TLG's
`runtime_memory.c` has the same carve path.

## 3. Readbacks: what we measured, and what did NOT help

SotC heavy scene: ~850 readbacks/s, the guest waiting ~1.4 ms on each. We split that wait into
begin, GPU and finish: about 30 / 1200 / 30 ms/s. So almost all of it is the GPU round trip.
Then we checked which batch wrote the read-back data: in ~85% of cases it was the newest
submitted batch. These are true data dependencies; the guest really needs that GPU result.

Dead ends in SotC (measured; don't repeat without a new idea):

| Idea | Result |
|---|---|
| Copy the readback in a separate async submit ahead of the open command buffer | neutral |
| Drain flush: submit at once when the GPU runs dry (from @deadinside28's Bloodborne port) | neutral or mixed |
| Second Vulkan queue for readback copies (TLG measured this too) | no gain |
| Spin waiting on the GPU instead of sleeping | already in; more spinning gives nothing |
| Smaller batches (`SOTC_FLUSH_EVERY=64`) | +1.4 FPS on the replay, no clear gain in play |
| Faster guest mutex | no gain |
| `-march=x86-64-v3` | no gain |
| Relaxed readbacks | stale data, unusable |

What did help a little: applying the clean parts of a dirtied in-flight readback instead of
redoing all of it, and re-copying after a GPU write into the window
(`readback_partial_recopy.patch`). Whether TLG's readback path has the same partial/dirty
handling needs checking first. Its code has diverged (batching, EOP/EOS flushes, second queue).

**Where we went wrong:** we concluded "true dependencies, so the cost is fixed" and stopped looking
at the readback path. opt11 (section 0) showed the cost of *applying* them (locking, re-checks,
hot vs cold) was the lever: GPU ms per readback dropped 1.32–1.45 → 1.21, with more readbacks/s.
In TLG, look at how each readback is applied and locked, not only whether it's needed.

Next lever: **avoid the readback itself**. Find which guest code reads the
GPU value (TLG: `R 0xc4c630` reads a GPU-produced count) and serve it without a round trip,
through a patch or HLE. Treat this as research, not a quick switch.

## 4. Vblank rate: 240 Hz in the 30/60 FPS modes (now the default)

SotC went from 120 to 240 Hz vblanks: same average FPS, frames over 33 ms 17 -> 8. TLG's
"unlocked" mode already used 480 Hz (`TLG_VBLANK_HZ=0`); the 30 and 60 FPS modes ran at 60 Hz,
where a frame that just misses a vblank waits a whole 16.7 ms more (judder below 60 FPS).
Now the launcher and run.py default to 240 Hz there, and the present thread limits "Unlimited"
to the engine rate (`TLG_ENGINE_FPS`, 30 or 60) whenever the vblank is faster (driver.cpp), so
the game never speeds up. `TLG_VBLANK_HZ=60` gives the old behaviour.

Suite 2026-10-25, 60 FPS mode, 60 Hz against 240 Hz + limit 60 (avg FPS / p99 / frames over
1.5x median):

| route | 60 Hz | 240 Hz |
|---|---|---|
| bench | 58.1 / 25.6 ms / 56 | 57.7 / 24.3 ms / 14 |
| garden (2 runs) | 43.2 / 34.1 ms / 360 + 212 | 46.5 / 27 ms / 13 + 25 |
| pulse | 59.6 / 20.2 ms / 67 | 60.0 / 19.9 ms / 34 |

## 3b. Hot readbacks (SotC opt11, @Oussama268): kept, default on

Ported 2026-10-25 (`TLG_RB_HOT=0` turns it off). It halves the readbacks (GPU completions 4150 ->
1900 per 5 s, side-copy time 420 -> 165 ms); the Finish waits stay at ~930 ms per 5 s (readbacks
whose last GPU write is still in flight). Same exe, unlocked, on against off: pulse +6.5%,
+13%, +5.6% in three pairs; bench and garden within noise. Pulse stands for most of the game,
so it stays on. One crash during a garden load with it on (guest read of `ffffffff00000030` at
0x1488a22, "CopyResource ByteWidth" error just before), not seen again: watch for it.

## 3c. Query results one frame late: dead end (changes gameplay)

The game's JsComputeContext (anzu) runs ~2 compute queries per frame and the CPU waits for each
(~2.3 ms, mostly GPU queue time: ACB compute shares the graphics queue). A patch
(`TLG_QUERY_PREV_FRAME=1`, parked in a git stash, not on any branch) read the previous frame's
results instead of waiting. Its load crash was fixed: stale records indexed the new world's
smaller pool tables (null pool at 0x12719aa), so a key of those tables dropped results across a
load. FPS, unlocked, 2 runs each: pulse 60.9 -> 66.4, garden 44.6 -> 48.0, bench 64.5 -> 62.9.
Not usable: the queries drive gameplay. In pulse the boy failed to grab Trico and fell off twice,
so the replay also took another path (the gain is not comparable). Do not retry one-frame-late
query results; a gain here has to come from the emulator side (the label written when the GPU is
done instead of at parse time; a separate compute queue does not help, see 3d).

## 3d. Two readback ideas we measured and dropped (2026-10-26)

**Wait for the writing submit only (toggle bit 16, `ReadbackWaitSubmitted`).** A readback whose
data is still being written waits for the submission that wrote it instead of `Finish()` (submit
and wait for the GPU to idle). FPS was within noise: bench 62.2 / 68.7 on against 66.6 / 68.5 off,
garden 53.8 / 53.8 against 52.8 / 52.1. Upstream shadPS4 uses Finish for sync readbacks
(DeferOperation only for async ones), as do SotC and HayatoG; a narrower wait risks reading data
another queued write still changes. No measured gain, so it stays out.

**A second compute queue for the async compute (ACB) rings.** Ring dispatches share the graphics
queue, so the idea was to run them in parallel and shorten the readback waits. Prior art:
@rayhlancaner-png's shadPS4-Infamous-FixBuild (262af3e16e, b9756b9ff4) added one behind a setting,
off by default: no FPS gain in inFAMOUS, 2/3 of its dispatches waited for graphics work anyway;
HayatoG/iss_pc skipped it. We first measured whether it could help TLG (pulse, diagnostic run,
per 5 s): ~100k ring dispatches, 85% independent of graphics work, but the data behind the Finish
readbacks is written by graphics work. Of ~1000 readbacks and ~1.1 s of waits, under 15 readbacks
and 6 ms had a ring dispatch as a pending writer, even when any ring write in the same 64 KiB
granule counted. The query site (guest+0xc4c630, ~140 waits, ~190 ms) had none. A second queue
cannot shorten these waits, so it was not built. More time goes to the fault handlers
(~2.6 s per 5 s; write faults at guest+0xeeb6c4 ~1 s and guest+0xc459ed ~0.6 s), but that is
not an FPS lever either: discarding the eeb6c4 write faults (W 4.5 s -> 0.13 s per 5 s) left
FPS unchanged.

## 3e. WaitRegMem drains and the compute queue: dead end (2026-10-26)

- Fix A (compute WAIT_REG_MEM without a draw-pipe drain): condor 65.6 off vs 67.6/65.4 on. No gain, reverted.
- Option B (compute queue pipelined onto the draw recording thread: dispatches, WRITE_DATA and RELEASE_MEM in order, graphics waits on compute labels yield). Compute drains fell from 21.7k/s to ~450/s, and WaitRegMem drains to ~1%. FPS: condor 70.2 vs 72.3/71.3 off, garden 56.9/57.5 vs 56.1/54.1 (noise level, more spike frames), water no gain. One intermittent hang while loading. The time moves elsewhere: the GPU command thread is not the limit. Patch kept outside the repo (devkit `patches/compute-pipe-b/`).

## 3f. Lock-free backing writes: dead end (2026-10-26)

`runtime_memory_write_backing` from the region cache without the table lock, like the read fast path in 3b's round. Garden, same exe toggled: 57.5/58.0 on vs 60.2/59.0 off, then 58.0/58.6 on vs 55.0/57.7 off with the order reversed: run order, no effect. Condor one run 67.0 (in the 65-74 range of the day). Writes through the backing view are not hot; a write racing an unmap could also land in freed memory. GPU protect calls (21k/s, 49 ms/s on garden) are the `VirtualProtect` cost itself, not the lock. The big garden cost stays the write faults (~430 ms/s). Diff in devkit `patches/write-backing-c/`.

## 5. The bench suite (2026-10-25)

`bench.py run LABEL --route all --runs 1` (skill bench): bench (condor clash, 30 s), garden
(S13 walk, 20 s, always 2 runs), pulse (S13, 25 s). Unlocked FPS, Fullscreen, no HDR. The
replay is time-based, so long routes drift (water was dropped: it ended somewhere random).
Trico lives its own life: garden varies about +-4 FPS between runs of the same build, so judge
garden only on large changes (>5%) and use bench/pulse for small ones (3%, pulse 5%). The
route's save is copied back before every run (the game autosaves during a replay). The shader
cache persists across builds, so run 1 counts.

Results, unlocked avg FPS: baseline (no PGO) bench 63.8, garden 47.0, pulse 57.1; PGO trained
on the four routes: +2-10% in the heavy scenes, -1-2% (noise) elsewhere.

## Rules carried over

- Fork check and sync first, before every task (section 0).

- Credit authors by @login in commits and release notes (e.g. @deadinside28 for the drain
  flush idea, if it's ever used).
- No commit or push without the user's OK; one item at a time.
- Report each result with numbers from the replay (before/after, several runs).
