"""ICO horns for the boy (Mods > ICO horns): the horn models, written from scratch.

No file or part of a file from the game is read: the shape, the colours, the hair tuft and the
surface are computed here, and written with own writers for the engine's object files (.bod),
buffer files (.data), pictures (.gnf) and the archive around them (.psarc). The launcher builds
the archive on the player's PC (tlg_run.py); tests/test_ico_horns.py checks that every machine
gets the same bytes.
  python ico_horns.py out.psarc"""
import math, random, struct, sys, hashlib

# ---------------------------------------------------------------- object file (.bod) writer
NULL = ('ref', None)
BLANK = '\x00'
def S(s): return ('s', s)
def I(i): return ('i', i)
def F(f): return ('f', f)
def R(t, n): return ('ref', (t, n))
def V(*f): return ('v', f)
def L(*refs): return ('list', refs)
def C(f): return ('col', f)
def RAW(b): return ('raw', b)


def bod(objs, counts):
    strs, ids = [], {}
    def sid(s):
        if s not in ids: ids[s] = len(strs); strs.append(s)
        return ids[s]
    body = b''
    for typ, name, props in objs:
        body += struct.pack('<3I', sid(typ), sid(name), sid(BLANK))
        for key, (kind, val) in props:
            k = sid(key)
            if kind == 's': v = struct.pack('<I', sid(val))
            elif kind == 'i': v = struct.pack('<I', val & 0xffffffff)
            elif kind == 'f': v = struct.pack('<f', val)
            elif kind == 'ref': v = struct.pack('<2I', *((sid(BLANK),) * 2 if val is None else (sid(val[0]), sid(val[1]))))
            elif kind == 'v': v = struct.pack('<4f', *val)
            elif kind == 'col': v = struct.pack('<If', 0xffffffff, val)
            elif kind == 'list': v = struct.pack('<I', len(val)) + b''.join(struct.pack('<2I', sid(t), sid(n)) for t, n in val)
            else: v = val
            body += struct.pack('<2I', k, len(v)) + v
        body += b'\xff' * 4
    st = struct.pack('<I', len(strs)) + b''.join(struct.pack('<I', len(s)) + s.encode('latin1') for s in strs)
    st += b'\x00' * 8 + b'\xff' * 4 + b'\x00' * 4
    size = 44 + len(body) + len(st)
    return struct.pack('<11I', 1, 21, 44, 44 + len(body), size - 16, size - 16, *counts, 0, 0) + body + st


def material(name, cl, nm, sp, rg, spec=1.0, rim=0.1):
    T = lambda n: R('Texture', n)
    return bod([('MaterialDefinition', name, [
        ('albedo', T(cl)), ('normal', T(nm)), ('specular', T(sp)), ('roughness', T(rg)),
        ('emissive', T('_white_texture')), ('backlight', T('_black_texture')), ('detailNormal', T('_normal_texture')),
        ('detailMask', T('_black_texture')), ('bloomMask', T('_white_texture')),
        ('color', C(3.5)), ('texTilePan', V(1, 1, 0, 0)), ('uvSelect', S('UV0')),
        ('specularIntensity', F(spec)), ('roughnessScale', F(1)), ('bumpScale', F(1)), ('emissiveIntensity', F(0.05)),
        ('alphaThreshold', F(0.5)), ('refractionAmount', F(0.3)), ('refractionScale', F(0.05)),
        ('incidenceAngleRampIntensity', F(rim)), ('incidenceAngleMaxIntensity', F(-1)), ('incidenceAngleRampRange', F(3)),
        ('incidenceAngleYupCutoff', F(0.5)), ('bloomIntensityOffset', F(0)), ('detailTilePan', V(1, 1, 0, 0)),
        ('detailBumpScale', F(1)), ('detailUvSelect', S('UV0')), ('uvScrollSpeed', V(0, 0, 0, 0)), ('uvScrollSnap', V(1, 1, 1, 1)),
        ('blendType', S('None')), ('renderPass', S('Deferred')), ('renderPriority', I(0)), ('materialFlags', I(2)),
        ('parallax', RAW(b'\x00' * 8)), ('twoSidedFlip', S('XYZ')), ('extStreamFlags', I(2)), ('layer', NULL), ('shader', NULL),
        ('customParamArray', I(0)), ('customTextureArray', I(0)), ('customTextureUsage', S('TextureUsagePS')),
        ('color2', C(1)), ('wetSpecularIntensity', F(0.04)), ('wetRoughnessScale', F(1)), ('wetBumpScale', F(1)),
        ('color3', C(1)), ('customDefinition', S(BLANK)), ('extraData', NULL)])], (1, 0, 1))


