"""The launcher's Controller page: which controller the game uses, a live drawing of it, and
recalibration (stick dead zones with a drift check, trigger point, buttons, rumble and motion test).

Controllers are read through out/tlg-pad-tool (tools/pad_tool.c), built with the game's own SDL3:
the page detects and lays out a controller exactly as the game will. SDL knows DualShock 3,
DualShock 4, DualSense, Xbox and Switch controllers and maps each to the same layout by position
(the bottom face button is Cross on every pad), so the defaults are right for each of them; the
page only stores changes. Settings go to the game as TLG_PAD_GUID, TLG_STICK_DEADZONE(_RIGHT),
TLG_TRIGGER_THRESHOLD and TLG_PAD_MAP (tlg_run.build_env).
"""
import queue
import subprocess
import threading
import tkinter as tk
from tkinter import ttk

import pad_drawing
import tlg_run

TEXT = {
    'title': 'Controller',
    'use': 'Controller used by the game',
    'auto': 'Automatic (the one used last)',
    'none': 'No controller found. Connect one (USB or Bluetooth); it shows up here at once.',
    'no_tool': 'The controller helper (out/tlg-pad-tool) is missing: build the port again.',
    'missing': 'not connected',
    'detected': 'Detected: {kind}',
    'layout': 'Buttons are mapped by position: the bottom button is Cross, the right one Circle.',
    'motion_yes': 'motion sensor',
    'motion_no': 'no motion sensor (tilt is simulated)',
    'rumble_yes': 'rumble',
    'rumble_no': 'no rumble',
    'sticks': 'Sticks',
    'left': 'Left dead zone',
    'right': 'Right dead zone',
    'drift': 'Check drift',
    'drift_wait': 'Leave both sticks alone…',
    'drift_done': 'Rest drift: left {l} %, right {r} %. Dead zones set to {lz} % and {rz} %.',
    'triggers': 'Triggers',
    'trigger': 'L2 / R2 press point',
    'buttons': 'Buttons',
    'press': 'press a button…',
    'set': 'Set',
    'default': 'default',
    'reset': 'Reset to defaults',
    'test': 'Test',
    'rumble': 'Rumble',
    'gyro': 'Motion',
    'combos': 'Combos',
    'combo_quit': 'Quit game (hold 2 seconds)',
    'combo_menu': 'In-game menu',
    'combo_qa': 'QA menu',
    'combo_press': 'hold the buttons, then release…',
    'combo_help': 'Two or more buttons together. Keyboard: Alt+Q quits, Insert opens the in-game menu.',
    'credit': 'Drawing: RPCS3 DualShock 3 by HerrHulaHoop (GPL-2.0), made a DualShock 4 for this port.',
}
KINDS = {'ps3': 'DualShock 3 (PS3)', 'ps4': 'DualShock 4 (PS4)', 'ps5': 'DualSense (PS5)',
         'xbox360': 'Xbox 360', 'xboxone': 'Xbox One / Series', 'switchpro': 'Switch Pro', 'joycon': 'Joy-Con',
         'generic': 'Generic controller'}
# SDL_GamepadButton order (bit n of the helper's mask) and the SDL names TLG_PAD_MAP takes.
SDL_BUTTONS = ['a', 'b', 'x', 'y', 'back', 'guide', 'start', 'leftstick', 'rightstick', 'leftshoulder',
               'rightshoulder', 'dpup', 'dpdown', 'dpleft', 'dpright', 'misc1', 'paddle1', 'paddle2', 'paddle3',
               'paddle4', 'touchpad']
# What each SDL button is called on a PlayStation / Xbox pad.
PS_LABEL = ['Cross', 'Circle', 'Square', 'Triangle', 'Share', 'PS', 'Options', 'L3', 'R3', 'L1', 'R1',
            'D-pad up', 'D-pad down', 'D-pad left', 'D-pad right', 'Mute / Create', 'Paddle 1', 'Paddle 2',
            'Paddle 3', 'Paddle 4', 'Touchpad']
XBOX_LABEL = ['A', 'B', 'X', 'Y', 'View', 'Xbox', 'Menu', 'LS', 'RS', 'LB', 'RB', 'D-pad up', 'D-pad down',
              'D-pad left', 'D-pad right', 'Share', 'P1', 'P2', 'P3', 'P4', 'Touchpad']
