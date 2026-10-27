// Native replacements and adjustments for specific game functions (see tools/hooks.txt).
#include "psx.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int frame_interval_override = 0;   // 0 = game default
bool race_60fps = true;            // races drawn at 60 fps (see below)

static uint32_t requested_interval = 1;   // what the game last asked for

// game: SetFrameInterval(n) — the race renders every n vblanks (2 = 30 fps).
int hook_set_frame_interval(CPU *c) {
    if (frame_interval_override && c->r[4] > 1) c->r[4] = (uint32_t)frame_interval_override;
    requested_interval = c->r[4];
    return 0;
}

// ---------------------------------------------------------------- 60 fps races
// The game simulates a fixed 1/30 s per frame, so races cannot simply run their loop every
// vblank. Instead the loop runs every vblank but the race update only every second one, and
// the frames drawn in between show the scene half way between the last two updates.
//
// Everything that moves is an object in a scene graph (root at 0x800c6da0, children at +8,
// siblings at +4) whose local transform at +0x48 (3x3 matrix and translation, 16.16) the update
// writes; drawing composes the world transforms (+0x78) from them, recomputing objects whose
// flags (+0xba) bit 0 is set. After each update the local transforms are recorded; the frame
// drawn right after it shows the midpoint between that update and the previous one (rotations
// interpolated as rotations, so spinning wheels keep their shape), the next frame the update
// itself. The true transforms are put back before the game runs again. This shows the race one
// vblank late.
#define SCENE_ROOT 0x800c6da0u
#define STATE_RACE 5u        // driving
#define STATE_REPLAY 8u      // stage results and the attract demo
#define MAX_OBJ 1024

typedef struct { uint32_t addr; int32_t local[12]; } ObjXform;
static ObjXform snap[2][MAX_OBJ];
static int nsnap[2], cur;            // snap[cur] is the latest update, snap[cur ^ 1] the one before
static bool have_prev, restore_pending, update_ran;
static bool fast_loop;               // this iteration runs at every vblank instead of every second
static void use_big_primbufs(uint32_t gp);
static void raise_view(uint32_t gp);
static void restore_view(uint32_t gp);
static long last_update = -10;       // vblank of the last race update

static int snapshot(ObjXform *out) {
    uint32_t stack[64];
    int n = 0, sp = 0;
    uint32_t root = RD32(SCENE_ROOT);
    if (!root) return 0;
    stack[sp++] = root;
    while (sp && n < MAX_OBJ) {
        uint32_t o = stack[--sp];
        out[n].addr = o;
        for (int i = 0; i < 12; i++) out[n].local[i] = (int32_t)RD32(o + 0x48 + 4 * i);
        n++;
        // children of o: first at +8, then each one's next sibling at +4
        for (uint32_t ch = RD32(o + 8), guard = 0; ch && sp < 64 && guard < MAX_OBJ; ch = RD32(ch + 4), guard++)
            stack[sp++] = ch;
    }
    return n;
}

static void write_local(uint32_t o, const int32_t *m) {
    for (int i = 0; i < 12; i++) WR32(o + 0x48 + 4 * i, (uint32_t)m[i]);
    WR16(o + 0xba, RD16(o + 0xba) | 1);   // world transform to be recomputed
}

static void mat_to_quat(const double *m, double *q) {
    double tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        double s = sqrt(1.0 + m[0] - m[4] - m[8]) * 2;
        q[0] = (m[7] - m[5]) / s; q[1] = 0.25 * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        double s = sqrt(1.0 + m[4] - m[0] - m[8]) * 2;
        q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25 * s; q[3] = (m[5] + m[7]) / s;
    } else {
        double s = sqrt(1.0 + m[8] - m[0] - m[4]) * 2;
        q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25 * s;
    }
}

static void quat_to_mat(const double *q, double *m) {
    double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w); m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w); m[7] = 2 * (y * z + x * w); m[8] = 1 - 2 * (x * x + y * y);
}

