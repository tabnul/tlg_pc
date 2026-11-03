"""The controller drawing on the launcher's Controller page.

The DualShock 3 drawing is RPCS3's DualShock_3.svg ("DualShock 3 SVG by HerrHulaHoop", added to
RPCS3 by @Megamouse; GPL-2.0, https://github.com/RPCS3/rpcs3). Thank you both. For this port it
was turned into a DualShock 4: touchpad, Share/Options, PS button between the sticks, L2/R2 behind
the shoulders and no Select/Start; everything else is the original drawing.
"""
import math
import re
import xml.etree.ElementTree as ET

DS4_SVG = r'''<svg xmlns="http://www.w3.org/2000/svg" id="DualShock_3_SVG" width="400" height="250" viewBox="0,  0, 400, 250">
<path id="MainOutline" d="M 116.5 41.6 C 116.9 40.5, 116 35.6, 115 29.9 C 112.7 14.7, 112.5 14.3, 108.9 12 C 108 11.4, 106.4 10.2, 105.4 9.3 C 100.4 4.6, 89 2.3, 77.4 3.5 C 70.7 4.2, 65.6 6.1, 62.2 9.2 C 61 10.2, 59.5 11.4, 58.7 11.9 C 56.8 13, 55 15.1, 54.6 16.8 C 54.2 18.1, 51.6 34, 50 42.4 M 25.4 74.2 C 22 81.1, 18.5 102.3, 10.4 159.5 C 8.1 175.6, 5.6 192.4, 4.8 196.7 C 3 207.4, 2.9 208.8, 3.4 212.7 C 7.2 241.6, 46.2 259.1, 69.4 242.3 C 77.3 236.6, 80.3 231.6, 103.5 186.3 L 110 174.1 M 292 173.5 C 292.5 174.5, 295 179.2, 297.9 184.9 C 308.1 205.1, 320.6 228.7, 323.2 232.6 C 335.6 250.8, 359.9 254.2, 380.5 240.4 C 394.5 231.1, 400.8 217.3, 398.1 202.1 C 396.5 193.4, 394.9 182.8, 390.4 151.1 C 383.3 101.3, 380 81.4, 376.8 75 M 351.9 42.3 C 351.8 40.5, 351 35.6, 350 29.9 C 347.7 14.7, 347.5 14.3, 343.9 11.5 C 343 10, 341.4 9.2, 342.6 9.8 C 335.4 4.6, 324 2.3, 312.4 3.5 C 305.7 4.2, 300.6 6.1, 297.1 9.2 C 296 10.2, 294.5 11.4, 293.7 11.9 C 291.8 13,  290.1 15.1, 289.6 16.8 C 289.2 18.1, 286.6 34, 285 42.4" stroke="black" fill="none" />
<path id="LeftShoulder" d="M 107 10.6 C 94.4 6.7, 74.8 6.5, 60.5 10.6" stroke="black" fill="none" />
<path id="RightShoulder" d="M 343.7 11.2 C 329.4 6.7, 309.8 6.5, 295.5 10.6" stroke="black" fill="none" />
<path id="UpperLine1" d="M 116.5 41.6 H 286" stroke="black" fill="none" />
<line id="LowerLine1" x1="181.5" y1="164" x2="220" y2="164" stroke="black" fill="none" />
<line id="LowerLine2" x1="179" y1="170.5" x2="222.5" y2="170.5" stroke="black" fill="none" />
<circle id="PSButton" cx="201" cy="146" r="8" stroke="black" fill="none" />
<circle id="RightJoystickOuter" cx="258" cy="154" r="39" stroke="black" fill="none" />
<circle id="RightJoystickMiddle" cx="258" cy="154" r="30" stroke="black" fill="none" />
<circle id="RightJoystickInner" cx="258" cy="154" r="27" stroke="black" fill="none" />
<circle id="LeftJoystickOuter" cx="143.4" cy="154" r="39" stroke="black" fill="none" />
<circle id="LeftJoystickMiddle" cx="143.4" cy="154" r="30" stroke="black" fill="none" />
<circle id="LeftJoystickInner" cx="143.4" cy="154" r="27" stroke="black" fill="none" />
<path id="RightFacebuttonCircle" d="M 297.3 152.5 A 61.8 61.8, 0, 1 0, 260 115" stroke="black" fill="none" />
<path id="RightButtonBorder" d="M 301 76 H 278 A 6 6, 0, 0 0, 272 82 V 105 A 6 6, 0, 0 0, 278 111 H 301 V 134 A 6 6, 0, 0 0, 307 140 H 330 A 6 6, 0, 0 0, 336 134 V 111 H 359 A 6 6, 0, 0 0, 365 105 V 82 A 6 6, 0, 0 0, 359 76 H 336 V 53 A 6 6, 0, 0 0, 330 47 H 307 A 6 6, 0, 0 0, 301 53 V 76.5" stroke="black" fill="none" />
<circle id="ButtonBorder1" cx="289.5" cy="93.5" r="13" stroke="black" fill="none" />
<rect id="ButtonSquare" x="282" y="86.5" width="15" height="15" stroke="black" fill="none" />
<circle id="ButtonBorder2" cx="347.5" cy="93.5" r="13" stroke="black" fill="none" />
<circle id="ButtonCircle" cx="347.5" cy="93.5" r="9" stroke="black" fill="none" />
<circle id="ButtonBorder3" cx="318.5" cy="64.5" r="13" stroke="black" fill="none" />
<path id="ButtonTriangle" d="M 310 70 H 327 L 318.5 56 Z" stroke="black" fill="none" />
<circle id="ButtonBorder4" cx="318.5" cy="122.5" r="13" stroke="black" fill="none" />
<path id="ButtonCross" d="M 311.5 115.5 L 325.5 129.5 M 311.5 129.5 L 325.5 115.5" stroke="black" fill="none" />
<path id="LeftDpadCircle" d="M 104.2 152.5 A 61.8 61.8, 0, 1 1, 142 115" stroke="black" fill="none" />
<path id="LeftDpadBorder" d="M 66 76 H 43 A 6 6, 0, 0 0, 37 82 V 105 A 6 6, 0, 0 0, 43 111 H 66 V 134 A 6 6, 0, 0 0, 72 140 H 95 A 6 6, 0, 0 0, 101 134 V 111 H 124 A 6 6, 0, 0 0, 130 105 V 82 A 6 6, 0, 0 0, 124 76 H 101 V 53 A 6 6, 0, 0 0, 95 47 H 72 A 6 6, 0, 0 0, 66 53 V 76.5" stroke="black" fill="none" />
<path id="DpadButtonLeft" d="M 52 85 H 69 L 75 93.5 L 69 102 H 52 Z M 48 88 V 99 L 43 93.5 Z" stroke="black" fill="none" />
<path id="DpadButtonRight" d="M 115 85 H 98 L 92 93.5 L 98 102 H 115 Z M 119 88 V 99 L 124 93.5 Z" stroke="black" fill="none" />
<path id="DpadButtonTop" d="M 75 62 V 79 L 83.5 85 L 92 79 V 62 Z M 78 58 H 89 L 83.5 53 Z" stroke="black" fill="none" />
<path id="DpadButtonBottom" d="M 75 125 V 108 L 83.5 102 L 92 108 V 125 Z M 78 129 H 89 L 83.5 134 Z" stroke="black" fill="none" />
<rect id="Touchpad" x="151" y="45" width="100" height="56" rx="6" stroke="black" fill="none" /><rect id="ShareButton" x="141" y="47" width="5" height="12" rx="2" stroke="black" fill="none" /><rect id="OptionsButton" x="256" y="47" width="5" height="12" rx="2" stroke="black" fill="none" /><path id="L2Trigger" d="M 56 16 C 60 -6, 106 -6, 112 12" stroke="black" fill="none" /><path id="R2Trigger" d="M 290 12 C 296 -6, 342 -6, 346 16" stroke="black" fill="none" /></svg>'''


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2, n=16):
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    phi = math.radians(phi); cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    co = math.sqrt(max(0, num / (rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2)))
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = cp * cxp - sp * cyp + (x1 + x2) / 2, sp * cxp + cp * cyp + (y1 + y2) / 2
    ang = lambda ux, uy, vx, vy: math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    out = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        out.append((cx + rx * math.cos(t) * cp - ry * math.sin(t) * sp, cy + rx * math.cos(t) * sp + ry * math.sin(t) * cp))
    return out


