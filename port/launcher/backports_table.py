# SPDX-License-Identifier: GPL-2.0-or-later
"""Every switch the Qt launcher's Graphics and Fixes screens offer for The Last Guardian.

The table layout and helpers come from infamous_pc's backports_table.py (same Qt screens), but
every row here was checked against THIS port's sources (tests/test_backports_table.py keeps it so):
  - MENU: tlg.ini keys parsed by gpu/shim/tlg_settings.cpp (Set) and shown by the in-game menu
    (gpu/shim/tlg_overlay.cpp); ranges and defaults from tlg_settings.h / tlg_graphics.h /
    tlg_perf_stats.h. They take effect on the next frame.
  - DIAGNOSTICS: TLG_* environment switches the GPU code reads at start (texture_cache.h,
    vk_presenter.cpp, resource.h, emulator_settings.h). They are kept in settings.json and passed
    as TLG_<name>=0/1 only when the player changed them.
  - READBACK: the same kind of switches for GPU readbacks (Tk launcher: Advanced settings), never
    part of a graphics preset.
There are no BPnn backports in this port (the GPU code has no TLG_BPnn switches), and no
Advanced parameters (advanced_table.py is empty), so those screens stay hidden.
"""
from collections import namedtuple
from pathlib import Path

import advanced_table

Item = namedtuple('Item', 'id group key env vtype default choices lo hi unit shader live store '
                          'pass_env risky broken name what source tab')


def _make(id, group, key, env, vtype, default, name, what, *, choices=(), lo=0, hi=0, unit='',
          shader=False, live=False, store='ini', pass_env=False, risky=False, broken=False,
          source='', tab=''):
    return Item(id, group, key, env, vtype, default, choices, lo, hi, unit, shader, live, store,
                pass_env, risky, broken, name, what, source, tab)


def _ab(key, env, default, live, name, what, store='launcher', pass_env=True, risky=False, broken=False):
    return _make(env, 'ab', key, env, 'bool', None if default is None else '1' if default else '0',
                 name, what, live=live, store=store, pass_env=pass_env, risky=risky, broken=broken)


def _menu(tab, key, vtype, default, name, what, **kw):
    return _make('menu:' + key, 'menu', key, '', vtype, default, name, what, live=True, tab=tab, **kw)


# The in-game menu (Insert). Display mode, VSync and the FPS overlay's on/off switch are launcher
# settings (settings.json -> tlg.ini at every start) and are not repeated here.
_CORNERS = (('0', 'Top left'), ('1', 'Top right'),
            ('2', 'Bottom left'), ('3', 'Bottom right'))