// Transform half way (t) from a to b; false when they are too far apart to be one motion
// (a camera cut), in which case b is shown.
static bool lerp_xform(const int32_t *a, const int32_t *b, double t, int32_t *out) {
    double ma[9], mb[9], sa[3], sb[3];
    for (int r = 0; r < 3; r++) {
        sa[r] = sqrt((double)a[r * 3] * a[r * 3] + (double)a[r * 3 + 1] * a[r * 3 + 1] + (double)a[r * 3 + 2] * a[r * 3 + 2]);
        sb[r] = sqrt((double)b[r * 3] * b[r * 3] + (double)b[r * 3 + 1] * b[r * 3 + 1] + (double)b[r * 3 + 2] * b[r * 3 + 2]);
        if (sa[r] < 1 || sb[r] < 1) return false;   // degenerate (scaled to nothing)
        for (int k = 0; k < 3; k++) { ma[r * 3 + k] = a[r * 3 + k] / sa[r]; mb[r * 3 + k] = b[r * 3 + k] / sb[r]; }
    }
    double dx = (double)b[9] - a[9], dy = (double)b[10] - a[10], dz = (double)b[11] - a[11];
    if (dx * dx + dy * dy + dz * dz > 8.0 * 65536 * 8.0 * 65536) return false;   // jumped more than ~8 m
    double qa[4], qb[4], q[4];
    mat_to_quat(ma, qa);
    mat_to_quat(mb, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) { d = -d; for (int i = 0; i < 4; i++) qb[i] = -qb[i]; }
    if (d > 0.9995) {
        for (int i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * t;
    } else {
        double th = acos(d), s = sin(th), wa = sin((1 - t) * th) / s, wb = sin(t * th) / s;
        for (int i = 0; i < 4; i++) q[i] = qa[i] * wa + qb[i] * wb;
    }
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
    double m[9];
    quat_to_mat(q, m);
    for (int r = 0; r < 3; r++) {
        double s = sa[r] + (sb[r] - sa[r]) * t;
        for (int k = 0; k < 3; k++) out[r * 3 + k] = (int32_t)lround(m[r * 3 + k] * s);
    }
    out[9] = (int32_t)lround(a[9] + dx * t);
    out[10] = (int32_t)lround(a[10] + dy * t);
    out[11] = (int32_t)lround(a[11] + dz * t);
    return true;
}

// Car shadows are not in the scene graph: while drawing, the game works out each car's shadow
// from its physics state as up to 11 points on the ground in world space (0x88-byte records from
// 0x800d5de0, the record of car i numbered by the byte at 0x800d68cc + 0x64c * i). On a frame
// drawn between two updates the shadow would be where the car is after the update, half a step
// ahead of the interpolated car and mostly hidden under it; it is moved along with the car.
#define NUM_CARS 0x800c6f9cu
#define CAR_SHADOW_IDX 0x800d68ccu
#define SHADOWS 0x800d5de0u
#define MAX_SHADOWS 8
#define SHADOW_WORDS 33
static struct { uint32_t addr; int32_t p[SHADOW_WORDS]; } shadow_true[MAX_SHADOWS];
static int nshadow;

static void restore(void) {
    if (!restore_pending) return;
    restore_pending = false;
    for (int i = 0; i < nsnap[cur]; i++) write_local(snap[cur][i].addr, snap[cur][i].local);
    for (int i = 0; i < nshadow; i++)
        for (int k = 0; k < SHADOW_WORDS; k++) WR32(shadow_true[i].addr + 4 * k, (uint32_t)shadow_true[i].p[k]);
    nshadow = 0;
}

// game: draws the shadow of every car.
int hook_draw_shadows(CPU *c) {
    (void)c;
    if (!restore_pending) return 0;   // not an interpolated frame
    int cars = (int32_t)RD32(NUM_CARS);
    uint32_t root = RD32(SCENE_ROOT);
    for (int i = 0; i < cars && nshadow < MAX_SHADOWS; i++) {
        uint32_t a = SHADOWS + (int8_t)RD8(CAR_SHADOW_IDX + 0x64c * i) * 0x88;
        int32_t p[SHADOW_WORDS];
        for (int k = 0; k < SHADOW_WORDS; k++) p[k] = (int32_t)RD32(a + 4 * k);
        // the car: the object at the top of the scene graph nearest to the shadow's first four
        // points, always in use (its true transform as of the update, and the interpolated one
        // it is drawn with)
        double cx = ((double)p[0] + p[3] + p[6] + p[9]) / 4, cy = ((double)p[1] + p[4] + p[7] + p[10]) / 4;
        double cz = ((double)p[2] + p[5] + p[8] + p[11]) / 4;
        const ObjXform *best = NULL;
        double best_d = 8.0 * 65536 * 8.0 * 65536;
        for (uint32_t o = root ? RD32(root + 8) : 0, guard = 0; o && guard < MAX_OBJ; o = RD32(o + 4), guard++) {
            for (int j = 0; j < nsnap[cur]; j++) {
                if (snap[cur][j].addr != o) continue;
                const int32_t *t = &snap[cur][j].local[9];
                double dx = cx - t[0], dy = cy - t[1], dz = cz - t[2];
                double d = dx * dx + dy * dy + dz * dz;
                if (d < best_d) { best_d = d; best = &snap[cur][j]; }
                break;
            }
        }
        if (!best) continue;
        shadow_true[nshadow].addr = a;
        memcpy(shadow_true[nshadow].p, p, sizeof p);
        nshadow++;
        // world = M * local + t (16.16): back into the car's frame with the true transform, out
        // with the interpolated one
        double mt[9], mm[9], tt[3], tm[3];
        for (int k = 0; k < 9; k++) {
            mt[k] = best->local[k] / 65536.0;
            mm[k] = (int32_t)RD32(best->addr + 0x48 + 4 * k) / 65536.0;
        }
        for (int k = 0; k < 3; k++) { tt[k] = best->local[9 + k]; tm[k] = (int32_t)RD32(best->addr + 0x6c + 4 * k); }
        for (int k = 0; k + 2 < SHADOW_WORDS; k += 3) {
            double d[3] = { p[k] - tt[0], p[k + 1] - tt[1], p[k + 2] - tt[2] }, l[3];
            for (int r = 0; r < 3; r++) l[r] = mt[r] * d[0] + mt[3 + r] * d[1] + mt[6 + r] * d[2];   // transpose
            for (int r = 0; r < 3; r++)
                WR32(a + 4 * (k + r), (uint32_t)(int32_t)lround(tm[r] + mm[r * 3] * l[0] + mm[r * 3 + 1] * l[1] + mm[r * 3 + 2] * l[2]));
        }
    }
    return 0;
}

// game: polls the controllers at the start of each main loop iteration; "newly pressed" is what
// changed since the previous poll. An iteration that only draws must not poll, or a press it sees
// is no longer new when the next update looks; its pressed flags (+0x60 of each of the 8 pad
// slots) are cleared instead, so drawing code cannot act on the last update's presses again.
int hook_pad_poll(CPU *c) {
    if (!race_60fps || !fast_loop || (long)(cpu.cycles / CYCLES_PER_FRAME) - last_update >= 2) return 0;
    for (int i = 0; i < 8; i++) WR32(0x800cfea0u + 0x3c * i, 0);
    return 1;
}

// game: one iteration of the main loop (state update, then its drawing). In the race and replay
// states of a 60 fps race the loop runs every vblank (gp+0xa7c is the game's frame interval),
// and every vblank has to fit a whole frame, so the emulated CPU runs twice as fast meanwhile;
// other states (fades between stages) keep the game's own pace.
int hook_main_iter(CPU *c) {
    static uint32_t saved_cpi;
    use_big_primbufs(c->r[28]);
    restore_view(c->r[28]);
    restore();
    update_ran = false;
    if (race_60fps) {
        uint32_t state = RD32(c->r[28] + 0x3e0);
        bool fast = fast_loop = requested_interval == 2 && (state == STATE_RACE || state == STATE_REPLAY);
        WR32(c->r[28] + 0xa7c, fast ? 1 : requested_interval);
        if (fast && !saved_cpi) {
            saved_cpi = cycles_per_insn;
            cycles_per_insn = 1;
        } else if (!fast && saved_cpi) {
            cycles_per_insn = saved_cpi;
            saved_cpi = 0;
        }
    }
    return 0;
}

// game: the update of the race and replay states. Skipped every second vblank.
int hook_race_update(CPU *c) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CMR_TRACE_60") != NULL;
    if (!race_60fps || !fast_loop) {
        if (trace) {   // the car's position before each update, for comparing with 60 fps runs
            uint32_t car = RD32(RD32(SCENE_ROOT) + 8);
            LOG("[60fps] tick pos %d %d %d\n", (int32_t)RD32(car + 0x6c), (int32_t)RD32(car + 0x70), (int32_t)RD32(car + 0x74));
        }
        return 0;
    }
    long v = (long)(cpu.cycles / CYCLES_PER_FRAME);
    if (v - last_update < 2) {
        c->r[2] = RD32(c->r[28] + 0x3e0);   // stay in the current state
        return 1;
    }
    if (v - last_update > 4) have_prev = false;   // back from a menu or another stage: start afresh
    if (trace) {
        uint32_t car = RD32(RD32(SCENE_ROOT) + 8);
        LOG("[60fps] update at vblank %ld (+%ld)\n", v, v - last_update);
        LOG("[60fps] tick pos %d %d %d\n", (int32_t)RD32(car + 0x6c), (int32_t)RD32(car + 0x70), (int32_t)RD32(car + 0x74));
    }
    last_update = v;
    update_ran = true;
    return 0;
}

