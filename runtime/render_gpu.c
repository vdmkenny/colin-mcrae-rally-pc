// SDL_GPU renderer (Vulkan, Metal): mirrors GPU drawing into a high-resolution copy of VRAM
// and presents it.
//
// The software GPU (gpu.c) stays authoritative for the 1x VRAM the game can read back and
// texture from. Every primitive is also drawn here at `scale` times the resolution, in true
// colour, with sub-pixel vertex positions and perspective-correct texturing when the GTE
// supplied precise coordinates. Textures are sampled from a 1x copy of VRAM in the shader.
#include "render.h"
#include "pgxp.h"
#include "gpu_shaders.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

extern uint16_t vram[1024 * 512];

// ---------------------------------------------------------------------- state
typedef struct {
    float x, y, w;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t tpage, clut, texwin, flags;
    uint32_t uvlim;   // umin | vmin << 8 | umax << 16 | vmax << 24, keeps filtering inside the texture region
} GVtx;

static SDL_Window *win;
static SDL_GPUDevice *dev;
static bool ready;
static SDL_GPUShader *draw_vs, *draw_fs, *blit_vs, *blit_fs, *present_vs, *present_fs;
static SDL_GPUGraphicsPipeline *pipe_draw[5];   // opaque, then semi-transparency modes 0-3
static SDL_GPUGraphicsPipeline *pipe_blit, *pipe_seed, *pipe_present;
static SDL_GPUSampler *smp_linear, *smp_nearest;
// 1x VRAM and CPU transfers (as they were at transfer time) are read by the shaders from
// storage buffers, two 16-bit pixels per word: SDL_GPU cannot sample integer textures.
static SDL_GPUBuffer *buf_vram;
static SDL_GPUBuffer *buf_upload;
static uint32_t buf_upload_size;
static SDL_GPUTexture *tex_hi;                    // high-resolution VRAM
static SDL_GPUTexture *tex_tmp;                   // scratch for VRAM-to-VRAM copies; MSAA resolve target
// Anti-aliasing: drawing goes to a multisampled texture that only lives for one render pass. It
// is seeded with the current image, drawn to, and resolved into tex_tmp, which then takes the
// place of the target (tile-based GPUs keep the samples on chip, so this costs about what loading
// and storing the target does).
static SDL_GPUTexture *tex_msaa;
static SDL_GPUSampleCount msaa_samples = SDL_GPU_SAMPLECOUNT_1;
static int msaa_applied;                          // render_cfg.msaa the targets were made for
static SDL_GPUTexture *tex_interp;                // in-between frame
static SDL_GPUTexture *tex_disp;                  // 24-bit / software display path
static SDL_GPUBuffer *vbuf;
static uint32_t vbuf_size;
static SDL_GPUTransferBuffer *xfer;
static uint32_t xfer_size;
static SDL_GPUCommandBuffer *cmd;                 // recording; submitted at the next present
static int scale = 1;
static bool vram_dirty = true;

bool widescreen_active;
static int frame_precise_vertices;

RenderConfig render_cfg = {.hw = true, .filter = true, .widescreen = true, .scale = 0, .interpolate = true, .msaa = 4};

static SDL_GPUCommandBuffer *command_buffer(void) {
    if (!cmd && !(cmd = SDL_AcquireGPUCommandBuffer(dev))) host_fatal("GPU: %s", SDL_GetError());
    return cmd;
}

// Maps `size` bytes of upload space. Cycling gives a fresh buffer while earlier uploads from it
// are still pending.
static uint8_t *map_upload(uint32_t size) {
    if (size > xfer_size) {
        if (xfer) SDL_ReleaseGPUTransferBuffer(dev, xfer);
        xfer_size = size + size / 2;
        SDL_GPUTransferBufferCreateInfo ci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = xfer_size};
        if (!(xfer = SDL_CreateGPUTransferBuffer(dev, &ci))) host_fatal("GPU: %s", SDL_GetError());
    }
    return SDL_MapGPUTransferBuffer(dev, xfer, true);
}

static void upload_texture(SDL_GPUCopyPass *cp, uint32_t offset, SDL_GPUTexture *t, int x, int y, int w, int h, bool cycle) {
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = xfer, .offset = offset, .pixels_per_row = (Uint32)w,
                                      .rows_per_layer = (Uint32)h};
    SDL_GPUTextureRegion dst = {.texture = t, .x = (Uint32)x, .y = (Uint32)y, .w = (Uint32)w, .h = (Uint32)h, .d = 1};
    SDL_UploadToGPUTexture(cp, &src, &dst, cycle);
}

// --------------------------------------------------------- recorded GPU work
// Drawing is collected as vertices plus a command list and executed in as few render passes
// as possible (each pass loads and stores the whole high-resolution target): one copy pass
// uploads VRAM, vertices and transferred pixels first, and a render pass is only split for
// VRAM-to-VRAM copies.
typedef enum { G_DRAW, G_BLIT, G_COPY } GCmdType;
typedef struct {
    uint8_t type;
    int8_t blend;             // draws: -1 opaque, 0-3 semi-transparency mode
    bool upload;              // blits: the transferred pixels instead of VRAM
    int x, y, w, h;           // draws: scissor; blits: rectangle; copies: source
    int dx, dy;               // copies: destination
    uint32_t first, count;    // draws: vertex range
    const uint16_t *pixels;   // uploads
    uint32_t base;            // uploads: first pixel in buf_upload
} GCmd;

static GVtx *verts;
static int nverts, verts_cap;
static GCmd *cmds;
static int ncmds, cmds_cap;

static GCmd *new_cmd(GCmdType t) {
    if (ncmds == cmds_cap) {
        cmds_cap = cmds_cap ? cmds_cap * 2 : 1024;
        cmds = realloc(cmds, sizeof(GCmd) * (size_t)cmds_cap);
    }
    GCmd *c = &cmds[ncmds++];
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)t;
    return c;
}

static GVtx *new_verts(int n) {
    if (nverts + n > verts_cap) {
        verts_cap = (nverts + n) * 2;
        verts = realloc(verts, sizeof(GVtx) * (size_t)verts_cap);
    }
    GVtx *v = &verts[nverts];
    nverts += n;
    cmds[ncmds - 1].count += (uint32_t)n;
    return v;
}

static void begin_draw(int blend, int x, int y, int w, int h) {
    GCmd *c = ncmds ? &cmds[ncmds - 1] : NULL;
    if (c && c->type == G_DRAW && c->blend == blend && c->x == x && c->y == y && c->w == w && c->h == h) return;
    c = new_cmd(G_DRAW);
    c->blend = (int8_t)blend;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->first = (uint32_t)nverts;
}

