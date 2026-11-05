# SPDX-License-Identifier: GPL-2.0-or-later
# tlg: draws the Fun move bar's icons (our own art, simple shapes) and writes them as one PNG atlas
# embedded in gpu/shim/tlg_fun_icons.h. Order = the working moves in TlgFun::Actions (tlg_fun.h),
# before the "May not work" heading. Run: python -I make_icons.py [preview.png]  (needs Pillow)
import io
import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 96  # icon size in the atlas
K = 4   # supersampling
W = S * K
COLS = 9

GROUPS = {  # background: inner, outer colour
    'sound': ((222, 168, 72), (70, 40, 10)),
    'body': ((70, 190, 180), (10, 45, 50)),
    'play': ((235, 120, 110), (70, 20, 30)),
    'rest': ((120, 130, 235), (20, 22, 70)),
    'tummy': ((150, 200, 80), (25, 50, 10)),
}
INK = (255, 248, 230, 255)
SHADOW = (0, 0, 0, 150)


def bg(group):
    inner, outer = GROUPS[group]
    small = Image.new('RGBA', (S, S))
    px = small.load()
    for y in range(S):
        for x in range(S):
            d = min(1.0, math.hypot(x - S * 0.42, y - S * 0.38) / (S * 0.75))
            px[x, y] = tuple(int(inner[i] * (1 - d) + outer[i] * d) for i in range(3)) + (255,)
    return small.resize((W, W), Image.BILINEAR)


