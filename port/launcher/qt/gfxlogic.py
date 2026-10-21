# SPDX-License-Identifier: GPL-2.0-or-later
"""Graphics screen logic that does not need Qt: presets, the VRAM budget rule and VRAM detection.

Presets only set switches that already exist (launcher settings and the table in backports_table.py,
which are also the options of the in-game Insert menu); none adds a key.
"""
import glob
import os
import re
import subprocess
from pathlib import Path

import backports_table

# ------------------------------------------------------------------------------------- presets
# Keys a preset sets (anything a preset does not list goes back to the default of the table).
# Only options this port's game really reads (backports_table.MENU, checked against gpu/shim).
PRESET_SETTINGS = ('vsync',)
PRESET_ITEMS = ('menu:fps_limit', 'menu:frames_queued', 'menu:fsr1', 'menu:rcas', 'menu:rcas_strength',
                'menu:smaa', 'menu:cas')
# Imported files (preset_share.py) are applied as "full" presets: they may also set these.
FULL_PRESETS = {}
# Defaults of the launcher settings a preset may set (tlg_run.DEFAULT_SETTINGS; a test keeps them equal).
SETTING_DEFAULTS = {'vsync': True, 'gpu_readbacks': '2', 'show_fps': False, 'frame_rate': '30'}
# id, settings overrides, item overrides (values as strings; unlisted = default)
PRESETS = (
    ('performance', {'vsync': False}, {'menu:frames_queued': '2', 'menu:fsr1': '1'}),
    ('balanced', {}, {}),
    ('quality', {}, {'menu:fsr1': '1', 'menu:smaa': '1'}),
    ('ps4', {}, {'menu:fps_limit': '30', 'menu:fsr1': '0', 'menu:rcas': '0'}),
)
DEFAULT_PRESET = 'balanced'


def item_by_id(item_id):
    return next(i for i in backports_table.ITEMS if i.id == item_id)


def _full_targets(overrides, items):
    """Targets of a full preset: the plain keys plus every key the preset lists."""
    settings = {'vsync': overrides.get('vsync', True)}
    stored = {item_id: backports_table.normalize(item_by_id(item_id), items.get(item_id))
              for item_id in PRESET_ITEMS}
    settings.update(overrides)
    known = {i.id for i in backports_table.ITEMS}
    for item_id, value in items.items():
        if item_id in known:
            stored[item_id] = backports_table.normalize(item_by_id(item_id), value)
    return settings, stored


def preset_targets(preset_id):
    """({settings key: value}, {item id: stored value or None}) for a preset: every key a preset
    can set, so applying one also undoes what another set."""
    _, overrides, items = next(p for p in PRESETS if p[0] == preset_id)
    if preset_id in FULL_PRESETS:
        return _full_targets(overrides, items)
    stored = {item_id: backports_table.normalize(item_by_id(item_id), items.get(item_id))
              for item_id in PRESET_ITEMS}
    return {'vsync': overrides.get('vsync', True)}, stored


def custom_targets(preset):
    """The targets of a preset that is not in PRESETS (an imported file, see preset_share.py): it is
    applied as a full preset."""
    return _full_targets(preset.get('settings', {}), preset.get('items', {}))


def apply_targets(settings, states, targets):
    """Puts the targets of a preset into the launcher settings and the fix states (in place); the
    caller saves them (Backend._save_fixes)."""
    settings.update(targets[0])
    states.update(targets[1])


def same_setting(key, current, wanted):
    return _same_setting(key, current, wanted)


def _same_setting(key, current, wanted):
    default = SETTING_DEFAULTS.get(key, True)
    value = default if current is None else current
    return str(value).lower() == str(wanted).lower()


def matching_preset(settings, states):
    """The id of the preset the current switches equal, or None (custom). `states` = fix states."""
    for preset_id, _, _ in PRESETS:
        want_settings, want_items = preset_targets(preset_id)
        if all(_same_setting(k, settings.get(k), v) for k, v in want_settings.items()) and \
           all(states.get(i) == v for i, v in want_items.items()):
            return preset_id
    return None


# --------------------------------------------------------------------------------- VRAM budget
# The Tk launcher's rule ("6 GB card: 5600 · 8 GB: 7000 · 12 GB: 10500 · 16 GB: 14000 · 24 GB: 21500"),
# in MB of VRAM to give the texture cache; in between the budget follows the line joining the points.
BUDGET_POINTS = ((6, 5600), (8, 7000), (12, 10500), (16, 14000), (24, 21500))
BUDGET_MIN, BUDGET_MAX = 2000, 24000      # the Tk spin box range


