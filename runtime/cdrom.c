// CD-ROM controller (register level), disc image access, XA-ADPCM and CD-DA audio.
#include "psx.h"
#include <ctype.h>
#include <stdlib.h>

// ---------------------------------------------------------------- disc image
typedef struct {
    FILE *f;
    uint32_t file_lba;     // disc LBA of the first sector in the file
    uint32_t sectors;
    uint32_t start_lba;    // INDEX 01
    bool audio;
} Track;
static Track tracks[100];
static int num_tracks;
static uint32_t disc_end_lba;

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

bool cdrom_open(const char *cue_path) {
    FILE *cue = fopen(cue_path, "r");
    if (!cue) return false;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    char line[1024];
    FILE *cur = NULL;
    uint32_t file_lba = 0, file_sectors = 0;
    int t = -1;
    while (fgets(line, sizeof line, cue)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            *q2 = 0;
            char path[2048];
            snprintf(path, sizeof path, "%s%s", dir, q1 + 1);
            if (cur) file_lba += file_sectors;
            cur = fopen(path, "rb");
            if (!cur) { LOG("cannot open %s\n", path); fclose(cue); return false; }
            fseek(cur, 0, SEEK_END);
            file_sectors = (uint32_t)(ftell(cur) / 2352);
        } else if (!strncmp(p, "TRACK", 5)) {
            int n; char type[32];
            if (sscanf(p, "TRACK %d %31s", &n, type) != 2) continue;
            t = num_tracks++;
            tracks[t].f = cur;
            tracks[t].file_lba = file_lba;
            tracks[t].sectors = file_sectors;
            tracks[t].audio = !strcmp(type, "AUDIO");
            tracks[t].start_lba = file_lba;
        } else if (!strncmp(p, "INDEX", 5) && t >= 0) {
            int idx, m, s, f;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1)
                tracks[t].start_lba = tracks[t].file_lba + (uint32_t)((m * 60 + s) * 75 + f);
        }
    }
    fclose(cue);
    if (cur) disc_end_lba = file_lba + file_sectors;
    return num_tracks > 0;
}

static Track *track_for(uint32_t lba) {
    for (int i = num_tracks - 1; i >= 0; i--)
        if (lba >= tracks[i].file_lba) return &tracks[i];
    return NULL;
}

static int track_index_for(uint32_t lba) {
    for (int i = num_tracks - 1; i >= 0; i--)
        if (lba >= tracks[i].start_lba) return i;
    return 0;
}

// Files that are not on the disc (extra cars) get sectors past its end and are read from memory.
#define EXTRA_LBA 200000u
typedef struct { uint32_t lba, size; const uint8_t *data; } ExtraFile;
static ExtraFile extra_files[256];
static int num_extra;
static uint32_t extra_next = EXTRA_LBA;

uint32_t cdrom_add_file(const uint8_t *data, uint32_t size) {
    if (num_extra == (int)(sizeof extra_files / sizeof *extra_files)) return 0;
    ExtraFile *e = &extra_files[num_extra++];
    e->lba = extra_next; e->size = size; e->data = data;
    extra_next += (size + 2047) / 2048 + 1;
    return e->lba;
}

static void extra_sector(uint32_t lba, uint8_t *out) {
    memset(out, 0, 2352);
    memset(out + 1, 0xff, 10);
    uint32_t a = lba + 150;
    out[12] = (uint8_t)bcd(a / 4500); out[13] = (uint8_t)bcd(a / 75 % 60); out[14] = (uint8_t)bcd(a % 75);
    out[15] = 2;
    out[18] = out[22] = 0x08;   // Form 1 data
    for (int i = 0; i < num_extra; i++) {
        const ExtraFile *e = &extra_files[i];
        if (lba < e->lba || lba >= e->lba + (e->size + 2047) / 2048) continue;
        uint32_t off = (lba - e->lba) * 2048;
        memcpy(out + 24, e->data + off, e->size - off < 2048 ? e->size - off : 2048);
        if (lba + 1 == e->lba + (e->size + 2047) / 2048) out[18] = out[22] = 0x89;   // end of file
    }
}

int cdrom_read_sector_raw(uint32_t lba, uint8_t *out) {
    if (lba >= EXTRA_LBA) { extra_sector(lba, out); return 0; }
    Track *t = track_for(lba);
    if (!t || lba >= t->file_lba + t->sectors) { memset(out, 0, 2352); return -1; }
    fseek(t->f, (long)(lba - t->file_lba) * 2352, SEEK_SET);
    if (fread(out, 1, 2352, t->f) != 2352) return -1;
    return 0;
}

