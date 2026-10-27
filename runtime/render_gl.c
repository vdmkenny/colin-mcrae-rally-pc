// OpenGL 3.3 renderer: mirrors GPU drawing into a high-resolution copy of VRAM and presents it.
//
// The software GPU (gpu.c) stays authoritative for the 1x VRAM the game can read back and
// texture from. Every primitive is also drawn here at `scale` times the resolution, in true
// colour, with sub-pixel vertex positions and perspective-correct texturing when the GTE
// supplied precise coordinates. Textures are sampled from a 1x copy of VRAM in the shader.
#include "render.h"
#include "pgxp.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stddef.h>
#include <stdlib.h>

extern uint16_t vram[1024 * 512];

// ------------------------------------------------------------------ GL loader
// OpenGL 1.x entry points have no PFN typedefs in glext.h.
typedef void (APIENTRY *PFNGLGENTEXTURESPROC)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLTEXIMAGE2DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFNGLTEXSUBIMAGE2DPROC)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFNGLSCISSORPROC)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFNGLENABLEPROC)(GLenum);
typedef void (APIENTRY *PFNGLDISABLEPROC)(GLenum);
typedef void (APIENTRY *PFNGLCLEARCOLORPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLCLEARPROC)(GLbitfield);
typedef void (APIENTRY *PFNGLDRAWARRAYSPROC)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFNGLPIXELSTOREIPROC)(GLenum, GLint);
typedef void (APIENTRY *PFNGLREADPIXELSPROC)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef GLenum (APIENTRY *PFNGLGETERRORPROC)(void);
typedef void (APIENTRY *PFNGLGETINTEGERVPROC)(GLenum, GLint *);
typedef void (APIENTRY *PFNGLREADBUFFERPROC)(GLenum);
typedef const GLubyte *(APIENTRY *PFNGLGETSTRINGPROC)(GLenum);

#define GL_FUNCS(X) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARPROC, glClear) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) X(PFNGLBLENDCOLORPROC, glBlendColor) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) X(PFNGLREADBUFFERPROC, glReadBuffer) \
    X(PFNGLGETSTRINGPROC, glGetString)

#define DECL(type, name) static type p_##name;
GL_FUNCS(DECL)
#undef DECL
#define GLF(name) p_##name

static bool load_gl(void) {
#define LOAD(type, name) \
    p_##name = (type)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) { LOG("GL: missing %s\n", #name); return false; }
    GL_FUNCS(LOAD)
#undef LOAD
    return true;
}

// -------------------------------------------------------------------- shaders
static const char *draw_vs =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"     // x, y (VRAM units), w
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in uvec4 a_tex;\n"
    "layout(location=4) in uint a_uvlim;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "flat out uvec4 v_tex;\n"
    "flat out uint v_uvlim;\n"
    "void main() {\n"
    "  vec2 ndc = a_pos.xy / vec2(1024.0, 512.0) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc * a_pos.z, 0.0, a_pos.z);\n"
    "  v_col = a_col; v_uv = a_uv; v_tex = a_tex; v_uvlim = a_uvlim;\n"
    "}\n";

