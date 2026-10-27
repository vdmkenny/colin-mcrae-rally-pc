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

#define BODY_MASS       0x48          // car physics body: mass (kg, 16.16), then its reciprocal
#define BODY_SLOT       0x624
#define BODY_CAR        0x625
#define GAME_CAR_NAMES  0x800bd05cu   // string pointer per car
#define FRONT_LAMPS     0x800bf5a4u   // per car 8 points (16.16 m, 12 bytes): glows, breakable
#define FRONT_LAMPS_DAY 0x800bfa24u   // byte per car: how many are lit always
#define FRONT_LAMPS_NIGHT 0x800bfa30u // byte per car: how many more at night
#define REAR_LAMPS_A    0x800beb60u   // rear lamps: per car up to 6 points (0x48 bytes) and a
#define REAR_LAMPS_A_N  0x800beec0u   //   count; 4-6 points outline the left lamp, which is drawn
#define REAR_LAMPS_B    0x800beeccu   //   lit over it (the right mirrored), and glows at their middle
#define REAR_LAMPS_B_N  0x800bf22cu
#define REAR_LAMPS_C    0x800bf238u
#define REAR_LAMPS_C_N  0x800bf598u
#define GAME_RACE_INFO  0x800c6db8u   // -> the frontend's choices: 0x60 bytes per player from +0x18
#define RI_CAR          0x76          // low 4 bits: the player's car

// --------------------------------------------------------------- FRONTEND.EXE
#define GAME_INFO_PTR   0x80179f48u   // the player's progress, saved to the memory card
#define GAME_INFO_MAGIC 0x87651234u
#define GI_UNLOCKS      0x18
#define GI_CAR_P1       0x7c          // bits 16-19: player 1's car
#define GI_CAR_P2       0xdc          // bits 16-19: player 2's car
#define GI_PLAYERS      0xf0          // bits 16-17: 0 for one player

#define CAR_NAMES       0x801ccd90u   // 12 string pointers per table
#define CAR_SHORT_NAMES 0x801ccdc0u
#define CAR_SPECS       0x801cd3e0u   // per car: bhp, wheelbase (mm), drive (0 4WD, 1 FWD, 2 RWD)
#define CAR_MODELS      0x80132118u   // per car: showroom object, model data
#define SHOWROOMS_MADE  0x801d07ccu   // byte: the car objects exist
#define SHOWROOM_SCENE  0x801d087cu   // -> the scene car objects are made in
#define SHOWROOM_POS    0x801cd3d4u
#define SHOWROOM_ROT    0x801d07c4u
#define FN_NEW_OBJECT   0x801990bcu   // (scene) -> object
#define FN_LOAD_MODEL   0x801ac4bcu   // (model data, object, 0)
#define FN_PLACE_OBJECT 0x80198cd0u   // (object, position, rotation)
#define FN_FIND_NODE    0x80199e60u   // (object, id) -> node
#define NODE_VISIBLE    0xb5          // object: per-viewport visibility mask
#define CAR_MENU_CARS   0x801cd470u   // byte per car menu entry (12): the car it shows
#define CAR_MENU_COUNT  0x801d07d8u
#define CAR_MENU_P2     0x801e3708u   // player 2's car menu; player 1's is on the car screen
#define MENU_ITEMS      0x40          // menu: item count, current item, items of 0x34 bytes from 0x4c
#define MENU_ITEM       0x44
#define ITEM_ID         0x0c
#define ITEM_COUNT      0x18
#define ITEM_SEL        0x1c
