// Extra cars from car folders: cars/<name>/ beside the program or in the save folder. A folder
// holds car.ini and any of the car files the disc has per car (DAY.BFX, NIGHT.BFX, MEDIUM.BFX,
// DAYTIM.BFX, NIGHTTIM.BFX, MEDIUM.TIM, SOUNDS.BFX). The game keeps using a disc car, the base,
// for handling and records; when a player has picked an extra car, the game's car folder for
// that player becomes CARS\EXTRA<n>\, whose files come from the car folder or else the base.
#include "psx.h"
#include "symbols.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const disc_dirs[NUM_DISC_CARS] = {
    "SUBARU", "SEAT", "SKODA", "QUATTRO", "RS200", "MK2", "MLANCER", "VWGOLF", "INTEGRAL", "TOYOTA", "COSWORTH", "MEGANE",
};
static const char *const car_files[] = {
    "DAY.BFX", "NIGHT.BFX", "MEDIUM.BFX", "DAYTIM.BFX", "NIGHTTIM.BFX", "MEDIUM.TIM", "SOUNDS.BFX",
};
#define NUM_FILES (int)(sizeof car_files / sizeof *car_files)

ExtraCar extra_cars[MAX_EXTRA_CARS];
int num_extra_cars;
int car_extra[2] = {-1, -1};   // extra car picked by player 1 and 2
bool extra_cars_enabled = true;

static uint8_t *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *size = (uint32_t)n;
    return d;
}

static void upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static bool read_ini(ExtraCar *car, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    car->base = -1;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;
        while (*v == ' ') v++;
        v[strcspn(v, "\r\n;#")] = 0;
        for (char *e = v + strlen(v); e > v && e[-1] == ' '; ) *--e = 0;
        if (!strcmp(k, "name")) { snprintf(car->name, sizeof car->name, "%s", v); upper(car->name); }
        else if (!strcmp(k, "short_name")) { snprintf(car->short_name, sizeof car->short_name, "%s", v); upper(car->short_name); }
        else if (!strcmp(k, "base")) {
            upper(v);
            for (int i = 0; i < NUM_DISC_CARS - 1; i++) if (!strcmp(v, disc_dirs[i])) car->base = i;
        }
        else if (!strcmp(k, "bhp")) car->bhp = atoi(v);
        else if (!strcmp(k, "wheelbase")) car->wheelbase = atoi(v);
        else if (!strcmp(k, "mass")) car->mass = atoi(v);
        else if (!strcmp(k, "wheel_drop")) car->wheel_drop = (float)atof(v);
        else if (!strcmp(k, "head_lamp") || !strcmp(k, "fog_lamp")) {
            float *l = car->lamps[k[0] == 'h' ? 0 : 1];
            sscanf(v, "%f %f %f", &l[0], &l[1], &l[2]);
        }
        else if (!strcmp(k, "tail_lamp")) {
            int n = 0;
            for (char *q = v; n < 18; n++) {
                char *e;
                float f = strtof(q, &e);
                if (e == q) break;
                car->tail[n / 3][n % 3] = f;
                q = e;
            }
            car->tail_points = n % 3 || n / 3 == 2 || n / 3 == 3 ? 0 : n / 3;
        }
        else if (!strcmp(k, "drive")) {
            upper(v);
            car->drive = !strcmp(v, "FWD") ? 1 : !strcmp(v, "RWD") ? 2 : 0;
        }
    }
    fclose(f);
    return true;
}

static void load_car(const char *dir, const char *folder) {
    if (num_extra_cars == MAX_EXTRA_CARS) return;
    ExtraCar *car = &extra_cars[num_extra_cars];
    memset(car, 0, sizeof *car);
    char path[1200];
    snprintf(path, sizeof path, "%s/car.ini", dir);
    if (!read_ini(car, path)) return;
    if (car->base < 0) { LOG("cars: %s: car.ini needs base = one of the disc cars\n", folder); return; }
    if (!car->name[0]) { snprintf(car->name, sizeof car->name, "%s", folder); upper(car->name); }
    if (!car->short_name[0]) snprintf(car->short_name, sizeof car->short_name, "%s", car->name);
    for (int i = 0; i < NUM_FILES; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, car_files[i]);
        uint32_t size;
        uint8_t *data = read_file(path, &size);
        if (!data) continue;
        car->size[i] = size;
        car->data[i] = data;
        car->lba[i] = cdrom_add_file(data, size);
    }
    LOG("cars: %s (%s, based on %s)\n", car->name, folder, disc_dirs[car->base]);
    num_extra_cars++;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void scan(const char *root) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%scars", root);
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    if (!names) return;
    qsort(names, (size_t)n, sizeof *names, by_name);
    for (int i = 0; i < n; i++) {
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", dir, names[i]);
        load_car(sub, names[i]);
    }
    SDL_free(names);
}

