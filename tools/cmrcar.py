#!/usr/bin/env python3
"""Colin McRae Rally (PS1, NTSC-U SCUS-94474) car file parser / writer.

Handles every car file in CMRALLY/CARS/<CAR>/ except SOUNDS.BFX:

    DAY.BFX, NIGHT.BFX, MEDIUM.BFX   -> ModelFile   (chunk container of Scene)
    DAYTIM.BFX, NIGHTTIM.BFX         -> TextureFile (chunk container of TIM)
    MEDIUM.TIM                       -> TIM         (one 16bpp VRAM block)

Stdlib only.  See docs/car-files.md for the byte layout and for which game function reads
every field.  Coordinates are the game's own: +x forward, +y up, +z = the car's
LEFT (a left-handed frame), vertex unit 1/32 m, node transforms 16.16 metres.

The writer always rebuilds the vertex and face blobs from the structured data
(sharing a block only where the same Python object is referenced twice, e.g.
the four wheels), so the code path that round-trips the original files
byte-for-byte is the same one that writes new models.
"""
import struct
import zlib

__all__ = [
    "Vertex", "FaceG3", "FaceGT3", "FaceBlock", "VertBlock", "Lod", "Mesh",
    "Node", "Scene", "ModelFile", "TIM", "TextureFile", "VRAM",
    "load", "save", "write_png",
]

NODE_SIZE = 0xBC
MESH_SIZE = 0x4C
T3_SIZE, T4_SIZE, T5_SIZE = 0x34, 0x18, 0x1C   # track-only record types
VERT_SIZE = 12
G3_SIZE, GT3_SIZE = 16, 12
NULL = 0xFFFFFFFF
MAX_VERTS_PER_BLOCK = 342           # 10-bit field holds index*3 -> max index 341

LOD_SEMITRANS = 0x02                # LOD flag: code |= 2 (POLY_GT3 0x36 / POLY_FT3 0x26)
LOD_UNLIT = 0x40                    # LOD flag: FUN_80014e48 (flat FT3, brightness byte) instead of FUN_800147c0


def _s32(v):
    return v - 0x100000000 if v & 0x80000000 else v


# ----------------------------------------------------------------------------- geometry
class Vertex:
    """12-byte vertex: s16 x, y, z, nz, nx, ny (file order!).

    Position in 1/32 m.  Normal is 4.12 fixed point (4096 = 1.0); the game loads
    it into the GTE as VX = s16@+8, VY = s16@+10, VZ = s16@+6 (FUN_800147c0 NCCT,
    FUN_800385b0 damage morph, FUN_8009dc1c env map), i.e. (nx, ny, nz)."""
    __slots__ = ("x", "y", "z", "nx", "ny", "nz")

    def __init__(self, x=0, y=0, z=0, nx=0, ny=0, nz=0):
        self.x, self.y, self.z, self.nx, self.ny, self.nz = x, y, z, nx, ny, nz

    @classmethod
    def unpack(cls, b, o):
        x, y, z, nz, nx, ny = struct.unpack_from("<6h", b, o)
        return cls(x, y, z, nx, ny, nz)

    def pack(self):
        return struct.pack("<6h", self.x, self.y, self.z, self.nz, self.nx, self.ny)

    def __eq__(self, o):
        return isinstance(o, Vertex) and self.pack() == o.pack()

    def __repr__(self):
        return "Vertex(%d,%d,%d n=%d,%d,%d)" % (self.x, self.y, self.z, self.nx, self.ny, self.nz)


def _pack_idx(v, hi):
    for i in v:
        if not 0 <= i < MAX_VERTS_PER_BLOCK:
            raise ValueError("vertex index %d out of range 0..%d" % (i, MAX_VERTS_PER_BLOCK - 1))
    return (v[0] * 3) | ((v[1] * 3) << 10) | ((v[2] * 3) << 20) | ((hi & 3) << 30)


