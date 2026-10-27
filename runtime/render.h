// Hardware renderer interface (render_gl.c), fed by the software GPU.
#pragma once
#include "psx.h"

typedef struct SDL_Window SDL_Window;

typedef struct {
    float x, y;      // VRAM coordinates, sub-pixel when the GTE supplied them
    float w;         // 1 for affine; view depth for perspective-correct texturing
    uint8_t r, g, b;
    float u, v;      // texel coordinates
    uint32_t key;    // vertex identity across frames (0 = none), for frame interpolation
    uint32_t xyword; // the integer screen position word the game supplied
    bool precise;    // x/y came from the GTE (or are exact for 2D primitives)
} HwVertex;

typedef struct {
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
    bool interpolate; // generate in-between frames when the game renders below 60 fps
} RenderConfig;
extern RenderConfig render_cfg;

bool render_init(SDL_Window *window);
void render_present(const DisplayInfo *d, const uint32_t *rgba_1x);
bool render_read_display(const DisplayInfo *d, uint32_t **out, int *w, int *h);
int render_scale(void);

void gpu_display_info(DisplayInfo *d);
void hw_set_draw_area(int x1, int y1, int x2, int y2);
void hw_triangle(const HwVertex v[3], const HwPrim *p);
void hw_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void hw_upload(int x, int y, int w, int h);
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
void hw_vram_changed(void);
void hw_flush(void);
void hw_flip(void);   // the game switched the displayed framebuffer