void cars_init(void) {
    const char *base = SDL_GetBasePath();
    if (base) scan(base);
    if (!base || strcmp(base, save_dir)) scan(save_dir);
}

static void psx_string(uint32_t a, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) if (!(out[i] = (char)RD8(a + (uint32_t)i))) break;
    out[i] = 0;
}

static void put_point(uint32_t a, const float *p, float z) {
    WR32(a, (uint32_t)(int32_t)(p[0] * 65536)); WR32(a + 4, (uint32_t)(int32_t)(p[1] * 65536));
    WR32(a + 8, (uint32_t)(int32_t)(z * 65536));
}

// The game keeps per car the name the stage screens show and where its lamps glow; for a race
// with an extra car its base car's entries take the extra car's.
static void patch_game_name(const ExtraCar *car) {
    uint32_t name = RD32(GAME_CAR_NAMES + (uint32_t)car->base * 4);
    size_t room = 0;
    while (room < 64 && RD8(name + (uint32_t)room)) room++;
    for (size_t i = 0; i < room; i++) WR8(name + (uint32_t)i, i < strlen(car->name) ? (uint8_t)car->name[i] : 0);
}

static void patch_game_lamps(const ExtraCar *car) {
    uint32_t b = (uint32_t)car->base;
    const float *head = car->lamps[0], *fog = car->lamps[1];
    if (head[0]) {
        uint32_t a = FRONT_LAMPS + b * 0x60;
        put_point(a, head, -head[2]); put_point(a + 12, head, head[2]);
        put_point(a + 24, fog, -fog[2]); put_point(a + 36, fog, fog[2]);
        WR8(FRONT_LAMPS_DAY + b, 2);
        WR8(FRONT_LAMPS_NIGHT + b, 2);
    }
    if (car->tail_points) {   // an outline is lit over the lamp; its middle is where it glows
        const uint32_t lists[3][2] = {{REAR_LAMPS_A, REAR_LAMPS_A_N}, {REAR_LAMPS_B, REAR_LAMPS_B_N},
                                      {REAR_LAMPS_C, REAR_LAMPS_C_N}};
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < car->tail_points; k++)
                put_point(lists[i][0] + b * 0x48 + (uint32_t)k * 12, car->tail[k], car->tail[k][2]);
            WR8(lists[i][1] + b, (uint8_t)car->tail_points);
        }
    }
}

// game: works out a player's car folder (a1) and DAY or NIGHT (a2), returns the car.
int hook_car_path(CPU *c, RecompFn orig) {
    uint32_t slot = c->r[4], dir = c->r[5];
    orig(c);
    if (slot > 1 || car_extra[slot] < 0) return 1;
    const ExtraCar *car = &extra_cars[car_extra[slot]];
    uint32_t players = RD32(GAME_RACE_INFO);
    if (c->r[2] != (uint32_t)car->base || (RD8(players + slot * 0x60 + RI_CAR) & 15) != (uint32_t)car->base) return 1;
    char s[32];
    snprintf(s, sizeof s, "CARS\\EXTRA%d\\", car_extra[slot]);
    for (size_t i = 0; i <= strlen(s); i++) WR8(dir + (uint32_t)i, (uint8_t)s[i]);
    patch_game_name(car);
    return 1;
}

// game: works out where a car's lamps glow from the per-car lamp tables (a0: its body).
int hook_lamps(CPU *c, RecompFn orig) {
    uint32_t body = c->r[4];
    int slot = (int)RD8(body + BODY_SLOT);
    if (slot <= 1 && car_extra[slot] >= 0 && (int)RD8(body + BODY_CAR) == extra_cars[car_extra[slot]].base)
        patch_game_lamps(&extra_cars[car_extra[slot]]);
    orig(c);
    return 1;
}