def path_polylines(d):
    toks = re.findall(r'[A-Za-z]|-?\d*\.?\d+(?:e-?\d+)?', d)
    i, cmd, x, y, sx, sy, lines, cur = 0, None, 0.0, 0.0, 0.0, 0.0, [], []
    num = lambda: float(toks[i])
    while i < len(toks):
        if re.match('[A-Za-z]', toks[i]):
            cmd = toks[i]; i += 1
            if cmd in 'Zz':
                if cur: cur.append(cur[0]); lines.append(cur); cur = []
                x, y = sx, sy
                continue
        rel = cmd.islower(); C = cmd.upper()
        def take(k):
            nonlocal i
            v = [float(t) for t in toks[i:i + k]]; i += k; return v
        if C == 'M':
            a, b = take(2); x, y = (x + a, y + b) if rel else (a, b)
            if cur: lines.append(cur)
            cur = [(x, y)]; sx, sy = x, y
            cmd = 'l' if rel else 'L'
        elif C == 'L':
            a, b = take(2); x, y = (x + a, y + b) if rel else (a, b); cur.append((x, y))
        elif C == 'H':
            a, = take(1); x = x + a if rel else a; cur.append((x, y))
        elif C == 'V':
            b, = take(1); y = y + b if rel else b; cur.append((x, y))
        elif C == 'C':
            v = take(6)
            if rel: v = [v[k] + (x if k % 2 == 0 else y) for k in range(6)]
            p0 = (x, y)
            for s in range(1, 13):
                t = s / 12; u = 1 - t
                cur.append((u**3*p0[0] + 3*u*u*t*v[0] + 3*u*t*t*v[2] + t**3*v[4],
                            u**3*p0[1] + 3*u*u*t*v[1] + 3*u*t*t*v[3] + t**3*v[5]))
            x, y = v[4], v[5]
        elif C == 'A':
            rx, ry, phi, large, sweep, a, b = take(7)
            nx, ny = (x + a, y + b) if rel else (a, b)
            cur += arc_points(x, y, rx, ry, phi, int(large), int(sweep), nx, ny); x, y = nx, ny
        else:
            raise ValueError(C)
    if cur: lines.append(cur)
    return lines


