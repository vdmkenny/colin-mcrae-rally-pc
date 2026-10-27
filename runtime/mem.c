// I/O register routing and the DMA controller.
#include "psx.h"

static uint32_t memctrl[16];
static uint32_t ram_size = 0x00000B88;
static uint32_t cache_ctrl;

// ------------------------------------------------------------------------ DMA
static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
static uint32_t dpcr = 0x07654321, dicr;

static void dma_irq_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    bool master = (dicr & (1u << 15)) || ((dicr & (1u << 23)) && (flags & en));
    bool was = dicr & 0x80000000u;
    dicr = master ? (dicr | 0x80000000u) : (dicr & 0x7FFFFFFFu);
    if (master && !was) irq_raise(IRQ_DMA);
}

static void dma_finish(int ch) {
    dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dma_irq_update();
}

static bool mdec_out_pending;

static void dma_run(int ch) {
    uint32_t chcr = dma_chcr[ch];
    uint32_t addr = dma_madr[ch] & RAM_MASK & ~3u;
    uint32_t sync = (chcr >> 9) & 3;
    bool to_dev = chcr & 1;
    uint32_t words;
    if (sync == 0) words = (dma_bcr[ch] & 0xFFFF) ? (dma_bcr[ch] & 0xFFFF) : 0x10000;
    else words = (dma_bcr[ch] & 0xFFFF) * (dma_bcr[ch] >> 16);

    DBG(3, "[dma] ch%d %s addr=%06x words=%u sync=%u\n", ch, to_dev ? "to-dev" : "from-dev", addr, words, sync);
    switch (ch) {
    case 0:  // MDEC in
        mdec_dma_in(addr, words);
        break;
    case 1:  // MDEC out
        mdec_dma_out(addr, words);
        break;
    case 2:  // GPU
        if (sync == 2) gpu_dma_list(addr);
        else gpu_dma_block(addr, words, to_dev);
        break;
    case 3:  // CD-ROM
        cdrom_dma(addr, words);
        break;
    case 4:  // SPU
        if (to_dev) spu_dma_write(addr, words);
        else spu_dma_read(addr, words);
        break;
    case 6: {  // OTC: build an empty ordering table, back to front
        uint32_t a = addr;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t v = (i == words - 1) ? 0x00FFFFFF : ((a - 4) & RAM_MASK);
            memcpy(psx_ram + a, &v, 4);
            a = (a - 4) & RAM_MASK & ~3u;
        }
        break;
    }
    default:
        DBG(1, "DMA%d unsupported\n", ch);
        break;
    }
    if (sync != 2) dma_madr[ch] = (dma_madr[ch] + (chcr & 2 ? -4 : 4) * words) & 0xFFFFFF;
    dma_finish(ch);
}

static void dma_check(int ch) {
    uint32_t chcr = dma_chcr[ch];
    if (!(chcr & (1u << 24))) return;
    if (((chcr >> 9) & 3) == 0 && !(chcr & (1u << 28))) return;
    if (!(dpcr & (8u << (ch * 4)))) return;
    dma_run(ch);
}

void dma_write(uint32_t addr, uint32_t v) {
    uint32_t off = addr & 0x7F;
    if (addr >= 0x1F8010F0) {
        if (off == 0x70) {
            dpcr = v;
            for (int ch = 0; ch < 7; ch++) dma_check(ch);
        } else if (off == 0x74) {
            // flags (24-30) are acknowledged by writing 1
            uint32_t flags = (dicr & 0x7F000000u) & ~(v & 0x7F000000u);
            dicr = (v & 0x00FF803Fu) | flags;
            dma_irq_update();
        }
        return;
    }
    int ch = (addr >> 4) & 7;
    switch (addr & 0xF) {
    case 0: dma_madr[ch] = v & 0xFFFFFF; break;
    case 4: dma_bcr[ch] = v; break;
    case 8:
        dma_chcr[ch] = ch == 6 ? ((v & 0x51000000u) | 2) : v;
        dma_check(ch);
        break;
    }
}

