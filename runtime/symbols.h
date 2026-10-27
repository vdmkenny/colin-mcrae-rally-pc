// Addresses in the game's executables. They hold for the NTSC-U release (SCUS-94474) only,
// which tools/build_game.py checks by hash; hooked functions are listed in tools/hooks.txt.
#pragma once

// ------------------------------------------------------------------- GAME.EXE
#define SCENE_ROOT      0x800c6da0u   // object tree: first child +8, next sibling +4
#define OBJ_LOCAL       0x48          // local transform: 3x3 matrix, then translation (16.16)
#define OBJ_FLAGS       0xba          // bit 0: world transform to be recomputed
#define NUM_CARS        0x800c6f9cu
#define CAR_SHADOW_IDX  0x800d68ccu   // byte per car, 0x64c apart: its shadow record
#define SHADOWS         0x800d5de0u   // shadow records, 0x88 bytes each
#define PAD_PRESSED     0x800cfea0u   // newly pressed buttons of 8 pad slots, 0x3c apart
#define OT_FAR          0x800b6524u   // far limit at stage load; sets the ordering table's depth shift
#define PRIM_CURSOR     0x800b6398u   // next free byte in the primitive buffer

// relative to gp
#define GP_STATE        0x3e0
#define GP_CONE         0xa2c   // view cone coefficient
#define GP_FOG          0xa48
#define GP_FAR          0xa4c   // far limit, quarter screen-depth units
#define GP_INTERVAL     0xa7c   // vblanks per frame
#define GP_PRIMBUF_A    0xa88
#define GP_PRIMBUF_B    0xa8c
#define GP_PRIMBUF_SIZE 0xa90

#define STATE_RACE      5u
#define STATE_REPLAY    8u   // stage results and the attract demo

// --------------------------------------------------------------- FRONTEND.EXE
#define GAME_INFO_PTR   0x80179f48u   // the player's progress, saved to the memory card
#define GAME_INFO_MAGIC 0x87651234u
#define GI_UNLOCKS      0x18
