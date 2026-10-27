// Sound Processing Unit: 24 ADPCM voices, ADSR, reverb, CD audio input.
#include "psx.h"

static uint8_t spu_ram[0x80000];
static uint16_t regs[0x200];    // raw register file, 0x1F801C00..0x1F801FFF

typedef enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_OFF } EnvPhase;

typedef struct {
    uint32_t addr;          // current block address (bytes)
    uint32_t repeat;        // repeat address (bytes)
    uint32_t counter;       // pitch counter, 12-bit fraction
    int16_t decoded[28];
    int16_t hist[4];        // last 4 output samples for interpolation
    int32_t prev[2];        // ADPCM filter history
    int block_pos;          // sample index inside decoded block
    bool repeat_set;        // repeat address written by software after KON
    EnvPhase phase;
    int32_t env;            // 0..0x7FFF
    int32_t env_wait;
    int16_t last_out;       // for pitch modulation of the next voice
} Voice;

static Voice voices[24];
static uint32_t endx;
static uint32_t transfer_addr;
static uint64_t spu_cycles;     // cycles already rendered
static int16_t out_buf[8192 * 2];
static int out_len;
static uint32_t reverb_addr;
static int32_t reverb_out_l, reverb_out_r;
static int32_t reverb_in_l, reverb_in_r;
static bool reverb_phase;
static uint32_t noise_level = 1;
static int32_t noise_count;

#define R16(off) regs[((off) - 0x1C00) >> 1]
#define SPUCNT R16(0x1DAA)

static const int ps_k0[5] = {0, 60, 115, 98, 122}, ps_k1[5] = {0, 0, -52, -55, -60};

static inline int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void check_irq(uint32_t addr) {
    if ((SPUCNT & 0x40) && (addr & 0x7FFF8) == ((uint32_t)R16(0x1DA4) << 3)) {
        if (!(R16(0x1DAE) & 0x40)) {
            R16(0x1DAE) |= 0x40;
            irq_raise(IRQ_SPU);
        }
    }
}

static void decode_block(Voice *v) {
    const uint8_t *b = spu_ram + (v->addr & 0x7FFF0);
    check_irq(v->addr);
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    for (int i = 0; i < 28; i++) {
        int32_t t = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (t & 8) t -= 16;
        int32_t s = (t << 12) >> shift;
        s += (v->prev[0] * ps_k0[filt] + v->prev[1] * ps_k1[filt] + 32) >> 6;
        s = clamp16(s);
        v->prev[1] = v->prev[0];
        v->prev[0] = s;
        v->decoded[i] = (int16_t)s;
    }
    uint8_t flags = b[1];
    if ((flags & 4) && !v->repeat_set) v->repeat = v->addr & 0x7FFF0;
}

static void advance_block(Voice *v, int idx) {
    const uint8_t *b = spu_ram + (v->addr & 0x7FFF0);
    uint8_t flags = b[1];
    if (flags & 1) {
        endx |= 1u << idx;
        v->addr = v->repeat;
        if (!(flags & 2)) { v->phase = ENV_RELEASE; v->env = 0; }
    } else {
        v->addr = (v->addr + 16) & 0x7FFF0;
    }
    decode_block(v);
}

// ---------------------------------------------------------------- envelope
static void env_params(Voice *v, int idx, int *shift, int *step, bool *exp, bool *decrease) {
    uint16_t lo = R16(0x1C08 + idx * 16), hi = R16(0x1C0A + idx * 16);
    switch (v->phase) {
    case ENV_ATTACK: *shift = (lo >> 10) & 31; *step = 7 - ((lo >> 8) & 3); *exp = lo & 0x8000; *decrease = false; break;
    case ENV_DECAY: *shift = ((lo >> 4) & 15) << 2; *step = -8; *exp = true; *decrease = true; break;
    case ENV_SUSTAIN:
        *shift = (hi >> 8) & 31;
        *decrease = hi & 0x4000;
        *step = *decrease ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3);
        *exp = hi & 0x8000;
        break;
    default: *shift = (hi & 31) << 2; *step = -8; *exp = hi & 0x20; *decrease = true; break;
    }
}