MENU = [
    # One Sharpening setting (2026-11-07): the AA step's own RCAS with DLSS / FSR / TAA, AMD CAS on
    # the final picture with Built-in. The old rcas / cas keys are still read but no longer used.
    _menu('Image', 'sharpen', 'bool', '1', 'Sharpening',
          'Sharpens the image: with DLSS, FSR or TAA their own sharpening (scene only), with Built-in AMD CAS.'),
    _menu('Image', 'sharpness', 'float', '1.75', 'Sharpening strength',
          '1.5 is the default; above 1 an extra pass with DLSS / FSR / TAA (CAS stops at 1).',
          lo=0.0, hi=4.0),
    _menu('Image', 'smaa', 'bool', '0', 'SMAA 1x anti-aliasing',
          'Smooths jagged edges on the final image (a little GPU time).'),
    _menu('Image', 'upscaler', 'choice', 'dlss', 'Upscaler',
          ('Draws the image at the chosen quality: DLSS (NVIDIA RTX), FSR 4 (any recent card), FSR 3.1 (any card), TAA (any card, lighter, native size only) '
           "or the game's own pass (Built-in). At Native AA quality it is the anti-aliasing."),
          choices=(('off', 'Built-in (game)'), ('dlss', 'DLSS'),
                   ('fsr3', 'FSR 3.1'), ('fsr4', 'FSR 4'), ('taa', 'TAA'))),
    _menu('Image', 'preset', 'choice', '0', 'Quality (requires restart)',
          ('The render size: Quality 1/1.5, Balanced 1/1.7, Performance 1/2, Ultra Performance 1/3 of the output '
           'on each side, upscaled by the upscaler. Applied at start.'),
          choices=(('0', 'Native AA'), ('1', 'Quality'), ('2', 'Balanced'),
                   ('3', 'Performance'), ('4', 'Ultra Performance'))),
    _menu('Image', 'frame_generation', 'choice', '0', 'Frame generation',
          ('Shows one made-up image between two game frames: smoother motion, a little more input delay. '
           'DLSS needs NVIDIA RTX 40 or newer; FSR is not tied to one brand.'),
          choices=(('0', 'Off'), ('1', 'DLSS'), ('2', 'FSR 3.1'))),
    _menu('Image', 'hdr', 'bool', '1', 'HDR',
          "The game's own HDR10 output, used when HDR is on in Windows display settings. Recommended: on."),
    _menu('Effects', 'shadow_scale', 'choice', '1', 'Shadow resolution',
          ("Draws the shadow maps at 2x or 4x the game's 2048x2048 (default: Game). The main gain is much less shimmer of "
           'tree and leaf shadows, with any anti-aliasing. '
           '8192 costs about 8 FPS and up to about 2 GB of video memory (4096 about a quarter of that). With 8 GB of video memory or less keep Game: running out causes stutter and crashes.'),
          choices=(('1', 'Game (2048)'), ('2', '4096'), ('4', '8192'))),
    _menu('Effects', 'ssao', 'int', '100', 'Ambient occlusion',
          ('Strength of the contact shadows in corners and under grass. 0% turns the pass off (saves a little GPU '
           'time). Above 150% the half-resolution AO shows blocky halos.'), lo=0, hi=400, unit='%'),
    _menu('Effects', 'bloom', 'int', '100', 'Bloom',
          'Glow around bright light. 0% turns the pass off.',
          lo=0, hi=400, unit='%'),
    _menu('Effects', 'ssr', 'int', '100', 'Screen-space reflections',
          'Reflections and light glare on water and wet stone. 0% turns the pass off.', lo=0, hi=400, unit='%'),
    _menu('Effects', 'motion_blur', 'int', '100', 'Motion blur',
          'Camera motion blur. 0% turns the pass off.',
          lo=0, hi=400, unit='%'),
    _menu('Effects', 'vignette_add', 'int', '0', 'Vignette',
          'Darkened screen corners. The game ships with this off: 0% is as shipped and recommended.', lo=0, hi=100, unit='%'),
    _menu('Effects', 'game_sharpen', 'int', '100', 'Game sharpening',
          "The game's own sharpening filter. On by default: 100% is the original and the maximum (recommended).",
          lo=0, hi=100, unit='%'),
    _menu('Effects', 'fov', 'int', '70', 'Field of view',
          'Camera field of view. Default 70° is as shipped; higher shows more around the boy.',
          lo=50, hi=110, unit='°'),
    _menu('Effects', 'trico_feathers', 'int', '100', 'Trico feather wind',
          "How strongly the wind moves Trico's feathers. Default 100% is as shipped; 0% keeps them still.",
          lo=0, hi=400, unit='%'),
    _menu('Effects', 'fog_distance', 'int', '100', 'Fog distance',
          'Where the fog starts and reaches full strength. Default 100% is as shipped; lower brings the fog closer.',
          lo=25, hi=400, unit='%'),
    _menu('Effects', 'haze_fringe', 'int', '0', 'Haze color fringe',
          ("The game's chromatic aberration on the light haze: yellow and blue edges around objects against fog, "
           'strongest toward the screen edges. Sharp anti-aliasing (DLAA, DLSS) makes it stand out more. '
           'Default 0% gives clean edges; 100% is as shipped.'),
          lo=0, hi=100, unit='%'),
    _menu('Effects', 'grass_smooth', 'bool', '1', 'Grass animated every frame',
          'The game moves most grass only every 4th frame. On (default) moves it every frame; off is as shipped.'),
    _menu('Effects', 'boy_wind', 'int', '100', 'Boy wind (hair + clothing)',
          "How strongly the wind moves the boy's hair and clothing. Default 100% is as shipped; 50% is recommended, 30% is calm.",
          lo=0, hi=100, unit='%'),
    _menu('Effects', 'tree_wind', 'int', '100', 'Tree wind',
          'How strongly the wind moves trees, leaves and ivy. Default 100% is as shipped; 50% looks more natural (recommended).',
          lo=0, hi=200, unit='%'),
    _menu('Color', 'color_adjust', 'bool', '1', 'Color adjustments',
          'Turns on the brightness, contrast, saturation and gamma sliders below (SDR only). Default on; off is the game as shipped.'),
    _menu('Color', 'color_brightness', 'float', '0', 'Brightness',
          'Added to the final image: 0 is the game\'s own.',
          lo=-0.5, hi=0.5),
    _menu('Color', 'color_contrast', 'float', '1', 'Contrast',
          '1 is the game\'s own.', lo=0.5, hi=1.5),
    _menu('Color', 'color_saturation', 'float', '1', 'Saturation',
          '1 is the game\'s own, 0 is black and white.', lo=0.0, hi=2.0),
    _menu('Color', 'color_gamma', 'float', '1', 'Gamma',
          'Default 1, the game\'s own; 0.75 tames the over-lit SDR image outdoors.', lo=0.5, hi=2.0),
    _menu('Performance', 'fps_limit', 'choice', '0', 'Frame rate limit',
          'Caps the presented frames (never above the game frame rate chosen above).',
          choices=(('30', '30'), ('60', '60'), ('120', '120'), ('240', '240'),
                   ('0', 'Unlimited'))),
    _menu('Performance', 'credits_fps', 'choice', '30', 'End credits frame rate',
          ('The end credits run at this rate whatever the limit above says. 30 is advised: the PS4 runs them '
           'at 30, and above 30 they hitch at every cut.'),
          choices=(('30', '30 (advised)'), ('60', '60'),
                   ('0', 'Unlimited'))),
    _menu('Performance', 'slow_motion', 'choice', '0', 'Slow-motion freeze fix (60 FPS / unlocked)',
          ('Fixes a known bug of the game at 60 FPS and unlocked: two slow-motion moments turn choppy and '
           'can freeze the game (the boy misses the catch and nothing happens). It is the same bug that made '
           'the PS4 60 FPS patches freeze here. The moments: the jump to Trico\'s tail (s06_FallingBridge, '
           'mission FB_e02_Demo_003, cutscene fb_110; issues #39, #40) and the collapsing bridge (s10_Pit, '
           'mission s10_06_Destroyed_Bridge_Gate_001, cutscene pi_160_p04_A). Advised: drop to 30 FPS '
           'only while the slow motion plays, then back. Or skip the slow motion. At 30 FPS nothing changes.'),
          choices=(('0', '30 FPS during slow motion (advised)'),
                   ('1', 'Skip the slow motion'))),
    # Hack and fun (English only): the same text as the in-game Fun tab (tlg_fun.h DroolHelp).
    _menu('Fun', 'trico_drool', 'choice', '0', "Trico's drool",
          "A feature the developers built and then switched off just before release: Trico drools. Always: a string of drool hangs from his beak the whole time. As the developers planned: it slowly grows while Trico smells something he wants, a smell-pot cart (s04_FirstAntenna and s10_Pit), and fades away when he loses interest (the boy's smell bottle was cut, so the carts are the only trigger). Stays on through jumps, new stages and extra Tricos, and is kept for the next start.",
          choices=(('0', 'Off (as released)'), ('1', 'Always'),
                   ('2', 'As the developers planned'))),
    _menu('Performance', 'frames_queued', 'choice', '1', 'Frames queued',
          'More frames queued: steadier FPS, slower response to input.',
          choices=(('1', '1 (lowest latency)'), ('2', '2'),
                   ('0', 'Unlimited'))),
    _menu('Performance', 'readback_batch', 'choice', '0', 'GPU readback batch',
          ('When the game reads GPU-written memory, also copy back the other GPU-written data around it. '
           'Off is fastest (2-5 FPS more in tests); 64 MB is not recommended.'),
          choices=(('0', 'Off (recommended)'), ('16', '16 MB'),
                   ('64', '64 MB (not recommended)'))),
    _menu('Performance', 'texture_cleanup', 'bool', '0',
          'Texture cleanup after loads',
          ('After each stage or checkpoint load, frees the textures of earlier stages that the new one does not '
           'use. Off by default for now: it helps when textures pile up, but the game still gets slower over '
           'many loads (issue #20). Off is the emulator\'s original behaviour.')),
    _menu('Performance', 'keep_gpu_textures', 'bool', '1',
          'Keep GPU-only textures',
          'Keeps a texture the GPU drew into while its only correct copy is on the GPU (a game write right next to it blocks the copy back), until video memory is almost full. Without it, cleanup can throw that picture away and the game gets old data back. From SpartanPort (@maze1377). On by default; turn it off if video memory runs full more often.'),
    _menu('Performance', 'arena_release', 'bool', '1',
          'Debug: give back freed buffer memory',
          'Since v0.5.2: when the game frees memory, its graphics card copy goes too. Test switch (issue #38): turn it off to check whether a problem started with v0.5.2. Off is the behaviour before v0.5.2. From the Shadow of the Colossus PC port (@AjncSystem, @Oussama268). Also in the in-game menu (Insert → Performance).'),
    _menu('Performance', 'gc_live_budget', 'bool', '1',
          'Debug: texture cleanup follows the driver',
          'Since v0.5.2: texture cleanup starts at the video memory the driver offers now, never below the start-up limits. Test switch (issue #38): off uses only the start-up limits (the behaviour before v0.5.2), so textures are cleaned up sooner. Also in the in-game menu (Insert → Performance).'),
    _menu('Overlay', 'overlay_fps', 'bool', '1', 'Overlay: FPS',
          'Shows the frame rate in the performance overlay.'),
    _menu('Overlay', 'overlay_frametime', 'bool', '1', 'Overlay: frame time',
          'Shows the time of each frame.'),
    _menu('Overlay', 'overlay_upscaler', 'bool', '0', 'Overlay: upscaler',
          'Shows the active upscaler.'),
    _menu('Overlay', 'overlay_gpu', 'bool', '1', 'Overlay: GPU',
          'Shows the GPU name and use.'),
    _menu('Overlay', 'overlay_cpu', 'bool', '0', 'Overlay: CPU',
          'Shows the game\'s CPU use.'),
    _menu('Overlay', 'overlay_ram', 'bool', '0', 'Overlay: RAM',
          'Shows RAM used by the game and total.'),
    _menu('Overlay', 'overlay_vram', 'bool', '1', 'Overlay: VRAM',
          'Shows video memory used and available.'),
    _menu('Overlay', 'overlay_scale', 'int', '100', 'Overlay font size',
          'Size of the overlay text.', lo=50, hi=300, unit='%'),
    _menu('Overlay', 'overlay_corner', 'choice', '1', 'Overlay corner',
          'Where the overlay sits.', choices=_CORNERS),
    _menu('Overlay', 'overlay_opacity', 'int', '50', 'Overlay background opacity',
          '0% is no background.', lo=0, hi=100, unit='%'),
    _menu('Overlay', 'overlay_layout', 'choice', '0', 'Overlay layout',
          'Items side by side or stacked.',
          choices=(('0', 'One line'), ('1', 'One item per line'))),
]

