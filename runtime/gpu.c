// PlayStation GPU: command processing and a software rasterizer on 1024x512 VRAM.
#include "psx.h"
#include "pgxp.h"
#include "render.h"
#include <math.h>
#include <stdlib.h>

uint16_t vram[1024 * 512];

// draw state
static uint32_t texpage;           // E1 bits
static uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static int da_x1, da_y1, da_x2, da_y2;   // drawing area (inclusive)
static int off_x, off_y;
static bool mask_set, mask_check;
static bool rect_flip_x, rect_flip_y;

// display state
static uint32_t disp_x, disp_y, disp_h1 = 0x200, disp_h2 = 0xC00, disp_v1 = 0x10, disp_v2 = 0x100;
static uint32_t disp_mode;
static bool disp_enabled;
static uint32_t dma_dir;
static uint32_t gpu_info;
static bool gpu_irq;
static bool field_odd;

bool gpu_dither_enabled = true;

uint32_t gpu_stat_precise, gpu_stat_imprecise;
uint32_t gpu_mix_stats[3];
int gpu_dump_now;

// command fifo; cmd_addr holds the RAM address each word came from (DMA) for PGXP lookups
static uint32_t cmd[16];
static uint32_t cmd_addr[16];
static uint32_t word_addr = PGXP_NO_ADDR;
static int cmd_len, cmd_need;
static bool polyline;

// VRAM transfers
typedef struct { int x, y, w, h, cx, cy; bool active; } Transfer;
static Transfer upload, download;

void gpu_init(void) {
    memset(vram, 0, sizeof vram);
    texpage = 0;
    da_x1 = da_y1 = 0; da_x2 = 1023; da_y2 = 511;
    off_x = off_y = 0;
    disp_enabled = false;
    cmd_len = 0;
}

// ------------------------------------------------------------------ helpers
static inline int sext11(int v) { return (int)((uint32_t)v << 21) >> 21; }
static inline int min3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int max3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

static const int dither[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2},
};

static inline uint16_t blend(uint16_t back, uint16_t front, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r | (g << 5) | (b << 10) | (front & 0x8000));
}

typedef struct {
    int tex_depth;          // 0=4bit 1=8bit 2=15bit
    int tx, ty;             // texture page base
    int clut_x, clut_y;
    int semi_mode;
} TexInfo;