static void env_tick(Voice *v, int idx) {
    if (v->phase == ENV_OFF) return;
    if (--v->env_wait > 0) return;
    int shift, step; bool exp, dec;
    env_params(v, idx, &shift, &step, &exp, &dec);
    int32_t cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t delta = step << (shift < 11 ? 11 - shift : 0);
    if (exp && !dec && v->env > 0x6000) cycles *= 4;
    if (exp && dec) delta = (delta * v->env) >> 15;
    v->env_wait = cycles;
    v->env += delta;
    if (v->env < 0) v->env = 0;
    if (v->env > 0x7FFF) v->env = 0x7FFF;
    uint16_t lo = R16(0x1C08 + idx * 16);
    switch (v->phase) {
    case ENV_ATTACK: if (v->env >= 0x7FFF) { v->phase = ENV_DECAY; v->env_wait = 0; } break;
    case ENV_DECAY: if (v->env <= (((lo & 15) + 1) * 0x800)) { v->phase = ENV_SUSTAIN; v->env_wait = 0; } break;
    case ENV_RELEASE: if (v->env == 0) v->phase = ENV_OFF; break;
    default: break;
    }
}

// ------------------------------------------------------------------ reverb
static inline uint32_t rv_addr(uint32_t off8) {
    uint32_t base = (uint32_t)R16(0x1DA2) << 3;
    uint32_t a = reverb_addr + off8 * 8;
    uint32_t size = 0x80000 - base;
    if (!size) return 0;
    while (a >= 0x80000) a = base + (a - 0x80000) % size;
    return a & 0x7FFFE;
}
static inline int16_t rv_read(uint32_t off8, int32_t extra) {
    uint32_t a = rv_addr(off8);
    uint32_t base = (uint32_t)R16(0x1DA2) << 3;
    int32_t b = (int32_t)a + extra * 2;
    if (b < (int32_t)base) b += 0x80000 - (int32_t)base;
    if (b >= 0x80000) b = (int32_t)base + (b - 0x80000);
    int16_t v;
    memcpy(&v, spu_ram + (b & 0x7FFFE), 2);
    return v;
}
static inline void rv_write(uint32_t off8, int32_t v) {
    if (!(SPUCNT & 0x80)) return;
    int16_t s = clamp16(v);
    memcpy(spu_ram + rv_addr(off8), &s, 2);
}