BACKPORTS = []   # no TLG_BPnn switch exists in this port's GPU code

# Start-up switches (environment, read once when the game starts). Defaults: the value the game
# gets from this port (tlg_run.DEFAULT_ENV or the code's own fallback).
DIAGNOSTICS = [
    _ab('direct_memory_access', 'TLG_DIRECT_MEMORY_ACCESS', False, False,
        'Direct memory access (shaders)',
        ('Shaders read constants straight from the game\'s memory when their address is computed: '
         'fixes some broken scenes, but the game runs slower. Its shaders are compiled again, in a cache '
         'of their own.'), risky=True),
    _ab('background_warmup', 'TLG_BACKGROUND_WARMUP', True, False,
        'Load pipelines while playing',
        'The cached pipelines load in the background (a corner note shows progress); off blocks until all are loaded.'),
    _ab('input_timing', 'TLG_INPUT_TIMING', True, False,
        'Button hold timing at any frame rate',
        ('The game counts how long a button is held in frames, made for 30 FPS: at 60 FPS a tap counts '
         'as a long press twice as fast (cancel, letting go, calling Trico). On keeps the 30 FPS timing.')),
    _ab('block1d', 'TLG_BLOCK1D', False, False, 'BC 1D textures as 2D',
        'Block-compressed 1D textures as 2D images, for drivers that refuse BC 1D.'),
    _ab('pipeline_binaries', 'TLG_PIPELINE_BINARIES', False, False,
        'Pipeline binaries (VK_KHR_pipeline_binary)',
        'Crashes the game on the NVIDIA 595 driver; off by default in this port.', broken=True),
]

