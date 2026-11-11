# Upscaler, Quality and anti-aliasing

Three settings control the image. They live in the launcher (Video page) and the in-game menu (Insert), and are stored in `tlg.ini` as `upscaler`, `preset`
and `dlaa`.

| Setting | Choices | Changes live? |
|---|---|---|
| Upscaler | Built-in (game), DLSS (NVIDIA only), FSR 3.1 (any card), FSR 4 (any recent card), TAA (any card, native size only) | yes |
| Quality | Native AA, Quality, Balanced, Performance, Ultra Performance | **no, requires restart** (Apply and restart in the in-game menu) |
| Sharpening | on / off + strength 0-400% (default 175%; up to 100% the upscaler's own sharpening, every 100% above adds one RCAS pass) | yes |

## How the pieces fit

- **Quality** sets the render size: the game renders at output ÷ factor and the engine's
  own resolution table is patched at start, which is why it needs a restart. Native AA
  renders at the output size.
- **Upscaler** picks what turns the render into the output picture. With a smaller render
  size DLSS, FSR 3.1 or FSR 4 replace the game's own upscale pass. Built-in keeps the
  game's bilinear upscale. An unsupported choice (no NVIDIA card, no FSR 4 assets) falls
  back to FSR 3.1 and the menu says so.
- **TAA** is our own light temporal anti-aliasing on the game's motion vectors. It only
  works at native size and does not upscale. It costs about as much as FSR 3.1 native AA,
  less than DLAA or FSR 4; which of the two looks better is a matter of taste.
- **Anti-aliasing** has no switch of its own: the upscaler choice is the anti-aliasing. DLSS
  is DLAA at Native AA quality, FSR is FSR native AA, TAA is TAA; while scaling up the
  upscaler smooths the edges too. Built-in keeps only the game's own. F8 in game switches
  between Built-in and your choice. A settings file from before with anti-aliasing off
  loads as Built-in.
- **Sharpening** is one setting for every upscaler. With DLSS, FSR 3.1, FSR 4 or TAA it is
  their own sharpening inside the anti-aliasing step, so it only touches the scene and the
  menus stay crisp. With Built-in it is AMD CAS on the final picture (CAS stops at 100%).
  Above 100% DLSS / FSR / TAA add an extra pass. The older separate RCAS / CAS choice is gone.

Frame generation (DLSS or FSR 3.1) works with every upscaler and Quality setting: it takes the render-size depth and motion and the full-size picture with menus.

## Render size per output and preset

Each side is output ÷ factor, rounded to the nearest even number. The launcher and the
in-game menu show the exact size for your output in the Quality hint.

| Output | Native AA (1×) | Quality (1.5×) | Balanced (1.7×) | Performance (2×) | Ultra Performance (3×) |
|---|---|---|---|---|---|
| 1920x1080 | 1920x1080 | 1280x720 | 1130x636 | 960x540 | 640x360 |
| 2560x1440 | 2560x1440 | 1706x960 | 1506x848 | 1280x720 | 854x480 |
| 3840x1600 | 3840x1600 | 2560x1066 | 2258x942 | 1920x800 | 1280x534 |
| 3840x2160 | 3840x2160 | 2560x1440 | 2258x1270 | 1920x1080 | 1280x720 |

## Supersampling (experimental)

The Quality row ends with two supersampling choices: 150 % and 200 % (`preset=5` / `preset=6`
in `tlg.ini`, or `TLG_UPSCALE_PRESET=5|6` for one run). Quality is one choice, so supersampling
and upscaling cannot be combined: with supersampling the chosen upscaler (DLSS, FSR) only does
its anti-aliasing at the larger size and never enlarges the picture. They render larger than the
output (150 %: 2880x1620 at 1080p, 5760x2400 at
3840x1600; 200 %: double) and shrink the picture with the game's own pass. They need the GPU
heap above 4 GiB (the launcher patches it to 5120 MB
only for these presets), so count on 12 GB VRAM or more. Tested on Windows at 3840x1600
with an RTX 5090 only; not tested on 16 GB cards or Linux. GPU cost is 2.3× (150 %) to
4× (200 %) of native.

## What upscaling does and does not fix

Rendering smaller saves GPU time, so it helps when the GPU is the limit (for example an RX
6600 at 1080p). It barely changes memory use: the render targets shrink by roughly
100-200 MB at 1080p, while the texture cache is unchanged. Stutter or slowdowns caused by
texture memory pressure on 8 GB cards are not helped by a lower Quality preset.

## Environment variables (testing)

- `TLG_RENDER_RESOLUTION=WxH` forces the render size regardless of the preset.
- `TLG_UPSCALE_PRESET=0..6` picks the preset for one run.
- `TLG_UPSCALER=off|dlss|fsr3|fsr4`, `TLG_DLAA=0|1` as before.

Credits: the DLSS and FSR integration builds on the upstream TLG port's temporal upscaler.
