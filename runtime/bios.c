// High-level replacement for the PlayStation BIOS kernel functions the game uses.
#include "psx.h"
#include <setjmp.h>
#include <stdlib.h>

#define KERNEL_STACK 0x8000EFF0u   // exception handlers run here (kernel RAM is otherwise unused)

void psx_exec(uint32_t entry, uint32_t sp, uint32_t gp, uint32_t a0, uint32_t a1);

static inline uint32_t ram32(uint32_t a) { uint32_t v; memcpy(&v, psx_ram + (a & 0x1FFFFC), 4); return v; }
static inline void wram32(uint32_t a, uint32_t v) { memcpy(psx_ram + (a & 0x1FFFFC), &v, 4); }
// Guest memory for BIOS arguments: main RAM or the scratchpad.
static uint8_t *guest_ptr(uint32_t a) {
    if ((a & 0x1FFFFC00) == 0x1F800000) return psx_scratch + (a & 0x3FF);
    return psx_ram + (a & 0x1FFFFF);
}
static const char *ram_str(uint32_t a) { return (const char *)guest_ptr(a); }

// -------------------------------------------------------------------- events
#define MAX_EVENTS 32
typedef struct { uint32_t cls, spec, mode, func, status; bool used; } Event;
static Event events[MAX_EVENTS];

enum { EV_ST_DISABLED = 0x1000, EV_ST_ENABLED = 0x2000, EV_ST_READY = 0x4000 };
enum { EV_MD_INTR = 0x1000, EV_MD_NOINTR = 0x2000 };

static void deliver_event(CPU *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        Event *e = &events[i];
        if (!e->used || e->cls != cls || e->spec != spec || e->status != EV_ST_ENABLED) continue;
        if (e->mode == EV_MD_INTR) {
            if (e->func) {
                CPU saved = *c;
                c->r[29] = KERNEL_STACK - 0x400;
                dispatch_call(c, e->func);
                uint64_t cyc = c->cycles, nxt = c->next_event;
                *c = saved;
                c->cycles = cyc; c->next_event = nxt;
            }
        } else {
            e->status = EV_ST_READY;
        }
    }
}

// Card operations complete asynchronously: queued and delivered on the next vblank.
static struct { uint32_t cls, spec; } pending_ev[16];
static int n_pending_ev;
static void queue_event(uint32_t cls, uint32_t spec) {
    if (n_pending_ev < 16) { pending_ev[n_pending_ev].cls = cls; pending_ev[n_pending_ev].spec = spec; n_pending_ev++; }
}

// ------------------------------------------------------------------ interrupts
static uint32_t chains[4];
static uint32_t custom_exit;          // jmp_buf address set by SetCustomExitFromException
static bool clear_pad = true, clear_rcnt[4] = {true, true, true, true};
static uint32_t pad_buf[2], pad_size[2];
static bool pad_started;
static jmp_buf exc_jb;
static int in_exception;

static void read_pads(void) {
    for (int p = 0; p < 2; p++) {
        if (!pad_buf[p]) continue;
        uint8_t *b = psx_ram + (pad_buf[p] & 0x1FFFFF);
        PadState *s = &pads[p];
        if (!s->connected) { b[0] = 0xFF; continue; }
        uint16_t btn = (uint16_t)~s->buttons;
        b[0] = 0x00;
        if (pad_analog_mode) {
            b[1] = 0x73;
            b[2] = btn & 0xFF; b[3] = btn >> 8;
            b[4] = s->rx; b[5] = s->ry; b[6] = s->lx; b[7] = s->ly;
        } else {
            b[1] = 0x41;
            b[2] = btn & 0xFF; b[3] = btn >> 8;
        }
    }
}