static SDL_GPURenderPass *begin_pass(SDL_GPUTexture *target) {
    if (msaa_samples == SDL_GPU_SAMPLECOUNT_1) {
        SDL_GPUColorTargetInfo ct = {.texture = target, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE};
        return SDL_BeginGPURenderPass(command_buffer(), &ct, 1, NULL);
    }
    SDL_GPUColorTargetInfo ct = {.texture = tex_msaa, .load_op = SDL_GPU_LOADOP_DONT_CARE,
                                 .store_op = SDL_GPU_STOREOP_RESOLVE, .resolve_texture = tex_tmp};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(command_buffer(), &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, pipe_seed);
    float whole[4] = {0, 0, 1, 1};
    SDL_PushGPUVertexUniformData(command_buffer(), 0, whole, sizeof whole);
    SDL_GPUTextureSamplerBinding b = {.texture = target, .sampler = smp_nearest};
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
    return rp;
}

// With MSAA the pass resolved into tex_tmp, which becomes the target.
static void end_pass(SDL_GPURenderPass *rp, SDL_GPUTexture **target) {
    SDL_EndGPURenderPass(rp);
    if (msaa_samples != SDL_GPU_SAMPLECOUNT_1) {
        SDL_GPUTexture *t = *target;
        *target = tex_tmp;
        tex_tmp = t;
    }
}

static void bind_pixels(SDL_GPURenderPass *rp, SDL_GPUBuffer *b) {
    SDL_BindGPUFragmentStorageBuffers(rp, 0, &b, 1);
}

static SDL_GPUBuffer *make_buffer(SDL_GPUBufferUsageFlags usage, uint32_t size) {
    SDL_GPUBufferCreateInfo ci = {.usage = usage, .size = size};
    SDL_GPUBuffer *b = SDL_CreateGPUBuffer(dev, &ci);
    if (!b) host_fatal("GPU: %s", SDL_GetError());
    return b;
}

static void upload_buffer(SDL_GPUCopyPass *cp, uint32_t offset, SDL_GPUBuffer *b, uint32_t size) {
    SDL_GPUTransferBufferLocation src = {.transfer_buffer = xfer, .offset = offset};
    SDL_GPUBufferRegion dst = {.buffer = b, .offset = 0, .size = size};
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
}

// Clips drawing to a VRAM rectangle; false if nothing of it is inside VRAM.
static bool set_scissor(SDL_GPURenderPass *rp, int x, int y, int w, int h) {
    int x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    int x2 = x + w > 1024 ? 1024 : x + w, y2 = y + h > 512 ? 512 : y + h;
    if (x2 <= x1 || y2 <= y1) return false;
    SDL_Rect r = {x1 * scale, y1 * scale, (x2 - x1) * scale, (y2 - y1) * scale};
    SDL_SetGPUScissor(rp, &r);
    return true;
}

static void copy_region(SDL_GPUTexture *from, int sx, int sy, SDL_GPUTexture *to, int dx, int dy, int w, int h) {
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(command_buffer());
    SDL_GPUTextureLocation src = {.texture = from, .x = (Uint32)sx, .y = (Uint32)sy};
    SDL_GPUTextureLocation dst = {.texture = to, .x = (Uint32)dx, .y = (Uint32)dy};
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)w, (Uint32)h, 1, false);
    SDL_EndGPUCopyPass(cp);
}

// Runs the recorded commands against *target (tex_hi or tex_interp).
static void execute(SDL_GPUTexture **target) {
    if (!ncmds) { nverts = 0; return; }
    uint32_t vram_bytes = vram_dirty ? 1024 * 512 * 2 : 0, vert_bytes = (uint32_t)nverts * sizeof(GVtx);
    uint32_t px = 0;   // transferred pixels, packed one after the other
    for (int i = 0; i < ncmds; i++)
        if (cmds[i].type == G_BLIT && cmds[i].upload) { cmds[i].base = px; px += (uint32_t)(cmds[i].w * cmds[i].h); }
    uint32_t px_bytes = (px * 2 + 3) & ~3u;
    if (vert_bytes > vbuf_size) {
        if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
        vbuf_size = vert_bytes + vert_bytes / 2;
        vbuf = make_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vbuf_size);
    }
    if (px_bytes > buf_upload_size) {
        if (buf_upload) SDL_ReleaseGPUBuffer(dev, buf_upload);
        buf_upload_size = px_bytes + px_bytes / 2;
        buf_upload = make_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, buf_upload_size);
    }
    if (vram_bytes + vert_bytes + px_bytes) {
        uint8_t *m = map_upload(vram_bytes + vert_bytes + px_bytes);
        if (vram_bytes) memcpy(m, vram, vram_bytes);
        memcpy(m + vram_bytes, verts, vert_bytes);
        for (int i = 0; i < ncmds; i++)
            if (cmds[i].type == G_BLIT && cmds[i].upload)
                memcpy(m + vram_bytes + vert_bytes + cmds[i].base * 2, cmds[i].pixels, (size_t)cmds[i].w * cmds[i].h * 2);
        SDL_UnmapGPUTransferBuffer(dev, xfer);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(command_buffer());
        if (vram_bytes) upload_buffer(cp, 0, buf_vram, vram_bytes);
        if (vert_bytes) upload_buffer(cp, vram_bytes, vbuf, vert_bytes);
        if (px_bytes) upload_buffer(cp, vram_bytes + vert_bytes, buf_upload, px_bytes);
        SDL_EndGPUCopyPass(cp);
        vram_dirty = false;
    }

    SDL_GPURenderPass *rp = NULL;
    int bound = -3;   // pipeline: -2 blit, -1 opaque draw, 0-3 blended draw
    for (int i = 0; i < ncmds; i++) {
        GCmd *c = &cmds[i];
        if (c->type == G_COPY) {
            if (rp) { end_pass(rp, target); rp = NULL; }
            copy_region(*target, c->x * scale, c->y * scale, tex_tmp, 0, 0, c->w * scale, c->h * scale);
            copy_region(tex_tmp, 0, 0, *target, c->dx * scale, c->dy * scale, c->w * scale, c->h * scale);
            continue;
        }
        if (c->type == G_DRAW && !c->count) continue;
        if (!rp) {
            rp = begin_pass(*target);
            bound = -3;
        }
        if (c->type == G_BLIT) {
            if (bound != -2) SDL_BindGPUGraphicsPipeline(rp, pipe_blit);
            bound = -2;
            struct { float rect[4]; uint32_t src[4]; } u = {
                {(float)c->x, (float)c->y, (float)c->w, (float)c->h},
                {c->upload ? c->base : 0, c->upload ? (uint32_t)c->w : 1024, c->upload ? (uint32_t)c->x : 0,
                 c->upload ? (uint32_t)c->y : 0}};
            SDL_PushGPUVertexUniformData(command_buffer(), 0, &u, sizeof u);
            bind_pixels(rp, c->upload ? buf_upload : buf_vram);
            set_scissor(rp, 0, 0, 1024, 512);
            SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
            continue;
        }
        if (bound != c->blend) {
            SDL_BindGPUGraphicsPipeline(rp, pipe_draw[c->blend + 1]);
            if (c->blend == 0 || c->blend == 3) {
                float k = c->blend == 0 ? 0.5f : 0.25f;
                SDL_SetGPUBlendConstants(rp, (SDL_FColor){k, k, k, k});
            }
            SDL_GPUBufferBinding vb = {.buffer = vbuf, .offset = 0};
            SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
            bind_pixels(rp, buf_vram);
            bound = c->blend;
        }
        if (set_scissor(rp, c->x, c->y, c->w, c->h)) SDL_DrawGPUPrimitives(rp, c->count, 1, c->first, 0);
    }
    if (rp) end_pass(rp, target);
    nverts = 0;
    ncmds = 0;
}

