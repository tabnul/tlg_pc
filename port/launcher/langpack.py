"""Launcher language packs.

English is built into the code. A language pack is one JSON file that translates any part of it;
whatever a pack leaves out (or gets wrong, see check()) stays English. Packs live in
  launcher/lang/<code>.json         shipped with the port
  <user folder>/lang/<code>.json    the player's own (wins over a shipped pack with the same code)
Format and workflow: port/docs/language-packs.md.

  python langpack.py list                  packs found, with how much they translate
  python langpack.py template <code>       a new pack with every English text, to translate
  python langpack.py check <pack.json>     problems: unknown keys, broken {placeholders}
  python langpack.py update <pack.json>    add texts that are new since the pack was made (in English)
  python langpack.py menu-scan             developers: refresh lang/menu-texts.json from the menu source
"""
import json
import re
import string
import sys
from pathlib import Path

FORMAT = 1
HERE = Path(__file__).resolve().parent
SHIPPED_DIR = HERE / 'lang'
ENGLISH = 'en'

_english = None     # snapshot of every English text, taken before the first pack is applied
_active = ENGLISH


def _modules():
    """The text tables per section. Imported late: tlg_launcher imports this module."""
    import backports_table
    import pad_page
    import preset_share
    import tlg_launcher
    import tlg_run
    return backports_table, {'launcher': tlg_launcher.TEXT, 'pad': pad_page.TEXT,
                             'preset': preset_share.TEXT, 'run': tlg_run.TEXT}


def _item_lists(backports_table):
    """Every module-level list of items (ITEMS, MENU, READBACK, ...): all are rewritten in place."""
    return [value for value in vars(backports_table).values()
            if isinstance(value, list) and value and all(isinstance(i, backports_table.Item) for i in value)]


def english():
    """{'text': {section.key: str}, 'items': {id: {name, what, why?, choices?}}}: the built-in texts."""
    global _english
    if _english is None:
        backports_table, tables = _modules()
        text = {f'{section}.{key}': value for section, table in tables.items()
                for key, value in table.items() if isinstance(value, str)}
        items = {}
        for item in backports_table.ITEMS:
            entry = {'name': item.name, 'what': item.what}
            why = backports_table.READBACK_WHY.get(item.id)
            if why:
                entry['why'] = why
            labels = {value: label for value, label in item.choices if label != value}
            if labels:
                entry['choices'] = labels
            items[item.id] = entry
        _english = {'text': text, 'items': items, 'menu': menu_texts()}
    return _english


# ---- in-game menu -------------------------------------------------------------------------------
# The menu (gpu/shim/tlg_overlay.cpp) looks its texts up by their English wording: TR("Save game").
# MENU_TEXTS lists them, made from the source by `langpack.py menu-scan` (a test checks it is
# current), so the template/check/update commands work without the C++ sources.
MENU_TEXTS = SHIPPED_DIR / 'menu-texts.json'
MENU_SOURCES = ('gpu/shim/tlg_overlay.cpp',)
MENU_CONSTANTS = 'gpu/shim/tlg_fun.h'      # TR(TlgFun::Name) / TR(TlgFun::Name[i]) come from here
_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
# printf conversions as tlg_lang.h counts them: no space flag, so "0% turns" is a plain percent sign.
_PRINTF = re.compile(r'%[-+#0-9.*]*[hlLqjzt]*[diouxXeEfFgGaAcsp]')


