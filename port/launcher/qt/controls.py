# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject behind the Controls screen: stick dead zone, the detected controller, the mouse
(tlg.ini keys the game reads) and the keyboard map (tlg.ini key_map, the same as the Tk launcher's
"Keyboard and mouse buttons" and the in-game menu). The logic and every key name live in
padsettings.py (checked against the game's code)."""
from PySide6.QtCore import Property, QObject, Signal, Slot

import padsettings

from .logic import backports_table, tlg_run
from .rowsmodel import RowsModel

# ids: 'c:' launcher setting, 'm:' tlg.ini mouse key.
MOUSE_ROWS = ('mouse_sensitivity', 'mouse_invert_y', 'mouse_smoothing', 'mouse_accel', 'mouse_deadzone')
# tlg.ini keys the launcher also keeps in settings.json (tlg_run.INI_KEYS): launch() writes them back,
# so the in-memory settings must follow every change made here.
SYNCED = {'mouse_enabled', 'mouse_sensitivity', 'mouse_invert_y'}


def key_map_text(value):
    """settings['key_map'] (the Tk launcher's {button: keys} dict, or older text) as tlg.ini text."""
    if isinstance(value, dict):
        return tlg_run.key_map_spec(value)
    return str(value or '').strip()


class Controls(QObject):
    changed = Signal()
    draftChanged = Signal()
    scrollRequested = Signal(int)   # used by the screenshot tool

    def __init__(self, backend):
        super().__init__(backend)
        self.backend = backend
        self.pad_model, self.mouse_model, self.touch_model = RowsModel(self), RowsModel(self), RowsModel(self)
        self._draft = key_map_text(backend.settings.get('key_map'))
        backend.profileChanged.connect(self.rebuild)
        backend.langChanged.connect(self.rebuild)
        backend.chipsChanged.connect(self.changed)          # a controller was plugged in
        self.rebuild()

    # -- helpers --------------------------------------------------------------------------------------
    def t(self, key, **fmt):
        return self.backend.t_(key, **fmt)

    @property
    def settings(self):
        return self.backend.settings

    def ini_path(self):
        return tlg_run.user_dir() / 'tlg.ini'

    def ini(self):
        return tlg_run.read_ini(self.ini_path())

    def mouse_on(self):
        return padsettings.mouse_value(self.ini(), 'mouse_enabled')

    # -- rows ---------------------------------------------------------------------------------------------
    def _row(self, rid, label, what, control, live, changed, **extra):
        row = {'kind': 'item', 'id': rid, 'code': '', 'label': label, 'what': what,
               'tags': [{'text': self.t('exp_live' if live else 'exp_restart'), 'tone': 'info'}],
               'shaderWarn': '', 'changed': changed, 'control': control, 'on': False, 'options': [], 'index': 0,
               'lo': 0, 'hi': 0, 'step': 1, 'unit': '', 'value': 0, 'valueText': '', 'disabled': False,
               'decimals': 0, 'note': '', 'info': '', 'action': None}
        row.update(extra)
        return row

    def _pad_rows(self):
        zone = int(self.settings.get('stick_deadzone', 10))
        rows = [self._row('c:stick_deadzone', self.t('deadzone'), self.t('deadzone_hint'), 'slider', False,
                          zone != tlg_run.DEFAULT_SETTINGS['stick_deadzone'], lo=0, hi=40, step=1, unit='%',
                          value=zone, valueText=f'{zone}%')]
        return rows

    def _mouse_rows(self):
        ini, off = self.ini(), not self.mouse_on()
        rows = []
        for key in MOUSE_ROWS:
            _, kind, default, _, slide = padsettings.MOUSE_SPEC[key]
            value = padsettings.mouse_value(ini, key)
            what = self.t('ctl_what_' + key)
            label = self.t('ctl_' + key)
            changed = key in ini and padsettings.mouse_update(key, value)[key] is not None
            if kind == 'bool':
                rows.append(self._row('m:' + key, label, what, 'switch', True, changed, on=value, disabled=off))
            else:
                value = min(max(value, slide[0]), slide[1])
                rows.append(self._row('m:' + key, label, what, 'slider', True, changed, lo=slide[0], hi=slide[1],
                                      step=0.05 if key == 'mouse_sensitivity' else 0.05, value=value,
                                      valueText=f'{value:.2f}', disabled=off, decimals=2))
        return rows

    def rebuild(self):
        self.pad_model.set_rows(self._pad_rows())
        self.mouse_model.set_rows(self._mouse_rows())
        self.touch_model.set_rows([])     # the mouse-as-touchpad mode of other ports: not in this game
        self.changed.emit()

    # -- writing --------------------------------------------------------------------------------------------
    @Slot(str, str)
    def setValue(self, rid, value):
        s = self.settings
        if rid == 'c:stick_deadzone':
            s['stick_deadzone'] = min(max(int(float(value)), 0), 40)
            tlg_run.save_settings(s)
        elif rid.startswith('m:'):
            key = rid[2:]
            kind = padsettings.MOUSE_SPEC[key][1]
            update = padsettings.mouse_update(key, value in ('1', 'true') if kind == 'bool' else float(value))
            backports_table.write_ini(self.ini_path(), update)
            self._sync(key)
        else:
            return
        self.rebuild()

    @Slot()
    def toggleMouse(self):
        on = self.mouse_on()
        backports_table.write_ini(self.ini_path(), padsettings.mouse_update('mouse_enabled', not on))
        self._sync('mouse_enabled')
        self.rebuild()

    def _sync(self, key):
        """Mirrors a tlg.ini mouse key into settings.json (tlg_run.launch writes those back at Play)."""
        if key in SYNCED:
            self.settings[key] = padsettings.mouse_value(self.ini(), key)
            tlg_run.save_settings(self.settings)

    # -- controller ------------------------------------------------------------------------------------------------
    @Property(str, notify=changed)
    def padName(self):
        return self.backend._pad or self.t('ctl_pad_none')

    @Property(str, notify=changed)
    def padKind(self):
        return padsettings.pad_kind(self.backend._pad)

    @Property(str, notify=changed)
    def padKindText(self):
        kind = self.padKind
        return self.t('ctl_kind_' + kind)

    @Property(str, notify=changed)
    def padHint(self):
        return self.t('ctl_hint_' + self.padKind) if self.padFound else ''

    @Property(bool, notify=changed)
    def padFound(self):
        return bool(self.backend._pad)

    @Property(QObject, constant=True)
    def padRows(self):
        return self.pad_model

    # -- mouse ---------------------------------------------------------------------------------------------------------
    @Property(QObject, constant=True)
    def mouseRows(self):
        return self.mouse_model

    @Property(QObject, constant=True)
    def touchRows(self):
        return self.touch_model

    @Property(bool, notify=changed)
    def mouseEnabled(self):
        return self.mouse_on()

    @Property(str, notify=changed)
    def mouseButtonText(self):
        return self.t('ctl_mouse_turn_on' if not self.mouse_on() else 'ctl_mouse_turn_off')

    # -- keyboard --------------------------------------------------------------------------------------------------------
    @Property('QVariantList', notify=changed)
    def keyRows(self):
        keys = padsettings.effective_keys(key_map_text(self.settings.get('key_map')))
        default = padsettings.effective_keys('')
        rows = [{'name': self.t('pad_left_stick'), 'keys': 'W A S D', 'fixed': True, 'changed': False},
                {'name': self.t('pad_right_stick'), 'keys': '← ↑ → ↓', 'fixed': True, 'changed': False}]
        for button, _, _ in padsettings.KEYBOARD_BUTTONS:
            names = keys[button]
            rows.append({'name': self.t('pad_btn_' + button), 'keys': ' | '.join(names) if names else self.t('ctl_key_none'),
                         'fixed': False, 'changed': names != default[button]})
        rows += [{'name': self.t('pad_sensor_right'), 'keys': 'T', 'fixed': True, 'changed': False},
                 {'name': self.t('pad_sensor_left'), 'keys': 'G', 'fixed': True, 'changed': False},
                 {'name': self.t('pad_sensor_shake'), 'keys': 'Y', 'fixed': True, 'changed': False}]
        return rows

    @Property(str, notify=changed)
    def savedKeyMap(self):
        return key_map_text(self.settings.get('key_map'))

    def draft_info(self):
        entries, errors, warnings = padsettings.parse_key_map(self._draft)
        messages = [{'tone': 'bad', 'text': self.t(code, detail=detail)} for code, detail in errors]
        messages += [{'tone': 'warn', 'text': self.t(code, detail=detail)} for code, detail in warnings]
        return entries, errors, messages

    @Property(str, notify=draftChanged)
    def draftText(self):
        return self._draft

    @Slot(str)
    def setDraft(self, text):
        if text != self._draft:
            self._draft = text
            self.draftChanged.emit()

    @Property('QVariantList', notify=draftChanged)
    def draftMessages(self):
        entries, errors, messages = self.draft_info()
        if not errors and self._draft.strip():
            messages.insert(0, {'tone': 'good', 'text': self.t('keymap_ok', n=len(entries))})
        return messages

    @Property(bool, notify=draftChanged)
    def draftValid(self):
        return not self.draft_info()[1]

    @Property(bool, notify=draftChanged)
    def draftDirty(self):
        return self._draft.strip() != self.savedKeyMap

    @Slot()
    def applyKeyMap(self):
        if self.draftValid:
            text = self._draft.strip()
            self.settings['key_map'] = tlg_run.parse_key_map(text)   # the Tk launcher's {button: keys}
            tlg_run.save_settings(self.settings)
            self.draftChanged.emit()
            self.changed.emit()

    @Slot()
    def clearKeyMap(self):
        self._draft = ''
        self.settings['key_map'] = {}
        tlg_run.save_settings(self.settings)
        self.draftChanged.emit()
        self.changed.emit()
