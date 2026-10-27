// MDEC macroblock decoder (used for STR full-motion video).
#include "psx.h"
#include <stdlib.h>

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static uint8_t iq_y[64], iq_uv[64];
static int16_t scale[64];
static uint32_t control;

static uint32_t cur_cmd;
static uint32_t *in_buf;
static uint32_t in_len, in_need, in_cap;
static uint32_t *out_buf;
static uint32_t out_len, out_pos, out_cap;

void mdec_init(void) {
    control = 0;
    in_len = in_need = 0;
    out_len = out_pos = 0;
}

static void out_push(uint32_t w) {
    if (out_len >= out_cap) {
        out_cap = out_cap ? out_cap * 2 : 65536;
        out_buf = realloc(out_buf, out_cap * 4);
    }
    out_buf[out_len++] = w;
}

static inline int sext10(int v) { return (v & 0x200) ? v - 0x400 : v; }

// Run-length decode + dequantise one 8x8 block; returns false at end of data.
static bool rl_block(int16_t *blk, const uint16_t **src, const uint16_t *end, const uint8_t *qt) {
    memset(blk, 0, 64 * sizeof(int16_t));
    const uint16_t *p = *src;
    uint16_t n;
    do {
        if (p >= end) return false;
        n = *p++;
    } while (n == 0xFE00);
    int k = 0;
    int q = (n >> 10) & 0x3F;
    int32_t val = sext10(n & 0x3FF) * qt[0];
    int32_t coef[64] = {0};
    for (;;) {
        if (q == 0) val = sext10(n & 0x3FF) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q > 0) coef[zigzag[k]] = val;
        else coef[k] = val;
        if (p >= end) break;
        n = *p++;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (sext10(n & 0x3FF) * qt[k] * q + 4) / 8;
    }
    *src = p;
    // Two-pass IDCT using the game's scale table (1.15 fixed point).
    int64_t tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)coef[y + z * 8] * scale[x + z * 8];
            tmp[x + y * 8] = sum;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += tmp[y + z * 8] * scale[x + z * 8];
            int64_t v = (sum + ((int64_t)1 << 31)) >> 32;
            blk[x + y * 8] = (int16_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    return true;
}

// Decode command: bits 27-28 output depth (0=4, 1=8, 2=24, 3=15 bit), 26 signed, 25 bit15.
static void emit_macroblock(int16_t *cr, int16_t *cb, int16_t y[4][64]) {
    int depth = (cur_cmd >> 27) & 3;
    bool is_signed = (cur_cmd >> 26) & 1;
    uint32_t bit15 = ((cur_cmd >> 25) & 1) << 15;
    uint8_t px[16 * 16][3];
    for (int b = 0; b < 4; b++) {
        int xx = (b & 1) * 8, yy = (b >> 1) * 8;
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++) {
                int R = cr[((i + xx) >> 1) + ((j + yy) >> 1) * 8];
                int B = cb[((i + xx) >> 1) + ((j + yy) >> 1) * 8];
                int G = (-22520 * B - 46811 * R) >> 16;   // -0.3437 B - 0.7143 R
                R = (91881 * R) >> 16;                    // 1.402
                B = (116130 * B) >> 16;                   // 1.772
                int Y = y[b][i + j * 8];
                int r = Y + R, g = Y + G, bb = Y + B;
                r = r < -128 ? -128 : r > 127 ? 127 : r;
                g = g < -128 ? -128 : g > 127 ? 127 : g;
                bb = bb < -128 ? -128 : bb > 127 ? 127 : bb;
                if (!is_signed) { r += 128; g += 128; bb += 128; }
                uint8_t *o = px[(i + xx) + (j + yy) * 16];
                o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)bb;
            }
    }
    if (depth == 2) {   // 24-bit
        uint8_t bytes[768];
        for (int i = 0; i < 256; i++) memcpy(bytes + i * 3, px[i], 3);
        for (int i = 0; i < 192; i++) { uint32_t w; memcpy(&w, bytes + i * 4, 4); out_push(w); }
    } else {   // 15-bit
        for (int i = 0; i < 256; i += 2) {
            uint32_t a = (px[i][0] >> 3) | ((px[i][1] >> 3) << 5) | ((px[i][2] >> 3) << 10) | bit15;
            uint32_t b = (px[i + 1][0] >> 3) | ((px[i + 1][1] >> 3) << 5) | ((px[i + 1][2] >> 3) << 10) | bit15;
            out_push(a | (b << 16));
        }
    }
}

