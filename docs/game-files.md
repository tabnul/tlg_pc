# Required game files

None of it is in the download; you need your own copy. Use **The Last Guardian CUSA03627 (US) or CUSA03745 (EU), with update 01.03**, merged into one folder, with the base game and the update on top. About 25 GB.

| File | From | Size | Purpose |
|---|---|---|---|
| `eboot.bin` | update 01.03 | 89 MB | the game code (must be the 1.03 one) |
| `sce_module/libc.prx`, `sce_module/libSceFios2.prx` | update 01.03 | 1.5 MB | the game's own libraries |
| `sce_sys/` (`param.sfo`, `playgo-chunk.dat`, `icon0.png`, `pic0.png`, `pic1.png`) | base + update | ~5 MB | metadata, install info and the launcher art |
| `trico_boot.psarc`, `trico_001.psarc`, `trico_002.psarc`, `trico_003.psarc` | base game | 2.8 + 5.8 + 5.4 + 6.8 GB | main content |
| `trico_bootp.psarc`, `trico_001p.psarc`, `trico_002p.psarc`, `trico_003p.psarc` | update 01.03 | ~1.6 GB | update fixes |
| `trico_enu.psarc` / `trico_ptb.psarc` / `trico_esl.psarc` / `trico_frc.psarc` (+ the update's `…p.psarc`) | base + update | ~280 MB each | voices and text: English, Portuguese, Spanish, French |
| `sce_discmap.plt`, `sce_discmap_patch.plt` | base, update | small | file map (recommended) |

**CUSA03745 (EU)** ships 15 languages instead of CUSA03627's four: English, French, Spanish (Spain), German, Italian, Dutch, Portuguese (Portugal), Russian, Finnish, Swedish, Danish, Norwegian, Polish, Turkish and Arabic. The launcher lists the languages of the loaded CUSA.

**From the console firmware (optional, for audio):** `libSceNgs2.sprx` (372 KB). It goes in the `system_lib/` folder next to the executable.