// ISO9660 lookup of a path like "\\CMRALLY\\GAME.EXE;1" or "cdrom:\\SCUS_944.74;1".
int cdrom_find_file(const char *path, uint32_t *lba_out, uint32_t *size_out) {
    uint8_t sec[2352];
    if (cdrom_read_sector_raw(16, sec)) return -1;
    uint32_t dir_lba, dir_size;
    memcpy(&dir_lba, sec + 24 + 156 + 2, 4);
    memcpy(&dir_size, sec + 24 + 156 + 10, 4);
    const char *p = strchr(path, ':') ? strchr(path, ':') + 1 : path;
    while (*p == '\\' || *p == '/') p++;
    char comp[64];
    while (*p) {
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && *p != ';' && n < 63) comp[n++] = (char)toupper((unsigned char)*p++);
        comp[n] = 0;
        while (*p && *p != '\\' && *p != '/') p++;
        while (*p == '\\' || *p == '/') p++;
        bool found = false;
        for (uint32_t s = 0; s < (dir_size + 2047) / 2048 && !found; s++) {
            if (cdrom_read_sector_raw(dir_lba + s, sec)) return -1;
            const uint8_t *d = sec + 24;
            for (int pos = 0; pos < 2048;) {
                int len = d[pos];
                if (!len) break;
                int nl = d[pos + 32];
                char name[64];
                memcpy(name, d + pos + 33, nl < 63 ? nl : 63);
                name[nl < 63 ? nl : 63] = 0;
                char *semi = strchr(name, ';');
                if (semi) *semi = 0;
                if (!strcmp(name, comp)) {
                    memcpy(&dir_lba, d + pos + 2, 4);
                    memcpy(&dir_size, d + pos + 10, 4);
                    found = true;
                    break;
                }
                pos += len;
            }
        }
        if (!found) return -1;
    }
    *lba_out = dir_lba;
    *size_out = dir_size;
    return 0;
}

// ----------------------------------------------------------- controller state
static uint8_t index_reg;
static uint8_t params[16]; static int nparams;
static uint8_t resp[16]; static int resp_len, resp_pos;
static uint8_t irq_enable = 0x1F, irq_flag;
static uint8_t request;
static uint8_t stat = 0x02;   // motor on
static uint8_t mode;
static uint32_t setloc_lba, cur_lba;
static bool setloc_pending;
static uint8_t filter_file, filter_chan;
static bool muted, adpcm_muted;
static uint8_t vol_ll = 0x80, vol_lr, vol_rl, vol_rr = 0x80;
static uint8_t pend_ll = 0x80, pend_lr, pend_rl, pend_rr = 0x80;

static uint8_t sector[2352];       // last read sector
static uint8_t data_fifo[2352];
static int data_len, data_pos;
static bool sector_ready;

enum { ST_IDLE, ST_READING, ST_PLAYING };
static int drive_state;
static int play_track;   // track being played (by file range, so pregaps count as their track)
static int report_counter;

// pending interrupts (the controller delivers one at a time)
typedef struct { uint8_t type; uint8_t data[16]; int len; uint32_t delay; } PendingIrq;
static PendingIrq queue[8];
static int qlen;
static bool irq_scheduled;

static void deliver_next(void);

static void schedule_delivery(uint32_t delay) {
    if (irq_scheduled) return;
    irq_scheduled = true;
    sched_at(EV_CDROM_IRQ, cpu.cycles + delay, deliver_next);
}

static void push_irq(uint8_t type, const uint8_t *data, int len, uint32_t delay) {
    if (qlen >= 8) { DBG(1, "cdrom: irq queue full\n"); return; }
    PendingIrq *p = &queue[qlen++];
    p->type = type; p->len = len; p->delay = delay;
    memcpy(p->data, data, len);
    if (qlen == 1 && !(irq_flag & 7)) schedule_delivery(delay);
}

static void deliver_next(void) {
    irq_scheduled = false;
    if (!qlen || (irq_flag & 7)) return;
    PendingIrq p = queue[0];
    DBG(4, "[cd] deliver INT%d\n", p.type);
    memmove(queue, queue + 1, sizeof(PendingIrq) * --qlen);
    memcpy(resp, p.data, p.len);
    resp_len = p.len; resp_pos = 0;
    irq_flag = (irq_flag & ~7) | p.type;
    if (irq_flag & irq_enable & 0x1F) irq_raise(IRQ_CDROM);
}

