# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard and mouse settings of the game, without Qt (the Controls screen uses it;
tlg_run.launch writes them to tlg.ini).

Every name here was checked in the game's code:
  - port/src/runtime_pad.c       key_map (default keyboard and mouse buttons), ps_names, apply_button_map
                                 (tlg.ini key_map or TLG_KEY_MAP: "<ps4 button>=<key>[|<key>...]",
                                 comma separated, "none"), the mouse sources
  - port/gpu/shim/tlg_settings.cpp  the tlg.ini keys mouse_enabled, mouse_sensitivity, mouse_invert_y,
                                 mouse_smoothing, mouse_accel, mouse_deadzone (and their ranges)
"""

# ------------------------------------------------------------------------- default keyboard map
# (PS4 button id, SDL scancode constant in runtime_pad.c, key name as the player types it)
KEYBOARD_BUTTONS = (
    ('cross', 'SPACE', 'Space'), ('circle', 'LSHIFT', 'Left Shift'), ('square', 'E', 'E|Mouse Middle'),
    ('triangle', 'Q', 'Q|Mouse Left'), ('l1', '1', '1'), ('r1', '3', '3|Mouse Right'), ('l2', 'R', 'R'), ('r2', 'F', 'F'),
    ('l3', 'Z', 'Z'), ('r3', 'C', 'C'), ('options', 'RETURN', 'Enter'),
    ('up', 'I', 'I'), ('left', 'J', 'J'), ('down', 'K', 'K'), ('right', 'L', 'L'),
    ('touchpad_left', 'TAB', 'Tab'), ('touchpad_right', 'BACKSPACE', 'Backspace'),
)
# Keys that are not remappable (read straight from the keyboard state in runtime_pad.c):
# (id, keys). The sensor keys T/G/Y, the sticks and the extra touchpad gestures.
FIXED_KEYS = (
    ('left_stick', 'W A S D'), ('right_stick', '← ↑ → ↓'),
    ('sensor_right', 'T'), ('sensor_left', 'G'), ('sensor_shake', 'Y'),
)
PS4_BUTTONS = ('cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3', 'options',
               'touchpad', 'touchpad_left', 'touchpad_right', 'up', 'down', 'left', 'right')
MOUSE_SOURCES = ('Mouse Left', 'Mouse Middle', 'Mouse Right', 'Mouse X1', 'Mouse X2',
                 'Wheel Up', 'Wheel Down', 'Wheel Left', 'Wheel Right')
# Key names SDL3 knows (SDL_GetScancodeName); the check is case-insensitive in the game. Not
# exhaustive: a name outside this list is only a warning, the game decides with SDL itself.
KEY_NAMES = frozenset(
    [chr(c) for c in range(ord('A'), ord('Z') + 1)] + [str(n) for n in range(10)] +
    [f'F{n}' for n in range(1, 25)] + [f'Keypad {n}' for n in range(10)] +
    ['Return', 'Escape', 'Backspace', 'Tab', 'Space', '-', '=', '[', ']', '\\', '#', ';', "'", '`', ',', '.',
     '/', 'CapsLock', 'PrintScreen', 'ScrollLock', 'Pause', 'Insert', 'Home', 'PageUp', 'Delete', 'End',
     'PageDown', 'Right', 'Left', 'Down', 'Up', 'Numlock', 'Keypad /', 'Keypad *', 'Keypad -', 'Keypad +',
     'Keypad Enter', 'Keypad .', 'Keypad =', 'Menu', 'Application',
     'Left Ctrl', 'Left Shift', 'Left Alt', 'Left GUI', 'Right Ctrl', 'Right Shift', 'Right Alt', 'Right GUI'])
_KEY_NAMES_LOWER = {name.lower() for name in KEY_NAMES}
MAX_SPEC = 1023        # runtime_pad.c copies the value into char copy[1024]
MAX_SOURCES = 8        # int sources[8] per entry


def parse_key_map(spec):
    """Checks a TLG_KEY_MAP value the way apply_button_map reads it.
    Returns (entries, errors, warnings): entries = [(ps4 button, [keys] or None for "none")];
    errors = [(code, detail)] that make the game ignore an entry or the whole value; warnings are
    key names this launcher cannot confirm (the game asks SDL). `code` is a Qt string suffix."""
    entries, errors, warnings = [], [], []
    spec = spec or ''
    if len(spec) > MAX_SPEC:
        errors.append(('keymap_err_long', str(MAX_SPEC)))
        return entries, errors, warnings
    for raw in spec.split(','):
        entry = raw.strip()
        if not entry:
            continue
        if '=' not in entry:
            errors.append(('keymap_err_format', entry))
            continue
        button, _, names = entry.partition('=')
        button, names = button.strip().lower(), names.strip()
        if button not in PS4_BUTTONS:
            errors.append(('keymap_err_button', button))
            continue
        if names.lower() == 'none':
            entries.append((button, None))
            continue
        keys = []
        for name in names.split('|'):
            name = name.strip()
            if not name:
                continue
            if name.lower() not in _KEY_NAMES_LOWER and name.lower() not in {m.lower() for m in MOUSE_SOURCES}:
                warnings.append(('keymap_warn_key', name))
            keys.append(name)
        if not keys:
            errors.append(('keymap_err_empty', button))
        elif len(keys) > MAX_SOURCES:
            errors.append(('keymap_err_many', button))
        else:
            entries.append((button, keys))
    return entries, errors, warnings


def effective_keys(spec):
    """{ps4 button: [keys]} for the remappable buttons: the default keyboard with the (valid)
    entries of `spec` applied; an empty list is a button left without any key ("none")."""
    keys = {button: name.split('|') for button, _, name in KEYBOARD_BUTTONS}
    keys['touchpad'] = keys['touchpad_left']          # alias in ps_names
    entries, _, _ = parse_key_map(spec)
    for button, names in entries:
        if button == 'touchpad':
            button = 'touchpad_left'
        keys[button] = list(names or [])
    return keys


# ------------------------------------------------------------------------------- mouse (tlg.ini)
# key, kind, default text, hard range of the game, range of the sliders
MOUSE_KEYS = (
    ('mouse_enabled', 'bool', '1', None, None),
    ('mouse_sensitivity', 'float', '1.00', (0.05, 10.0), (0.1, 5.0)),
    ('mouse_invert_y', 'bool', '0', None, None),
    ('mouse_smoothing', 'float', '0.50', (0.0, 0.95), (0.0, 0.9)),
    ('mouse_accel', 'float', '0.00', (0.0, 2.0), (0.0, 2.0)),
    ('mouse_deadzone', 'float', '0.10', (0.0, 0.5), (0.0, 0.5)),
)
MOUSE_SPEC = {row[0]: row for row in MOUSE_KEYS}


def mouse_value(ini, key):
    """The value the game will use for a mouse key of tlg.ini: bool for the switches, float otherwise."""
    _, kind, default, hard, _ = MOUSE_SPEC[key]
    text = str(ini.get(key, default)).strip()
    try:
        if kind == 'bool':
            return int(float(text)) != 0
        return min(max(float(text), hard[0]), hard[1])
    except ValueError:
        return mouse_value({}, key)


def mouse_update(key, value):
    """{key: text or None} to hand to backports_table.write_ini: None removes the key (default again)."""
    _, kind, default, hard, _ = MOUSE_SPEC[key]
    if kind == 'bool':
        text = '1' if value else '0'
    else:
        text = f'{min(max(float(value), hard[0]), hard[1]):.2f}'
    return {key: None if text == default else text}


def pad_kind(name):
    """'playstation' | 'xbox' | 'switch' | 'other' | 'none' from the label launcher/qt/logic.detect_pad gives."""
    if not name:
        return 'none'
    if name in ('DualSense', 'DualShock 4'):
        return 'playstation'
    return {'Xbox': 'xbox', 'Switch Pro': 'switch'}.get(name, 'other')