// ------------------------------------------------------------ drawing
// Draw area for the triangles being recorded.
static struct { int x1, y1, x2, y2; } pending;

void hw_set_draw_area(int x1, int y1, int x2, int y2) {
    pending.x1 = x1; pending.y1 = y1; pending.x2 = x2; pending.y2 = y2;
}

// Texture region the current primitive samples (umin | vmin << 8 | umax << 16 | vmax << 24):
// bilinear filtering is kept inside it so neighbouring textures on the page do not bleed in.
// It covers the whole primitive, so the triangles of a quad filter alike along their diagonal.
static uint32_t cur_uvlim;
// The PlayStation samples the texel at each pixel's top-left corner. Where a flat (2D) primitive
// maps its texture mirrored (u falling as x rises, as for flipped sprites), sampling inside the
// pixel at high resolution lands one texel short and shows the neighbouring texel as a line along
// the right or bottom edge; such primitives sample one texel step further on that axis.
static float cur_uoff, cur_voff;

static void push(GVtx *o, const HwVertex *v, const HwPrim *p, uint32_t pass) {
    o->x = v->x; o->y = v->y; o->w = v->w;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = 255;
    o->u = v->u + cur_uoff; o->v = v->v + cur_voff;
    o->tpage = (uint32_t)p->tex_x | ((uint32_t)p->tex_y << 16);
    o->clut = (uint32_t)p->clut_x | ((uint32_t)p->clut_y << 16);
    o->texwin = p->texwin;
    o->uvlim = cur_uvlim;
    o->flags = (uint32_t)p->depth | (p->textured ? 4u : 0) | (p->raw ? 8u : 0) | (pass << 4) |
               (p->textured && render_cfg.filter && p->filterable ? 64u : 0);
}

static void draw_tri(const HwVertex v[3], const HwPrim *p) {
    int x = pending.x1, y = pending.y1, w = pending.x2 - pending.x1 + 1, h = pending.y2 - pending.y1 + 1;
    if (p->semi && p->textured) {
        // opaque texels first, then translucent ones with blending, keeping primitive order
        begin_draw(-1, x, y, w, h);
        GVtx *o = new_verts(3);
        for (int i = 0; i < 3; i++) push(&o[i], &v[i], p, 1);
        begin_draw(p->semi_mode, x, y, w, h);
        o = new_verts(3);
        for (int i = 0; i < 3; i++) push(&o[i], &v[i], p, 2);
    } else {
        begin_draw(p->semi ? p->semi_mode : -1, x, y, w, h);
        GVtx *o = new_verts(3);
        for (int i = 0; i < 3; i++) push(&o[i], &v[i], p, 0);
    }
}

// Fill rectangle: an untextured quad clipped to the rectangle (fills ignore the draw area).
static void draw_fill(int x, int y, int w, int h, uint32_t rgb) {
    begin_draw(-1, x, y, w, h);
    GVtx *o = new_verts(6);
    const float c[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 6; i++) {
        memset(&o[i], 0, sizeof o[i]);
        o[i].x = (float)x + c[i][0] * (float)w; o[i].y = (float)y + c[i][1] * (float)h; o[i].w = 1.0f;
        o[i].r = rgb & 255; o[i].g = (rgb >> 8) & 255; o[i].b = (rgb >> 16) & 255; o[i].a = 255;
    }
}

static void blit_from_vram(int x, int y, int w, int h) {
    GCmd *c = new_cmd(G_BLIT);
    c->x = x; c->y = y; c->w = w; c->h = h;
}

// Draws CPU-transferred pixels (as they were at transfer time) into the target.
static void draw_upload(int x, int y, int w, int h, const uint16_t *pixels) {
    if (pixels && x + w <= 1024 && y + h <= 512) {
        GCmd *c = new_cmd(G_BLIT);
        c->x = x; c->y = y; c->w = w; c->h = h;
        c->upload = true;
        c->pixels = pixels;
        return;
    }
    // wrap-around uploads are split into up to four rectangles
    int w1 = x + w > 1024 ? 1024 - x : w, h1 = y + h > 512 ? 512 - y : h;
    blit_from_vram(x, y, w1, h1);
    if (w1 < w) blit_from_vram(0, y, w - w1, h1);
    if (h1 < h) blit_from_vram(x, 0, w1, h - h1);
    if (w1 < w && h1 < h) blit_from_vram(0, 0, w - w1, h - h1);
}

static void draw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512) {
        // rare wrapping copy: rebuild the destination from 1x VRAM
        blit_from_vram(dx, dy, w < 1024 - dx ? w : 1024 - dx, h < 512 - dy ? h : 512 - dy);
        return;
    }
    GCmd *c = new_cmd(G_COPY);
    c->x = sx; c->y = sy; c->dx = dx; c->dy = dy; c->w = w; c->h = h;
}

// ------------------------------------------------------------ frame recording
// High-resolution drawing is deferred: operations are recorded and rendered at the display
// flip, or earlier when the area being drawn is on screen. The game only ever sees the 1x
// software VRAM, so this is invisible to it. Seeing the whole frame first allows
//  - giving vertices the CPU copied from GTE results the precise position of the original
//    (closes seams between polygons), and
//  - replaying the frame with interpolated vertices to make an in-between frame.
typedef enum { OP_TRI, OP_FILL, OP_UPLOAD, OP_COPY } OpType;
typedef struct {
    uint8_t type;
    HwVertex v[3];
    HwPrim p;
    int a[6];           // draw area (x1,y1,x2,y2) for triangles; rectangle/colour for other ops
    uint16_t *pixels;   // uploads: the transferred data
} Op;

typedef struct { Op *ops; int n, cap; } OpList;
static OpList frame_ops, prev_ops;
static bool frame_has_keys;

static Op *record(OpType t) {
    if (frame_ops.n == frame_ops.cap) {
        frame_ops.cap = frame_ops.cap ? frame_ops.cap * 2 : 4096;
        frame_ops.ops = realloc(frame_ops.ops, sizeof(Op) * (size_t)frame_ops.cap);
    }
    Op *o = &frame_ops.ops[frame_ops.n++];
    o->type = (uint8_t)t;
    o->pixels = NULL;
    return o;
}

static void clear_ops(OpList *l) {
    for (int i = 0; i < l->n; i++) free(l->ops[i].pixels);
    l->n = 0;
}

void hw_triangle(const HwVertex v[3], const HwPrim *p) {
    if (!render_cfg.hw || !ready) return;
    Op *o = record(OP_TRI);
    memcpy(o->v, v, sizeof o->v);
    o->p = *p;
    o->a[0] = pending.x1; o->a[1] = pending.y1; o->a[2] = pending.x2; o->a[3] = pending.y2;
    if (v[0].key || v[1].key || v[2].key) frame_has_keys = true;
}

