// Geometry Transformation Engine (COP2), bit-accurate to the hardware.
#include "psx.h"
#include "pgxp.h"
#include "render.h"
#include <stdlib.h>

static int16_t V[3][3];          // vectors 0-2 (x,y,z)
static uint8_t RGBC[4];
static uint16_t OTZ;
static int16_t IR[4];
static int16_t SXY[3][2];         // screen fifo
static bool sxy_clip[3];          // the precise position differs from SXY (see rtp)
static uint16_t SZ[4];            // z fifo
static uint8_t RGB[3][4];         // colour fifo
static uint32_t RES1;
static int32_t MAC[4];
static uint32_t LZCS, LZCR;

static int16_t RT[3][3], LLM[3][3], LCM[3][3];
static int32_t TR[3], BK[3], FC[3];
static int32_t OFX, OFY;
static uint16_t H;
int gte_projections;   // vertices projected (the game is drawing 3D)
static int16_t DQA, ZSF3, ZSF4;
static int32_t DQB;
static uint32_t FLAG;

static uint8_t unr_table[257];

// Debug aids, read once: CMR_DUMP_GTE=<frame> logs the first use of each command (and the
// lighting registers at the first NCCT) from that frame on; CMR_TRACE_LIGHT logs writes to
// the light colour matrix and ambient colour.
static long dump_gte_from = -1;
static bool trace_light;

void gte_init(void) {
    const char *e = getenv("CMR_DUMP_GTE");
    dump_gte_from = e ? atol(e) : -1;
    trace_light = getenv("CMR_TRACE_LIGHT") != NULL;
    for (int i = 0; i < 257; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = v > 0 ? (uint8_t)v : 0;
    }
}

static int clz32(uint32_t v) {
    int n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

// ---------------------------------------------------------------- registers
uint32_t gte_read_data(int r) {
    switch (r) {
    case 0: return (uint16_t)V[0][0] | ((uint32_t)(uint16_t)V[0][1] << 16);
    case 1: return (uint32_t)(int32_t)V[0][2];
    case 2: return (uint16_t)V[1][0] | ((uint32_t)(uint16_t)V[1][1] << 16);
    case 3: return (uint32_t)(int32_t)V[1][2];
    case 4: return (uint16_t)V[2][0] | ((uint32_t)(uint16_t)V[2][1] << 16);
    case 5: return (uint32_t)(int32_t)V[2][2];
    case 6: return RGBC[0] | (RGBC[1] << 8) | (RGBC[2] << 16) | ((uint32_t)RGBC[3] << 24);
    case 7: return OTZ;
    case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)IR[r - 8];
    case 12: case 13: case 14:
        return (uint16_t)SXY[r - 12][0] | ((uint32_t)(uint16_t)SXY[r - 12][1] << 16);
    case 15: return (uint16_t)SXY[2][0] | ((uint32_t)(uint16_t)SXY[2][1] << 16);
    case 16: case 17: case 18: case 19: return SZ[r - 16];
    case 20: case 21: case 22:
        return RGB[r - 20][0] | (RGB[r - 20][1] << 8) | (RGB[r - 20][2] << 16) | ((uint32_t)RGB[r - 20][3] << 24);
    case 23: return RES1;
    case 24: case 25: case 26: case 27: return (uint32_t)MAC[r - 24];
    case 28: case 29: {
        uint32_t rr = IR[1] < 0 ? 0 : (IR[1] >> 7 > 0x1F ? 0x1F : IR[1] >> 7);
        uint32_t gg = IR[2] < 0 ? 0 : (IR[2] >> 7 > 0x1F ? 0x1F : IR[2] >> 7);
        uint32_t bb = IR[3] < 0 ? 0 : (IR[3] >> 7 > 0x1F ? 0x1F : IR[3] >> 7);
        return rr | (gg << 5) | (bb << 10);
    }
    case 30: return LZCS;
    case 31: return LZCR;
    }
    return 0;
}