// game: sets up a car's physics (a0: its body, which holds the player slot and the car).
int hook_car_physics(CPU *c, RecompFn orig) {
    uint32_t body = c->r[4];
    orig(c);
    int slot = (int)RD8(body + BODY_SLOT);
    if (slot > 1 || car_extra[slot] < 0) return 1;
    const ExtraCar *car = &extra_cars[car_extra[slot]];
    if (car->mass <= 0 || (int)RD8(body + BODY_CAR) != car->base) return 1;
    uint32_t m = (uint32_t)car->mass << 16;
    WR32(body + BODY_MASS, m);
    WR32(body + BODY_MASS + 4, 0x7fffffffu / m << 1);   // its reciprocal
    return 1;
}

// game: looks up a file's position and size (a0: CdlFILE, a1: path) in the directory cache.
int hook_find_file(CPU *c, RecompFn orig) {
    char path[128];
    psx_string(c->r[5], path, sizeof path);
    const char *p = strstr(path, "\\CARS\\EXTRA");
    if (!p) return 0;
    int n = atoi(p + 11);
    const char *file = strchr(p + 11, '\\');
    if (n < 0 || n >= num_extra_cars || !file) return 0;
    const ExtraCar *car = &extra_cars[n];
    uint32_t lba = 0, size = 0;
    for (int i = 0; i < NUM_FILES; i++)
        if (car->lba[i] && !strncmp(file + 1, car_files[i], strlen(car_files[i]))) { lba = car->lba[i]; size = car->size[i]; }
    if (!lba) {
        char disc[128];
        snprintf(disc, sizeof disc, "\\CMRALLY\\CARS\\%s%s", disc_dirs[car->base], file);
        if (cdrom_find_file(disc, &lba, &size)) { LOG("cars: %s not found\n", disc); c->r[2] = 0; return 1; }
    }
    uint32_t a = lba + 150, f = c->r[4];
    WR8(f, (a / 4500 / 10) << 4 | a / 4500 % 10);
    WR8(f + 1, (a / 75 % 60 / 10) << 4 | a / 75 % 60 % 10);
    WR8(f + 2, (a % 75 / 10) << 4 | a % 75 % 10);
    WR32(f + 4, size);
    c->r[2] = 1;
    return 1;
}

// ------------------------------------------------------------------- car menu
// The car menu gets one more entry, for car 11 (the unused Megane slot); left and right on it
// step through the extra cars the mode allows (those whose base car it allows). The frontend
// stores the base car for the race.
#define MENU_CAR 11
#define FE_TEXT 0x807ff000u   // name strings, above the console's 2 MB
#define FN_CAR_ALLOWED 0x80196894u
static uint32_t p1_menu;      // the car screen's menu (player 1's car is item 2)
static int extra_entry = -1;  // menu entry of the extra cars
static int shown[2];          // extra car on show for each player

static uint32_t menu_item(uint32_t menu, int id) {
    for (int i = 0; i < (int)RD32(menu + MENU_ITEMS); i++)
        if ((int)RD32(menu + 0x4c + i * 0x34 + ITEM_ID) == id) return menu + 0x4c + i * 0x34;
    return 0;
}

// Runs the frontend's check for whether the mode allows a disc car.
static bool allowed(CPU *c, int car) {
    uint32_t a0 = c->r[4], v0 = c->r[2], ra = c->r[31];
    c->r[4] = (uint32_t)car;
    dispatch_call(c, FN_CAR_ALLOWED);
    bool ok = (c->r[2] & 0xff) != 0;
    c->r[4] = a0; c->r[2] = v0; c->r[31] = ra;
    return ok;
}

// The next extra car the mode allows from `from` in direction `dir`, or -1.
static int step(CPU *c, int from, int dir) {
    for (int i = from; i >= 0 && i < num_extra_cars; i += dir)
        if (allowed(c, extra_cars[i].base)) return i;
    return -1;
}

static void put_text(uint32_t a, const char *s) {
    for (size_t i = 0; i <= strlen(s); i++) WR8(a + (uint32_t)i, (uint8_t)s[i]);
}