def _unpack_idx(w):
    f = (w & 0x3FF, (w >> 10) & 0x3FF, (w >> 20) & 0x3FF)
    if any(x % 3 for x in f):
        raise ValueError("vertex field not a multiple of 3: %08x" % w)
    return [x // 3 for x in f], w >> 30


class FaceG3:
    """16-byte untextured Gouraud triangle -> POLY_G3 (FUN_800147c0 first loop).

    c0..c2: u32 0xCCBBGGRR.  Only c0 is used by the lit renderer (it is the GTE
    RGBC for NCCT, so its top byte becomes the GPU command: must be 0x30, or 0x32
    for semi-transparent).  c1, c2 are copies of c0 in every original file.
    v: three vertex indices.  Unlit (flag 0x40) LODs must not contain G3 faces."""
    __slots__ = ("c0", "c1", "c2", "v", "hi")

    def __init__(self, v, c0=0x30808080, c1=None, c2=None, hi=0):
        self.v = list(v)
        self.c0 = c0
        self.c1 = c0 if c1 is None else c1
        self.c2 = c0 if c2 is None else c2
        self.hi = hi

    @classmethod
    def unpack(cls, b, o):
        c0, c1, c2, w = struct.unpack_from("<4I", b, o)
        v, hi = _unpack_idx(w)
        return cls(v, c0, c1, c2, hi)

    def pack(self):
        return struct.pack("<4I", self.c0, self.c1, self.c2, _pack_idx(self.v, self.hi))

    @property
    def rgb(self):
        return (self.c0 & 0xFF, (self.c0 >> 8) & 0xFF, (self.c0 >> 16) & 0xFF)


class FaceGT3:
    """12-byte textured Gouraud triangle -> POLY_GT3 (FUN_800147c0) or POLY_FT3 (FUN_80014e48).

    +0 u0 v0 u1 v1 u2 v2 (bytes; texel coords inside the 256x256 texture page)
    +6 u8 tex   index into the Scene's tex table (tpage, clut)
    +7 u8 pad   0 in all files (the renderer reads +6 as s16, so keep it 0)
    +8 u32 vertex indices (3 x 10 bits, each = index*3; bits 30-31 unused = 0)"""
    __slots__ = ("uv", "tex", "pad", "v", "hi")

    def __init__(self, v, uv, tex, pad=0, hi=0):
        self.v = list(v)
        self.uv = [tuple(p) for p in uv]
        self.tex = tex
        self.pad = pad
        self.hi = hi

    @classmethod
    def unpack(cls, b, o):
        u0, v0, u1, v1, u2, v2, tex, pad, w = struct.unpack_from("<8BI", b, o)
        v, hi = _unpack_idx(w)
        return cls(v, [(u0, v0), (u1, v1), (u2, v2)], tex, pad, hi)

    def pack(self):
        (u0, v0), (u1, v1), (u2, v2) = self.uv
        return struct.pack("<8BI", u0, v0, u1, v1, u2, v2, self.tex, self.pad,
                           _pack_idx(self.v, self.hi))


class FaceBlock:
    """The faces of one LOD entry: n_g3 G3 records followed by n_gt3 GT3 records.
    One FaceBlock object referenced from several LODs is written once (shared)."""

    def __init__(self, g3=None, gt3=None):
        self.g3 = g3 or []
        self.gt3 = gt3 or []

    def nbytes(self):
        return G3_SIZE * len(self.g3) + GT3_SIZE * len(self.gt3)

    def pack(self):
        return b"".join(f.pack() for f in self.g3) + b"".join(f.pack() for f in self.gt3)

    def max_index(self):
        m = -1
        for f in self.g3:
            m = max(m, *f.v)
        for f in self.gt3:
            m = max(m, *f.v)
        return m


class VertBlock:
    """Vertex array of one LOD entry (shared like FaceBlock).  The game never stores
    its length: it is max(face index)+1 (FUN_80092af4)."""

    def __init__(self, verts=None):
        self.verts = verts or []

    def pack(self):
        return b"".join(v.pack() for v in self.verts)


class Lod:
    """16-byte entry at mesh+0x18/+0x28/+0x38.

    +0 u16 n_g3  +2 u16 n_gt3  +4 s32 face offset (into face blob, -1 = none)
    +8 s32 vertex offset (into vertex blob, -1 = none)
    +0xC u8 flags (bit1 semi-transparent, bit6 unlit; bit7 used by track code)
    +0xD u8 brightness (unlit gray level; 0x80 in used entries)
    +0xE u8, +0xF u8 pad (0)

    For car body panels the three entries are not distance LODs but damage states:
    [0] = live geometry (drawn; vertices morphed in place by FUN_800385b0, face
    pointer swapped to [1]'s by FUN_80039304), [1] = fully damaged target,
    [2] = pristine reference (copied back into [0] by FUN_80039a94 on repair)."""

    def __init__(self, faces=None, verts=None, flags=0, brightness=0x80, pad=(0, 0),
                 n_g3=None, n_gt3=None):
        self.faces = faces          # FaceBlock or None
        self.verts = verts          # VertBlock or None
        self.flags = flags
        self.brightness = brightness
        self.pad = tuple(pad)
        # counts are normally taken from `faces`; kept for faces=None entries
        self._n_g3 = n_g3
        self._n_gt3 = n_gt3

    @property
    def used(self):
        return self.faces is not None

    def counts(self):
        if self.faces is None:
            return (self._n_g3 or 0, self._n_gt3 or 0)
        return (len(self.faces.g3), len(self.faces.gt3))


class Mesh:
    """0x4C-byte mesh record.

    +0x00 u32 0 (unused by the loader; 0 in all files)
    +0x04 u32 tex-table pointer, written by FUN_8009d344 (0xFFFFFFFF in files)
    +0x08 s32 bounding radius, 16.16 m about the node origin (x1.75 at load,
          frustum test FUN_800b66b4)
    +0x0C s32[3] half extents x,y,z (16.16 m). Only the node with id 6 has them;
          podium-mode shadow box FUN_8007a0e0
    +0x18 Lod[3]
    +0x48 u8 current LOD/state (always 0; FUN_80091ac8 draws lods[cur])
    +0x49 u8 number of LOD entries in use (1 or 3)
    +0x4A s16 ordering-table bias (z sort offset)"""

    def __init__(self, lods=None, radius=0, extents=(0, 0, 0), cur=0, nlod=None, zbias=1,
                 w0=0, w4=NULL):
        self.lods = lods or [Lod(), Lod(None, None, 0, 0), Lod(None, None, 0, 0)]
        while len(self.lods) < 3:
            self.lods.append(Lod(None, None, 0, 0))
        self.radius = radius
        self.extents = tuple(extents)
        self.cur = cur
        self.nlod = nlod if nlod is not None else sum(1 for l in self.lods if l.used)
        self.zbias = zbias
        self.w0 = w0
        self.w4 = w4


class Node:
    """0xBC-byte scene-graph node (the game's own runtime object, loaded in place).

    +0x00 u32 global-list link (must be 0: FUN_8008b2e0 links it in)
    +0x04 next sibling, +0x08 first child, +0x0C parent: offsets from the node
          table start (multiples of 0xBC) or -1
    +0x10 mesh: offset from the mesh table start (multiple of 0x4C) or -1
    +0x14 u32 id; the game uses id & 0xFFF (FUN_8008ad1c)
    +0x18 s32[12] base transform (3x3 row-major 16.16 rotation, 16.16 m translation)
    +0x48 s32[12] local transform (copy of base in the files)
    +0x78 s32[12] world transform (runtime; identity in the files)
    +0xA8 u32 pointer, relocated against the node table if not -1 (always -1)
    +0xAC s16 zone index (-1), +0xAE u16 0, +0xB0 u32 0
    +0xB4 u8 type (0 = mesh node, 3 = group/no mesh)
    +0xB5 u8 per-viewport visibility mask (0xFF)
    +0xB6 u8 0, +0xB7 u8 light scale index (0xFF = full)
    +0xB8 u8 visible-this-frame mask (0), +0xB9 u8 0, +0xBA u16 flags (0)"""

    IDENT = (0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x10000, 0, 0, 0)

    def __init__(self, id=0, mesh=-1, parent=-1, child=-1, next=-1, base=IDENT, local=None,
                 world=IDENT, type=None, w0=0, a8=NULL, ac=0xFFFF, ae=0, b0=0,
                 vismask=0xFF, b6=0, light=0xFF, b8=0, b9=0, flags=0):
        self.id = id
        self.mesh = mesh
        self.parent, self.child, self.next = parent, child, next
        self.base = list(base)
        self.local = list(base if local is None else local)
        self.world = list(world)
        self.type = (0 if mesh >= 0 else 3) if type is None else type
        self.w0, self.a8, self.ac, self.ae, self.b0 = w0, a8, ac, ae, b0
        self.vismask, self.b6, self.light, self.b8, self.b9, self.flags = vismask, b6, light, b8, b9, flags

    @property
    def translation(self):
        return tuple(t / 65536.0 for t in self.base[9:12])


def _rel(v, size, what):
    if v == -1:
        return -1
    if v < 0 or v % size:
        raise ValueError("%s offset %#x not a multiple of %#x" % (what, v, size))
    return v // size


class Scene:
    """One model chunk, as parsed by FUN_8009d344.

    Header (16 bytes):
      u16 n_tex, u16 n_nodes, u16 n_mesh, u16 n_t3, u16 n_t4, u16 n_t5,
      u32 vertex blob size
    then tex[n_tex] (u16 tpage, u16 clut), nodes, meshes, t3/t4/t5 tables,
    vertex blob, face blob (runs to the end of the chunk)."""

    def __init__(self):
        self.tex = []        # list of (tpage, clut)
        self.nodes = []
        self.meshes = []

    # ---- parse
    @classmethod
    def parse(cls, c):
        s = cls()
        n_tex, n_nodes, n_mesh, n3, n4, n5, vsize = struct.unpack_from("<6HI", c, 0)
        if n3 or n4 or n5:
            raise NotImplementedError("track record types 3/4/5 are not used by car files")
        o = 0x10
        s.tex = [struct.unpack_from("<HH", c, o + 4 * i) for i in range(n_tex)]
        o += 4 * n_tex
        node_off = o
        o += NODE_SIZE * n_nodes
        mesh_off = o
        o += MESH_SIZE * n_mesh
        vbase, fbase = o, o + vsize
        if fbase > len(c):
            raise ValueError("vertex blob runs past the chunk")
        for i in range(n_nodes):
            b = node_off + NODE_SIZE * i
            w = struct.unpack_from("<6i", c, b)
            n = Node(id=w[5] & 0xFFFFFFFF, mesh=_rel(w[4], MESH_SIZE, "mesh"),
                     parent=_rel(w[3], NODE_SIZE, "parent"), child=_rel(w[2], NODE_SIZE, "child"),
                     next=_rel(w[1], NODE_SIZE, "next"),
                     base=struct.unpack_from("<12i", c, b + 0x18),
                     local=struct.unpack_from("<12i", c, b + 0x48),
                     world=struct.unpack_from("<12i", c, b + 0x78), w0=w[0] & 0xFFFFFFFF)
            (n.a8, n.ac, n.ae, n.b0, n.type, n.vismask, n.b6, n.light, n.b8, n.b9,
             n.flags) = struct.unpack_from("<IHHI6BH", c, b + 0xA8)
            s.nodes.append(n)
        # vertex/face blocks shared by offset
        fblocks, vblocks, vneed = {}, {}, {}
        raw = []
        for i in range(n_mesh):
            b = mesh_off + MESH_SIZE * i
            w0, w4, radius = struct.unpack_from("<IIi", c, b)
            ext = struct.unpack_from("<3i", c, b + 0xC)
            cur, nlod, zbias = struct.unpack_from("<BBh", c, b + 0x48)
            ents = [struct.unpack_from("<HHii4B", c, b + 0x18 + 0x10 * l) for l in range(3)]
            raw.append((w0, w4, radius, ext, cur, nlod, zbias, ents))
            for ng3, ngt3, fo, vo, *_ in ents:
                if fo != -1 and fo not in fblocks:
                    p = fbase + fo
                    g3 = [FaceG3.unpack(c, p + G3_SIZE * k) for k in range(ng3)]
                    p += G3_SIZE * ng3
                    gt3 = [FaceGT3.unpack(c, p + GT3_SIZE * k) for k in range(ngt3)]
                    if p + GT3_SIZE * ngt3 > len(c):
                        raise ValueError("face block runs past the chunk")
                    fblocks[fo] = FaceBlock(g3, gt3)
                elif fo != -1:
                    fb = fblocks[fo]
                    if (len(fb.g3), len(fb.gt3)) != (ng3, ngt3):
                        raise NotImplementedError("same face offset with different counts")
                if vo != -1:
                    nv = (fblocks[fo].max_index() + 1) if fo != -1 else 0
                    vneed[vo] = max(vneed.get(vo, 0), nv)
        for vo, nv in vneed.items():
            if vbase + vo + VERT_SIZE * nv > fbase:
                raise ValueError("vertex block runs past the vertex blob")
            vblocks[vo] = VertBlock([Vertex.unpack(c, vbase + vo + VERT_SIZE * k) for k in range(nv)])
        for w0, w4, radius, ext, cur, nlod, zbias, ents in raw:
            lods = []
            for ng3, ngt3, fo, vo, fl, br, p0, p1 in ents:
                lods.append(Lod(fblocks.get(fo) if fo != -1 else None,
                                vblocks.get(vo) if vo != -1 else None,
                                fl, br, (p0, p1), ng3 if fo == -1 else None, ngt3 if fo == -1 else None))
            s.meshes.append(Mesh(lods, radius, ext, cur, nlod, zbias, w0, w4))
        s._check_coverage(c, vbase, fbase, fblocks, vblocks)
        return s

    @staticmethod
    def _check_coverage(c, vbase, fbase, fblocks, vblocks):
        """The writer regenerates the blobs, so refuse files with bytes no LOD references."""
        vsize = fbase - vbase
        vlen = sum(VERT_SIZE * len(b.verts) for b in vblocks.values())
        flen = sum(b.nbytes() for b in fblocks.values())
        if vlen != vsize or flen != len(c) - fbase:
            raise ValueError("blob not exactly covered by LOD references (v %d/%d f %d/%d)"
                             % (vlen, vsize, flen, len(c) - fbase))

    # ---- write
    def serialize(self):
        vblob, fblob = bytearray(), bytearray()
        voff, foff = {}, {}
        lod_offsets = []
        for m in self.meshes:
            offs = []
            for l in m.lods:
                fo = vo = -1
                if l.faces is not None:
                    k = id(l.faces)
                    if k not in foff:
                        foff[k] = len(fblob)
                        fblob += l.faces.pack()
                    fo = foff[k]
                if l.verts is not None:
                    k = id(l.verts)
                    if k not in voff:
                        voff[k] = len(vblob)
                        vblob += l.verts.pack()
                    vo = voff[k]
                offs.append((fo, vo))
            lod_offsets.append(offs)
        out = bytearray(struct.pack("<6HI", len(self.tex), len(self.nodes), len(self.meshes),
                                    0, 0, 0, len(vblob)))
        for tp, cl in self.tex:
            out += struct.pack("<HH", tp, cl)
        for n in self.nodes:
            def r(i, size):
                return -1 if i < 0 else i * size
            out += struct.pack("<I4iI", n.w0, r(n.next, NODE_SIZE), r(n.child, NODE_SIZE),
                               r(n.parent, NODE_SIZE), r(n.mesh, MESH_SIZE), n.id)
            out += struct.pack("<12i", *n.base) + struct.pack("<12i", *n.local)
            out += struct.pack("<12i", *n.world)
            out += struct.pack("<IHHI6BH", n.a8, n.ac, n.ae, n.b0, n.type, n.vismask, n.b6,
                               n.light, n.b8, n.b9, n.flags)
        for m, offs in zip(self.meshes, lod_offsets):
            out += struct.pack("<IIi3i", m.w0, m.w4, m.radius, *m.extents)
            for l, (fo, vo) in zip(m.lods, offs):
                ng3, ngt3 = l.counts()
                out += struct.pack("<HHii4B", ng3, ngt3, fo, vo, l.flags, l.brightness, *l.pad)
            out += struct.pack("<BBh", m.cur, m.nlod, m.zbias)
        return bytes(out + vblob + fblob)

    # ---- helpers
    def find(self, nid):
        """Node index by id & 0xFFF (like FUN_8008ad1c), or -1."""
        for i, n in enumerate(self.nodes):
            if (n.id & 0xFFF) == nid:
                return i
        return -1

    def children(self, i):
        out, k = [], self.nodes[i].child
        while k >= 0:
            out.append(k)
            k = self.nodes[k].next
        return out

    def world_matrix(self, i):
        """Base transform of node i composed with its parents (floats: R 3x3, t)."""
        R = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
        t = [0.0, 0.0, 0.0]
        chain = []
        while i >= 0:
            chain.append(i)
            i = self.nodes[i].parent
        for k in reversed(chain):
            b = self.nodes[k].base
            Rl = [[b[3 * r + c] / 65536.0 for c in range(3)] for r in range(3)]
            tl = [b[9 + j] / 65536.0 for j in range(3)]
            # p_parent = Rl * p + tl ; world = R * p_parent + t
            t = [t[r] + sum(R[r][c] * tl[c] for c in range(3)) for r in range(3)]
            R = [[sum(R[r][k2] * Rl[k2][c] for k2 in range(3)) for c in range(3)] for r in range(3)]
        return R, t

    def link(self):
        """Recompute next/child pointers from the parent fields (children keep list order,
        written so that the first child is the LAST node listing that parent, matching
        how the original tool chained them: child -> highest index, next -> lower)."""
        for n in self.nodes:
            n.child = n.next = -1
        last = {}
        for i, n in enumerate(self.nodes):
            if n.parent >= 0:
                n.next = last.get(n.parent, -1)
                last[n.parent] = i
        for p, i in last.items():
            self.nodes[p].child = i


# ----------------------------------------------------------------------------- containers
def _chunks(data):
    o, out = 0, []
    while o < len(data):
        if o + 4 > len(data):
            raise ValueError("truncated chunk header at %#x" % o)
        n = struct.unpack_from("<I", data, o)[0]
        if o + 4 + n > len(data):
            raise ValueError("chunk at %#x runs past the end" % o)
        out.append(data[o + 4:o + 4 + n])
        o += 4 + n
    return out


def _join(chunks):
    out = bytearray()
    for c in chunks:
        out += struct.pack("<I", len(c)) + c
    return bytes(out)


class ModelFile:
    """DAY.BFX / NIGHT.BFX / MEDIUM.BFX: [u32 size][Scene] ... (FUN_800991d0/FUN_80099218).

    Chunk order is fixed by the loader:
      0 exterior car (FUN_8007c5b4)      1 env-map shine hull (FUN_8007c5b4, id forced to 1000)
      2 in-car cockpit  3 steering wheel  4,5 driver arms  6,7 wipers  (FUN_80059f54;
      DAY/NIGHT only, not loaded in MEDIUM mode)"""

    def __init__(self, scenes=None):
        self.scenes = scenes or []

    @classmethod
    def parse(cls, data):
        return cls([Scene.parse(c) if len(c) else None for c in _chunks(data)])

    def serialize(self):
        return _join([s.serialize() if s is not None else b"" for s in self.scenes])


class TIM:
    """Standard PlayStation TIM.

    u32 0x10, u32 flags (bits0-2: 0=4bpp 1=8bpp 2=16bpp; bit3: CLUT present),
    [CLUT block: u32 len, u16 x, y, w, h, u16 data[w*h]],
    image block: u32 len, u16 x, y, w(halfwords), h, data.
    Uploaded by FUN_800955cc (OpenTIM/ReadTIM + LoadImage)."""

    def __init__(self, flags=8, clut=None, image=None, magic=0x10, tail=b""):
        self.magic = magic
        self.flags = flags
        self.clut = clut      # (x, y, w, h, [u16...]) or None
        self.image = image    # (x, y, w, h, bytes)
        self.tail = tail
        self._lens = None

    @property
    def bpp(self):
        return {0: 4, 1: 8, 2: 16, 3: 24}[self.flags & 7]

    @classmethod
    def parse(cls, d):
        magic, flags = struct.unpack_from("<II", d, 0)
        if magic != 0x10:
            raise ValueError("not a TIM")
        o, clut, lens = 8, None, []
        if flags & 8:
            ln, x, y, w, h = struct.unpack_from("<I4H", d, o)
            clut = (x, y, w, h, list(struct.unpack_from("<%dH" % (w * h), d, o + 12)))
            lens.append(ln)
            o += ln
        ln, x, y, w, h = struct.unpack_from("<I4H", d, o)
        img = (x, y, w, h, bytes(d[o + 12:o + 12 + 2 * w * h]))
        lens.append(ln)
        t = cls(flags, clut, img, magic, bytes(d[o + ln:]))
        t._lens = lens
        return t

    def serialize(self):
        out = bytearray(struct.pack("<II", self.magic, self.flags))
        if self.flags & 8:
            x, y, w, h, data = self.clut
            out += struct.pack("<I4H", 12 + 2 * w * h, x, y, w, h) + struct.pack("<%dH" % (w * h), *data)
        x, y, w, h, data = self.image
        out += struct.pack("<I4H", 12 + len(data), x, y, w, h) + data
        tail = self.tail
        if not tail and len(out) % 4:
            tail = b"\0" * (4 - len(out) % 4)   # chunk sizes must stay word aligned
        return bytes(out + tail)

    # convenience
    @property
    def clut_word(self):
        """GPU CLUT attribute (x/16 | y<<6) for this TIM's palette."""
        x, y = self.clut[0], self.clut[1]
        return (x >> 4) | (y << 6)

    def pixel_size(self):
        x, y, w, h, _ = self.image
        return w * 16 // self.bpp, h

    @classmethod
    def make_4bpp(cls, x, y, pixels, width, clut_x, clut_y, palette):
        """Build a 4bpp TIM.  x,y: VRAM position of the image in halfwords (x*4 = texel
        column inside VRAM); pixels: row-major palette indices 0..15, width a multiple
        of 4; palette: 16 15-bit colours (0x0000 = transparent, use 0x8000 for opaque
        black).  The texture is then addressed from a model as tpage x//64 (+16 if
        y >= 256), u = (x % 64)*4 + column, v = y % 256 + row, clut = clut_word."""
        if width % 4:
            raise ValueError("4bpp width must be a multiple of 4 texels")
        h = len(pixels) // width
        if h * width != len(pixels) or len(palette) != 16:
            raise ValueError("bad pixel/palette size")
        words = []
        for r in range(h):
            row = pixels[r * width:(r + 1) * width]
            for k in range(0, width, 4):
                words.append(row[k] | (row[k + 1] << 4) | (row[k + 2] << 8) | (row[k + 3] << 12))
        img = (x, y, width // 4, h, struct.pack("<%dH" % len(words), *words))
        return cls(8, (clut_x, clut_y, 16, 1, list(palette)), img)


class TextureFile:
    """DAYTIM.BFX / NIGHTTIM.BFX: [u32 size][TIM] ..., size 0 = empty slot (FUN_8001f010).
    Slots 0..11 are special: CLUT entries 14/15 are overwritten with 0x08EC/0x0466
    (mud), their VRAM rects and file data are kept for the dirt system."""

    def __init__(self, tims=None):
        self.tims = tims or []

    @classmethod
    def parse(cls, data):
        return cls([TIM.parse(c) if len(c) else None for c in _chunks(data)])

    def serialize(self):
        return _join([t.serialize() if t is not None else b"" for t in self.tims])


# ----------------------------------------------------------------------------- helpers for new files
def to15(r, g, b, stp=0):
    """8-bit RGB -> PSX 15-bit colour.  (0,0,0) without stp is the transparent texel,
    so opaque black is returned as 0x8000."""
    c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (0x8000 if stp else 0)
    return c or 0x8000


def mesh_radius(mesh):
    """16.16 bounding radius about the node origin covering every LOD (the game multiplies
    the stored value by 1.75 before its frustum test, originals store ~max |v|)."""
    m = 0.0
    for l in mesh.lods:
        if l.verts is not None:
            for v in l.verts.verts:
                m = max(m, (v.x * v.x + v.y * v.y + v.z * v.z) ** 0.5)
    return int(m / 32.0 * 65536 + 0.999)


def validate(model, textures=None, kind="DAY"):
    """Check a ModelFile (+ TextureFile or MEDIUM TIM) against everything the game code
    relies on.  Returns (errors, warnings); errors are things that crash or corrupt
    memory in the real game, warnings are visual problems."""
    E, W = [], []
    kind = kind.upper()
    need = 2 if kind == "MEDIUM" else 8
    sc = model.scenes
    if len(sc) != need:
        E.append("%s model needs exactly %d chunks, has %d" % (kind, need, len(sc)))
    for ci, s in enumerate(sc):
        if s is None or not s.nodes:
            E.append("chunk %d is empty / has no nodes (loader returns NULL, game dereferences it)" % ci)
            continue
        if len(s.serialize()) % 4:
            E.append("chunk %d size not a multiple of 4" % ci)
        if s.nodes[0].parent != -1:
            E.append("chunk %d: node 0 must be the root (it is what FUN_8009d344 returns)" % ci)
        seen = set()
        for i, n in enumerate(s.nodes):
            if n.w0 != 0:
                E.append("chunk %d node %d: +0 must be 0" % (ci, i))
            if n.mesh >= len(s.meshes) or n.parent >= len(s.nodes):
                E.append("chunk %d node %d: bad mesh/parent index" % (ci, i))
            if n.type != (0 if n.mesh >= 0 else 3):
                W.append("chunk %d node %d: type byte %d unusual for mesh=%d" % (ci, i, n.type, n.mesh))
            if (n.id & 0x30000) == 0x30000:
                E.append("chunk %d node %d: id bits 0x30000 set (night track-light hook FUN_8001fee4 would rewrite its LOD flags)" % (ci, i))
            k = n.id & 0xFFF
            if ci == 0:
                if k in seen:
                    E.append("chunk 0: duplicate id %d" % k)
                if k == 1000:
                    E.append("chunk 0: id 1000 is reserved for the shine hull")
            seen.add(k)
        # tree consistency
        for i, n in enumerate(s.nodes):
            kids, k, guard = [], n.child, 0
            while k >= 0 and guard < 4096:
                kids.append(k)
                if s.nodes[k].parent != i:
                    E.append("chunk %d: node %d is in node %d's child list but has parent %d" % (ci, k, i, s.nodes[k].parent))
                k, guard = s.nodes[k].next, guard + 1
            for j, m in enumerate(s.nodes):
                if m.parent == i and j not in kids:
                    E.append("chunk %d: node %d has parent %d but is not in its child list" % (ci, j, i))
        for mi, m in enumerate(s.meshes):
            used = [l for l in m.lods if l.used]
            if m.nlod != len(used) or any(not l.used for l in m.lods[:m.nlod]):
                E.append("chunk %d mesh %d: nlod %d does not match the used LOD entries" % (ci, mi, m.nlod))
            if m.cur != 0:
                E.append("chunk %d mesh %d: +0x48 must be 0" % (ci, mi))
            if m.radius < mesh_radius(m) * 0.9:
                W.append("chunk %d mesh %d: radius smaller than geometry (frustum culling pops)" % (ci, mi))
            for li, l in enumerate(m.lods):
                if not l.used:
                    if l.verts is not None or l.counts() != (0, 0):
                        E.append("chunk %d mesh %d lod %d: unused entry must have no faces/verts" % (ci, mi, li))
                    continue
                if l.verts is None:
                    E.append("chunk %d mesh %d lod %d: faces without vertices" % (ci, mi, li))
                    continue
                nv = len(l.verts.verts)
                if nv > MAX_VERTS_PER_BLOCK:
                    E.append("chunk %d mesh %d lod %d: %d vertices > %d" % (ci, mi, li, nv, MAX_VERTS_PER_BLOCK))
                if l.faces.max_index() >= nv:
                    E.append("chunk %d mesh %d lod %d: face index past vertex block" % (ci, mi, li))
                if l.flags & LOD_UNLIT and l.faces.g3:
                    E.append("chunk %d mesh %d lod %d: unlit LOD with G3 faces (FUN_80014e48 reads them as GT3)" % (ci, mi, li))
                for f in l.faces.g3:
                    if f.c0 >> 24 not in (0x30, 0x32):
                        E.append("chunk %d mesh %d lod %d: G3 command byte %#x (must be 0x30/0x32)" % (ci, mi, li, f.c0 >> 24))
                        break
                for f in l.faces.gt3:
                    if f.pad != 0 or f.tex >= max(1, len(s.tex)):
                        E.append("chunk %d mesh %d lod %d: GT3 tex index %d/pad %d invalid" % (ci, mi, li, f.tex, f.pad))
                        break
        for tp, cl in s.tex:
            if ci != 1 and (tp >> 7) & 3 != 0:
                W.append("chunk %d: tpage %#x is not 4bpp (only 4bpp is used/tested; 15bpp triggers FUN_8009d344's animated-texture path)" % (ci, tp))
    if sc and sc[0] is not None and sc[0].nodes:
        s = sc[0]
        for k in (1, 2, 3, 4, 5, 6):
            if s.find(k) < 0:
                E.append("chunk 0: missing node id %d (wheels 1-4, body group 5, panel 6)" % k)
        for k in (1, 2, 3, 4):
            i = s.find(k)
            if i >= 0 and (s.nodes[i].mesh < 0 or s.nodes[i].parent != 0):
                E.append("chunk 0: wheel id %d must be a mesh node directly under the root" % k)
        b = s.find(5)
        if b >= 0:
            if s.nodes[b].mesh >= 0 or s.nodes[b].parent != 0:
                W.append("chunk 0: body group id 5 should be a mesh-less child of the root")
            ids = sorted(s.nodes[k].id & 0xFFF for k in s.children(b) if s.nodes[k].mesh >= 0)
            if ids != list(range(6, 6 + len(ids))):
                E.append("chunk 0: body panel ids must be 6,7,8,... contiguous (damage slots), got %s" % ids)
            if len(ids) > 30:
                E.append("chunk 0: more than 30 body panels (damage table has 30 slots)")
            for k in s.children(b):
                n = s.nodes[k]
                if n.mesh < 0 or not 6 <= (n.id & 0xFFF) < 100:
                    continue
                m = s.meshes[n.mesh]
                if m.nlod != 3:
                    E.append("chunk 0: panel id %d needs 3 damage states (nlod=3)" % (n.id & 0xFFF))
                    continue
                a, d, c = m.lods
                if len({a.counts(), d.counts(), c.counts()}) != 1:
                    E.append("chunk 0: panel id %d damage states have different face counts "
                             "(FUN_80039304 swaps only the face pointer)" % (n.id & 0xFFF))
                if len({len(a.verts.verts), len(d.verts.verts), len(c.verts.verts)}) != 1:
                    E.append("chunk 0: panel id %d damage states have different vertex counts" % (n.id & 0xFFF))
                elif a.verts.pack() != c.verts.pack() or a.faces.pack() != c.faces.pack():
                    W.append("chunk 0: panel id %d state 0 != state 2 (pristine copy)" % (n.id & 0xFFF))
                if a.verts is c.verts or a.verts is d.verts:
                    E.append("chunk 0: panel id %d state 0 vertex block must not be shared (deformed in place)" % (n.id & 0xFFF))
        i6 = s.find(6)
        if i6 >= 0 and s.nodes[i6].mesh >= 0 and 0 in s.meshes[s.nodes[i6].mesh].extents:
            W.append("chunk 0: id 6 mesh has no half extents (podium shadow box disappears)")
    if len(sc) > 1 and sc[1] is not None and sc[1].nodes and sc[1].nodes[0].mesh >= 0:
        m = sc[1].meshes[sc[1].nodes[0].mesh]
        l = m.lods[0]
        if l.faces is not None and (l.faces.g3 or any(f.tex != 0 for f in l.faces.gt3)):
            E.append("chunk 1 (shine hull): LOD0 must be GT3-only with tex index 0")
    # textures
    if kind == "MEDIUM":
        for ci, s in enumerate(sc[:2]):
            for tp, cl in (s.tex if s else []):
                if ci == 1:
                    continue
                if tp & 0x1F != 13 or not (832 <= (cl & 0x3F) * 16 < 896) or (cl >> 6) > 253:
                    E.append("MEDIUM chunk %d: tex (%#x,%#x) outside the 64x255 block at (832,0) that is moved per player" % (ci, tp, cl))
        if textures is not None:
            x, y, w, h, _ = textures.image
            if textures.bpp != 16 or (x, y) != (832, 0) or w > 64 or h > 255:
                E.append("MEDIUM.TIM must be a 16bpp image at (832,0) no larger than 64x255")
    elif textures is not None:
        rects, cluts = [], {}
        last_nonempty = max([i for i, t in enumerate(textures.tims[:12]) if t] or [-1])
        for i, t in enumerate(textures.tims):
            if t is None:
                if i < last_nonempty:
                    W.append("texture slot %d empty before slot %d: FUN_8001f010 keeps 4 bytes too few per "
                             "empty slot, the end of slot %d's data is freed (dirt reload garbage)" % (i, last_nonempty, last_nonempty))
                continue
            x, y, w, h, _ = t.image
            if not (832 <= x and x + w <= 960 and y + h <= 255):
                E.append("slot %d image (%d,%d %dx%d) outside the car VRAM area x 832..959 y 0..254" % (i, x, y, w, h))
            if t.flags != 8 or t.clut is None or t.clut[2:4] != (16, 1):
                (E if i < 12 else W).append("slot %d is not 4bpp with a 16x1 CLUT%s" % (i, " (required: dirt writes 4-bit texels)" if i < 12 else ""))
                continue
            cx, cy = t.clut[0], t.clut[1]
            if not (832 <= cx and cx + 16 <= 960 and cy + 1 <= 254):
                E.append("slot %d CLUT (%d,%d) outside the car VRAM area" % (i, cx, cy))
            if i < 12 and any(p in (14, 15) for p in _nibbles(t)) and t.clut[4][14:16] != [0x08EC, 0x0466]:
                W.append("slot %d uses palette indices 14/15, which the game overwrites with mud colours" % i)
            rects.append((x, y, w, h, i))
            cluts[(cx, cy)] = i
        for a in range(len(rects)):
            for b in range(a + 1, len(rects)):
                x0, y0, w0, h0, i0 = rects[a]
                x1, y1, w1, h1, i1 = rects[b]
                if x0 < x1 + w1 and x1 < x0 + w0 and y0 < y1 + h1 and y1 < y0 + h0:
                    W.append("texture slots %d and %d overlap in VRAM" % (i0, i1))
        for (cx, cy), i in cluts.items():
            for (x, y, w, h, j) in rects:
                if x < cx + 16 and cx < x + w and y <= cy + 1 and cy < y + h:
                    E.append("CLUT of slot %d (rows %d,%d) overlaps image of slot %d (row y+1 is rewritten as the fog CLUT)" % (i, cy, cy + 1, j))
            if (cx, cy + 1) in cluts or (cx, cy - 1) in cluts:
                E.append("CLUT rows %d/%d both used: each CLUT needs the row below free (fog CLUT, FUN_8009cd1c)" % (cy, cy + 1))
        words = {((cx >> 4) | (cy << 6)) for (cx, cy) in cluts}
        for ci, s in enumerate(sc):
            if s is None or ci == 1:
                continue
            for tp, cl in s.tex:
                if cl not in words:
                    W.append("chunk %d tex entry clut %#x is not uploaded by any TIM" % (ci, cl))
    return E, W


def _nibbles(t):
    out = set()
    for b in t.image[4]:
        out.add(b & 15)
        out.add(b >> 4)
    return out


# ----------------------------------------------------------------------------- VRAM model
class VRAM:
    """1024x512 halfword VRAM, enough to decode textures the way the GPU samples them."""

    def __init__(self):
        self.mem = [0] * (1024 * 512)

    def upload(self, tim, mud=False):
        x, y, w, h, data = tim.image
        px = struct.unpack("<%dH" % (w * h), data)
        for r in range(h):
            self.mem[(y + r) * 1024 + x:(y + r) * 1024 + x + w] = px[r * w:(r + 1) * w]
        if tim.clut:
            cx, cy, cw, ch, cd = tim.clut
            cd = list(cd)
            if mud and len(cd) >= 16:
                cd[14], cd[15] = 0x08EC, 0x0466     # FUN_8001f010 for slots 0..11
            for r in range(ch):
                self.mem[(cy + r) * 1024 + cx:(cy + r) * 1024 + cx + cw] = cd[r * cw:(r + 1) * cw]

    def load_texfile(self, tf, mud=True):
        for i, t in enumerate(tf.tims):
            if t is not None:
                self.upload(t, mud and i < 12)

    def texel(self, tpage, clut, u, v):
        """15-bit colour (or None if transparent 0x0000) of texel (u,v) of tpage/clut."""
        tx, ty = (tpage & 0xF) * 64, ((tpage >> 4) & 1) * 256
        mode = (tpage >> 7) & 3
        cx, cy = (clut & 0x3F) * 16, (clut >> 6) & 0x1FF
        if mode == 0:
            w = self.mem[(ty + v) * 1024 + tx + (u >> 2)]
            idx = (w >> ((u & 3) * 4)) & 0xF
            c = self.mem[cy * 1024 + cx + idx]
        elif mode == 1:
            w = self.mem[(ty + v) * 1024 + tx + (u >> 1)]
            idx = (w >> ((u & 1) * 8)) & 0xFF
            c = self.mem[cy * 1024 + cx + idx]
        else:
            c = self.mem[(ty + v) * 1024 + tx + u]
        return None if c == 0 else c


def rgb15(c):
    return ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31)


def write_png(path, w, h, rows, alpha=False):
    """rows: list of h bytes objects (RGB or RGBA)."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ----------------------------------------------------------------------------- top level
def load_bytes(name, data):
    """Parse by file name: *.TIM -> TIM, *TIM.BFX -> TextureFile, other *.BFX -> ModelFile."""
    name = name.upper().replace("\\", "/").rsplit("/", 1)[-1]
    if name.endswith(".TIM"):
        return TIM.parse(data)
    if name.endswith("TIM.BFX"):
        return TextureFile.parse(data)
    return ModelFile.parse(data)


def load(path):
    with open(path, "rb") as f:
        return load_bytes(path, f.read())


def save(obj, path):
    with open(path, "wb") as f:
        f.write(obj.serialize())


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        o = load(p)
        if isinstance(o, ModelFile):
            for i, s in enumerate(o.scenes):
                if s is None:
                    print(p, "chunk", i, "empty")
                    continue
                print(p, "chunk %d: %d tex, %d nodes, %d meshes" % (i, len(s.tex), len(s.nodes), len(s.meshes)))
                for k, n in enumerate(s.nodes):
                    m = s.meshes[n.mesh] if n.mesh >= 0 else None
                    info = ""
                    if m:
                        info = " lods=" + "/".join("%d+%d f, %d v" % (len(l.faces.g3), len(l.faces.gt3),
                                                                      len(l.verts.verts))
                                                   for l in m.lods if l.used)
                    print("   node %2d id %03x parent %2d t=(%.3f %.3f %.3f)%s"
                          % (k, n.id & 0xFFF, n.parent, *n.translation, info))
        elif isinstance(o, TextureFile):
            for i, t in enumerate(o.tims):
                if t is None:
                    print(p, "slot", i, "empty")
                else:
                    print(p, "slot %d: %dbpp image %s clut %s" % (i, t.bpp, t.image[:4], t.clut[:4] if t.clut else None))
        else:
            print(p, "TIM %dbpp image %s" % (o.bpp, o.image[:4]))
