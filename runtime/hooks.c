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

static void restore(void) {
    if (!restore_pending) return;
    restore_pending = false;
    for (int i = 0; i < nsnap[cur]; i++) write_local(snap[cur][i].addr, snap[cur][i].local);
}

// game: one iteration of the main loop (state update, then its drawing). In the race and replay
// states of a 60 fps race the loop runs every vblank (gp+0xa7c is the game's frame interval),
// and every vblank has to fit a whole frame, so the emulated CPU runs twice as fast meanwhile;
// other states (fades between stages) keep the game's own pace.
int hook_main_iter(CPU *c) {
    static uint32_t saved_cpi;
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