static void emit_mono(int16_t *blk) {
    int depth = (cur_cmd >> 27) & 3;
    bool is_signed = (cur_cmd >> 26) & 1;
    uint8_t v[64];
    for (int i = 0; i < 64; i++) v[i] = (uint8_t)(blk[i] + (is_signed ? 0 : 128));
    if (depth == 1) {
        for (int i = 0; i < 64; i += 4) out_push(v[i] | (v[i + 1] << 8) | (v[i + 2] << 16) | ((uint32_t)v[i + 3] << 24));
    } else {
        for (int i = 0; i < 64; i += 8) {
            uint32_t w = 0;
            for (int k = 0; k < 8; k++) w |= (uint32_t)(v[i + k] >> 4) << (k * 4);
            out_push(w);
        }
    }
}

static void execute(void) {
    uint32_t op = cur_cmd >> 29;
    DBG(3, "[mdec] cmd %08x with %u words\n", cur_cmd, in_len);
    if (op == 1) {
        if (out_pos >= out_len) out_len = out_pos = 0;
        const uint16_t *p = (const uint16_t *)in_buf;
        const uint16_t *end = p + in_len * 2;
        int depth = (cur_cmd >> 27) & 3;
        int16_t cr[64], cb[64], y[4][64];
        while (p < end) {
            if (depth >= 2) {
                if (!rl_block(cr, &p, end, iq_uv) || !rl_block(cb, &p, end, iq_uv)) break;
                bool ok = true;
                for (int b = 0; b < 4 && ok; b++) ok = rl_block(y[b], &p, end, iq_y);
                if (!ok) break;
                emit_macroblock(cr, cb, y);
            } else {
                if (!rl_block(y[0], &p, end, iq_y)) break;
                emit_mono(y[0]);
            }
        }
    } else if (op == 2) {
        memcpy(iq_y, in_buf, 64);
        if (cur_cmd & 1) memcpy(iq_uv, (uint8_t *)in_buf + 64, 64);
    } else if (op == 3) {
        memcpy(scale, in_buf, 128);
    }
    in_len = in_need = 0;
}

static void input_word(uint32_t w) {
    if (in_need == 0) {
        cur_cmd = w;
        uint32_t op = w >> 29;
        if (op == 1) in_need = w & 0xFFFF;
        else if (op == 2) in_need = (w & 1) ? 32 : 16;
        else if (op == 3) in_need = 32;
        else in_need = 0;
        in_len = 0;
        if (in_need == 0 && op == 1) execute();
        return;
    }
    if (in_len >= in_cap) {
        in_cap = in_cap ? in_cap * 2 : 65536;
        in_buf = realloc(in_buf, in_cap * 4);
    }
    in_buf[in_len++] = w;
    if (in_len >= in_need) execute();
}

void mdec_write(int reg, uint32_t v) {
    if (reg == 0) { input_word(v); return; }
    if (v & 0x80000000u) { in_len = in_need = 0; out_len = out_pos = 0; cur_cmd = 0; }
    control = v;
}

uint32_t mdec_read(int reg) {
    if (reg == 0) return out_pos < out_len ? out_buf[out_pos++] : 0;
    uint32_t s = 0;
    if (out_pos >= out_len) s |= 1u << 31;
    if (in_need) s |= 1u << 29;
    if ((control & (1u << 30)) && in_need) s |= 1u << 28;
    if ((control & (1u << 29)) && out_pos < out_len) s |= 1u << 27;
    s |= ((cur_cmd >> 25) & 0xF) << 23;
    s |= (in_need > in_len ? (in_need - in_len - 1) : 0xFFFF) & 0xFFFF;
    return s;
}

int mdec_activity;   // words fed to the decoder (video playing)

void mdec_dma_in(uint32_t addr, uint32_t words) {
    mdec_activity += (int)words;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w;
        memcpy(&w, psx_ram + ((addr + i * 4) & RAM_MASK & ~3u), 4);
        input_word(w);
    }
}

void mdec_dma_out(uint32_t addr, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = out_pos < out_len ? out_buf[out_pos++] : 0;
        memcpy(psx_ram + ((addr + i * 4) & RAM_MASK & ~3u), &w, 4);
    }
}
