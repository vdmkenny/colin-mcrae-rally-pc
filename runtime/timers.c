// Root counters 0-2, evaluated lazily from the CPU cycle counter.
#include "psx.h"

#define CYCLES_PER_HBLANK 2146u   // 263 lines per NTSC field
#define CYCLES_PER_DOT 5u         // ~320-wide dotclock

typedef struct {
    uint32_t mode, target;
    uint32_t base_value;      // counter value at base_cycle
    uint64_t base_cycle;
    bool irq_fired;           // one-shot mode already fired
} Timer;
static Timer tmr[3];

static uint32_t divider(int n) {
    uint32_t src = (tmr[n].mode >> 8) & 3;
    switch (n) {
    case 0: return (src & 1) ? CYCLES_PER_DOT : 1;
    case 1: return (src & 1) ? CYCLES_PER_HBLANK : 1;
    default: return (src & 2) ? 8 : 1;
    }
}

static uint32_t wrap_at(int n) {
    return (tmr[n].mode & 8) && tmr[n].target ? tmr[n].target : 0xFFFF;
}

static uint32_t current(int n, uint64_t now) {
    uint64_t ticks = (now - tmr[n].base_cycle) / divider(n);
    uint64_t v = tmr[n].base_value + ticks;
    uint32_t w = wrap_at(n);
    if (v > w) {
        // record wrap/target flags for the mode register
        if ((tmr[n].mode & 8) && tmr[n].target) tmr[n].mode |= 1u << 11;
        else tmr[n].mode |= 1u << 12;
        v %= (uint64_t)w + 1;
    } else if (v >= tmr[n].target && tmr[n].target) {
        tmr[n].mode |= 1u << 11;
    }
    return (uint32_t)v;
}

static void rebase(int n) {
    tmr[n].base_value = current(n, cpu.cycles);
    tmr[n].base_cycle = cpu.cycles - (cpu.cycles - tmr[n].base_cycle) % divider(n);
}

static void timer_event(void);

static void schedule(void) {
    uint64_t best = UINT64_MAX;
    for (int n = 0; n < 3; n++) {
        uint32_t m = tmr[n].mode;
        if (!(m & 0x30) || (tmr[n].irq_fired && !(m & 0x40))) continue;
        uint32_t v = current(n, cpu.cycles), w = wrap_at(n), t = tmr[n].target;
        uint64_t dist = UINT64_MAX;
        if ((m & 0x10) && t) dist = t > v ? t - v : (uint64_t)(w + 1 - v) + t;
        if (m & 0x20) {
            uint64_t d = 0xFFFF > v ? 0xFFFFu - v : 0x10000u;
            if (d < dist) dist = d;
        }
        if (dist == UINT64_MAX) continue;
        uint64_t when = cpu.cycles + dist * divider(n);
        if (when < best) best = when;
    }
    if (best != UINT64_MAX) sched_at(EV_TIMER, best, timer_event);
    else sched_cancel(EV_TIMER);
}

static uint32_t last_seen[3];

static void timer_event(void) {
    for (int n = 0; n < 3; n++) {
        uint32_t m = tmr[n].mode;
        if (!(m & 0x30) || (tmr[n].irq_fired && !(m & 0x40))) continue;
        uint32_t v = current(n, cpu.cycles);
        bool hit = false;
        if ((m & 0x10) && tmr[n].target && (v == tmr[n].target || (v < last_seen[n] && (m & 8)) ||
                                            (v >= tmr[n].target && last_seen[n] < tmr[n].target)))
            hit = true;
        if ((m & 0x20) && v < last_seen[n]) hit = true;
        last_seen[n] = v;
        if (hit) {
            tmr[n].irq_fired = true;
            tmr[n].mode &= ~(1u << 10);
            irq_raise(IRQ_TMR0 + n);
        }
    }
    schedule();
}

void timers_init(void) {
    for (int n = 0; n < 3; n++) {
        tmr[n] = (Timer){0};
        tmr[n].mode = 1u << 10;
    }
}

uint32_t timer_read(uint32_t addr) {
    int n = (addr >> 4) & 3;
    if (n > 2) return 0;
    switch (addr & 0xF) {
    case 0: return current(n, cpu.cycles);
    case 4: {
        current(n, cpu.cycles);
        uint32_t m = tmr[n].mode;
        tmr[n].mode &= ~((1u << 11) | (1u << 12));
        tmr[n].mode |= 1u << 10;
        return m;
    }
    case 8: return tmr[n].target;
    }
    return 0;
}

void timer_write(uint32_t addr, uint32_t v) {
    int n = (addr >> 4) & 3;
    if (n > 2) return;
    switch (addr & 0xF) {
    case 0:
        tmr[n].base_value = v & 0xFFFF;
        tmr[n].base_cycle = cpu.cycles;
        break;
    case 4:
        tmr[n].mode = (v & 0x3FF) | (1u << 10);
        tmr[n].base_value = 0;
        tmr[n].base_cycle = cpu.cycles;
        tmr[n].irq_fired = false;
        break;
    case 8:
        rebase(n);
        tmr[n].target = v & 0xFFFF;
        break;
    }
    last_seen[n] = current(n, cpu.cycles);
    schedule();
}

void timers_hblank(void) {}