class Pen:
    """Light ink on a transparent layer; finish() adds a soft shadow under it."""

    def __init__(self):
        self.ink = Image.new('RGBA', (W, W), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.ink)

    @staticmethod
    def p(v):  # icon units 0..96 -> pixels
        return v * K

    def line(self, pts, w=6):
        pts = [(self.p(x), self.p(y)) for x, y in pts]
        self.d.line(pts, fill=INK, width=int(w * K), joint='curve')
        r = w * K / 2
        for x, y in (pts[0], pts[-1]):
            self.d.ellipse((x - r, y - r, x + r, y + r), fill=INK)

    def circle(self, x, y, r, fill=True, w=6):
        self.ellipse(x - r, y - r, x + r, y + r, fill, w)

    def ellipse(self, x0, y0, x1, y1, fill=True, w=6):
        box = (self.p(x0), self.p(y0), self.p(x1), self.p(y1))
        if fill:
            self.d.ellipse(box, fill=INK)
        else:
            self.d.ellipse(box, outline=INK, width=int(w * K))

    def arc(self, x, y, r, a0, a1, w=6):
        box = (self.p(x - r), self.p(y - r), self.p(x + r), self.p(y + r))
        self.d.arc(box, a0, a1, fill=INK, width=int(w * K))

    def poly(self, pts):
        self.d.polygon([(self.p(x), self.p(y)) for x, y in pts], fill=INK)

    def cut(self, x, y, r):  # erase a circle (crescent moon, ear hole)
        self.d.ellipse((self.p(x - r), self.p(y - r), self.p(x + r), self.p(y + r)), fill=(0, 0, 0, 0))

    def arrow(self, x0, y0, x1, y1, w=7, head=13):
        a = math.atan2(y1 - y0, x1 - x0)
        self.line([(x0, y0), (x1 - math.cos(a) * head * 0.8, y1 - math.sin(a) * head * 0.8)], w)
        l, r = a + 2.5, a - 2.5
        self.poly([(x1, y1), (x1 + math.cos(l) * head, y1 + math.sin(l) * head),
                   (x1 + math.cos(r) * head, y1 + math.sin(r) * head)])

    def paw(self, x, y, s=1.0):
        self.ellipse(x - 13 * s, y - 4 * s, x + 13 * s, y + 16 * s)
        for dx, dy in ((-15, -12), (-6, -20), (6, -20), (15, -12)):
            self.circle(x + dx * s, y + dy * s, 5.2 * s)

    def feather(self, x, y, ang, length, w=9):
        a = math.radians(ang)
        ex, ey = x + math.cos(a) * length, y + math.sin(a) * length
        nx, ny = -math.sin(a) * w / 2, math.cos(a) * w / 2
        mx, my = (x + ex) / 2, (y + ey) / 2
        self.poly([(x, y), (mx + nx, my + ny), (ex, ey), (mx - nx, my - ny)])

    def wing(self, x, y, side, spread, n=5, length=34):
        a0, a1 = spread
        for i in range(n):
            ang = a0 + (a1 - a0) * i / (n - 1)
            if side < 0:
                ang = 180 - ang
            self.feather(x, y, ang, length - abs(i - n // 2) * 2)

    def waves(self, x, y, n=3, r0=10, step=8, a0=-40, a1=40, w=5):
        for i in range(n):
            self.arc(x, y, r0 + i * step, a0, a1, w)

    def bar(self, y, x0=16, x1=80):
        self.line([(x0, y), (x1, y)], 6)

    def finish(self, base):
        shadow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
        shadow.paste(Image.new('RGBA', (W, W), SHADOW), (0, 0), self.ink)
        shadow = shadow.filter(ImageFilter.GaussianBlur(3 * K))
        out = base.copy()
        out.alpha_composite(shadow, (K * 2, K * 3))
        out.alpha_composite(self.ink)
        return out


def moon(p):
    p.circle(30, 30, 16)
    p.cut(38, 24, 14)


def draw(name, p):
    if name == 'A_sneeze':
        p.circle(30, 54, 14); p.circle(44, 46, 12); p.circle(40, 62, 10)
        for x, y, r in ((64, 34, 4), (74, 48, 5), (66, 58, 3.5), (80, 30, 3), (82, 62, 4)):
            p.circle(x, y, r)
    elif name == 'A_yawn':
        p.ellipse(28, 26, 68, 78, fill=False, w=7)
        p.poly([(36, 34), (42, 34), (39, 44)]); p.poly([(54, 34), (60, 34), (57, 44)])
        p.line([(74, 20), (86, 20), (74, 32), (86, 32)], 4)
    elif name == 'roar':
        p.poly([(14, 44), (44, 30), (40, 48), (44, 66), (14, 52)])
        p.waves(46, 48, 3, 14, 10, -45, 45, 7)
    elif name == 'howling':
        moon(p); p.waves(52, 80, 3, 12, 10, -100, -30, 6)
    elif name == 'howling_calm':
        moon(p); p.waves(52, 80, 2, 12, 9, -95, -35, 3.5)
    elif name == 'flap_wings':
        p.wing(46, 60, 1, (-100, -20)); p.wing(50, 60, -1, (-100, -20))
        p.line([(22, 76), (32, 82)], 4); p.line([(74, 76), (64, 82)], 4)
    elif name == 'A_wing_stretchA':
        p.wing(46, 50, 1, (-40, 30), 6, 38); p.wing(50, 50, -1, (-40, 30), 6, 38)
    elif name == 'A_wing_stretchB':
        p.wing(48, 52, 1, (-70, 0), 5, 34); p.wing(48, 52, -1, (-70, 0), 5, 34)
        p.arrow(48, 66, 48, 88, 5, 9)
    elif name == 'stretch_front':
        p.paw(24, 60, 0.75); p.arrow(40, 52, 86, 52)
    elif name == 'stretch_back':
        p.paw(72, 60, 0.75); p.arrow(56, 52, 10, 52)
    elif name == 'stretch_up':
        p.paw(48, 76, 0.75); p.arrow(48, 58, 48, 10)
    elif name == 'stand_shake_high':
        p.ellipse(34, 30, 62, 70)
        for s in (-1, 1):
            p.line([(48 + s * 22, 30), (48 + s * 28, 40), (48 + s * 22, 50), (48 + s * 28, 60),
                    (48 + s * 22, 70)], 4)
        for x, y in ((14, 24), (82, 26), (16, 76), (80, 74)):
            p.circle(x, y, 3.5)
    elif name in ('scratch_left', 'scratch_right'):
        s = -1 if name == 'scratch_left' else 1
        for i in (-1, 0, 1):
            x = 48 + i * 16
            p.line([(x - s * 12, 22), (x + s * 12, 74)], 6)
    elif name == 'kneading':
        p.paw(30, 58, 0.6); p.paw(66, 46, 0.6)
        p.arrow(30, 84, 30, 72, 4, 7); p.arrow(66, 12, 66, 24, 4, 7)
    elif name == 'P_eye_wink_left':
        p.arc(28, 36, 16, 20, 160, 6)
        p.ellipse(52, 28, 84, 60); p.cut(68, 44, 7); p.circle(68, 44, 4)
    elif name == 'P_ear_shake_left':
        p.poly([(34, 80), (48, 14), (62, 80)]); p.cut(48, 62, 8)
        p.arc(48, 44, 30, 200, 240, 4); p.arc(48, 44, 30, 300, 340, 4)
        p.arc(48, 44, 38, 205, 235, 3); p.arc(48, 44, 38, 305, 335, 3)
    elif name == 'roll_on_ground':
        p.arc(48, 46, 26, 40, 330, 7); p.arrow(70, 30, 74, 50, 6, 12); p.bar(84, 18, 78)
    elif name == 'sit_plays':
        p.circle(64, 34, 14, fill=False, w=5); p.line([(52, 30), (76, 38)], 3); p.paw(34, 66, 0.7)
    elif name == 'toilet':
        p.ellipse(28, 62, 68, 80); p.ellipse(34, 48, 62, 66); p.ellipse(40, 36, 56, 52)
        for x in (22, 74):
            p.line([(x, 40), (x - 4, 30), (x, 20)], 3.5)
    elif name == 'tlg_gag':  # a queasy mouth and a swirl
        p.line([(20, 66), (32, 58), (44, 66), (56, 58), (68, 66), (80, 58)], 6)
        p.arc(48, 32, 16, 0, 300, 5); p.arc(48, 32, 7, 60, 360, 4)
    elif name == 'tlg_throw_up':  # an open beak with drops falling out
        p.line([(16, 22), (56, 30), (16, 42)], 7)
        for x, y, r in ((50, 50, 6), (62, 60, 7), (52, 72, 5), (68, 78, 6), (40, 84, 4)):
            p.circle(x, y, r)
    elif name == 'sit':
        p.arrow(48, 12, 48, 62); p.bar(76)
    elif name == 'sit_shake':
        p.arrow(48, 12, 48, 58); p.bar(76)
        for s in (-1, 1):
            p.line([(48 + s * 18, 30), (48 + s * 24, 38), (48 + s * 18, 46)], 4)
    elif name == 'sit_end':
        p.bar(80); p.arrow(48, 70, 48, 14)
    elif name == 'lie_down':
        p.arrow(48, 14, 48, 50); p.line([(14, 68), (82, 68)], 9); p.bar(82)
    elif name == 'lie_down_relax':
        p.line([(14, 68), (82, 68)], 9); p.bar(82)
        p.line([(18, 30), (26, 24), (34, 30), (42, 24), (50, 30)], 4)
        p.line([(46, 46), (54, 40), (62, 46), (70, 40), (78, 46)], 4)
    elif name == 'rise':
        p.line([(14, 78), (82, 78)], 9); p.arrow(34, 66, 34, 16, 6, 12); p.arrow(62, 66, 62, 16, 6, 12)
    elif name == 'stand':
        p.ellipse(18, 30, 78, 56)
        for x in (26, 38, 58, 70):
            p.line([(x, 50), (x, 80)], 6)
        p.circle(80, 30, 9)
    else:
        raise SystemExit('no icon for ' + name)


def works(header):
    """The working moves (command, group) from tlg_fun.h, in menu order."""
    out, group = [], None
    names = {'Sounds and moods': 'sound', 'Body': 'body', 'Play': 'play', 'Tummy': 'tummy', 'Rest': 'rest'}
    text = open(header, encoding='utf-8').read()
    text = text[text.index('Actions[] = {'):]
    for m in re.finditer(r'\{(nullptr|"[^"]*"), "([^"]*)"', text):
        cmd, name = m.group(1), m.group(2)
        if cmd == 'nullptr':
            if name not in names:
                break
            group = names[name]
            continue
        out.append((cmd.strip('"'), group))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    shim = os.path.join(here, '..', '..', 'gpu', 'shim')
    moves = works(os.path.join(shim, 'tlg_fun.h'))
    rows = (len(moves) + COLS - 1) // COLS
    atlas = Image.new('RGBA', (COLS * S, rows * S), (0, 0, 0, 0))
    for i, (cmd, group) in enumerate(moves):
        p = Pen()
        draw(cmd, p)
        icon = p.finish(bg(group)).resize((S, S), Image.LANCZOS)
        atlas.paste(icon, ((i % COLS) * S, (i // COLS) * S))
    if len(sys.argv) > 1:
        atlas.save(sys.argv[1])
    buf = io.BytesIO()
    atlas.save(buf, 'PNG', optimize=True)
    data = buf.getvalue()
    lines = ['// SPDX-License-Identifier: GPL-2.0-or-later',
             '// tlg: generated by tools/fun-icons/make_icons.py (our own art), do not edit.',
             '#pragma once', '', 'namespace TlgFunIcons {',
             f'inline constexpr int Size = {S}, Columns = {COLS}, Count = {len(moves)};',
             f'inline constexpr unsigned char Png[{len(data)}] = {{']
    for i in range(0, len(data), 24):
        lines.append('    ' + ','.join(str(b) for b in data[i:i + 24]) + ',')
    lines += ['};', '} // namespace TlgFunIcons', '']
    with open(os.path.join(shim, 'tlg_fun_icons.h'), 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    print(len(moves), 'icons,', len(data), 'bytes')


main()