// game: draws the 3D scene of every viewport and flips.
int hook_scene_render(CPU *c) {
    raise_view(c->r[28]);
    if (race_60fps && getenv("CMR_TRACE_60"))
        LOG("[60fps] draw at vblank %ld%s\n", (long)(cpu.cycles / CYCLES_PER_FRAME), update_ran ? " (after update)" : "");
    if (!race_60fps || !update_ran) return 0;
    update_ran = false;
    cur ^= 1;
    nsnap[cur] = snapshot(snap[cur]);
    const ObjXform *prev = snap[cur ^ 1];
    int np = nsnap[cur ^ 1];
    if (!have_prev) { have_prev = true; return 0; }
    // objects are visited in the same order each frame; match by address
    for (int i = 0, j = 0; i < nsnap[cur]; i++) {
        const ObjXform *o = &snap[cur][i];
        if (j >= np || prev[j].addr != o->addr) {
            for (j = 0; j < np && prev[j].addr != o->addr; j++) {}
            if (j == np) { j = 0; continue; }
        }
        int32_t mid[12];
        if (memcmp(prev[j].local, o->local, sizeof mid) && lerp_xform(prev[j].local, o->local, 0.5, mid))
            write_local(o->addr, mid);
        j++;
    }
    restore_pending = true;
    return 0;
}

