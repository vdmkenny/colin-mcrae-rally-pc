// Serial port 0 with controllers attached (digital pad / DualShock protocol).
// Memory cards are serviced by the HLE BIOS, so card accesses here see no device.
#include "psx.h"

static uint16_t sio_mode, sio_ctrl, sio_baud = 0x88;
static uint8_t rx_data;
static bool rx_full, busy, ack_level, irq_flag;
static uint8_t tx_pending;

typedef struct {
    bool negcon;            // Namco NeGcon: twist steering, analog I/II/L
    bool analog;            // reports as 0x73 instead of 0x41
    bool config;            // in DualShock configuration mode
    uint8_t cmd;
    int pos;                // byte index within the current command
    uint8_t reply[8];
    int reply_len;
    uint8_t rumble_map[6];
    uint8_t motor_small, motor_large;
} Pad;
static Pad padsio[2];
static enum { DEV_NONE, DEV_PAD, DEV_IGNORE } device;
static int seq;

uint8_t pad_motor_small[2], pad_motor_large[2];

static void build_reply(Pad *p, int port) {
    PadState *s = &pads[port];
    uint16_t b = (uint16_t)~s->buttons;
    memset(p->reply, 0, sizeof p->reply);
    p->reply_len = 6;
    if (p->config) {
        switch (p->cmd) {
        case 0x45: { const uint8_t r[6] = {0x01, 0x02, p->analog, 0x02, 0x01, 0x00}; memcpy(p->reply, r, 6); break; }
        case 0x47: { const uint8_t r[6] = {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}; memcpy(p->reply, r, 6); break; }
        case 0x4D: memcpy(p->reply, p->rumble_map, 6); break;
        case 0x42: case 0x43:
            p->reply[0] = b & 0xFF; p->reply[1] = b >> 8;
            p->reply[2] = s->rx; p->reply[3] = s->ry; p->reply[4] = s->lx; p->reply[5] = s->ly;
            break;
        default: break;   // 0x44, 0x46, 0x4C answered byte by byte below
        }
        return;
    }
    p->reply[0] = b & 0xFF;
    p->reply[1] = b >> 8;
    if (p->negcon) {
        // NeGcon: Start/d-pad as usual, R = R1, A = Circle, B = Triangle; I/II/L are analog
        uint16_t nb = 0xFFFF;
        uint16_t pressed = s->buttons;
        if (pressed & PAD_START) nb &= ~(1u << 3);
        if (pressed & PAD_UP) nb &= ~(1u << 4);
        if (pressed & PAD_RIGHT) nb &= ~(1u << 5);
        if (pressed & PAD_DOWN) nb &= ~(1u << 6);
        if (pressed & PAD_LEFT) nb &= ~(1u << 7);
        if (pressed & PAD_R1) nb &= ~(1u << 11);
        if (pressed & PAD_TRIANGLE) nb &= ~(1u << 12);
        if (pressed & PAD_CIRCLE) nb &= ~(1u << 13);
        p->reply[0] = nb & 0xFF;
        p->reply[1] = nb >> 8;
        p->reply[2] = s->lx;                                   // twist
        p->reply[3] = s->negcon_i;                             // I
        p->reply[4] = s->negcon_ii;                            // II
        p->reply[5] = (pressed & PAD_L1) ? 0xFF : s->negcon_l; // L
        p->reply_len = 6;
        return;
    }
    if (p->analog) {
        p->reply[2] = s->rx; p->reply[3] = s->ry; p->reply[4] = s->lx; p->reply[5] = s->ly;
    } else {
        p->reply_len = 2;
    }
}