// Writes the names and specs of extra car k into car `slot`'s table entries (text: string slot).
static void fill(int slot, int k, int text_slot) {
    const ExtraCar *car = &extra_cars[k];
    uint32_t text = FE_TEXT + (uint32_t)text_slot * 0x40;
    put_text(text, car->name);
    put_text(text + 0x20, car->short_name);
    WR32(CAR_NAMES + (uint32_t)slot * 4, text);
    WR32(CAR_SHORT_NAMES + (uint32_t)slot * 4, text + 0x20);
    WR32(CAR_SPECS + (uint32_t)slot * 12, (uint32_t)car->bhp);
    WR32(CAR_SPECS + (uint32_t)slot * 12 + 4, (uint32_t)car->wheelbase);
    WR32(CAR_SPECS + (uint32_t)slot * 12 + 8, (uint32_t)car->drive);
}

// The showroom: an extra car's MEDIUM model is loaded as one more car object, its MEDIUM.TIM
// block going where the game keeps it in a race, (832, 0), which the frontend leaves free.
#define SHOWROOM_DATA 0x80700000u   // model data, which the objects use in place
static int uploaded = -1;            // extra car whose texture block is in VRAM

static uint32_t call(CPU *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
    dispatch_call(c, fn);
    return c->r[2];
}

static void hide_showrooms(void) {
    for (int i = 0; i < num_extra_cars; i++)
        if (extra_cars[i].showroom) WR8(extra_cars[i].showroom + NODE_VISIBLE, 0);
}

static void upload_showroom(int k) {
    const ExtraCar *car = &extra_cars[k];
    if (uploaded == k) return;
    uploaded = k;
    const uint8_t *tim = car->data[5];
    uint16_t x, y, w, h;
    memcpy(&x, tim + 12, 2); memcpy(&y, tim + 14, 2); memcpy(&w, tim + 16, 2); memcpy(&h, tim + 18, 2);
    gpu_gp0(0xA0000000u);
    gpu_gp0((uint32_t)y << 16 | x);
    gpu_gp0((uint32_t)h << 16 | w);
    for (uint32_t i = 0; i < (uint32_t)w * h; i += 2) {
        uint32_t v;
        memcpy(&v, tim + 20 + i * 2, 4);
        gpu_gp0(v);
    }
}

// After the car menu the frontend's screens show the stored car, the base: while player 1 has
// an extra car, the base car's names, specs and showroom model are the extra car's.
static int patched = -1;            // base car whose entries are an extra car's
static uint32_t patch_saved[6];

static void unpatch_base(void) {
    if (patched < 0) return;
    uint32_t b = (uint32_t)patched;
    WR32(CAR_NAMES + b * 4, patch_saved[0]); WR32(CAR_SHORT_NAMES + b * 4, patch_saved[1]);
    WR32(CAR_SPECS + b * 12, patch_saved[2]); WR32(CAR_SPECS + b * 12 + 4, patch_saved[3]);
    WR32(CAR_SPECS + b * 12 + 8, patch_saved[4]);
    for (int i = 0; i < num_extra_cars; i++)
        if (extra_cars[i].showroom && RD32(CAR_MODELS + b * 8) == extra_cars[i].showroom)
            WR32(CAR_MODELS + b * 8, patch_saved[5]);
    patched = -1;
}

static void patch_base(void) {
    unpatch_base();
    int k = car_extra[0];
    if (k < 0) return;
    uint32_t b = (uint32_t)extra_cars[k].base;
    if ((int)(RD32(RD32(GAME_INFO_PTR) + GI_CAR_P1) >> 16 & 15) != (int)b) return;
    patched = (int)b;
    patch_saved[0] = RD32(CAR_NAMES + b * 4); patch_saved[1] = RD32(CAR_SHORT_NAMES + b * 4);
    patch_saved[2] = RD32(CAR_SPECS + b * 12); patch_saved[3] = RD32(CAR_SPECS + b * 12 + 4);
    patch_saved[4] = RD32(CAR_SPECS + b * 12 + 8); patch_saved[5] = RD32(CAR_MODELS + b * 8);
    fill((int)b, k, 2);
    if (extra_cars[k].showroom) {
        upload_showroom(k);
        WR32(CAR_MODELS + b * 8, extra_cars[k].showroom);
    }
}

