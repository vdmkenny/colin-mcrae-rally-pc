// Sub-pixel vertex precision: the GTE's unrounded screen coordinates and depths are
// remembered for each RAM word the game stores them to. When the GPU later reads a vertex
// from that word, and it still holds the same integer value, the precise position is used
// by the high-resolution renderer (no polygon wobble, perspective-correct textures).
#include "pgxp.h"

typedef struct { float x, y, w; uint32_t value; uint32_t key; } PgxpVal;

static PgxpVal shadow_ram[0x200000 / 4];
static PgxpVal shadow_scratch[0x400 / 4];
static PgxpVal gpr[32];
bool pgxp_enabled = true;

// precise SXY fifo, maintained by the GTE alongside the integer one
float pgxp_sxy[3][3];
bool pgxp_sxy_valid[3];
uint32_t pgxp_sxy_key[3];

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
        s->key = pgxp_sxy_key[i];
        s->value = v;
    } else {
        s->value = ~v;   // never matches
    }
}

void pgxp_mfc2(int rt, int reg, uint32_t value) {
    int i = reg == 15 ? 2 : reg - 12;
    if (reg >= 12 && reg <= 15 && pgxp_sxy_valid[i]) {
        gpr[rt].x = pgxp_sxy[i][0]; gpr[rt].y = pgxp_sxy[i][1]; gpr[rt].w = pgxp_sxy[i][2];
        gpr[rt].key = pgxp_sxy_key[i];
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

bool pgxp_lookup(uint32_t addr, uint32_t value, float *x, float *y, float *w, uint32_t *key) {
    if (!pgxp_enabled || addr == PGXP_NO_ADDR) return false;
    PgxpVal *s = slot(addr);
    if (!s || s->value != value) return false;
    *x = s->x; *y = s->y; *w = s->w; *key = s->key;
    return true;
}

// Where each GTE input vertex (V0..V2) was loaded from, for vertex identity.
static struct { uint32_t addr, value; } prov[32];
uint32_t pgxp_vsrc[3];

void pgxp_prov(int rt, uint32_t addr, uint32_t value) { prov[rt].addr = addr; prov[rt].value = value; }

void pgxp_vsrc_reg(int v, int rt, uint32_t value) {
    pgxp_vsrc[v] = prov[rt].value == value ? prov[rt].addr : 0;
}

void pgxp_vsrc_addr(int v, uint32_t addr) { pgxp_vsrc[v] = addr; }

// Vertex identity (nonzero for every GTE-projected vertex): where the input vertex was loaded
// from, or its object-space coordinates when that is unknown.
void pgxp_new_frame(void) {}

uint32_t pgxp_vertex_key(int v, int16_t x, int16_t y, int16_t z) {
    uint32_t h;
    if (pgxp_vsrc[v]) h = ((pgxp_vsrc[v] & 0x1FFFFFFF) * 0x9E3779B1u) ^ 0x5BD1E995u;
    else h = (uint16_t)x * 0x9E3779B1u ^ (uint16_t)y * 0x85EBCA77u ^ (uint16_t)z * 0xC2B2AE3Du;
    h ^= h >> 15;
    return h | 1;   // 0 means "no key"
}