uint32_t dma_read(uint32_t addr) {
    uint32_t off = addr & 0x7F;
    if (addr >= 0x1F8010F0) return off == 0x70 ? dpcr : dicr;
    int ch = (addr >> 4) & 7;
    switch (addr & 0xF) {
    case 0: return dma_madr[ch];
    case 4: return dma_bcr[ch];
    default: return dma_chcr[ch];
    }
}

// ------------------------------------------------------------------ I/O map
uint32_t mem_read_slow(uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (addr == 0xFFFE0130) return cache_ctrl;
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t off = p & 0xFFF;
        if (off < 0x24) return memctrl[off >> 2];
        if (off >= 0x40 && off < 0x60) return sio_read(p, size);
        if (off == 0x60) return ram_size;
        if (off == 0x70) return i_stat;
        if (off == 0x74) return i_mask;
        if (off >= 0x80 && off < 0x100) return dma_read(p & ~3u) >> ((p & 3) * 8);
        if (off >= 0x100 && off < 0x130) return timer_read(p);
        if (off >= 0x800 && off < 0x804) return cdrom_read(off & 3);
        if (off == 0x810) return gpu_read();
        if (off == 0x814) return gpu_status();
        if (off == 0x820 || off == 0x824) return mdec_read((off >> 2) & 1);
        if (off >= 0xC00) {
            if (size == 4) return spu_read(p) | ((uint32_t)spu_read(p + 2) << 16);
            if (size == 1) return (spu_read(p & ~1u) >> ((p & 1) * 8)) & 0xFF;
            return spu_read(p);
        }
        DBG(1, "read%d unknown io %08x\n", size * 8, addr);
        return 0;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return size == 1 ? 0xFF : 0xFFFFFFFF;  // expansion 1
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;  // no BIOS ROM
    if (p >= 0x1F802000 && p < 0x1F803000) return 0;
    DBG(1, "read%d unmapped %08x\n", size * 8, addr);
    return 0;
}

void mem_write_slow(uint32_t addr, uint32_t v, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (addr == 0xFFFE0130) { cache_ctrl = v; return; }
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t off = p & 0xFFF;
        if (off < 0x24) { memctrl[off >> 2] = v; return; }
        if (off >= 0x40 && off < 0x60) { sio_write(p, v, size); return; }
        if (off == 0x60) { ram_size = v; return; }
        if (off == 0x70) { i_stat &= v; irq_update(); return; }
        if (off == 0x74) { i_mask = v & 0x7FF; irq_update(); return; }
        if (off >= 0x80 && off < 0x100) {
            if (size < 4) {
                // Narrow writes only replace their byte lanes; DICR flags in other lanes
                // must not be acknowledged by the merge.
                uint32_t shift = (p & 3) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
                uint32_t old = dma_read(p & ~3u);
                if ((p & ~3u) == 0x1F8010F4) old &= ~0x7F000000u;
                v = (old & ~mask) | ((v << shift) & mask);
            }
            dma_write(p & ~3u, v);
            return;
        }
        if (off >= 0x100 && off < 0x130) { timer_write(p, v); return; }
        if (off >= 0x800 && off < 0x804) { cdrom_write(off & 3, (uint8_t)v); return; }
        if (off == 0x810) { gpu_gp0(v); return; }
        if (off == 0x814) { gpu_gp1(v); return; }
        if (off == 0x820 || off == 0x824) { mdec_write((off >> 2) & 1, v); return; }
        if (off >= 0xC00) {
            if (size == 4) { spu_write(p, (uint16_t)v); spu_write(p + 2, (uint16_t)(v >> 16)); }
            else spu_write(p & ~1u, (uint16_t)v);
            return;
        }
        DBG(1, "write%d unknown io %08x = %08x\n", size * 8, addr, v);
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F803000) {
        if ((p & 0xFFF) == 0x23 || (p & 0xFFF) == 0x80) {  // debug UART / POST
            if ((p & 0xFFF) == 0x23) fputc((int)v, stderr);
        }
        return;
    }
    DBG(1, "write%d unmapped %08x = %08x\n", size * 8, addr, v);
}

// Called by the MDEC when output becomes available for a waiting DMA1.
void dma_mdec_out_ready(void) {
    if (mdec_out_pending) { mdec_out_pending = false; dma_check(1); }
}