# GPU readback start-up switches: the Tk launcher's Advanced settings dialog. Never part of a
# graphics preset (NOT_IN_PRESETS): they trade correctness for speed, not looks.
READBACK = [
    _ab('deferred_readback', 'TLG_DEFERRED_READBACK', True, False,
        'Deferred GPU readbacks',
        'Only the thread that reads GPU results waits, not the whole GPU.'),
    _ab('rb_hot', 'TLG_RB_HOT', True, False,
        'Hot readbacks',
        'Small regions the game reads back again and again stay readable instead of waiting each time.'),
    _ab('fast_read_fault', 'TLG_FAST_READ_FAULT', True, False,
        'Skip readbacks of unchanged memory',
        'A CPU read of memory the GPU did not write does not wait for the GPU thread.'),
    _ab('readback_linear', 'TLG_READBACK_LINEAR', True, False,
        'Read small render targets back',
        'Copies small and linear render targets back to the CPU.'),
    _ab('stale_readback', 'TLG_STALE_READBACK', False, False,
        'Stale readbacks',
        'CPU reads see the last published GPU version instead of waiting (needs deferred readbacks).', risky=True),
    _ab('gc_batch_download', 'TLG_GC_BATCH_DOWNLOAD', False, False,
        'Batch write-backs of evicted textures',
        'When textures are evicted, waits for the GPU once for all of them instead of once per texture.'),
    _ab('async_fences', 'TLG_ASYNC_FENCES', False, False,
        'Asynchronous fences',
        'Signals the game\'s fences once the memory copies before them are done, instead of waiting for them.'),
]
# Why each default is what it is (shown under the row, with the default).
READBACK_WHY = {
    'TLG_DEFERRED_READBACK': ('3-6% more FPS on three test routes (2026-10-26). Turn it off only to check '
                              'whether a broken scene comes from it.'),
    'TLG_RB_HOT': 'Halves the readbacks: 6-13% more FPS in the pulse test, the other routes unchanged (2026-10-25).',
    'TLG_FAST_READ_FAULT': ('Off only adds waits for the GPU thread (about 3 ms per read in inFAMOUS, hundreds '
                            'per second).'),
    'TLG_READBACK_LINEAR': 'The game\'s auto exposure needs it: off makes scenes too dark or too bright (#38, #40).',
    'TLG_STALE_READBACK': ('The game drives gameplay from GPU results: in a test with its GPU queries one frame late '
                           'the boy missed Trico.'),
    'TLG_GC_BATCH_DOWNLOAD': 'Not measured in this game yet.',
    'TLG_ASYNC_FENCES': 'Measured slower (79 against 83 FPS): the game waits on these fences.',
    'menu:readback_batch': 'Off is 2-5 FPS faster than a batch since the fence flushes and the side queue (2026-10-23).',
}