// a_tex: x = texpage x | y<<16, y = clut x | y<<16, z = texture window, w = flags
// flags: 0-1 depth, 2 textured, 3 raw, 4-5 pass (0 all, 1 opaque texels, 2 translucent texels),
//        6 bilinear filtering
static const char *draw_fs =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "flat in uvec4 v_tex;\n"
    "flat in uint v_uvlim;\n"
    "uniform usampler2D u_vram;\n"
    "out vec4 o_col;\n"
    "uint fetch16(int x, int y) { return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }\n"
    "uint texel(ivec2 c) {\n"
    "  uint tw = v_tex.z;\n"
    "  uint mx = tw & 31u, my = (tw >> 5) & 31u, ox = (tw >> 10) & 31u, oy = (tw >> 15) & 31u;\n"
    "  uint u = uint(c.x) & 255u, v = uint(c.y) & 255u;\n"
    "  u = (u & ~(mx * 8u)) | ((ox & mx) * 8u);\n"
    "  v = (v & ~(my * 8u)) | ((oy & my) * 8u);\n"
    "  int tx = int(v_tex.x & 0xFFFFu), ty = int(v_tex.x >> 16);\n"
    "  int cx = int(v_tex.y & 0xFFFFu), cy = int(v_tex.y >> 16);\n"
    "  uint depth = v_tex.w & 3u;\n"
    "  if (depth == 0u) {\n"
    "    uint w = fetch16(tx + int(u >> 2), ty + int(v));\n"
    "    return fetch16(cx + int((w >> ((u & 3u) * 4u)) & 15u), cy);\n"
    "  } else if (depth == 1u) {\n"
    "    uint w = fetch16(tx + int(u >> 1), ty + int(v));\n"
    "    return fetch16(cx + int((w >> ((u & 1u) * 8u)) & 255u), cy);\n"
    "  }\n"
    "  return fetch16(tx + int(u), ty + int(v));\n"
    "}\n"
    "vec4 decode(uint t) {\n"
    "  return vec4(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u), 31.0) / 31.0;\n"
    "}\n"
    "void main() {\n"
    "  uint flags = v_tex.w;\n"
    "  vec4 col = v_col;\n"
    "  if ((flags & 4u) != 0u) {\n"
    "    uint pass = (flags >> 4) & 3u;\n"
    "    vec4 t;\n"
    "    bool stp;\n"
    "    if ((flags & 64u) != 0u) {\n"
    "      vec2 p = v_uv - 0.5;\n"
    "      ivec2 b = ivec2(floor(p));\n"
    "      vec2 f = p - vec2(b);\n"
    "      ivec2 lo = ivec2(int(v_uvlim & 255u), int((v_uvlim >> 8) & 255u));\n"
    "      ivec2 hi = ivec2(int((v_uvlim >> 16) & 255u), int(v_uvlim >> 24));\n"
    "      ivec2 b0 = clamp(b, lo, hi), b1 = clamp(b + 1, lo, hi);\n"
    "      uint t00 = texel(b0), t10 = texel(ivec2(b1.x, b0.y)), t01 = texel(ivec2(b0.x, b1.y)), t11 = texel(b1);\n"
    "      vec4 c00 = t00 == 0u ? vec4(0) : decode(t00), c10 = t10 == 0u ? vec4(0) : decode(t10);\n"
    "      vec4 c01 = t01 == 0u ? vec4(0) : decode(t01), c11 = t11 == 0u ? vec4(0) : decode(t11);\n"
    "      t = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);\n"
    "      if (t.a < 0.5) discard;\n"
    "      t.rgb /= t.a;\n"
    "      uint tn = texel(ivec2(floor(v_uv)));\n"
    "      stp = (tn & 0x8000u) != 0u;\n"
    "    } else {\n"
    "      uint tn = texel(ivec2(floor(v_uv)));\n"
    "      if (tn == 0u) discard;\n"
    "      t = decode(tn);\n"
    "      stp = (tn & 0x8000u) != 0u;\n"
    "    }\n"
    "    if (pass == 1u && stp) discard;\n"
    "    if (pass == 2u && !stp) discard;\n"
    "    if ((flags & 8u) != 0u) col = vec4(t.rgb, 1.0);\n"
    "    else col = vec4(min(t.rgb * col.rgb * (255.0 / 128.0), vec3(1.0)), 1.0);\n"
    "  }\n"
    "  o_col = vec4(col.rgb, 1.0);\n"
    "}\n";

// Copies a 1x VRAM rectangle (15-bit) into the high-resolution target.
static const char *blit_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_pos;\n"
    "void main() { v_pos = a_pos; gl_Position = vec4(a_pos / vec2(1024.0, 512.0) * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *blit_fs =
    "#version 330 core\n"
    "in vec2 v_pos;\n"
    "uniform usampler2D u_vram;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  uint t = texelFetch(u_vram, ivec2(floor(v_pos)) & ivec2(1023, 511), 0).r;\n"
    "  o_col = vec4(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u), 31.0) / 31.0;\n"
    "}\n";

// Presents a region of a texture to the window.
static const char *present_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "uniform vec4 u_src;\n"   // x, y, w, h in texture coordinates (0..1)
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = u_src.xy + vec2(a_pos.x, 1.0 - a_pos.y) * u_src.zw;\n"
    "  gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *present_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_flip;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec2 uv = v_uv;\n"
    "  if (u_flip > 0.5) uv.y = 1.0 - uv.y;\n"
    "  o_col = vec4(texture(u_tex, uv).rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = GLF(glCreateShader)(type);
    GLF(glShaderSource)(s, 1, &src, NULL);
    GLF(glCompileShader)(s);
    GLint ok;
    GLF(glGetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLF(glGetShaderInfoLog)(s, sizeof log, NULL, log);
        host_fatal("shader compile failed: %s", log);
    }
    return s;
}

