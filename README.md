<p align="center"><img src="port/packaging/icon/tlg-icon-1024.png" width="160" alt=""></p>

# The Last Guardian for PC

A native PC port of The Last Guardian (PS4, 1.03) for Windows and Linux. This fork builds on
[HayatoG/tlg_pc](https://github.com/HayatoG/tlg_pc) and concentrates on one thing: making the game
look as good as it can on a PC, without changing what it is.

The port itself, the loader, the runtime and most of what makes the game playable at all, is
HayatoG's work; see their [README](https://github.com/HayatoG/tlg_pc#readme) (Portuguese and
English). Bug fixes made here go back upstream as pull requests.

> No game files are included. You need your own copy, dumped from your own PS4.

<!-- Before/after screenshots go here. -->

## What this fork adds

**DLAA.** NVIDIA's DLSS anti-aliasing at native resolution, fed with the game's own motion vectors.
It replaces the game's SMAA and removes most of the shimmer on foliage, feathers and fences. On by
default on RTX cards; F8 switches between DLAA and the original SMAA. Other cards keep SMAA.

**Any resolution and aspect ratio.** The game renders at the window's resolution, including 21:9
and 32:9. The menus, hints and subtitles are laid out for the wider screen instead of being
stretched.

**AI-upscaled textures.** [ps4-texturepack-generator](https://github.com/tabnul/ps4-texturepack-generator),
a separate tool, turns the textures of your own copy into a 4x upscaled pack (colour textures and
normal maps). The game loads them while it runs and frees them again when they
are no longer drawn. Runs on NVIDIA, AMD/Intel or, slowly, on the CPU. Entirely optional. See
[docs/texture-pack.md](docs/texture-pack.md).

**Graphics settings.** In the in-game menu (Insert) and in the launcher:

- presets: *Stock* (the game as shipped), *Recommended* (the default) and your own
- shadow resolution 2048, 4096 or 8192; the higher ones mostly fix shimmering shadows under trees
- field of view, 50 to 110 degrees
- the game's own effects as sliders: bloom, depth of field, motion blur, ambient occlusion, screen
  space reflections, sharpening, vignette, chromatic aberration, character detail distance
- HDR10 output, using the game's own HDR path

Every setting shows which value the game originally used, and the help text says what we would pick
and why. Settings that turned out to make no visible difference were tested and left out; the
evidence is in [docs/graphics-investigation.md](docs/graphics-investigation.md).

## Download

Builds of this fork are under [Releases](https://github.com/tabnul/tlg_pc/releases).

## Game files

Use **CUSA03627 (US) or CUSA03745 (EU) with update 01.03**: dump the base game and the update with
Itemzflow, then copy the update over the base game folder. About 25 GB. Copy large files with a USB
drive; GoldHEN's FTP truncates files over 4 GB.

For audio, also copy `libSceNgs2.sprx` from `/system/common/lib/` on your PS4 into `system_lib/`
next to the executable.

The full file list is in [docs/ARQUIVOS_NECESSARIOS.md](docs/ARQUIVOS_NECESSARIOS.md).

**PC:** a Vulkan 1.3 graphics card with 6 GB of video memory or more. DLAA needs an NVIDIA RTX card.

## Playing

1. Extract the release and put `libSceNgs2.sprx` in `system_lib/`.
2. Start the launcher, pick the game folder and language, and press **Play**.
3. In game: **Insert** (or R3 + L2) opens the port's menu, **F10** shows the frame rate, **F8**
   switches DLAA/SMAA, **Esc** releases the mouse.

Controllers (DualSense, DualShock 4, Xbox) work through SDL3, motion sensor included. Mouse and
keyboard work too and can be remapped. More in
[port/packaging/README-player.md](port/packaging/README-player.md).

## Building

```bash
git clone --recursive https://github.com/tabnul/tlg_pc.git && cd tlg_pc
cd port && TLG_LTO=OFF bash build.sh
```

On Windows this runs in an MSYS2 CLANG64 shell; see
[port/packaging/windows/README.md](port/packaging/windows/README.md). DLAA needs the DLSS SDK as
well (`build_dlss.sh`).

## Credits

- [HayatoG/tlg_pc](https://github.com/HayatoG/tlg_pc) by HayatoG: the port this fork is based on.
- [gow3_pc](https://github.com/FelipePrado0/gow3_pc), [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)
  and the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core, which the port builds on.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) for audio.
- [4x-UltraSharp](https://huggingface.co/Kim2091/UltraSharp) by Kim2091, the default texture
  upscale model (CC BY-NC-SA 4.0), and Microsoft's
  [DirectXTex](https://github.com/microsoft/DirectXTex) for texture compression.
- NVIDIA DLSS, under the NVIDIA DLSS license.

GPL-2.0, see [port/LICENSE](port/LICENSE).

Not affiliated with Sony Interactive Entertainment, SIE Japan Studio or genDESIGN. The Last
Guardian is a trademark of Sony Interactive Entertainment. No game code, files or art are
distributed here.