ADVANCED = advanced_table.build(_make)
ITEMS = MENU + BACKPORTS + DIAGNOSTICS + READBACK + ADVANCED
# Rows a graphics preset never sets, exports or imports (they are in the Advanced settings dialog).
NOT_IN_PRESETS = frozenset([item.id for item in READBACK] + ['menu:readback_batch'])
PREFIX = 'TLG_'


def _from_launcher(item, settings):
    value = settings.get(item.key)
    return item.default if value is None else '1' if value else '0'


def read_states(ini, settings=None):
    """{item.id: value text or None}: the player's choice, None = the default applies.
    A value equal to the default is also None, so only real changes are kept."""
    settings = settings or {}
    states = {}
    for item in ITEMS:
        value = _from_launcher(item, settings) if item.store == 'launcher' else ini.get(item.key)
        states[item.id] = normalize(item, value)
    return states


def normalize(item, value):
    """The value if valid and different from the default, else None."""
    if value is None:
        return None
    value = str(value).strip()
    if item.vtype == 'bool':
        valid = value in ('0', '1')
    elif item.vtype == 'choice':
        valid = value in [c[0] for c in item.choices]
    elif item.vtype == 'float':
        try:
            number = float(value)
            valid = item.lo <= number <= item.hi
        except ValueError:
            valid = False
        if valid:
            if abs(number - float(item.default)) <= 1e-6 * max(1.0, abs(number)):
                return None
            value = f'{number:g}'
    else:
        try:
            valid = item.lo <= int(value) <= item.hi
        except ValueError:
            valid = False
        value = str(int(value)) if valid else value
    return value if valid and value != item.default else None


