# Language packs

The launcher and the in-game menu (Insert, or R3 + L2) are written in English. A language pack
translates both. A pack is one JSON file; nothing in it can break the launcher or the game,
because every text that is missing, empty or broken simply stays English.

The game's own text and voices are a separate setting (the game language). A language pack does not
change them.

## For players

Pick a pack under **Settings > Language > Launcher and menu language**:

- **English** is the default.
- **Auto** uses your system language when a pack has it (`de_AT` finds `de`), and English otherwise.
- Any installed pack by name.

The launcher switches at once. The in-game menu reads the pack when the game starts, so it changes
at the next **Play**.

Shipped packs:

| Code | Language | Source |
|---|---|---|
| `de` | Deutsch | Machine translation, needs a native check |
| `es` | Español | Machine translation, needs a native check |
| `fr` | Français | Machine translation, needs a native check |
| `it` | Italiano | Machine translation, needs a native check |
| `ja` | 日本語 | Machine translation, needs a native check |
| `ko` | 한국어 | Machine translation, needs a native check |
| `nl` | Nederlands | Machine translation, needs a native check |
| `pl` | Polski | Machine translation, needs a native check |
| `pt_BR` | Português (Brasil) | HayatoG's original translation from [HayatoG/tlg_pc](https://github.com/HayatoG/tlg_pc) |
| `pt_PT` | Português (Portugal) | Machine translation, needs a native check |
| `tr` | Türkçe | Machine translation, needs a native check |
| `uk` | Українська | Machine translation, needs a native check |
| `zh_CN` | 简体中文 | Machine translation, needs a native check |
| `zh_TW` | 繁體中文 | Machine translation, needs a native check |

**Your own packs** go in the `lang/` folder inside the port's `user/` folder (create it). A pack
there with the same code as a shipped one replaces it, so you can fix a translation without
waiting for a release.

Latin and Cyrillic packs (Polish, Turkish, Ukrainian, ...) need nothing extra: the menu font has
those letters.

**Japanese, Chinese and Korean in the in-game menu** need a font with those characters. The menu
font has none, so the game merges in a system font:

- Windows: Yu Gothic, Meiryo or MS Gothic for Japanese; Microsoft YaHei or SimSun for Simplified
  Chinese; Microsoft JhengHei or MingLiU for Traditional Chinese; Malgun Gothic for Korean. All of
  these come with Windows.
- Linux: Noto Sans CJK (`noto-fonts-cjk` on Arch/CachyOS, `fonts-noto-cjk` on Debian/Ubuntu,
  `google-noto-sans-cjk-fonts` on Fedora), or WenQuanYi Micro Hei.

If none is found, the game log says `language pack font: none found`, and the characters show
as `?`. Set `TLG_LANG_FONT=<path to a .ttf/.otf/.ttc>` to use any other font.

## For translators

You need Python 3 and the port's `launcher/` folder (from a release or from git). Run the commands
from `port/launcher`.

1. **Start a pack:** `python langpack.py template nl` writes `nl.json`, with every English text in
   place. Use the language code as the file name: `nl`, `pt_BR`, `zh_TW`, ...
2. **Fill in** `name` (the language in its own words, shown in the launcher), `english_name`, and
   `authors` (your name, plus a link if you like). Then translate the values.
3. **Check it:** `python langpack.py check nl.json` lists every problem and counts the translated
   texts. Put the file in `user/lang/` and pick it in the launcher to see it live.
4. **After a new release:** `python langpack.py update nl.json` adds the new texts, still in
   English, so you can see what is left to do. `check` also reports texts that the port no longer
   uses.

### The file

```json
{
  "format": 1,
  "code": "nl",
  "name": "Nederlands",
  "english_name": "Dutch",
  "authors": ["Your Name (github.com/you)"],
  "text":  { "launcher.home_settings": "Instellingen", "run.play": "Spelen" },
  "items": { "menu:sharpen": { "name": "Verscherpen", "what": "...", "choices": { "off": "Uit" } } },
  "menu":  { "Save game": "Spel opslaan", "%d minutes ago": "%d minuten geleden" }
}
```

- **`text`**: the launcher's own texts. The key is `<section>.<key>`; keep the keys.
- **`items`**: the switches on the Graphics, Advanced and Mods pages, keyed by their id. `name` is
  the label, `what` the help text, `why` the extra note some switches have, and `choices` the labels
  of a drop-down (keep the keys: they are the values the game reads).
- **`menu`**: the in-game menu. The **key is the English text itself**; translate only the value.

Leave a text out, or leave it equal to English, and the English one is used.

### Rules

- **Placeholders stay exactly as they are.** Launcher texts use `{name}`. You may move them, but keep
  every one. Menu texts use printf codes: `%d`, `%s`, `%.1f`, `%02d`, `%08X`. Keep all of them **in
  the same order**, because C cannot reorder them. A wrong placeholder could crash the game, so a
  text with one is never used: `check` reports it, and the game drops it and says so in its log. A
  plain `50%` is not a placeholder; write `%%` only where the English has it.
- Keep `\n` line breaks, and anything after `##` in a menu text (`Camera##pilotcam`). That part is
  hidden; it tells the menu which control is which.
- Keep technical names as they are: DLSS, FSR, TAA, HDR, VSync, FPS, key and button names.
- Keep labels short. Buttons and menu rows have little room.

### Sharing a pack

Open a pull request that adds `port/launcher/lang/<code>.json`, or attach the file to an issue.
The test suite runs `check` on every shipped pack; it must report 0 problems. Corrections to the
machine-translated packs are very welcome. When you have checked a pack, add your name to its
`authors` and remove the "machine translation" line.

## For developers

- **Launcher** (`launcher/langpack.py`): the English texts live in the `TEXT` dicts of
  `tlg_launcher`, `pad_page`, `preset_share` and `tlg_run`, and in the `Item`s of
  `backports_table`. `langpack.apply(code)` first restores English, then lays the pack over it in
  place; the launcher calls it at start and when the setting changes, then rebuilds the window. A
  new text needs no extra step: add it to a `TEXT` dict and it shows up in `template` and `update`.
- **In-game menu** (`gpu/shim/tlg_lang.h`): write every menu text as `TR("English text")`, or
  `TR(TlgFun::Name)` for a constant from `tlg_fun.h`. Afterwards run
  `python launcher/langpack.py menu-scan` (from `port/`) to refresh `launcher/lang/menu-texts.json`;
  a test fails while the list is out of date. `TR()` needs a string that lives as long as the game
  (a literal), because results are cached by pointer.
- **Hand-over:** the launcher passes `TLG_LANG_PACK=<path of the pack>` to the game
  (`tlg_run.build_env`). The game reads only the `code` and the `menu` section.
- **Tests:** `tests/test_langpack.py`.