static void irq_stat(uint8_t type, uint32_t delay) { uint8_t s = stat; push_irq(type, &s, 1, delay); }

// ----------------------------------------------------------------- XA / CDDA
#define AUDIO_RING 32768
static int16_t audio_ring[AUDIO_RING * 2];
static uint32_t ring_w, ring_r;
static int32_t xa_prev[2][2];
static uint32_t xa_resample_pos;   // 16.16 fraction for rate conversion
static int16_t xa_last[2];

static int ring_fill(void) { return (int)(ring_w - ring_r); }

static void ring_push(int16_t l, int16_t r) {
    if (ring_fill() >= AUDIO_RING - 1) return;
    audio_ring[(ring_w % AUDIO_RING) * 2] = l;
    audio_ring[(ring_w % AUDIO_RING) * 2 + 1] = r;
    ring_w++;
}

static void apply_volume(int32_t l, int32_t r, int16_t *ol, int16_t *or_) {
    int32_t L = (l * vol_ll + r * vol_rl) >> 7;
    int32_t R = (l * vol_lr + r * vol_rr) >> 7;
    *ol = (int16_t)(L < -32768 ? -32768 : L > 32767 ? 32767 : L);
    *or_ = (int16_t)(R < -32768 ? -32768 : R > 32767 ? 32767 : R);
}

void cdrom_audio(int16_t *buf, int frames) {
    for (int i = 0; i < frames; i++) {
        int16_t l = 0, r = 0;
        if (ring_fill() > 0) {
            l = audio_ring[(ring_r % AUDIO_RING) * 2];
            r = audio_ring[(ring_r % AUDIO_RING) * 2 + 1];
            ring_r++;
        }
        if (muted) l = r = 0;
        buf[i * 2] = l;
        buf[i * 2 + 1] = r;
    }
}

static const int xa_k0[4] = {0, 60, 115, 98}, xa_k1[4] = {0, 0, -52, -55};

static void xa_decode_block(const uint8_t *grp, int unit, bool eight_bit, int16_t *out, int32_t *prev) {
    // 28 samples of sound unit `unit` within a 128-byte sound group
    uint8_t hdr = grp[4 + unit];
    int range = hdr & 15;
    int shift = (eight_bit ? 8 : 12) - range;
    if (shift < 0) shift = eight_bit ? 0 : 9;
    int filt = (hdr >> 4) & 3;
    for (int i = 0; i < 28; i++) {
        int32_t t;
        if (eight_bit) {
            t = (int8_t)grp[16 + unit + i * 4];
        } else {
            uint8_t b = grp[16 + (unit >> 1) + i * 4];
            t = ((unit & 1) ? (b >> 4) : (b & 15));
            if (t & 8) t -= 16;
        }
        int32_t s = (t << shift) + ((prev[0] * xa_k0[filt] + prev[1] * xa_k1[filt] + 32) >> 6);
        s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
        prev[1] = prev[0];
        prev[0] = s;
        out[i] = (int16_t)s;
    }
}

static void xa_sector(const uint8_t *sec) {
    uint8_t coding = sec[16 + 3];
    bool stereo = (coding & 3) == 1;
    bool half_rate = (coding >> 2) & 1;
    bool eight_bit = (coding >> 4) & 1;
    int16_t samples[2][18 * 8 * 28];
    int n[2] = {0, 0};
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = sec + 24 + g * 128;
        int units = eight_bit ? 4 : 8;
        for (int u = 0; u < units; u++) {
            int ch = stereo ? (u & 1) : 0;
            xa_decode_block(grp, u, eight_bit, &samples[ch][n[ch]], xa_prev[ch]);
            n[ch] += 28;
        }
    }
    if (adpcm_muted || muted) return;
    // Resample (linear) from 37800/18900 Hz to 44100 Hz.
    uint32_t rate = half_rate ? 18900 : 37800;
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100);
    int count = n[0];
    uint32_t pos = xa_resample_pos;
    while ((pos >> 16) < (uint32_t)count) {
        int idx = (int)(pos >> 16);
        uint32_t frac = pos & 0xFFFF;
        int16_t l0 = idx ? samples[0][idx - 1] : xa_last[0];
        int16_t l1 = samples[0][idx];
        int16_t r0, r1;
        if (stereo) { r0 = idx ? samples[1][idx - 1] : xa_last[1]; r1 = samples[1][idx]; }
        else { r0 = l0; r1 = l1; }
        int32_t l = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> 16);
        int32_t r = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> 16);
        int16_t ol, or_;
        apply_volume(l, r, &ol, &or_);
        ring_push(ol, or_);
        pos += step;
    }
    xa_resample_pos = pos - ((uint32_t)count << 16);
    xa_last[0] = samples[0][count - 1];
    xa_last[1] = stereo ? samples[1][count - 1] : xa_last[0];
}