// ------------------------------------------------------- draw distance and culling
// The game drops every polygon and object beyond a far limit (gp+0xa4c, in quarter
// screen-depth units; trigger zones along the stage change it as the car drives) and outside a
// view cone as wide as its 4:3 picture (coefficient gp+0xa2c, shared by both axes). While a scene
// is drawn, and only then, the far limit is raised by draw_distance (up to what the 16-bit screen
// depth can hold) and the cone is widened to the 16:9 picture plus a margin, so objects do not
// pop in at the edges; the game's own values are back before any of its logic runs. Fog moves
// out with the far limit where the stage has none in view (on clear stages the game's fog is
// black and normally out of reach); foggy and rainy stages keep their limits.
float draw_distance = 2.0f;
float cull_margin = 1.15f;
#define FAR_MAX 16383   // depth << 2 must stay within the 16-bit screen depth
static bool view_raised;
static uint32_t saved_far, saved_fog, saved_cone;   // the game's values
static uint32_t set_far, set_fog, set_cone;         // what was written in their place

static void raise_view(uint32_t gp) {
    extern bool widescreen_active;
    if (view_raised) return;
    view_raised = true;
    saved_far = RD32(gp + 0xa4c);
    saved_fog = RD32(gp + 0xa48);
    saved_cone = RD32(gp + 0xa2c);
    int32_t far = (int32_t)saved_far;
    if (draw_distance > 1.0f && (int32_t)saved_fog >= far) {
        far = (int32_t)((float)far * (draw_distance > 2.0f ? 2.0f : draw_distance));
        if (far > FAR_MAX) far = FAR_MAX;
        if (far < (int32_t)saved_far) far = (int32_t)saved_far;
        WR32(gp + 0xa4c, (uint32_t)far);
        if ((int32_t)saved_fog < far) WR32(gp + 0xa48, (uint32_t)far);
    }
    float k = cull_margin * (widescreen_active ? 4.0f / 3.0f : 1.0f);
    WR32(gp + 0xa2c, (uint32_t)(int32_t)((float)(int32_t)saved_cone * k));
    set_far = RD32(gp + 0xa4c); set_fog = RD32(gp + 0xa48); set_cone = RD32(gp + 0xa2c);
}