def menu_texts():
    try:
        return json.loads(MENU_TEXTS.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return []


def _c_string(body):
    """The text of one C string literal body (UTF-8 source, the usual escapes)."""
    out, raw, i = bytearray(), body.encode('utf-8'), 0
    simple = {'n': 10, 't': 9, 'r': 13, '"': 34, "'": 39, '\\': 92, '0': 0, 'a': 7, '?': 63}
    while i < len(raw):
        if raw[i] == 92:
            escape = chr(raw[i + 1])
            if escape == 'x':
                digits = re.match(rb'[0-9a-fA-F]+', raw[i + 2:]).group()
                out.append(int(digits, 16))
                i += 2 + len(digits)
                continue
            out.append(simple[escape])
            i += 2
            continue
        out.append(raw[i])
        i += 1
    return out.decode('utf-8')


def _call_args(src, start):
    """The argument text of a call whose '(' is at start-1, up to its ')'."""
    depth, j = 0, start
    while True:
        c = src[j]
        if c == '"':
            j += 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                return src[start:j]
            depth -= 1
        j += 1


def scan_menu(port_dir):
    """Every English text the menu passes to TR(), in source order, no duplicates."""
    port_dir = Path(port_dir)
    constants = {}
    header = (port_dir / MENU_CONSTANTS).read_text(encoding='utf-8')
    for m in re.finditer(r'constexpr const char\* (\w+)(\[\])? =\s*(\{[^}]*\}|(?:\s*"(?:[^"\\]|\\.)*")+)', header):
        values = [_c_string(body) for body in _LITERAL.findall(m.group(3))]
        constants[m.group(1)] = values if m.group(2) else ''.join(values)
    found = []
    for name in MENU_SOURCES:
        src = (port_dir / name).read_text(encoding='utf-8')
        for m in re.finditer(r'\bTR\(', src):
            args = _call_args(src, m.end()).strip()
            ref = re.fullmatch(r'TlgFun::(\w+)(?:\[(\d+)\])?', args)
            if ref:
                value = constants[ref.group(1)]
                texts = [value[int(ref.group(2))]] if ref.group(2) else [value]
            elif _LITERAL.sub('', args).strip() == '' and _LITERAL.search(args):
                texts = [''.join(_c_string(body) for body in _LITERAL.findall(args))]
            else:
                continue   # TR(en) inside the definition itself
            found += [text for text in texts if text not in found]
    return found


def _printf(text):
    return _PRINTF.findall(text)


def _fields(text):
    """The {placeholders} of a text, or None when the braces are broken."""
    try:
        return sorted({field for _, field, _, _ in string.Formatter().parse(text) if field is not None})
    except ValueError:
        return None


def _usable(translated, original):
    return isinstance(translated, str) and translated.strip() != '' and _fields(translated) == _fields(original)


def pack_dirs():
    """Search order: shipped first, the user's own after it (a later one wins)."""
    dirs = [SHIPPED_DIR]
    try:
        import tlg_run
        dirs.append(tlg_run.user_dir() / 'lang')
    except Exception:
        pass
    return dirs


def read_pack(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(data, dict) or not isinstance(data.get('code'), str):
        raise ValueError(f'{path}: not a language pack (no "code")')
    if data.get('format', FORMAT) > FORMAT:
        raise ValueError(f'{path}: made for a newer launcher (format {data["format"]})')
    return data


def available():
    """{code: (display name, path)} for every readable pack; English is always there."""
    packs = {ENGLISH: ('English', None)}
    for folder in pack_dirs():
        if not folder.is_dir():
            continue
        for path in sorted(folder.glob('*.json')):
            if path.name == MENU_TEXTS.name:
                continue
            try:
                data = read_pack(path)
            except (OSError, ValueError) as exc:
                print(f'language pack skipped: {exc}', file=sys.stderr)
                continue
            if data['code'] != ENGLISH:
                packs[data['code']] = (data.get('name') or data['code'], path)
    return packs


def resolve(setting):
    """The pack code for the launcher setting: a code, or 'auto' = the system language when a pack has it."""
    packs = available()
    if setting in packs:
        return setting
    if setting != 'auto':
        return ENGLISH
    import tlg_run
    system = tlg_run.system_game_language()
    if system in packs:
        return system
    prefix = system.split('_')[0]
    return next((code for code in packs if code.split('_')[0] == prefix), ENGLISH)


def apply(code):
    """Put the texts of pack `code` (or English) into the launcher's tables. Safe to call again:
    every call starts from English, so switching packs never leaves texts of the previous one.
    Returns the code in use (English when the pack is missing or unreadable)."""
    global _active
    base = english()
    backports_table, tables = _modules()
    pack = {}
    path = available().get(code, (None, None))[1]
    if path is not None:
        try:
            pack = read_pack(path)
        except (OSError, ValueError) as exc:
            print(f'language pack not used: {exc}', file=sys.stderr)
            code = ENGLISH
    else:
        code = ENGLISH
    text, items = pack.get('text') or {}, pack.get('items') or {}

    for full_key, original in base['text'].items():
        section, key = full_key.split('.', 1)
        wanted = text.get(full_key)
        tables[section][key] = wanted if _usable(wanted, original) else original

    for item_list in _item_lists(backports_table):
        for index, item in enumerate(item_list):
            item_list[index] = _translated_item(item, base['items'].get(item.id), items.get(item.id) or {})
    for item_id, entry in base['items'].items():
        if 'why' in entry:
            wanted = (items.get(item_id) or {}).get('why')
            backports_table.READBACK_WHY[item_id] = wanted if _usable(wanted, entry['why']) else entry['why']
    _active = code
    return code


def _translated_item(item, original, wanted):
    if original is None:
        return item
    name = wanted.get('name') if _usable(wanted.get('name'), original['name']) else original['name']
    what = wanted.get('what') if _usable(wanted.get('what'), original['what']) else original['what']
    labels, own = original.get('choices', {}), wanted.get('choices') or {}
    choices = tuple((value, own[value] if value in labels and _usable(own.get(value), labels[value])
                     else labels.get(value, value)) for value, _ in item.choices)
    return item._replace(name=name, what=what, choices=choices)


def active():
    return _active


def template(code, name=''):
    """A new pack: every English text under its key, ready to be overwritten."""
    base = english()
    return {'format': FORMAT, 'code': code, 'name': name or code, 'english_name': '', 'authors': [],
            'text': dict(base['text']), 'items': json.loads(json.dumps(base['items'])),
            'menu': {text: text for text in base['menu']}}


def check(pack):
    """(problems, translated, total): problems are lines a translator must fix; a text equal to
    English counts as untranslated (some, like 'FPS', are meant to stay)."""
    base = english()
    problems, translated, total = [], 0, 0
    for key in pack.get('text') or {}:
        if key not in base['text']:
            problems.append(f'unknown text key (removed or misspelled): {key}')
    for key, original in base['text'].items():
        total += 1
        wanted = (pack.get('text') or {}).get(key)
        if wanted is None:
            continue
        if not _usable(wanted, original):
            problems.append(f'{key}: placeholders must be {_fields(original)}, are {_fields(wanted)}')
        elif wanted != original:
            translated += 1
    for item_id, entry in (pack.get('items') or {}).items():
        original = base['items'].get(item_id)
        if original is None:
            problems.append(f'unknown item (removed or misspelled): {item_id}')
            continue
        for field in ('name', 'what', 'why'):
            if field in entry and field not in original:
                problems.append(f'{item_id}.{field}: this item has no {field}')
            elif field in entry and not _usable(entry[field], original[field]):
                problems.append(f'{item_id}.{field}: placeholders must be {_fields(original[field])}')
        for value in entry.get('choices') or {}:
            if value not in original.get('choices', {}):
                problems.append(f'{item_id}.choices: unknown value {value!r}')
    for item_id, original in base['items'].items():
        entry = (pack.get('items') or {}).get(item_id) or {}
        for field in ('name', 'what', 'why'):
            if field in original:
                total += 1
                translated += entry.get(field, original[field]) != original[field]
        for value, label in original.get('choices', {}).items():
            total += 1
            translated += (entry.get('choices') or {}).get(value, label) != label
    menu, known = pack.get('menu') or {}, set(base['menu'])
    for original, wanted in menu.items():
        if original not in known:
            problems.append(f'menu: unknown text (changed or removed in the game menu): {original[:60]!r}')
        elif not isinstance(wanted, str) or _printf(wanted) != _printf(original):
            problems.append(f'menu: {original[:40]!r}: placeholders must be {_printf(original)} in this order')
    for original in base['menu']:
        total += 1
        translated += isinstance(menu.get(original), str) and menu[original] not in ('', original)
    return problems, translated, total


def update(pack):
    """Add (in English) every text the pack lacks; returns how many were added."""
    base, added = english(), 0
    text = pack.setdefault('text', {})
    for key, original in base['text'].items():
        if key not in text:
            text[key] = original
            added += 1
    items = pack.setdefault('items', {})
    for item_id, original in base['items'].items():
        entry = items.setdefault(item_id, {})
        for field in ('name', 'what', 'why'):
            if field in original and field not in entry:
                entry[field] = original[field]
                added += 1
        if original.get('choices'):
            choices = entry.setdefault('choices', {})
            for value, label in original['choices'].items():
                if value not in choices:
                    choices[value] = label
                    added += 1
    menu = pack.setdefault('menu', {})
    for original in base['menu']:
        if original not in menu:
            menu[original] = original
            added += 1
    return added


def write_pack(pack, path):
    Path(path).write_text(json.dumps(pack, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def _main(argv):
    sys.stdout.reconfigure(encoding='utf-8')
    if not argv or argv[0] not in ('list', 'template', 'check', 'update', 'menu-scan'):
        print(__doc__)
        return 2
    if argv[0] == 'menu-scan':
        texts = scan_menu(HERE.parent)
        SHIPPED_DIR.mkdir(exist_ok=True)
        MENU_TEXTS.write_text(json.dumps(texts, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        print(f'wrote {MENU_TEXTS}: {len(texts)} menu texts')
        return 0
    if argv[0] == 'list':
        for code, (name, path) in available().items():
            if path is None:
                print(f'{code:8} {name} (built in)')
                continue
            _, done, total = check(read_pack(path))
            print(f'{code:8} {name}: {done}/{total} texts translated ({path})')
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[0] == 'template':
        path = Path(f'{argv[1]}.json')
        write_pack(template(argv[1]), path)
        print(f'wrote {path}: translate the values, keep the keys and the {{placeholders}}')
        return 0
    pack = read_pack(argv[1])
    if argv[0] == 'update':
        added = update(pack)
        write_pack(pack, argv[1])
        print(f'{argv[1]}: added {added} new texts (in English, to translate)')
        return 0
    problems, done, total = check(pack)
    for line in problems:
        print('  ' + line)
    print(f'{argv[1]}: {done}/{total} texts translated, {len(problems)} problems')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.path.insert(0, str(HERE))
    sys.exit(_main(sys.argv[1:]))