static void cdda_sector(const uint8_t *sec) {
    if (muted) return;
    for (int i = 0; i < 588; i++) {
        int16_t l, r, ol, or_;
        memcpy(&l, sec + i * 4, 2);
        memcpy(&r, sec + i * 4 + 2, 2);
        apply_volume(l, r, &ol, &or_);
        ring_push(ol, or_);
    }
}

// --------------------------------------------------------------- read engine
static uint32_t sector_cycles(void) { return (mode & 0x80) ? PSX_CLOCK / 150 : PSX_CLOCK / 75; }

// Sectors delivered since the last call, by kind (for detecting loading).
static int act_data, act_xa, act_cdda;

void cdrom_activity(int *data, int *xa, int *cdda) {
    *data = act_data; *xa = act_xa; *cdda = act_cdda;
    act_data = act_xa = act_cdda = 0;
}

static void read_tick(void);

static void start_read(uint32_t seek_delay) {
    drive_state = ST_READING;
    if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = false; }
    stat = (stat & ~0xE0) | 0x20;
    sched_at(EV_CDROM_READ, cpu.cycles + seek_delay + sector_cycles(), read_tick);
}

static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
    lba += 150;
    *m = (uint8_t)bcd(lba / 4500);
    *s = (uint8_t)bcd((lba / 75) % 60);
    *f = (uint8_t)bcd(lba % 75);
}

static void read_tick(void) {
    if (drive_state == ST_READING) {
        // Hold the sector while the previous interrupt is still unacknowledged.
        if ((irq_flag & 7) || qlen) {
            DBG(3, "[cd] hold sector %u: irq_flag=%02x qlen=%d enable=%02x\n", cur_lba, irq_flag, qlen, irq_enable);
            sched_at(EV_CDROM_READ, cpu.cycles + 4000, read_tick);
            return;
        }
        cdrom_read_sector_raw(cur_lba, sector);
        DBG(3, "[cd] sector %u submode=%02x\n", cur_lba, sector[18]);
        cur_lba++;
        uint8_t submode = sector[18];
        bool is_audio_xa = (sector[15] == 2) && (submode & 0x04);
        if ((mode & 0x40) && is_audio_xa) {
            bool pass = !(mode & 0x08) || (sector[16] == filter_file && sector[17] == filter_chan);
            if (pass) { xa_sector(sector); act_xa++; }
            if (submode & 0x80) { /* end of file */ }
        } else if (!((mode & 0x08) && is_audio_xa)) {
            sector_ready = true;
            act_data++;
            irq_stat(1, 0);
        }
        sched_at(EV_CDROM_READ, cpu.cycles + sector_cycles(), read_tick);
    } else if (drive_state == ST_PLAYING) {
        uint8_t buf[2352];
        int ti = track_index_for(cur_lba);
        if (cur_lba >= disc_end_lba ||
            (play_track + 1 < num_tracks && cur_lba >= tracks[play_track + 1].file_lba && (mode & 0x02))) {
            // end of track with AutoPause
            drive_state = ST_IDLE;
            stat &= ~0x80;
            irq_stat(4, 0);
            return;
        }
        cdrom_read_sector_raw(cur_lba, buf);
        cdda_sector(buf);
        act_cdda++;
        if ((mode & 0x04) && (++report_counter % 10) == 0 && !(irq_flag & 7) && !qlen) {
            uint8_t r[8];
            Track *t = &tracks[ti];
            r[0] = stat;
            r[1] = (uint8_t)bcd(ti + 1);
            r[2] = 1;
            uint32_t rel = cur_lba >= t->start_lba ? cur_lba - t->start_lba : 0;
            bool absolute = (report_counter / 10) & 1;
            uint32_t pos = absolute ? cur_lba + 150 : rel;
            r[3] = (uint8_t)bcd(pos / 4500);
            r[4] = (uint8_t)(bcd((pos / 75) % 60) | (absolute ? 0 : 0x80));
            r[5] = (uint8_t)bcd(pos % 75);
            r[6] = 0; r[7] = 0;
            push_irq(1, r, 8, 0);
        }
        cur_lba++;
        sched_at(EV_CDROM_READ, cpu.cycles + PSX_CLOCK / 75, read_tick);
    }
}

