#!/usr/bin/env python3
"""Round-trip every car model and texture file: parse -> serialize -> compare bytes.

Usage: python3 -I cmrcar_test.py [CARS_DIR]   (default: game/extracted/CMRALLY/CARS)

The model writer regenerates the vertex/face blobs and all offsets from the
parsed objects, so byte identity here also exercises the path used for new
models.  A second pass re-parses the output and also checks a few structural
invariants the game relies on (see docs/car-files.md)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmrcar  # noqa: E402

CARS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                          "game", "extracted", "CMRALLY", "CARS")
FILES = ["DAY.BFX", "NIGHT.BFX", "MEDIUM.BFX", "DAYTIM.BFX", "NIGHTTIM.BFX", "MEDIUM.TIM"]


def invariants(name, obj):
    """Return a list of violated invariants (empty = fine)."""
    bad = []
    if isinstance(obj, cmrcar.ModelFile):
        want = 2 if name == "MEDIUM.BFX" else 8
        if len(obj.scenes) != want:
            bad.append("chunk count %d != %d" % (len(obj.scenes), want))
        for ci, s in enumerate(obj.scenes):
            for n in s.nodes:
                if n.w0 != 0:
                    bad.append("node +0 != 0")
                if n.world != list(cmrcar.Node.IDENT):
                    bad.append("world matrix not identity")
                if n.base != n.local:
                    bad.append("base != local")
                if n.type != (0 if n.mesh >= 0 else 3):
                    bad.append("type/mesh mismatch")
            for m in s.meshes:
                for l in m.lods:
                    if not l.used:
                        continue
                    nv = len(l.verts.verts)
                    if nv > cmrcar.MAX_VERTS_PER_BLOCK:
                        bad.append("too many verts")
                    if l.flags & cmrcar.LOD_UNLIT and l.faces.g3:
                        bad.append("G3 faces in unlit LOD")
                    for f in l.faces.g3:
                        if f.c0 >> 24 not in (0x30, 0x32):
                            bad.append("G3 code %02x" % (f.c0 >> 24))
                    for f in l.faces.gt3:
                        if f.tex >= len(s.tex):
                            bad.append("tex index out of range")
                if m.nlod == 3:
                    a, b, c = m.lods
                    if a.verts.pack() != c.verts.pack() or a.faces.pack() != c.faces.pack():
                        bad.append("damage state 0 != 2")
                    if len(a.verts.verts) != len(b.verts.verts):
                        bad.append("damage state vertex counts differ")
            if ci == 0:
                body = s.find(5)
                for wid in (1, 2, 3, 4):
                    if s.find(wid) < 0:
                        bad.append("missing wheel id %d" % wid)
                if body < 0 or s.find(6) < 0:
                    bad.append("missing id 5/6")
                else:
                    ids = sorted(s.nodes[k].id & 0xFFF for k in s.children(body))
                    if ids != list(range(6, 6 + len(ids))):
                        bad.append("panel ids not contiguous from 6: %s" % ids)
    elif isinstance(obj, cmrcar.TextureFile):
        for i, t in enumerate(obj.tims):
            if t is None:
                continue
            if t._lens and t._lens != [12 + 2 * len(t.clut[4])] * bool(t.clut) + [12 + len(t.image[4])]:
                bad.append("TIM block length field unusual in slot %d" % i)
            if len(t.tail) != (len(t.image[4]) + (2 * len(t.clut[4]) if t.clut else 0)) % 4:
                bad.append("TIM padding unusual in slot %d" % i)
            if t.flags != 8:
                bad.append("slot %d not 4bpp+CLUT" % i)
            if t.clut and t.clut[1] % 2:
                bad.append("slot %d CLUT on odd row" % i)
    return bad


def main():
    if not os.path.isdir(CARS):
        print("no extracted disc files (run tools/build_game.py); skipped")
        return 77
    total = ok = 0
    sizes = {}
    validated = [0, 0]
    for car in sorted(os.listdir(CARS)):
        d = os.path.join(CARS, car)
        if not os.path.isdir(d):
            continue
        for fn in FILES:
            p = os.path.join(d, fn)
            data = open(p, "rb").read()
            obj = cmrcar.load(p)
            out = obj.serialize()
            total += 1
            same = out == data
            again = cmrcar.load_bytes(fn, out).serialize() == data if same else False
            bad = invariants(fn, obj)
            if isinstance(obj, cmrcar.ModelFile):
                kind = fn.split(".")[0]
                tex = cmrcar.load(os.path.join(d, "MEDIUM.TIM" if kind == "MEDIUM" else kind + "TIM.BFX"))
                errs, warns = cmrcar.validate(obj, tex, kind)
                bad += ["VALIDATOR ERROR: " + e for e in errs]
                relinked = cmrcar.load(p)
                for sc in relinked.scenes:
                    sc.link()
                if relinked.serialize() != data:
                    bad.append("Scene.link() does not reproduce the stored tree pointers")
                validated[0] += 1
                validated[1] += len(errs)
            status = "OK  " if same and again else "FAIL"
            if same and again:
                ok += 1
            sizes.setdefault(fn, []).append(len(data))
            print("%s %-9s %-12s %6d bytes%s" % (status, car, fn, len(data),
                                                ("   notes: " + "; ".join(sorted(set(bad)))) if bad else ""))
    print()
    print("%d/%d files byte-identical after parse -> serialize (and re-parse -> serialize)" % (ok, total))
    print("%d model files validated together with their textures: %d validator errors; "
          "Scene.link() rebuilt every node tree identically" % tuple(validated))
    for fn, s in sizes.items():
        print("  %-12s size range %d .. %d" % (fn, min(s), max(s)))
    return 0 if ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