def model(path, asset, pre, mat, nverts, ntris, bmin, bmax):
    """path = asset path, asset = short name, pre = prefix of the inner object names."""
    m = pre + asset + '_01'; M = R('MaterialDefinition', mat)
    pos, one = V(0, 0, 0, 1), V(1, 1, 1, 1)
    def sbuf(name, stride, num, flag, fmt):
        return ('StreamBuffer', name, [('data', RAW(b'')), ('type', S('Static')), ('stride', I(stride)), ('num', I(num)),
                                       ('data_flag', S(flag)), ('dataFormat', S(fmt))])
    nul = ('StreamBuffer', '_null_streambuf')
    def ent(an, ext):
        return [('assetName', S(an)), ('remoteAttach', S(BLANK)), ('entityFlags', S(BLANK)), ('parent', R('SceneRoot', path)),
                ('children', I(0)), ('extensions', ext), ('position', pos), ('scale', one), ('rotation', pos)]
    return bod([
        ('RenderBatch', m + 'Shape_b0', [('material', NULL), ('materialDefinition', M), ('materialLocalDef', NULL),
                                         ('primType', I(0)), ('start', I(0)), ('numTris', I(ntris))]),
        ('ShadingGroup', m + '_sg0', [('start', I(0)), ('numPrims', I(ntris)), ('primType', I(0)), ('material', NULL), ('materialDefinition', M)]),
        ('RenderShape', m + '_shape', [('baseVertexIndex', I(0)), ('baseElemIndex', I(0)), ('numVerts', I(nverts)), ('numElems', I(ntris * 3)),
                                       ('shadingGroups', L(('ShadingGroup', m + '_sg0')))]),
        sbuf('_null_streambuf', 0, 0, BLANK, 'Uknown'),
        sbuf(path + '_gbuf_uv1', 8, nverts, 'GPURead', 'R32G32_Float'),
        ('RenderExt', m, [('parentEntity', R('Mesh', m + 'Shape')), ('shapeIndex', I(0)), ('geometryBuf', R('GeometryBuffer', path + '_gbuf')),
                          ('shadowGBuf', NULL), ('modifierTarget', NULL), ('instanceBuf2', NULL), ('indirectBuf', NULL),
                          ('batches', L(('RenderBatch', m + 'Shape_b0'))), ('baseVertexIndex', I(0)), ('numVerts', I(nverts)),
                          ('baseElemIndex', I(0)), ('numElems', I(ntris * 3)), ('layer', S('Opaque')), ('renderableFlags', S(BLANK)),
                          ('decalId', S('Default')), ('category', S(BLANK)), ('cascadeFlagSunIndexMask', I(0)), ('cascadeBatches', I(0)),
                          ('bboxMin', V(*bmin, 1)), ('bboxMax', V(*bmax, 1))]),
        ('Mesh', m + 'Shape', ent(asset + '_01', L(('RenderExt', m))) + [('isClosed', I(0))]),
        ('Entity', pre + asset, ent(asset, I(0))),
        ('SceneRoot', path, [('assetName', S(BLANK)), ('remoteAttach', S(BLANK)), ('entityFlags', S(BLANK)), ('parent', NULL),
                             ('children', L(('Mesh', m + 'Shape'), ('Entity', pre + asset))), ('extensions', I(0)),
                             ('position', pos), ('scale', one), ('rotation', pos), ('childSelection', I(-1)), ('animDataBinds', I(0)),
                             ('geometryBuffer', R('GeometryBuffer', path + '_gbuf')), ('baseName', S(path)), ('instanceCtx', I(0)),
                             ('createCascadeBatchFlags', S(BLANK))]),
        ('GeometryBuffer', path + '_gbuf', [('format', S('Mesh')), ('verts', R('StreamBuffer', path + '_gbuf_verts')),
                                            ('elems', R('StreamBuffer', path + '_gbuf_elems')), ('auxBuf', NULL),
                                            ('extBufs', L(*[nul] * 6, ('StreamBuffer', path + '_gbuf_uv1'))),
                                            ('shapes', L(('RenderShape', m + '_shape')))]),
        sbuf(path + '_gbuf_verts', 32, nverts, 'GPURead', 'Uknown'),
        sbuf(path + '_gbuf_elems', 2, ntris * 3, 'GPURead', 'Uknown')], (12, 8, 64))