// ------------------------------------------------------------------ commands
#define T_ACK 25000u      // first response
#define T_SECOND 100000u  // second response for slow commands

static void command(uint8_t c) {
    uint8_t r[16];
    DBG(2, "cdrom cmd %02x (%d params: %02x %02x %02x) @%.2fs\n", c, nparams, params[0], params[1], params[2],
        (double)cpu.cycles / PSX_CLOCK);
    switch (c) {
    case 0x01: irq_stat(3, T_ACK); stat &= ~0x10; break;   // Getstat
    case 0x02:   // Setloc
        setloc_lba = (uint32_t)((unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2])) - 150;
        setloc_pending = true;
        irq_stat(3, T_ACK);
        break;
    case 0x03: {   // Play
        if (nparams && params[0]) {
            int tn = unbcd(params[0]);
            if (tn >= 1 && tn <= num_tracks) cur_lba = tracks[tn - 1].start_lba;
            setloc_pending = false;
        } else if (setloc_pending) {
            cur_lba = setloc_lba;
            setloc_pending = false;
        }
        drive_state = ST_PLAYING;
        report_counter = 0;
        play_track = (int)(track_for(cur_lba) - tracks);
        stat = (stat & ~0xE0) | 0x80;
        irq_stat(3, T_ACK);
        sched_at(EV_CDROM_READ, cpu.cycles + T_SECOND, read_tick);
        break;
    }
    case 0x04: case 0x05: irq_stat(3, T_ACK); break;   // Forward/Backward
    case 0x06: case 0x1B:   // ReadN / ReadS
        irq_stat(3, T_ACK);
        start_read(T_ACK * 2);
        break;
    case 0x07: stat |= 0x02; irq_stat(3, T_ACK); irq_stat(2, T_SECOND); break;   // MotorOn
    case 0x08:   // Stop
        drive_state = ST_IDLE;
        sched_cancel(EV_CDROM_READ);
        irq_stat(3, T_ACK);
        stat &= ~0xE2;
        irq_stat(2, T_SECOND);
        break;
    case 0x09:   // Pause
        irq_stat(3, T_ACK);
        drive_state = ST_IDLE;
        sched_cancel(EV_CDROM_READ);
        stat &= ~0xE0;
        irq_stat(2, (mode & 0x80) ? 70000 : 140000);
        break;
    case 0x0A:   // Init
        irq_stat(3, T_ACK);
        mode = 0x20;
        drive_state = ST_IDLE;
        sched_cancel(EV_CDROM_READ);
        stat = 0x02;
        irq_stat(2, T_SECOND);
        break;
    case 0x0B: muted = true; irq_stat(3, T_ACK); break;
    case 0x0C: muted = false; irq_stat(3, T_ACK); break;
    case 0x0D: filter_file = params[0]; filter_chan = params[1]; irq_stat(3, T_ACK); break;
    case 0x0E: mode = params[0]; irq_stat(3, T_ACK); break;
    case 0x0F: r[0] = stat; r[1] = mode; r[2] = 0; r[3] = filter_file; r[4] = filter_chan; push_irq(3, r, 5, T_ACK); break;
    case 0x10:   // GetlocL
        memcpy(r, sector + 12, 8);
        push_irq(3, r, 8, T_ACK);
        break;
    case 0x11: {   // GetlocP
        uint32_t lba = cur_lba ? cur_lba - 1 : 0;
        int ti = track_index_for(lba);
        uint32_t rel = lba >= tracks[ti].start_lba ? lba - tracks[ti].start_lba : tracks[ti].start_lba - lba;
        r[0] = (uint8_t)bcd(ti + 1); r[1] = 1;
        r[2] = (uint8_t)bcd(rel / 4500); r[3] = (uint8_t)bcd((rel / 75) % 60); r[4] = (uint8_t)bcd(rel % 75);
        lba_to_msf(lba, &r[5], &r[6], &r[7]);
        push_irq(3, r, 8, T_ACK);
        break;
    }
    case 0x12: irq_stat(3, T_ACK); irq_stat(2, T_SECOND); break;   // SetSession
    case 0x13: r[0] = stat; r[1] = 1; r[2] = (uint8_t)bcd(num_tracks); push_irq(3, r, 3, T_ACK); break;
    case 0x14: {   // GetTD
        int tn = unbcd(params[0]);
        uint32_t lba = (tn == 0 || tn > num_tracks) ? disc_end_lba : tracks[tn - 1].start_lba;
        uint8_t f;
        r[0] = stat;
        lba_to_msf(lba, &r[1], &r[2], &f);
        push_irq(3, r, 3, T_ACK);
        break;
    }
    case 0x15: case 0x16:   // SeekL / SeekP
        if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = false; }
        drive_state = ST_IDLE;
        sched_cancel(EV_CDROM_READ);
        stat = (stat & ~0xE0) | 0x40;
        irq_stat(3, T_ACK);
        stat &= ~0x40;
        irq_stat(2, T_SECOND);
        break;
    case 0x19:   // Test
        if (params[0] == 0x20) { r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xC0; push_irq(3, r, 4, T_ACK); }
        else irq_stat(3, T_ACK);
        break;
    case 0x1A: {   // GetID
        irq_stat(3, T_ACK);
        uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
        push_irq(2, id, 8, T_SECOND);
        break;
    }
    case 0x1C: irq_stat(3, T_ACK); break;   // Reset
    case 0x1E: irq_stat(3, T_ACK); irq_stat(2, T_SECOND * 4); break;   // ReadTOC
    default:
        LOG("cdrom: unknown command %02x\n", c);
        r[0] = stat | 1; r[1] = 0x40;
        push_irq(5, r, 2, T_ACK);
        break;
    }
    nparams = 0;
}