static void reverb_step(int32_t in_l, int32_t in_r) {
#define RV(o) ((int32_t)(int16_t)R16(o))
#define RA(o) ((uint32_t)R16(o))
    int32_t vIIR = RV(0x1DC4), vWALL = RV(0x1DCE);
    int32_t Lin = (in_l * RV(0x1DFC)) >> 15, Rin = (in_r * RV(0x1DFE)) >> 15;
    int32_t lsame = (Lin + ((rv_read(RA(0x1DE0), 0) * vWALL) >> 15) - rv_read(RA(0x1DD4), -1)) * vIIR / 32768 + rv_read(RA(0x1DD4), -1);
    int32_t rsame = (Rin + ((rv_read(RA(0x1DE2), 0) * vWALL) >> 15) - rv_read(RA(0x1DD6), -1)) * vIIR / 32768 + rv_read(RA(0x1DD6), -1);
    int32_t ldiff = (Lin + ((rv_read(RA(0x1DF2), 0) * vWALL) >> 15) - rv_read(RA(0x1DE4), -1)) * vIIR / 32768 + rv_read(RA(0x1DE4), -1);
    int32_t rdiff = (Rin + ((rv_read(RA(0x1DF0), 0) * vWALL) >> 15) - rv_read(RA(0x1DE6), -1)) * vIIR / 32768 + rv_read(RA(0x1DE6), -1);
    rv_write(RA(0x1DD4), lsame);
    rv_write(RA(0x1DD6), rsame);
    rv_write(RA(0x1DE4), ldiff);
    rv_write(RA(0x1DE6), rdiff);
    int32_t Lout = (RV(0x1DC6) * rv_read(RA(0x1DD8), 0) + RV(0x1DC8) * rv_read(RA(0x1DDC), 0) +
                    RV(0x1DCA) * rv_read(RA(0x1DE8), 0) + RV(0x1DCC) * rv_read(RA(0x1DEC), 0)) >> 15;
    int32_t Rout = (RV(0x1DC6) * rv_read(RA(0x1DDA), 0) + RV(0x1DC8) * rv_read(RA(0x1DDE), 0) +
                    RV(0x1DCA) * rv_read(RA(0x1DEA), 0) + RV(0x1DCC) * rv_read(RA(0x1DEE), 0)) >> 15;
    int32_t vAPF1 = RV(0x1DD0), vAPF2 = RV(0x1DD2);
    uint32_t dAPF1 = RA(0x1DC0), dAPF2 = RA(0x1DC2);
    int32_t t;
    t = rv_read(RA(0x1DF4) - dAPF1, 0); Lout = clamp16(Lout - ((vAPF1 * t) >> 15)); rv_write(RA(0x1DF4), Lout); Lout = clamp16(((Lout * vAPF1) >> 15) + t);
    t = rv_read(RA(0x1DF6) - dAPF1, 0); Rout = clamp16(Rout - ((vAPF1 * t) >> 15)); rv_write(RA(0x1DF6), Rout); Rout = clamp16(((Rout * vAPF1) >> 15) + t);
    t = rv_read(RA(0x1DF8) - dAPF2, 0); Lout = clamp16(Lout - ((vAPF2 * t) >> 15)); rv_write(RA(0x1DF8), Lout); Lout = clamp16(((Lout * vAPF2) >> 15) + t);
    t = rv_read(RA(0x1DFA) - dAPF2, 0); Rout = clamp16(Rout - ((vAPF2 * t) >> 15)); rv_write(RA(0x1DFA), Rout); Rout = clamp16(((Rout * vAPF2) >> 15) + t);
    reverb_out_l = (Lout * (int16_t)R16(0x1D84)) >> 15;
    reverb_out_r = (Rout * (int16_t)R16(0x1D86)) >> 15;
    uint32_t base = (uint32_t)R16(0x1DA2) << 3;
    reverb_addr = (reverb_addr + 2) & 0x7FFFE;
    if (reverb_addr < base) reverb_addr = base;
#undef RV
#undef RA
}

// ------------------------------------------------------------------- mixing
static int16_t interp(const int16_t *h, uint32_t frac) {
    // 4-point Catmull-Rom between h[1] and h[2]
    int32_t p0 = h[0], p1 = h[1], p2 = h[2], p3 = h[3];
    int32_t t = (int32_t)(frac >> 4);   // 8-bit fraction
    int32_t a = (-p0 + 3 * p1 - 3 * p2 + p3);
    int32_t b = (2 * p0 - 5 * p1 + 4 * p2 - p3);
    int32_t c = (-p0 + p2);
    int64_t v = (((((int64_t)a * t >> 8) + b) * t >> 8) + c) * t >> 9;
    return clamp16(p1 + (int32_t)v);
}