void hw_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    if (!ready) return;
    vram_dirty = true;
    if (!render_cfg.hw) return;
    Op *o = record(OP_FILL);
    o->a[0] = x; o->a[1] = y; o->a[2] = w; o->a[3] = h; o->a[4] = r | (g << 8) | (b << 16);
}

// A CPU-to-VRAM transfer finished (vram[] already holds the data).
void hw_upload(int x, int y, int w, int h) {
    if (!ready) return;
    vram_dirty = true;
    if (!render_cfg.hw) return;
    Op *o = record(OP_UPLOAD);
    o->a[0] = x; o->a[1] = y; o->a[2] = w; o->a[3] = h;
    if (x + w <= 1024 && y + h <= 512) {
        o->pixels = malloc((size_t)w * h * 2);
        for (int j = 0; j < h; j++) memcpy(o->pixels + j * w, vram + (y + j) * 1024 + x, (size_t)w * 2);
    }
}

void hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!ready) return;
    vram_dirty = true;
    if (!render_cfg.hw) return;
    Op *o = record(OP_COPY);
    o->a[0] = sx; o->a[1] = sy; o->a[2] = dx; o->a[3] = dy; o->a[4] = w; o->a[5] = h;
}

void hw_vram_changed(void) { vram_dirty = true; }

// Whole-frame vertex cache: vertices without precise data take the precise position of a
// GTE vertex at the same integer screen position in the same drawing area.
#define VC_SLOTS 32768
static struct { uint32_t xy, gen; int ox, oy; float x, y, w; uint32_t key; } vc[VC_SLOTS];
static uint32_t vc_gen = 1;

static uint32_t vc_hash(uint32_t xy, int ox, int oy) { return ((xy ^ (uint32_t)(ox * 131 + oy * 7919)) * 0x9E3779B1u) >> 17; }

static void resolve_frame(OpList *l) {
    vc_gen++;
    for (int i = 0; i < l->n; i++) {
        Op *o = &l->ops[i];
        if (o->type != OP_TRI) continue;
        for (int k = 0; k < 3; k++) {
            HwVertex *v = &o->v[k];
            if (!v->precise || !v->key) continue;
            uint32_t h = vc_hash(v->xyword, o->a[0], o->a[1]);
            for (int n = 0; n < 16; n++, h = (h + 1) & (VC_SLOTS - 1)) {
                if (vc[h].gen != vc_gen) {
                    vc[h].gen = vc_gen; vc[h].xy = v->xyword; vc[h].ox = o->a[0]; vc[h].oy = o->a[1];
                    vc[h].x = v->x - o->a[0]; vc[h].y = v->y - o->a[1]; vc[h].w = v->w; vc[h].key = v->key;
                    break;
                }
                if (vc[h].xy == v->xyword && vc[h].ox == o->a[0] && vc[h].oy == o->a[1]) break;
            }
        }
    }
    for (int i = 0; i < l->n; i++) {
        Op *o = &l->ops[i];
        if (o->type != OP_TRI) continue;
        for (int k = 0; k < 3; k++) {
            HwVertex *v = &o->v[k];
            if (v->precise || !pgxp_enabled) continue;
            uint32_t h = vc_hash(v->xyword, o->a[0], o->a[1]);
            for (int n = 0; n < 16; n++, h = (h + 1) & (VC_SLOTS - 1)) {
                if (vc[h].gen != vc_gen) break;
                if (vc[h].xy == v->xyword && vc[h].ox == o->a[0] && vc[h].oy == o->a[1]) {
                    v->x = o->a[0] + vc[h].x; v->y = o->a[1] + vc[h].y; v->key = vc[h].key;
                    v->w = vc[h].w;
                    v->precise = true;
                    break;
                }
            }
        }
        // perspective needs depth on all three vertices
        bool all = o->p.textured && o->v[0].precise && o->v[1].precise && o->v[2].precise &&
                   o->v[0].key && o->v[1].key && o->v[2].key;
        if (!all) for (int k = 0; k < 3; k++) o->v[k].w = 1.0f;
        else for (int k = 0; k < 3; k++) if (o->v[k].w == 1.0f) { for (int j = 0; j < 3; j++) o->v[j].w = 1.0f; break; }
    }
}

// Widescreen: the GTE already widened the 3D projection; 2D elements keep their 4:3
// proportions instead of being stretched, each anchored to the left edge, centre or right edge
// by which third of the screen it is in. A line of text is one element: consecutive 2D
// primitives of similar height side by side on the same line move as a group. Sprites
// placed in 3D (dust, smoke) keep their place and are narrowed; elements spanning the screen
// (menu bars) stretch to the full width. Decided after the vertex resolution above.
static void squeeze_ops(OpList *l, int i, int j, float anchor) {
    for (int t = i; t < j; t++)
        if (l->ops[t].type == OP_TRI)
            for (int k = 0; k < 3; k++) l->ops[t].v[k].x = anchor + (l->ops[t].v[k].x - anchor) * 0.75f;
}

static void widescreen_frame(OpList *l) {
    frame_precise_vertices = 0;
    for (int i = 0; i < l->n; i++)
        if (l->ops[i].type == OP_TRI)
            for (int k = 0; k < 3; k++) frame_precise_vertices += l->ops[i].v[k].key != 0;
    if (!widescreen_active) return;
    int gi = -1, gj = 0;   // open group of 2D primitives: ops [gi, gj)
    float gx1 = 0, gx2 = 0;
    float px1 = 0, px2 = 0, py1 = 0, py2 = 0;   // the group's last primitive
    const Op *ga = NULL;
    for (int i = 0; i <= l->n;) {
        Op *o = i < l->n ? &l->ops[i] : NULL;
        int j = i + 1;
        bool is2d = false;
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        if (o && o->type == OP_TRI) {
            bool any3d = false;
            for (j = i; j < l->n && l->ops[j].type == OP_TRI && l->ops[j].p.id == o->p.id; j++)
                for (int k = 0; k < 3; k++) {
                    HwVertex *v = &l->ops[j].v[k];
                    minx = fminf(minx, v->x); maxx = fmaxf(maxx, v->x);
                    miny = fminf(miny, v->y); maxy = fmaxf(maxy, v->y);
                    if (o->p.kind == PRIM_POLY && v->precise) any3d = true;
                }
            is2d = o->p.kind == PRIM_2D || (o->p.kind == PRIM_POLY && !any3d);
        }
        if (gi >= 0) {
            float hp = py2 - py1, hn = maxy - miny, h = fminf(hp, hn);
            bool joins = is2d && o->a[0] == ga->a[0] && o->a[2] == ga->a[2] && h >= 4 && fmaxf(hp, hn) <= 2 * h &&
                         miny < py2 && maxy > py1 && minx <= px2 + h && maxx >= px1 - h;
            if (joins) {
                gj = j;
                gx1 = fminf(gx1, minx); gx2 = fmaxf(gx2, maxx);
                px1 = minx; px2 = maxx; py1 = miny; py2 = maxy;
                i = j;
                continue;
            }
            float x1 = (float)ga->a[0], w = (float)(ga->a[2] - ga->a[0] + 1);
            float c = (gx1 + gx2) * 0.5f - x1;
            // elements spanning the screen (menu bars, backdrops) stretch to the full width
            if (gx2 - gx1 < w * 0.9f) squeeze_ops(l, gi, gj, c < w / 3 ? x1 : c > w * 2 / 3 ? x1 + w : x1 + w * 0.5f);
            gi = -1;
        }
        if (is2d) {
            gi = i; gj = j; ga = o;
            gx1 = px1 = minx; gx2 = px2 = maxx; py1 = miny; py2 = maxy;
        } else if (o && o->type == OP_TRI && o->p.kind == PRIM_SPRITE_3D) {
            squeeze_ops(l, i, j, (minx + maxx) * 0.5f);
        }
        i = j;
    }
}

