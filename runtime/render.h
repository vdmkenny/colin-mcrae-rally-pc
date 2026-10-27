// Hardware renderer interface (render_gpu.c), fed by the software GPU.
#pragma once
#include "psx.h"

typedef struct SDL_Window SDL_Window;

typedef struct {
    float x, y;      // VRAM coordinates, sub-pixel when the GTE supplied them
    float w;         // 1 for affine; view depth for perspective-correct texturing
    uint8_t r, g, b;
    float u, v;      // texel coordinates
    uint32_t key;    // identity of a GTE-projected vertex (0 = none)
    uint32_t xyword; // the integer screen position word the game supplied
    bool precise;    // x/y came from the GTE (or are exact for 2D primitives)
} HwVertex;

enum { PRIM_POLY, PRIM_2D, PRIM_SPRITE_3D };

typedef struct {
    uint32_t id;                   // triangles of one GPU primitive share an id
    uint8_t kind;                  // PRIM_POLY (2D or 3D, decided per frame), PRIM_2D, PRIM_SPRITE_3D
    bool textured, raw, semi, filterable;
    int semi_mode;
    int depth;                     // 0 = 4-bit CLUT, 1 = 8-bit CLUT, 2 = 15-bit direct
    int tex_x, tex_y, clut_x, clut_y;
    uint32_t texwin;               // mask x | mask y << 5 | offset x << 10 | offset y << 15
} HwPrim;

typedef struct {
    bool enabled, rgb24;
    int x, y, w, h;                // display area in VRAM
} DisplayInfo;

typedef struct {
    bool hw;          // high-resolution renderer (false: show the 1x software output)
    bool filter;      // bilinear texture filtering
    bool widescreen;  // 16:9 output with the GTE projection widened to match
    int scale;        // internal resolution multiplier, 0 = match the window
    int msaa;         // anti-aliasing samples: 1 (off), 2, 4 or 8
} RenderConfig;
extern RenderConfig render_cfg;
extern bool widescreen_active;   // widescreen applies to the current frames (3D gameplay)

bool render_init(SDL_Window *window);
bool render_set_vsync(bool on);   // false if that mode is unavailable
void render_present(const DisplayInfo *d, const uint32_t *rgba_1x);
void render_vblank(void);
bool render_read_display(const DisplayInfo *d, uint32_t **out, int *w, int *h);
int render_scale(void);

void gpu_display_info(DisplayInfo *d);
void hw_set_draw_area(int x1, int y1, int x2, int y2);
void hw_triangle(const HwVertex v[3], const HwPrim *p);
void hw_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void hw_upload(int x, int y, int w, int h);
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
void hw_vram_changed(void);
void hw_flip(void);   // the game switched the displayed framebuffer