bool bios_exception(CPU *c) {
    if (in_exception) return false;
    in_exception = 1;
    CPU saved = *c;
    c->sr = (c->sr & ~0x3Fu) | ((c->sr << 2) & 0x3Cu);   // push interrupt-enable stack
    c->epc = 0;
    c->r[29] = KERNEL_STACK;

    uint32_t pending = i_stat & i_mask;
    DBG(3, "[irq] exception i_stat=%03x i_mask=%03x hook=%08x\n", i_stat, i_mask, custom_exit);
    // Built-in kernel handlers: pads and root counters on vblank.
    if (pending & 1) {
        if (pad_started) read_pads();
        for (int i = 0; i < n_pending_ev; i++) deliver_event(c, pending_ev[i].cls, pending_ev[i].spec);
        n_pending_ev = 0;
        deliver_event(c, 0xF2000003, 0x0002);
        if ((pad_started && clear_pad) || clear_rcnt[3]) i_stat &= ~1u;
    }
    for (int t = 0; t < 3; t++) {
        if (pending & (1u << (4 + t))) {
            deliver_event(c, 0xF2000000 + t, 0x0002);
            if (clear_rcnt[t]) i_stat &= ~(1u << (4 + t));
        }
    }
    // Priority chains installed with SysEnqIntRP.
    for (int prio = 0; prio < 4; prio++) {
        for (uint32_t s = chains[prio]; s; s = ram32(s)) {
            uint32_t f1 = ram32(s + 8), f2 = ram32(s + 4);
            if (!f1) continue;
            c->r[31] = 0;
            dispatch_call(c, f1);
            if (c->r[2] && f2) {
                c->r[4] = c->r[2];
                dispatch_call(c, f2);
            }
        }
    }
    // Custom exit (libetc's interrupt dispatcher), which leaves via ReturnFromException.
    if (custom_exit) {
        uint32_t jb = custom_exit;
#ifdef RECOMP_TRACE
        extern int trace_depth;
        int depth = trace_depth;
#endif
        if (!setjmp(exc_jb)) {
            uint32_t pc = ram32(jb);
            c->r[29] = ram32(jb + 4);
            c->r[30] = ram32(jb + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = ram32(jb + 12 + i * 4);
            c->r[28] = ram32(jb + 44);
            c->r[2] = 1;
            c->r[31] = 0;
            dispatch_call(c, pc);
        }
#ifdef RECOMP_TRACE
        trace_depth = depth;
#endif
    }
    uint64_t cyc = c->cycles, nxt = c->next_event;
    *c = saved;
    c->cycles = cyc;
    c->next_event = nxt;
    in_exception = 0;
    irq_update();
    return true;
}

// ------------------------------------------------------------- memory card
#define MC_SIZE 0x20000
typedef struct { uint8_t data[MC_SIZE]; char path[1100]; bool present, dirty; } MemCard;
static MemCard cards[2];

static void mc_checksum(uint8_t *frame) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= frame[i];
    frame[127] = x;
}

static void mc_format(MemCard *m) {
    memset(m->data, 0, MC_SIZE);
    m->data[0] = 'M'; m->data[1] = 'C';
    mc_checksum(m->data);
    for (int i = 1; i < 16; i++) {
        uint8_t *f = m->data + i * 128;
        f[0] = 0xA0;
        f[8] = 0xFF; f[9] = 0xFF;
        mc_checksum(f);
    }
    for (int i = 16; i < 36; i++) {
        uint8_t *f = m->data + i * 128;
        f[0] = 0xFF; f[1] = 0xFF; f[2] = 0xFF; f[3] = 0xFF;
        f[8] = 0xFF; f[9] = 0xFF;
        mc_checksum(f);
    }
    memcpy(m->data + 63 * 128, m->data, 128);
    m->dirty = true;
}

static void mc_flush(MemCard *m) {
    if (!m->dirty || !m->present) return;
    FILE *f = fopen(m->path, "wb");
    if (f) { fwrite(m->data, 1, MC_SIZE, f); fclose(f); m->dirty = false; }
    else LOG("[card] cannot write %s\n", m->path);
}

