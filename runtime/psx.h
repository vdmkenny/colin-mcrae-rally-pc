// Internal runtime interfaces shared by the hardware/BIOS replacements.
#pragma once
#include "recomp.h"
#include <stdbool.h>
#include <stdio.h>

#define PSX_CLOCK 33868800u
#define CYCLES_PER_FRAME (PSX_CLOCK / 60u)   // NTSC field, close enough to 59.94 Hz
#define CYCLES_PER_SAMPLE 768u               // SPU runs at 44100 Hz

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
extern int log_level;
#define DBG(lvl, ...) do { if (log_level >= (lvl)) fprintf(stderr, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------- interrupts
enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3,
    IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6, IRQ_SIO = 7, IRQ_SPU = 9,
};
extern uint32_t i_stat, i_mask;
void irq_raise(int n);
void irq_update(void);   // re-evaluate pending interrupts after a state change

// ----------------------------------------------------------------- scheduler
enum { EV_VBLANK, EV_HBLANK, EV_CDROM_IRQ, EV_CDROM_READ, EV_TIMER, EV_DMA, EV_SIO, EV_SIO_ACK, EV_COUNT };
typedef void (*EventFn)(void);
void sched_init(void);
void sched_at(int ev, uint64_t when, EventFn fn);
void sched_cancel(int ev);
bool sched_pending(int ev);

// --------------------------------------------------------------- modules
extern const Module *const all_modules[];
extern const unsigned num_modules;
const Module *module_active(void);
bool module_activate(uint32_t entry);
void psx_start(uint32_t entry, uint32_t sp, uint32_t gp);   // never returns

// ---------------------------------------------------------------------- BIOS
void bios_init(void);
bool bios_exception(CPU *c);       // returns true if an interrupt was taken
void bios_vblank(void);
int bios_load_exe(const char *disc_path, uint32_t *entry, uint32_t *sp, uint32_t *gp);

// ------------------------------------------- game changes (race60.c, view.c, unlock.c)
extern int frame_interval_override;
extern bool race_60fps;
extern float draw_distance, cull_margin;
extern bool unlock_all;
void view_raise(uint32_t gp);
void view_restore(uint32_t gp);
void primbufs_use(uint32_t gp);
void unlock_all_vblank(void);
void unlock_all_card_write(uint32_t src, uint32_t len, bool before);   // around a memory card write

// ------------------------------------------------------------------ extra cars (cars.c)
#define NUM_DISC_CARS 12   // the last, a Renault Megane, never shipped
#define MAX_EXTRA_CARS 32
typedef struct {
    char name[32], short_name[16];
    int base;                       // disc car used for handling, sounds and records
    int bhp, wheelbase, drive;      // for the car menu; drive 0 4WD, 1 FWD, 2 RWD
    int mass;                       // kg, or 0 for the game's (1120 for every car)
    uint32_t lba[7], size[7];       // files served from memory (lba 0: from the base car)
    const uint8_t *data[7];
    uint32_t showroom;              // its showroom object in the frontend (0: none)
} ExtraCar;
extern ExtraCar extra_cars[MAX_EXTRA_CARS];
extern int num_extra_cars;
extern int car_extra[2];   // extra car picked by each player, or -1
extern bool extra_cars_enabled;
void cars_init(void);

// ------------------------------------------------------------------- devices
void dma_write(uint32_t addr, uint32_t v);
uint32_t dma_read(uint32_t addr);

void timers_init(void);
uint32_t timer_read(uint32_t addr);
void timer_write(uint32_t addr, uint32_t v);
void timers_hblank(void);

void gpu_init(void);
void gpu_gp0(uint32_t v);
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_status(void);
void gpu_vblank(void);
void gpu_dma_block(uint32_t addr, uint32_t words, bool to_gpu);
void gpu_dma_list(uint32_t addr);
void gpu_vram_read(int x, int y, int w, int h, uint16_t *out);
// Converts the current display area to RGBA; returns false if display is off.
bool gpu_display(uint32_t *rgba, int *w, int *h, int *stride);

void gte_init(void);

bool cdrom_open(const char *cue_path);
void cdrom_init(void);
uint8_t cdrom_read(int reg);
void cdrom_write(int reg, uint8_t v);
void cdrom_dma(uint32_t addr, uint32_t words);
void cdrom_audio(int16_t *buf, int frames);   // CD-DA / XA samples at 44.1 kHz, stereo
int cdrom_read_sector_raw(uint32_t lba, uint8_t *out2352);
int cdrom_find_file(const char *path, uint32_t *lba, uint32_t *size);
uint32_t cdrom_add_file(const uint8_t *data, uint32_t size);   // serves data at the returned LBA
void cdrom_activity(int *data, int *xa, int *cdda);   // sectors delivered since the last call

void spu_init(void);
uint16_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint16_t v);
void spu_dma_write(uint32_t addr, uint32_t words);
void spu_dma_read(uint32_t addr, uint32_t words);
void spu_render(int16_t *out, int frames);     // mixes voices + CD audio, stereo
void spu_run_to(uint64_t cycles);

void mdec_init(void);
void mdec_write(int reg, uint32_t v);
uint32_t mdec_read(int reg);
void mdec_dma_in(uint32_t addr, uint32_t words);
void mdec_dma_out(uint32_t addr, uint32_t words);

uint8_t sio_read8(uint32_t addr);
uint32_t sio_read(uint32_t addr, int size);
void sio_write(uint32_t addr, uint32_t v, int size);

// -------------------------------------------------------------------- input
typedef struct {
    uint16_t buttons;          // PlayStation layout, active high (see PAD_* below)
    uint8_t lx, ly, rx, ry;    // analog sticks, 0x80 = centre
    uint8_t negcon_i, negcon_ii, negcon_l;   // NeGcon analog buttons, 0 = released
    bool connected;
} PadState;
enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};
extern PadState pads[2];
extern bool pad_analog_mode;

// --------------------------------------------------------------------- host
void host_frame(void);        // called every vblank: present, input, audio, pacing
void host_fatal(const char *fmt, ...);
extern char save_dir[1024];