// ----------------------------------------------------------------- registers
uint8_t cdrom_read(int reg) {
    switch (reg) {
    case 0: {
        uint8_t s = index_reg;
        if (nparams == 0) s |= 0x08;
        if (nparams < 16) s |= 0x10;
        if (resp_pos < resp_len) s |= 0x20;
        if (data_pos < data_len) s |= 0x40;
        return s;
    }
    case 1: return resp_pos < resp_len ? resp[resp_pos++] : 0;
    case 2: return data_pos < data_len ? data_fifo[data_pos++] : 0;
    case 3:
        if (index_reg & 1) return irq_flag | 0xE0;
        return irq_enable | 0xE0;
    }
    return 0;
}

void cdrom_write(int reg, uint8_t v) {
    if (reg == 0) { index_reg = v & 3; return; }
    DBG(4, "[cd] write %d.%d = %02x (flag=%02x qlen=%d)\n", reg, index_reg, v, irq_flag, qlen);
    switch ((reg << 4) | index_reg) {
    case 0x10: command(v); break;
    case 0x11: break;   // sound map data
    case 0x12: break;   // sound map coding info
    case 0x13: pend_rr = v; break;
    case 0x20: if (nparams < 16) params[nparams++] = v; break;
    case 0x21: irq_enable = v & 0x1F; break;
    case 0x22: pend_ll = v; break;
    case 0x23: pend_rl = v; break;
    case 0x30:   // request register
        request = v;
        if (v & 0x80) {
            if (sector_ready && data_pos >= data_len) {
                if (mode & 0x20) { memcpy(data_fifo, sector + 12, 0x924); data_len = 0x924; }
                else { memcpy(data_fifo, sector + 24, 0x800); data_len = 0x800; }
                data_pos = 0;
                sector_ready = false;
            }
        } else {
            data_len = data_pos = 0;
        }
        break;
    case 0x31:   // acknowledge
        irq_flag &= ~(v & 0x1F);
        if (v & 0x40) nparams = 0;
        if (!(irq_flag & 7) && qlen) schedule_delivery(queue[0].delay ? queue[0].delay / 4 + 500 : 500);
        break;
    case 0x32: pend_lr = v; break;
    case 0x33:
        DBG(2, "[cd] volume apply %02x: ll=%02x lr=%02x rl=%02x rr=%02x\n", v, pend_ll, pend_lr, pend_rl, pend_rr);
        adpcm_muted = v & 1;
        if (v & 0x20) { vol_ll = pend_ll; vol_lr = pend_lr; vol_rl = pend_rl; vol_rr = pend_rr; }
        break;
    }
}

void cdrom_dma(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words * 4; i++) {
        uint8_t b = data_pos < data_len ? data_fifo[data_pos++] : 0;
        psx_ram[(addr + i) & RAM_MASK] = b;
    }
}

void cdrom_init(void) {
    stat = 0x02;
    mode = 0;
    qlen = 0;
    irq_flag = 0;
    drive_state = ST_IDLE;
}