void gte_write_data(int r, uint32_t v) {
    switch (r) {
    case 0: V[0][0] = (int16_t)v; V[0][1] = (int16_t)(v >> 16); pgxp_vsrc[0] = 0; break;
    case 1: V[0][2] = (int16_t)v; break;
    case 2: V[1][0] = (int16_t)v; V[1][1] = (int16_t)(v >> 16); pgxp_vsrc[1] = 0; break;
    case 3: V[1][2] = (int16_t)v; break;
    case 4: V[2][0] = (int16_t)v; V[2][1] = (int16_t)(v >> 16); pgxp_vsrc[2] = 0; break;
    case 5: V[2][2] = (int16_t)v; break;
    case 6: RGBC[0] = v; RGBC[1] = v >> 8; RGBC[2] = v >> 16; RGBC[3] = v >> 24; break;
    case 7: OTZ = (uint16_t)v; break;
    case 8: case 9: case 10: case 11: IR[r - 8] = (int16_t)v; break;
    case 12: case 13: case 14:
        SXY[r - 12][0] = (int16_t)v; SXY[r - 12][1] = (int16_t)(v >> 16);
        pgxp_sxy_valid[r - 12] = false;
        sxy_clip[r - 12] = false;
        break;
    case 15:
        SXY[0][0] = SXY[1][0]; SXY[0][1] = SXY[1][1];
        SXY[1][0] = SXY[2][0]; SXY[1][1] = SXY[2][1];
        SXY[2][0] = (int16_t)v; SXY[2][1] = (int16_t)(v >> 16);
        memcpy(pgxp_sxy[0], pgxp_sxy[1], sizeof pgxp_sxy[0]);
        memcpy(pgxp_sxy[1], pgxp_sxy[2], sizeof pgxp_sxy[0]);
        pgxp_sxy_valid[0] = pgxp_sxy_valid[1];
        pgxp_sxy_valid[1] = false;
        pgxp_sxy_valid[2] = false;
        pgxp_sxy_key[0] = pgxp_sxy_key[1];
        sxy_clip[0] = sxy_clip[1];
        sxy_clip[1] = sxy_clip[2] = false;
        break;
    case 16: case 17: case 18: case 19: SZ[r - 16] = (uint16_t)v; break;
    case 20: case 21: case 22:
        RGB[r - 20][0] = v; RGB[r - 20][1] = v >> 8; RGB[r - 20][2] = v >> 16; RGB[r - 20][3] = v >> 24;
        break;
    case 23: RES1 = v; break;
    case 24: case 25: case 26: case 27: MAC[r - 24] = (int32_t)v; break;
    case 28:
        IR[1] = (int16_t)((v & 0x1F) << 7);
        IR[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        IR[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: break;
    case 30:
        LZCS = v;
        LZCR = (v & 0x80000000u) ? clz32(~v) : clz32(v);
        break;
    case 31: break;
    }
}

static void mat_write(int16_t m[3][3], int i, uint32_t v) {
    int16_t *p = &m[0][0];
    if (i < 4) { p[i * 2] = (int16_t)v; p[i * 2 + 1] = (int16_t)(v >> 16); }
    else p[8] = (int16_t)v;
}
static uint32_t mat_read(int16_t m[3][3], int i) {
    int16_t *p = &m[0][0];
    if (i < 4) return (uint16_t)p[i * 2] | ((uint32_t)(uint16_t)p[i * 2 + 1] << 16);
    return (uint32_t)(int32_t)p[8];
}

uint32_t gte_read_ctrl(int r) {
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: return mat_read(RT, r);
    case 5: case 6: case 7: return (uint32_t)TR[r - 5];
    case 8: case 9: case 10: case 11: case 12: return mat_read(LLM, r - 8);
    case 13: case 14: case 15: return (uint32_t)BK[r - 13];
    case 16: case 17: case 18: case 19: case 20: return mat_read(LCM, r - 16);
    case 21: case 22: case 23: return (uint32_t)FC[r - 21];
    case 24: return (uint32_t)OFX;
    case 25: return (uint32_t)OFY;
    case 26: return (uint32_t)(int32_t)(int16_t)H;   // hardware sign-extends on read
    case 27: return (uint32_t)(int32_t)DQA;
    case 28: return (uint32_t)DQB;
    case 29: return (uint32_t)(int32_t)ZSF3;
    case 30: return (uint32_t)(int32_t)ZSF4;
    case 31: return FLAG;
    }
    return 0;
}

void gte_write_ctrl(int r, uint32_t v) {
#ifdef RECOMP_TRACE
    if (trace_light && r >= 13 && r <= 20) {
        extern uint32_t trace_stack[1024];
        extern int trace_depth;
        LOG("[gte] ctrl %d = %08x @frame %llu in %08x <- %08x\n", r, v, (unsigned long long)(cpu.cycles / CYCLES_PER_FRAME),
            trace_depth > 0 ? trace_stack[trace_depth - 1] : 0, trace_depth > 1 ? trace_stack[trace_depth - 2] : 0);
    }
#endif
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: mat_write(RT, r, v); break;
    case 5: case 6: case 7: TR[r - 5] = (int32_t)v; break;
    case 8: case 9: case 10: case 11: case 12: mat_write(LLM, r - 8, v); break;
    case 13: case 14: case 15: BK[r - 13] = (int32_t)v; break;
    case 16: case 17: case 18: case 19: case 20: mat_write(LCM, r - 16, v); break;
    case 21: case 22: case 23: FC[r - 21] = (int32_t)v; break;
    case 24: OFX = (int32_t)v; break;
    case 25: OFY = (int32_t)v; break;
    case 26: H = (uint16_t)v; break;
    case 27: DQA = (int16_t)v; break;
    case 28: DQB = (int32_t)v; break;
    case 29: ZSF3 = (int16_t)v; break;
    case 30: ZSF4 = (int16_t)v; break;
    case 31:
        FLAG = v & 0x7FFFF000u;
        if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
        break;
    }
}