def cdat(tag, stride, payload): return b'CDAT' + struct.pack('<3I', tag, stride, len(payload)) + payload


# ---------------------------------------------------------------- picture (.gnf) writer: one level
BC1, BC4, BC5 = 35, 38, 39


def gnf(w, h, fmt, snorm, channels, blocks):
    """channels: 4 picks (x, y, z, w) of 0 = zero, 1 = one, 4 = R, 5 = G, 6 = B, 7 = A."""
    assert len(blocks) == (w // 4) * (h // 4) * (16 if fmt == BC5 else 8)
    r1 = 8 | fmt << 20 | (1 if snorm else 0) << 26
    r2 = (w - 1) | (h - 1) << 14 | 7 << 28
    r3 = channels[0] | channels[1] << 3 | channels[2] << 6 | channels[3] << 9 | 8 << 20 | 1 << 25 | 1 << 26 | 9 << 28
    r4 = (w - 1) << 13
    hdr = b'GNF ' + struct.pack('<IBBBBI', 0xf8, 2, 1, 8, 0, 256 + len(blocks)) + struct.pack('<7I', 0, r1, r2, r3, r4, 0, 0) + struct.pack('<I', len(blocks))
    return hdr + b'\x00' * (256 - len(hdr)) + blocks


def rgb565(r, g, b): return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def horn_colours(ring=0.0):
    """512x512, one colour per 4x4 block: bone white at the base, grey streaks along the outer half, black tip."""
    rnd = random.Random(7)
    raw = [rnd.random() for _ in range(128)]
    st = [sum(raw[(x + d) % 128] for d in range(-3, 4)) / 7 for x in range(128)]
    lo, hi = min(st), max(st); st = [(v - lo) / (hi - lo) for v in st]
    def ss(e0, e1, x):
        k = max(0.0, min(1.0, (x - e0) / (e1 - e0))); return k * k * (3 - 2 * k)
    bone, tip0, sk = (232, 228, 216), 0.50, 0.8
    grey, black = (112, 108, 102), (26, 24, 22)
    r2 = random.Random(23); lines, f0 = [], 0.03          # growth rings: irregular dark lines across the horn
    while f0 < 0.8: lines.append((f0, 0.5 + 0.5 * r2.random(), r2.random() * 6.28)); f0 += 0.018 + 0.03 * r2.random()
    fib = [r2.random() for _ in range(128)]               # fine fibres along the horn
    out = bytearray()
    for y in range(128):
        f = (y + 0.5) / 128
        for x in range(128):
            n = rnd.random() * 0.06
            g1 = min(1.0, sk * ss(tip0 - 0.37, tip0 + 0.03, f) * (0.35 + 0.9 * st[x]) + n * ss(0.3, 0.6, f))
            c = tuple(a + (z - a) * g1 for a, z in zip(bone, grey))
            if ring:
                fw = f + 0.006 * math.sin(x * 2 * math.pi / 128 * 3 + y * 0.2)
                d = max([w * max(0.0, 1 - abs(fw - p) / 0.007) for p, w, _ in lines] or [0])
                m = (1 - ring * d) * (1 - ring * 0.25 * (fib[x] - 0.5)) * (1 - ring * 0.12 * ss(0.0, 0.5, f))
                c = tuple(q * m for q in c)
            k = ss(tip0, min(0.99, tip0 + 0.23), f)
            v = rgb565(*(int(round(a + (z - a) * k)) for a, z in zip(c, black)))
            out += struct.pack('<HHI', v, v, 0)
    # The game reads the picture in small tiles of 8x8 blocks, not row by row (seen in game: blocks written at the
    # left edge showed up as a regular grid of dots). Place the hair strip where the left edge is really shown.
    for Y in range(128):
        for X in range(10):
            ty, py, tx, px = Y // 8, Y % 8, X // 8, X % 8
            sy, sx = ty * 8 + tx // 2, (tx % 2) * 64 + py * 8 + px
            h = HAIR[0] if Y < 64 else HAIR[1]; m = 0.85 + 0.3 * raw[(X * 13 + Y // 3) % 128]
            v = rgb565(*(min(255, int(q * m)) for q in h))
            out[(sy * 128 + sx) * 8:(sy * 128 + sx) * 8 + 8] = struct.pack('<HHI', v, v, 0)
    return bytes(out)


# ---------------------------------------------------------------- horn shape
HAIR = ((14, 13, 12), (14, 13, 12))    # black, like the boy's hair
SURF = (0.0, 150, 0.5, 0.05)            # growth rings 0..1, roughness 0..255, shine strength, edge glow
TUFT = (0.044, 1.1, 0, 0.2)                     # None, or (reach along the horn 0..1, flare at the root, colour 0/1, lock size 0..1)
FLIP, INNER = True, False             # faces the other way round / second wall inside, facing inward
THICK, TILT, SWEEP = 1.05, 0.0, 0.0     # thickness factor, tilt up (degrees), sweep back (degrees)
P, RAD, SIDES, RINGS = ((0.135, 0.03), (0.175, 0.025), (0.16, 0.07)), 0.016, 8, 12


def centerline(t):
    Z = -0.045
    (x1, y1), (x2, y2), (x3, y3) = P
    p = (0.055, 0.0, Z), (x1, y1, Z), (x2, y2, Z - 0.02), (x3, y3, Z - 0.06)
    x, y, z = ((1-t)**3*a + 3*(1-t)**2*t*b + 3*(1-t)*t*t*c + t**3*d for a, b, c, d in zip(*p))
    dx, dy, dz = x - p[0][0], y - p[0][1], z - p[0][2]              # turn around the base point
    a = math.radians(TILT); dx, dy = math.cos(a) * dx - math.sin(a) * dy, math.sin(a) * dx + math.cos(a) * dy
    a = math.radians(SWEEP); dx, dz = math.cos(a) * dx + math.sin(a) * dz, -math.sin(a) * dx + math.cos(a) * dz
    return (p[0][0] + dx, p[0][1] + dy, p[0][2] + dz)


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1
    return tuple(c / l for c in v)


def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def sn8(v): return bytes((int(round(max(-1, min(1, c)) * 127)) & 0xff) for c in v)


def horn(mirror):
    pos, nrm, tan, uv = [], [], [], []
    for r in range(RINGS):
        t = r / (RINGS - 1) * 0.985
        c = centerline(t); T = norm(sub(centerline(t + 1e-3), c))
        B = norm(cross(T, (0, 0, 1))); N = cross(B, T)
        for s in range(SIDES + 1):
            a = 2 * math.pi * s / SIDES
            d = tuple(math.cos(a) * B[i] + math.sin(a) * N[i] for i in range(3))
            pos.append(tuple(c[i] + (RAD * (1 - t) + 0.001) * THICK * d[i] for i in range(3)))
            nrm.append(d); tan.append(T); uv.append((s / SIDES, t))
    tip = centerline(1.0)
    pos.append(tip); nrm.append(norm(sub(tip, centerline(0.99)))); tan.append((1, 0, 0)); uv.append((0.5, 1.0))
    tris, W = [], SIDES + 1
    for r in range(RINGS - 1):
        for s in range(SIDES):
            a, b, c, d = r*W+s, r*W+s+1, (r+1)*W+s, (r+1)*W+s+1
            tris += [(a, c, b), (b, c, d)]
    for s in range(SIDES): tris.append(((RINGS-1)*W+s, len(pos) - 1, (RINGS-1)*W+s+1))
    # closed base: own copy of the first ring facing back into the head, plus a centre point
    back = norm(sub(centerline(0), centerline(1e-3))); c0 = len(pos)
    for s in range(W): pos.append(pos[s]); nrm.append(back); tan.append(tan[s]); uv.append((s / SIDES, 0.0))
    pos.append(centerline(0)); nrm.append(back); tan.append(tan[0]); uv.append((0.5, 0.0))
    for s in range(SIDES): tris.append((len(pos) - 1, c0 + s, c0 + s + 1))
    if TUFT:
        reach, flare, col, amt = TUFT; rnd = random.Random(11)
        def frame(t):
            c = centerline(t); T = norm(sub(centerline(t + 1e-3), c)); B = norm(cross(T, (0, 0, 1))); return c, T, B, cross(B, T)
        def at(t, a, r):
            c, T, B, N = frame(t); return tuple(c[i] + r * (math.cos(a) * B[i] + math.sin(a) * N[i]) for i in range(3))
        hr = lambda t: (RAD * (1 - t) + 0.001) * THICK
        def face(a, b, c, mid, T, v0, v1):
            n = norm(cross(sub(b, a), sub(c, a))); ce = tuple((a[i] + b[i] + c[i]) / 3 for i in range(3))
            if sum(n[i] * (ce[i] - mid[i]) for i in range(3)) < 0: b, c, n = c, b, tuple(-x for x in n)
            k = len(pos)
            for q, vv in ((a, v0), (b, v0), (c, v1)):
                pos.append(q); nrm.append(n); tan.append(T)
                u = 0.012 + 0.05 * rnd.random(); v = (0.03 + 0.42 * vv) + (0.5 if col else 0.0)
                uv.append(((u - 0.1) / 0.8, (v - 0.02) / 0.96))
            tris.append((k, k + 1, k + 2))
        # collar: closes the root so no scalp shows between the locks
        n, t1 = 10, reach * 0.5
        for s in range(n):
            a0, a1 = 2 * math.pi * s / n, 2 * math.pi * (s + 1) / n
            p0, p1, q0, q1 = at(0, a0, hr(0) * flare), at(0, a1, hr(0) * flare), at(t1, a0, hr(t1) + 0.0015), at(t1, a1, hr(t1) + 0.0015)
            mid = centerline(t1 / 2); T = frame(0)[1]
            face(p0, p1, q0, mid, T, 0.0, 0.5); face(p1, q1, q0, mid, T, 0.0, 0.5)
        # locks: pointed, lying along the horn
        n = 14
        for k in range(n):
            a = 2 * math.pi * (k + 0.6 * rnd.random()) / n; ta = rnd.random() * 0.05; tb = reach * (0.55 + 0.45 * rnd.random())
            r0 = hr(ta) * flare * (0.95 + 0.25 * rnd.random()); w = 0.35 + 0.15 * rnd.random()
            base = [at(ta, a - w, r0), at(ta, a, r0 + 0.006 * amt), at(ta, a + w, r0), at(ta, a, r0 * 0.6)]
            tipp = at(tb, a + 0.5 * (rnd.random() - 0.5), hr(tb) + 0.001 + 0.002 * amt)
            mid = tuple((sum(p[i] for p in base) / 4 * 2 + tipp[i]) / 3 for i in range(3)); T = frame(ta)[1]
            for j in range(4): face(base[j], base[(j + 1) % 4], tipp, mid, T, 0.0, 1.0)
    if FLIP: tris = [(a, c, b) for a, b, c in tris]
    if INNER:        # a slightly smaller copy inside, facing the other way: no gap can show the background
        n = len(pos); mid = [centerline(min(1.0, (v - 0.0))) for _, v in uv]
        pos += [tuple(m[i] + 0.9 * (q[i] - m[i]) for i in range(3)) for q, m in zip(pos[:n], mid)]
        nrm += [tuple(-x for x in q) for q in nrm[:n]]; tan += tan[:n]; uv += uv[:n]
        tris += [(a + n, c + n, b + n) for a, b, c in tris]
    if mirror:
        fl = lambda l: [(-x, y, z) for x, y, z in l]
        pos, nrm, tan, tris = fl(pos), fl(nrm), fl(tan), [(a, c, b) for a, b, c in tris]
    uv = [(0.1 + u * 0.8, 0.02 + v * 0.96) for u, v in uv]
    return pos, nrm, tan, uv, tris


def build(variants=None):
    """variants: list of (THICK, TILT, SWEEP, FLIP, INNER, TUFT, SURF, names); names = None for the shared surface,
    or (material, colour, normal, specular, roughness) paths for an own surface."""
    global THICK, TILT, SWEEP, FLIP, INNER, TUFT, SURF
    files = {}
    D = 'game/textures/props/icohorn_'; shared = ('game/materials/SM_icohorn', D + 'cl', D + 'nm', D + 'sp', D + 'rg')
    def surface(nm):
        MAT, cl, n_, sp, rg = nm
        files[MAT.lower() + '.bod'] = material(MAT, cl, n_, sp, rg, SURF[2], SURF[3])
        files.setdefault(cl + '.gnf', gnf(512, 512, BC1, False, (4, 5, 6, 7), horn_colours(SURF[0])))
        files.setdefault(n_ + '.gnf', gnf(64, 64, BC5, True, (4, 5, 0, 1), bytes([0x80, 0x80] + [0] * 6) * 2 * 256))
        files.setdefault(sp + '.gnf', gnf(64, 64, BC1, False, (4, 5, 6, 7), struct.pack('<HHI', rgb565(30, 30, 30), rgb565(30, 30, 30), 0) * 256))
        files.setdefault(rg + '.gnf', gnf(64, 64, BC4, False, (4, 0, 0, 1), bytes([SURF[1], SURF[1]] + [0] * 6) * 256))
    base = SURF; surface(shared)
    for vi, side in [(v, s) for v in range(len(variants or [0])) for s in 'rl']:
        nm = shared
        if variants:
            THICK, TILT, SWEEP, FLIP, INNER, TUFT, SURF, own = variants[vi]; nm = own or shared
            if own: surface(own)
            else: assert SURF == base
        a = ('icoh%02d_' % (vi + 1) if variants else 'icohorn_') + side + 'x'; path = 'game/assets/props/level/' + a + '/' + a
        pos, nrm, tan, uv, tris = horn(side == 'l')
        vb = b''.join(struct.pack('<3f', *pos[i]) + sn8(nrm[i]) + b'\x00' + b'\xff' * 4 + sn8(tan[i]) + b'\x7f' + struct.pack('<2f', *uv[i])
                      for i in range(len(pos)))
        bmin = tuple(min(p[k] for p in pos) for k in range(3)); bmax = tuple(max(p[k] for p in pos) for k in range(3))
        files[path + '.bod'] = model(path, a, 'tlg_', nm[0], len(pos), len(tris), bmin, bmax)
        files[path + '_gbuf_verts.data'] = cdat(0xc0000025, 32, vb)
        files[path + '_gbuf_elems.data'] = cdat(0xc0000029, 2, b''.join(struct.pack('<3H', *t) for t in tris))
        files[path + '_gbuf_uv1.data'] = cdat(0xc0000021, 8, b''.join(struct.pack('<2f', *q) for q in uv))
    return files


def write_psarc(path, files):
    BS = 0x10000
    names = sorted(files, key=lambda k: hashlib.md5(k.upper().encode()).digest())   # the engine binary-searches by md5 of the upper-cased path
    blobs = ['\x00'.join(names).encode()] + [files[k] for k in names]
    entries, blocks, data = [], [], bytearray()
    for i, b in enumerate(blobs):
        md5 = b'\x00' * 16 if i == 0 else hashlib.md5(names[i-1].upper().encode()).digest()
        entries.append((md5, len(blocks), len(b), len(data)))
        for o in range(0, max(len(b), 1), BS):
            chunk = b[o:o+BS]; blocks.append(0 if len(chunk) == BS else len(chunk)); data += chunk
    toc_len = 32 + len(blobs) * 30 + len(blocks) * 2
    toc = bytearray(b'PSAR' + struct.pack('>HH', 1, 4) + b'zlib' + struct.pack('>IIIII', toc_len, 30, len(blobs), BS, 0xd))
    for md5, blk, usize, off in entries: toc += md5 + struct.pack('>I', blk) + usize.to_bytes(5, 'big') + (off + toc_len).to_bytes(5, 'big')
    for b in blocks: toc += struct.pack('>H', b)
    open(path, 'wb').write(toc + data)


def write(path):
    """Builds the horn archive at path; returns its sha256."""
    write_psarc(path, build())
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


if __name__ == '__main__':
    print(write(sys.argv[1]))
