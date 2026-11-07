# DLSS, FSR 4 and FSR 3.1: anti-aliasing and upscaling

All three run in two ways. At **Native AA** the game renders at your screen size and they only
smooth the edges (below). At **Quality, Balanced, Performance or Ultra Performance** the game
renders below your screen size and DLSS or FSR builds the full picture, for more FPS; menus and
subtitles are drawn at full size. Render sizes, costs and how it works:
[port/docs/upscaling.md](../port/docs/upscaling.md).

DLAA is NVIDIA's DLSS anti-aliasing at native resolution. The port feeds it the game's own motion
vectors and uses it instead of the game's SMAA: foliage, feathers and fences stop shimmering.

FSR 3.1 Native AA is the same with AMD FSR 3.1 (through
[UniversalScalers-Vulkan](https://github.com/FireBurn/UniversalScalers-Vulkan) by @FireBurn): it runs on any card and needs
no extra files. On an RTX card both are available; pick the one you like.

FSR 4 Native AA is the same with AMD FSR 4: sharper and steadier than FSR 3.1, above all in rain
and on fine moving detail. It runs on Windows and Linux on cards with the shader features it needs
(recent AMD cards, and many NVIDIA and Intel cards); on other cards it is greyed out. The open FSR 4
model (MIT, `LICENSE-FSR4-v07.txt`) comes with the packages in `fsr4_shaders\`.

The first time the game starts on a card that runs FSR 4, it moves FSR 3.1 Native AA (or DLAA
without DLSS) to FSR 4 Native AA, once. After that your choice stays; `fsr4_default=1` in
`tlg.ini` records that it happened.

## Playing

**Needs:** Windows and an NVIDIA RTX card (RTX 20xx or newer) with a recent driver.

**The release zip has everything.** The two DLLs DLAA needs come with the Windows package, in `out\`
next to the game's exe:

| File | |
|---|---|
| `out\tlg_dlss.dll` | the port's bridge to DLSS |
| `out\nvngx_dlss.dll` | NVIDIA's DLSS runtime, shipped under NVIDIA's license |
| `out\NVIDIA-DLSS-LICENSE.txt` | that license |

DLAA is **on by default**. Nothing needs to be downloaded or copied.

- **Launcher:** *Upscaler* (Built-in, DLSS, FSR 3.1, FSR 4, TAA) and *Quality* (Native AA,
  Quality, Balanced, Performance, Ultra Performance). The upscaler is the anti-aliasing; Built-in
  keeps the game's own.
- **In game:** the overlay menu (Insert) > *Upscaling and anti-aliasing*, the same choices.
  Quality needs a restart (*Apply and restart*); the rest changes live.
- **F8** switches between that choice and the game's SMAA and saves it; a label in the top right
  shows which one is active.

On other cards, on Linux, or when a DLL is missing, DLAA is greyed out with the reason; FSR 3.1
Native AA still works. FSR 4 falls back to FSR 3.1 when its model files are missing.

`tlg.ini` keys: `upscaler=dlss`, `upscaler=fsr4`, `upscaler=fsr3`, `upscaler=taa` or
`upscaler=off` (Built-in) picks it, `preset=0` (Native AA) to `4` (Ultra Performance) the Quality; the game writes `fsr4_supported` for the launcher. `fsr_velocity` (0..1, default 1, AMD's value) sets how much motion cuts FSR's history: lower
keeps moving hair and fur smoother, at the risk of ghosting.

## Building from source

`build.sh` builds the port without DLAA. The DLSS part is a separate DLL built with MSVC, because
the DLSS SDK is a Visual Studio library:

1. Install Visual Studio 2022 or newer (the Build Tools are enough) with *Desktop development
   with C++*.
2. Get NVIDIA's DLSS SDK at the version the CI uses:
   ```bash
   git clone --depth 1 --branch v310.9.16 https://github.com/NVIDIA/DLSS.git F:/sdk/DLSS
   ```
3. In the MSYS2 CLANG64 shell, from `port/`:
   ```bash
   DLSS_SDK_ROOT=F:/sdk/DLSS bash packaging/windows/build_dlss.sh
   ```
   It puts `tlg_dlss.dll`, `nvngx_dlss.dll` and `NVIDIA-DLSS-LICENSE.txt` in `out/`, next to
   `tlg-probe.exe`. `packaging/windows/package.sh` ships them when they are there;
   `TLG_REQUIRE_DLSS=1` makes their absence an error (the CI sets it).

`DLSS_VS_GENERATOR` overrides the Visual Studio version that `vswhere` finds; `CMAKE_EXE` points at
a CMake other than the one on the `PATH`.