static GLuint program(const char *vs, const char *fs) {
    GLuint p = GLF(glCreateProgram)();
    GLF(glAttachShader)(p, compile(GL_VERTEX_SHADER, vs));
    GLF(glAttachShader)(p, compile(GL_FRAGMENT_SHADER, fs));
    GLF(glLinkProgram)(p);
    GLint ok;
    GLF(glGetProgramiv)(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLF(glGetProgramInfoLog)(p, sizeof log, NULL, log);
        host_fatal("shader link failed: %s", log);
    }
    return p;
}

// ---------------------------------------------------------------------- state
typedef struct {
    float x, y, w;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t tpage, clut, texwin, flags;
    uint32_t uvlim;   // umin | vmin << 8 | umax << 16 | vmax << 24, keeps filtering inside the texture region
} GVtx;

#define MAX_VERTS (3 * 65536)
static GVtx verts[MAX_VERTS];
static int nverts;

static SDL_Window *win;
static SDL_GLContext ctx;
static GLuint prog_draw, prog_blit, prog_present;
static GLint u_present_src, u_present_flip;
static GLuint vao_draw, vbo_draw, vao_quad, vbo_quad;
static GLuint tex_vram;                   // 1x VRAM, R16UI
static GLuint tex_hi, fbo_hi;             // high-resolution VRAM
static GLuint tex_tmp, fbo_tmp;           // scratch for VRAM-to-VRAM copies
static GLuint tex_disp;                   // 24-bit / software display path
static GLuint tex_interp, fbo_interp;     // in-between frame
static GLuint target_fbo;                 // where drawing goes (fbo_hi, or fbo_interp while replaying)
static int scale = 1;
static bool vram_dirty = true;

bool widescreen_active;
static int frame_precise_vertices;

RenderConfig render_cfg = {.hw = true, .filter = false, .widescreen = false, .scale = 0, .interpolate = true};

// current batch state
static struct { int blend; int x1, y1, x2, y2; } bstate, pending;

