# Colin McRae Rally (PS1, NTSC-U SCUS-94474) — car model and texture formats

Covers `CMRALLY/CARS/<CAR>/{DAY,NIGHT,MEDIUM}.BFX`, `{DAY,NIGHT}TIM.BFX` and `MEDIUM.TIM`
(`SOUNDS.BFX` is not covered). Everything below was read from the game code
(`game.bin`, loaded at 0x80013000; function names are Ghidra's `FUN_<addr>`) and checked
against all 11 cars × 6 files with `tools/cmrcar.py` (byte-identical round trip,
`tools/cmrcar_test.py`).

Each statement is marked by how far it was checked:
**[code]** read in the decompilation or disassembly, **[data]** true for every original
file, **[render]** checked visually with the exporter. Section 12 lists what is still
unknown.

---------------------------------------------------------------------------------------

## 0. Quick facts

* Model files are a list of length-prefixed chunks. Each chunk is a scene the game loads
  in place: a header, a texture table (tpage/CLUT pairs), scene-graph nodes (0xBC bytes,
  the engine's own runtime object), meshes (0x4C), one vertex blob and one face blob,
  with offsets relocated to pointers at load time.
* Units: vertices are s16 in **1/32 m**. Node transforms are 16.16 fixed point in
  **metres**. Axes: **+x forward, +y up, +z = car's left** (a left-handed frame).
  The SUBARU measures 4.28 × 1.81 × 1.38 m (L×W×H); the real Impreza WRC97 is 4.34 × 1.77 × 1.39 m.
* Faces are triangles only: 16-byte untextured Gouraud (POLY_G3) and 12-byte textured
  Gouraud (POLY_GT3). Car meshes are lit per vertex by the GTE (NCCT); only the shine
  hull is unlit. Every face is back-face culled.
* Car body meshes have 3 entries that look like LODs but are **damage states**:
  live / fully damaged target / pristine copy.
* Textures are standard 4bpp TIMs placed by their own headers in the VRAM rectangle
  **x 832..959, y 0..254** (tpages 13 and 14). Each CLUT sits at x 832 on an even row,
  and the row under it is reserved. TIM slots 0..11 belong to the mud and dirt system.
* The budget the original cars keep to: about **400 triangles** for the day exterior
  including 72 for the wheels, about 440 at night, about 250 for MEDIUM. A mesh can have
  at most **342 vertices** (hard limit, 10-bit index field).

---------------------------------------------------------------------------------------

## 1. Who loads what [code]

| Step | Function | Notes |
|---|---|---|
| Per-car load loop | `FUN_8007cc34` | Sets the MEDIUM flag (byte gp+0x82d = 0x000017CD), then `FUN_8007c5b4(player)` for each car |
| Path building | `FUN_80061764` | `"CARS\%s"` with the directory name from the table at **0x800BFAC4** (index → SUBARU, SEAT, SKODA, QUATTRO, RS200, MK2, MLANCER, VWGOLF, INTEGRAL, TOYOTA, COSWORTH, MEGANE (no files)); second part `"DAY"`, or `"NIGHT"` when `FUN_8006b238()==8` |
| Textures (normal) | `FUN_8001f010` | `"%s%sTIM.BFX;1"` → `DAYTIM.BFX` / `NIGHTTIM.BFX` |
| Textures (MEDIUM) | `FUN_8007c5b4` | `"%sMEDIUM.TIM;1"`; `FUN_8009a418` + `FUN_800955cc(0,tim,0)`; fatal "File not found" if missing |
| Model | `FUN_8007c5b4` | `"%s%s.BFP;1"` or `"%sMEDIUM.BFP;1"`. The file layer maps `.BFP→.BFX` (also `.BMP→.TIM`, `.C3D→.P3D`; string table at 0x800C68D4) |
| Chunks 0, 1 | `FUN_8007c5b4` → `FUN_8009d344` | Chunk 0 is parsed under a new "holder" node (`FUN_80089f78`), chunk 1 under chunk 0's root |
| Chunks 2..7 | `FUN_80059f54` (from `FUN_8007c5b4`) | **Not called in MEDIUM mode** |
| Hierarchy fix-up | `FUN_80026838` → `FUN_80026618` | Finds nodes by id and re-parents them (section 7.1) |

Textures are always uploaded **before** the model is parsed. The parser reads CLUTs back
from VRAM (section 5.1), so this order matters.

**MEDIUM mode** is set when `DAT_800c5fa7` (podium/results with several cars),
`FUN_8008066c()`, `FUN_800212f0()`, `FUN_80021328()` (multi-car and 2-player modes,
selected by the game-mode bits at `*(0x800C6DB8)+0xD8/0xDC`) or `DAT_800c611e` is set.
So MEDIUM is used whenever several cars are on screen: 2-player split screen, the
two-car stages and the podium. It is not a distance LOD. In MEDIUM mode every loaded car
(loop count `_DAT_000017b8`: 1–3, doubled when `FUN_80021328()`) loads its own
`MEDIUM.BFX` + `MEDIUM.TIM`. The VRAM relocation in section 8.4 has slots for players 0..3.

**Other executables:** `RSCHOOL.EXE` (rally school) has its own copy of the same loader
(`FUN_80056468`, parser `FUN_8009556c`, identical layout) and always loads `CARS\SKODA`
(MEDIUM when its gp+0x931 flag is set). `FRONTEND.EXE` (loaded at **0x80130000**, not
0x80013000: `FRONTEND\NTSCGFX.BFX;1` at file offset 0x4DE64 is address 0x8017DE64)
never loads car models or textures. It only lists the `\CMRALLY\CARS` directory.

---------------------------------------------------------------------------------------

## 2. Coordinates, units, transforms [code][data][render]

* Vertex position: s16, **32 units = 1 m**. `FUN_800bb7ac` loads a node's 16.16 world
  matrix into the GTE as `R = m >> 2` (1.0 → 0x4000, i.e. ×4 in 4.12) and
  `T = t >> 9` (128 units per metre). So a vertex unit is 128/4 = 32 per metre.
* Node matrices: 3×3 row-major s32 16.16 (`m[r][c]` at +4·(3r+c)), then translation
  s32[3] 16.16 metres at +0x24. Column getters `FUN_80036694/6b8/6dc`, translation
  `FUN_80036670/700`.
* Axes: +x forward (front wheels are at +x; bonnet, bumper and headlights are at +x),
  +y up (wheels at y = −0.35 m, roof at +0.69 m), **+z = left**. Evidence: the driver's
  eye offset table has z = +0.43 for the left-hand-drive cars and z = −0.36 for the
  right-hand-drive Escort MK2, and side and plate text renders unmirrored with this
  convention (`tools/cmrexport.py` renders). The frame is left-handed. To get a
  right-handed export, negate z and reverse the triangle order (cmrexport.py does this).
* Origin: the model origin is the car's body centre. With the static model, the ground
  (bottom of the tyres) is at y ≈ −0.69 m for SUBARU (wheel centre −0.35 m, tyre radius
  0.344 m).
* Normals: 4.12 fixed point (4096 = 1.0), object space, stored in an odd order (section 5.6).
* **Winding:** a face is front-facing when `cross(v1−v0, v2−v0)` (in game x,y,z) points
  out of the surface. This holds for about 97% of faces in every car (the rest are
  slivers or have smoothed normals). The renderer culls faces whose screen-space NCLIP is
  ≤ 0, so every face is **single-sided**.

Sizes in vertex units: SUBARU body x −68..69, y −22.2 (tyre bottom)..22, z ±29.
Wheel radius 10..11 units (0.31–0.34 m), wheel half-width 4..5 units.

---------------------------------------------------------------------------------------

## 3. Chunk container (all `.BFX`) [code][data]

```
repeat until end of file:
    u32  size            (0 = empty chunk)
    u8   data[size]
```
Readers: `FUN_800991d0` (load the whole file, set cursor/end), `FUN_80099218` (next
chunk: returns a pointer to the data, or **NULL for size 0**), `FUN_80099280`
(at end?), `FUN_80099274` (size of the last chunk).

**`size` must be a multiple of 4.** The next size is read with `lw`, so a misaligned
chunk raises an address error. Odd-sized TIMs are padded with 2 bytes of junk in the
originals. The block length inside the TIM excludes the padding. `cmrcar.TIM.serialize`
pads automatically.

---------------------------------------------------------------------------------------

## 4. Model files: chunk list [code][data]

| # | DAY/NIGHT | MEDIUM | Loaded by | Content (SUBARU DAY) |
|---|---|---|---|---|
| 0 | yes | yes | `FUN_8007c5b4` | exterior car: root, 4 wheels, body group, body panels |
| 1 | yes | yes | `FUN_8007c5b4` | env-map "shine" hull (16 verts, 26 faces) |
| 2 | yes | – | `FUN_80059f54` | in-car cockpit: dash, A-pillars, bonnet seen from inside (×3 scale) |
| 3 | yes | – | `FUN_80059f54` | steering wheel (×3) |
| 4 | yes | – | `FUN_80059f54` | driver's arm/hand (×3) |
| 5 | yes | – | `FUN_80059f54` | other arm/hand, mirror image of 4 (×3) |
| 6 | yes | – | `FUN_80059f54` | wiper (G3 only, untextured) (×3) |
| 7 | yes | – | `FUN_80059f54` | second wiper (×3) |

DAY/NIGHT files have exactly 8 chunks and MEDIUM files 2. **No chunk may be empty or
have 0 nodes.** `FUN_8007c5b4` and `FUN_80059f54` store the returned root pointer and
then write through it (`root+0x14 = 1000`, `root+0xB5 = …`) without checking for NULL.
`tools/cmrexport.py` renders chunks 1..7 (`*_incar.png`).

---------------------------------------------------------------------------------------

## 5. Scene chunk layout — parser `FUN_8009d344(chunk, parent, fogflag)` [code][data]

```
+0x00  u16 n_tex          texture table entries (4 bytes each)
+0x02  u16 n_nodes        0xBC bytes each
+0x04  u16 n_mesh         0x4C bytes each
+0x06  u16 n_t3           0x34 bytes each   \
+0x08  u16 n_t4           0x18 bytes each    } track-only record types, 0 in every car file
+0x0A  u16 n_t5           0x1C bytes each   /
+0x0C  u32 vblob_size     bytes of vertex data
+0x10  tex[n_tex]
       node[n_nodes]
       mesh[n_mesh]
       (t3/t4/t5 tables)
       vertex blob  (vblob_size bytes)
       face blob    (to the end of the chunk)
```
Relocation (file values → pointers; −1 → NULL):

| Field | Relative to |
|---|---|
| node +0x04 next, +0x08 child, +0x0C parent, +0xA8 | node table start |
| node +0x10 mesh | mesh table start |
| LOD +0x04 faces (mesh +0x1C/+0x2C/+0x3C) | face blob start |
| LOD +0x08 vertices (mesh +0x20/+0x30/+0x40) | vertex blob start |
| mesh +0x04 | overwritten with the address of this chunk's tex table |
| mesh +0x08 radius | multiplied by 1.75 (`FUN_800bacb8(r, 0x1C000)`) |

Any node whose parent is −1 is attached under the `parent` argument
(`FUN_80089998`). The function **returns node 0**, which must be the chunk's root
(the only node with parent −1 in every original). Every node is pushed onto a global
linked list (`FUN_8008b2e0`, which requires node+0 == 0). There is no fixed capacity.

### 5.1 Texture table entry (4 bytes) [code][data]

`u16 tpage, u16 clut` (GPU attribute words). The loader rewrites tpage as
`mode<<7 | (w & 0x1F) | (w & 0x800)`. It keeps the page x/y bits and the colour mode,
and clears the blend bits 5–6 and bit 9. If the mode is 2 (15-bit) it registers the
entry as an animated texture (at most 2, table 0x800E1518). Don't use that for cars.
If `fogflag` (`FUN_8006b220()==5`) is set, it calls **`FUN_8009cd1c(clut)`**: this reads
the 16×2 halfwords at (clut_x, clut_y) and writes row **y+1** as a "fog" CLUT
(`0xC210` for every non-zero entry, 0 for transparent ones). Every CLUT therefore needs
the row below it free.

Car values: tpage `0x0D` (x 832) or `0x0E` (x 896), 4bpp, y 0. CLUT `x = (clut & 0x3F)*16`
is always 832 (low bits 0x34), and `y = clut >> 6` is even (0..58). The hull (chunk 1)
has `(0,0)`, which the env-map code replaces at run time.
In MEDIUM mode `FUN_8009cb70(node, dx, 0, abr)` shifts every entry's tpage x and CLUT x
by `dx` texture pages. It processes an entry once, using bit 9 as the "done" marker
(section 8.4).

### 5.2 Node, 0xBC bytes (the engine object, used in place) [code][data]

| Off | Type | File value | Meaning / readers |
|---|---|---|---|
| 0x00 | u32 | 0 | Global node-list link. **Must be 0** (`FUN_8008b2e0`) |
| 0x04 | s32 | rel/−1 | next sibling |
| 0x08 | s32 | rel/−1 | first child |
| 0x0C | s32 | rel/−1 | parent (−1: attach to the loader's parent) |
| 0x10 | s32 | rel/−1 | mesh |
| 0x14 | u32 | id | `id & 0xFFF` is the identity (`FUN_8008ad1c` find-by-id, depth first). Upper bits are leftovers from the authoring tool (e.g. QUATTRO 0x91xxxx). **Bits 0x30000 must not both be set**: `FUN_8001fee4` (the night lighting callback, run for every drawn node) then forces the node's LOD unlit |
| 0x18 | s32[12] | | **Base transform** (rest pose). Copied to local by `FUN_80089b8c` (rotation and translation) and `FUN_80089cf0` (rotation only). Wheel rest positions are read from +0x3C by `FUN_800713ec` |
| 0x48 | s32[12] | = base | **Local transform**. `FUN_80026618` copies the wheel translations (+0x6C) into car struct +0x4F8; per frame the game writes it for root, holder, wheels and attachments |
| 0x78 | s32[12] | identity | World transform, computed by `FUN_80089834` |
| 0xA8 | u32 | −1 | Pointer, relocated against the node table. Purpose unknown; −1 in every file |
| 0xAC | s16 | −1 | Track zone index (computed at run time, `FUN_8008e8f4`) |
| 0xAE | u16 | 0 | |
| 0xB0 | u32 | 0 | |
| 0xB4 | u8 | 0 / 3 | Type: **0 = mesh node** (the only kind `FUN_80091ac8` draws), **3 = group** (no mesh). `FUN_80089834` skips types > 3 |
| 0xB5 | u8 | 0xFF | Per-viewport visibility mask (`FUN_80089a28`). Rewritten per camera mode (`FUN_8005ace4`); a 0 hides the whole subtree |
| 0xB6 | u8 | 0 | LOD index used only by the track-light callback |
| 0xB7 | u8 | 0xFF | Light scale: the colour matrix is scaled by (b7+1)/256 (`FUN_80091ac8` → `FUN_800bbfec`) |
| 0xB8 | u8 | 0 | Visible-this-frame mask (run time) |
| 0xB9 | u8 | 0 | |
| 0xBA | u16 | 0 | Flags (run time: bit0 world dirty, bit1 share parent's world matrix, bit2 heap node, bit5 no zone cull) |

### 5.3 Mesh, 0x4C bytes [code][data]

| Off | Type | File value | Meaning / readers |
|---|---|---|---|
| 0x00 | u32 | 0 | unused by car code |
| 0x04 | u32 | 0xFFFFFFFF | → tex table pointer (loader) |
| 0x08 | s32 | | Bounding-sphere radius about the **node origin**, 16.16 m. The loader multiplies it by 1.75 and the frustum test (`FUN_800b66b4(0,0,0,r)` in `FUN_80091ac8`) uses that. Originals store about max\|v\|/32 m (e.g. SUBARU wheel 0.497 vs geometric 0.51; panel 2.359 vs 2.38). `cmrcar.mesh_radius()` |
| 0x0C | s32[3] | 0 | Half extents x,y,z (16.16 m). **Only the mesh of node id 6** has them (SUBARU 2.135/0.685/0.906). They are read by `FUN_8007a0e0`, which draws a box shadow for cars without physics (podium, `DAT_800c5fa7`) |
| 0x18 | LOD[3] | | 16 bytes each, section 5.4 |
| 0x48 | u8 | 0 | Current state index; `FUN_80091ac8` draws `lods[cur]`. Nothing sets it in GAME.EXE (`FUN_8008ac68`, the only writer, is never called), so it stays 0 |
| 0x49 | u8 | 1 or 3 | Number of LOD entries in use (`FUN_80092bf8`, `FUN_8008ac68`). Wheels, hull and in-car parts: 1. Body panels: 3 |
| 0x4A | s16 | | Ordering-table bias added to the face's OT index. Lower values sort nearer, so they are drawn later (on top). This is consistent with the hull's −10 shine overlay; the OT direction itself was not traced. Body and wheels +1 (the game adds +1 more to wheels and +2 to id 6, `FUN_8006c864`), in-car parts −1/−2, hull −10 |

### 5.4 LOD / damage-state entry, 16 bytes at mesh +0x18, +0x28, +0x38 [code][data]

| Off | Type | Meaning |
|---|---|---|
| 0x0 | u16 | n_g3: number of 16-byte G3 faces (stored first) |
| 0x2 | u16 | n_gt3: number of 12-byte GT3 faces (after the G3s) |
| 0x4 | s32 | face offset (face blob), −1 = unused |
| 0x8 | s32 | vertex offset (vertex blob), −1 = unused |
| 0xC | u8 | flags: bit1 `0x02` semi-transparent (GT3 code 0x36 / FT3 0x26), bit6 `0x40` **unlit** (renderer `FUN_80014e48`), bit7 used by track code. Cars: 0; hull 0x42 (forced at load by `FUN_8008b33c(hull,0x42)`). In the 2-player ghost mode `FUN_8003aa30` forces 0 or 0x02 on whole car subtrees |
| 0xD | u8 | brightness for unlit meshes (0x80 = 1.0). The hull's is set every frame (0x48 day, 0x10 night) |
| 0xE | u8[2] | 0 |

Unused entries are `0,0,−1,−1,0,0,0,0`. **There is no vertex count:** the game derives
it as max(index field)/3 + 1 (`FUN_80092af4`).

### 5.5 Blob layout [data]

Every original lays out the blocks in mesh order, then LOD order, with no gaps, no
padding and no unreferenced bytes. The only sharing is identical offsets: the 4 wheel
meshes point at one vertex block and one face block. `cmrcar` rebuilds both blobs that
way and reproduces all 33 model files byte for byte. **Damage state 0 must have its own
vertex block** (it is deformed in place). States 1 and 2 are separate copies in all
originals.

### 5.6 Vertex, 12 bytes [code][data]

```
+0 s16 x   +2 s16 y   +4 s16 z        position, 1/32 m
+6 s16 nz  +8 s16 nx  +A s16 ny       unit normal ×4096, note the order
```
The GTE gets `VXY = word@+8`, `VZ = s16@+6`, so the normal is (s16@+8, s16@+10, s16@+6).
Readers: `FUN_800147c0` (NCCT), `FUN_800385b0` and `FUN_80039304` (damage morph),
`FUN_8009dc1c` (env map), `FUN_8009e058` (normal rewrite).
Example (hull): vertex (−4,−22,−62) normal (1536, 0, −3796) points −z as expected.

### 5.7 Faces [code][data]

**Vertex index word** (both kinds): three 10-bit fields at bits 0, 10, 20. **Each field is
vertex_index × 3** (a word offset into the 12-byte vertex array). Bits 30–31 are 0.
**Max index 341, so at most 342 vertices per vertex block.** The renderer uses
`(field & 0x3FF)*4 + verts` directly, with no bounds check.

**G3 — 16 bytes, untextured Gouraud triangle**
```
+0 u32 c0 = 0xCCBBGGRR   +4 u32 c1   +8 u32 c2   +C u32 indices
```
The lit renderer uses only c0. It becomes the GTE RGBC for NCCT, so its top byte is
**the GPU command and must be 0x30** (or 0x32 for a semi-transparent G3). Any other code
desynchronises the primitive stream. c1 = c2 = c0 in every file. Colour scale: 0x80 = full
brightness. Originals use dark colours for wheel arches and interior, e.g. (0,7,7).
The packet is 7 words. **Unlit LODs (flag 0x40) must not contain G3 faces:**
`FUN_80014e48` starts at the face pointer and reads everything as GT3.

**GT3 — 12 bytes, textured Gouraud triangle**
```
+0 u8 u0, v0, u1, v1, u2, v2     texel coordinates in the 256×256 texture page
+6 u8 tex                         index into this chunk's tex table
+7 u8 0                           (the renderer reads +6 as s16: must be 0)
+8 u32 indices
```
The lit path (`FUN_800147c0`) emits POLY_GT3 (10 words) with RGBC 0x34808080, or
0x36808080 if the LOD flag has 0x02. Past the fog distance it emits a **second**
GT3 with blend mode 1 (additive), CLUT+1 row, and fog-table colours (80 bytes in total).
The unlit path (`FUN_80014e48`) emits POLY_FT3 code 0x24/0x26 with gray = brightness.
UVs address the page given by tex[tex].tpage. A UV may sit one texel past the edge of
its texture (PSX exclusive edge). 330 such corners exist in the originals.

---------------------------------------------------------------------------------------

## 6. Rendering path (what any new geometry goes through) [code]

`FUN_80091d2c` (scene) → `FUN_8009dc1c` (env maps) → **`FUN_80091ac8(node)`**,
recursive over the scene graph:

1. Skip the subtree unless `node+0xB8 & viewport_mask` (set by `FUN_80089a28` from
   +0xB5 and zone culling).
2. Draw only if `type==0`, the mesh is set and `lods[cur].faces != NULL`.
3. Combine the world matrix with the camera, then frustum-test a sphere at the node
   origin with radius 1.75 × mesh+0x08.
4. Unlit flag clear: set the light matrix from the node's world rotation
   (`FUN_800bbb4c`), then `FUN_800147c0(lod, otbias, tex)`. Otherwise
   `FUN_80014e48(lod, otbias, tex, brightness)`.
5. Per triangle: RTPT, then NCLIP (cull if ≤ 0), then AVSZ3. Skip if OTZ is nearer than
   the near limit or farther than the far limit. Then emit packets (section 5.7) and link
   them at `((OTZ>>shift)+bias) & mask`.

There is no per-mesh vertex transform buffer (every triangle transforms its own 3 vertices).
Mesh size is limited only by the index field and by the primitive buffer:
two heap buffers of 0x1A9C8 = 109 000 bytes (`FUN_8006c8a4` → `FUN_80096108`), filled
**without bounds checks**. The scene graph (all cars) is
drawn first. The track is drawn afterwards and stops at 1/2 and then 5/6 of the buffer
(`FUN_80091d2c`), so a heavier car mostly costs track detail. A frame that runs over
only prints "OH SHIT CRAPPED OVER THE H…". 40 bytes per GT3 (80 when fogged),
28 per G3, 32 per FT3.

---------------------------------------------------------------------------------------

## 7. Object hierarchy and run-time roles

### 7.1 Chunk 0 node ids and the re-parenting [code][data]

Every original exterior has this exact structure (`cmrcar.validate` enforces it):

```
node 0  id 0   root, group (type 3)            <- returned by the parser
 ├ id 1  front-left  wheel  (+x, +z)   mesh, 1 LOD
 ├ id 2  front-right wheel  (+x, −z)
 ├ id 3  rear-left   wheel  (−x, +z)
 ├ id 4  rear-right  wheel  (−x, −z)
 └ id 5  body group, group (type 3), identity transform
     ├ id 6  ... id 6+n−1   body panels, mesh, 3 damage states, identity transform
```
Node order in the file: root, wheels 1..4, body group, panels 6.., with child and next
chains built as "parent.child = highest index, next = next lower index". `Scene.link()`
rebuilds that and reproduces every original.

`FUN_80026618` (via `FUN_80026838`), with `holder` = the node the chunk was parsed under:
* car+0x00 = root, car+0x08 = holder, car+0x04 = `find(holder,5)` (body group),
  car+0x0C..0x18 = `find(root,1..4)` (wheels), car+0x1C = `find(holder,1000)` (hull).
* `FUN_8006c864`: wheels' mesh OT bias +1, id 6's bias +2.
* **Re-parent:** the root goes under the global scene node, the **body group (id 5) goes
  under the holder**, and the hull goes under the body group (inserted as its first child).
  The root (with the wheels) and the holder (with the body) are then moved separately
  by the physics, so the body pitches and rolls on its suspension while the wheels
  follow the ground.
* Wheel rest positions = wheel node translation (+0x6C) → car+0x4F8 + 12·i.

Ids are looked up by `id & 0xFFF`, depth first. **They must be unique, and 1000 is
reserved.** Root and body-group base transforms must be identity: they are composed
with the physics pose.

### 7.2 Wheels [code][data][render]

* Geometry is centred on the wheel node, **axle along local z**. Originals use one
  octagonal tread (16 GT3), a square disc at z = +h and one at z = −h (2 GT3 each,
  round textures with transparent corners). 18 faces and 36 vertices, all four wheel
  meshes sharing one vertex block and one face block.
* `FUN_80050000` per frame: `FUN_80089cf0(wheel, {x: camber from damage, y: steer
  (+0x800 when flipped), z: spin})` rebuilds the local rotation from the base rotation
  (`FUN_8008a19c` applies the y, then z, then x rotations). Translation = rest position +
  suspension (`FUN_80036700`). **Steering applies to ids 1 and 2** (front).
* **Hub flip:** `FUN_80050c2c` sets a per-wheel flag (0x800C6F18). Flagged wheels get a
  further 180° about y. Left wheels (ids 1, 3) are flagged when spinning fast
  (\|ω\| > 0x8000); right wheels (2, 4) when slow. The **+z face is the sharp, static hub
  and the −z face the motion-blurred one**, and the correct face always points outward.
  (SUBARU: +z face uses the upper wheel texture, −z the blurred lower one, see the VRAM
  image.)
* The physics does **not** take its contact geometry from the model. Car dimensions and
  suspension constants are hard-coded per car index in `FUN_80026838` (e.g. SUBARU
  car+0x20/24/28 = 4.271/1.370/1.812 m). Only the drawn wheel positions come from the
  model.

### 7.3 Body panels = damage system [code][data]

`FUN_80038104` (single-player; `FUN_80037a14` for multi-car) walks the body group's
children with `6 <= id&0xFFF < 100`. It puts each in **damage slot id−6** (30 slots per
car, table at 0x800D28C8 + player·0x734). It stores pointers to the three entries and the
vertex count of entry 0, and builds a bounding box. Ids outside 6..35 overflow the
30-entry arrays. The update loops run over slots 0..count−1, so **ids must be 6, 7, …
contiguous**. That holds in every original (11–14 panels at most).

The three entries of a panel mesh:

| Entry | Role | Code |
|---|---|---|
| 0 (mesh+0x18) | **Live**: what is drawn. Its vertices are morphed in place toward entry 1 near impacts (`FUN_800385b0`, `FUN_80039304`). Past a damage threshold its **face pointer is replaced by entry 1's** (`FUN_80039304`) | renderer |
| 1 (mesh+0x28) | **Fully damaged target**: dented vertices, plus faces with the "damaged" texturing (e.g. cracked-glass windows: SUBARU slots 13–16) | morph target |
| 2 (mesh+0x38) | **Pristine copy** (byte-identical to entry 0 in every file). Used as the reference for morph deltas and copied back into entry 0 on repair (`FUN_80039a94`, which also restores the face pointer) | |

So all three entries must have the **same vertex count** and **identical n_g3/n_gt3**
(only the face pointer is swapped). Entry 1's faces index entry 0's vertex layout.
Damage morph strength falls off with distance (radius 1 m) from the impact point
car+0x344.

Panel roles. The damage→mechanics mapping in `FUN_80038104` is hard-coded by **slot
number** (front bumper → slot 7, rear → 10, etc.; filled at table+0x727..0x733), so
every car follows the same scheme. Panel centroids across all 11 cars agree: 7 = +z side,
8 = −z side, 10 = roof, 11 = rear deck, 12 = bonnet, 13 = front (low), 14/15 = tail
lights, 16 = rear. Ids 6 and 9 vary most, and COSWORTH/MLANCER's id 13 reaches further
back. Roles as identified in SUBARU (`tools/cmrexport.py`, `*_panels.png`):

| id | slot | SUBARU content |
|---|---|---|
| 6 | 0 | inner wheel arches / dark interior bits (30 G3 + 14 GT3); carries the half extents |
| 7 | 1 | left side (doors, side windows, door number) |
| 8 | 2 | right side |
| 9 | 3 | rear wing + textured underbody |
| 10 | 4 | roof + windscreen |
| 11 | 5 | rear window + boot lid |
| 12 | 6 | bonnet + headlights (stays visible in the in-car view) |
| 13 | 7 | front bumper |
| 14 | 8 | left tail light |
| 15 | 9 | right tail light |
| 16 | 10 | rear bumper + number plate |
| 17 | 11 | NIGHT only: the extra lamp pod on the bonnet (others: 17–19) |

In-car view (`FUN_8005ace4`): the body group's first child (the hull) and every child
with id < 16 except 12 are hidden. Id 12 (bonnet) and ids ≥ 16 stay visible.

### 7.4 Chunk 1 — env-map shine hull [code][data]

* One mesh node, 1 LOD, GT3 only, all faces `tex 0`. File flags 0x42 (forced again at
  load), OT bias −10. A rotation in the node's base matrix is allowed (the originals map
  local x→world y). It is a coarse shell (SUBARU: 16 vertices, 26 faces), slightly
  **inside the body silhouette**: 3.9 × 1.37 m vs the body's 4.28 × 1.81.
* `FUN_8007c5b4`: `root+0x14 = 1000` (the id is overwritten). Then
  `FUN_8009e058(hull,0,0x10000)` **replaces every normal with the normalised vertex
  position**, so the file normals don't matter.
* Every frame `FUN_8009dc1c` (registered by `FUN_8006cba8` with the env texture
  `DAT_800c65b8`, brightness 0x48 by day and 0x10 at night, additive) rewrites the UVs of
  entry 0's faces from the view-space normal (sphere map, 64×64 texels), and writes
  tex[0] = env tpage/CLUT.
* The "chrome cars" mode (flags & 0x44000000) instead registers **every body panel**
  for env-mapping (abr 0) and hides the hull. In that mode `FUN_8009dc1c` writes the
  tex-table entries that panel faces use. This is an original behaviour, not a constraint.

### 7.5 Chunks 2..7 — in-car parts [code][render]

`FUN_80059f54(car, physics, carIndex, player)` parses each chunk under the **body
group** and places its root with `FUN_80089b8c(node, pos, angles)`
(local = base, then translation += pos). These models are stored at **3× scale** and
placed at `3·P − 2·E`, where E is the driver's eye. Scaling about the eye leaves the
projection unchanged and avoids near-plane clipping.

* E = `DAT_800be578` (−0.434, 0.250, 0) + per-car `DAT_800be584[car]` (12 bytes each).
  SUBARU: eye (−0.25, 0.50, 0.43).
* 2 cockpit: at −2E. 3 steering wheel: real centre = `DAT_800be584[car]`, rotated by
  steering (`FUN_80059b24`).
* 4/5 arms: parked at y = 100 m, then posed by `FUN_8005b12c`. One hand stays on the
  wheel and one goes to the gear lever, chosen by the LHD byte `DAT_800be620[car]`.
* 6/7 wipers: pivots `DAT_800be734/6a4[car]`, angles `DAT_800be824/7c4[car]`,
  `DAT_800be884/8b4[car]`.

All placement data is per **car index** in GAME.EXE. A replacement car keeps the
original's eye point, wipers and steering-wheel position. The simplest valid approach is
to copy chunks 2..7 (and the TIMs they sample) from the car being replaced. Their
visibility is switched with +0xB5 depending on camera mode (`FUN_8005ace4`).

### 7.6 NIGHT vs DAY [data][render]

NIGHT is a complete separate model and texture set. Chunk 0 has one or two extra body
panels: the bonnet lamp pod (SUBARU id 17, 45 GT3; QUATTRO ids up to 19), with slightly
different tex tables. Chunk 2 adds a few faces. NIGHTTIM.BFX has 0–4 more TIMs (lit
lamps). Headlight beams are not in the model.

### 7.7 MEDIUM [code][data][render]

A reduced exterior: ~230–270 triangles, 3–5 body panels with ids 6.. that still have
3 damage states, and the same 4 wheels, body group and hull. All textures are in one
64×255 block (section 8.4). Only chunks 0 and 1 exist.

---------------------------------------------------------------------------------------

## 8. Textures

### 8.1 `DAYTIM.BFX` / `NIGHTTIM.BFX` [code][data]

A chunk container (section 3) of standard TIMs. A size-0 chunk is an empty slot.
`FUN_8001f010` uploads every non-empty TIM with `FUN_800955cc(sprite, tim, 1)`
(libgpu OpenTIM/ReadTIM + LoadImage of the image and CLUT). There is no limit on the count
(originals 32–37 slots). In every original, every TIM is **4bpp (flags 0x08) with a
16×1 CLUT**.

TIM: `u32 0x10, u32 flags (bit3 CLUT, bits0-2 mode), [u32 len=12+2·w·h, u16 x,y,w,h,
u16 clut[w·h]], u32 len=12+2·w·h, u16 x,y,w,h, image`. `w` is in halfwords (4 texels at
4bpp). The image's VRAM position determines the UVs: tpage = x/64, u = (x mod 64)·4 + column,
v = y + row.

### 8.2 VRAM area and CLUT rules [code][data]

* All car images in all originals lie inside **x 832..959, y 0..254**. CLUTs lie at
  x 832..847 on **even rows 0..58**. Images in that column start below each car's last
  CLUT row pair (e.g. SUBARU: CLUT rows 0..43, first image row 46) and never overlap a
  CLUT row or the row under it.
* **Row y+1 under each CLUT is reserved** for the fog CLUT (section 5.1). Two CLUTs must
  not be on adjacent rows.
* The car owns this area in single-player. What else lives in VRAM (y = 255,
  x < 832, y ≥ 256) was not mapped, so **stay inside the original rectangle**.
* Colour 0x0000 is transparent. Opaque black is 0x8000. The STP bit only matters for
  semi-transparent faces.
* `tools/cmrexport.py` (`*_vram.png`) shows a car's area decoded with each TIM's
  own CLUT (magenta = transparent texels; the top-left strip is the CLUT column).

### 8.3 Slots 0..11: mud and dirt [code][data]

`FUN_8001f010` treats the first 12 chunks specially (`FUN_8001eef4` repeats this on reload):
* It reads back each one's CLUT and **overwrites entries 14 and 15 with 0x08EC and
  0x0466** (mud browns). Palette indices 14/15 in these textures therefore always show
  mud. The originals keep placeholder greens there in their side textures.
* It saves each slot's image rect (table 0x800C71D0, 8 bytes per slot) and its file
  pointer (0x800C72D0). It then **shrinks the file buffer** to Σ(4+size) over the
  non-empty slots among the first 12, keeping their pixel data in RAM for the dirt reload.
  Bug: empty slots are not counted, so the retained buffer is 4 bytes per empty slot
  short. If empty slots come **before** the last used slot among 0..11 (most originals),
  the tail of that slot's data is freed. **Put empty slots after the last used one**
  (or use all 12).
* Dirt (`FUN_8001ec84` → `FUN_8001e418` → `FUN_8001de68` → `FUN_8001dbe4`) writes 4×4
  splats of **4-bit texels with indices 14/15** straight into VRAM (StoreImage, patch,
  LoadImage of 2×4 halfwords). It assumes 4bpp and 4 texels per halfword, so **slots
  0..11 must be 4bpp**. Splat x < w·4−8, y < h−4.
* Slot roles: **1 and 2 = the large side textures** that get mud sprayed along them
  (types 1 and 2, SUBARU: 256×48 texels each, used by ids 7 and 8). **3, 4, 5, 9, 10, 11**
  get random stone chips (type 5). **0, 6, 7, 8** get no dirt (SUBARU slot 0 = underbody).
* Mud direction on slots 1 and 2 is hard-coded per **car index** in `FUN_8001e214`.
  Flags 0x800C6960/68/70/78 set which texture edge (top/bottom, left/right) the mud
  grows from: cars 0, 1 / default (2, 3, 4, 6, 9) / 5, 7, 8, 10 differ. A replacement car
  must orient its side textures like the car it replaces.

### 8.4 `MEDIUM.TIM` [code][data]

One TIM, **16bpp, 64×255 halfwords at (832,0)**. This is a raw pre-packed VRAM block
containing the 4bpp textures and their CLUTs (MEDIUM tex tables use tpage 0x0D and CLUTs
at x 832..880, y 0..6). It is uploaded with `FUN_800955cc(0,tim,0)`. In multi-car modes
`FUN_8007c5b4` then copies the fixed rect **(832,0,64,255)** (`DAT_800c65c4`) to
x = 512 / 576 / 896 (`FUN_800a2d18`) and shifts the model's tex entries by −5/−4/+1
pages (`FUN_8009cb70`). So every MEDIUM texture and CLUT must lie inside that 64×255
block and use tpage 13.

### 8.5 Texture usage in SUBARU DAY (for reference)

18 (tpage, CLUT) materials in the exterior. Wheels use 3 TIMs (sharp hub, blurred hub,
tread). Windows use separate TIMs for intact (entries 0 and 2) and cracked (entry 1) glass.
Two faces of id 9 reference CLUT 0xB34 (row 44), which no TIM uploads: an original data
glitch, also present in a few other cars.

---------------------------------------------------------------------------------------

## 9. Limits that matter for a new model

| Item | Limit | Source |
|---|---|---|
| Vertices per vertex block (= per mesh state) | **342** (index 0..341) | 10-bit field × 3, `FUN_800147c0` |
| Faces per state | u16, practically limited by the primitive buffer | |
| Exterior triangles (original budget) | DAY ≈ 395–401 (incl. 72 wheel), NIGHT ≤ 450, MEDIUM ≤ 269 | data |
| Largest original mesh state | 140 vertices / 116 faces (RS200 MEDIUM) | data |
| Body panels | ids 6.. contiguous, **≤ 30** (originals 11–14) | `FUN_80038104` |
| Tex table entries per chunk | ≤ 256 (u8 index) | |
| Chunks | DAY/NIGHT 8, MEDIUM 2, none empty | loaders |
| Car VRAM | x 832..959 × y 0..254 (512×255 4bpp texels incl. CLUTs); MEDIUM 64×255 halfwords at (832,0) | data / `FUN_8007c5b4` |
| CLUTs | 16×1 at even rows with the next row free; originals use rows 0..58 at x 832 | `FUN_8009cd1c` |
| Texture depth | 4bpp (required for slots 0..11, used everywhere) | dirt code |
| File sizes (originals) | DAY ≤ 48 252, NIGHT ≤ 53 464, MEDIUM ≤ 22 540, DAYTIM ≤ 54 364, NIGHTTIM ≤ 54 940 bytes | data |
| RAM | Model files stay resident for the whole stage. The texture file is resident only until it is shrunk to slots 0..11. Heap 0x800E2CE8..0x801FFC00 (first-fit, `FUN_8008b614`). Running out halts the game with a coloured screen (trap). Headroom during a stage was **not measured**: stay near the original sizes, especially for MEDIUM (up to 4 cars loaded) | `FUN_8008b5a4` |
| Primitive buffer | 2 × 109 KB per frame, no bounds check. GT3 40/80 bytes. Cars draw before the track, and the track cuts itself back | `FUN_80091d2c` |

---------------------------------------------------------------------------------------

## 10. What a replacement car inherits from GAME.EXE (per car index, not in the files)

Physics and collision box dimensions plus handling (`FUN_80026838` switch). Driver eye,
steering wheel, arm poses and wipers (section 7.5). The LHD/RHD byte `DAT_800be620`.
Mud direction (`FUN_8001e214`). Damage-slot → mechanics mapping (fixed slots, section 7.3).
Multi-car damage tweaks (`FUN_80037a14` switch). The name and selection screens are
in FRONTEND.EXE. **RSCHOOL.EXE also loads SKODA.** To change any of these you have to
patch the executables. The files alone can't.

---------------------------------------------------------------------------------------

## 11. Checklist for writing a new car (all checked by `cmrcar.validate`)

1. DAY/NIGHT model: 8 non-empty chunks. MEDIUM: 2. Chunk sizes are multiples of 4.
2. Chunk 0: node 0 = root (group, id 0, identity). Wheels ids 1–4 are mesh nodes directly
   under the root at their hub positions, axle along z, +z face = static hub. Id 5 is a
   group under the root with identity transform. Panels ids 6..6+n−1 (n ≤ 30) are mesh
   nodes under id 5 with identity transforms. Ids are unique and 1000 is not used.
   Bits 0x30000 of an id are never both set. Node +0 = 0. Type 0 for mesh nodes,
   3 for groups. Base = local, world = identity, +0xA8 = −1, +0xAC = −1,
   +0xB5 = +0xB7 = 0xFF, the rest 0.
3. Every panel has 3 states with equal vertex counts and identical face counts. State 0
   and state 2 are identical, with separate blocks. State 1 is the dented shape (and
   damaged texturing).
4. Id 6's mesh carries the half extents. Mesh radius ≥ max |vertex| about the node origin.
   cur = 0. nlod = number of used entries. OT bias +1 (originals).
5. Faces: G3 code byte 0x30/0x32 with c1 = c2 = c0. GT3 tex < n_tex, byte 7 = 0.
   Indices < vertex count ≤ 342. Front faces wound so that `(v1−v0)×(v2−v0)` points
   outward (game coordinates). Normals stored as (nz, nx, ny) ×4096.
6. Chunk 1: a hull node with 1 GT3-only LOD, tex 0, flags 0x42, bias −10, lying just
   inside the body silhouette.
7. Chunks 2..7: copy them from the car being replaced, together with the TIMs they use.
8. Textures: 4bpp TIMs inside x 832..959, y 0..254. 16-entry CLUTs at (832, even y) with
   row y+1 free and no overlaps. Slots 1/2 are the two side textures (mud). Slots
   3,4,5,9,10,11 take stone chips. Palette indices 14/15 of slots 0..11 are mud. Empty
   slots come after the last used one of the first 12. The model's tex entries must
   point at CLUTs that are actually uploaded.
9. MEDIUM: everything in tpage 13. MEDIUM.TIM = 16bpp 64×255 block at (832,0).

---------------------------------------------------------------------------------------

## 12. Open questions / not verified

* **In-game test:** none of this has been run in the game or emulator with a modified
  file. Everything rests on reading the code and on the round trip.
* node +0xA8 (relocated pointer, always −1), mesh +0x00 (always 0), G3 c1/c2 (always
  = c0). Their meaning is unknown, but car code doesn't use them in a way that needs other
  values. Keep the original values.
* Exact rule the authoring tool used for the mesh radius (≈ max |v|, sometimes ~2%
  smaller). The game only needs it large enough.
* Names of the multi-car modes behind `FUN_8008066c`, `FUN_800212f0`, `FUN_80021328`,
  `DAT_800c611e`. That they all select MEDIUM is certain. Which UI mode is which is not.
* `FUN_8006b220()==5` (fog CLUT generation) is a stage environment index, assumed to be fog.
* Whether VRAM outside x 832..959, y 0..254 is free during a stage (not mapped). Heap
  headroom during a stage (not measured).
* The 2-player "ghost" semi-transparency (`FUN_8003aa30` forcing flags 0x02) and the
  "chrome cars" mode were seen in code only.
* Rally school (RSCHOOL.EXE) uses the same format. Its copy of the per-car tables was not
  dumped.
* The physics constants per car were not decoded beyond the three body dimensions.

---------------------------------------------------------------------------------------

## 13. Tools

* `tools/cmrcar.py`: parser/writer, stdlib only. `load(path)`, `save(obj,path)`; `ModelFile` →
  `Scene` → `Node`/`Mesh`/`Lod`/`FaceBlock`/`VertBlock`/`FaceG3`/`FaceGT3`/`Vertex`;
  `TextureFile` → `TIM` (`TIM.make_4bpp`); `VRAM` (sampling); `validate()`;
  `mesh_radius()`; `Scene.link()`; `to15()`. The writer always regenerates offsets and
  blobs from the objects, sharing a block only where the same Python object is used twice.
* `tools/cmrcar_test.py`: all 11 cars × 6 files, byte-identical parse→serialize→parse→serialize,
  validator, `link()` check (`ctest` runs it on `game/extracted`).
* `tools/cmrexport.py CAR_DIR OUT_DIR [DAY|NIGHT|MEDIUM]`: OBJ/MTL/PNG export, the car's VRAM
  area decoded, and software renders (3/4 view, four views, panels, in-car parts).
* `tools/gt2car.py` writes new cars with it (see the README's Extra cars section).
