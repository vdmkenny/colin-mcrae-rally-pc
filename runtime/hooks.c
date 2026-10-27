// Native replacements and adjustments for specific game functions (see tools/hooks.txt).
#include "psx.h"

int frame_interval_override = 0;   // 0 = game default

// game: SetFrameInterval(n) — the race renders every n vblanks (2 = 30 fps).
int hook_set_frame_interval(CPU *c) {
    if (frame_interval_override && c->r[4] > 1) c->r[4] = (uint32_t)frame_interval_override;
    return 0;
}