// A value the game set itself while drawing (a new view after a replay camera cut) is kept.
static void restore_view(uint32_t gp) {
    if (!view_raised) return;
    view_raised = false;
    if (RD32(gp + 0xa4c) == set_far) WR32(gp + 0xa4c, saved_far);
    if (RD32(gp + 0xa48) == set_fog) WR32(gp + 0xa48, saved_fog);
    if (RD32(gp + 0xa2c) == set_cone) WR32(gp + 0xa2c, saved_cone);
}

// Each frame's GPU primitives go into one of two buffers, 109000 bytes each on the console,
// written without checking their end; a renderer also drops detail once a frame passes half and
// five sixths of the size. The port points the game at two 3 MB buffers in RAM beyond the
// console's 2 MB instead, so neither a longer draw distance nor the budget can run out. The game
// still allocates its own (later allocations, and data that depends on where they land, stay
// as they were); those are handed back when it frees them.
#define PRIM_CURSOR 0x800b6398u
#define PRIMBUF_A 0x80200000u
#define PRIMBUF_B 0x80500000u
#define PRIMBUF_SIZE 0x300000u
static uint32_t heap_buf[2];   // the game's own buffers while ours are in use

static void use_big_primbufs(uint32_t gp) {
    uint32_t a = RD32(gp + 0xa88), b = RD32(gp + 0xa8c);
    if (!a || !b || a >= PRIMBUF_A) return;
    heap_buf[0] = a; heap_buf[1] = b;
    WR32(gp + 0xa88, PRIMBUF_A);
    WR32(gp + 0xa8c, PRIMBUF_B);
    WR32(gp + 0xa90, PRIMBUF_SIZE);
}

// game: FreePrimitiveBuffers() — give the game back its own buffers to free.
int hook_free_primbuf(CPU *c) {
    uint32_t gp = c->r[28];
    if (RD32(gp + 0xa88) >= PRIMBUF_A) WR32(gp + 0xa88, heap_buf[0]);
    if (RD32(gp + 0xa8c) >= PRIMBUF_A) WR32(gp + 0xa8c, heap_buf[1]);
    return 0;
}

// game: the flip at the end of drawing a frame. CMR_TRACE_PRIMBUF=1 logs the buffer use.
int hook_frame_end(CPU *c) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CMR_TRACE_PRIMBUF") != NULL;
    uint32_t cur = RD32(PRIM_CURSOR);
    if (trace) LOG("[primbuf] %u bytes, far %d\n", cur - (cur >= PRIMBUF_B ? PRIMBUF_B : PRIMBUF_A), (int32_t)RD32(c->r[28] + 0xa4c));
    restore_view(c->r[28]);
    return 0;
}