def draw(canvas, scale, ox, oy, line, muted):
    """Draws the controller; returns {svg id: [canvas items]} and {stick id: (x, y, r)} in canvas pixels."""
    P = lambda x, y: (ox + x * scale, oy + y * scale)
    items, circles = {}, {}
    for el in ET.fromstring(DS4_SVG).iter():
        tag = el.tag.split('}')[-1]
        eid = el.get('id', '')
        made = []
        if tag == 'path':
            for pl in path_polylines(el.get('d')):
                closed = len(pl) > 2 and pl[0] == pl[-1]
                pts = [v for p in pl for v in P(*p)]
                made.append(canvas.create_polygon(pts, outline=line, fill='', width=1.6) if closed and eid != 'MainOutline'
                            else canvas.create_line(pts, fill=line, width=1.6))
        elif tag == 'circle':
            cx, cy, r = (float(el.get(k)) for k in ('cx', 'cy', 'r'))
            (x0, y0), (x1, y1) = P(cx - r, cy - r), P(cx + r, cy + r)
            made.append(canvas.create_oval(x0, y0, x1, y1, outline=line, width=1.4, fill=''))
            circles[eid] = (*P(cx, cy), r * scale)
        elif tag == 'rect':
            x, y, w, h = (float(el.get(k)) for k in ('x', 'y', 'width', 'height'))
            r = float(el.get('rx', 0))
            pts = [(x + r, y), (x + w - r, y), (x + w, y), (x + w, y + r), (x + w, y + h - r), (x + w, y + h),
                   (x + w - r, y + h), (x + r, y + h), (x, y + h), (x, y + h - r), (x, y + r), (x, y)]
            made.append(canvas.create_polygon([v for p in pts for v in P(*p)], smooth=bool(r), outline=line,
                                              fill='', width=1.4))
        elif tag == 'line':
            made.append(canvas.create_line(*P(float(el.get('x1')), float(el.get('y1'))),
                                           *P(float(el.get('x2')), float(el.get('y2'))), fill=muted, width=1.2))
        if eid and made:
            items.setdefault(eid, []).extend(made)
    return items, circles
