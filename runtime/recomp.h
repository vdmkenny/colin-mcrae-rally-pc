// Interface between recompiled game code and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t sr, cause, epc;   // COP0
    uint64_t cycles;           // elapsed CPU cycles
    uint64_t next_event;       // cycle at which psx_events() must run
} CPU;

typedef void (*RecompFn)(CPU *c);
typedef struct { uint32_t addr; RecompFn fn; } FuncEntry;
typedef struct {
    const char *name;
    uint32_t base, end, entry;
    uint32_t sig[4];           // first words at the entry point, identifies the loaded exe
    const FuncEntry *funcs;
    unsigned nfuncs;
} Module;

extern CPU cpu;
extern uint8_t psx_ram[0x200000];
extern uint8_t psx_scratch[0x400];
extern uint32_t cycles_per_insn;

#ifdef RECOMP_TRACE
extern uint32_t trace_ring[256];
extern unsigned trace_pos;
extern uint32_t trace_stack[1024];
extern int trace_depth;
static inline int trace_push(uint32_t a) {
    trace_ring[trace_pos++ & 255] = a;
    if (trace_depth >= 0 && trace_depth < 1024) trace_stack[trace_depth] = a;
    return trace_depth++;
}
static inline void trace_pop(int *d) { trace_depth = *d; }
#define FN_ENTER(a) int trace_frame __attribute__((cleanup(trace_pop), unused)) = trace_push(a)
#else
#define FN_ENTER(a) ((void)0)
#endif

#define R(n) c->r[n]
#define CYC(n) (c->cycles += (uint64_t)(n) * cycles_per_insn)
#define CHECK_EVENTS() do { if (c->cycles >= c->next_event) psx_events(c); } while (0)

void psx_events(CPU *c);
void dispatch_call(CPU *c, uint32_t addr);
void dispatch_jump(CPU *c, uint32_t addr);
void bios_call(CPU *c, uint32_t table, uint32_t fn);
void psx_syscall(CPU *c, uint32_t pc);
void psx_break(CPU *c, uint32_t pc);
void psx_bad_insn(CPU *c, uint32_t pc, uint32_t word);
uint32_t cop0_read(CPU *c, int reg);
void cop0_write(CPU *c, int reg, uint32_t v);
void cop0_rfe(CPU *c);

uint32_t gte_read_data(int reg);
uint32_t gte_read_ctrl(int reg);
void gte_write_data(int reg, uint32_t v);
void gte_write_ctrl(int reg, uint32_t v);
void gte_command(uint32_t op);

void pgxp_swc2(uint32_t addr, int reg);
void pgxp_mfc2(int rt, int reg, uint32_t value);
void pgxp_sw(uint32_t addr, int rt, uint32_t value);

uint32_t mem_read_slow(uint32_t addr, int size);
void mem_write_slow(uint32_t addr, uint32_t v, int size);

// Memory access: RAM and scratchpad inline, everything else through the slow path.
static inline uint32_t RD32(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) { uint32_t v; memcpy(&v, psx_ram + (p & 0x1FFFFC), 4); return v; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { uint32_t v; memcpy(&v, psx_scratch + (p & 0x3FC), 4); return v; }
    return mem_read_slow(a, 4);
}
static inline uint32_t RD16(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) { uint16_t v; memcpy(&v, psx_ram + (p & 0x1FFFFE), 2); return v; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { uint16_t v; memcpy(&v, psx_scratch + (p & 0x3FE), 2); return v; }
    return mem_read_slow(a, 2);
}
static inline uint32_t RD8(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return psx_ram[p & 0x1FFFFF];
    if ((p & 0xFFFFFC00) == 0x1F800000) return psx_scratch[p & 0x3FF];
    return mem_read_slow(a, 1);
}
static inline void WR32(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) { memcpy(psx_ram + (p & 0x1FFFFC), &v, 4); return; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(psx_scratch + (p & 0x3FC), &v, 4); return; }
    mem_write_slow(a, v, 4);
}
static inline void WR16(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF;
    uint16_t h = (uint16_t)v;
    if (p < 0x00800000) { memcpy(psx_ram + (p & 0x1FFFFE), &h, 2); return; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { memcpy(psx_scratch + (p & 0x3FE), &h, 2); return; }
    mem_write_slow(a, v & 0xFFFF, 2);
}
static inline void WR8(uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) { psx_ram[p & 0x1FFFFF] = (uint8_t)v; return; }
    if ((p & 0xFFFFFC00) == 0x1F800000) { psx_scratch[p & 0x3FF] = (uint8_t)v; return; }
    mem_write_slow(a, v & 0xFF, 1);
}

// Unaligned load/store halves (little-endian R3000 semantics).
static inline uint32_t op_lwl(uint32_t a, uint32_t rt) {
    uint32_t w = RD32(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFF) | (w << 24);
    case 1: return (rt & 0x0000FFFF) | (w << 16);
    case 2: return (rt & 0x000000FF) | (w << 8);
    default: return w;
    }
}
static inline uint32_t op_lwr(uint32_t a, uint32_t rt) {
    uint32_t w = RD32(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000) | (w >> 8);
    case 2: return (rt & 0xFFFF0000) | (w >> 16);
    default: return (rt & 0xFFFFFF00) | (w >> 24);
    }
}
static inline void op_swl(uint32_t a, uint32_t rt) {
    uint32_t w = RD32(a & ~3u);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000) | (rt >> 8); break;
    default: w = rt; break;
    }
    WR32(a & ~3u, w);
}
static inline void op_swr(uint32_t a, uint32_t rt) {
    uint32_t w = RD32(a & ~3u);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FF) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFF) | (rt << 16); break;
    default: w = (w & 0x00FFFFFF) | (rt << 24); break;
    }
    WR32(a & ~3u, w);
}

static inline void op_div(CPU *c, uint32_t n, uint32_t d) {
    int32_t sn = (int32_t)n, sd = (int32_t)d;
    if (sd == 0) { c->hi = n; c->lo = sn >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (n == 0x80000000u && sd == -1) { c->lo = n; c->hi = 0; }
    else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
static inline void op_divu(CPU *c, uint32_t n, uint32_t d) {
    if (d == 0) { c->hi = n; c->lo = 0xFFFFFFFFu; }
    else { c->lo = n / d; c->hi = n % d; }
}
