# Japanese text on the EU build

## What this supports

The verified setup is **EU CUSA03745 with update 01.03 installed/merged into the game
folder**, displaying Japanese assets already included in the shared boot archives.
This is **not support for the Japanese-region disc executable**. US, Korean, Chinese,
other updates and modified asset sets have not been validated for this fallback.
Japanese menus and gameplay prompts have been seen in-game; full-game text, narration
and audio coverage have **not** been exhaustively verified.

The launcher option and automatic handling below require **both a launcher and native
runtime built with this change**. Do not copy only the new launcher over an old runtime:
the old runtime will not know how to hide the French packs.

## Setup — no archive editing or moving required

1. Use your own EU game dump with its **01.03 update applied to the same game folder**.
   An update PKG merely stored beside the base game is not an installed update.
2. In the launcher, select the folder containing `eboot.bin` and `sce_sys/param.sfo`.
   Keep the normal game files, including `trico_boot.psarc`, `trico_bootp.psarc`,
   `trico_frf.psarc` and `trico_frfp.psarc`. Do not rename, delete or edit them.
3. In **Game language**, choose **日本語 (EU 1.03 text fallback)**, then **Play**.
   Both the Qt and classic Tk launchers expose this choice for the verified asset layout.

The port automatically uses the appropriate internal language id and hides the two
French localization packs **only from the game's view for that launch**. Their bytes
and names on disk remain unchanged. No extra language download, save conversion, package
modification, region spoof or custom launcher is required.

To return to French, select **Français** and launch again. Other languages also disable
the temporary mask automatically. Japanese text is opt-in: **Automatic** does not quietly
select this experimental fallback just because the operating system is Japanese.

This does not swap controller buttons. Japanese-style O-confirm/X-back is a separate
menu preference/change ([PR #140](https://github.com/HayatoG/tlg_pc/pull/140)); selecting
Japanese text alone does not enable it.

## Troubleshooting

- **The option is missing:** verify that the launcher/runtime contain this change, the
  game folder is EU `CUSA03745`, `APP_VER` is `01.03`, and both boot archives are present.
  The launcher does not advertise this workaround for unverified regions or versions.
- **A previously saved Japanese choice is shown for a different game folder:** that
  choice is preserved, but the fallback is disabled and the game uses English. The Qt
  launcher displays an explanation; this does not enable Japanese-region disc support.
- **It still shows French:** ensure the native runtime was rebuilt too. Remove a manual
  `TLG_LANGUAGE` override that conflicts with the launcher selection. The normal launch
  log should report `Japanese text fallback: French root archives hidden from guest`.
- **It shows English with a forced language id:** setting `TLG_LANGUAGE=0` is not this
  workaround; that fell back to English in the tested EU build. Let the launcher select
  the internal id instead.
- **You already moved the French packs for the old workaround:** the option also works
  without them, but French cannot be restored by changing the language until you put
  your original packs back. Never replace game archives from an unknown source.

Saves stay under the same EU title id and user folder. This feature does not rewrite
saves, game archives, metadata or the game executable. Backups before each launch still
follow the port's normal save-backup policy.

## Why the code is needed

Japanese paths remain in the shared boot archives, but the EU launcher's normal language
table does not expose them. The working fallback requests the internal French language
id (`2`) while making the France-French packs unavailable. With those packs absent, the
game falls back to its embedded Japanese assets. Simply requesting native Japanese id
`0` did not produce the same result.

Previously a player had to select the misleading **French** option and manually move
`trico_frf.psarc` and `trico_frfp.psarc`, then restore them to play in French. The new
launcher explicitly names the Japanese fallback and sets `TLG_JAPANESE_FALLBACK=1`
together with the internal id. `runtime_file.c` returns `ENOENT` for those two root
archives in `/app0`, `/hostapp` and equivalent relative paths, and omits them from guest
directory listings. It does not hide similarly named files in saves or nested folders,
other language packs, or any host files from other processes.

Switching away clears the mask, even if the launcher's parent environment contained a
stale value. An explicit incompatible `TLG_LANGUAGE` override wins and disables the mask;
remove that debugging override to use the launcher option. Direct native-runtime users
must supply both `TLG_LANGUAGE=2` and `TLG_JAPANESE_FALLBACK=1` on the verified build;
the native flag alone does not identify or validate a disc region.

## Validation and limits

The earlier manual archive-omission setup was tested in-game on EU 01.03 and produced
Japanese menus/gameplay prompts. Its actual kernel opens were `/app0/trico_frf.psarc`
and `/app0/trico_frfp.psarc`, the paths now masked by this change.

Automated C tests exercise real guest open/stat/list behavior with French packs still
present on disk, check relative and hostapp aliases, ensure saves/nested files and other
packs remain readable, and verify French is readable again when the option is off.
Python tests cover version/region/layout gating, UI choices, language-id/mask pairing,
normal French, stale environment cleanup, explicit overrides and non-mutation.

A full newly rebuilt launcher/runtime still needs end-to-end in-game regression testing.
The previous manual proof is not presented as testing of a rebuilt version of this PR.