// ---------------------------------------------------------- arithmetic helpers
static int sf_shift;   // 0 or 12
static int lm_flag;

static int64_t mac_check(int i, int64_t v) {
    // 44-bit overflow checks on MAC1-3
    if (v > 0x7FFFFFFFFFFLL) FLAG |= 1u << (31 - i);
    else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
    return (v << 20) >> 20;   // wrap to 44 bits
}

static int32_t set_mac(int i, int64_t v) {
    mac_check(i, v);
    MAC[i] = (int32_t)(v >> sf_shift);
    return MAC[i];
}

static void set_mac0(int64_t v) {
    if (v > 0x7FFFFFFFLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    MAC[0] = (int32_t)v;
}

static int16_t sat_ir(int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { FLAG |= 1u << (25 - i); return (int16_t)lo; }
    if (v > 0x7FFF) { FLAG |= 1u << (25 - i); return 0x7FFF; }
    return (int16_t)v;
}

static void set_ir(int i, int32_t v, int lm) { IR[i] = sat_ir(i, v, lm); }

static void mac_to_ir(int lm) {
    set_ir(1, MAC[1], lm);
    set_ir(2, MAC[2], lm);
    set_ir(3, MAC[3], lm);
}

static void set_ir0(int64_t v) {
    if (v < 0) { FLAG |= 1u << 12; v = 0; }
    else if (v > 0x1000) { FLAG |= 1u << 12; v = 0x1000; }
    IR[0] = (int16_t)v;
}

static uint8_t sat_col(int i, int32_t v) {
    if (v < 0) { FLAG |= 1u << (21 - i); return 0; }
    if (v > 255) { FLAG |= 1u << (21 - i); return 255; }
    return (uint8_t)v;
}

static void push_rgb(void) {
    memcpy(RGB[0], RGB[1], 4);
    memcpy(RGB[1], RGB[2], 4);
    RGB[2][0] = sat_col(1, MAC[1] >> 4);
    RGB[2][1] = sat_col(2, MAC[2] >> 4);
    RGB[2][2] = sat_col(3, MAC[3] >> 4);
    RGB[2][3] = RGBC[3];
}

static void push_sz(int64_t v) {
    if (v < 0) { FLAG |= 1u << 18; v = 0; }
    else if (v > 0xFFFF) { FLAG |= 1u << 18; v = 0xFFFF; }
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
    SZ[3] = (uint16_t)v;
}

static bool push_sxy(int64_t x, int64_t y) {
    bool clamped = false;
    if (x < -0x400) { FLAG |= 1u << 14; x = -0x400; clamped = true; } else if (x > 0x3FF) { FLAG |= 1u << 14; x = 0x3FF; clamped = true; }
    if (y < -0x400) { FLAG |= 1u << 13; y = -0x400; clamped = true; } else if (y > 0x3FF) { FLAG |= 1u << 13; y = 0x3FF; clamped = true; }
    SXY[0][0] = SXY[1][0]; SXY[0][1] = SXY[1][1];
    SXY[1][0] = SXY[2][0]; SXY[1][1] = SXY[2][1];
    SXY[2][0] = (int16_t)x; SXY[2][1] = (int16_t)y;
    memcpy(pgxp_sxy[0], pgxp_sxy[1], sizeof pgxp_sxy[0]);
    memcpy(pgxp_sxy[1], pgxp_sxy[2], sizeof pgxp_sxy[0]);
    pgxp_sxy_valid[0] = pgxp_sxy_valid[1];
    pgxp_sxy_valid[1] = pgxp_sxy_valid[2];
    pgxp_sxy_valid[2] = false;
    pgxp_sxy_key[0] = pgxp_sxy_key[1];
    pgxp_sxy_key[1] = pgxp_sxy_key[2];
    pgxp_sxy_key[2] = 0;
    sxy_clip[0] = sxy_clip[1];
    sxy_clip[1] = sxy_clip[2];
    sxy_clip[2] = false;
    return !clamped;
}

static uint32_t gte_divide(void) {
    uint32_t sz3 = SZ[3];
    if (H < sz3 * 2) {
        int z = clz32((uint32_t)sz3 | 1) - 16;
        uint32_t n = (uint32_t)H << z;
        uint32_t d = sz3 << z;
        uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
    }
    FLAG |= 1u << 17;
    return 0x1FFFF;
}

static void rtp(int v, bool last) {
    int64_t x = (int64_t)TR[0] * 0x1000 + (int64_t)RT[0][0] * V[v][0] + (int64_t)RT[0][1] * V[v][1] + (int64_t)RT[0][2] * V[v][2];
    int64_t y = (int64_t)TR[1] * 0x1000 + (int64_t)RT[1][0] * V[v][0] + (int64_t)RT[1][1] * V[v][1] + (int64_t)RT[1][2] * V[v][2];
    int64_t z = (int64_t)TR[2] * 0x1000 + (int64_t)RT[2][0] * V[v][0] + (int64_t)RT[2][1] * V[v][1] + (int64_t)RT[2][2] * V[v][2];
    set_mac(1, x); set_mac(2, y); set_mac(3, z);
    set_ir(1, MAC[1], lm_flag);
    set_ir(2, MAC[2], lm_flag);
    // IR3 saturation flag is computed from the unshifted value (hardware quirk)
    {
        int32_t m3 = MAC[3];
        int32_t zs = (int32_t)(z >> 12);
        if (zs < -0x8000 || zs > 0x7FFF) FLAG |= 1u << 22;
        int32_t lo = lm_flag ? 0 : -0x8000;
        IR[3] = (int16_t)(m3 < lo ? lo : (m3 > 0x7FFF ? 0x7FFF : m3));
    }
    push_sz(z >> 12);
    bool div_ok = H < (uint32_t)SZ[3] * 2;
    uint32_t div = gte_divide();
    int64_t sx = (int64_t)div * IR[1] + OFX;
    int64_t sy = (int64_t)div * IR[2] + OFY;
    if (widescreen_active) sx = OFX + (sx - OFX) * 3 / 4;
    set_mac0(sx); set_mac0(sy);
    bool ok = push_sxy(sx >> 16, sy >> 16);
    // Precise projection for the high-resolution renderer. Where the hardware result is
    // unusable (saturated somewhere along the way, or the vertex too close to or behind the
    // camera: the near edge of the road), the precise one is kept all the same, as a homogeneous
    // position whose depth may be negative, and flagged: the renderer clips such polygons at the
    // camera plane instead of drawing them from the clamped coordinates, or not at all.
    int64_t m1 = x >> 12, m2 = y >> 12;
    if (sf_shift == 12) {
        double zf = (double)z / 4096.0;
        if (zf < 1.0 / 16 && zf > -1.0 / 16) zf = zf < 0 ? -1.0 / 16 : 1.0 / 16;
        double px = (double)x / 4096.0 * H / zf, py = (double)y / 4096.0 * H / zf;
        if (widescreen_active) px *= 0.75;
        pgxp_sxy[2][0] = (float)(OFX / 65536.0 + px);
        pgxp_sxy[2][1] = (float)(OFY / 65536.0 + py);
        pgxp_sxy[2][2] = (float)zf;
        pgxp_sxy_valid[2] = true;
        pgxp_sxy_key[2] = pgxp_vertex_key(v, V[v][0], V[v][1], V[v][2]);
        sxy_clip[2] = !(ok && div_ok && m1 >= -0x8000 && m1 <= 0x7FFF && m2 >= -0x8000 && m2 <= 0x7FFF && z > 0);
    }
    if (last) {
        int64_t d = (int64_t)div * DQA + DQB;
        set_mac0(d);
        set_ir0(d >> 12);
    }
}

static void mvmva_mul(int16_t m[3][3], const int16_t *vec, const int32_t *tr) {
    for (int i = 0; i < 3; i++) {
        int64_t t = tr ? (int64_t)tr[i] * 0x1000 : 0;
        t = mac_check(i + 1, t + (int64_t)m[i][0] * vec[0]);
        t = mac_check(i + 1, t + (int64_t)m[i][1] * vec[1]);
        t = mac_check(i + 1, t + (int64_t)m[i][2] * vec[2]);
        MAC[i + 1] = (int32_t)(t >> sf_shift);
    }
}

// colour helpers shared by NC*/CC/CDP/DPC* ----------------------------------
static void light_color(const int16_t *n) {
    int16_t ir[3];
    mvmva_mul(LLM, n, NULL);
    mac_to_ir(lm_flag);
    ir[0] = IR[1]; ir[1] = IR[2]; ir[2] = IR[3];
    mvmva_mul(LCM, ir, BK);
    mac_to_ir(lm_flag);
}

// MAC = m; IR = (FC<<12 - MAC) >> sf; MAC = (IR * IR0 + m) >> sf. The inputs are at
// MAC precision: RGB<<16 (DPCS/DPCT), IR<<12 (INTPL) or (RGB*IR)<<4 (NCDS/CDP/DCPL).
static void depth_cue(int64_t m1, int64_t m2, int64_t m3) {
    int64_t in[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++) {
        int64_t d = mac_check(i + 1, ((int64_t)FC[i] << 12) - in[i]);
        int16_t t = sat_ir(i + 1, (int32_t)(d >> sf_shift), 0);
        set_mac(i + 1, mac_check(i + 1, (int64_t)t * IR[0] + in[i]));
    }
}

static void finish_color(void) {
    mac_to_ir(lm_flag);
    push_rgb();
}

static void cmd_ncd(int v) {
    light_color(V[v]);
    depth_cue((int64_t)(RGBC[0] * IR[1]) << 4, (int64_t)(RGBC[1] * IR[2]) << 4, (int64_t)(RGBC[2] * IR[3]) << 4);
    finish_color();
}

static void cmd_nc(int v) {
    light_color(V[v]);
    finish_color();
}

static void cmd_ncc(int v) {
    light_color(V[v]);
    set_mac(1, ((int64_t)RGBC[0] * IR[1]) << 4);
    set_mac(2, ((int64_t)RGBC[1] * IR[2]) << 4);
    set_mac(3, ((int64_t)RGBC[2] * IR[3]) << 4);
    finish_color();
}

#ifdef RECOMP_TRACE
extern uint32_t trace_stack[1024];
extern int trace_depth;
static struct { uint32_t fn, n; } rtp_prof[64];
void gte_profile_dump(void) {
    for (int i = 0; i < 64; i++) if (rtp_prof[i].n) LOG("[gte] %08x: %u projections\n", rtp_prof[i].fn, rtp_prof[i].n);
}
#endif

void gte_command(uint32_t op) {
    if (dump_gte_from >= 0 && cpu.cycles / CYCLES_PER_FRAME >= (uint64_t)dump_gte_from) {
        static uint64_t seen;
        if (!(seen & (1ull << (op & 0x3F)))) {
            seen |= 1ull << (op & 0x3F);
            LOG("[gte] cmd %02x op=%07x IR0=%d IR=%d,%d,%d FC=%d,%d,%d DQA=%d DQB=%d H=%u RGBC=%02x%02x%02x MAC=%d,%d,%d\n",
                op & 0x3F, op, IR[0], IR[1], IR[2], IR[3], FC[0], FC[1], FC[2], DQA, DQB, H, RGBC[0], RGBC[1], RGBC[2],
                MAC[1], MAC[2], MAC[3]);
        }
    }
    FLAG = 0;
#ifdef RECOMP_TRACE
    if (log_level >= 3 && ((op & 0x3F) == 0x01 || (op & 0x3F) == 0x30) && trace_depth > 0 && trace_depth <= 1024) {
        uint32_t fn = trace_stack[trace_depth - 1];
        for (int i = 0; i < 64; i++) {
            if (rtp_prof[i].fn == fn || !rtp_prof[i].n) { rtp_prof[i].fn = fn; rtp_prof[i].n += (op & 0x3F) == 0x30 ? 3 : 1; break; }
        }
    }
#endif
    sf_shift = (op & (1u << 19)) ? 12 : 0;
    lm_flag = (op >> 10) & 1;
    switch (op & 0x3F) {
    case 0x01: rtp(0, true); gte_projections++; break;   // RTPS
    case 0x30: rtp(0, false); rtp(1, false); rtp(2, true); gte_projections += 3; break;   // RTPT
    case 0x06: {   // NCLIP
        int64_t v = (int64_t)SXY[0][0] * SXY[1][1] + (int64_t)SXY[1][0] * SXY[2][1] + (int64_t)SXY[2][0] * SXY[0][1]
                  - (int64_t)SXY[0][0] * SXY[2][1] - (int64_t)SXY[1][0] * SXY[0][1] - (int64_t)SXY[2][0] * SXY[1][1];
        // With a vertex too near or behind the camera the clamped coordinates can face either
        // way; the facing of the real triangle decides (the sign of the homogeneous determinant:
        // the screen-space winding times the signs of the depths). Only the sign changes.
        if (render_cfg.hw && pgxp_enabled && (sxy_clip[0] || sxy_clip[1] || sxy_clip[2]) &&
            pgxp_sxy_valid[0] && pgxp_sxy_valid[1] && pgxp_sxy_valid[2]) {
            const float (*p)[3] = pgxp_sxy;
            double c = ((double)p[1][0] - p[0][0]) * ((double)p[2][1] - p[0][1]) - ((double)p[2][0] - p[0][0]) * ((double)p[1][1] - p[0][1]);
            if (p[0][2] < 0) c = -c;
            if (p[1][2] < 0) c = -c;
            if (p[2][2] < 0) c = -c;
            int64_t m = v < 0 ? -v : v;
            if (m == 0) m = 1;
            v = c > 0 ? m : c < 0 ? -m : 0;
        }
        set_mac0(v);
        break;
    }
    case 0x0C: {   // OP (outer product)
        int32_t d1 = RT[0][0], d2 = RT[1][1], d3 = RT[2][2];
        set_mac(1, (int64_t)IR[3] * d2 - (int64_t)IR[2] * d3);
        set_mac(2, (int64_t)IR[1] * d3 - (int64_t)IR[3] * d1);
        set_mac(3, (int64_t)IR[2] * d1 - (int64_t)IR[1] * d2);
        mac_to_ir(lm_flag);
        break;
    }
    case 0x10: {   // DPCS
        depth_cue((int64_t)RGBC[0] << 16, (int64_t)RGBC[1] << 16, (int64_t)RGBC[2] << 16);
        finish_color();
        break;
    }
    case 0x11: {   // INTPL
        depth_cue((int64_t)IR[1] << 12, (int64_t)IR[2] << 12, (int64_t)IR[3] << 12);
        finish_color();
        break;
    }
    case 0x12: {   // MVMVA
        int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
        int16_t garbage[3][3] = {{(int16_t)-(RGBC[0] << 4), (int16_t)(RGBC[0] << 4), IR[0]},
                                 {RT[0][2], RT[0][2], RT[0][2]}, {RT[1][1], RT[1][1], RT[1][1]}};
        int16_t (*m)[3] = mx == 0 ? RT : mx == 1 ? LLM : mx == 2 ? LCM : garbage;
        int16_t vec[3];
        if (vv == 3) { vec[0] = IR[1]; vec[1] = IR[2]; vec[2] = IR[3]; }
        else { vec[0] = V[vv][0]; vec[1] = V[vv][1]; vec[2] = V[vv][2]; }
        const int32_t *tr = cv == 0 ? TR : cv == 1 ? BK : cv == 2 ? FC : NULL;
        if (cv == 2) {
            // FC translation is buggy on hardware: only the last two products are kept
            for (int i = 0; i < 3; i++) {
                int64_t t = (int64_t)tr[i] * 0x1000 + (int64_t)m[i][0] * vec[0];
                sat_ir(i + 1, (int32_t)(mac_check(i + 1, t) >> sf_shift), 0);
                t = mac_check(i + 1, (int64_t)m[i][1] * vec[1]);
                t = mac_check(i + 1, t + (int64_t)m[i][2] * vec[2]);
                MAC[i + 1] = (int32_t)(t >> sf_shift);
            }
        } else {
            mvmva_mul(m, vec, tr);
        }
        mac_to_ir(lm_flag);
        break;
    }
    case 0x13: cmd_ncd(0); break;   // NCDS
    case 0x16: cmd_ncd(0); cmd_ncd(1); cmd_ncd(2); break;   // NCDT
    case 0x14: {   // CDP
        int16_t ir[3] = {IR[1], IR[2], IR[3]};
        mvmva_mul(LCM, ir, BK);
        mac_to_ir(lm_flag);
        depth_cue((int64_t)(RGBC[0] * IR[1]) << 4, (int64_t)(RGBC[1] * IR[2]) << 4, (int64_t)(RGBC[2] * IR[3]) << 4);
        finish_color();
        break;
    }
    case 0x1B: cmd_ncc(0); break;   // NCCS
    case 0x3F: {   // NCCT
        static int dumped;
        if (dump_gte_from >= 0 && !dumped && cpu.cycles / CYCLES_PER_FRAME >= (uint64_t)dump_gte_from) {
            dumped = 1;
            LOG("[gte] NCCT op=%07x LLM=%d,%d,%d/%d,%d,%d/%d,%d,%d LCM=%d,%d,%d/%d,%d,%d/%d,%d,%d BK=%d,%d,%d RGBC=%02x%02x%02x V0=%d,%d,%d\n",
                op, LLM[0][0], LLM[0][1], LLM[0][2], LLM[1][0], LLM[1][1], LLM[1][2], LLM[2][0], LLM[2][1], LLM[2][2],
                LCM[0][0], LCM[0][1], LCM[0][2], LCM[1][0], LCM[1][1], LCM[1][2], LCM[2][0], LCM[2][1], LCM[2][2],
                BK[0], BK[1], BK[2], RGBC[0], RGBC[1], RGBC[2], V[0][0], V[0][1], V[0][2]);
        }
        cmd_ncc(0); cmd_ncc(1); cmd_ncc(2);
        break;
    }
    case 0x1C: {   // CC
        int16_t ir[3] = {IR[1], IR[2], IR[3]};
        mvmva_mul(LCM, ir, BK);
        mac_to_ir(lm_flag);
        set_mac(1, ((int64_t)RGBC[0] * IR[1]) << 4);
        set_mac(2, ((int64_t)RGBC[1] * IR[2]) << 4);
        set_mac(3, ((int64_t)RGBC[2] * IR[3]) << 4);
        finish_color();
        break;
    }
    case 0x1E: cmd_nc(0); break;   // NCS
    case 0x20: cmd_nc(0); cmd_nc(1); cmd_nc(2); break;   // NCT
    case 0x28: {   // SQR
        set_mac(1, (int64_t)IR[1] * IR[1]);
        set_mac(2, (int64_t)IR[2] * IR[2]);
        set_mac(3, (int64_t)IR[3] * IR[3]);
        mac_to_ir(lm_flag);
        break;
    }
    case 0x29: {   // DCPL
        depth_cue((int64_t)(RGBC[0] * IR[1]) << 4, (int64_t)(RGBC[1] * IR[2]) << 4, (int64_t)(RGBC[2] * IR[3]) << 4);
        finish_color();
        break;
    }
    case 0x2A: {   // DPCT
        for (int k = 0; k < 3; k++) {
            depth_cue((int64_t)RGB[0][0] << 16, (int64_t)RGB[0][1] << 16, (int64_t)RGB[0][2] << 16);
            finish_color();
        }
        break;
    }
    case 0x2D: {   // AVSZ3
        int64_t v = (int64_t)ZSF3 * (SZ[1] + SZ[2] + SZ[3]);
        set_mac0(v);
        int64_t o = v >> 12;
        if (o < 0) { FLAG |= 1u << 18; o = 0; } else if (o > 0xFFFF) { FLAG |= 1u << 18; o = 0xFFFF; }
        OTZ = (uint16_t)o;
        break;
    }
    case 0x2E: {   // AVSZ4
        int64_t v = (int64_t)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]);
        set_mac0(v);
        int64_t o = v >> 12;
        if (o < 0) { FLAG |= 1u << 18; o = 0; } else if (o > 0xFFFF) { FLAG |= 1u << 18; o = 0xFFFF; }
        OTZ = (uint16_t)o;
        break;
    }
    case 0x3D: {   // GPF
        set_mac(1, (int64_t)IR[0] * IR[1]);
        set_mac(2, (int64_t)IR[0] * IR[2]);
        set_mac(3, (int64_t)IR[0] * IR[3]);
        finish_color();
        break;
    }
    case 0x3E: {   // GPL
        set_mac(1, ((int64_t)MAC[1] << sf_shift) + (int64_t)IR[0] * IR[1]);
        set_mac(2, ((int64_t)MAC[2] << sf_shift) + (int64_t)IR[0] * IR[2]);
        set_mac(3, ((int64_t)MAC[3] << sf_shift) + (int64_t)IR[0] * IR[3]);
        finish_color();
        break;
    }
    default:
        DBG(1, "GTE: unknown command %07x\n", op);
        break;
    }
    if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
}
