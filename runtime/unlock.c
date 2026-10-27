// Unlock all. The frontend keeps the player's progress in its GAME_INFO block, whose unlock word
// holds: bits 0-3 novice countries, 4-7 countries (8 = all), 8 expert mode, 9-11 super-specials
// (5 = all), 12-15 one bit per bonus car, and the cheat toggles above. While the frontend runs
// it sees everything unlocked, but the player's own word is what gets loaded, merged and saved,
// so turning the option off locks everything again.
#include "psx.h"
#include "symbols.h"
#include <string.h>

bool unlock_all = false;
#define ALL_UNLOCKED 0xfb80u   // cars 0xf000, super-specials 5 << 9, expert 0x100, countries 8 << 4
static uint32_t info_addr;     // the block while unlocks are applied (0: not applied)
static uint32_t own_unlocks;   // the player's own word
static uint32_t set_unlocks;   // what was written in its place

static bool in_frontend(void) {
    const Module *m = module_active();
    return m && !strcmp(m->name, "frontend");
}

static uint32_t game_info(void) {
    uint32_t p = RD32(GAME_INFO_PTR);
    return (p >> 24) == 0x80 && RD32(p) == GAME_INFO_MAGIC ? p : 0;
}

static void apply_unlocks(void) {
    set_unlocks = (own_unlocks & 0xffff000fu) | ALL_UNLOCKED;
    WR32(info_addr + GI_UNLOCKS, set_unlocks);
}

// Takes in what the game changed in the word since it was last set.
static void update_own(uint32_t cur) {
    own_unlocks = (own_unlocks & ~(cur ^ set_unlocks)) | (cur & (cur ^ set_unlocks));
}

void unlock_all_vblank(void) {
    uint32_t p = unlock_all && in_frontend() ? game_info() : 0;
    if (info_addr && p != info_addr) {
        if (RD32(info_addr) == GAME_INFO_MAGIC && RD32(info_addr + GI_UNLOCKS) == set_unlocks)
            WR32(info_addr + GI_UNLOCKS, own_unlocks);
        info_addr = 0;
    }
    if (!p) return;
    uint32_t cur = RD32(p + GI_UNLOCKS);
    if (!info_addr) own_unlocks = cur;
    else update_own(cur);
    info_addr = p;
    apply_unlocks();
}

// Runs the hooked function with the player's own word in place; what it leaves there is theirs.
static int with_own_unlocks(CPU *c, RecompFn orig) {
    if (!info_addr || game_info() != info_addr) return 0;
    update_own(RD32(info_addr + GI_UNLOCKS));
    WR32(info_addr + GI_UNLOCKS, own_unlocks);
    orig(c);
    own_unlocks = RD32(info_addr + GI_UNLOCKS);
    apply_unlocks();
    return 1;
}

// frontend: loads GAME_INFO from the memory card, keeping unlocks already in memory.
int hook_load_progress(CPU *c, RecompFn orig) { return with_own_unlocks(c, orig); }
// frontend: merges the unlocks recorded in the championship saves.
int hook_merge_unlocks(CPU *c, RecompFn orig) { return with_own_unlocks(c, orig); }
// frontend: copies the progress into the championship being started (and so into its save).
int hook_copy_progress(CPU *c, RecompFn orig) { return with_own_unlocks(c, orig); }

// The game's card writes complete frames after it asks for them, so GAME_INFO is swapped
// around the BIOS write itself.
void unlock_all_card_write(uint32_t src, uint32_t len, bool before) {
    if (!info_addr || info_addr + GI_UNLOCKS < src || info_addr + GI_UNLOCKS + 4 > src + len) return;
    if (before) {
        update_own(RD32(info_addr + GI_UNLOCKS));
        WR32(info_addr + GI_UNLOCKS, own_unlocks);
    } else {
        apply_unlocks();
    }
}