static void render_sample(int16_t *out_l, int16_t *out_r) {
    int32_t sum_l = 0, sum_r = 0, rev_l = 0, rev_r = 0;
    uint32_t pmon = R16(0x1D90) | ((uint32_t)R16(0x1D92) << 16);
    uint32_t non = R16(0x1D94) | ((uint32_t)R16(0x1D96) << 16);
    uint32_t eon = R16(0x1D98) | ((uint32_t)R16(0x1D9A) << 16);

    // noise generator
    {
        int nshift = (SPUCNT >> 10) & 15, nstep = ((SPUCNT >> 8) & 3) + 4;
        noise_count -= nstep;
        uint32_t parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
        if (noise_count < 0) {
            noise_level = ((noise_level << 1) | parity) & 0xFFFF;
            noise_count += 0x20000 >> nshift;
            if (noise_count < 0) noise_count += 0x20000 >> nshift;
        }
    }

    for (int i = 0; i < 24; i++) {
        Voice *v = &voices[i];
        if (v->phase == ENV_OFF) { v->last_out = 0; continue; }
        int32_t pitch = R16(0x1C04 + i * 16);
        if (i > 0 && (pmon & (1u << i))) {
            int32_t f = voices[i - 1].last_out + 0x8000;
            pitch = (pitch * f) >> 15;
        }
        if (pitch > 0x3FFF) pitch = 0x3FFF;
        int16_t s = (non & (1u << i)) ? (int16_t)noise_level : interp(v->hist, v->counter & 0xFFF);
        env_tick(v, i);
        int32_t out = (s * v->env) >> 15;
        v->last_out = (int16_t)out;
        R16(0x1C0C + i * 16) = (uint16_t)v->env;
        int16_t vl = (int16_t)R16(0x1C00 + i * 16), vr = (int16_t)R16(0x1C02 + i * 16);
        int32_t l = (vl & 0x8000) ? out : (out * (int16_t)(vl << 1)) >> 15;
        int32_t r = (vr & 0x8000) ? out : (out * (int16_t)(vr << 1)) >> 15;
        sum_l += l; sum_r += r;
        if (eon & (1u << i)) { rev_l += l; rev_r += r; }
        // advance pitch counter
        v->counter += (uint32_t)pitch;
        while (v->counter >= 0x1000) {
            v->counter -= 0x1000;
            v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
            if (++v->block_pos >= 28) { v->block_pos = 0; advance_block(v, i); }
            v->hist[3] = v->decoded[v->block_pos];
        }
    }

    // CD audio
    int16_t cd[2];
    cdrom_audio(cd, 1);
    if (SPUCNT & 1) {
        int32_t cl = (cd[0] * (int16_t)R16(0x1DB0)) >> 15, cr = (cd[1] * (int16_t)R16(0x1DB2)) >> 15;
        sum_l += cl; sum_r += cr;
        if (SPUCNT & 4) { rev_l += cl; rev_r += cr; }
    }

    // reverb runs at 22.05 kHz
    if (!reverb_phase) { reverb_in_l = rev_l; reverb_in_r = rev_r; }
    else reverb_step((reverb_in_l + rev_l) / 2, (reverb_in_r + rev_r) / 2);
    reverb_phase = !reverb_phase;
    if (SPUCNT & 0x80) { sum_l += reverb_out_l; sum_r += reverb_out_r; }

    int32_t ml = (int16_t)R16(0x1D80), mr = (int16_t)R16(0x1D82);
    ml = (ml & 0x8000) ? 0x3FFF : ml;
    mr = (mr & 0x8000) ? 0x3FFF : mr;
    if (!(SPUCNT & 0x4000)) { sum_l = sum_r = 0; }   // mute
    *out_l = clamp16((clamp16(sum_l) * (ml << 1)) >> 15);
    *out_r = clamp16((clamp16(sum_r) * (mr << 1)) >> 15);
}

void spu_run_to(uint64_t cycles) {
    while (spu_cycles + CYCLES_PER_SAMPLE <= cycles) {
        spu_cycles += CYCLES_PER_SAMPLE;
        int16_t l, r;
        render_sample(&l, &r);
        if (out_len < 8192) {
            out_buf[out_len * 2] = l;
            out_buf[out_len * 2 + 1] = r;
            out_len++;
        }
    }
}

// Takes up to `frames` rendered stereo frames; returns how many were copied.
int spu_take(int16_t *dst, int frames) {
    int n = out_len < frames ? out_len : frames;
    memcpy(dst, out_buf, (size_t)n * 4);
    memmove(out_buf, out_buf + n * 2, (size_t)(out_len - n) * 4);
    out_len -= n;
    return n;
}