static void mc_init(int port) {
    MemCard *m = &cards[port];
    if (m->present) return;
    snprintf(m->path, sizeof m->path, "%smemcard%d.mcd", save_dir, port + 1);
    FILE *f = fopen(m->path, "rb");
    if (f && fread(m->data, 1, MC_SIZE, f) == MC_SIZE && m->data[0] == 'M' && m->data[1] == 'C') {
        fclose(f);
    } else {
        if (f) fclose(f);
        mc_format(m);
    }
    m->present = port == 0;   // one card, in slot 1
    if (m->present) mc_flush(m);
}

static uint8_t *mc_dir(MemCard *m, int block) { return m->data + block * 128; }

static int mc_find(MemCard *m, const char *name) {
    for (int b = 1; b < 16; b++) {
        uint8_t *d = mc_dir(m, b);
        if ((d[0] & 0xF0) == 0x50 && d[0] == 0x51 && strncmp((char *)d + 10, name, 20) == 0) return b;
    }
    return -1;
}

static bool mc_match(const char *pat, const char *name) {
    for (; *pat; pat++, name++) {
        if (*pat == '*') return true;
        if (*pat == '?') { if (!*name) return false; continue; }
        if (*pat != *name) return false;
    }
    return *name == 0;
}

// Parses "buXY:NAME" into port (0/1) and name.
static bool mc_path(const char *path, int *port, const char **name) {
    if (strncmp(path, "bu", 2) != 0) return false;
    *port = path[2] == '1' ? 1 : 0;
    const char *colon = strchr(path, ':');
    *name = colon ? colon + 1 : path + 5;
    return true;
}

typedef struct { bool open; int port, first_block, pos, size; bool async; } FileDesc;
static FileDesc fds[16];

