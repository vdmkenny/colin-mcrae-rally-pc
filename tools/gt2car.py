#!/usr/bin/env python3
"""Make an extra car for the game from a Gran Turismo 2 car, read from your own GT2 disc.

Usage: gt2car.py <GT2 disc .bin or .cue> <car id> <out dir> [options]
       gt2car.py <GT2 disc .bin or .cue> --list [text]

The car id is GT2's five-letter code (--list finds it by name, e.g. --list vivio). The out dir
becomes a car folder for the game's cars/ folder: car.ini plus the car's model and texture files.
The body comes from GT2 (its night model for night stages, a simpler one for split screen and
the car menu); handling, sounds, records, wheels and the driver's view come from a disc car
(--base), whose files are read from your Colin McRae Rally disc (--cmr). car.ini can be edited
afterwards. Works with the NTSC-U discs; the Simulation disc has every car.
"""
import argparse
import collections
import colorsys
import copy
import gzip
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmrcar  # noqa: E402

RAW = 2352
UNIT = 1 / 4096   # GT2 model units per metre


# ------------------------------------------------------------------ disc and GT2.VOL
class Disc:
    def __init__(self, path):
        if path.lower().endswith(".cue"):
            base = os.path.dirname(os.path.abspath(path))
            line = next(ln for ln in open(path, encoding="latin-1") if ln.strip().upper().startswith("FILE"))
            path = os.path.join(base, line[line.index('"') + 1:line.rindex('"')])
        self.f = open(path, "rb")

    def sectors(self, lba, n):
        out = []
        for i in range(n):
            self.f.seek((lba + i) * RAW + 24)
            out.append(self.f.read(2048))
        return b"".join(out)

    def find(self, name):
        pvd = self.sectors(16, 1)
        if pvd[1:6] != b"CD001":
            sys.exit("not an ISO9660 MODE2/2352 image")
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        data = self.sectors(lba, (size + 2047) // 2048)
        pos = 0
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            if data[pos + 33:pos + 33 + data[pos + 32]].split(b";")[0].upper() == name.upper().encode():
                return struct.unpack_from("<I", data, pos + 2)[0], struct.unpack_from("<I", data, pos + 10)[0]
            pos += ln
        sys.exit(f"{name} not found - is this a Gran Turismo 2 disc?")


class Vol:
    """GT2.VOL ("GTFS"): an offset table, then 32-byte directory entries."""
    def __init__(self, disc):
        self.disc = disc
        self.lba, _ = disc.find("GT2.VOL")
        head = self.read(0, 16)
        if head[:4] != b"GTFS":
            sys.exit("GT2.VOL is not a GTFS volume")
        nfiles, nentries = struct.unpack_from("<HH", head, 8)
        self.offs = struct.unpack_from(f"<{nfiles}I", self.read(16, 4 * nfiles))
        d = self.read(self.offs[1] & ~0x7ff, 32 * nentries)
        self.entries = []
        for i in range(nentries):
            idx, flags = struct.unpack_from("<HB", d, 32 * i + 4)
            self.entries.append((d[32 * i + 7:32 * i + 32].split(b"\0")[0].decode("ascii", "replace"), idx, flags))

    def read(self, off, size):
        first = off // 2048
        data = self.disc.sectors(self.lba + first, (off + size + 2047) // 2048 - first)
        return data[off - first * 2048:off - first * 2048 + size]

    def dir(self, name):
        """{file name: data reader} of a top-level directory."""
        top = next(idx for n, idx, fl in self.entries if n == name and fl & 1)
        files = {}
        for n, idx, fl in self.entries[top + 1:]:
            if fl & 1:
                break
            start = self.offs[idx] & ~0x7ff
            files[n] = (start, (self.offs[idx + 1] & ~0x7ff) - self.offs[idx])
            if fl & 0x80:
                break
        return files

    def file(self, files, name):
        start, size = files[name]
        data = self.read(start, size)
        return gzip.decompress(data) if name.endswith(".gz") else data


def car_list(vol):
    """{car id: (US name, [swatch colour per paint colour])} from the American car list."""
    root = {n: (vol.offs[idx] & ~0x7ff, (vol.offs[idx + 1] & ~0x7ff) - vol.offs[idx])
            for n, idx, fl in vol.entries if not fl & 1}
    d = vol.read(*root[".carinfoa"])
    chars = "-0123456789abcdefghijklmnopqrstuvwxyz"
    cars = {}
    for c in range(struct.unpack_from("<I", d, 4)[0]):
        cid, idx, misc = struct.unpack_from("<IHH", d, (c + 1) * 8)
        colours = ((misc & 0x3c) >> 2) + 1
        n = d[idx + colours * 3]
        code = "".join(chars[(cid >> (6 * k)) & 63] for k in range(4, -1, -1))
        name = d[idx + colours * 3 + 1:idx + colours * 3 + 1 + n].split(b"\0")[0].decode("latin-1")
        cars[code] = (name, list(struct.unpack_from(f"<{colours}H", d, idx)))
    return cars


# ------------------------------------------------------------------ GT2 car model (.cdo/.cno)
def _normal(w):
    def field(shift):
        v = (w >> shift) & 0x3ff
        return (v - 1024 if v & 0x200 else v) / 500
    return field(2), field(12), field(22)


def parse_model(d):
    """A GT2 car model in metres: wheels and three levels of detail, most detailed first.

    GT2 has y up, the front towards -z and the left towards -x. Returns {wheels: [(x, y, z,
    radius, width)] front left, front right, rear left, rear right, lods: [{vertices: [(x, y, z)],
    faces: [{verts, normals, uvs, palette, colour}]}]}; normals are per face corner, faces
    without uvs are flat colour (0xBBGGRR).
    """
    menu = struct.unpack_from("<4H", d, 0x18 if not struct.unpack_from("<H", d, 8)[0] else 8)
    wheels = []
    for i in range(4):
        x, y, z, _ = struct.unpack_from("<4h", d, 0x20 + 8 * i)
        r, w = (menu[0], menu[1]) if i < 2 else (menu[2], menu[3])
        wheels.append((x * UNIT, y * UNIT, z * UNIT, r * UNIT, w * UNIT))
    lods = []
    o = 0x884
    for _ in range(struct.unpack_from("<I", d, 0x868)[0]):
        nv, nn, nt, nq, _, _, nut, nuq = struct.unpack_from("<8H", d, o)
        k = UNIT * 2.0 ** (struct.unpack_from("<H", d, o + 0x4c)[0] - 16)
        p = o + 0x50
        verts = [tuple(c * k for c in struct.unpack_from("<3h", d, p + 8 * i)) for i in range(nv)]
        p += 8 * nv
        normals = [_normal(struct.unpack_from("<I", d, p + 4 * i)[0]) for i in range(nn)]
        p += 4 * nn
        faces = []
        for n, quad, uv in ((nt, False, False), (nq, True, False), (nut, False, True), (nuq, True, True)):
            size = 28 if uv else 16
            for i in range(n):
                q = p + size * i
                vi = list(d[q:q + (4 if quad else 3)])
                first, _, rest, ftype = struct.unpack_from("<HHII", d, q + 4)
                ni = [(first >> 5) & 0x1ff, (rest >> 1) & 0x1ff, (rest >> 10) & 0x1ff, (rest >> 19) & 0x1ff]
                f = {"verts": vi, "normals": [normals[j] if j < nn else None for j in ni[:len(vi)]],
                     "colour": ftype & 0xffffff, "uvs": None, "palette": None}
                if uv:
                    pal = struct.unpack_from("<H", d, q + 18)[0]
                    f["palette"] = (pal & 3) + ((pal >> 6) & 3) * 4
                    corners = [d[q + 16:q + 18], d[q + 20:q + 22], d[q + 24:q + 26], d[q + 26:q + 28]]
                    f["uvs"] = [tuple(c) for c in corners[:len(vi)]]
                faces.append(f)
            p += size * n
        lods.append({"vertices": verts, "faces": faces})
        o = p
    return {"wheels": wheels, "lods": lods}


# ------------------------------------------------------------------ GT2 car paint (.cdp/.cnp)
def parse_paint(d, colour=0):
    """A 256x224 4-bit texture and its 16 palettes (15-bit colours) for one of the car's colours."""
    colour = min(colour, d[0] - 1)
    base = 0x20 + colour * 0x240
    palettes = [list(struct.unpack_from("<16H", d, base + 32 * i)) for i in range(16)]
    pixels = bytearray(256 * 224)
    for i, b in enumerate(d[0x43a0:0x43a0 + 256 * 224 // 2]):
        pixels[2 * i] = b & 15
        pixels[2 * i + 1] = b >> 4
    return {"pixels": pixels, "palettes": palettes}


def rgb_of(c):
    return (c & 31) * 8, (c >> 5 & 31) * 8, (c >> 10 & 31) * 8


def repaint(palettes, rgb, used, swatch):
    """Paints the car rgb instead of its GT2 colour (swatch, 15-bit), keeping the shading.
    A paint colour is a brighter or darker swatch: for white, silver and black paints, which
    glass, chrome and trim also are, that means the same tint (red and green against blue)
    exactly; for other paints, the same proportions. Palette 0 (the wheels) stays."""
    base = rgb_of(swatch)
    neutral = max(base) - min(base) <= 16
    out = [list(p) for p in palettes]
    for k in used - {0}:
        for i, c in enumerate(out[k]):
            col = rgb_of(c)
            if not c & 0x7fff or not max(col):
                continue
            f = max(col) / max(base)
            if neutral:
                paint = (col[0] - col[2], col[1] - col[2]) == (base[0] - base[2], base[1] - base[2]) and max(col) >= 64
            else:
                paint = all(abs(a - b * f) <= 12 for a, b in zip(col, base))
            if paint:
                out[k][i] = cmrcar.to15(*(min(255, round(ch * f)) for ch in rgb), stp=c & 0x8000)
    return out


# ------------------------------------------------------------------ the game's car files
M = 32                              # game vertex units per metre
PAGE13, PAGE14 = 0x0D, 0x0E
IDENT = (0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x10000)
FIX = 65536


def clut_word(x, y):
    return (x >> 4) | (y << 6)


def to_game(p, dy=0.0):
    """GT2 axes to the game's: x forward, y up, z left (metres)."""
    x, y, z = p
    return -z, y + dy, -x


def norm(v):
    m = math.sqrt(sum(c * c for c in v)) or 1
    return tuple(c / m for c in v)


def cross(a, b):
    return a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]


def triangles(lod, dy, transparent):
    """GT2 faces as game-axis triangles: [(corners [(position, normal)], uvs, palette, colour,
    decal)]. GT2 winds every face outward the same way, which the change of axes mirrors.
    A decal (a lamp) is drawn over another face on the same corners: the one with transparent
    texels, else the first. The face under a decal takes the decal's normals (GT2 gives it a
    placeholder), and corners whose normals point inward get the face's own."""
    out = []
    verts = [to_game(v, dy) for v in lod["vertices"]]
    twins = collections.defaultdict(list)
    for f in lod["faces"]:
        twins[tuple(sorted(f["verts"]))].append(f)
    decals = set()
    for fs in twins.values():
        if len(fs) > 1:
            holes = [f for f in fs if transparent(f)]
            decals.add(id(holes[0] if holes and len(holes) < len(fs) else fs[0]))
    for f in lod["faces"]:
        normals = dict(zip(f["verts"], f["normals"]))
        cover = [g for g in twins[tuple(sorted(f["verts"]))] if id(g) in decals and g is not f]
        if cover and id(f) not in decals:
            normals = dict(zip(cover[0]["verts"], cover[0]["normals"]))
        order = [(0, 2, 1), (0, 3, 2)] if len(f["verts"]) == 4 else [(0, 2, 1)]
        for t in order:
            vi = [f["verts"][i] for i in t]
            pos = [verts[i] for i in vi]
            face_n = norm(cross([b - a for a, b in zip(pos[0], pos[1])], [b - a for a, b in zip(pos[0], pos[2])]))
            ns = [norm(to_game(normals[i])) if normals[i] else face_n for i in vi]
            if sum(sum(a * b for a, b in zip(face_n, n)) for n in ns) < 0:
                ns = [face_n] * 3
            uvs = [f["uvs"][i] for i in t] if f["uvs"] else None
            out.append((list(zip(pos, ns)), uvs, f["palette"], f["colour"], id(f) in decals))
    return out


def transparent_texels(paint):
    """Whether a face's texture area has transparent texels."""
    def test(f):
        if not f["uvs"]:
            return False
        pal = paint["palettes"][f["palette"]]
        us, vs = [u for u, _ in f["uvs"]], [v for _, v in f["uvs"]]
        return any(not pal[paint["pixels"][v * 256 + u]] & 0x7fff
                   for v in range(min(vs), min(223, max(vs)) + 1) for u in range(min(us), max(us) + 1))
    return test


def classify(tris, panels):
    """Splits the body into damage panels (ids from 6) by where each triangle is and faces. With
    11 panels, the disc cars' layout: arches and dark parts, left, right, underbody, roof and
    windscreen, rear window and hatch, bonnet and headlights, front bumper, left and right tail
    lights, rear bumper. With 4 (MEDIUM): dark parts, left, right, the rest."""
    pts = [p for t in tris for p, _ in t[0]]
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
    zw = max(abs(p[2]) for p in pts)
    out = [[] for _ in range(panels)]
    for t in tris:
        c = [sum(p[i] for p, _ in t[0]) / 3 for i in range(3)]
        n = norm([sum(nn[i] for _, nn in t[0]) for i in range(3)])
        fx, h = (c[0] - x0) / (x1 - x0), (c[1] - y0) / (y1 - y0)
        if t[1] is None:
            k = 0
        elif abs(n[2]) >= 0.6:
            k = 1 if c[2] > 0 else 2
        elif panels == 4:
            k = 3
        elif n[1] <= -0.6:
            k = 3
        elif n[1] >= 0.6:
            k = 6 if fx > 0.72 else 5 if fx < 0.12 else 4
        elif n[0] > 0.3:
            k = 7 if h < 0.35 else 6 if h < 0.6 else 4
        elif n[0] < -0.3:
            k = 10 if h < 0.3 else (8 if c[2] > 0 else 9) if h < 0.6 and abs(c[2]) > 0.4 * zw else 5
        else:
            k = 6 if fx > 0.75 else 5 if fx < 0.15 else 1 if c[2] > 0 else 2
        out[k].append(t)
    return out


def vertex(pos, n):
    return cmrcar.Vertex(round(pos[0] * M), round(pos[1] * M), round(pos[2] * M),
                         round(n[0] * 4096), round(n[1] * 4096), round(n[2] * 4096))


def dent(v):
    """The fully damaged position: up to 9 cm in towards the car's middle, the same for every
    panel's copy of a corner, so decals stay on what they cover."""
    d = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z) or 1
    k = (1 + hash((v.x, v.y, v.z)) % 3) / d
    return cmrcar.Vertex(v.x - round(v.x * k), v.y - round(v.y * k), v.z - round(v.z * k), v.nx, v.ny, v.nz)


def panel_mesh(tris, tex_of, zbias=1):
    """A body panel: the live, dented and pristine damage states."""
    verts, index, g3, gt3 = [], {}, [], []
    for corners, uvs, palette, colour, _ in tris:
        ids = []
        for pos, n in corners:
            v = vertex(pos, n)
            ids.append(index.setdefault(v.pack(), len(verts)))
            if ids[-1] == len(verts):
                verts.append(v)
        if uvs:
            gt3.append(cmrcar.FaceGT3(ids, uvs, tex_of(palette)))
        else:
            r, g, b = colour & 255, colour >> 8 & 255, colour >> 16 & 255
            g3.append(cmrcar.FaceG3(ids, 0x30000000 | max(6, b // 2) << 16 | max(6, g // 2) << 8 | max(6, r // 2)))
    if not verts:   # an empty panel keeps the ids contiguous: one zero-area face
        verts, g3 = [cmrcar.Vertex()], [cmrcar.FaceG3([0, 0, 0], 0x30060606)]
    if len(verts) > cmrcar.MAX_VERTS_PER_BLOCK:
        sys.exit(f"a body panel has {len(verts)} vertices; the game allows {cmrcar.MAX_VERTS_PER_BLOCK}")
    dented = [dent(v) for v in verts]
    faces = lambda: cmrcar.FaceBlock(list(g3), list(gt3))   # noqa: E731
    pristine = [cmrcar.Vertex.unpack(v.pack(), 0) for v in verts]
    mesh = cmrcar.Mesh([cmrcar.Lod(faces(), cmrcar.VertBlock(verts)),
                        cmrcar.Lod(faces(), cmrcar.VertBlock(dented)),
                        cmrcar.Lod(faces(), cmrcar.VertBlock(pristine))])
    mesh.radius, mesh.zbias = cmrcar.mesh_radius(mesh), zbias
    return mesh


def place(x, y, z):
    return list(IDENT) + [round(x * FIX), round(y * FIX), round(z * FIX)]


class Texels:
    """Copies 4-bit texture areas out of the base car's VRAM into free space in page 13 and
    remaps the faces that use them."""
    def __init__(self, src, area, clut_slots):
        self.src, self.area, self.clut_slots = src, area, list(clut_slots)
        self.shelf_x, self.shelf_y, self.shelf_h = area[0], area[1], 0
        self.tims, self.moved = [], {}

    def texel(self, tx, u, v):
        return self.src.mem[v * 1024 + tx + u // 4] >> (u % 4 * 4) & 15

    def rim_size(self, tp, cl, box):
        """How far out (0..1 of the radius) a wheel hub texture's rim reaches before the tyre."""
        u0, v0, u1, v1 = box
        cu, cv, rad = (u0 + u1) / 2, (v0 + v1) / 2, (u1 - u0 + 1) / 2
        pal = self.src.mem[(cl >> 6) * 1024 + (cl & 63) * 16:][:16]
        rings = collections.defaultdict(list)
        for v in range(v0, v1 + 1):
            for u in range(u0, u1 + 1):
                d = math.hypot(u - cu, v - cv) / rad
                if d < 1:
                    rings[int(d * 16)].append(sum(rgb_of(pal[self.texel((tp & 15) * 64, u, v)])))
        lum = [sum(rings[i]) / len(rings[i]) for i in range(16)]
        edge = (max(lum) + lum[15]) / 2
        return max(i for i in range(16) if lum[i] > edge) / 16 + 1 / 16

    def region(self, tp, cl, box, scale=1.0, rim=None):
        """Copies a texture area, scaled, with its rim resized to `rim` if it is a wheel hub."""
        key = (tp, cl, scale, rim)
        if key in self.moved:
            return self.moved[key]
        u0, v0, u1, v1 = box
        u0 &= ~3
        w, h = u1 - u0 + 1, v1 - v0 + 1
        dw, dh = max(4, (math.ceil(w * scale) + 3) & ~3), max(1, math.ceil(h * scale))
        if self.shelf_x + dw // 4 > self.area[2]:
            self.shelf_x, self.shelf_y, self.shelf_h = self.area[0], self.shelf_y + self.shelf_h, 0
        x, y = self.shelf_x, self.shelf_y
        if y + dh > self.area[3]:
            sys.exit("the car's textures do not fit in its VRAM area")
        self.shelf_x += dw // 4
        self.shelf_h = max(self.shelf_h, dh)
        tx = (tp & 15) * 64
        src_rim = rim and self.rim_size(tp, cl, box)
        px = []
        for r in range(dh):
            for c in range(dw):
                fu, fv = c / scale / w * 2 - 1, r / scale / h * 2 - 1
                if rim:
                    d = math.hypot(fu, fv)
                    k = src_rim / rim if d <= rim else (src_rim + (d - rim) * (1 - src_rim) / (1 - rim)) / d
                    fu, fv = fu * k, fv * k
                su = u0 + max(0, min(w - 1, int((fu + 1) / 2 * w)))
                sv = v0 + max(0, min(h - 1, int((fv + 1) / 2 * h)))
                px.append(self.texel(tx, su, sv))
        cx, cy = self.clut_slots.pop(0)
        palette = self.src.mem[(cl >> 6) * 1024 + (cl & 63) * 16:][:16]
        self.tims.append(cmrcar.TIM.make_4bpp(x, y, px, dw, cx, cy, palette))
        self.moved[key] = (PAGE13, clut_word(cx, cy), u0, v0, (x - 832) * 4, y, scale)
        return self.moved[key]

    def remap(self, scene, faces, scale=1.0, rim=None):
        """Moves the faces' texture areas (per tex entry, the box all of them use) and remaps."""
        boxes = {}
        for f in faces:
            b = boxes.setdefault(scene.tex[f.tex], [255, 255, 0, 0])
            for u, v in f.uv:
                b[:] = min(b[0], u), min(b[1], v), max(b[2], u), max(b[3], v)
        for f in faces:
            tp, cl = scene.tex[f.tex]
            ntp, ncl, u0, v0, du, dv, k = self.region(tp, cl, boxes[(tp, cl)], scale, rim)
            f.uv = [(du + int((u - u0) * k), dv + int((v - v0) * k)) for u, v in f.uv]
            f.tex = tex_index(scene, ntp, ncl)


def tex_index(scene, tp, cl):
    if (tp, cl) not in scene.tex:
        scene.tex.append((tp, cl))
    return scene.tex.index((tp, cl))


def exterior(gt2, paint, base, texels, palette_tex, panels, wheel_scale, lod, rim):
    """Chunk 0: root, wheels (the base car's, resized, rims to `rim` inches), body group, body
    panels, and the decals in a last panel drawn one step nearer, over what they cover."""
    bw = next(n for n in base.nodes if n.id & 0xfff == 1)
    bmesh = base.meshes[bw.mesh]
    bverts = bmesh.lods[0].verts.verts
    b_r = max(math.hypot(v.x, v.y) for v in bverts) / M / math.sqrt(2)
    b_hw = max(abs(v.z) for v in bverts) / M
    ground = bw.translation[1] - b_r
    x, y, z, r, width = gt2["wheels"][0]
    dy = ground - (y - r)
    s = cmrcar.Scene()
    wheel_faces = [cmrcar.FaceGT3(list(f.v), list(f.uv), f.tex) for f in bmesh.lods[0].faces.gt3]
    hub = [f for f in wheel_faces if len({bverts[i].z for i in f.v}) == 1]
    texels.remap(base, hub, wheel_scale, rim and rim * 0.0254 / 2 / r)
    texels.remap(base, [f for f in wheel_faces if f not in hub], wheel_scale)
    for f in wheel_faces:
        f.tex = tex_index(s, *base.tex[f.tex])
    k, kz = r / b_r, width / 2 / b_hw
    wheel = cmrcar.Mesh([cmrcar.Lod(cmrcar.FaceBlock([], wheel_faces), cmrcar.VertBlock(
        [cmrcar.Vertex(round(v.x * k), round(v.y * k), round(v.z * kz), v.nx, v.ny, v.nz) for v in bverts]))])
    wheel.radius, wheel.zbias = cmrcar.mesh_radius(wheel), bmesh.zbias
    s.meshes.append(wheel)
    s.nodes.append(cmrcar.Node(id=0))
    for wx, wy, wz, _, ww in gt2["wheels"]:   # GT2's x is the wheel's outer face
        centre = (wx - math.copysign(ww / 2, wx), wy, wz)
        s.nodes.append(cmrcar.Node(id=len(s.nodes), mesh=0, parent=0, base=place(*to_game(centre, dy))))
    s.nodes.append(cmrcar.Node(id=5, parent=0))
    tris = triangles(gt2["lods"][lod], dy, transparent_texels(paint))
    parts = classify([t for t in tris if not t[4]], panels)
    decals = [t for t in tris if t[4]]
    for i, part in enumerate(parts + ([decals] if decals else [])):
        s.meshes.append(panel_mesh(part, lambda p: tex_index(s, *palette_tex(p)), 0 if i == len(parts) else 1))
        s.nodes.append(cmrcar.Node(id=6 + i, mesh=len(s.meshes) - 1, parent=5))
    pts = [p for t in tris for p, _ in t[0]]
    s.meshes[1].extents = tuple(round((max(p[i] for p in pts) - min(p[i] for p in pts)) / 2 * FIX) for i in range(3))
    s.link()
    return s, dy


def hull(gt2, base_hull, dy):
    """Chunk 1: the shine shell, GT2's least detailed model shrunk inside the body."""
    s = copy.deepcopy(base_hull)
    tris = triangles(gt2["lods"][-1], dy, lambda f: False)
    pts = [p for t in tris for p, _ in t[0]]
    mid = [(max(p[i] for p in pts) + min(p[i] for p in pts)) / 2 for i in range(3)]
    # the base hull's node maps local axes onto the car's; undo that so ours sits in car axes
    s.nodes[0].base = s.nodes[0].local = place(0, 0, 0)
    verts, index, faces = [], {}, []
    for corners, *_ in tris:
        ids = []
        for pos, _ in corners:
            p = [m + (c - m) * 0.9 for c, m in zip(pos, mid)]
            v = vertex(p, norm(p))
            ids.append(index.setdefault(v.pack(), len(verts)))
            if ids[-1] == len(verts):
                verts.append(v)
        faces.append(cmrcar.FaceGT3(ids, [(0, 0)] * 3, 0))
    lod = s.meshes[0].lods[0]
    lod.faces, lod.verts = cmrcar.FaceBlock([], faces), cmrcar.VertBlock(verts)
    s.meshes[0].radius = cmrcar.mesh_radius(s.meshes[0])
    return s


def dummies(clut_y):
    """The first 12 texture slots belong to the mud system; ours are small unused squares."""
    return [cmrcar.TIM.make_4bpp(832, 246, [0] * 16 * 8, 16, 832, clut_y, [0x8000] * 16) for _ in range(12)]


def repaint_cockpit(tims, rgb):
    """The base car's cockpit shows its bonnet through the windscreen, in its own paint: the
    commonest strong hue of its textures becomes rgb."""
    hsv = lambda c: colorsys.rgb_to_hsv(*(ch / 255 for ch in rgb_of(c)))   # noqa: E731
    hues = collections.Counter(int(hsv(c)[0] * 12) for t in tims for c in t.clut[4]
                               if c & 0x7fff and hsv(c)[1] > 0.4 and hsv(c)[2] > 0.15)
    if not hues:
        return
    paint = hues.most_common(1)[0][0]
    match = lambda c: c & 0x7fff and hsv(c)[1] > 0.3 and min((int(hsv(c)[0] * 12) - paint) % 12,   # noqa: E731
                                                              (paint - int(hsv(c)[0] * 12)) % 12) <= 1
    top = max(hsv(c)[2] for t in tims for c in t.clut[4] if match(c))
    for t in tims:
        x, y, w, h, clut = t.clut
        t.clut = (x, y, w, h, [cmrcar.to15(*(min(255, round(ch * hsv(c)[2] / top)) for ch in rgb), stp=c & 0x8000)
                               if match(c) else c for c in clut])


def car_files(gt2, paint, base, kind, rim):
    """DAY or NIGHT model and textures. GT2's texture fills page 14 with its 16 palettes in
    the CLUT column; the base car's wheels and in-car parts get their textures copied into
    page 13."""
    model, tims = base[kind + ".BFX"], base[kind + "TIM.BFX"]
    src = cmrcar.VRAM()
    src.load_texfile(tims)
    rows = iter(range(32, 60, 2))
    texels = Texels(src, (848, 0, 896, 255), [(832, next(rows)) for _ in range(12)])
    chunk0, dy = exterior(gt2, paint, model.scenes[0], texels, lambda p: (PAGE14, clut_word(832, 2 * p)), 11, 1.0, 0, rim)
    scenes = [chunk0, hull(gt2, model.scenes[1], dy)]
    for i, s in enumerate(model.scenes[2:]):
        s = copy.deepcopy(s)
        faces = [f for me in s.meshes for l in me.lods if l.faces for f in l.faces.gt3]
        first = len(texels.tims)
        texels.remap(s, faces)
        if i == 0:
            repaint_cockpit(texels.tims[first:], paint["colour"])
        scenes.append(s)
    pal = paint["palettes"]
    image = cmrcar.TIM.make_4bpp(896, 0, paint["pixels"], 256, 832, 0, pal[0])
    out = dummies(next(rows)) + [image]
    out += [cmrcar.TIM.make_4bpp(847, 254, [0] * 4, 4, 832, 2 * k, pal[k]) for k in range(1, 16)]
    return cmrcar.ModelFile(scenes), cmrcar.TextureFile(out + texels.tims)


def medium_files(gt2, paint, base, rim):
    """MEDIUM model and its 64x255 VRAM block at (832, 0): GT2's texture, its palettes four to a
    row underneath, and the base car's wheel textures shrunk into the rows left."""
    model = base["MEDIUM.BFX"]
    src = cmrcar.VRAM()
    src.load_texfile(base["DAYTIM.BFX"])
    slots = [(832 + 16 * (k % 4), 224 + 2 * (k // 4)) for k in range(20)]
    texels = Texels(src, (832, 234, 896, 255), slots[16:])
    day = base["DAY.BFX"].scenes[0]
    chunk0, dy = exterior(gt2, paint, day, texels, lambda p: (PAGE13, clut_word(*slots[p])), 4, 20 / 64, 1, rim)
    scenes = [chunk0, hull(gt2, model.scenes[1], dy)]
    block = cmrcar.VRAM()
    for k in range(16):
        x, y = slots[k]
        block.mem[y * 1024 + x:y * 1024 + x + 16] = paint["palettes"][k]
    px = paint["pixels"]
    for r in range(224):
        for c in range(64):
            block.mem[r * 1024 + 832 + c] = px[r * 256 + 4 * c] | px[r * 256 + 4 * c + 1] << 4 | \
                px[r * 256 + 4 * c + 2] << 8 | px[r * 256 + 4 * c + 3] << 12
    for t in texels.tims:
        block.upload(t)
    data = b"".join(struct.pack("<64H", *block.mem[r * 1024 + 832:r * 1024 + 896]) for r in range(255))
    return cmrcar.ModelFile(scenes), cmrcar.TIM(2, None, (832, 0, 64, 255, data))


def write_car(out, base, day, night, paint, night_paint, rim):
    files = {}
    for kind, model, p in (("DAY", day, paint), ("NIGHT", night, night_paint)):
        files[kind + ".BFX"], files[kind + "TIM.BFX"] = car_files(model, p, base, kind, rim)
    files["MEDIUM.BFX"], files["MEDIUM.TIM"] = medium_files(day, paint, base, rim)
    for kind in ("DAY", "NIGHT", "MEDIUM"):
        tex = files["MEDIUM.TIM" if kind == "MEDIUM" else kind + "TIM.BFX"]
        errors, _ = cmrcar.validate(files[kind + ".BFX"], tex, kind)
        if errors:
            sys.exit(f"{kind}: " + "; ".join(errors[:5]))
    for name, obj in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(obj.serialize())


def base_files(cue, car):
    from build_game import data_track, read_file
    names = ("DAY.BFX", "NIGHT.BFX", "MEDIUM.BFX", "DAYTIM.BFX", "NIGHTTIM.BFX")
    with open(data_track(cue), "rb") as img:
        return {n: cmrcar.load_bytes(n, read_file(img, f"CMRALLY/CARS/{car}/{n}")) for n in names}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc")
    ap.add_argument("car", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--list", nargs="?", const="", metavar="TEXT", help="list car ids (whose names contain TEXT)")
    ap.add_argument("--colour", type=int, default=0, help="paint colour number (default 0)")
    ap.add_argument("--name", help="name in the car menu (default: GT2's name)")
    ap.add_argument("--short-name", help="short name (default: the name without the maker)")
    ap.add_argument("--base", default="SUBARU", help="disc car for handling, sounds and records (default SUBARU)")
    ap.add_argument("--bhp", type=int, default=0)
    ap.add_argument("--wheelbase", type=int, default=0, help="mm (default: from the model)")
    ap.add_argument("--drive", default="4WD", choices=["4WD", "FWD", "RWD"])
    ap.add_argument("--mass", type=int, default=0, help="kg (default: the game's 1120)")
    ap.add_argument("--paint", help="paint colour as RRGGBB (default: GT2's)")
    ap.add_argument("--rim", type=float, help="rim size in inches (default: as the base car's wheels look)")
    ap.add_argument("--cmr", help="Colin McRae Rally .cue for the base car's files "
                    "(default: the one build_game.py set up)")
    a = ap.parse_args()

    vol = Vol(Disc(a.disc))
    cars = car_list(vol)
    if a.list is not None:
        for code, (name, _) in sorted(cars.items(), key=lambda kv: kv[1][0]):
            if a.list.lower() in name.lower():
                print(code, name)
        return 0
    if not a.car or not a.out:
        ap.error("car id and out dir are needed")
    objs = vol.dir("carobj")
    if f"{a.car}.cdo.gz" not in objs:
        sys.exit(f"no car {a.car} on this disc (try --list)")
    day = parse_model(vol.file(objs, f"{a.car}.cdo.gz"))
    night = parse_model(vol.file(objs, f"{a.car}.cno.gz"))
    paint = parse_paint(vol.file(objs, f"{a.car}.cdp.gz"), a.colour)
    night_paint = parse_paint(vol.file(objs, f"{a.car}.cnp.gz"), a.colour)
    swatches = cars[a.car][1]
    swatch = swatches[min(a.colour, len(swatches) - 1)]
    rgb = rgb_of(swatch)
    if a.paint:
        rgb = tuple(int(a.paint.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
        used = {f["palette"] for lod in day["lods"] for f in lod["faces"] if f["uvs"]}
        for p in (paint, night_paint):
            p["palettes"] = repaint(p["palettes"], rgb, used, swatch)
    paint["colour"] = night_paint["colour"] = rgb
    cue = a.cmr
    if not cue:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "disc_path.txt")
        cue = open(path).read().strip() if os.path.exists(path) else None
    if not cue:
        sys.exit("give the Colin McRae Rally .cue with --cmr")
    base = base_files(cue, a.base.upper())

    name = a.name or cars[a.car][0].rsplit(" '", 1)[0]
    short = a.short_name or (name.split(" ", 1)[1] if " " in name else name)
    w = day["wheels"]
    wheelbase = a.wheelbase or round(abs(w[0][2] - w[2][2]) * 1000)
    os.makedirs(a.out, exist_ok=True)
    write_car(a.out, base, day, night, paint, night_paint, a.rim)
    with open(os.path.join(a.out, "car.ini"), "w") as f:
        f.write(f"# converted from Gran Turismo 2 ({a.car}) by tools/gt2car.py\n"
                f"name = {name}\nshort_name = {short}\nbase = {a.base}\n"
                f"bhp = {a.bhp}\nwheelbase = {wheelbase}\ndrive = {a.drive}\n"
                + (f"mass = {a.mass}\n" if a.mass else ""))
    print(f"{name}: {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