// ----------------------------------------------------------- interpolation
#define MAP_SLOTS 65536
static struct { uint32_t key; float x, y, w; } vmap[MAP_SLOTS];
static uint32_t vmap_gen[MAP_SLOTS], map_gen = 1;
static bool interp_pending, presented_interp;
static int vblanks_since_flip, last_interval = 2;
int interp_stat_matched, interp_stat_total, interp_stat_found;

// Positions are stored relative to the drawing area origin: consecutive frames are drawn
// into different halves of VRAM. A key may occur several times (instanced models, vertices
// projected once per adjacent polygon strip); lookups pick the nearest occurrence.
static void map_put(uint32_t key, float x, float y, float w) {
    uint32_t i = (key * 0x9E3779B1u) >> 16;
    for (int n = 0; n < 64; n++, i = (i + 1) & (MAP_SLOTS - 1)) {
        if (vmap_gen[i] != map_gen) {
            vmap_gen[i] = map_gen; vmap[i].key = key; vmap[i].x = x; vmap[i].y = y; vmap[i].w = w;
            return;
        }
        if (vmap[i].key == key && vmap[i].x == x && vmap[i].y == y) return;
    }
}

static bool map_get(uint32_t key, float cx, float cy, float *x, float *y, float *w) {
    uint32_t i = (key * 0x9E3779B1u) >> 16;
    float best = 1e30f;
    for (int n = 0; n < 64; n++, i = (i + 1) & (MAP_SLOTS - 1)) {
        if (vmap_gen[i] != map_gen) break;
        if (vmap[i].key != key) continue;
        float dx = vmap[i].x - cx, dy = vmap[i].y - cy, d = dx * dx + dy * dy;
        if (d < best) { best = d; *x = vmap[i].x; *y = vmap[i].y; *w = vmap[i].w; }
    }
    return best < 1e30f;
}


// Per-vertex motion for the in-between frame, from (in order of preference) the vertex's own
// match in the previous frame, the average motion of matched vertices nearby (16-pixel grid)
// for other GTE vertices, and the motion of the rest of the polygon for CPU-built vertices.
#define GRID_W 64
#define GRID_H 32
static float grid_dx[GRID_H][GRID_W], grid_dy[GRID_H][GRID_W];
static int grid_n[GRID_H][GRID_W];
// dx/dy: offset from the current position back to the previous frame's; w: previous depth
typedef struct { float dx, dy, w; uint8_t state; } VMotion;   // 0 none, 1 matched, 2 estimated
static VMotion *motion;
static int motion_cap;
static bool motion_valid;   // prev_ops (the frame on screen) has a motion field

static void compute_motion(void) {
    map_gen++;
    for (int i = 0; i < prev_ops.n; i++) {
        Op *o = &prev_ops.ops[i];
        if (o->type != OP_TRI) continue;
        for (int k = 0; k < 3; k++)
            if (o->v[k].key) map_put(o->v[k].key, o->v[k].x - o->a[0], o->v[k].y - o->a[1], o->v[k].w);
    }
    if (motion_cap < frame_ops.n * 3) {
        motion_cap = frame_ops.n * 3 + 1024;
        motion = realloc(motion, sizeof(VMotion) * (size_t)motion_cap);
    }
    memset(grid_n, 0, sizeof grid_n);
    memset(grid_dx, 0, sizeof grid_dx);
    memset(grid_dy, 0, sizeof grid_dy);
    for (int i = 0; i < frame_ops.n; i++) {
        Op *o = &frame_ops.ops[i];
        if (o->type != OP_TRI) continue;
        for (int k = 0; k < 3; k++) {
            VMotion *m = &motion[i * 3 + k];
            HwVertex *v = &o->v[k];
            m->state = 0;
            if (!v->key) continue;
            interp_stat_total++;
            float cx = v->x - o->a[0], cy = v->y - o->a[1], px = 0, py = 0, pw = 1;
            if (!map_get(v->key, cx, cy, &px, &py, &pw)) continue;
            interp_stat_found++;
            float dx = cx - px, dy = cy - py;
            if (dx * dx + dy * dy > 64.0f * 64.0f) continue;   // probably a different vertex
            interp_stat_matched++;
            m->dx = -dx;
            m->dy = -dy;
            m->w = (v->w != 1.0f && pw != 1.0f) ? pw : v->w;
            m->state = 1;
            int gx = (int)cx >> 4, gy = (int)cy >> 4;
            if (gx >= 0 && gx < GRID_W && gy >= 0 && gy < GRID_H) {
                grid_dx[gy][gx] += m->dx; grid_dy[gy][gx] += m->dy; grid_n[gy][gx]++;
            }
        }
    }
    for (int i = 0; i < frame_ops.n; i++) {
        Op *o = &frame_ops.ops[i];
        if (o->type != OP_TRI) continue;
        bool has_key = o->v[0].key || o->v[1].key || o->v[2].key;
        for (int k = 0; k < 3; k++) {
            VMotion *m = &motion[i * 3 + k];
            HwVertex *v = &o->v[k];
            // GTE vertices without a match, and CPU-built vertices of 3D polygons
            if (m->state || !has_key) continue;
            int gx = (int)(v->x - o->a[0]) >> 4, gy = (int)(v->y - o->a[1]) >> 4;
            float sx = 0, sy = 0;
            int n = 0;
            for (int y = gy - 1; y <= gy + 1; y++)
                for (int x = gx - 1; x <= gx + 1; x++)
                    if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H && grid_n[y][x]) {
                        sx += grid_dx[y][x]; sy += grid_dy[y][x]; n += grid_n[y][x];
                    }
            if (n) { m->dx = sx / n; m->dy = sy / n; m->w = v->w; m->state = 2; }
        }
    }
}

// Renders a recorded frame into `target`. With alpha < 1 vertices are moved back along the
// motion field: alpha 0 is the previous frame's geometry, 1 the frame as recorded.
// The span [lo, hi] of texture coordinates covers an aligned 16..256-texel tile (polygons
// usually leave out its last texel).
static bool whole_tile(float lo, float hi) {
    for (int size = 16; size <= 256; size *= 2)
        if (lo == floorf(lo) && (int)lo % size == 0 && hi - lo >= size - 2.5f && hi - lo <= size + 0.5f) return true;
    return false;
}

