// 60 fps races. The game simulates a fixed 1/30 s per update, so the race loop runs every vblank
// but the update only every second one; the frame drawn in between shows each object's local
// transform half way between the last two updates, and the true ones are back before the game
// runs again. The race is shown one vblank late.
#include "psx.h"
#include "symbols.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int frame_interval_override = 0;   // 0 = game default
bool race_60fps = true;

#define MAX_OBJ 1024

static uint32_t requested_interval = 1;   // what the game last asked for

// game: SetFrameInterval(n), n vblanks per frame (2 = 30 fps).
int hook_set_frame_interval(CPU *c, RecompFn orig) {
    if (frame_interval_override && c->r[4] > 1) c->r[4] = (uint32_t)frame_interval_override;
    requested_interval = c->r[4];
    return 0;
}

typedef struct { uint32_t addr; int32_t local[12]; } ObjXform;
static ObjXform snap[2][MAX_OBJ];
static int nsnap[2], cur;            // snap[cur] is the latest update, snap[cur ^ 1] the one before
static bool have_prev, restore_pending, update_ran;
static bool fast_loop;               // the loop runs every vblank
static long last_update = -10;       // vblank of the last race update

static int snapshot(ObjXform *out) {
    static uint32_t stack[MAX_OBJ];
    int n = 0, sp = 0;
    uint32_t root = RD32(SCENE_ROOT);
    if (!root) return 0;
    stack[sp++] = root;
    while (sp && n < MAX_OBJ) {
        uint32_t o = stack[--sp];
        out[n].addr = o;
        for (int i = 0; i < 12; i++) out[n].local[i] = (int32_t)RD32(o + OBJ_LOCAL + 4 * i);
        n++;
        for (uint32_t ch = RD32(o + 8), guard = 0; ch && sp < MAX_OBJ && guard < MAX_OBJ; ch = RD32(ch + 4), guard++)
            stack[sp++] = ch;
    }
    return n;
}

