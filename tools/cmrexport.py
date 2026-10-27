#!/usr/bin/env python3
"""Export a Colin McRae Rally car model to OBJ/MTL/PNG and render previews.

Usage:
  python3 -I cmrexport.py CAR_DIR OUT_DIR [DAY|NIGHT|MEDIUM]

Writes (prefix = <car>_<model>):
  <prefix>.obj / .mtl          exterior car (chunk 0, damage state 0, wheels placed)
  tex/<prefix>_p<tpage>_c<clut>.png   one 256x256 RGBA page per (tpage, clut) material
  <prefix>_vram.png            the car's VRAM area (x 832..959, y 0..255) decoded,
                               every texture with its own CLUT, CLUT rows shown in place
  <prefix>_preview.png         software-rendered 3/4 front view
  <prefix>_views.png           4 more views (left, right, rear 3/4, top)
  <prefix>_panels.png          each body panel (id 6, 7, ...) highlighted, front-left and rear-right
  <prefix>_incar.png           chunks 1..7 (shine hull, cockpit, steering wheel, arms, wipers)

Coordinates: game (x fwd, y up, z LEFT, 1/32 m) -> OBJ (X=x, Y=y, Z=-z, metres),
which is right-handed with Z pointing to the car's right; triangle order is
reversed so OBJ faces are counter-clockwise seen from outside.
Wheels with z < 0 (ids 2, 4) get the 180-degree turn about y the game applies at
low speed (FUN_80050000), so the sharp hub side faces outward on all four."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmrcar  # noqa: E402
from cmrcar import rgb15, write_png  # noqa: E402

CAR_VRAM_X, CAR_VRAM_W, CAR_VRAM_H = 832, 128, 256


# ----------------------------------------------------------------------------- scene -> triangles
def place(scene, flip_right_wheels=True):
    """Yield (node_index, mesh, lod0, R, t) with world (car) transforms in game units (1/32 m)."""
    for i, n in enumerate(scene.nodes):
        if n.mesh < 0:
            continue
        m = scene.meshes[n.mesh]
        lod = m.lods[m.cur]
        if not lod.used:
            continue
        R, t = scene.world_matrix(i)
        t = [x * 32.0 for x in t]
        if flip_right_wheels and (n.id & 0xFFF) in (2, 4):
            R = [[-R[0][0], R[0][1], -R[0][2]], [-R[1][0], R[1][1], -R[1][2]], [-R[2][0], R[2][1], -R[2][2]]]
        yield i, m, lod, R, t


def xf(R, t, x, y, z):
    return (R[0][0] * x + R[0][1] * y + R[0][2] * z + t[0],
            R[1][0] * x + R[1][1] * y + R[1][2] * z + t[1],
            R[2][0] * x + R[2][1] * y + R[2][2] * z + t[2])


def triangles(scene, flip_right_wheels=True):
    """Flatten a scene to triangles in OBJ space (metres, right-handed, CCW front).

    Each tri: dict(p=[3 xyz], n=[3 xyz], uv=[3 (u,v)] or None, tex=(tpage,clut) or None,
    rgb=(r,g,b) for G3, semi=bool, unlit=bool, brightness)."""
    out = []
    for i, m, lod, R, t in place(scene, flip_right_wheels):
        V = lod.verts.verts
        P, N = [], []
        for v in V:
            x, y, z = xf(R, t, v.x, v.y, v.z)
            P.append((x / 32.0, y / 32.0, -z / 32.0))
            nx, ny, nz = xf(R, [0, 0, 0], v.nx / 4096.0, v.ny / 4096.0, v.nz / 4096.0)
            N.append((nx, ny, -nz))
        semi = bool(lod.flags & cmrcar.LOD_SEMITRANS)
        unlit = bool(lod.flags & cmrcar.LOD_UNLIT)
        for f in lod.faces.g3:
            a, b, c = f.v
            out.append(dict(p=[P[a], P[c], P[b]], n=[N[a], N[c], N[b]], uv=None, tex=None,
                            rgb=f.rgb, semi=semi or (f.c0 >> 24) == 0x32, unlit=unlit,
                            br=lod.brightness, node=i))
        for f in lod.faces.gt3:
            a, b, c = f.v
            out.append(dict(p=[P[a], P[c], P[b]], n=[N[a], N[c], N[b]],
                            uv=[f.uv[0], f.uv[2], f.uv[1]], tex=scene.tex[f.tex], rgb=None,
                            semi=semi, unlit=unlit, br=lod.brightness, node=i))
    return out


# ----------------------------------------------------------------------------- textures
def page_png(vram, tpage, clut, path):
    rows = []
    for v in range(256):
        r = bytearray()
        for u in range(256):
            c = vram.texel(tpage, clut, u, v)
            r += bytes(rgb15(c) + (255,)) if c is not None else b"\0\0\0\0"
        rows.append(r)
    write_png(path, 256, 256, rows, alpha=True)


def vram_png(texfile, path, scale=2):
    """The car's VRAM area as the GPU would sample it: each texture decoded with its own
    CLUT (4bpp -> 4 texels per halfword), CLUT rows drawn as 4-texel-wide colour cells."""
    W, H = CAR_VRAM_W * 4, CAR_VRAM_H
    img = [[(40, 40, 48, 255)] * W for _ in range(H)]
    vr = cmrcar.VRAM()
    vr.load_texfile(texfile)
    for slot, t in enumerate(texfile.tims):
        if t is None:
            continue
        x, y, w, h, _ = t.image
        cx, cy = t.clut[0], t.clut[1]
        tp = (x // 64) | ((y // 256) << 4)
        cw = (cx >> 4) | (cy << 6)
        for yy in range(y, y + h):
            for xx in range(x * 4, (x + w) * 4):
                u = xx - (x // 64) * 256
                c = vr.texel(tp, cw, u, yy & 255)
                img[yy][xx - CAR_VRAM_X * 4] = rgb15(c) + (255,) if c is not None else (255, 0, 255, 255)
        for k in range(t.clut[2]):
            c = vr.mem[cy * 1024 + cx + k]
            for j in range(4):
                img[cy][(cx + k - CAR_VRAM_X) * 4 + j] = rgb15(c) + (255,)
    rows = []
    for y in range(H):
        r = bytearray()
        for x in range(W):
            r += bytes(img[y][x]) * scale
        for _ in range(scale):
            rows.append(r)
    write_png(path, W * scale, H * scale, rows, alpha=True)


# ----------------------------------------------------------------------------- OBJ
def export_obj(scene, vram, outdir, prefix):
    tris = triangles(scene)
    os.makedirs(os.path.join(outdir, "tex"), exist_ok=True)
    mats = {}
    for tr in tris:
        if tr["tex"] is not None:
            key = "p%02x_c%04x" % tr["tex"]
            if key not in mats:
                png = "tex/%s_%s.png" % (prefix, key)
                page_png(vram, tr["tex"][0], tr["tex"][1], os.path.join(outdir, png))
                mats[key] = ("tex", png)
        else:
            key = "rgb_%02x%02x%02x" % tr["rgb"]
            mats.setdefault(key, ("rgb", tr["rgb"]))
        tr["mat"] = key
    with open(os.path.join(outdir, prefix + ".mtl"), "w") as f:
        for k, (kind, val) in sorted(mats.items()):
            f.write("newmtl %s\nKa 0 0 0\nKs 0 0 0\nillum 1\n" % k)
            if kind == "tex":
                f.write("Kd 1 1 1\nmap_Kd %s\nmap_d %s\n\n" % (val, val))
            else:
                # PSX modulation: 0x80 = 1.0, so G3 colours are doubled for display
                f.write("Kd %.4f %.4f %.4f\n\n" % tuple(min(1.0, c / 128.0) for c in val))
    with open(os.path.join(outdir, prefix + ".obj"), "w") as f:
        f.write("# Colin McRae Rally car %s, exported by cmrexport.py\n" % prefix)
        f.write("# X forward, Y up, Z right (metres); game z = -Z\nmtllib %s.mtl\n" % prefix)
        vi = vti = 1
        cur_obj = None
        for tr in sorted(tris, key=lambda t: (t["node"], t["mat"])):
            if tr["node"] != cur_obj:
                cur_obj = tr["node"]
                n = scene.nodes[cur_obj]
                f.write("o node%02d_id%03x\n" % (cur_obj, n.id & 0xFFF))
                cur_mat = None
            if tr["mat"] != cur_mat:
                cur_mat = tr["mat"]
                f.write("usemtl %s\n" % cur_mat)
            for p in tr["p"]:
                f.write("v %.5f %.5f %.5f\n" % p)
            for nn in tr["n"]:
                f.write("vn %.4f %.4f %.4f\n" % nn)
            if tr["uv"]:
                for u, v in tr["uv"]:
                    f.write("vt %.6f %.6f\n" % (u / 256.0, 1.0 - v / 256.0))
                f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (vi, vti, vi, vi + 1, vti + 1, vi + 1,
                                                           vi + 2, vti + 2, vi + 2))
                vti += 3
            else:
                f.write("f %d//%d %d//%d %d//%d\n" % (vi, vi, vi + 1, vi + 1, vi + 2, vi + 2))
            vi += 3
    return len(tris), len(mats)


# ----------------------------------------------------------------------------- rasterizer
def _norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / l for c in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


class Canvas:
    def __init__(self, w, h, bg=(70, 80, 95)):
        self.w, self.h = w, h
        self.col = [list(bg) * w for _ in range(h)]
        self.z = [[1e30] * w for _ in range(h)]

    def tri(self, s, zinv, attr, shade):
        """s: 3 screen (x,y); zinv: 3 1/z; attr: 3 tuples interpolated perspective-correctly;
        shade(attr_tuple) -> (r,g,b) or None (transparent)."""
        (x0, y0), (x1, y1), (x2, y2) = s
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            return
        minx, maxx = max(0, int(min(x0, x1, x2))), min(self.w - 1, int(max(x0, x1, x2)) + 1)
        miny, maxy = max(0, int(min(y0, y1, y2))), min(self.h - 1, int(max(y0, y1, y2)) + 1)
        na = len(attr[0])
        A = [[a * zi for a in at] for at, zi in zip(attr, zinv)]
        for py in range(miny, maxy + 1):
            fy = py + 0.5
            zr, cr = self.z[py], self.col[py]
            for px in range(minx, maxx + 1):
                fx = px + 0.5
                w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                w2 = 1.0 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                zi = w0 * zinv[0] + w1 * zinv[1] + w2 * zinv[2]
                z = 1.0 / zi
                if z >= zr[px]:
                    continue
                at = tuple((w0 * A[0][k] + w1 * A[1][k] + w2 * A[2][k]) * z for k in range(na))
                c = shade(at)
                if c is None:
                    continue
                zr[px] = z
                cr[3 * px:3 * px + 3] = c


def render(tris, vram, path, eye, target, w=960, h=640, fov=32.0, ss=2, light=(0.4, 0.8, 0.45)):
    W, H = w * ss, h * ss
    cv = Canvas(W, H)
    f = _norm([target[i] - eye[i] for i in range(3)])
    r = _norm(_cross(f, [0, 1, 0]))
    u = _cross(r, f)
    foc = (H / 2) / math.tan(math.radians(fov) / 2)
    L = _norm(light)

    def proj(p):
        d = [p[i] - eye[i] for i in range(3)]
        cx, cy, cz = sum(d[i] * r[i] for i in range(3)), sum(d[i] * u[i] for i in range(3)), sum(d[i] * f[i] for i in range(3))
        return (W / 2 + foc * cx / cz, H / 2 - foc * cy / cz), cz

    order = sorted(tris, key=lambda t: t["semi"])     # semi-transparent last
    for tr in order:
        sp, zs = zip(*[proj(p) for p in tr["p"]])
        if min(zs) <= 0.05:
            continue
        (x0, y0), (x1, y1), (x2, y2) = sp
        if (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) >= 0:     # CCW in OBJ space -> CW on y-down screen
            continue
        lit = [0.45 + 0.75 * max(0.0, sum(n[i] * L[i] for i in range(3))) for n in tr["n"]]
        if tr["unlit"]:
            lit = [tr["br"] / 128.0] * 3
        if tr["tex"] is not None:
            tp, cl = tr["tex"]
            attr = [(tr["uv"][k][0], tr["uv"][k][1], lit[k]) for k in range(3)]

            def shade(a, tp=tp, cl=cl, semi=tr["semi"]):
                c = vram.texel(tp, cl, min(255, max(0, int(a[0]))), min(255, max(0, int(a[1]))))
                if c is None:
                    return None
                rr, gg, bb = rgb15(c)
                k = a[2]
                return [min(255, int(rr * k)), min(255, int(gg * k)), min(255, int(bb * k))]
        else:
            base = [min(255, 2 * c) for c in tr["rgb"]]
            attr = [(lit[k],) for k in range(3)]

            def shade(a, base=base):
                return [min(255, int(c * a[0])) for c in base]
        cv.tri(sp, [1.0 / z for z in zs], attr, shade)
    # downsample
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            acc = [0, 0, 0]
            for yy in range(ss):
                src = cv.col[y * ss + yy]
                for xx in range(ss):
                    k = 3 * (x * ss + xx)
                    acc[0] += src[k]
                    acc[1] += src[k + 1]
                    acc[2] += src[k + 2]
            row += bytes(c // (ss * ss) for c in acc)
        rows.append(row)
    if path:
        write_png(path, w, h, rows)
    return rows


def tile(images, cols, path):
    """images: list of (w, h, rows RGB); same size; tiled cols wide."""
    w, h = images[0][0], images[0][1]
    rows_n = (len(images) + cols - 1) // cols
    out = []
    for ry in range(rows_n):
        for y in range(h):
            r = bytearray()
            for cx in range(cols):
                k = ry * cols + cx
                r += images[k][2][y] if k < len(images) else bytes(3 * w)
            out.append(r)
    write_png(path, w * cols, h * rows_n, out)


def bbox(tris):
    pts = [p for t in tris for p in t["p"]]
    return [min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]


def main():
    car_dir, outdir = sys.argv[1], sys.argv[2]
    kind = (sys.argv[3] if len(sys.argv) > 3 else "DAY").upper()
    car = os.path.basename(os.path.normpath(car_dir))
    prefix = "%s_%s" % (car.lower(), kind.lower())
    os.makedirs(outdir, exist_ok=True)
    model = cmrcar.load(os.path.join(car_dir, kind + ".BFX"))
    vram = cmrcar.VRAM()
    if kind == "MEDIUM":
        vram.upload(cmrcar.load(os.path.join(car_dir, "MEDIUM.TIM")))
        texfile = None
    else:
        texfile = cmrcar.load(os.path.join(car_dir, kind + "TIM.BFX"))
        vram.load_texfile(texfile)
        vram_png(texfile, os.path.join(outdir, prefix + "_vram.png"))
    ext = model.scenes[0]
    nt, nm = export_obj(ext, vram, outdir, prefix)
    tris = triangles(ext)
    lo, hi = bbox(tris)
    print("%s: %d triangles, %d materials; bbox X %.3f..%.3f Y %.3f..%.3f Z %.3f..%.3f m"
          % (prefix, nt, nm, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    print("  length %.3f m, height %.3f m, width %.3f m" % (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
    tgt = [(lo[i] + hi[i]) / 2 for i in range(3)]
    # 3/4 front view from the car's front-left (game +x, +z  ->  OBJ +X, -Z)
    render(tris, vram, os.path.join(outdir, prefix + "_preview.png"),
           eye=[6.2, 2.6, -4.6], target=tgt)
    views = [([0.0, 0.9, -8.5], "left side"), ([0.0, 0.9, 8.5], "right side"),
             ([-6.2, 2.4, 4.6], "rear 3/4 right"), ([0.01, 9.5, 0.0], "top")]
    imgs = [(480, 320, render(tris, vram, None, eye=e, target=tgt, w=480, h=320)) for e, _ in views]
    tile(imgs, 2, os.path.join(outdir, prefix + "_views.png"))
    # one tile per body panel (ids 6..): that panel textured, the rest of the car dark
    imgs = []
    for i, n in enumerate(ext.nodes):
        if n.mesh < 0 or (n.id & 0xFFF) < 6:
            continue
        for eye in ([5.0, 3.2, -4.2], [-5.0, 2.8, 3.8]):
            hl = []
            for t in tris:
                t = dict(t)
                if t["node"] != i:
                    t.update(tex=None, rgb=(40, 40, 40))
                elif t["tex"] is None:
                    t.update(rgb=(150, 40, 40))
                hl.append(t)
            imgs.append((320, 220, render(hl, vram, None, eye=eye, target=tgt, w=320, h=220)))
    tile(imgs, 4, os.path.join(outdir, prefix + "_panels.png"))
    if kind != "MEDIUM":
        imgs = []
        for ci in range(1, len(model.scenes)):
            t2 = triangles(model.scenes[ci])
            if ci == 1:     # env-map hull: texture is assigned at run time, show it flat
                for tr in t2:
                    tr.update(tex=None, rgb=(70, 90, 110), unlit=False)
            lo2, hi2 = bbox(t2)
            c2 = [(lo2[i] + hi2[i]) / 2 for i in range(3)]
            size = max(hi2[i] - lo2[i] for i in range(3))
            eye = [c2[0] + 1.3 * size, c2[1] + 0.8 * size, c2[2] - 1.4 * size]
            imgs.append((320, 240, render(t2, vram, None, eye=eye, target=c2, w=320, h=240, fov=40)))
        tile(imgs, 4, os.path.join(outdir, prefix + "_incar.png"))


if __name__ == "__main__":
    main()
