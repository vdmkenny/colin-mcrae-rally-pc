// CPU state, event scheduler, interrupt controller and function dispatch.
#include "psx.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>

CPU cpu;
uint8_t psx_ram[RAM_SIZE];
uint8_t psx_scratch[0x400];
uint32_t cycles_per_insn = 2;
int log_level = 0;

// ------------------------------------------------------------------ scheduler
static struct { uint64_t when; EventFn fn; bool active; } events[EV_COUNT];

static void sched_recalc(void) {
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < EV_COUNT; i++)
        if (events[i].active && events[i].when < next) next = events[i].when;
    if ((i_stat & i_mask) && (cpu.sr & 0x401) == 0x401) next = 0;
    cpu.next_event = next;
}

void sched_init(void) { memset(events, 0, sizeof events); sched_recalc(); }

void sched_at(int ev, uint64_t when, EventFn fn) {
    events[ev].when = when;
    events[ev].fn = fn;
    events[ev].active = true;
    if (when < cpu.next_event) cpu.next_event = when;
}

void sched_cancel(int ev) { events[ev].active = false; sched_recalc(); }
bool sched_pending(int ev) { return events[ev].active; }

// ----------------------------------------------------------------- interrupts
uint32_t i_stat, i_mask;

void irq_raise(int n) {
    i_stat |= 1u << n;
    irq_update();
}

void irq_update(void) {
    if ((i_stat & i_mask) && (cpu.sr & 0x401) == 0x401) cpu.next_event = 0;
}

static int in_events;

void psx_events(CPU *c) {
    if (in_events) return;
    in_events = 1;
    for (;;) {
        int best = -1;
        for (int i = 0; i < EV_COUNT; i++)
            if (events[i].active && events[i].when <= c->cycles &&
                (best < 0 || events[i].when < events[best].when))
                best = i;
        if (best < 0) break;
        events[best].active = false;
        events[best].fn();
    }
    in_events = 0;
    sched_recalc();
    if ((i_stat & i_mask) && (c->sr & 0x401) == 0x401) {
        bios_exception(c);
        sched_recalc();
    }
}

// ---------------------------------------------------------------------- COP0
uint32_t cop0_read(CPU *c, int reg) {
    switch (reg) {
    case 12: return c->sr;
    case 13: return c->cause | ((i_stat & i_mask) ? 0x400 : 0);
    case 14: return c->epc;
    case 15: return 0x00000002;  // PRId
    default: return 0;
    }
}

void cop0_write(CPU *c, int reg, uint32_t v) {
    if (reg == 12) { c->sr = v; irq_update(); }
    else if (reg == 13) c->cause = (c->cause & ~0x300u) | (v & 0x300u);
}

void cop0_rfe(CPU *c) {
    c->sr = (c->sr & ~0xFu) | ((c->sr >> 2) & 0xFu);
    irq_update();
}

void psx_syscall(CPU *c, uint32_t pc) {
    // BIOS syscall(n): a0 selects the function.
    switch (c->r[4]) {
    case 1:  // EnterCriticalSection
        c->r[2] = (c->sr & 0x404) == 0x404;
        c->sr &= ~0x404u;
        break;
    case 2:  // ExitCriticalSection
        c->sr |= 0x404;
        irq_update();
        break;
    case 0:
        break;
    default:
        DBG(1, "syscall(%u) at %08x\n", c->r[4], pc);
        break;
    }
}

void psx_break(CPU *c, uint32_t pc) {
    (void)c;
    LOG("break at %08x\n", pc);
}

void psx_bad_insn(CPU *c, uint32_t pc, uint32_t word) {
    host_fatal("invalid instruction %08x at %08x (ra=%08x)", word, pc, c->r[31]);
}

// ------------------------------------------------------------------ modules
static const Module *active;
static RecompFn *table;       // direct-mapped: (addr - base) / 4
static RecompFn *tables[8];

static bool module_loaded(const Module *m) {
    for (int i = 0; i < 4; i++) {
        uint32_t w;
        memcpy(&w, psx_ram + ((m->entry & RAM_MASK) + i * 4), 4);
        if (w != m->sig[i]) return false;
    }
    return true;
}