# Drawing part lit by each SDL button.
DRAWING = {0: 'ButtonCross', 1: 'ButtonCircle', 2: 'ButtonSquare', 3: 'ButtonTriangle', 4: 'ShareButton',
           5: 'PSButton', 6: 'OptionsButton', 9: 'LeftShoulder', 10: 'RightShoulder', 11: 'DpadButtonTop',
           12: 'DpadButtonBottom', 13: 'DpadButtonLeft', 14: 'DpadButtonRight', 20: 'Touchpad'}
# Controller combos (tlg.ini pad_combo_*, tlg_settings.cpp ComboNames): name -> bit. Bits 0-20 are the
# SDL buttons, L2/R2 (the triggers) are bits 24/25, as in the game.
COMBO_BITS = {'cross': 0, 'circle': 1, 'square': 2, 'triangle': 3, 'l1': 9, 'r1': 10, 'l2': 24, 'r2': 25,
              'l3': 7, 'r3': 8, 'share': 4, 'options': 6, 'touchpad': 20, 'ps': 5, 'up': 11, 'down': 12,
              'left': 13, 'right': 14}
COMBO_LABELS = {'share': 'Share', 'options': 'Options', 'touchpad': 'Touchpad', 'ps': 'PS', 'cross': 'Cross',
                'circle': 'Circle', 'square': 'Square', 'triangle': 'Triangle', 'up': 'Up', 'down': 'Down',
                'left': 'Left', 'right': 'Right'}
COMBOS = (('pad_combo_quit', 'combo_quit', 'share+options'), ('pad_combo_menu', 'combo_menu', 'l2+r3'),
          ('pad_combo_qa', 'combo_qa', 'l1+l3'))


def parse_combo(text, fallback):
    bits = 0
    for part in (text or '').lower().split('+'):
        if part.strip() in COMBO_BITS:
            bits |= 1 << COMBO_BITS[part.strip()]
    if not bits and fallback:
        return parse_combo(fallback, '')
    return bits


def combo_text(bits):
    return '+'.join(name for name, bit in COMBO_BITS.items() if bits & (1 << bit))


def combo_label(bits):
    return ' + '.join(COMBO_LABELS.get(n, n.upper()) for n in combo_text(bits).split('+') if n)


# The game's PS4 buttons that take gamepad buttons, with their default SDL source (runtime_pad.c pad_map).
REMAP = [('cross', 'Cross', 'a'), ('circle', 'Circle', 'b'), ('square', 'Square', 'x'),
         ('triangle', 'Triangle', 'y'), ('l1', 'L1', 'leftshoulder'), ('r1', 'R1', 'rightshoulder'),
         ('l3', 'L3', 'leftstick'), ('r3', 'R3', 'rightstick'), ('options', 'Options', 'start'),
         ('touchpad_right', 'Touchpad (right)', 'back'), ('up', 'D-pad up', 'dpup'),
         ('down', 'D-pad down', 'dpdown'), ('left', 'D-pad left', 'dpleft'), ('right', 'D-pad right', 'dpright')]


class PadTool:
    """Runs tlg-pad-tool and turns its lines into a queue the page polls."""

    def __init__(self):
        self.lines = queue.Queue()
        self.process = None
        path = tlg_run.pad_tool()
        if not path:
            return
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        try:
            self.process = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                            env=tlg_run.pad_tool_env(), creationflags=flags)
        except OSError:
            self.process = None
            return
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip('\n'))

    def send(self, command):
        try:
            self.process.stdin.write(command + '\n')
            self.process.stdin.flush()
        except (OSError, AttributeError, ValueError):
            pass

    def stop(self):
        if self.process and self.process.poll() is None:
            self.send('quit')
            try:
                self.process.wait(1)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None


