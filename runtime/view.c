// Draw distance, culling and primitive buffers. While a scene is drawn, and only then, the far
// limit is raised by draw_distance and the view cone widened to the 16:9 picture plus a margin;
// the game's values are back before any of its logic runs, since trigger zones along the stage
// set them. Fog moves out with the far limit where the stage has none in view (on clear stages
// it is black and normally out of reach).
#include "psx.h"
#include "render.h"
#include "symbols.h"
#include <stdlib.h>

float draw_distance = 2.0f;
float cull_margin = 1.15f;
#define FAR_MAX 16383   // depth << 2 must stay within the 16-bit screen depth
static bool view_raised;
static uint32_t saved_far, saved_fog, saved_cone, saved_ot_far;   // the game's values
static uint32_t set_far, set_fog, set_cone, set_ot_far;           // what was written in their place

void view_raise(uint32_t gp) {
    if (view_raised) return;
    view_raised = true;
    saved_far = RD32(gp + GP_FAR);
    saved_fog = RD32(gp + GP_FOG);
    saved_cone = RD32(gp + GP_CONE);
    saved_ot_far = RD32(OT_FAR);
    int32_t far = (int32_t)saved_far;
    if (draw_distance > 1.0f && (int32_t)saved_fog >= far) {
        far = (int32_t)((float)far * (draw_distance > 2.0f ? 2.0f : draw_distance));
        if (far > FAR_MAX) far = FAR_MAX;
        if (far < (int32_t)saved_far) far = (int32_t)saved_far;
        WR32(gp + GP_FAR, (uint32_t)far);
        if ((int32_t)saved_fog < far) WR32(gp + GP_FOG, (uint32_t)far);
        // The ordering table's depth shift is worked out from OT_FAR so that the far limit fills
        // half the table, leaving the rest for depth offsets; sized for the stage-load limit,
        // raised depths plus an offset run off the end and wrap to the nearest slots.
        if ((int32_t)saved_ot_far < far) WR32(OT_FAR, (uint32_t)far);
    }
    float k = cull_margin * (widescreen_active ? 4.0f / 3.0f : 1.0f);
    WR32(gp + GP_CONE, (uint32_t)(int32_t)((float)(int32_t)saved_cone * k));
    set_far = RD32(gp + GP_FAR); set_fog = RD32(gp + GP_FOG); set_cone = RD32(gp + GP_CONE); set_ot_far = RD32(OT_FAR);
}

// A value the game set itself while drawing (a new view after a replay camera cut) is kept.
void view_restore(uint32_t gp) {
    if (!view_raised) return;
    view_raised = false;
    if (RD32(gp + GP_FAR) == set_far) WR32(gp + GP_FAR, saved_far);
    if (RD32(gp + GP_FOG) == set_fog) WR32(gp + GP_FOG, saved_fog);
    if (RD32(gp + GP_CONE) == set_cone) WR32(gp + GP_CONE, saved_cone);
    if (RD32(OT_FAR) == set_ot_far) WR32(OT_FAR, saved_ot_far);
}

// The console's two primitive buffers (109000 bytes each) are written without an end check, and
// renderers drop detail past half and five sixths of them. The game is pointed at two 3 MB
// buffers above the console's 2 MB instead; its own stay allocated, so later allocations land
// where they did, and are handed back when it frees them.
#define PRIMBUF_A 0x80200000u
#define PRIMBUF_B 0x80500000u
#define PRIMBUF_SIZE 0x300000u
static uint32_t heap_buf[2];   // the game's own buffers while ours are in use

void primbufs_use(uint32_t gp) {
    uint32_t a = RD32(gp + GP_PRIMBUF_A), b = RD32(gp + GP_PRIMBUF_B);
    if (!a || !b || a >= PRIMBUF_A) return;
    heap_buf[0] = a; heap_buf[1] = b;
    WR32(gp + GP_PRIMBUF_A, PRIMBUF_A);
    WR32(gp + GP_PRIMBUF_B, PRIMBUF_B);
    WR32(gp + GP_PRIMBUF_SIZE, PRIMBUF_SIZE);
}

// game: FreePrimitiveBuffers().
int hook_free_primbuf(CPU *c, RecompFn orig) {
    uint32_t gp = c->r[28];
    if (RD32(gp + GP_PRIMBUF_A) >= PRIMBUF_A) WR32(gp + GP_PRIMBUF_A, heap_buf[0]);
    if (RD32(gp + GP_PRIMBUF_B) >= PRIMBUF_A) WR32(gp + GP_PRIMBUF_B, heap_buf[1]);
    return 0;
}

// game: the flip at the end of a frame. CMR_TRACE_PRIMBUF=1 logs the buffer use.
int hook_frame_end(CPU *c, RecompFn orig) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CMR_TRACE_PRIMBUF") != NULL;
    uint32_t cur = RD32(PRIM_CURSOR);
    if (trace) LOG("[primbuf] %u bytes, far %d\n", cur - (cur >= PRIMBUF_B ? PRIMBUF_B : PRIMBUF_A), (int32_t)RD32(c->r[28] + GP_FAR));
    view_restore(c->r[28]);
    return 0;
}