// One byte exchange with the pad; returns the reply byte and whether it ACKs.
static uint8_t pad_exchange(int port, uint8_t tx, bool *ack) {
    Pad *p = &padsio[port];
    *ack = true;
    if (seq == 1) {
        p->cmd = tx;
        p->pos = 0;
        bool valid = tx == 0x42 || (!p->negcon && (tx == 0x43 || (p->config && tx >= 0x40 && tx <= 0x4F)));
        if (!valid) { *ack = false; return 0xFF; }
        build_reply(p, port);
        if (p->config) return 0xF3;
        if (p->negcon) return 0x23;
        return p->analog ? 0x73 : 0x41;
    }
    if (seq == 2) return 0x5A;
    int i = seq - 3;
    uint8_t r = i < p->reply_len ? p->reply[i] : 0;
    switch (p->cmd) {
    case 0x42:
        if (!p->config) {
            for (int k = 0; k < 6; k++) {
                if (p->rumble_map[k] == 0x00 && i == k) pad_motor_small[port] = tx ? 0xFF : 0;
                if (p->rumble_map[k] == 0x01 && i == k) pad_motor_large[port] = tx;
            }
        }
        break;
    case 0x43:
        if (i == 0) p->config = tx == 1;
        break;
    case 0x44:
        if (p->config && i == 0) p->analog = tx == 1;
        break;
    case 0x46:
        if (i == 0) {
            static const uint8_t a[6] = {0, 0, 1, 2, 0, 10}, b[6] = {0, 0, 1, 1, 1, 20};
            memcpy(p->reply, tx ? b : a, 6);
        }
        r = i < 6 ? p->reply[i] : 0;
        break;
    case 0x4C:
        if (i == 0) { memset(p->reply, 0, 6); p->reply[3] = tx ? 7 : 4; }
        r = i < 6 ? p->reply[i] : 0;
        break;
    case 0x4D:
        if (i < 6) p->rumble_map[i] = tx;
        break;
    }
    int len = p->config ? 6 : p->reply_len;
    if (i >= len - 1) *ack = false;
    return r;
}

static void ack_release(void) { ack_level = false; }

static void transfer_done(void) {
    busy = false;
    rx_full = true;
    uint8_t tx = tx_pending;
    int port = (sio_ctrl >> 13) & 1;
    bool ack = false;
    uint8_t r = 0xFF;
    if (seq == 0) {
        if (tx == 0x01 && pads[port].connected) { device = DEV_PAD; ack = true; }
        else device = DEV_IGNORE;
    } else if (device == DEV_PAD) {
        r = pad_exchange(port, tx, &ack);
        if (!ack) device = DEV_IGNORE;
    }
    seq++;
    rx_data = r;
    if (ack) {
        ack_level = true;
        irq_flag = true;
        if (sio_ctrl & 0x1000) irq_raise(IRQ_SIO);
        sched_at(EV_SIO_ACK, cpu.cycles + 100, ack_release);
    }
}

static void ack_event(void) { transfer_done(); }

uint32_t sio_read(uint32_t addr, int size) {
    (void)size;
    switch (addr & 0xF) {
    case 0x0:
        if (!rx_full) return 0xFF;
        rx_full = false;
        return rx_data;
    case 0x4: {
        uint32_t s = 0x1;   // TX ready
        if (rx_full) s |= 0x2;
        if (!busy) s |= 0x4;
        if (ack_level) s |= 0x80;
        if (irq_flag) s |= 0x200;
        return s;
    }
    case 0x8: return sio_mode;
    case 0xA: return sio_ctrl;
    case 0xE: return sio_baud;
    }
    return 0;
}

void sio_write(uint32_t addr, uint32_t v, int size) {
    (void)size;
    switch (addr & 0xF) {
    case 0x0: {
        if (!(sio_ctrl & 1) || !(sio_ctrl & 2)) { rx_data = 0xFF; rx_full = true; break; }
        tx_pending = (uint8_t)v;
        busy = true;
        // 8 bits at the programmed baud rate, then the device's reply and /ACK
        uint32_t t = (uint32_t)(sio_baud ? sio_baud : 0x88) * 8 + 300;
        sched_at(EV_SIO, cpu.cycles + t, ack_event);
        break;
    }
    case 0x8: sio_mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) {   // reset
            rx_full = busy = ack_level = irq_flag = false;
            seq = 0;
            device = DEV_NONE;
        }
        if (v & 0x10) irq_flag = false;   // acknowledge
        if (!(v & 2)) { seq = 0; device = DEV_NONE; }   // deselect ends the transaction
        sio_ctrl = (uint16_t)v & ~0x50;
        break;
    case 0xE: sio_baud = (uint16_t)v; break;
    }
}



void sio_set_port(int port, bool negcon, bool analog) {
    padsio[port & 1].negcon = negcon;
    padsio[port & 1].analog = analog;
}