class ControllerPage:
    def __init__(self, app, page, colors):
        self.app, self.page = app, page
        self.BG, self.LINE, self.TEXT, self.MUTED, self.ACCENT, self.FIELD = colors
        self.tool = None
        self.pads = {}            # id -> dict(type, guid, gyro, rumble, name)
        self.state = {}           # id -> (buttons, lx, ly, rx, ry, lt, rt, gx, gy, gz)
        self.pending = None       # pads list being read
        self.capture = None       # (ps button, label widget) waiting for a press
        self.capture_mask = 0
        self.drift = None         # [samples left, max l, max r]
        self.job = None
        self.build()

    def t(self, key, **fmt):
        return TEXT[key].format(**fmt)

    # -- layout ---------------------------------------------------------------------------
    def build(self):
        s = self.app.settings
        body = ttk.Frame(self.page, padding=(12, 4, 12, 8))
        body.pack(fill='both', expand=True)
        left = ttk.Frame(body)
        left.pack(side='left', fill='both', expand=True)
        right = ttk.Frame(body)
        right.pack(side='left', fill='y', padx=(16, 0))

        ttk.Label(left, text=self.t('use')).pack(anchor='w')
        self.choice = ttk.Combobox(left, state='readonly', width=54)
        self.choice.pack(anchor='w', pady=(2, 4))
        self.choice.bind('<<ComboboxSelected>>', self.choose)
        self.info = ttk.Label(left, text='', foreground=self.MUTED)
        self.info.pack(anchor='w')
        ttk.Label(left, text=self.t('layout'), foreground=self.MUTED).pack(anchor='w', pady=(0, 6))
        self.scale = 1.75
        self.canvas = tk.Canvas(left, width=int(400 * self.scale + 20), height=int(250 * self.scale + 20),
                                background=self.BG, highlightthickness=0)
        self.canvas.pack(anchor='w')
        self.items, circles = pad_drawing.draw(self.canvas, self.scale, 10, 18, self.LINE, self.MUTED)
        self.sticks = {}
        for side, key in (('l', 'LeftJoystickInner'), ('r', 'RightJoystickInner')):
            x, y, r = circles[key]
            zone = self.canvas.create_oval(0, 0, 0, 0, outline=self.MUTED, dash=(2, 3))
            dot = self.canvas.create_oval(x - 7, y - 7, x + 7, y + 7, fill=self.ACCENT, outline='')
            self.sticks[side] = (x, y, r, dot, zone)
        self.triggers = {}
        for side, x in (('lt', 10 + 84 * self.scale), ('rt', 10 + 318 * self.scale)):
            self.canvas.create_rectangle(x - 30, 2, x + 30, 10, outline=self.LINE)
            self.triggers[side] = (x - 30, self.canvas.create_rectangle(x - 30, 2, x - 30, 10, fill=self.ACCENT, outline=''),
                                   self.canvas.create_line(0, 0, 0, 0, fill=self.TEXT))
        test = ttk.Frame(left)
        test.pack(anchor='w', pady=(6, 0))
        ttk.Label(test, text=self.t('test') + ':').pack(side='left')
        self.rumble_button = ttk.Button(test, text=self.t('rumble'), command=self.rumble)
        self.rumble_button.pack(side='left', padx=6)
        self.gyro = ttk.Label(test, text='', foreground=self.MUTED, width=40)
        self.gyro.pack(side='left', padx=6)
        ttk.Label(left, text=self.t('credit'), foreground=self.MUTED, font=('TkDefaultFont', 8)).pack(anchor='w', pady=(8, 0))
        self.status = ttk.Label(left, text='', foreground=self.ACCENT, wraplength=700)
        self.status.pack(anchor='w', pady=(4, 0))

        ttk.Label(right, text=self.t('sticks'), font=('TkDefaultFont', 10, 'bold')).pack(anchor='w')
        self.zone, self.zone_labels = {}, {}
        for key, setting in (('left', 'stick_deadzone'), ('right', 'stick_deadzone_right')):
            row = ttk.Frame(right)
            row.pack(fill='x', pady=1)
            ttk.Label(row, text=self.t(key), width=22).pack(side='left')
            var = tk.IntVar(value=int(s.get(setting, s.get('stick_deadzone', 10))))
            shown = ttk.Label(row, text=f'{var.get()} %', width=5)
            ttk.Scale(row, from_=0, to=50, variable=var, length=160,
                      command=lambda _, v=var, l=shown, k=setting: self.set_zone(v, l, k)).pack(side='left')
            shown.pack(side='left', padx=4)
            self.zone[setting] = var
            self.zone_labels[setting] = shown
        self.drift_button = ttk.Button(right, text=self.t('drift'), command=self.check_drift)
        self.drift_button.pack(anchor='w', pady=(2, 8))
        ttk.Label(right, text=self.t('triggers'), font=('TkDefaultFont', 10, 'bold')).pack(anchor='w')
        row = ttk.Frame(right)
        row.pack(fill='x', pady=(1, 8))
        ttk.Label(row, text=self.t('trigger'), width=22).pack(side='left')
        self.trigger = tk.IntVar(value=int(s.get('trigger_threshold', 12)))
        shown = self.trigger_label = ttk.Label(row, text=f'{self.trigger.get()} %', width=5)
        ttk.Scale(row, from_=2, to=90, variable=self.trigger, length=160,
                  command=lambda _: self.set_trigger(shown)).pack(side='left')
        shown.pack(side='left', padx=4)
        ttk.Label(right, text=self.t('buttons'), font=('TkDefaultFont', 10, 'bold')).pack(anchor='w')
        grid = ttk.Frame(right)
        grid.pack(fill='x')
        self.sources = {}
        for index, (ps, label, _) in enumerate(REMAP):
            ttk.Label(grid, text=label, width=16).grid(row=index, column=0, sticky='w')
            shown = ttk.Label(grid, text='', width=22, foreground=self.MUTED)
            shown.grid(row=index, column=1, sticky='w')
            ttk.Button(grid, text=self.t('set'), width=8,
                       command=lambda p=ps, l=shown: self.start_capture(p, l)).grid(row=index, column=2, pady=1)
            self.sources[ps] = shown
        ttk.Button(right, text=self.t('reset'), command=self.reset).pack(anchor='w', pady=(8, 0))
        ttk.Label(right, text=self.t('combos'), font=('TkDefaultFont', 10, 'bold')).pack(anchor='w', pady=(8, 0))
        grid = ttk.Frame(right)
        grid.pack(fill='x')
        ini = tlg_run.read_ini(tlg_run.user_dir() / 'tlg.ini')
        self.combo_shown = {}
        for index, (key, text, default) in enumerate(COMBOS):
            ttk.Label(grid, text=self.t(text), width=26).grid(row=index, column=0, sticky='w')
            shown = ttk.Label(grid, text=combo_label(parse_combo(ini.get(key), default)), width=18,
                              foreground=self.MUTED)
            shown.grid(row=index, column=1, sticky='w')
            ttk.Button(grid, text=self.t('set'), width=8,
                       command=lambda k=key: self.start_combo(k)).grid(row=index, column=2, pady=1)
            ttk.Button(grid, text=self.t('default'), width=8,
                       command=lambda k=key, d=default: self.store_combo(k, parse_combo(d, ''))).grid(
                row=index, column=3, pady=1, padx=(4, 0))
            self.combo_shown[key] = shown
        ttk.Label(right, text=self.t('combo_help'), foreground=self.MUTED, wraplength=420).pack(anchor='w')
        self.show_sources()

    # -- settings --------------------------------------------------------------------------
    def save(self):
        tlg_run.save_settings(self.app.settings)

    def set_zone(self, var, label, key):
        label.configure(text=f'{var.get()} %')
        self.app.settings[key] = int(var.get())
        self.save()

    def set_trigger(self, label):
        label.configure(text=f'{self.trigger.get()} %')
        self.app.settings['trigger_threshold'] = int(self.trigger.get())
        self.save()

    def kind(self):
        pad = self.pads.get(self.shown_id())
        return pad['type'] if pad else 'ps4'

    def label_for(self, name):
        names = PS_LABEL if not self.kind().startswith(('xbox', 'generic')) else XBOX_LABEL
        index = SDL_BUTTONS.index(name) if name in SDL_BUTTONS else -1
        return names[index] if index >= 0 else name

    def show_sources(self):
        mapping = self.app.settings.get('pad_map') or {}
        for ps, _, default in REMAP:
            name = mapping.get(ps)
            text = self.label_for(name) if name else f'{self.label_for(default)}  ({self.t("default")})'
            self.sources[ps].configure(text=text, foreground=self.TEXT if name else self.MUTED)

    def start_capture(self, ps, label):
        self.capture = (ps, label)
        state = self.state.get(self.shown_id())
        self.capture_mask = state[0] if state else 0   # only a new press counts
        label.configure(text=self.t('press'), foreground=self.ACCENT)

    # -- combos (written to tlg.ini at once; the in-game Controls tab edits the same keys) ----
    def start_combo(self, key):
        self.combo_capture = [key, None, 0]   # key, buttons held at the start, buttons pressed since
        self.combo_shown[key].configure(text=self.t('combo_press'), foreground=self.ACCENT)

    def store_combo(self, key, bits):
        tlg_run.update_ini(tlg_run.user_dir() / 'tlg.ini', {key: combo_text(bits)})
        self.combo_shown[key].configure(text=combo_label(bits), foreground=self.MUTED)

    def combo_poll(self, buttons, lt, rt):
        threshold = self.trigger.get() / 100
        held = buttons & 0x1fffff
        held |= (1 << 24) if lt / 32767 > threshold else 0
        held |= (1 << 25) if rt / 32767 > threshold else 0
        key, ignore, pressed = self.combo_capture
        ignore = held if ignore is None else ignore & held
        pressed |= held & ~ignore
        self.combo_capture = [key, ignore, pressed]
        if not held and pressed:
            if bin(pressed).count('1') >= 2:
                self.combo_capture = None
                self.store_combo(key, pressed)
            else:
                self.combo_capture[2] = 0   # one button only: keep waiting

    def finish_capture(self, bit):
        ps, _ = self.capture
        self.capture = None
        mapping = dict(self.app.settings.get('pad_map') or {})
        default = dict((p, d) for p, _, d in REMAP)[ps]
        name = SDL_BUTTONS[bit]
        if name == default:
            mapping.pop(ps, None)
        else:
            mapping[ps] = name
        self.app.settings['pad_map'] = mapping
        self.save()
        self.show_sources()

    def reset(self):
        s = self.app.settings
        s['pad_map'] = {}
        for key in self.zone:
            s[key] = 10
            self.zone[key].set(10)
            self.zone_labels[key].configure(text='10 %')
        s['trigger_threshold'] = 12
        self.trigger.set(12)
        self.trigger_label.configure(text='12 %')
        self.save()
        self.show_sources()

    # -- controllers -----------------------------------------------------------------------
    def start(self):
        if self.tool is None:
            self.tool = PadTool()
            if not self.tool.process:
                self.status.configure(text=self.t('no_tool'))
        if self.job is None:
            self.poll()

    def stop(self):
        if self.job:
            self.page.after_cancel(self.job)
            self.job = None
        if self.tool:
            self.tool.stop()
            self.tool = None

    def shown_id(self):
        guid = self.app.settings.get('pad_guid', 'auto')
        for pid, pad in self.pads.items():
            if pad['guid'] == guid:
                return pid
        return getattr(self, 'last_used', next(iter(self.pads), None))

    def choose(self, _=None):
        index = self.choice.current()
        self.app.settings['pad_guid'] = 'auto' if index <= 0 else self.choice_guids[index]
        self.save()
        self.refresh_list()

    def refresh_list(self):
        guid = self.app.settings.get('pad_guid', 'auto')
        values, self.choice_guids = [self.t('auto')], ['auto']
        for pad in self.pads.values():
            values.append(f"{pad['name']}  —  {KINDS.get(pad['type'], pad['type'])}")
            self.choice_guids.append(pad['guid'])
        if guid != 'auto' and guid not in self.choice_guids:
            values.append(f"{guid[:12]}…  ({self.t('missing')})")
            self.choice_guids.append(guid)
        self.choice.configure(values=values)
        self.choice.current(self.choice_guids.index(guid) if guid in self.choice_guids else 0)
        pid = self.shown_id()
        pad = self.pads.get(pid)
        if pad:
            self.info.configure(text=self.t('detected', kind=KINDS.get(pad['type'], pad['type'])) + '  ·  ' +
                                self.t('motion_yes' if pad['gyro'] else 'motion_no') + '  ·  ' +
                                self.t('rumble_yes' if pad['rumble'] else 'rumble_no'))
            self.rumble_button.state(['!disabled'] if pad['rumble'] else ['disabled'])
        else:
            self.info.configure(text=self.t('none') if self.tool and self.tool.process else '')
            self.rumble_button.state(['disabled'])
        self.show_sources()

    def rumble(self):
        pid = self.shown_id()
        if self.tool and pid is not None:
            self.tool.send(f'rumble {pid}')

    def check_drift(self):
        self.drift = [90, 0.0, 0.0]   # about 1.5 s of samples
        self.status.configure(text=self.t('drift_wait'))
        self.drift_button.state(['disabled'])

    def poll(self):
        self.job = self.page.after(33, self.poll)
        if not self.tool or not self.tool.process:
            return
        changed = False
        while True:
            try:
                line = self.tool.lines.get_nowait()
            except queue.Empty:
                break
            if line.startswith('pads '):
                self.pending, self.expect = {}, int(line.split()[1])
                if self.expect == 0:
                    self.pads, self.pending, changed = {}, None, True
            elif line.startswith('pad ') and self.pending is not None:
                pid, kind, guid, gyro, rumble, name = line[4:].split('\t', 5)
                self.pending[int(pid)] = dict(type=kind, guid=guid, gyro=gyro == '1', rumble=rumble == '1', name=name)
                if len(self.pending) == self.expect:
                    self.pads, self.pending, changed = self.pending, None, True
            elif line.startswith('state '):
                parts = line.split()
                pid = int(parts[1])
                values = (int(parts[2], 16), *map(int, parts[3:9]), *map(float, parts[9:12]))
                previous = self.state.get(pid)
                self.state[pid] = values
                if self.app.settings.get('pad_guid', 'auto') == 'auto' and previous and (
                        values[0] & ~previous[0] or any(abs(v) > 16000 for v in values[1:5])):
                    if getattr(self, 'last_used', None) != pid:
                        self.last_used, changed = pid, True
        if changed:
            self.refresh_list()
        self.draw_state()

    def draw_state(self):
        c = self.canvas
        state = self.state.get(self.shown_id())
        buttons, lx, ly, rx, ry, lt, rt, gx, gy, gz = state or (0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0)
        threshold = self.trigger.get() / 100
        lit = {DRAWING[b] for b in DRAWING if buttons & (1 << b)}
        if lt / 32767 > threshold:
            lit.add('L2Trigger')
        if rt / 32767 > threshold:
            lit.add('R2Trigger')
        for eid, items in self.items.items():
            on = eid in lit
            for item in items:
                if c.type(item) == 'line':
                    c.itemconfig(item, fill=self.ACCENT if on else self.LINE, width=3 if on else 1.6)
                else:
                    c.itemconfig(item, outline=self.ACCENT if on else self.LINE,
                                 fill=self.ACCENT if on and 'Border' not in eid else '')
        for side, (ax, ay), key in (('l', (lx, ly), 'stick_deadzone'), ('r', (rx, ry), 'stick_deadzone_right')):
            x, y, r, dot, zone = self.sticks[side]
            dx, dy = ax / 32767 * r, ay / 32767 * r
            pressed = buttons & (1 << (7 if side == 'l' else 8))
            c.coords(dot, x + dx - 7, y + dy - 7, x + dx + 7, y + dy + 7)
            c.itemconfig(dot, fill=self.TEXT if pressed else self.ACCENT)
            z = self.zone[key].get() / 100 * r
            c.coords(zone, x - z, y - z, x + z, y + z)
        for side, value in (('lt', lt), ('rt', rt)):
            x0, bar, mark = self.triggers[side]
            c.coords(bar, x0, 2, x0 + 60 * value / 32767, 10)
            c.coords(mark, x0 + 60 * threshold, 0, x0 + 60 * threshold, 12)
        pad = self.pads.get(self.shown_id())
        self.gyro.configure(text=f"{self.t('gyro')}: {gx:+.2f} {gy:+.2f} {gz:+.2f}" if pad and pad['gyro'] else '')
        if getattr(self, 'combo_capture', None) and state:
            self.combo_poll(buttons, lt, rt)
        if self.capture and state:
            new = buttons & ~self.capture_mask
            self.capture_mask &= buttons
            for bit in range(len(SDL_BUTTONS)):
                if new & (1 << bit) and bit != 5:   # the PS / Xbox button stays the port's own
                    self.finish_capture(bit)
                    break
        if self.drift and state:
            self.drift[1] = max(self.drift[1], abs(lx) / 32767, abs(ly) / 32767)
            self.drift[2] = max(self.drift[2], abs(rx) / 32767, abs(ry) / 32767)
            self.drift[0] -= 1
            if self.drift[0] <= 0:
                _, dl, dr = self.drift
                self.drift = None
                self.drift_button.state(['!disabled'])
                zones = []
                for key, value in (('stick_deadzone', dl), ('stick_deadzone_right', dr)):
                    zone = min(50, max(5, int(value * 100 * 1.25 + 2.999)))   # a margin over the drift
                    self.zone[key].set(zone)
                    self.zone_labels[key].configure(text=f'{zone} %')
                    self.app.settings[key] = zone
                    zones.append(zone)
                self.save()
                self.status.configure(text=self.t('drift_done', l=round(dl * 100, 1), r=round(dr * 100, 1),
                                                  lz=zones[0], rz=zones[1]))