static void set_uvlim(const OpList *l, int i) {
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    const HwVertex *left = NULL, *right = NULL, *top = NULL, *bottom = NULL;
    bool flat = true;
    for (int j = i; j < l->n && l->ops[j].type == OP_TRI && l->ops[j].p.id == l->ops[i].p.id; j++)
        for (int k = 0; k < 3; k++) {
            const HwVertex *v = &l->ops[j].v[k];
            umin = fminf(umin, v->u); umax = fmaxf(umax, v->u);
            vmin = fminf(vmin, v->v); vmax = fmaxf(vmax, v->v);
            if (!left || v->x < left->x) left = v;
            if (!right || v->x > right->x) right = v;
            if (!top || v->y < top->y) top = v;
            if (!bottom || v->y > bottom->y) bottom = v;
            flat &= v->w == 1.0f;
        }
    // mirrored axes of flat primitives: one texel step (at most a whole texel) further
    cur_uoff = cur_voff = 0.0f;
    if (flat && right->x > left->x && right->u < left->u)
        cur_uoff = fminf((left->u - right->u) / (right->x - left->x), 1.0f);
    if (flat && bottom->y > top->y && bottom->v < top->v)
        cur_voff = fminf((top->v - bottom->v) / (bottom->y - top->y), 1.0f);
    umin += cur_uoff; umax += cur_uoff;
    vmin += cur_voff; vmax += cur_voff;
    // texel coordinates are floored, so the last texel of a span is max - 1 when max is fractional
    int u0 = (int)umin, v0 = (int)vmin, u1 = (int)(umax - 0.001f), v1 = (int)(vmax - 0.001f);
    if (u1 < u0) u1 = u0;
    if (v1 < v0) v1 = v0;
    // Terrain and sky polygons each map a whole aligned tile whose texture continues on the
    // neighbouring polygon: clamping there leaves a seam along every polygon edge, so they filter
    // freely (one texel into the neighbouring tile at most). Atlases (the car) stay clamped.
    if (!flat && whole_tile(umin, umax) && whole_tile(vmin, vmax)) { u0 = 0; v0 = 0; u1 = 255; v1 = 255; }
    cur_uvlim = (uint32_t)(u0 & 255) | ((uint32_t)(v0 & 255) << 8) | ((uint32_t)(u1 & 255) << 16) | ((uint32_t)(v1 & 255) << 24);
}

static void render_ops(OpList *l, float alpha, SDL_GPUTexture **target) {
    bool moved = alpha < 1.0f;
    float back = 1.0f - alpha;
    int ax1 = pending.x1, ay1 = pending.y1, ax2 = pending.x2, ay2 = pending.y2;
    for (int i = 0; i < l->n; i++) {
        Op *o = &l->ops[i];
        switch (o->type) {
        case OP_TRI: {
            HwVertex v[3];
            memcpy(v, o->v, sizeof v);
            if (moved) {
                float sx = 0, sy = 0;
                int nm = 0;
                for (int k = 0; k < 3; k++) {
                    VMotion *m = &motion[i * 3 + k];
                    if (!m->state) continue;
                    v[k].x += m->dx * back; v[k].y += m->dy * back;
                    if (v[k].w != 1.0f && m->w != 1.0f) v[k].w = m->w + (v[k].w - m->w) * alpha;
                    sx += m->dx * back; sy += m->dy * back; nm++;
                }
                if (nm && nm < 3)
                    for (int k = 0; k < 3; k++)
                        if (!motion[i * 3 + k].state) { v[k].x += sx / nm; v[k].y += sy / nm; }
            }
            pending.x1 = o->a[0]; pending.y1 = o->a[1]; pending.x2 = o->a[2]; pending.y2 = o->a[3];
            if (!o->p.textured) cur_uoff = cur_voff = 0.0f;
            else if (i == 0 || l->ops[i - 1].type != OP_TRI || l->ops[i - 1].p.id != o->p.id) set_uvlim(l, i);
            draw_tri(v, &o->p);
            break;
        }
        case OP_FILL: draw_fill(o->a[0], o->a[1], o->a[2], o->a[3], (uint32_t)o->a[4]); break;
        case OP_UPLOAD: draw_upload(o->a[0], o->a[1], o->a[2], o->a[3], o->pixels); break;
        case OP_COPY: draw_copy(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5]); break;
        }
    }
    execute(target);
    pending.x1 = ax1; pending.y1 = ay1; pending.x2 = ax2; pending.y2 = ay2;
}

// Renders whatever has been recorded so far as a normal (not interpolated) frame.
static void update_widescreen(void) {
    const Module *m = module_active();
    bool gameplay = m && (!strcmp(m->name, "game") || !strcmp(m->name, "rschool")) && frame_precise_vertices > 300;
    widescreen_active = render_cfg.widescreen && render_cfg.hw && gameplay;
}

static void render_pending(void) {
    if (!frame_ops.n) return;
    resolve_frame(&frame_ops);
    widescreen_frame(&frame_ops);
    render_ops(&frame_ops, 1.0f, &tex_hi);
    clear_ops(&frame_ops);
    clear_ops(&prev_ops);
    frame_has_keys = false;
    motion_valid = false;
}

// Debug aid: CMR_DUMP_HW=<frame> logs the triangles of the first flip from that frame on, as the
// renderer draws them.
static void dump_frame(OpList *l) {
    static long want = -2;
    if (want == -2) want = getenv("CMR_DUMP_HW") ? atol(getenv("CMR_DUMP_HW")) : -1;
    if (want < 0 || (long)(cpu.cycles / CYCLES_PER_FRAME) < want) return;
    want = -1;
    for (int i = 0; i < l->n; i++) {
        Op *o = &l->ops[i];
        if (o->type != OP_TRI) continue;
        LOG("[hw] %d id=%u kind=%d semi=%d tex=%d depth=%d raw=%d page=%d,%d clut=%d,%d win=%05x area=%d,%d", i, o->p.id,
            o->p.kind, o->p.semi, o->p.textured, o->p.depth, o->p.raw, o->p.tex_x, o->p.tex_y, o->p.clut_x, o->p.clut_y,
            o->p.texwin, o->a[0], o->a[1]);
        for (int k = 0; k < 3; k++)
            LOG(" (%.3f,%.3f w=%.1f %s uv=%.2f,%.2f)", o->v[k].x - o->a[0], o->v[k].y - o->a[1], o->v[k].w,
                o->v[k].precise ? "p" : "i", o->v[k].u, o->v[k].v);
        LOG("\n");
    }
}

void hw_flip(void) {
    if (!ready || !render_cfg.hw) return;
    last_interval = vblanks_since_flip < 1 ? 1 : vblanks_since_flip;
    vblanks_since_flip = 0;
    resolve_frame(&frame_ops);
    widescreen_frame(&frame_ops);
    update_widescreen();
    dump_frame(&frame_ops);
    render_ops(&frame_ops, 1.0f, &tex_hi);
    motion_valid = render_cfg.interpolate && last_interval >= 2 && last_interval <= 4 && frame_has_keys && prev_ops.n;
    if (motion_valid) compute_motion();
    clear_ops(&prev_ops);
    OpList t = prev_ops;
    prev_ops = frame_ops;
    frame_ops = t;
    frame_has_keys = false;
}