// ---------------------------------------------------------------- unlock all
// The frontend keeps the player's progress in its GAME_INFO block (pointer at 0x80179f48, magic
// 0x87651234; saved to the memory card whole, and copied into each championship save). Its word
// at +0x18 holds what is unlocked: bits 0-3 novice countries, 4-7 countries (8 = all), 8 expert
// mode, 9-11 super-specials (5 = all), 12-15 one bit per bonus car; above that the cheat toggles.
// With unlock_all the frontend sees everything unlocked, as after the game's own unlock code plus
// expert mode and the bonus cars, but the player's own progress is what gets loaded, merged and
// saved: the functions doing that run with it in place, and the memory card is written with it.
// Nothing is unlocked outside the frontend.
bool unlock_all = false;
#define GAME_INFO_PTR 0x80179f48u
#define GAME_INFO_MAGIC 0x87651234u
#define UNLOCKS 0x18
#define ALL_UNLOCKED 0xfb80u   // cars 0xf000, super-specials 5 << 9, expert 0x100, countries 8 << 4
static uint32_t info_addr;     // the block while unlocks are applied (0: not applied)
static uint32_t own_unlocks;   // the player's own word
static uint32_t set_unlocks;   // what was written in its place

static bool in_frontend(void) {
    const Module *m = module_active();
    return m && !strcmp(m->name, "frontend");
}

static uint32_t game_info(void) {
    uint32_t p = RD32(GAME_INFO_PTR);
    return (p >> 24) == 0x80 && RD32(p) == GAME_INFO_MAGIC ? p : 0;
}

static void apply_unlocks(void) {
    set_unlocks = (own_unlocks & 0xffff000fu) | ALL_UNLOCKED;
    WR32(info_addr + UNLOCKS, set_unlocks);
}

// Takes in what the game changed in the word since it was last set.
static void update_own(uint32_t cur) {
    own_unlocks = (own_unlocks & ~(cur ^ set_unlocks)) | (cur & (cur ^ set_unlocks));
}

// Every vblank: unlocks on while the frontend runs, the player's own word back otherwise.
void unlock_all_vblank(void) {
    uint32_t p = unlock_all && in_frontend() ? game_info() : 0;
    if (info_addr && p != info_addr) {
        if (RD32(info_addr) == GAME_INFO_MAGIC && RD32(info_addr + UNLOCKS) == set_unlocks)
            WR32(info_addr + UNLOCKS, own_unlocks);
        info_addr = 0;
    }
    if (!p) return;
    uint32_t cur = RD32(p + UNLOCKS);
    if (!info_addr) own_unlocks = cur;
    else update_own(cur);
    info_addr = p;
    apply_unlocks();
}

// Runs the hooked function with the player's own word in place; what it leaves there is theirs.
static int with_own_unlocks(CPU *c, uint32_t fn, bool *busy) {
    if (!info_addr || *busy || game_info() != info_addr) return 0;
    *busy = true;
    update_own(RD32(info_addr + UNLOCKS));
    WR32(info_addr + UNLOCKS, own_unlocks);
    dispatch_call(c, fn);
    own_unlocks = RD32(info_addr + UNLOCKS);
    apply_unlocks();
    *busy = false;
    return 1;
}

// frontend: loads GAME_INFO from the memory card, keeping unlocks already in memory.
int hook_load_progress(CPU *c) { static bool busy; return with_own_unlocks(c, 0x8018ee64u, &busy); }
// frontend: merges the unlocks recorded in the championship saves.
int hook_merge_unlocks(CPU *c) { static bool busy; return with_own_unlocks(c, 0x8018ec18u, &busy); }
// frontend: copies the progress into the championship being started (and so into its save).
int hook_copy_progress(CPU *c) { static bool busy; return with_own_unlocks(c, 0x80187f10u, &busy); }
// BIOS write() to the memory card (the game's card writes complete a few frames after it asks
// for them): GAME_INFO goes out with the player's own word.
void unlock_all_card_write(uint32_t src, uint32_t len, bool before) {
    if (!info_addr || info_addr + UNLOCKS < src || info_addr + UNLOCKS + 4 > src + len) return;
    if (before) {
        update_own(RD32(info_addr + UNLOCKS));
        WR32(info_addr + UNLOCKS, own_unlocks);
    } else {
        apply_unlocks();
    }
}
