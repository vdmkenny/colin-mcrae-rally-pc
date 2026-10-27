// Sub-pixel vertex precision: the GTE's unrounded screen coordinates and depths are
// remembered for each RAM word the game stores them to. When the GPU later reads a vertex
// from that word, and it still holds the same integer value, the precise position is used
// by the high-resolution renderer (no polygon wobble, perspective-correct textures).
#include "pgxp.h"

typedef struct { float x, y, w; uint32_t value; } PgxpVal;

static PgxpVal shadow_ram[0x200000 / 4];
static PgxpVal shadow_scratch[0x400 / 4];
static PgxpVal gpr[32];
bool pgxp_enabled = true;

// precise SXY fifo, maintained by the GTE alongside the integer one
float pgxp_sxy[3][3];
bool pgxp_sxy_valid[3];

static PgxpVal *slot(uint32_t addr) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) return &shadow_ram[(p & 0x1FFFFC) >> 2];
    if ((p & 0xFFFFFC00) == 0x1F800000) return &shadow_scratch[(p & 0x3FC) >> 2];
    return NULL;
}

void pgxp_swc2(uint32_t addr, int reg) {
    uint32_t v = gte_read_data(reg);
    WR32(addr, v);
    PgxpVal *s = slot(addr);
    if (!s) return;
    int i = reg == 15 ? 2 : reg - 12;
    if (pgxp_enabled && reg >= 12 && reg <= 15 && pgxp_sxy_valid[i]) {
        s->x = pgxp_sxy[i][0]; s->y = pgxp_sxy[i][1]; s->w = pgxp_sxy[i][2];
        s->value = v;
    } else {
        s->value = ~v;   // never matches
    }
}

void pgxp_mfc2(int rt, int reg, uint32_t value) {
    int i = reg == 15 ? 2 : reg - 12;
    if (reg >= 12 && reg <= 15 && pgxp_sxy_valid[i]) {
        gpr[rt].x = pgxp_sxy[i][0]; gpr[rt].y = pgxp_sxy[i][1]; gpr[rt].w = pgxp_sxy[i][2];
        gpr[rt].value = value;
    } else {
        gpr[rt].value = ~value;
    }
}

void pgxp_sw(uint32_t addr, int rt, uint32_t value) {
    WR32(addr, value);
    PgxpVal *s = slot(addr);
    if (!s) return;
    if (gpr[rt].value == value) *s = gpr[rt];
    else s->value = ~value;
}

bool pgxp_lookup(uint32_t addr, uint32_t value, float *x, float *y, float *w) {
    if (!pgxp_enabled || addr == PGXP_NO_ADDR) return false;
    PgxpVal *s = slot(addr);
    if (!s || s->value != value) return false;
    *x = s->x; *y = s->y; *w = s->w;
    return true;
}
