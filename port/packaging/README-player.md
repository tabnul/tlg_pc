# The Last Guardian — PC port

Code: [github.com/tabnul/the-last-guardian-definitive](https://github.com/tabnul/the-last-guardian-definitive) · Based on the port by [HayatoG](https://github.com/HayatoG/tlg_pc)

**No game files are included.** You need your own copy of The Last Guardian for PS4.

Experimental build: the game runs from boot to gameplay, but may still crash in untested areas.

## Required game files

None of it is in the download; you need your own copy. Use **The Last Guardian CUSA03627 (US) with update 01.03**, merged into one folder, with the base game and the update on top. About 25 GB.

| File | From | Size | Purpose |
|---|---|---|---|
| `eboot.bin` | update 01.03 | 89 MB | the game code (must be the 1.03 one) |
| `sce_module/libc.prx`, `sce_module/libSceFios2.prx` | update 01.03 | 1.5 MB | the game's own libraries |
| `sce_sys/` (`param.sfo`, `playgo-chunk.dat`, `icon0.png`, `pic0.png`, `pic1.png`) | base + update | ~5 MB | metadata, install info and the launcher art |
| `trico_boot.psarc`, `trico_001.psarc`, `trico_002.psarc`, `trico_003.psarc` | base game | 2.8 + 5.8 + 5.4 + 6.8 GB | main content |
| `trico_bootp.psarc`, `trico_001p.psarc`, `trico_002p.psarc`, `trico_003p.psarc` | update 01.03 | ~1.6 GB | update fixes |
| `trico_enu.psarc` / `trico_ptb.psarc` / `trico_esl.psarc` / `trico_frc.psarc` (+ the update's `…p.psarc`) | base + update | ~280 MB each | voices and text: English, Portuguese, Spanish, French |
| `sce_discmap.plt`, `sce_discmap_patch.plt` | base, update | small | file map (recommended) |

**From the console firmware (optional, for native audio):** `libSceNgs2.sprx` (372 KB). It goes in the `system_lib/` folder next to the executable.

**PC:** 64-bit Windows 10 (1903+)/11 or x86-64 Linux (glibc 2.39+), a graphics card with Vulkan 1.3 (6 GB of VRAM recommended).

## Dumping your own copy

The port runs your own copy of the game: a disc or a PSN purchase. No game files are shared or linked here.

You need:
- a PS4 that can run homebrew with GoldHEN,
- Itemzflow (or the dumper in GoldHEN),
- the game with update 01.03 installed,
- about 25 GB free on an exFAT USB drive or on the PC.

Steps:
1. Start GoldHEN, then Itemzflow.
2. Dump the game (CUSA03627 or CUSA03745): the base game and the update into one folder, the update on top. A "merged" dump does this for you.
3. Copy the folder to the PC, from the USB drive or over FTP (for example FileZilla).
4. Check the folder against the table under *Required game files*.
5. Start the launcher and pick that folder.

Pitfalls:
- **FTP can cut big files off at 4 GB.** Compare the file sizes with the console. When one is short, copy it again from a USB dump.
- **FAT32 cannot hold files over 4 GB.** Format the USB drive as exFAT.
- **Without update 01.03 the port does not work properly** (30 FPS, none of the port's fixes). The launcher shows a warning when the folder is older.

## How to play

1. Extract the package anywhere (on Windows, outside the .zip).
2. Put `libSceNgs2.sprx` in the `system_lib/` folder next to the executable (optional: without it, the port uses its own audio mixer, still experimental).
3. Open **The Last Guardian** (`The Last Guardian.exe` on Windows): the launcher, with all graphics options (resolution/widescreen, DLSS and FSR upscaling, HDR, effects).
   - On Windows, if "Windows protected your PC" appears, click *More info*, then *Run anyway*. The program is not digitally signed.
4. Under **Game folder**, choose the folder that has `eboot.bin`.
5. Pick one of the languages offered for your game folder and click **Play**. The first start takes a little longer.

**Launcher and menu language:** English by default. Under **Settings > Language > Launcher and menu language** you can pick German, French, Spanish, Brazilian Portuguese, Japanese or Simplified Chinese, or **Auto** for your system language. This changes the launcher and the port's in-game menu, not the game's own text and voices. Packs you make or fix yourself go in `user/lang/`; see [docs/language-packs.md](https://github.com/tabnul/the-last-guardian-definitive/blob/master/port/docs/language-packs.md).

**Experimental Japanese text (EU 01.03):** with CUSA03745 and update 01.03 applied to the same folder, select **日本語 (EU 1.03 text fallback)**. Do not move or delete the French archives: the port hides them only from the game's view for that launch. To return to French, select Français and launch again. Both launcher and runtime must contain this feature; it does not add Japanese-region disc executable support. Japanese menus and gameplay prompts were verified, not full-game audio/text coverage. Controller buttons are not swapped.

On Linux, `./install_desktop.sh` adds the game to the applications menu.

## While playing

- **F10**: toggles the FPS counter
- **Insert** (or R3 + L2 on a controller): opens the port's menu (image, performance, overlay)
- **F8**: switches between the chosen upscaler and the game's own anti-aliasing. DLSS (RTX cards), FSR 4 (recent cards, Windows and Linux) or FSR 3.1 (any card) keep foliage and thin edges stable. At the Native AA quality (the default, DLSS on RTX cards) the game renders at your screen size; at Quality, Balanced, Performance or Ultra Performance it renders smaller and DLSS or FSR upscale it, for more FPS. Change it in the launcher or under Upscaling and anti-aliasing in the port's menu (Quality needs a restart). FSR 4 is chosen once by itself on a card that runs it.
- PS4/PS5, Xbox and 8BitDo controllers work through SDL3, over USB or Bluetooth.

## Keyboard and mouse

While the game window has focus, it takes the mouse and the mouse turns the camera. **Esc** releases the mouse; a click in the window takes it back. Opening the port's menu (Insert) also releases it.

| In game | PS4 button | Keyboard and mouse |
|---|---|---|
| Move | Left stick | W A S D |
| Camera | Right stick | Mouse (or the arrow keys) |
| Jump | Cross | Space |
| Grab, hold on | Triangle | Left click or Q |
| Call Trico | R1 | Right click or 3 |
| | Square | E or middle click |
| | Circle | Left Shift |
| | L1 / L2 / R2 | 1 / R / F |
| | L3 / R3 | Z / C |
| | D-pad | I J K L |
| Menu | Options | Enter |
| Photo mode / pause | Touchpad (left / right) | Tab / Backspace |

- In the launcher, the **Controls** box turns the mouse on or off, sets its sensitivity and inverts the Y axis. **Keyboard and mouse buttons…** remaps each button: click *Set* and press the key or mouse button (the side buttons and the wheel work too). It applies at the next start.
- In game, the **Controls** tab of the port's menu (Insert) changes sensitivity, Y axis, smoothing, acceleration and anti-dead zone at once.

## Where things are

- Saves: `user/savedata/` next to the executable (keep this folder when updating)
- Logs: `out/logs/` (`out/last_run.log` is the latest session; attach it to bug reports)
- Launcher settings: `~/.config/tlg-launcher/` (Linux) or `%APPDATA%\tlg-launcher` (Windows). If you pick another data folder in **Saves & data**, its path is kept in `~/.config/tlg-pc/launcher.json` (`%APPDATA%\TLG-PC` on Windows)
- Launcher background: by default the picture from your own game files (`sce_sys/pic1.png`). To use another one, save it as `launcher_background.png` (or `.jpg`) in the `user/` folder; it is fitted to the window. Example: [the game's cover art](https://tpi-public.s3.eu-central-1.amazonaws.com/tlg_cover.png) (3840×2160).

## Problems

- **Incomplete or odd sound**: without `libSceNgs2.sprx` in `system_lib/`, the port uses its own mixer, still experimental (no reverb or filters yet). With the file, audio is native.
- **"Error 4551" or blocked by Smart App Control**: Windows 11 blocks programs without a digital signature, and the port is not signed. Turn it off in Windows Security > App & browser control > Smart App Control settings.
- **Does not start or closes by itself**: check the log box in the launcher and send `out/last_run.log` in an [issue](https://github.com/tabnul/the-last-guardian-definitive/issues).
- **NVIDIA**: use a recent driver.

---

This project is not affiliated with Sony Interactive Entertainment, SIE Japan Studio or genDESIGN. The Last Guardian is a trademark of Sony Interactive Entertainment.

The Windows package contains NVIDIA DLSS (`out/nvngx_dlss.dll`) under NVIDIA's license (`out/NVIDIA-DLSS-LICENSE.txt`). NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.

The packages contain the open AMD FSR 4 model (`fsr4_shaders/`, MIT, `LICENSE-FSR4-v07.txt`), built by @FireBurn's Q2RTX from AMD's FidelityFX SDK. AMD and FSR are trademarks of Advanced Micro Devices, Inc.