// frontend: sets up the car menus (a0: the car screen's menu).
int hook_car_menu_init(CPU *c, RecompFn orig) {
    unpatch_base();
    p1_menu = c->r[4];
    orig(c);
    extra_entry = -1;
    uploaded = -1;   // other screens may have used the area
    int n = (int)RD32(CAR_MENU_COUNT);
    if (!num_extra_cars || n >= 12 || step(c, 0, 1) < 0) return 1;
    extra_entry = n;
    WR8(CAR_MENU_CARS + (uint32_t)n, MENU_CAR);
    WR32(CAR_MENU_COUNT, (uint32_t)n + 1);
    uint32_t info = RD32(GAME_INFO_PTR);
    const uint32_t items[3] = {menu_item(p1_menu, 2), menu_item(p1_menu, 3), menu_item(CAR_MENU_P2, 3)};
    for (int i = 0; i < 3; i++) {
        uint32_t item = items[i];
        int p = i > 0;
        if (!item || (int)RD32(item + ITEM_COUNT) != n) continue;
        WR32(item + ITEM_COUNT, (uint32_t)n + 1);
        shown[p] = step(c, 0, 1);
        int car = car_extra[p];
        if (car >= 0 && (int)(RD32(info + (p ? GI_CAR_P2 : GI_CAR_P1)) >> 16 & 15) == extra_cars[car].base &&
            allowed(c, extra_cars[car].base)) {
            shown[p] = car;
            WR32(item + ITEM_SEL, (uint32_t)extra_entry);
        }
    }
    return 1;
}

// frontend: a menu's input (a0: menu, a1: buttons). On the extra cars' entry, left and right
// step through the extra cars before leaving it.
int hook_menu_input(CPU *c, RecompFn orig) {
    uint32_t menu = c->r[4], buttons = c->r[5];
    if (extra_entry < 0 || (menu != p1_menu && menu != CAR_MENU_P2)) return 0;
    uint32_t item = menu + 0x4c + RD32(menu + MENU_ITEM) * 0x34;
    int id = (int)RD32(item + ITEM_ID);
    int player = id == 2 && menu == p1_menu ? 0 : id == 3 ? 1 : -1;
    if (player < 0) return 0;
    int dir = buttons & 0x20 ? 1 : buttons & 0x80 ? -1 : 0;   // right, left
    int before = (int)RD32(item + ITEM_SEL);
    if (before == extra_entry && dir) {
        int next = step(c, shown[player] + dir, dir);
        if (next >= 0) {
            shown[player] = next;
            c->r[5] = buttons & ~0xa0u;
        }
    }
    orig(c);
    if (before != extra_entry && (int)RD32(item + ITEM_SEL) == extra_entry && dir) {
        shown[player] = dir > 0 ? step(c, 0, 1) : step(c, num_extra_cars - 1, -1);
    }
    return 1;
}

// frontend: whether the mode allows a car (a0). Car 11 is the extra cars' entry.
int hook_car_allowed(CPU *c, RecompFn orig) {
    if (c->r[4] != MENU_CAR || extra_entry < 0) return 0;
    c->r[2] = step(c, 0, 1) >= 0;
    return 1;
}

// frontend: makes the showroom objects of the 12 disc cars the first time the car screen opens.
int hook_showrooms(CPU *c, RecompFn orig) {
    bool first = !RD8(SHOWROOMS_MADE);
    orig(c);
    if (!first) return 1;
    uploaded = -1;
    uint32_t data = SHOWROOM_DATA;
    for (int i = 0; i < num_extra_cars; i++) {
        ExtraCar *car = &extra_cars[i];
        car->showroom = 0;
        const uint8_t *bfx = car->data[2], *tim = car->data[5];
        uint32_t size;
        if (!bfx || !tim || car->size[5] < 20 + 64 * 255 * 2) continue;
        memcpy(&size, bfx, 4);
        for (uint32_t k = 0; k < size; k++) WR8(data + k, bfx[4 + k]);
        uint32_t obj = call(c, FN_NEW_OBJECT, RD32(SHOWROOM_SCENE), 0, 0);
        // the showroom has no physics to lower the wheels to the ground (see wheel_drop)
        uint32_t nodes = data + 16 + 4 * RD16(data);
        for (uint32_t k = 0; k < RD16(data + 2); k++) {
            uint32_t nd = nodes + k * 0xbc, id = RD32(nd + 0x14) & 0xfff;
            for (uint32_t t = 0x40; id >= 1 && id <= 4 && t <= 0x70; t += 0x30)   // base, local y
                WR32(nd + t, RD32(nd + t) - (uint32_t)(int32_t)(car->wheel_drop * 65536));
        }
        call(c, FN_LOAD_MODEL, data, obj, 0);
        call(c, FN_PLACE_OBJECT, obj, SHOWROOM_POS, SHOWROOM_ROT);
        uint32_t arches = call(c, FN_FIND_NODE, obj, 6, 0);   // drawn behind, as the game does
        if (arches && !RD8(arches + 0xb4) && RD32(arches + 0x10))
            WR16(RD32(arches + 0x10) + 0x4a, RD16(RD32(arches + 0x10) + 0x4a) + 2);
        WR8(obj + NODE_VISIBLE, 0);
        car->showroom = obj;
        data += (size + 0xfff) & ~0xfffu;
    }
    patched = -1;   // a new frontend: its tables are the disc's
    patch_base();
    return 1;
}