static void set_blend(int mode) {
    if (mode < 0) { GLF(glDisable)(GL_BLEND); return; }
    GLF(glEnable)(GL_BLEND);
    switch (mode) {
    case 0:   // B/2 + F/2
        GLF(glBlendColor)(0.5f, 0.5f, 0.5f, 0.5f);
        GLF(glBlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(glBlendFuncSeparate)(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO);
        break;
    case 1:   // B + F
        GLF(glBlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(glBlendFuncSeparate)(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case 2:   // B - F
        GLF(glBlendEquationSeparate)(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        GLF(glBlendFuncSeparate)(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    default:  // B + F/4
        GLF(glBlendColor)(0.25f, 0.25f, 0.25f, 0.25f);
        GLF(glBlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
        GLF(glBlendFuncSeparate)(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO);
        break;
    }
}

static void upload_vram(void) {
    if (!vram_dirty) return;
    GLF(glActiveTexture)(GL_TEXTURE0);
    GLF(glBindTexture)(GL_TEXTURE_2D, tex_vram);
    GLF(glPixelStorei)(GL_UNPACK_ALIGNMENT, 2);
    GLF(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_RED_INTEGER, GL_UNSIGNED_SHORT, vram);
    vram_dirty = false;
}

static void bind_hi(void) {
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, target_fbo ? target_fbo : fbo_hi);
    GLF(glViewport)(0, 0, 1024 * scale, 512 * scale);
}

static void flush(void) {
    if (!nverts) return;
    upload_vram();
    bind_hi();
    GLF(glEnable)(GL_SCISSOR_TEST);
    GLF(glScissor)(bstate.x1 * scale, bstate.y1 * scale, (bstate.x2 - bstate.x1 + 1) * scale,
                   (bstate.y2 - bstate.y1 + 1) * scale);
    set_blend(bstate.blend);
    GLF(glUseProgram)(prog_draw);
    GLF(glActiveTexture)(GL_TEXTURE0);
    GLF(glBindTexture)(GL_TEXTURE_2D, tex_vram);
    GLF(glBindVertexArray)(vao_draw);
    GLF(glBindBuffer)(GL_ARRAY_BUFFER, vbo_draw);
    GLF(glBufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)(nverts * sizeof(GVtx)), verts, GL_STREAM_DRAW);
    GLF(glDrawArrays)(GL_TRIANGLES, 0, nverts);
    GLF(glDisable)(GL_SCISSOR_TEST);
    GLF(glDisable)(GL_BLEND);
    nverts = 0;
}

void hw_flush(void) {
    if (!render_cfg.hw || !ctx) return;
    flush();
}

static void begin_prim(int blend, int verts_needed) {
    pending.blend = blend;
    if (nverts && (memcmp(&pending, &bstate, sizeof bstate) != 0 || nverts + verts_needed > MAX_VERTS)) flush();
    bstate = pending;
}

// ------------------------------------------------------------ immediate drawing
void hw_set_draw_area(int x1, int y1, int x2, int y2) {
    pending.x1 = x1; pending.y1 = y1; pending.x2 = x2; pending.y2 = y2;
}

static uint32_t cur_uvlim;

static void push(const HwVertex *v, const HwPrim *p, uint32_t pass) {
    GVtx *o = &verts[nverts++];
    o->x = v->x; o->y = v->y; o->w = v->w;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = 255;
    o->u = v->u; o->v = v->v;
    o->tpage = (uint32_t)p->tex_x | ((uint32_t)p->tex_y << 16);
    o->clut = (uint32_t)p->clut_x | ((uint32_t)p->clut_y << 16);
    o->texwin = p->texwin;
    o->uvlim = cur_uvlim;
    o->flags = (uint32_t)p->depth | (p->textured ? 4u : 0) | (p->raw ? 8u : 0) | (pass << 4) |
               (p->textured && render_cfg.filter && p->filterable ? 64u : 0);
}

static void draw_tri(const HwVertex v[3], const HwPrim *p) {
    if (p->textured) {
        float umin = v[0].u, umax = v[0].u, vmin = v[0].v, vmax = v[0].v;
        for (int i = 1; i < 3; i++) {
            if (v[i].u < umin) umin = v[i].u; if (v[i].u > umax) umax = v[i].u;
            if (v[i].v < vmin) vmin = v[i].v; if (v[i].v > vmax) vmax = v[i].v;
        }
        // texel coordinates are floored, so the last texel of a span is max - 1 when max is fractional
        int u0 = (int)umin, v0 = (int)vmin, u1 = (int)(umax - 0.001f), v1 = (int)(vmax - 0.001f);
        if (u1 < u0) u1 = u0;
        if (v1 < v0) v1 = v0;
        cur_uvlim = (uint32_t)(u0 & 255) | ((uint32_t)(v0 & 255) << 8) | ((uint32_t)(u1 & 255) << 16) | ((uint32_t)(v1 & 255) << 24);
    }
    if (p->semi && p->textured) {
        // opaque texels first, then translucent ones with blending, keeping primitive order
        begin_prim(-1, 3);
        for (int i = 0; i < 3; i++) push(&v[i], p, 1);
        begin_prim(p->semi_mode, 3);
        for (int i = 0; i < 3; i++) push(&v[i], p, 2);
    } else {
        begin_prim(p->semi ? p->semi_mode : -1, 3);
        for (int i = 0; i < 3; i++) push(&v[i], p, 0);
    }
}

static void draw_fill(int x, int y, int w, int h, uint32_t rgb) {
    flush();
    bind_hi();
    GLF(glEnable)(GL_SCISSOR_TEST);
    GLF(glScissor)(x * scale, y * scale, w * scale, h * scale);
    GLF(glClearColor)((rgb & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, ((rgb >> 16) & 255) / 255.0f, 1.0f);
    GLF(glClear)(GL_COLOR_BUFFER_BIT);
    GLF(glDisable)(GL_SCISSOR_TEST);
}

static void blit_from_vram(int x, int y, int w, int h) {
    float q[12] = {(float)x, (float)y, (float)(x + w), (float)y, (float)x, (float)(y + h),
                   (float)(x + w), (float)y, (float)(x + w), (float)(y + h), (float)x, (float)(y + h)};
    bind_hi();
    GLF(glUseProgram)(prog_blit);
    GLF(glActiveTexture)(GL_TEXTURE0);
    GLF(glBindTexture)(GL_TEXTURE_2D, tex_vram);
    GLF(glBindVertexArray)(vao_quad);
    GLF(glBindBuffer)(GL_ARRAY_BUFFER, vbo_quad);
    GLF(glBufferData)(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    GLF(glDrawArrays)(GL_TRIANGLES, 0, 6);
}

// Draws CPU-transferred pixels (as they were at transfer time) into the target.
static void draw_upload(int x, int y, int w, int h, const uint16_t *pixels) {
    flush();
    upload_vram();
    if (pixels && x + w <= 1024 && y + h <= 512) {
        GLF(glActiveTexture)(GL_TEXTURE0);
        GLF(glBindTexture)(GL_TEXTURE_2D, tex_vram);
        GLF(glPixelStorei)(GL_UNPACK_ALIGNMENT, 2);
        GLF(glTexSubImage2D)(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT, pixels);
        blit_from_vram(x, y, w, h);
        vram_dirty = true;   // restore current VRAM contents before the next texture use
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
    GLuint dst = target_fbo ? target_fbo : fbo_hi;
    flush();
    if (sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512) {
        // rare wrapping copy: rebuild the destination from 1x VRAM
        upload_vram();
        blit_from_vram(dx, dy, w < 1024 - dx ? w : 1024 - dx, h < 512 - dy ? h : 512 - dy);
        return;
    }
    GLF(glBindFramebuffer)(GL_READ_FRAMEBUFFER, dst);
    GLF(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, fbo_tmp);
    GLF(glBlitFramebuffer)(sx * scale, sy * scale, (sx + w) * scale, (sy + h) * scale, 0, 0, w * scale, h * scale,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLF(glBindFramebuffer)(GL_READ_FRAMEBUFFER, fbo_tmp);
    GLF(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, dst);
    GLF(glBlitFramebuffer)(0, 0, w * scale, h * scale, dx * scale, dy * scale, (dx + w) * scale, (dy + h) * scale,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
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
    if (!render_cfg.hw || !ctx) return;
    Op *o = record(OP_TRI);
    memcpy(o->v, v, sizeof o->v);
    o->p = *p;
    o->a[0] = pending.x1; o->a[1] = pending.y1; o->a[2] = pending.x2; o->a[3] = pending.y2;
    if (v[0].key || v[1].key || v[2].key) frame_has_keys = true;
}

void hw_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    if (!ctx) return;
    vram_dirty = true;
    if (!render_cfg.hw) return;
    Op *o = record(OP_FILL);
    o->a[0] = x; o->a[1] = y; o->a[2] = w; o->a[3] = h; o->a[4] = r | (g << 8) | (b << 16);
}

// A CPU-to-VRAM transfer finished (vram[] already holds the data).
void hw_upload(int x, int y, int w, int h) {
    if (!ctx) return;
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
    if (!ctx) return;
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
// by which third of the screen it is in. Sprites placed in 3D (dust, smoke) keep their place
// and are narrowed. Decided per GPU primitive, after the vertex resolution above.
static void widescreen_frame(OpList *l) {
    frame_precise_vertices = 0;
    for (int i = 0; i < l->n; i++)
        if (l->ops[i].type == OP_TRI)
            for (int k = 0; k < 3; k++) frame_precise_vertices += l->ops[i].v[k].key != 0;
    if (!widescreen_active) return;
    for (int i = 0; i < l->n;) {
        Op *o = &l->ops[i];
        if (o->type != OP_TRI) { i++; continue; }
        int j = i;
        bool is2d = o->p.kind == PRIM_2D, any3d = false;
        float minx = 1e9f, maxx = -1e9f;
        while (j < l->n && l->ops[j].type == OP_TRI && l->ops[j].p.id == o->p.id) {
            for (int k = 0; k < 3; k++) {
                HwVertex *v = &l->ops[j].v[k];
                if (v->x < minx) minx = v->x;
                if (v->x > maxx) maxx = v->x;
                if (o->p.kind == PRIM_POLY && v->precise) any3d = true;
            }
            j++;
        }
        if (o->p.kind == PRIM_POLY && !any3d) is2d = true;
        float x1 = (float)o->a[0], w = (float)(o->a[2] - o->a[0] + 1);
        float anchor, factor = 0.75f;
        if (is2d) {
            float c = (minx + maxx) * 0.5f - x1;
            anchor = c < w / 3 ? x1 : c > w * 2 / 3 ? x1 + w : x1 + w * 0.5f;
        } else if (o->p.kind == PRIM_SPRITE_3D) {
            anchor = (minx + maxx) * 0.5f;
        } else {
            i = j;
            continue;
        }
        for (int t = i; t < j; t++)
            for (int k = 0; k < 3; k++) l->ops[t].v[k].x = anchor + (l->ops[t].v[k].x - anchor) * factor;
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

static void copy_target(GLuint from, GLuint to) {
    GLF(glBindFramebuffer)(GL_READ_FRAMEBUFFER, from);
    GLF(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, to);
    GLF(glBlitFramebuffer)(0, 0, 1024 * scale, 512 * scale, 0, 0, 1024 * scale, 512 * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
}

// Per-vertex motion for the in-between frame, from (in order of preference) the vertex's own
// match in the previous frame, the average motion of matched vertices nearby (16-pixel grid)
// for other GTE vertices, and the motion of the rest of the polygon for CPU-built vertices.
#define GRID_W 64
#define GRID_H 32
static float grid_dx[GRID_H][GRID_W], grid_dy[GRID_H][GRID_W];
static int grid_n[GRID_H][GRID_W];
typedef struct { float dx, dy, w; uint8_t state; } VMotion;   // 0 none, 1 matched, 2 estimated
static VMotion *motion;
static int motion_cap;

static void compute_motion(float alpha) {
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
            float cx = v->x - o->a[0], cy = v->y - o->a[1], px, py, pw;
            if (!map_get(v->key, cx, cy, &px, &py, &pw)) continue;
            interp_stat_found++;
            float dx = cx - px, dy = cy - py;
            if (dx * dx + dy * dy > 64.0f * 64.0f) continue;   // probably a different vertex
            interp_stat_matched++;
            m->dx = -dx * (1.0f - alpha);
            m->dy = -dy * (1.0f - alpha);
            m->w = (v->w != 1.0f && pw != 1.0f) ? pw + (v->w - pw) * alpha : v->w;
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

// Renders a recorded frame into `target`, optionally displaced by the motion field.
static void render_ops(OpList *l, bool moved, GLuint target) {
    int ax1 = pending.x1, ay1 = pending.y1, ax2 = pending.x2, ay2 = pending.y2;
    target_fbo = target;
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
                    v[k].x += m->dx; v[k].y += m->dy; v[k].w = m->w;
                    sx += m->dx; sy += m->dy; nm++;
                }
                if (nm && nm < 3)
                    for (int k = 0; k < 3; k++)
                        if (!motion[i * 3 + k].state) { v[k].x += sx / nm; v[k].y += sy / nm; }
            }
            pending.x1 = o->a[0]; pending.y1 = o->a[1]; pending.x2 = o->a[2]; pending.y2 = o->a[3];
            draw_tri(v, &o->p);
            break;
        }
        case OP_FILL: draw_fill(o->a[0], o->a[1], o->a[2], o->a[3], (uint32_t)o->a[4]); break;
        case OP_UPLOAD: draw_upload(o->a[0], o->a[1], o->a[2], o->a[3], o->pixels); break;
        case OP_COPY: draw_copy(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5]); break;
        }
    }
    flush();
    target_fbo = 0;
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
    render_ops(&frame_ops, false, fbo_hi);
    clear_ops(&frame_ops);
    clear_ops(&prev_ops);
    frame_has_keys = false;
}

void hw_flip(void) {
    if (!ctx || !render_cfg.hw) return;
    last_interval = vblanks_since_flip < 1 ? 1 : vblanks_since_flip;
    vblanks_since_flip = 0;
    resolve_frame(&frame_ops);
    widescreen_frame(&frame_ops);
    update_widescreen();
    render_ops(&frame_ops, false, fbo_hi);
    if (render_cfg.interpolate && last_interval == 2 && frame_has_keys && prev_ops.n) {
        compute_motion(0.5f);
        copy_target(fbo_hi, fbo_interp);   // anything left uncovered shows nearly identical pixels
        render_ops(&frame_ops, true, fbo_interp);
        interp_pending = true;
    }
    clear_ops(&prev_ops);
    OpList t = prev_ops;
    prev_ops = frame_ops;
    frame_ops = t;
    frame_has_keys = false;
}

// ------------------------------------------------------------- setup/present
static void create_targets(void) {
    if (tex_hi) {
        GLuint t[3] = {tex_hi, tex_tmp, tex_interp}, f[3] = {fbo_hi, fbo_tmp, fbo_interp};
        GLF(glDeleteTextures)(3, t);
        GLF(glDeleteFramebuffers)(3, f);
    }
    GLuint t[3], f[3];
    GLF(glGenTextures)(3, t);
    GLF(glGenFramebuffers)(3, f);
    for (int i = 0; i < 3; i++) {
        GLF(glBindTexture)(GL_TEXTURE_2D, t[i]);
        GLF(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, 1024 * scale, 512 * scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLF(glBindFramebuffer)(GL_FRAMEBUFFER, f[i]);
        GLF(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t[i], 0);
        if (GLF(glCheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            host_fatal("GL: framebuffer incomplete at scale %d", scale);
    }
    tex_hi = t[0]; tex_tmp = t[1]; fbo_hi = f[0]; fbo_tmp = f[1];
    tex_interp = t[2]; fbo_interp = f[2];
    clear_ops(&frame_ops);
    clear_ops(&prev_ops);
    // seed the new target with the current 1x VRAM
    vram_dirty = true;
    upload_vram();
    blit_from_vram(0, 0, 1024, 512);
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
}

static int wanted_scale(void) {
    if (render_cfg.scale > 0) return render_cfg.scale;
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    int s = (h + 239) / 240;
    GLint max_tex = 4096;
    GLF(glGetIntegerv)(GL_MAX_TEXTURE_SIZE, &max_tex);
    while (s > 1 && 1024 * s > max_tex) s--;
    return s < 1 ? 1 : s > 16 ? 16 : s;
}

bool render_init(SDL_Window *window) {
    win = window;
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) { LOG("GL: %s\n", SDL_GetError()); return false; }
    if (!load_gl()) return false;
    LOG("[render] %s / %s\n", (const char *)GLF(glGetString)(GL_RENDERER), (const char *)GLF(glGetString)(GL_VERSION));
    prog_draw = program(draw_vs, draw_fs);
    prog_blit = program(blit_vs, blit_fs);
    prog_present = program(present_vs, present_fs);
    GLF(glUseProgram)(prog_draw);
    GLF(glUniform1i)(GLF(glGetUniformLocation)(prog_draw, "u_vram"), 0);
    GLF(glUseProgram)(prog_blit);
    GLF(glUniform1i)(GLF(glGetUniformLocation)(prog_blit, "u_vram"), 0);
    GLF(glUseProgram)(prog_present);
    GLF(glUniform1i)(GLF(glGetUniformLocation)(prog_present, "u_tex"), 0);
    u_present_src = GLF(glGetUniformLocation)(prog_present, "u_src");
    u_present_flip = GLF(glGetUniformLocation)(prog_present, "u_flip");

    GLF(glGenVertexArrays)(1, &vao_draw);
    GLF(glGenBuffers)(1, &vbo_draw);
    GLF(glBindVertexArray)(vao_draw);
    GLF(glBindBuffer)(GL_ARRAY_BUFFER, vbo_draw);
    GLF(glVertexAttribPointer)(0, 3, GL_FLOAT, GL_FALSE, sizeof(GVtx), (void *)offsetof(GVtx, x));
    GLF(glVertexAttribPointer)(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GVtx), (void *)offsetof(GVtx, r));
    GLF(glVertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, sizeof(GVtx), (void *)offsetof(GVtx, u));
    GLF(glVertexAttribIPointer)(3, 4, GL_UNSIGNED_INT, sizeof(GVtx), (void *)offsetof(GVtx, tpage));
    GLF(glVertexAttribIPointer)(4, 1, GL_UNSIGNED_INT, sizeof(GVtx), (void *)offsetof(GVtx, uvlim));
    for (int i = 0; i < 5; i++) GLF(glEnableVertexAttribArray)(i);

    GLF(glGenVertexArrays)(1, &vao_quad);
    GLF(glGenBuffers)(1, &vbo_quad);
    GLF(glBindVertexArray)(vao_quad);
    GLF(glBindBuffer)(GL_ARRAY_BUFFER, vbo_quad);
    GLF(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
    GLF(glEnableVertexAttribArray)(0);

    GLuint t[2];
    GLF(glGenTextures)(2, t);
    tex_vram = t[0]; tex_disp = t[1];
    GLF(glBindTexture)(GL_TEXTURE_2D, tex_vram);
    GLF(glTexImage2D)(GL_TEXTURE_2D, 0, GL_R16UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLF(glBindTexture)(GL_TEXTURE_2D, tex_disp);
    GLF(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLF(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    scale = wanted_scale();
    create_targets();
    LOG("[render] internal resolution %dx%d (%dx)\n", 1024 * scale, 512 * scale, scale);
    return true;
}

int render_scale(void) { return scale; }

static void draw_present(GLuint tex, float sx, float sy, float sw, float sh, bool flip, int vx, int vy, int vw, int vh) {
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    GLF(glViewport)(0, 0, ww, wh);
    GLF(glClearColor)(0, 0, 0, 1);
    GLF(glClear)(GL_COLOR_BUFFER_BIT);
    GLF(glViewport)(vx, vy, vw, vh);
    GLF(glUseProgram)(prog_present);
    GLF(glUniform4f)(u_present_src, sx, sy, sw, sh);
    GLF(glUniform1f)(u_present_flip, flip ? 1.0f : 0.0f);
    GLF(glActiveTexture)(GL_TEXTURE0);
    GLF(glBindTexture)(GL_TEXTURE_2D, tex);
    float q[12] = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
    GLF(glBindVertexArray)(vao_quad);
    GLF(glBindBuffer)(GL_ARRAY_BUFFER, vbo_quad);
    GLF(glBufferData)(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    GLF(glDrawArrays)(GL_TRIANGLES, 0, 6);
}

// Window viewport keeping the game's aspect ratio (4:3, or 16:9 in widescreen mode).
static void viewport(int *x, int *y, int *w, int *h) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    float aspect = widescreen_active ? 16.0f / 9.0f : 4.0f / 3.0f;
    int vw = ww, vh = (int)(ww / aspect + 0.5f);
    if (vh > wh) { vh = wh; vw = (int)(wh * aspect + 0.5f); }
    *x = (ww - vw) / 2; *y = (wh - vh) / 2; *w = vw; *h = vh;
}

void render_present(const DisplayInfo *d, const uint32_t *rgba_1x) {
    // Double-buffered games flip every frame; anything that draws without flipping
    // (loading screens, single-buffered menus) is shown after a short wait.
    if (render_cfg.hw && (vblanks_since_flip >= 4 || frame_ops.n > 200000)) render_pending();
    flush();
    int vx, vy, vw, vh;
    viewport(&vx, &vy, &vw, &vh);
    if (!d->enabled) {
        draw_present(tex_disp, 0, 0, 0, 0, false, vx, vy, vw, vh);
    } else if (!render_cfg.hw || d->rgb24) {
        // software output (24-bit video or the software renderer)
        GLF(glBindTexture)(GL_TEXTURE_2D, tex_disp);
        GLF(glPixelStorei)(GL_UNPACK_ALIGNMENT, 4);
        GLF(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, d->w, d->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_1x);
        draw_present(tex_disp, 0, 0, d->w / 640.0f, d->h / 512.0f, false, vx, vy, vw, vh);
    } else {
        GLuint src = interp_pending ? tex_interp : tex_hi;
        draw_present(src, d->x / 1024.0f, d->y / 512.0f, d->w / 1024.0f, d->h / 512.0f, false, vx, vy, vw, vh);
    }
    presented_interp = interp_pending;
    interp_pending = false;
    vblanks_since_flip++;
    SDL_GL_SwapWindow(win);
    int s = wanted_scale();
    if (s != scale && render_cfg.hw) {
        scale = s;
        create_targets();
        LOG("[render] internal resolution %dx%d (%dx)\n", 1024 * scale, 512 * scale, scale);
    }
}

// Reads the displayed image at internal resolution (for screenshots).
bool render_read_display(const DisplayInfo *d, uint32_t **out, int *w, int *h) {
    if (!render_cfg.hw || d->rgb24 || !d->enabled) return false;
    flush();
    *w = d->w * scale; *h = d->h * scale;
    *out = malloc((size_t)*w * *h * 4);
    GLF(glBindFramebuffer)(GL_READ_FRAMEBUFFER, presented_interp ? fbo_interp : fbo_hi);
    GLF(glPixelStorei)(GL_PACK_ALIGNMENT, 4);
    GLF(glReadPixels)(d->x * scale, d->y * scale, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, *out);
    GLF(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    return true;
}