static inline uint16_t tex_fetch(const TexInfo *t, int u, int v) {
    u = (u & ~(int)(tw_mask_x * 8)) | (int)((tw_off_x & tw_mask_x) * 8);
    v = (v & ~(int)(tw_mask_y * 8)) | (int)((tw_off_y & tw_mask_y) * 8);
    u &= 255; v &= 255;
    int y = (t->ty + v) & 511;
    switch (t->tex_depth) {
    case 0: {
        uint16_t w = vram[y * 1024 + ((t->tx + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return vram[t->clut_y * 1024 + ((t->clut_x + idx) & 1023)];
    }
    case 1: {
        uint16_t w = vram[y * 1024 + ((t->tx + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return vram[t->clut_y * 1024 + ((t->clut_x + idx) & 1023)];
    }
    default:
        return vram[y * 1024 + ((t->tx + u) & 1023)];
    }
}

// Writes one pixel. col is 8-bit per channel unless `raw15` is used.
static inline void plot(int x, int y, uint16_t pix, bool semi, int semi_mode) {
    uint16_t *d = &vram[y * 1024 + x];
    if (mask_check && (*d & 0x8000)) return;
    if (semi) pix = blend(*d, pix, semi_mode);
    *d = pix | (mask_set ? 0x8000 : 0);
}

static inline uint16_t pack_rgb(int r, int g, int b, int x, int y, bool dith) {
    if (dith) {
        int d = dither[y & 3][x & 3];
        r += d; g += d; b += d;
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// Modulates a texel by an 8-bit colour (0x80 = identity), with optional dithering.
static inline uint16_t modulate(uint16_t texel, int r, int g, int b, int x, int y, bool dith) {
    int tr = (texel & 31) << 3, tg = ((texel >> 5) & 31) << 3, tb = ((texel >> 10) & 31) << 3;
    int mr = (tr * r) >> 7, mg = (tg * g) >> 7, mb = (tb * b) >> 7;
    if (dith) {
        int d = dither[y & 3][x & 3];
        mr += d; mg += d; mb += d;
        if (mr < 0) mr = 0;
        if (mg < 0) mg = 0;
        if (mb < 0) mb = 0;
    }
    mr = mr > 255 ? 255 : mr; mg = mg > 255 ? 255 : mg; mb = mb > 255 ? 255 : mb;
    return (uint16_t)((mr >> 3) | ((mg >> 3) << 5) | ((mb >> 3) << 10) | (texel & 0x8000));
}

// ------------------------------------------------------------- triangles
typedef struct { int x, y, r, g, b, u, v; float fx, fy, fw; bool precise; uint32_t key; uint32_t xy; } Vtx;

enum { F_SHADED = 1, F_TEXTURED = 2, F_SEMI = 4, F_RAW = 8 };

static void draw_triangle(Vtx p0, Vtx p1, Vtx p2, int flags, const TexInfo *t) {
    int64_t area = (int64_t)(p1.x - p0.x) * (p2.y - p0.y) - (int64_t)(p2.x - p0.x) * (p1.y - p0.y);
    if (area == 0) return;
    if (area < 0) { Vtx tmp = p1; p1 = p2; p2 = tmp; area = -area; }
    int minx = min3(p0.x, p1.x, p2.x), maxx = max3(p0.x, p1.x, p2.x);
    int miny = min3(p0.y, p1.y, p2.y), maxy = max3(p0.y, p1.y, p2.y);
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    if (minx < da_x1) minx = da_x1;
    if (miny < da_y1) miny = da_y1;
    if (maxx > da_x2) maxx = da_x2;
    if (maxy > da_y2) maxy = da_y2;
    if (minx > maxx || miny > maxy) return;

    // Edge functions E_ab(p) = (b.x-a.x)(p.y-a.y) - (b.y-a.y)(p.x-a.x); inside when all >= 0.
    const Vtx *a[3] = {&p0, &p1, &p2}, *b[3] = {&p1, &p2, &p0};
    int64_t e_row[3], e_dx[3], e_dy[3];
    int bias[3];
    for (int i = 0; i < 3; i++) {
        int dx = b[i]->x - a[i]->x, dy = b[i]->y - a[i]->y;
        e_dx[i] = -dy;
        e_dy[i] = dx;
        e_row[i] = (int64_t)dx * (miny - a[i]->y) - (int64_t)dy * (minx - a[i]->x);
        bool top_left = (dy < 0) || (dy == 0 && dx > 0);
        bias[i] = top_left ? 0 : -1;
    }

    // Attribute gradients in 16.16 fixed point.
    int64_t y10 = p1.y - p0.y, y20 = p2.y - p0.y, x10 = p1.x - p0.x, x20 = p2.x - p0.x;
#define GRAD(field, ddx, ddy) \
    int64_t ddx = (((int64_t)(p1.field - p0.field) * y20 - (int64_t)(p2.field - p0.field) * y10) << 16) / area; \
    int64_t ddy = (((int64_t)(p2.field - p0.field) * x10 - (int64_t)(p1.field - p0.field) * x20) << 16) / area;
    GRAD(r, drdx, drdy) GRAD(g, dgdx, dgdy) GRAD(b, dbdx, dbdy) GRAD(u, dudx, dudy) GRAD(v, dvdx, dvdy)
#undef GRAD
    bool shaded = flags & F_SHADED, textured = flags & F_TEXTURED, semi = flags & F_SEMI, raw = flags & F_RAW;
    bool dith = gpu_dither_enabled && (texpage & 0x200) && (shaded || (textured && !raw));
    int semi_mode = textured ? t->semi_mode : (int)((texpage >> 5) & 3);
    uint16_t flat = pack_rgb(p0.r, p0.g, p0.b, 0, 0, false);
    const int64_t half = 1 << 15;

    for (int y = miny; y <= maxy; y++) {
        int64_t e0 = e_row[0], e1 = e_row[1], e2 = e_row[2];
        int64_t ox = minx - p0.x, oy = y - p0.y;
        int64_t r = ((int64_t)p0.r << 16) + drdx * ox + drdy * oy + half;
        int64_t g = ((int64_t)p0.g << 16) + dgdx * ox + dgdy * oy + half;
        int64_t bl = ((int64_t)p0.b << 16) + dbdx * ox + dbdy * oy + half;
        int64_t u = ((int64_t)p0.u << 16) + dudx * ox + dudy * oy + half;
        int64_t v = ((int64_t)p0.v << 16) + dvdx * ox + dvdy * oy + half;
        bool entered = false;
        for (int x = minx; x <= maxx; x++) {
            if (e0 + bias[0] >= 0 && e1 + bias[1] >= 0 && e2 + bias[2] >= 0) {
                entered = true;
                int cr = 128, cg = 128, cb = 128;
                if (shaded) {
                    cr = (int)(r >> 16); cg = (int)(g >> 16); cb = (int)(bl >> 16);
                    cr = cr < 0 ? 0 : cr > 255 ? 255 : cr;
                    cg = cg < 0 ? 0 : cg > 255 ? 255 : cg;
                    cb = cb < 0 ? 0 : cb > 255 ? 255 : cb;
                } else {
                    cr = p0.r; cg = p0.g; cb = p0.b;
                }
                if (textured) {
                    int tu = (int)(u >> 16), tv = (int)(v >> 16);
                    tu = tu < 0 ? 0 : tu > 255 ? 255 : tu;
                    tv = tv < 0 ? 0 : tv > 255 ? 255 : tv;
                    uint16_t texel = tex_fetch(t, tu, tv);
                    if (texel) {
                        uint16_t pix = raw ? texel : modulate(texel, cr, cg, cb, x, y, dith);
                        plot(x, y, pix, semi && (texel & 0x8000), semi_mode);
                    }
                } else {
                    uint16_t pix = shaded ? pack_rgb(cr, cg, cb, x, y, dith) : flat;
                    plot(x, y, pix, semi, semi_mode);
                }
            } else if (entered) {
                break;
            }
            e0 += e_dx[0]; e1 += e_dx[1]; e2 += e_dx[2];
            r += drdx; g += dgdx; bl += dbdx; u += dudx; v += dvdx;
        }
        e_row[0] += e_dy[0]; e_row[1] += e_dy[1]; e_row[2] += e_dy[2];
    }
}

static TexInfo make_tex(uint32_t clut_word, uint32_t page_word) {
    TexInfo t;
    t.clut_x = (clut_word & 0x3F) * 16;
    t.clut_y = (clut_word >> 6) & 0x1FF;
    t.tx = (page_word & 0xF) * 64;
    t.ty = ((page_word >> 4) & 1) * 256;
    t.semi_mode = (page_word >> 5) & 3;
    t.tex_depth = (page_word >> 7) & 3;
    if (t.tex_depth == 3) t.tex_depth = 2;
    return t;
}

static uint32_t texwin_bits(void) {
    return tw_mask_x | (tw_mask_y << 5) | (tw_off_x << 10) | (tw_off_y << 15);
}

static HwVertex hw_vtx(const Vtx *v, bool perspective) {
    HwVertex h;
    h.x = v->precise ? v->fx : (float)v->x;
    h.y = v->precise ? v->fy : (float)v->y;
    h.w = perspective && v->precise ? v->fw : 1.0f;
    h.r = (uint8_t)v->r; h.g = (uint8_t)v->g; h.b = (uint8_t)v->b;
    h.u = (float)v->u; h.v = (float)v->v;
    h.key = v->precise ? v->key : 0;
    h.xyword = v->xy;
    h.precise = v->precise;
    h.clip = false;
    return h;
}

// A GTE vertex the game only has a clamped screen position for (too near or behind the camera).
static bool clipped(const Vtx *v) {
    return v->precise && (v->fw <= 0.0f || fabsf(v->fx - (float)v->x) > 2.0f || fabsf(v->fy - (float)v->y) > 2.0f);
}

static void hw_tri(const Vtx *a, const Vtx *b, const Vtx *c, const HwPrim *p) {
    bool persp = p->textured;   // the renderer drops depth unless all three vertices have it
    bool clip = clipped(a) || clipped(b) || clipped(c);   // drawn in 3D if it can be (see resolve_frame)
    HwVertex v[3] = {hw_vtx(a, persp || clip), hw_vtx(b, persp || clip), hw_vtx(c, persp || clip)};
    v[0].clip = clipped(a); v[1].clip = clipped(b); v[2].clip = clipped(c);
    // Same size limit as the hardware: oversized polygons are not drawn at all. Polygons with a
    // vertex too near the camera are decided by the renderer, once it knows whether they can be
    // drawn in 3D (see resolve_frame).
    int minx = min3(a->x, b->x, c->x), maxx = max3(a->x, b->x, c->x);
    int miny = min3(a->y, b->y, c->y), maxy = max3(a->y, b->y, c->y);
    if ((maxx - minx >= 1024 || maxy - miny >= 512) && !v[0].clip && !v[1].clip && !v[2].clip) return;
    hw_triangle(v, p);
}

static uint32_t prim_counter;
static bool rect_3d;   // the sprite's position word came from the GTE

static HwPrim hw_prim(const TexInfo *t, bool textured, bool raw, bool semi, bool filterable) {
    HwPrim p;
    p.id = ++prim_counter;
    p.kind = PRIM_POLY;
    p.textured = textured; p.raw = raw; p.semi = semi; p.filterable = filterable;
    p.semi_mode = textured ? t->semi_mode : (int)((texpage >> 5) & 3);
    p.depth = t->tex_depth;
    p.tex_x = t->tx; p.tex_y = t->ty; p.clut_x = t->clut_x; p.clut_y = t->clut_y;
    p.texwin = texwin_bits();
    return p;
}

static void cmd_polygon(void) {
    uint32_t op = cmd[0] >> 24;
    bool quad = op & 8, shaded = op & 0x10, textured = op & 4, semi = op & 2, raw = op & 1;
    int n = quad ? 4 : 3;
    Vtx v[4];
    uint32_t clut = 0, page = texpage;
    // [cmd+col0] v0 [uv0+clut] ([col1]) v1 [uv1+page] ([col2]) v2 [uv2] ...
    int k = 1;
    for (int i = 0; i < n; i++) {
        uint32_t col = (i > 0 && shaded) ? cmd[k++] : cmd[0];
        uint32_t xy_addr = cmd_addr[k];
        uint32_t xy = cmd[k++];
        v[i].r = col & 0xFF; v[i].g = (col >> 8) & 0xFF; v[i].b = (col >> 16) & 0xFF;
        v[i].x = sext11(sext11(xy & 0x7FF) + off_x);
        v[i].y = sext11(sext11((xy >> 16) & 0x7FF) + off_y);
        v[i].precise = pgxp_lookup(xy_addr, xy, &v[i].fx, &v[i].fy, &v[i].fw, &v[i].key);
        // the final integer position (after the drawing offset) identifies the vertex on screen
        v[i].xy = (uint32_t)(uint16_t)v[i].x | ((uint32_t)(uint16_t)v[i].y << 16);
        if (v[i].precise) { v[i].fx += off_x; v[i].fy += off_y; gpu_stat_precise++; }
        else gpu_stat_imprecise++;
        v[i].u = v[i].v = 0;
        if (textured) {
            uint32_t uv = cmd[k++];
            v[i].u = uv & 0xFF; v[i].v = (uv >> 8) & 0xFF;
            if (i == 0) clut = uv >> 16;
            if (i == 1) page = uv >> 16;
        }
    }
    if (textured) {
        // the polygon's texpage updates the global one
        texpage = (texpage & ~0x9FFu) | (page & 0x9FF);
    }
    TexInfo t = make_tex(clut, page);
    int flags = (shaded ? F_SHADED : 0) | (textured ? F_TEXTURED : 0) | (semi ? F_SEMI : 0) | (raw ? F_RAW : 0);
    if (textured && (texpage & 0x800)) flags &= ~F_TEXTURED;   // texture disable
    draw_triangle(v[0], v[1], v[2], flags, &t);
    if (quad) draw_triangle(v[1], v[2], v[3], flags, &t);
    {
        extern uint32_t gpu_mix_stats[3];
        int np = 0;
        for (int i = 0; i < n; i++) np += v[i].precise;
        gpu_mix_stats[np == 0 ? 0 : np == n ? 2 : 1]++;
    }
    HwPrim hp = hw_prim(&t, flags & F_TEXTURED, raw, semi, true);
    if (!shaded) for (int i = 1; i < n; i++) { v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b; }
    hw_tri(&v[0], &v[1], &v[2], &hp);
    if (quad) hw_tri(&v[1], &v[2], &v[3], &hp);
}

// -------------------------------------------------------------------- lines
static void hw_line(Vtx a, Vtx b, bool shaded, bool semi) {
    // a one-pixel-wide quad covering the line, endpoints included
    if (!shaded) { b.r = a.r; b.g = a.g; b.b = a.b; }
    int dx = b.x - a.x, dy = b.y - a.y;
    float ax = (float)a.x, ay = (float)a.y, bx = (float)b.x, by = (float)b.y;
    Vtx q[4] = {a, b, a, b};
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        if (bx >= ax) bx += 1; else ax += 1;
        q[0].fx = ax; q[0].fy = ay; q[1].fx = bx; q[1].fy = by;
        q[2].fx = ax; q[2].fy = ay + 1; q[3].fx = bx; q[3].fy = by + 1;
    } else {
        if (by >= ay) by += 1; else ay += 1;
        q[0].fx = ax; q[0].fy = ay; q[1].fx = bx; q[1].fy = by;
        q[2].fx = ax + 1; q[2].fy = ay; q[3].fx = bx + 1; q[3].fy = by;
    }
    for (int i = 0; i < 4; i++) { q[i].precise = true; q[i].key = 0; q[i].xy = 0; }
    TexInfo t = {0};
    HwPrim p = hw_prim(&t, false, false, semi, false);
    p.kind = PRIM_2D;
    hw_tri(&q[0], &q[1], &q[2], &p);
    hw_tri(&q[1], &q[2], &q[3], &p);
}

static void draw_line(Vtx a, Vtx b, bool shaded, bool semi) {
    hw_line(a, b, shaded, semi);
    int dx = b.x - a.x, dy = b.y - a.y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= 1024 || ady >= 512) return;
    int steps = adx > ady ? adx : ady;
    bool dith = gpu_dither_enabled && (texpage & 0x200) && shaded;
    int semi_mode = (texpage >> 5) & 3;
    for (int i = 0; i <= steps; i++) {
        int x, y, r = a.r, g = a.g, bl = a.b;
        if (steps) {
            x = a.x + (dx * i * 2 + (dx >= 0 ? steps : -steps)) / (steps * 2);
            y = a.y + (dy * i * 2 + (dy >= 0 ? steps : -steps)) / (steps * 2);
            if (shaded) {
                r = a.r + (b.r - a.r) * i / steps;
                g = a.g + (b.g - a.g) * i / steps;
                bl = a.b + (b.b - a.b) * i / steps;
            }
        } else {
            x = a.x; y = a.y;
        }
        if (x < da_x1 || x > da_x2 || y < da_y1 || y > da_y2) continue;
        plot(x, y, pack_rgb(r, g, bl, x, y, dith), semi, semi_mode);
    }
}

static Vtx line_vtx(uint32_t col, uint32_t xy) {
    Vtx v = {0};
    v.r = col & 0xFF; v.g = (col >> 8) & 0xFF; v.b = (col >> 16) & 0xFF;
    v.x = sext11(sext11(xy & 0x7FF) + off_x);
    v.y = sext11(sext11((xy >> 16) & 0x7FF) + off_y);
    return v;
}

// -------------------------------------------------------------- rectangles
static void draw_rect(int x0, int y0, int w, int h, uint32_t col, int u0, int v0, uint32_t clut, bool textured,
                      bool semi, bool raw) {
    TexInfo t = make_tex(clut, texpage);
    int r = col & 0xFF, g = (col >> 8) & 0xFF, b = (col >> 16) & 0xFF;
    uint16_t flat = pack_rgb(r, g, b, 0, 0, false);
    int semi_mode = (texpage >> 5) & 3;
    if (texpage & 0x800) textured = false;
    {
        // UVs at the corners such that each pixel centre samples the same texel as hardware
        float ua = rect_flip_x ? u0 + 1.0f : (float)u0, ub = rect_flip_x ? u0 + 1.0f - w : (float)(u0 + w);
        float va = rect_flip_y ? v0 + 1.0f : (float)v0, vb = rect_flip_y ? v0 + 1.0f - h : (float)(v0 + h);
        Vtx q[4];
        float xs[4] = {(float)x0, (float)(x0 + w), (float)x0, (float)(x0 + w)};
        float ys[4] = {(float)y0, (float)y0, (float)(y0 + h), (float)(y0 + h)};
        float us[4] = {ua, ub, ua, ub}, vs[4] = {va, va, vb, vb};
        for (int i = 0; i < 4; i++) {
            q[i] = (Vtx){x0, y0, r, g, b, 0, 0, xs[i], ys[i], 1.0f, true, 0, 0};
        }
        HwPrim p = hw_prim(&t, textured, raw, semi, false);
        p.semi_mode = semi_mode;
        p.kind = rect_3d ? PRIM_SPRITE_3D : PRIM_2D;
        HwVertex hv[4];
        for (int i = 0; i < 4; i++) { hv[i] = hw_vtx(&q[i], false); hv[i].u = us[i]; hv[i].v = vs[i]; }
        if (w > 0 && h > 0) {
            HwVertex t1[3] = {hv[0], hv[1], hv[2]}, t2[3] = {hv[1], hv[2], hv[3]};
            hw_triangle(t1, &p);
            hw_triangle(t2, &p);
        }
    }
    for (int yy = 0; yy < h; yy++) {
        int y = y0 + yy;
        if (y < da_y1 || y > da_y2) continue;
        int tv = rect_flip_y ? v0 - yy : v0 + yy;
        for (int xx = 0; xx < w; xx++) {
            int x = x0 + xx;
            if (x < da_x1 || x > da_x2) continue;
            if (textured) {
                int tu = rect_flip_x ? u0 - xx : u0 + xx;
                uint16_t texel = tex_fetch(&t, tu & 0xFF, tv & 0xFF);
                if (!texel) continue;
                uint16_t pix = raw ? texel : modulate(texel, r, g, b, x, y, false);
                plot(x, y, pix, semi && (texel & 0x8000), semi_mode);
            } else {
                plot(x, y, flat, semi, semi_mode);
            }
        }
    }
}

static void cmd_rect(void) {
    uint32_t op = cmd[0] >> 24;
    bool textured = op & 4, semi = op & 2, raw = op & 1;
    int size = (op >> 3) & 3;
    int k = 1;
    {
        float fx, fy, fw;
        uint32_t key;
        rect_3d = pgxp_lookup(cmd_addr[k], cmd[k], &fx, &fy, &fw, &key);
    }
    uint32_t xy = cmd[k++];
    int x = sext11(sext11(xy & 0x7FF) + off_x);
    int y = sext11(sext11((xy >> 16) & 0x7FF) + off_y);
    int u = 0, v = 0;
    uint32_t clut = 0;
    if (textured) {
        uint32_t uv = cmd[k++];
        u = uv & 0xFF; v = (uv >> 8) & 0xFF; clut = uv >> 16;
    }
    int w, h;
    switch (size) {
    case 0: w = cmd[k] & 0x3FF; h = (cmd[k] >> 16) & 0x1FF; break;
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    draw_rect(x, y, w, h, cmd[0], u, v, clut, textured, semi, raw);
}

// ----------------------------------------------------------- VRAM transfers
static void vram_put(uint16_t pix) {
    if (!upload.active) return;
    int x = (upload.x + upload.cx) & 1023, y = (upload.y + upload.cy) & 511;
    uint16_t *d = &vram[y * 1024 + x];
    if (!(mask_check && (*d & 0x8000))) *d = pix | (mask_set ? 0x8000 : 0);
    if (++upload.cx >= upload.w) {
        upload.cx = 0;
        if (++upload.cy >= upload.h) {
            upload.active = false;
            hw_upload(upload.x, upload.y, upload.w, upload.h);
        }
    }
}

static uint16_t vram_get(void) {
    if (!download.active) return 0;
    uint16_t v = vram[((download.y + download.cy) & 511) * 1024 + ((download.x + download.cx) & 1023)];
    if (++download.cx >= download.w) {
        download.cx = 0;
        if (++download.cy >= download.h) download.active = false;
    }
    return v;
}

static void start_transfer(bool to_vram) {
    int x = cmd[1] & 0x3FF, y = (cmd[1] >> 16) & 0x1FF;
    int w = ((cmd[2] & 0xFFFF) - 1) & 0x3FF, h = (((cmd[2] >> 16) & 0xFFFF) - 1) & 0x1FF;
    w += 1; h += 1;
    Transfer t = {x, y, w, h, 0, 0, true};
    if (to_vram) upload = t;
    else download = t;
}

void gpu_vram_read(int x, int y, int w, int h, uint16_t *out) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) out[j * w + i] = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
}

// --------------------------------------------------------------- GP0 / GP1
static int cmd_size(uint32_t op) {
    if (op >= 0x20 && op < 0x40) {
        bool quad = op & 8, shaded = op & 0x10, textured = op & 4;
        int n = quad ? 4 : 3;
        return 1 + n * (1 + (textured ? 1 : 0)) + (shaded ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) {
        if (op & 8) return -1;   // polyline: variable
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) {
        int size = (op >> 3) & 3;
        return 2 + ((op & 4) ? 1 : 0) + (size == 0 ? 1 : 0);
    }
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    return 1;
}

uint32_t gpu_stat_prims, gpu_stat_uploads, gpu_stat_fills, gpu_stat_flips;

static void exec_cmd(void) {
    uint32_t op = cmd[0] >> 24;
    if (gpu_dump_now && op >= 0x20 && op < 0x80) {
        LOG("[cmd] off=%d,%d page=%03x:", off_x, off_y, texpage);
        for (int i = 0; i < cmd_size(op); i++) LOG(" %08x", cmd[i]);
        LOG("\n");
    }
    if (op >= 0x20 && op < 0x80) gpu_stat_prims++;
    else if (op >= 0xA0 && op < 0xC0) gpu_stat_uploads++;
    else if (op == 0x02) gpu_stat_fills++;
    if (op >= 0x20 && op < 0x40) { cmd_polygon(); return; }
    if (op >= 0x40 && op < 0x60) {
        bool shaded = op & 0x10;
        Vtx a = line_vtx(cmd[0], cmd[1]);
        Vtx b = line_vtx(shaded ? cmd[2] : cmd[0], shaded ? cmd[3] : cmd[2]);
        draw_line(a, b, shaded, op & 2);
        return;
    }
    if (op >= 0x60 && op < 0x80) { cmd_rect(); return; }
    switch (op) {
    case 0x02: {   // fill rectangle (ignores drawing area and mask)
        int x = cmd[1] & 0x3F0, y = (cmd[1] >> 16) & 0x1FF;
        int w = ((cmd[2] & 0x3FF) + 15) & ~15, h = (cmd[2] >> 16) & 0x1FF;
        uint16_t c = pack_rgb(cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF, (cmd[0] >> 16) & 0xFF, 0, 0, false);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = c;
        hw_fill(x, y, x + w > 1024 ? 1024 - x : w, y + h > 512 ? 512 - y : h, cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF,
                (cmd[0] >> 16) & 0xFF);
        break;
    }
    case 0x1F: gpu_irq = true; irq_raise(IRQ_GPU); break;
    case 0xE1:
        texpage = cmd[0] & 0x3FFF;
        rect_flip_x = cmd[0] & (1 << 12);
        rect_flip_y = cmd[0] & (1 << 13);
        break;
    case 0xE2:
        tw_mask_x = cmd[0] & 31; tw_mask_y = (cmd[0] >> 5) & 31;
        tw_off_x = (cmd[0] >> 10) & 31; tw_off_y = (cmd[0] >> 15) & 31;
        break;
    case 0xE3: da_x1 = cmd[0] & 0x3FF; da_y1 = (cmd[0] >> 10) & 0x1FF; hw_set_draw_area(da_x1, da_y1, da_x2, da_y2); break;
    case 0xE4: da_x2 = cmd[0] & 0x3FF; da_y2 = (cmd[0] >> 10) & 0x1FF; hw_set_draw_area(da_x1, da_y1, da_x2, da_y2); break;
    case 0xE5: off_x = sext11(cmd[0] & 0x7FF); off_y = sext11((cmd[0] >> 11) & 0x7FF); break;
    case 0xE6: mask_set = cmd[0] & 1; mask_check = cmd[0] & 2; break;
    default:
        if (op >= 0x80 && op < 0xA0) {   // VRAM to VRAM copy
            int sx = cmd[1] & 0x3FF, sy = (cmd[1] >> 16) & 0x1FF;
            int dx = cmd[2] & 0x3FF, dy = (cmd[2] >> 16) & 0x1FF;
            int w = (((cmd[3] & 0xFFFF) - 1) & 0x3FF) + 1, h = ((((cmd[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    uint16_t v = vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
                    uint16_t *d = &vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)];
                    if (!(mask_check && (*d & 0x8000))) *d = v | (mask_set ? 0x8000 : 0);
                }
            hw_copy(sx, sy, dx, dy, w, h);
        } else if (op >= 0xA0 && op < 0xC0) {
            start_transfer(true);
        } else if (op >= 0xC0 && op < 0xE0) {
            start_transfer(false);
        }
        break;
    }
}

static Vtx pl_prev;
static uint32_t pl_color;
enum { PL_FIRST, PL_COLOR, PL_VERTEX };
static int pl_state;

static void polyline_word(uint32_t v) {
    uint32_t op = cmd[0] >> 24;
    bool shaded = op & 0x10;
    bool term = (v & 0xF000F000u) == 0x50005000u;
    switch (pl_state) {
    case PL_FIRST:
        pl_prev = line_vtx(cmd[0], v);
        pl_color = cmd[0];
        pl_state = shaded ? PL_COLOR : PL_VERTEX;
        break;
    case PL_COLOR:
        if (term) { polyline = false; break; }
        pl_color = v;
        pl_state = PL_VERTEX;
        break;
    case PL_VERTEX: {
        if (term) { polyline = false; break; }
        Vtx nv = line_vtx(pl_color, v);
        draw_line(pl_prev, nv, shaded, op & 2);
        pl_prev = nv;
        pl_state = shaded ? PL_COLOR : PL_VERTEX;
        break;
    }
    }
}

void gpu_gp0(uint32_t v) {
    if (upload.active) {
        vram_put(v & 0xFFFF);
        vram_put(v >> 16);
        return;
    }
    if (polyline) { polyline_word(v); return; }
    if (cmd_len == 0) {
        int n = cmd_size(v >> 24);
        if (n < 0) {
            cmd[0] = v;
            polyline = true;
            pl_state = PL_FIRST;
            return;
        }
        cmd_need = n;
    }
    cmd_addr[cmd_len] = word_addr;
    cmd[cmd_len++] = v;
    if (cmd_len >= cmd_need) {
        cmd_len = 0;
        exec_cmd();
    }
}

void gpu_gp1(uint32_t v) {
    uint32_t op = (v >> 24) & 0x3F;
    switch (op) {
    case 0x00:
        cmd_len = 0; polyline = false; upload.active = download.active = false;
        texpage = 0; disp_enabled = false; dma_dir = 0; gpu_irq = false;
        disp_x = disp_y = 0; disp_mode = 0;
        da_x1 = da_y1 = 0; da_x2 = da_y2 = 0; off_x = off_y = 0;
        mask_set = mask_check = false;
        tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0;
        break;
    case 0x01: cmd_len = 0; polyline = false; upload.active = false; break;
    case 0x02: gpu_irq = false; break;
    case 0x03: disp_enabled = !(v & 1); break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05:
        if ((v & 0x3FE) != disp_x || ((v >> 10) & 0x1FF) != disp_y) {
            gpu_stat_flips++;
            {
                static long want = -2;
                if (want == -2) want = getenv("CMR_DUMP_PRIMS") ? atol(getenv("CMR_DUMP_PRIMS")) : -1;
                long f = (long)(cpu.cycles / CYCLES_PER_FRAME);
                gpu_dump_now = want >= 0 && f >= want && f < want + 2;
            }
            hw_flip();
            pgxp_new_frame();
        }
        disp_x = v & 0x3FE; disp_y = (v >> 10) & 0x1FF;
        break;
    case 0x06: disp_h1 = v & 0xFFF; disp_h2 = (v >> 12) & 0xFFF; break;
    case 0x07: disp_v1 = v & 0x3FF; disp_v2 = (v >> 10) & 0x3FF; break;
    case 0x08: disp_mode = v & 0xFF; break;
    case 0x10:
        switch (v & 7) {
        case 2: gpu_info = (tw_mask_x) | (tw_mask_y << 5) | (tw_off_x << 10) | (tw_off_y << 15); break;
        case 3: gpu_info = da_x1 | (da_y1 << 10); break;
        case 4: gpu_info = da_x2 | (da_y2 << 10); break;
        case 5: gpu_info = (off_x & 0x7FF) | ((off_y & 0x7FF) << 11); break;
        case 7: gpu_info = 2; break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_read(void) {
    if (download.active) {
        uint32_t lo = vram_get();
        uint32_t hi = vram_get();
        return lo | (hi << 16);
    }
    return gpu_info;
}

uint32_t gpu_status(void) {
    uint32_t s = texpage & 0x7FF;
    s |= (uint32_t)mask_set << 11;
    s |= (uint32_t)mask_check << 12;
    s |= (texpage & 0x800) ? (1u << 15) : 0;
    s |= ((disp_mode >> 6) & 1) << 16;
    s |= (disp_mode & 3) << 17;
    s |= ((disp_mode >> 2) & 1) << 19;
    s |= ((disp_mode >> 3) & 1) << 20;
    s |= ((disp_mode >> 4) & 1) << 21;
    s |= ((disp_mode >> 5) & 1) << 22;
    s |= (uint32_t)!disp_enabled << 23;
    s |= (uint32_t)gpu_irq << 24;
    s |= (1u << 26) | (1u << 27) | (1u << 28);
    s |= dma_dir << 29;
    if (dma_dir == 1 || dma_dir == 2) s |= 1u << 25;
    if (dma_dir == 3) s |= 1u << 25;
    // Line parity / interlace field from the current position in the frame.
    uint32_t line = (uint32_t)(cpu.cycles % CYCLES_PER_FRAME) / 2146u;
    bool interlaced = (disp_mode & 0x24) == 0x24;
    if (interlaced) {
        if (field_odd) s |= 1u << 13 | (1u << 31);
    } else if (line < 240 && (line & 1)) {
        s |= 1u << 31;
    }
    return s;
}

void gpu_vblank(void) {
    field_odd = !field_odd;
    hw_vram_changed();
    static int n;
    if (log_level >= 1 && ++n % 60 == 0) {
        LOG("[gpu] polys none/mixed/all precise=%u/%u/%u ", gpu_mix_stats[0], gpu_mix_stats[1], gpu_mix_stats[2]);
        gpu_mix_stats[0] = gpu_mix_stats[1] = gpu_mix_stats[2] = 0;
        LOG("[gpu] precise=%u/%u flips=%u prims=%u uploads=%u fills=%u disp=%s mode=%02x at %u,%u range v%u-%u\n", gpu_stat_precise, gpu_stat_precise + gpu_stat_imprecise, gpu_stat_flips, gpu_stat_prims,
            gpu_stat_uploads, gpu_stat_fills, disp_enabled ? "on" : "off", disp_mode, disp_x, disp_y, disp_v1, disp_v2);
        gpu_stat_prims = gpu_stat_uploads = gpu_stat_fills = gpu_stat_flips = 0;
        gpu_stat_precise = gpu_stat_imprecise = 0;
    }
}

void gpu_dma_block(uint32_t addr, uint32_t words, bool to_gpu) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t a = (addr + i * 4) & RAM_MASK & ~3u;
        if (to_gpu) { uint32_t w; memcpy(&w, psx_ram + a, 4); word_addr = a; gpu_gp0(w); word_addr = PGXP_NO_ADDR; }
        else { uint32_t w = gpu_read(); memcpy(psx_ram + a, &w, 4); }
    }
}

void gpu_dma_list(uint32_t addr) {
    int guard = 0x100000;
    addr &= RAM_MASK & ~3u;
    for (;;) {
        uint32_t hdr;
        memcpy(&hdr, psx_ram + addr, 4);
        uint32_t n = hdr >> 24;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w;
            word_addr = (addr + 4 + i * 4) & RAM_MASK & ~3u;
            memcpy(&w, psx_ram + word_addr, 4);
            gpu_gp0(w);
        }
        word_addr = PGXP_NO_ADDR;
        if ((hdr & 0x800000) || --guard == 0) break;
        addr = hdr & RAM_MASK & ~3u;
    }
}

// -------------------------------------------------------------- display out
bool gpu_display(uint32_t *rgba, int *w, int *h, int *stride) {
    static const int widths[8] = {256, 320, 512, 640, 368, 368, 368, 368};
    int width = (disp_mode & 0x40) ? 368 : widths[disp_mode & 3];
    bool interlaced = (disp_mode & 0x24) == 0x24;
    int lines = (int)disp_v2 - (int)disp_v1;
    if (lines <= 0 || lines > 256) lines = 240;
    int height = interlaced ? lines * 2 : lines;
    if (height > 512) height = 512;
    *w = width; *h = height; *stride = width;
    if (!disp_enabled) {
        memset(rgba, 0, (size_t)width * height * 4);
        return false;
    }
    bool rgb24 = disp_mode & 0x10;
    for (int y = 0; y < height; y++) {
        int vy = (disp_y + y) & 511;
        uint32_t *o = rgba + y * width;
        if (rgb24) {
            const uint8_t *row = (const uint8_t *)&vram[vy * 1024];
            for (int x = 0; x < width; x++) {
                int bx = disp_x * 2 + x * 3;
                uint8_t r = row[bx % 2048], g = row[(bx + 1) % 2048], b = row[(bx + 2) % 2048];
                o[x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            }
        } else {
            for (int x = 0; x < width; x++) {
                uint16_t p = vram[vy * 1024 + ((disp_x + x) & 1023)];
                uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                r |= r >> 5; g |= g >> 5; b |= b >> 5;
                o[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
            }
        }
    }
    return true;
}

void gpu_display_info(DisplayInfo *d) {
    static const int widths[8] = {256, 320, 512, 640, 368, 368, 368, 368};
    d->w = (disp_mode & 0x40) ? 368 : widths[disp_mode & 3];
    bool interlaced = (disp_mode & 0x24) == 0x24;
    int lines = (int)disp_v2 - (int)disp_v1;
    if (lines <= 0 || lines > 256) lines = 240;
    d->h = interlaced ? lines * 2 : lines;
    if (d->h > 512) d->h = 512;
    d->x = disp_x; d->y = disp_y;
    d->enabled = disp_enabled;
    d->rgb24 = disp_mode & 0x10;
}