static int mc_block_at(MemCard *m, int first, int index) {
    int b = first;
    while (index-- > 0 && b >= 1 && b < 16) {
        uint16_t nx = mc_dir(m, b)[8] | (mc_dir(m, b)[9] << 8);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    return b;
}

static int mc_create(MemCard *m, const char *name, int blocks) {
    int list[15], n = 0;
    for (int b = 1; b < 16 && n < blocks; b++)
        if ((mc_dir(m, b)[0] & 0xF0) == 0xA0) list[n++] = b;
    if (n < blocks || blocks < 1) return -1;
    for (int i = 0; i < n; i++) {
        uint8_t *d = mc_dir(m, list[i]);
        memset(d, 0, 128);
        d[0] = i == 0 ? 0x51 : (i == n - 1 ? 0x53 : 0x52);
        if (n == 1) d[0] = 0x51;
        if (i == 0) { uint32_t sz = blocks * 8192; memcpy(d + 4, &sz, 4); strncpy((char *)d + 10, name, 20); }
        uint16_t nx = i == n - 1 ? 0xFFFF : (uint16_t)(list[i + 1] - 1);
        d[8] = nx & 0xFF; d[9] = nx >> 8;
        mc_checksum(d);
        memset(m->data + list[i] * 8192, 0, 8192);
    }
    m->dirty = true;
    return list[0];
}

static void mc_erase(MemCard *m, int first) {
    int b = first;
    while (b >= 1 && b < 16) {
        uint8_t *d = mc_dir(m, b);
        uint16_t nx = d[8] | (d[9] << 8);
        d[0] = (d[0] & 0x0F) | 0xA0;
        mc_checksum(d);
        b = nx == 0xFFFF ? -1 : nx + 1;
    }
    m->dirty = true;
}

static uint32_t dirent_iter_port, dirent_iter_block;
static char dirent_pattern[32];

static uint32_t mc_next_dirent(uint32_t dst) {
    MemCard *m = &cards[dirent_iter_port];
    if (!m->present) return 0;
    for (uint32_t b = dirent_iter_block; b < 16; b++) {
        uint8_t *d = mc_dir(m, b);
        if (d[0] != 0x51 || !mc_match(dirent_pattern, (char *)d + 10)) continue;
        uint8_t *o = guest_ptr(dst);
        memset(o, 0, 40);
        strncpy((char *)o, (char *)d + 10, 20);
        uint32_t attr = 0x50, size;
        memcpy(&size, d + 4, 4);
        memcpy(o + 20, &attr, 4);
        memcpy(o + 24, &size, 4);
        uint32_t head = b;
        memcpy(o + 32, &head, 4);
        dirent_iter_block = b + 1;
        return dst;
    }
    return 0;
}

// --------------------------------------------------------------- heap/misc
static uint32_t heap_base, heap_size;

static void do_execute(CPU *c) {
    uint32_t h = c->r[4];
    uint32_t pc = ram32(h), gp = ram32(h + 4);
    uint32_t b_addr = ram32(h + 0x18), b_size = ram32(h + 0x1C);
    uint32_t s_addr = ram32(h + 0x20), s_size = ram32(h + 0x24);
    if (b_addr && b_size) memset(psx_ram + (b_addr & 0x1FFFFF), 0, b_size);
    uint32_t sp = s_addr ? s_addr + s_size : c->r[29];
    LOG("[bios] DoExecute pc=%08x sp=%08x\n", pc, sp);
    for (int p = 0; p < 2; p++) mc_flush(&cards[p]);
    psx_exec(pc, sp, gp, c->r[5], c->r[6]);
}

void bios_call(CPU *c, uint32_t table, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    (void)a3;
    uint32_t v0 = 0;
    DBG(2, "[bios] %02X:%02X(%08x, %08x, %08x) ra=%08x\n", table, fn, a0, a1, a2, c->r[31]);
    switch ((table << 8) | fn) {
    // ---- A0
    case 0xA039: heap_base = a0; heap_size = a1; break;
    case 0xA043: do_execute(c); return;
    case 0xA044: break;   // FlushCache
    case 0xA049: gpu_gp0(a0); break;   // GPU_cw
    case 0xA070: mc_init(0); mc_init(1); break;   // _bu_init
    case 0xA072: break;   // CdRemove
    case 0xA09F: break;   // SetMem
    case 0xA0AB:   // _card_info(port)
    case 0xA0AC: { // _card_load(port)
        int port = (a0 >> 4) & 1;
        mc_init(port);
        queue_event(0xF4000001, cards[port].present ? 0x0004 : 0x8000);
        v0 = 1;
        break;
    }
    // ---- B0
    case 0xB007: deliver_event(c, a0, a1); v0 = 1; break;
    case 0xB008: {   // OpenEvent(class, spec, mode, func)
        v0 = 0xFFFFFFFF;
        for (int i = 0; i < MAX_EVENTS; i++) {
            if (events[i].used) continue;
            events[i] = (Event){a0, a1, a2, c->r[7], EV_ST_DISABLED, true};
            v0 = 0xF1000000u | i;
            break;
        }
        break;
    }
    case 0xB009: events[a0 & 0xFF].used = false; v0 = 1; break;
    case 0xB00B: {   // TestEvent
        Event *e = &events[a0 & 0xFF];
        if (e->status == EV_ST_READY) { e->status = EV_ST_ENABLED; v0 = 1; }
        break;
    }
    case 0xB00C: events[a0 & 0xFF].status = EV_ST_ENABLED; v0 = 1; break;
    case 0xB00D: events[a0 & 0xFF].status = EV_ST_DISABLED; v0 = 1; break;
    case 0xB012: pad_buf[0] = a0; pad_size[0] = a1; pad_buf[1] = a2; pad_size[1] = c->r[7]; v0 = 2; break;
    case 0xB013: pad_started = true; i_mask |= 1; irq_update(); v0 = 1; break;
    case 0xB014: pad_started = false; break;
    case 0xB015: pad_buf[0] = a1; pad_buf[1] = a1 + 0x22; pad_started = true; i_mask |= 1; v0 = 2; break;
    case 0xB017:   // ReturnFromException
        if (in_exception) longjmp(exc_jb, 1);
        break;
    case 0xB018: custom_exit = 0; break;
    case 0xB019: custom_exit = a0; break;
    case 0xB032: {   // open(name, mode)
        int port; const char *name;
        v0 = 0xFFFFFFFF;
        if (!mc_path(ram_str(a0), &port, &name)) { LOG("[bios] open %s unsupported\n", ram_str(a0)); break; }
        mc_init(port);
        MemCard *m = &cards[port];
        if (!m->present) break;
        int b = mc_find(m, name);
        if (a1 & 0x200) {   // create
            if (b >= 0) break;
            b = mc_create(m, name, (int)(a1 >> 16) ? (int)(a1 >> 16) : 1);
            if (b < 0) break;
        }
        if (b < 0) break;
        for (int fd = 2; fd < 16; fd++) {
            if (fds[fd].open) continue;
            uint32_t sz; memcpy(&sz, mc_dir(m, b) + 4, 4);
            fds[fd] = (FileDesc){true, port, b, 0, (int)sz, (a1 & 0x8000) != 0};
            v0 = fd;
            break;
        }
        LOG("[card] open %s mode %x -> %d\n", name, a1, (int)v0);
        break;
    }
    case 0xB033: {   // lseek(fd, off, whence)
        FileDesc *f = &fds[a0 & 15];
        if (!f->open) { v0 = 0xFFFFFFFF; break; }
        f->pos = a2 == 0 ? (int)a1 : f->pos + (int)a1;
        v0 = f->pos;
        break;
    }
    case 0xB034:     // read(fd, dst, len)
    case 0xB035: {   // write(fd, src, len)
        if (fn == 0x35 && a0 == 1) {   // stdout
            fwrite(ram_str(a1), 1, a2, stderr);
            v0 = a2;
            break;
        }
        FileDesc *f = &fds[a0 & 15];
        if (!f->open) { v0 = 0xFFFFFFFF; break; }
        MemCard *m = &cards[f->port];
        uint32_t done = 0;
        if (fn == 0x35) unlock_all_card_write(a1, a2, true);
        while (done < a2 && f->pos < f->size) {
            int blk = mc_block_at(m, f->first_block, f->pos / 8192);
            if (blk < 1) break;
            uint32_t off = blk * 8192 + f->pos % 8192;
            uint32_t n = 8192 - f->pos % 8192;
            if (n > a2 - done) n = a2 - done;
            if (fn == 0x34) memcpy(guest_ptr(a1 + done), m->data + off, n);
            else { memcpy(m->data + off, guest_ptr(a1 + done), n); m->dirty = true; }
            done += n;
            f->pos += n;
        }
        if (fn == 0x35) unlock_all_card_write(a1, a2, false);
        if (fn == 0x35) mc_flush(m);
        v0 = done;
        if (f->async) queue_event(0xF4000001, 0x0004);
        break;
    }
    case 0xB036: fds[a0 & 15].open = false; v0 = a0; break;
    case 0xB038: host_fatal("game called exit(%d)", (int)a0); break;
    case 0xB041: {   // FormatDevice
        int port; const char *name;
        if (mc_path(ram_str(a0), &port, &name)) { mc_init(port); mc_format(&cards[port]); mc_flush(&cards[port]); v0 = 1; }
        break;
    }
    case 0xB042: {   // firstfile(pattern, dirent)
        int port; const char *name;
        v0 = 0;
        if (!mc_path(ram_str(a0), &port, &name)) break;
        mc_init(port);
        dirent_iter_port = port;
        dirent_iter_block = 1;
        snprintf(dirent_pattern, sizeof dirent_pattern, "%s", name);
        v0 = mc_next_dirent(a1);
        break;
    }
    case 0xB043: v0 = mc_next_dirent(a0); break;   // nextfile(dirent)
    case 0xB045: {   // erase(name)
        int port; const char *name;
        if (mc_path(ram_str(a0), &port, &name)) {
            int b = mc_find(&cards[port], name);
            if (b > 0) { mc_erase(&cards[port], b); mc_flush(&cards[port]); v0 = 1; }
        }
        break;
    }
    case 0xB04A: mc_init(0); mc_init(1); break;   // InitCARD
    case 0xB04B: break;   // StartCARD
    case 0xB04C: break;   // StopCARD
    case 0xB04E:     // _card_write(port, sector, src)
    case 0xB04F: {   // _card_read(port, sector, dst)
        int port = (a0 >> 4) & 1;
        MemCard *m = &cards[port];
        if (m->present && a1 < 1024) {
            if (fn == 0x4E) { memcpy(m->data + a1 * 128, psx_ram + (a2 & 0x1FFFFF), 128); m->dirty = true; mc_flush(m); }
            else memcpy(psx_ram + (a2 & 0x1FFFFF), m->data + a1 * 128, 128);
            queue_event(0xF4000001, 0x0004);
            v0 = 1;
        } else {
            queue_event(0xF4000001, 0x8000);
        }
        break;
    }
    case 0xB050: break;   // _new_card
    case 0xB056: v0 = 0x674; break;   // GetC0Table
    case 0xB057: v0 = 0x874; break;   // GetB0Table
    case 0xB05B: clear_pad = a0 != 0; break;
    // ---- C0
    case 0xC002: {   // SysEnqIntRP(prio, struct)
        uint32_t prio = a0 & 3;
        wram32(a1, chains[prio]);
        chains[prio] = a1;
        v0 = 1;
        break;
    }
    case 0xC003: {   // SysDeqIntRP(prio, struct)
        uint32_t prio = a0 & 3, *link = &chains[prio];
        while (*link) {
            if (*link == a1) { *link = ram32(a1); break; }
            link = (uint32_t *)(psx_ram + (*link & 0x1FFFFC));
        }
        v0 = 1;
        break;
    }
    case 0xC00A: v0 = clear_rcnt[a0 & 3]; clear_rcnt[a0 & 3] = a1 != 0; break;
    default:
        LOG("[bios] unimplemented %02X:%02X (a0=%08x a1=%08x ra=%08x)\n", table, fn, a0, a1, c->r[31]);
        break;
    }
    c->r[2] = v0;
}

void bios_init(void) {
    memset(events, 0, sizeof events);
    memset(chains, 0, sizeof chains);
    custom_exit = 0;
    i_mask = 0;
    cpu.sr = 0x40000401;   // COP2 usable, IM2 + IEc: the BIOS starts executables with interrupts on
    // A jr ra at the BIOS vectors, in case game code inspects them.
    for (uint32_t a = 0xA0; a <= 0xC0; a += 0x10) wram32(a, 0x03E00008);
    // Function tables at their usual kernel addresses. Libraries look up BIOS routines
    // through them to patch their code; every entry points at a scratch area so those
    // patches land somewhere harmless.
    for (uint32_t i = 0; i < 0xC0; i++) wram32(0x200 + i * 4, 0x00001000);
    for (uint32_t i = 0; i < 0x80; i++) wram32(0x874 + i * 4, 0x00001000);
    for (uint32_t i = 0; i < 0x20; i++) wram32(0x674 + i * 4, 0x00001000);
}

// Loads the boot executable named in SYSTEM.CNF into RAM, like the BIOS shell does.
int bios_load_exe(const char *path, uint32_t *entry, uint32_t *sp, uint32_t *gp) {
    uint32_t lba, size;
    if (cdrom_find_file(path, &lba, &size)) return -1;
    uint8_t *buf = malloc(size + 2352);
    uint8_t sec[2352];
    for (uint32_t i = 0; i * 2048 < size; i++) {
        if (cdrom_read_sector_raw(lba + i, sec)) { free(buf); return -1; }
        memcpy(buf + i * 2048, sec + 24, 2048);
    }
    uint32_t pc0, gp0, taddr, tsize, saddr, ssize;
    memcpy(&pc0, buf + 0x10, 4); memcpy(&gp0, buf + 0x14, 4);
    memcpy(&taddr, buf + 0x18, 4); memcpy(&tsize, buf + 0x1C, 4);
    memcpy(&saddr, buf + 0x30, 4); memcpy(&ssize, buf + 0x34, 4);
    memcpy(psx_ram + (taddr & 0x1FFFFF), buf + 0x800, tsize);
    free(buf);
    *entry = pc0;
    *gp = gp0;
    *sp = saddr ? saddr + ssize : 0x801FFF00;
    return 0;
}