// frontend: hides the showroom objects when leaving the car screen.
int hook_hide_showrooms(CPU *c, RecompFn orig) {
    orig(c);
    hide_showrooms();
    return 1;
}

// The car menu rows check the mode's rules themselves, so the extra cars' entry is drawn as its
// base car, with the extra car's names, specs and model in the base car's entries meanwhile.
static int car_row(CPU *c, RecompFn orig, int player, uint32_t item) {
    if (c->r[6] & 0xff) hide_showrooms();   // the selected row draws the car
    if (extra_entry < 0 || !item || (int)RD32(item + ITEM_SEL) != extra_entry) return 0;
    uint32_t b = (uint32_t)extra_cars[shown[player]].base;
    uint32_t saved[6] = {
        RD32(CAR_NAMES + b * 4), RD32(CAR_SHORT_NAMES + b * 4), RD32(CAR_SPECS + b * 12),
        RD32(CAR_SPECS + b * 12 + 4), RD32(CAR_SPECS + b * 12 + 8), RD32(CAR_MODELS + b * 8),
    };
    fill((int)b, shown[player], player);
    if (extra_cars[shown[player]].showroom) {   // else the base car's model shows
        upload_showroom(shown[player]);
        WR32(CAR_MODELS + b * 8, extra_cars[shown[player]].showroom);
    }
    WR8(CAR_MENU_CARS + (uint32_t)extra_entry, b);
    orig(c);
    WR8(CAR_MENU_CARS + (uint32_t)extra_entry, MENU_CAR);
    WR32(CAR_NAMES + b * 4, saved[0]); WR32(CAR_SHORT_NAMES + b * 4, saved[1]);
    WR32(CAR_SPECS + b * 12, saved[2]); WR32(CAR_SPECS + b * 12 + 4, saved[3]);
    WR32(CAR_SPECS + b * 12 + 8, saved[4]); WR32(CAR_MODELS + b * 8, saved[5]);
    return 1;
}

// frontend: draws player 1's or 2's car or gear menu row (a0: menu, a1: item, a2: selected);
// the gear row shows the car the car row last showed.
int hook_car_row_p1(CPU *c, RecompFn orig) { return car_row(c, orig, 0, c->r[5]); }
int hook_car_row_p2(CPU *c, RecompFn orig) { return car_row(c, orig, 1, c->r[5]); }
int hook_gear_row_p1(CPU *c, RecompFn orig) { return car_row(c, orig, 0, menu_item(c->r[4], 2)); }
int hook_gear_row_p2(CPU *c, RecompFn orig) { return car_row(c, orig, 1, menu_item(c->r[4], 3)); }

// frontend: stores the car screen's choices. An extra car is stored as its base car.
int hook_car_commit(CPU *c, RecompFn orig) {
    orig(c);
    uint32_t info = RD32(GAME_INFO_PTR);
    for (int p = 0; p < 2; p++) {
        uint32_t a = info + (p ? GI_CAR_P2 : GI_CAR_P1), v = RD32(a);
        bool used = !p || (RD32(info + GI_PLAYERS) & 0x30000);
        car_extra[p] = -1;
        if (!used || (v >> 16 & 15) != MENU_CAR || extra_entry < 0) continue;
        car_extra[p] = shown[p];
        WR32(a, (v & ~0xf0000u) | (uint32_t)extra_cars[shown[p]].base << 16);
    }
    hide_showrooms();
    patch_base();
    return 1;
}