void spu_render(int16_t *out, int frames) { (void)out; (void)frames; }

// ---------------------------------------------------------------- registers
static void key_on(int i) {
    Voice *v = &voices[i];
    v->addr = (uint32_t)R16(0x1C06 + i * 16) << 3;
    v->counter = 0;
    v->block_pos = 0;
    v->prev[0] = v->prev[1] = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->phase = ENV_ATTACK;
    v->env = 0;
    v->env_wait = 0;
    v->repeat_set = false;
    endx &= ~(1u << i);
    decode_block(v);
    v->hist[3] = v->decoded[0];
}

static void key_off(int i) {
    if (voices[i].phase != ENV_OFF) { voices[i].phase = ENV_RELEASE; voices[i].env_wait = 0; }
}

uint16_t spu_read(uint32_t addr) {
    uint32_t off = addr & 0x3FE;
    spu_run_to(cpu.cycles);
    if (off < 0x180 && (off & 0xF) == 0xC) return (uint16_t)voices[off >> 4].env;
    switch (off + 0x1C00) {
    case 0x1D9C: return endx & 0xFFFF;
    case 0x1D9E: return endx >> 16;
    case 0x1DAE: return (R16(0x1DAE) & 0x40) | (SPUCNT & 0x3F) | ((SPUCNT & 0x20) ? 0x80 : 0);
    case 0x1DA8: return 0;
    }
    return regs[off >> 1];
}

void spu_write(uint32_t addr, uint16_t v) {
    uint32_t off = addr & 0x3FE;
    spu_run_to(cpu.cycles);
    if (off + 0x1C00 == 0x1DAA || off + 0x1C00 == 0x1DB0 || off + 0x1C00 == 0x1DB2 || off + 0x1C00 == 0x1D80)
        DBG(2, "[spu] %04x = %04x @%.2fs\n", off + 0x1C00, v, (double)cpu.cycles / PSX_CLOCK);
    regs[off >> 1] = v;
    if (off < 0x180) {
        int i = off >> 4;
        if ((off & 0xF) == 0xE) { voices[i].repeat = (uint32_t)v << 3; voices[i].repeat_set = true; }
        return;
    }
    switch (off + 0x1C00) {
    case 0x1D88: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
    case 0x1D8A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
    case 0x1D8C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
    case 0x1D8E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
    case 0x1DA2: reverb_addr = (uint32_t)v << 3; break;
    case 0x1DA6: transfer_addr = (uint32_t)v << 3; break;
    case 0x1DA8:
        check_irq(transfer_addr);
        memcpy(spu_ram + (transfer_addr & 0x7FFFE), &v, 2);
        transfer_addr = (transfer_addr + 2) & 0x7FFFF;
        break;
    case 0x1DAA:
        if (!(v & 0x40)) R16(0x1DAE) &= ~0x40;   // disabling IRQ acknowledges it
        break;
    }
}

void spu_dma_write(uint32_t addr, uint32_t words) {
    spu_run_to(cpu.cycles);
    for (uint32_t i = 0; i < words * 4; i += 2) {
        check_irq(transfer_addr);
        memcpy(spu_ram + (transfer_addr & 0x7FFFE), psx_ram + ((addr + i) & 0x1FFFFE), 2);
        transfer_addr = (transfer_addr + 2) & 0x7FFFF;
    }
}

void spu_dma_read(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words * 4; i += 2) {
        memcpy(psx_ram + ((addr + i) & 0x1FFFFE), spu_ram + (transfer_addr & 0x7FFFE), 2);
        transfer_addr = (transfer_addr + 2) & 0x7FFFF;
    }
}

void spu_init(void) {
    memset(spu_ram, 0, sizeof spu_ram);
    memset(regs, 0, sizeof regs);
    for (int i = 0; i < 24; i++) voices[i].phase = ENV_OFF;
    spu_cycles = 0;
}