# Slider steps of the 'float' items (launchers).
FLOAT_STEPS = {'sharpness': 0.05, 'color_brightness': 0.05, 'color_contrast': 0.05, 'color_saturation': 0.05, 'color_gamma': 0.05}


def effective(item, states):
    """The value the game will use; None for an automatic switch left on automatic."""
    value = states.get(item.id)
    return item.default if value is None else value


def ini_updates(states):
    """tlg.ini keys to write: the value for a change, None to remove the key (default again)."""
    return {item.key: states.get(item.id) for item in ITEMS if item.store == 'ini'}


def environment(states, environ=None):
    """TLG_<name>=0/1 for the switches the player changed; one already in the environment wins.
    Untouched switches add nothing, so the defaults hold."""
    environ = {} if environ is None else environ
    env = {}
    for item in ITEMS:
        value = states.get(item.id)
        if value is None or not item.pass_env or item.env in environ:
            continue
        env[item.env] = value
    return env


def previous_release(states):
    """Kept for the shared screens: this port has no backports to undo."""
    return dict(states)


def defaults(states, group=None):
    """Forget every choice (of one group, or all)."""
    return {k: (None if group is None or _group(k) == group else v) for k, v in states.items()}


def is_recommended(item):
    """Rows that are part of the Recommended profile: the switches that are on by default."""
    return item.group in ('bp', 'ab') and item.vtype == 'bool' and item.default == '1'


def changes(states):
    """How many start-up switches differ from the Recommended profile. The game menu options
    (overlay corner, limits...) are preferences and do not count."""
    return sum(1 for item in BACKPORTS + DIAGNOSTICS + READBACK if states.get(item.id) is not None)


def advanced_changes(states):
    return sum(1 for item in ADVANCED if states.get(item.id) is not None)


def notice_pending(settings):
    """No one-time notice in this port (inFAMOUS showed one about its backports)."""
    return False


def mark_notice_seen(settings):
    settings['backports_notice_seen'] = True


def _group(item_id):
    return next((item.group for item in ITEMS if item.id == item_id), None)


def write_ini(path, updates):
    """Sets or removes keys in tlg.ini, keeping every other line (the game's own keys included)."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# tlg settings']
    out, seen = [], set()
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            seen.add(key)
            if updates[key] is not None:
                out.append(f'{key}={updates[key]}')
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if v is not None and k not in seen]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')