// ------------------------------------------------------------- setup/present
static SDL_GPUTexture *make_texture(SDL_GPUTextureFormat fmt, int w, int h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage, .width = (Uint32)w,
                                   .height = (Uint32)h, .layer_count_or_depth = 1, .num_levels = 1};
    return SDL_CreateGPUTexture(dev, &ci);
}

static void create_draw_pipelines(void);

static void release_targets(void) {
    SDL_GPUTexture **t[4] = {&tex_hi, &tex_tmp, &tex_interp, &tex_msaa};
    for (int i = 0; i < 4; i++)
        if (*t[i]) { SDL_ReleaseGPUTexture(dev, *t[i]); *t[i] = NULL; }
}

static void create_targets(void) {
    // largest scale (up to the one wanted) the GPU accepts
    for (;; scale--) {
        release_targets();
        SDL_GPUTextureUsageFlags u = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex_hi = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1024 * scale, 512 * scale, u);
        tex_tmp = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1024 * scale, 512 * scale, u);
        tex_interp = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1024 * scale, 512 * scale, u);
        if (tex_hi && tex_tmp && tex_interp) break;
        if (scale == 1) host_fatal("GPU: cannot create the render targets: %s", SDL_GetError());
    }
    // the most samples (up to the number wanted) the GPU supports at this size
    msaa_applied = render_cfg.msaa;
    SDL_GPUSampleCount want = render_cfg.msaa >= 8 ? SDL_GPU_SAMPLECOUNT_8 : render_cfg.msaa >= 4 ? SDL_GPU_SAMPLECOUNT_4
                            : render_cfg.msaa >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    for (; want != SDL_GPU_SAMPLECOUNT_1; want--) {
        if (!SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, want)) continue;
        SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                       .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, .width = (Uint32)(1024 * scale),
                                       .height = (Uint32)(512 * scale), .layer_count_or_depth = 1, .num_levels = 1,
                                       .sample_count = want};
        if ((tex_msaa = SDL_CreateGPUTexture(dev, &ci))) break;
    }
    if (want != msaa_samples || !pipe_blit) {
        msaa_samples = want;
        create_draw_pipelines();
    }
    LOG("[render] internal resolution %dx%d (%dx), %s\n", 1024 * scale, 512 * scale, scale,
        msaa_samples == SDL_GPU_SAMPLECOUNT_1 ? "no anti-aliasing"
        : msaa_samples == SDL_GPU_SAMPLECOUNT_2 ? "2x MSAA" : msaa_samples == SDL_GPU_SAMPLECOUNT_4 ? "4x MSAA" : "8x MSAA");
    clear_ops(&frame_ops);
    clear_ops(&prev_ops);
    ncmds = nverts = 0;
    // seed the new target with the current 1x VRAM
    vram_dirty = true;
    blit_from_vram(0, 0, 1024, 512);
    execute(&tex_hi);
}

static int wanted_scale(void) {
    if (render_cfg.scale > 0) return render_cfg.scale > 16 ? 16 : render_cfg.scale;
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    int s = (h + 239) / 240;
    return s < 1 ? 1 : s > 16 ? 16 : s;
}

static SDL_GPUShader *shader(const uint8_t *spv, size_t spv_size, const uint8_t *msl, size_t msl_size,
                             SDL_GPUShaderStage stage, int samplers, int storage_buffers, int uniforms) {
    SDL_GPUShaderCreateInfo ci = {.stage = stage, .num_samplers = (Uint32)samplers,
                                  .num_storage_buffers = (Uint32)storage_buffers, .num_uniform_buffers = (Uint32)uniforms};
    if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV) {
        ci.code = spv; ci.code_size = spv_size; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.entrypoint = "main";
    } else {
        ci.code = msl; ci.code_size = msl_size; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.entrypoint = "main0";
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(dev, &ci);
    if (!s) host_fatal("GPU: shader: %s", SDL_GetError());
    return s;
}
#define SHADER(name, stage, samplers, storage_buffers, uniforms) \
    shader(name##_spv, sizeof name##_spv, name##_msl, sizeof name##_msl, stage, samplers, storage_buffers, uniforms)

// blend: -1 none, 0-3 PlayStation semi-transparency mode
static SDL_GPUGraphicsPipeline *pipeline(SDL_GPUShader *vs, SDL_GPUShader *fs, SDL_GPUTextureFormat format, int blend,
                                         bool vertices, SDL_GPUSampleCount samples) {
    SDL_GPUColorTargetDescription ct = {.format = format};
    if (blend >= 0) {
        SDL_GPUColorTargetBlendState *b = &ct.blend_state;
        b->enable_blend = true;
        // 0: B/2 + F/2, 1: B + F, 2: B - F, 3: B + F/4 (constants 0.5 and 0.25)
        b->color_blend_op = blend == 2 ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
        b->src_color_blendfactor = blend == 0 || blend == 3 ? SDL_GPU_BLENDFACTOR_CONSTANT_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        b->dst_color_blendfactor = blend == 0 ? SDL_GPU_BLENDFACTOR_CONSTANT_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        b->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        b->src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b->dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    }
    SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = sizeof(GVtx), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute attrs[5] = {
        {.location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(GVtx, x)},
        {.location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(GVtx, r)},
        {.location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(GVtx, u)},
        {.location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4, .offset = offsetof(GVtx, tpage)},
        {.location = 4, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = offsetof(GVtx, uvlim)},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE},
        .multisample_state = {.sample_count = samples},
        .target_info = {.color_target_descriptions = &ct, .num_color_targets = 1},
    };
    if (vertices) {
        ci.vertex_input_state.vertex_buffer_descriptions = &vb;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = attrs;
        ci.vertex_input_state.num_vertex_attributes = 5;
    }
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!p) host_fatal("GPU: pipeline: %s", SDL_GetError());
    return p;
}

// Pipelines drawing into the high-resolution target, for the sample count in use.
static void create_draw_pipelines(void) {
    SDL_GPUGraphicsPipeline **all[7] = {&pipe_draw[0], &pipe_draw[1], &pipe_draw[2], &pipe_draw[3], &pipe_draw[4],
                                        &pipe_blit, &pipe_seed};
    for (int i = 0; i < 7; i++)
        if (*all[i]) { SDL_ReleaseGPUGraphicsPipeline(dev, *all[i]); *all[i] = NULL; }
    const SDL_GPUTextureFormat f = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < 5; i++) pipe_draw[i] = pipeline(draw_vs, draw_fs, f, i - 1, true, msaa_samples);
    pipe_blit = pipeline(blit_vs, blit_fs, f, -1, false, msaa_samples);
    if (msaa_samples != SDL_GPU_SAMPLECOUNT_1) pipe_seed = pipeline(present_vs, present_fs, f, -1, false, msaa_samples);
}