static void write_local(uint32_t o, const int32_t *m) {
    for (int i = 0; i < 12; i++) WR32(o + OBJ_LOCAL + 4 * i, (uint32_t)m[i]);
    WR16(o + OBJ_FLAGS, RD16(o + OBJ_FLAGS) | 1);
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

// Rotations are interpolated as rotations so spinning wheels keep their shape. A jump of more than
// ~8 m is a camera cut (every cut seen in replays moves at least 12 m); no rotation limit, since
// wheels and the orbiting replay camera turn up to 180 degrees per update.
static bool lerp_xform(const int32_t *a, const int32_t *b, double t, int32_t *out) {
    double ma[9], mb[9], sa[3], sb[3];
    for (int r = 0; r < 3; r++) {
        sa[r] = sqrt((double)a[r * 3] * a[r * 3] + (double)a[r * 3 + 1] * a[r * 3 + 1] + (double)a[r * 3 + 2] * a[r * 3 + 2]);
        sb[r] = sqrt((double)b[r * 3] * b[r * 3] + (double)b[r * 3 + 1] * b[r * 3 + 1] + (double)b[r * 3 + 2] * b[r * 3 + 2]);
        if (sa[r] < 1 || sb[r] < 1) return false;
        for (int k = 0; k < 3; k++) { ma[r * 3 + k] = a[r * 3 + k] / sa[r]; mb[r * 3 + k] = b[r * 3 + k] / sb[r]; }
    }
    double dx = (double)b[9] - a[9], dy = (double)b[10] - a[10], dz = (double)b[11] - a[11];
    if (dx * dx + dy * dy + dz * dz > 8.0 * 65536 * 8.0 * 65536) return false;
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

// Car shadows are not in the object tree: the game works them out from the physics while
// drawing, as up to 11 world-space points, so on an in-between frame they sit where the car is
// after the update. They are moved along with the car.
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
int hook_draw_shadows(CPU *c, RecompFn orig) {
    if (!restore_pending) return 0;   // not an in-between frame
    int cars = (int32_t)RD32(NUM_CARS);
    uint32_t root = RD32(SCENE_ROOT);
    for (int i = 0; i < cars && nshadow < MAX_SHADOWS; i++) {
        uint32_t a = SHADOWS + (int8_t)RD8(CAR_SHADOW_IDX + 0x64c * i) * 0x88;
        int32_t p[SHADOW_WORDS];
        for (int k = 0; k < SHADOW_WORDS; k++) p[k] = (int32_t)RD32(a + 4 * k);
        // the car: the top-level object nearest the centre of the first four points
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
        // into the car's frame with its true transform, out with the interpolated one
        double mt[9], mm[9], tt[3], tm[3];
        for (int k = 0; k < 9; k++) {
            mt[k] = best->local[k] / 65536.0;
            mm[k] = (int32_t)RD32(best->addr + OBJ_LOCAL + 4 * k) / 65536.0;
        }
        for (int k = 0; k < 3; k++) { tt[k] = best->local[9 + k]; tm[k] = (int32_t)RD32(best->addr + OBJ_LOCAL + 0x24 + 4 * k); }
        for (int k = 0; k + 2 < SHADOW_WORDS; k += 3) {
            double d[3] = { p[k] - tt[0], p[k + 1] - tt[1], p[k + 2] - tt[2] }, l[3];
            for (int r = 0; r < 3; r++) l[r] = mt[r] * d[0] + mt[3 + r] * d[1] + mt[6 + r] * d[2];   // transpose
            for (int r = 0; r < 3; r++)
                WR32(a + 4 * (k + r), (uint32_t)(int32_t)lround(tm[r] + mm[r * 3] * l[0] + mm[r * 3 + 1] * l[1] + mm[r * 3 + 2] * l[2]));
        }
    }
    return 0;
}

// game: polls the controllers; "newly pressed" is what changed since the last poll. A loop
// iteration that only draws must not poll, or the next update would miss a press; the pressed
// flags are cleared instead so drawing code cannot act on the last update's presses again.
int hook_pad_poll(CPU *c, RecompFn orig) {
    if (!race_60fps || !fast_loop || (long)(cpu.cycles / CYCLES_PER_FRAME) - last_update >= 2) return 0;
    for (int i = 0; i < 8; i++) WR32(PAD_PRESSED + 0x3c * i, 0);
    return 1;
}

// game: one iteration of the main loop. A whole frame has to fit in every vblank now, so the
// emulated CPU runs twice as fast meanwhile; fades between stages keep the game's own pace.
int hook_main_iter(CPU *c, RecompFn orig) {
    static uint32_t saved_cpi;
    primbufs_use(c->r[28]);
    view_restore(c->r[28]);
    restore();
    update_ran = false;
    if (race_60fps) {
        uint32_t state = RD32(c->r[28] + GP_STATE);
        bool fast = fast_loop = requested_interval == 2 && (state == STATE_RACE || state == STATE_REPLAY);
        WR32(c->r[28] + GP_INTERVAL, fast ? 1 : requested_interval);
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

// CMR_TRACE_60=1 logs the car's position before each update; 30 and 60 fps runs must match.
static void trace_tick(void) {
    uint32_t car = RD32(RD32(SCENE_ROOT) + 8);
    LOG("[60fps] tick pos %d %d %d\n", (int32_t)RD32(car + 0x6c), (int32_t)RD32(car + 0x70), (int32_t)RD32(car + 0x74));
}

// game: the update of the race and replay states.
int hook_race_update(CPU *c, RecompFn orig) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CMR_TRACE_60") != NULL;
    if (!race_60fps || !fast_loop) {
        if (trace) trace_tick();
        return 0;
    }
    long v = (long)(cpu.cycles / CYCLES_PER_FRAME);
    if (v - last_update < 2) {
        c->r[2] = RD32(c->r[28] + GP_STATE);   // stay in the current state
        return 1;
    }
    if (v - last_update > 4) have_prev = false;   // back from a menu or another stage
    if (trace) {
        LOG("[60fps] update at vblank %ld (+%ld)\n", v, v - last_update);
        trace_tick();
    }
    last_update = v;
    update_ran = true;
    return 0;
}

// game: draws the 3D scene of every viewport and flips.
int hook_scene_render(CPU *c, RecompFn orig) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CMR_TRACE_60") != NULL;
    view_raise(c->r[28]);
    if (race_60fps && trace)
        LOG("[60fps] draw at vblank %ld%s\n", (long)(cpu.cycles / CYCLES_PER_FRAME), update_ran ? " (after update)" : "");
    if (!race_60fps || !update_ran) return 0;
    update_ran = false;
    cur ^= 1;
    nsnap[cur] = snapshot(snap[cur]);
    const ObjXform *prev = snap[cur ^ 1];
    int np = nsnap[cur ^ 1];
    if (!have_prev) { have_prev = true; return 0; }
    // objects come in the same order each frame; match by address
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