static RecompFn *module_table(unsigned idx) {
    if (!tables[idx]) {
        const Module *m = all_modules[idx];
        tables[idx] = calloc((m->end - m->base) / 4, sizeof(RecompFn));
        for (unsigned i = 0; i < m->nfuncs; i++)
            tables[idx][(m->funcs[i].addr - m->base) >> 2] = m->funcs[i].fn;
    }
    return tables[idx];
}

const Module *module_active(void) { return active; }

bool module_activate(uint32_t entry) {
    for (unsigned i = 0; i < num_modules; i++) {
        const Module *m = all_modules[i];
        if (m->entry == entry && module_loaded(m)) {
            active = m;
            table = module_table(i);
            LOG("[exec] %s (entry %08x)\n", m->name, entry);
            return true;
        }
    }
    return false;
}

static RecompFn lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFF) | 0x80000000;
    if (active && addr >= active->base && addr < active->end) {
        RecompFn f = table[(addr - active->base) >> 2];
        if (f) return f;
    }
    // Code from another module that is still resident in RAM.
    for (unsigned i = 0; i < num_modules; i++) {
        const Module *m = all_modules[i];
        if (m == active || addr < m->base || addr >= m->end || !module_loaded(m)) continue;
        RecompFn f = module_table(i)[(addr - m->base) >> 2];
        if (f) return f;
    }
    return NULL;
}

void dispatch_call(CPU *c, uint32_t addr) {
    uint32_t a = addr & 0x1FFFFFFF;
    if (a == 0xA0 || a == 0xB0 || a == 0xC0) {
        bios_call(c, a, c->r[9]);
        return;
    }
    if (a < 0x10000) {   // kernel RAM: code the real BIOS would have here does not exist
        DBG(1, "[cpu] call into kernel RAM %08x ignored (ra=%08x)\n", addr, c->r[31]);
        return;
    }
    RecompFn f = lookup(addr);
    if (!f) host_fatal("no recompiled function at %08x (ra=%08x, module %s)", addr, c->r[31],
                       active ? active->name : "-");
    f(c);
}

void dispatch_jump(CPU *c, uint32_t addr) { dispatch_call(c, addr); }

// ---------------------------------------------------------------- execution
#ifdef RECOMP_TRACE
extern int trace_depth;
#endif
static jmp_buf exec_jb;
static uint32_t exec_entry, exec_sp, exec_gp, exec_a0, exec_a1;

void psx_exec(uint32_t entry, uint32_t sp, uint32_t gp, uint32_t a0, uint32_t a1) {
    exec_entry = entry; exec_sp = sp; exec_gp = gp; exec_a0 = a0; exec_a1 = a1;
    longjmp(exec_jb, 1);
}

void psx_start(uint32_t entry, uint32_t sp, uint32_t gp) {
    exec_entry = entry; exec_sp = sp; exec_gp = gp; exec_a0 = 1; exec_a1 = 0;
    setjmp(exec_jb);
#ifdef RECOMP_TRACE
    trace_depth = 0;   // the host stack was unwound by the switch to a new executable
#endif
    if (!module_activate(exec_entry))
        host_fatal("no recompiled module matches the executable at %08x", exec_entry);
    CPU *c = &cpu;
    memset(c->r, 0, sizeof c->r);
    R(29) = R(30) = exec_sp;
    R(28) = exec_gp;
    R(4) = exec_a0;
    R(5) = exec_a1;
    R(31) = 0;
    dispatch_call(c, exec_entry);
    host_fatal("executable at %08x returned", exec_entry);
}

#ifdef RECOMP_TRACE
uint32_t trace_ring[256];
unsigned trace_pos;
uint32_t trace_stack[1024];
int trace_depth;
#endif

void trace_dump(void) {
#ifdef RECOMP_TRACE
    fprintf(stderr, "  recent functions (oldest first):");
    for (unsigned i = 0; i < 64; i++) {
        uint32_t a = trace_ring[(trace_pos - 64 + i) & 255];
        fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", a);
    }
    fprintf(stderr, "\n  call stack (outermost first):");
    for (int i = 0; i < trace_depth && i < 1024; i++) fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", trace_stack[i]);
    fprintf(stderr, "\n");
#endif
}

void host_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fprintf(stderr, "  regs:");
    for (int i = 0; i < 32; i++) fprintf(stderr, "%s%08x", (i % 8) ? " " : "\n   ", cpu.r[i]);
    fprintf(stderr, "\n");
    trace_dump();
    exit(1);
}