def suggested_budget_mb(vram_gb):
    """MB of texture-cache budget for a card with this much VRAM (rounded to 100)."""
    gb = float(vram_gb)
    points = BUDGET_POINTS
    if gb <= points[0][0]:
        value = points[0][1] * gb / points[0][0]
    elif gb >= points[-1][0]:
        value = points[-1][1] + (gb - points[-1][0]) * 900
    else:
        value = points[-1][1]
        for (g0, b0), (g1, b1) in zip(points, points[1:]):
            if g0 <= gb <= g1:
                value = b0 + (b1 - b0) * (gb - g0) / (g1 - g0)
                break
    value = min(value, gb * 1024 * 0.95)   # never the whole card
    return int(min(max(round(value / 100) * 100, BUDGET_MIN), BUDGET_MAX))


def budget_is_too_high(budget_mb, vram_mb):
    """True when the budget leaves less than ~5% of the VRAM to the rest of the game."""
    return bool(vram_mb) and budget_mb > vram_mb * 0.95


# --------------------------------------------------------------------------- VRAM detection
def parse_nvidia_smi(text):
    """MB of the largest card in `nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits`."""
    sizes = [int(m) for m in re.findall(r'^\s*(\d+)\s*$', text or '', re.M)]
    return max(sizes) if sizes else None


def parse_vulkan_heaps(text, device=None):
    """MB of the biggest DEVICE_LOCAL heap in `vulkaninfo` output, of the device whose name
    contains `device` (else of the largest device)."""
    best, name, size, local, heap = {}, '', None, False, False

    def close():
        if heap and size and local:
            best[name] = max(best.get(name, 0), size)
    for line in (text or '').splitlines():
        m = re.match(r'\s*deviceName\s*=\s*(.+?)\s*$', line)
        if m:
            close()
            size, local, heap, name = None, False, False, m.group(1)
            continue
        if re.match(r'\s*memoryHeaps\[\d+\]:', line):
            close()
            size, local, heap = None, False, True
            continue
        if not heap:
            continue
        m = re.match(r'\s*size\s*=\s*(\d+)', line)
        if m and size is None:
            size = int(m.group(1))
        elif 'MEMORY_HEAP_DEVICE_LOCAL_BIT' in line:
            local = True
        elif re.match(r'\s*memoryTypes\[', line) or re.match(r'\s*memoryHeapCount', line):
            close()
            size, local, heap = None, False, False
    close()
    if device:
        for found, value in best.items():
            if device.lower() in found.lower() or found.lower() in device.lower():
                return value // (1024 * 1024)
    return max(best.values()) // (1024 * 1024) if best else None


def read_sysfs_vram(pattern='/sys/class/drm/card*/device/mem_info_vram_total'):
    """MB of the largest amdgpu card (Linux, no tools needed)."""
    sizes = []
    for path in glob.glob(pattern):
        try:
            sizes.append(int(Path(path).read_text().strip()))
        except (OSError, ValueError):
            pass
    return max(sizes) // (1024 * 1024) if sizes else None


def detect_vram_mb(gpu_name=None, run=subprocess.run, sysfs=read_sysfs_vram, timeout=15):
    """VRAM in MB: nvidia-smi, then the amdgpu sysfs file, then vulkaninfo's memory heaps.
    None when none of them answers. Slow (a process): call it off the UI thread."""
    flags = {'stdin': subprocess.DEVNULL, 'capture_output': True, 'text': True, 'timeout': timeout,
             'creationflags': getattr(subprocess, 'CREATE_NO_WINDOW', 0)}
    try:
        value = parse_nvidia_smi(run(['nvidia-smi', '--query-gpu=memory.total', '--format=csv,noheader,nounits'],
                                     **flags).stdout)
        if value:
            return value
    except (OSError, subprocess.SubprocessError):
        pass
    value = sysfs()
    if value:
        return value
    try:
        env = dict(os.environ)
        try:
            import tlg_vulkan
            tlg_vulkan.configure(env)
        except Exception:
            pass
        return parse_vulkan_heaps(run([os.environ.get('TLG_VULKANINFO', 'vulkaninfo')], env=env, **flags).stdout,
                                  gpu_name)
    except (OSError, subprocess.SubprocessError):
        return None
