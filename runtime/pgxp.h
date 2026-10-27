// Precise vertex tracking between the GTE and the GPU (see pgxp.c).
#pragma once
#include "recomp.h"
#include <stdbool.h>

#define PGXP_NO_ADDR 0xFFFFFFFFu

extern bool pgxp_enabled;
extern float pgxp_sxy[3][3];      // x, y, depth for SXY0..2
extern bool pgxp_sxy_valid[3];
extern uint32_t pgxp_sxy_key[3];

void pgxp_swc2(uint32_t addr, int reg);
void pgxp_mfc2(int rt, int reg, uint32_t value);
void pgxp_sw(uint32_t addr, int rt, uint32_t value);
bool pgxp_lookup(uint32_t addr, uint32_t value, float *x, float *y, float *w, uint32_t *key);
extern uint32_t pgxp_vsrc[3];
uint32_t pgxp_vertex_key(int v, int16_t x, int16_t y, int16_t z);
void pgxp_new_frame(void);