bool render_init(SDL_Window *window) {
    win = window;
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, getenv("CMR_GPU_DEBUG") != NULL, NULL);
    if (!dev) { LOG("GPU: %s\n", SDL_GetError()); return false; }
    if (!SDL_ClaimWindowForGPUDevice(dev, win)) { LOG("GPU: %s\n", SDL_GetError()); return false; }
    LOG("[render] SDL_GPU %s\n", SDL_GetGPUDeviceDriver(dev));

    draw_vs = SHADER(draw_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    draw_fs = SHADER(draw_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    blit_vs = SHADER(blit_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
    blit_fs = SHADER(blit_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, 0);
    present_vs = SHADER(present_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
    present_fs = SHADER(present_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 0);
    pipe_present = pipeline(present_vs, present_fs, SDL_GetGPUSwapchainTextureFormat(dev, win), -1, false,
                            SDL_GPU_SAMPLECOUNT_1);

    SDL_GPUSamplerCreateInfo sc = {.min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
                                   .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                   .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                   .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                   .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    smp_linear = SDL_CreateGPUSampler(dev, &sc);
    sc.min_filter = sc.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp_nearest = SDL_CreateGPUSampler(dev, &sc);

    buf_vram = make_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, 1024 * 512 * 2);
    tex_disp = make_texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 640, 512, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!smp_linear || !smp_nearest || !tex_disp) {
        LOG("GPU: %s\n", SDL_GetError());
        return false;
    }
    ready = true;
    scale = wanted_scale();
    create_targets();
    return true;
}

bool render_set_vsync(bool on) {
    SDL_GPUPresentMode m = SDL_GPU_PRESENTMODE_VSYNC;
    if (!on) {   // mailbox does not tear; immediate where that is all there is (Metal)
        if (SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_MAILBOX)) m = SDL_GPU_PRESENTMODE_MAILBOX;
        else if (SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE)) m = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    if (!SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m)) {
        LOG("GPU: %s\n", SDL_GetError());
        return false;
    }
    return on == (m == SDL_GPU_PRESENTMODE_VSYNC);
}

int render_scale(void) { return scale; }

// Window viewport keeping the game's aspect ratio (4:3, or 16:9 in widescreen mode).
static void viewport(int ww, int wh, int *x, int *y, int *w, int *h) {
    float aspect = widescreen_active ? 16.0f / 9.0f : 4.0f / 3.0f;
    int vw = ww, vh = (int)(ww / aspect + 0.5f);
    if (vh > wh) { vh = wh; vw = (int)(wh * aspect + 0.5f); }
    *x = (ww - vw) / 2; *y = (wh - vh) / 2; *w = vw; *h = vh;
}

int render_frame_interval(void) { return last_interval; }
int render_vblanks_since_flip(void) { return vblanks_since_flip; }
void render_vblank(void) { vblanks_since_flip++; }

void render_present(const DisplayInfo *d, const uint32_t *rgba_1x, float alpha) {
    // Double-buffered games flip every frame; anything that draws without flipping
    // (loading screens, single-buffered menus) is shown after a short wait.
    if (render_cfg.hw && (vblanks_since_flip >= 4 || frame_ops.n > 200000)) render_pending();
    SDL_GPUTexture *src = NULL;
    float rect[4] = {0, 0, 0, 0};
    if (!d->enabled) {
        // black
    } else if (!render_cfg.hw || d->rgb24) {
        // software output (24-bit video or the software renderer)
        uint8_t *m = map_upload((uint32_t)(d->w * d->h * 4));
        memcpy(m, rgba_1x, (size_t)d->w * d->h * 4);
        SDL_UnmapGPUTransferBuffer(dev, xfer);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(command_buffer());
        upload_texture(cp, 0, tex_disp, 0, 0, d->w, d->h, false);
        SDL_EndGPUCopyPass(cp);
        src = tex_disp;
        rect[2] = d->w / 640.0f; rect[3] = d->h / 512.0f;
    } else {
        // In-between frame: the frame on screen, moved part of the way back to the previous one
        // (anything the replay leaves uncovered shows the finished frame underneath).
        interp_pending = motion_valid && alpha < 1.0f;
        if (interp_pending) {
            copy_region(tex_hi, 0, 0, tex_interp, 0, 0, 1024 * scale, 512 * scale);
            render_ops(&prev_ops, alpha, &tex_interp);
        }
        src = interp_pending ? tex_interp : tex_hi;
        rect[0] = d->x / 1024.0f; rect[1] = d->y / 512.0f; rect[2] = d->w / 1024.0f; rect[3] = d->h / 512.0f;
    }
    presented_interp = interp_pending;
    interp_pending = false;
    SDL_GPUCommandBuffer *cb = command_buffer();
    SDL_GPUTexture *swap = NULL;
    Uint32 ww = 0, wh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, win, &swap, &ww, &wh)) LOG("GPU: %s\n", SDL_GetError());
    if (swap) {   // NULL while the window is minimised or hidden
        SDL_GPUColorTargetInfo ct = {.texture = swap, .clear_color = {0, 0, 0, 1}, .load_op = SDL_GPU_LOADOP_CLEAR,
                                     .store_op = SDL_GPU_STOREOP_STORE};
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
        if (src) {
            int vx, vy, vw, vh;
            viewport((int)ww, (int)wh, &vx, &vy, &vw, &vh);
            SDL_GPUViewport v = {(float)vx, (float)vy, (float)vw, (float)vh, 0.0f, 1.0f};
            SDL_SetGPUViewport(rp, &v);
            SDL_BindGPUGraphicsPipeline(rp, pipe_present);
            SDL_PushGPUVertexUniformData(cb, 0, rect, sizeof rect);
            SDL_GPUTextureSamplerBinding b = {.texture = src, .sampler = smp_linear};
            SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
            SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
    }
    SDL_SubmitGPUCommandBuffer(cb);
    cmd = NULL;
    int s = wanted_scale();
    if ((s != scale || render_cfg.msaa != msaa_applied) && render_cfg.hw) {
        scale = s;
        create_targets();
    }
}

// Reads the displayed image at internal resolution (for screenshots).
bool render_read_display(const DisplayInfo *d, uint32_t **out, int *w, int *h) {
    if (!ready || !render_cfg.hw || d->rgb24 || !d->enabled) return false;
    *w = d->w * scale; *h = d->h * scale;
    uint32_t size = (uint32_t)*w * (uint32_t)*h * 4;
    SDL_GPUTransferBufferCreateInfo ci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = size};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ci);
    if (!tb) return false;
    SDL_GPUCommandBuffer *cb = command_buffer();
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion src = {.texture = presented_interp ? tex_interp : tex_hi, .x = (Uint32)(d->x * scale),
                                .y = (Uint32)(d->y * scale), .w = (Uint32)*w, .h = (Uint32)*h, .d = 1};
    SDL_GPUTextureTransferInfo dst = {.transfer_buffer = tb, .pixels_per_row = (Uint32)*w, .rows_per_layer = (Uint32)*h};
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    cmd = NULL;
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    *out = malloc(size);
    memcpy(*out, SDL_MapGPUTransferBuffer(dev, tb, false), size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return true;
}
