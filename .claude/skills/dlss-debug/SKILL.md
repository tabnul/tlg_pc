---
name: dlss-debug
description: Find and fix image defects that appear only when upscaling (DLSS or FSR Quality/Balanced/Performance) in The Last Guardian port, and not at native size or with DLAA (see-through hair, dotted grass, holes, missing detail). Use when a reduced-resolution mode looks worse than DLAA at the same spot.
---

Upscaling defects usually come from the **reduced scene pass**, not the upscaler: draws that
match at native size stop matching at the coarser pixels. Compare with DLAA (same pipeline,
native size) to separate the two. For jitter artifacts at native size, use `dlaa-debug` instead.

## Setup (one run, everything live)
Launch per the `tlg-test-run` skill, with these added: `TLG_JITTER_CONTROL=<jc.txt>`,
`TLG_DUMP_TRIGGER=<dumpnow> TLG_DUMP_DIR=<dir> TLG_DUMP_FRAMES=2`. The dump dir must exist
(`mkdir -p`) before each `touch dumpnow`.
- Each dump holds the upscaler `input`, `motion` (rg16f, pixels), `depth` (f32) and `output`
  (3840 rgba16f) raw files. They work for DLSS and FSR 3.1.
- Switch modes live with ini plus reload (`upscaler`, `preset`, `dlaa`). The DLSS render preset is
  mask1 bits 60-63.

## Method
1. **Measure, don't eye-ball.** Crop the same box from the `output` dumps (DLAA vs the mode).
   Screenshots at window size hide the detail.
2. **Zero motion is the fingerprint.** Pixels where a G-buffer draw lost the depth test show
   what is behind them and carry its motion (often exactly 0). Count the exactly-zero motion
   in a box: DLAA about 0.1%, a broken mode 20%+.
3. **Bisect per draw** with `jc.txt`: `skip <ps>` (direct and indirect draws), then `map` to
   read the `Jitter draw:` lines (mrt, zwrite, zfunc, VS hash, jittered). Look for a depth-only
   prepass (mrt 0) followed by the color draw of the same mesh with another VS.
4. **Test a fix live**: `zslack <ps>` gives one draw the depth tolerance; `zslackval <c> <s>`
   changes it. If the zero motion drops back near the DLAA level, make it a general rule.

## Rules in place (`UpdateDepthStencilState`, mask2 bit 16 `SceneDepthSlack` turns them off)
Reduced pass, LessEqual, no game depth bias, and either no depth write or color targets get a
bias of -16 constant and -1 slope. Depth-only prepasses (mrt 0) keep their depth.
- Hair (2026-10-24): G-buffer without depth writes over its prepass, see-through at Q/P.
- Grass ps 2d9fcfbf after prepass ps 268b7235 (2026-10-24): dotted blades. Zero motion went
  from 0.24 to 0.04 in DLSS Q and in FSR Q.

The user judges the final look; dumps explain, the screen decides.
