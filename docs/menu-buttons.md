# Menu confirm button

Choose **Menu confirm button** on the Qt launcher's Controls screen (or the Tk launcher's
Controls box). Cross (X / A) is the default; Circle (O / B) enables Japanese-style menus.
The other button goes back. Changes take effect on the next launch, without swapping
normal gameplay actions.

The setting is `menu_confirm=cross|circle` in the user's `tlg.ini`, mirrored in launcher
settings. `TLG_MENU_CONFIRM=circle` overrides it. Missing/invalid values keep Cross.
The launcher passes the same normalized preference to the runtime and to the game-patch
preparation step. Direct patch-tool users can select `--menu-confirm circle`.

## Two separate layers

PC dialogs and overlays honor the preference in system parameter 1000, text entry,
save/message dialog input and controller glyphs, and Dear ImGui navigation. Keyboard
Enter/Escape and mouse handling in these PC dialogs are unchanged. The ordinary SDL/PS4
pad mapping is never changed.

The game's own menus do not query system parameter 1000: the only call to that import
requests system language (parameter 1). Its gamepad normalizer constructs separate
menu-confirm (`0x20000`) and menu-cancel (`0x40000`) flags, in addition to ordinary
Cross/Circle bits. On the verified EU executable:

- At image offset `0xd2ebee`, raw Circle (`0x2000`) is shifted left by 5, making cancel.
- At `0xd2ebf2`, raw Cross (`0x4000`) is shifted left by 3, making confirm.
- Circle preference changes both shifts to 4. The ordinary Cross (`0x1`) and Circle
  (`0x2`) normalization instructions remain untouched.

The change is applied only to the loaded image, alongside (but independently of) FPS
patches. Neither the original game executable nor extracted ELF is rewritten.

## Supported game build and safety

The game-menu patch currently supports **CUSA03745 EU, installed update 01.03 only**.
The entire decoded ELF must have SHA-256
`ce2a5da1de13d7e43a2403cefe649f0f35a968e8b3779b911a314292472ef573`, and all 14 bytes of the
conversion sequence must match before the 8-byte write is emitted. A plain ELF and the
SELF containing that same ELF are supported. Other builds receive no game-menu write
and a diagnostic; PC dialogs still honor the chosen preference. US/KR/CN and other
updates are not claimed to be supported. External mods overlapping the conversion
cause external mod writes to be skipped with a diagnostic, not silently overriding it.

## Validation

C service tests cover the PS4 preference and dialog action roles. Python tests cover
launcher/environment/ini propagation, the default no-op, exact writes, instruction and
hash rejection, ordinary gameplay bits, 30 FPS independence and mod overlap handling.
The generated write was also checked against the real EU SELF without modifying it.

The earlier equivalent in-memory proof of concept passed scripted live-process testing
(Cross did not confirm; Circle opened the selected menu; Cross returned) and the user
confirmed it worked in-game with their controller. A complete rebuilt source runtime
still needs UI/controller regression testing; exhaustive in-game glyph coverage is not
claimed.
