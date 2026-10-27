// SDL3 host: window, presentation, audio, input and frame pacing.
#include "psx.h"
#include "pgxp.h"
#include "render.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <sys/stat.h>

PadState pads[2];
bool pad_analog_mode = false;
void sio_set_port(int port, bool negcon, bool analog);
static bool negcon_mode;
enum { CTRL_AUTO, CTRL_DIGITAL, CTRL_DUALSHOCK, CTRL_NEGCON };
static int controller_type = CTRL_AUTO;
static const char *controller_names[] = {"auto", "digital", "dualshock", "negcon"};
char save_dir[1024] = ".";

extern bool gpu_dither_enabled;
int spu_take(int16_t *dst, int frames);
void trace_dump(void);

static SDL_Window *window;
static SDL_AudioStream *audio;
static SDL_Gamepad *gamepads[2];
static uint32_t frame_rgba[640 * 512];

static uint64_t next_vblank;
static uint64_t pace_ns;   // target time of the end of the previous vblank
// Length of one emulated vblank in real time: 59.94 Hz, or exactly the display's rate when
// buffer swaps pace emulation (so the timer never fights vsync).
static uint64_t vblank_ns = 1000000000ull * 1001 / 60000;
static bool fast_forward;
static bool vsync = true;
static bool vsync_paces;     // the display runs at ~60 Hz, so buffer swaps pace emulation

static struct {
    const char *cue;
    bool fullscreen;
    int scale;
    int exit_after;          // testing: quit after this many frames
    int shot_every;          // testing: save a screenshot every N frames
    const char *shot_dir;
    const char *script;      // testing: scripted input "frame:buttons,..."
    bool turbo;              // testing: no frame pacing
} opts = {NULL, false, 3, 0, 0, ".", NULL, false};

static int frame_count;

static uint16_t script_buttons(int frame, PadState *pad) {
    // Format: "<start>-<end>:<hex buttons>[:<lx hex>]...;" e.g. "300-310:8;600-605:4000:20"
    if (!opts.script) return 0;
    uint16_t b = 0;
    const char *p = opts.script;
    while (*p) {
        int a = 0, e = 0;
        unsigned m = 0, lx = 0x80, ni = 0;
        int n = sscanf(p, "%d-%d:%x:%x:%x", &a, &e, &m, &lx, &ni);
        if (n >= 3 && frame >= a && frame <= e) {
            b |= (uint16_t)m;
            if (n >= 4) pad->lx = (uint8_t)lx;
            if (n >= 5) pad->negcon_i = (uint8_t)ni;
        }
        p = strchr(p, ';');
        if (!p) break;
        p++;
    }
    return b;
}

static void save_screenshot(void) {
    DisplayInfo d;
    gpu_display_info(&d);
    uint32_t *hi = NULL;
    int w, h, stride;
    SDL_Surface *s;
    if (render_read_display(&d, &hi, &w, &h)) {
        s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
        for (int y = 0; y < h; y++)
            memcpy((uint8_t *)s->pixels + (size_t)y * s->pitch, hi + (size_t)y * w, (size_t)w * 4);
        free(hi);
    } else {
        gpu_display(frame_rgba, &w, &h, &stride);
        s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
        for (int y = 0; y < h; y++) memcpy((uint8_t *)s->pixels + (size_t)y * s->pitch, frame_rgba + y * stride, (size_t)w * 4);
    }
    char path[1200];
    snprintf(path, sizeof path, "%s/frame%05d.bmp", opts.shot_dir, frame_count);
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

// ------------------------------------------------------------------- input
static void poll_keyboard(PadState *p) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= PAD_RIGHT;
    if (k[SDL_SCANCODE_X]) b |= PAD_CROSS;
    if (k[SDL_SCANCODE_C]) b |= PAD_CIRCLE;
    if (k[SDL_SCANCODE_Z]) b |= PAD_SQUARE;
    if (k[SDL_SCANCODE_S]) b |= PAD_TRIANGLE;
    if (k[SDL_SCANCODE_Q]) b |= PAD_L1;
    if (k[SDL_SCANCODE_W]) b |= PAD_R1;
    if (k[SDL_SCANCODE_A]) b |= PAD_L2;
    if (k[SDL_SCANCODE_D]) b |= PAD_R2;
    if (k[SDL_SCANCODE_RETURN]) b |= PAD_START;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= PAD_SELECT;
    p->buttons |= b;
}

static uint8_t axis8(Sint16 v) { return (uint8_t)((v + 32768) >> 8); }

// Stick axis to a NeGcon twist value with a small dead zone.
static uint8_t twist8(Sint16 v) {
    const int dead = 2500;
    int x = v;
    if (x > -dead && x < dead) return 0x80;
    x = x > 0 ? (x - dead) * 32767 / (32767 - dead) : (x + dead) * 32768 / (32768 - dead);
    int t = 0x80 + x * 127 / 32767;
    return (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
}

static uint8_t trigger8(Sint16 v) { return v <= 0 ? 0 : (uint8_t)(v * 255 / 32767); }

static void poll_gamepad(PadState *p, SDL_Gamepad *gp, bool negcon) {
    if (!gp) return;
    static const struct { SDL_GamepadButton b; uint16_t m; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
        {SDL_GAMEPAD_BUTTON_START, PAD_START}, {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(gp, map[i].b)) p->buttons |= map[i].m;
    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p->buttons |= PAD_L2;
    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p->buttons |= PAD_R2;
    Sint16 lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
    uint8_t keyboard_twist = p->lx;
    p->lx = axis8(lx); p->ly = axis8(ly);
    p->rx = axis8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    p->ry = axis8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    if (negcon) {
        // NeGcon: twist from the left stick, I/II (throttle/brake) from the triggers or
        // the South/West buttons, A/B from North/East, L/R from the shoulders
        p->lx = twist8(lx);
        if (p->lx == 0x80) p->lx = keyboard_twist;   // stick centred: keep keyboard steering
        uint8_t ti = trigger8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        uint8_t tii = trigger8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH)) ti = 255;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST)) tii = 255;
        if (ti > p->negcon_i) p->negcon_i = ti;
        if (tii > p->negcon_ii) p->negcon_ii = tii;
        p->buttons &= ~(PAD_L2 | PAD_R2);
        return;
    }
    if (!pad_analog_mode) {
        // digital pad: the left stick doubles as the d-pad
        const int dead = 14000;
        if (lx < -dead) p->buttons |= PAD_LEFT;
        if (lx > dead) p->buttons |= PAD_RIGHT;
        if (ly < -dead) p->buttons |= PAD_UP;
        if (ly > dead) p->buttons |= PAD_DOWN;
    }
}

// Controller type per port. "auto" presents a NeGcon where a gamepad is connected (the game
// supports its analog steering and throttle, but not DualShock sticks), else a digital pad.
static bool port_negcon[2];

static void update_controller_types(void) {
    static int shown[2] = {-1, -1};
    for (int port = 0; port < 2; port++) {
        int t = controller_type;
        if (t == CTRL_AUTO) t = gamepads[port] ? CTRL_NEGCON : CTRL_DIGITAL;
        if (shown[port] != t) LOG("[input] port %d: %s\n", port + 1, controller_names[t]);
        shown[port] = t;
        port_negcon[port] = t == CTRL_NEGCON;
        sio_set_port(port, t == CTRL_NEGCON, t == CTRL_DUALSHOCK);
    }
    negcon_mode = port_negcon[0];
}

// The first two gamepads drive ports 1 and 2 (port 1 also takes the keyboard).
static void assign_gamepads(void) {
    for (int i = 0; i < 2; i++)
        if (gamepads[i] && !SDL_GamepadConnected(gamepads[i])) { SDL_CloseGamepad(gamepads[i]); gamepads[i] = NULL; }
    if (!gamepads[0] && gamepads[1]) { gamepads[0] = gamepads[1]; gamepads[1] = NULL; }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        bool used = false;
        for (int k = 0; k < 2; k++) used |= gamepads[k] && SDL_GetGamepadID(gamepads[k]) == ids[i];
        if (used) continue;
        for (int k = 0; k < 2; k++) {
            if (gamepads[k]) continue;
            gamepads[k] = SDL_OpenGamepad(ids[i]);
            if (gamepads[k]) LOG("[input] port %d gamepad: %s\n", k + 1, SDL_GetGamepadName(gamepads[k]));
            break;
        }
    }
    SDL_free(ids);
    update_controller_types();
}

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            LOG("quit\n");
            exit(0);
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            assign_gamepads();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_F11 ||
                (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                opts.fullscreen = !opts.fullscreen;
                SDL_SetWindowFullscreen(window, opts.fullscreen);
            } else if (e.key.scancode == SDL_SCANCODE_TAB) {
                fast_forward = true;
            } else if (e.key.scancode == SDL_SCANCODE_F2) {
                render_cfg.filter = !render_cfg.filter;
            } else if (e.key.scancode == SDL_SCANCODE_F3) {
                gpu_dither_enabled = !gpu_dither_enabled;
            } else if (e.key.scancode == SDL_SCANCODE_F4) {
                render_cfg.hw = !render_cfg.hw;
                LOG("[render] %s renderer\n", render_cfg.hw ? "high-resolution" : "original 1x");
            } else if (e.key.scancode == SDL_SCANCODE_F6) {
                render_cfg.interpolate = !render_cfg.interpolate;
                LOG("[render] frame interpolation %s\n", render_cfg.interpolate ? "on" : "off");
            } else if (e.key.scancode == SDL_SCANCODE_F5) {
                pgxp_enabled = !pgxp_enabled;
                LOG("[render] precise vertices %s\n", pgxp_enabled ? "on" : "off");
            } else if (e.key.scancode == SDL_SCANCODE_ESCAPE && (e.key.mod & SDL_KMOD_SHIFT)) {
                exit(0);
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (e.key.scancode == SDL_SCANCODE_TAB) fast_forward = false;
            break;
        }
    }
    for (int port = 0; port < 2; port++) {
        PadState *p = &pads[port];
        p->buttons = 0;
        p->lx = p->ly = p->rx = p->ry = 0x80;
        p->negcon_i = p->negcon_ii = p->negcon_l = 0;
        p->connected = port == 0 || gamepads[port];
        if (port == 0) {
            poll_keyboard(p);
            p->buttons |= script_buttons(frame_count, p);
            if (port_negcon[0]) {
                // keyboard on a NeGcon: arrows twist fully, X/Z are I/II
                if (p->buttons & PAD_LEFT) p->lx = 0x00;
                if (p->buttons & PAD_RIGHT) p->lx = 0xFF;
                if (p->buttons & PAD_CROSS) p->negcon_i = 255;
                if (p->buttons & PAD_SQUARE) p->negcon_ii = 255;
            }
        }
        poll_gamepad(p, gamepads[port], port_negcon[port]);
    }
}

extern int frame_interval_override;

// ----------------------------------------------------------------- frames
static int presents_per_vblank = 1;   // >1 on high-refresh displays
static int presents_override;

static void present(float alpha) {
    DisplayInfo d;
    gpu_display_info(&d);
    if (!render_cfg.hw || d.rgb24) {
        int w, h, stride;
        gpu_display(frame_rgba, &w, &h, &stride);
        render_present(&d, frame_rgba, 1.0f);
    } else {
        render_present(&d, NULL, alpha);
    }
}

// Shows this vblank's images. A game frame lasting `interval` vblanks is shown as
// interval * presents_per_vblank images, the in-between ones interpolated.
static void present_vblank(void) {
    int interval = render_frame_interval(), k = render_vblanks_since_flip();
    int n = presents_per_vblank;
    const uint64_t frame_ns = vblank_ns;
    uint64_t deadline = pace_ns + frame_ns;   // when this vblank's time is up
    for (int p = 0; p < n; p++) {
        // Extra images are optional: skip them rather than slow the game down.
        if (p > 0 && !opts.turbo && SDL_GetTicksNS() + frame_ns / (uint64_t)n / 2 > deadline) break;
        float alpha = 1.0f;
        if (render_cfg.interpolate && interval >= 2 && interval <= 4)
            alpha = (float)(k * n + p + 1) / (float)(interval * n);
        present(alpha > 1.0f ? 1.0f : alpha);
        if (p + 1 < n && !vsync_paces && !opts.turbo) SDL_DelayPrecise(1000000000ull * 1001 / 60000 / (uint64_t)n);
    }
    render_vblank();
}

static FILE *wav_out;
static uint32_t wav_frames;

static void wav_header(void) {
    uint32_t data = wav_frames * 4, riff = 36 + data, fmt_size = 16, rate = 44100, byte_rate = 44100 * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(wav_out, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav_out); fwrite(&riff, 4, 1, wav_out); fwrite("WAVEfmt ", 1, 8, wav_out);
    fwrite(&fmt_size, 4, 1, wav_out); fwrite(&pcm, 2, 1, wav_out); fwrite(&ch, 2, 1, wav_out);
    fwrite(&rate, 4, 1, wav_out); fwrite(&byte_rate, 4, 1, wav_out); fwrite(&align, 2, 1, wav_out);
    fwrite(&bits, 2, 1, wav_out); fwrite("data", 1, 4, wav_out); fwrite(&data, 4, 1, wav_out);
    fseek(wav_out, 0, SEEK_END);
}

static void close_wav(void) {
    if (wav_out) { wav_header(); fclose(wav_out); wav_out = NULL; }
}

static void push_audio(void) {
    int16_t buf[8192 * 2];
    int n = spu_take(buf, 8192);
    if (wav_out && n) { fwrite(buf, 4, (size_t)n, wav_out); wav_frames += (uint32_t)n; }
    if (!audio || !n || fast_forward) return;
    // Dynamic rate control: nudge the playback rate so the queue hovers around ~50 ms,
    // which absorbs the 60 vs 59.94 Hz difference when the display paces emulation.
    const int target = 44100 * 4 / 20;
    int queued = SDL_GetAudioStreamQueued(audio);
    if (queued > target * 8) { SDL_ClearAudioStream(audio); queued = 0; }
    float ratio = 1.0f + (float)(queued - target) / (float)target * 0.005f;
    if (ratio < 0.995f) ratio = 0.995f;
    if (ratio > 1.005f) ratio = 1.005f;
    SDL_SetAudioStreamFrequencyRatio(audio, ratio);
    SDL_PutAudioStreamData(audio, buf, n * 4);
}

static void pace(void) {
    uint64_t now = SDL_GetTicksNS();
    if (!pace_ns || now > pace_ns + vblank_ns * 4) pace_ns = now;
    pace_ns += vblank_ns;
    if (fast_forward) { pace_ns = now; return; }
    // With vsync the swaps normally take us right up to the deadline; the timer is the safety
    // net for when they do not block (occluded window, vsync not honoured by the system).
    if (pace_ns > now + (vsync_paces ? 1000000u : 0u)) SDL_DelayPrecise(pace_ns - now);
}

void host_frame(void) {
    static uint64_t fps_t0;
    frame_count++;
    handle_events();
    if (!fast_forward || (frame_count % 8) == 0) present_vblank();
    else render_vblank();
    push_audio();
    if (opts.shot_every && frame_count % opts.shot_every == 0) save_screenshot();
    if (opts.exit_after && frame_count >= opts.exit_after) {
        if (getenv("CMR_DUMP_RAM")) {
            FILE *f = fopen(getenv("CMR_DUMP_RAM"), "wb");
            if (f) { fwrite(psx_ram, 1, sizeof psx_ram, f); fclose(f); }
        }
        if (log_level >= 1) trace_dump();
#ifdef RECOMP_TRACE
        extern void gte_profile_dump(void);
        if (log_level >= 3) gte_profile_dump();
#endif
        exit(0);
    }
    if (log_level >= 1 && frame_count % 300 == 0) {
        uint64_t now = SDL_GetTicksNS();
        extern int interp_stat_matched, interp_stat_total, interp_stat_found;
        if (fps_t0) LOG("[host] frame %d, %.1f fps, interpolated vertices %d/%d (keys found %d)\n", frame_count,
                        300e9 / (double)(now - fps_t0), interp_stat_matched, interp_stat_total, interp_stat_found);
        interp_stat_matched = interp_stat_total = interp_stat_found = 0;
        fps_t0 = now;
    }
    if (!opts.turbo) pace();
}

// Debug aid: CMR_WATCH=addr[,addr...] logs when RAM words change (checked every vblank).
static void watch_ram(void) {
    static uint32_t addrs[16], vals[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("CMR_WATCH");
        while (e && *e && n < 16) {
            addrs[n] = (uint32_t)strtoul(e, (char **)&e, 16) & 0x1FFFFC;
            memcpy(&vals[n], psx_ram + addrs[n], 4);
            n++;
            if (*e == ',') e++;
        }
    }
    for (int i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, psx_ram + addrs[i], 4);
        if (v != vals[i]) LOG("[watch] frame %d: %06x %08x -> %08x\n", frame_count, addrs[i], vals[i], v);
        vals[i] = v;
    }
}

static void vblank_event(void) {
    watch_ram();
    static int watching = -1;
    if (watching < 0) watching = getenv("CMR_WATCH") != NULL;
    if (watching) {
        static uint32_t last_gp;
        if (cpu.r[28] != last_gp) { LOG("[watch] frame %d: gp = %08x\n", frame_count, cpu.r[28]); last_gp = cpu.r[28]; }
    }
    next_vblank += CYCLES_PER_FRAME;
    sched_at(EV_VBLANK, next_vblank, vblank_event);
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    spu_run_to(cpu.cycles);
    host_frame();
}

// ------------------------------------------------------------------- setup
static bool read_line(const char *file, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok && out[0];
}

// The disc image: command line, then disc_path.txt written by tools/build_game.py.
static const char *find_cue(void) {
    static char path[1024];
    char file[1100];
    const char *base = SDL_GetBasePath();
    if (base) {
        snprintf(file, sizeof file, "%sdisc_path.txt", base);
        if (read_line(file, path, sizeof path)) return path;
    }
    if (read_line("disc_path.txt", path, sizeof path)) return path;
    return NULL;
}

// ----------------------------------------------------------------- settings
static char settings_path[1100];

static void apply_setting(const char *k, const char *v) {
    int n = atoi(v);
    if (!strcmp(k, "fullscreen")) opts.fullscreen = n;
    else if (!strcmp(k, "window")) opts.scale = n > 0 ? n : 3;
    else if (!strcmp(k, "scale")) render_cfg.scale = n;
    else if (!strcmp(k, "hires")) render_cfg.hw = n;
    else if (!strcmp(k, "filter")) render_cfg.filter = n;
    else if (!strcmp(k, "widescreen")) render_cfg.widescreen = n;
    else if (!strcmp(k, "pgxp")) pgxp_enabled = n;
    else if (!strcmp(k, "interpolation")) render_cfg.interpolate = n;
    else if (!strcmp(k, "vsync")) vsync = n;
    else if (!strcmp(k, "dither")) gpu_dither_enabled = n;
    else if (!strcmp(k, "controller")) {
        for (int c = 0; c < 4; c++) if (!strcmp(v, controller_names[c])) controller_type = c;
    }
    else if (!strcmp(k, "cpu")) cycles_per_insn = n > 0 ? (uint32_t)n : 2;
}

static void load_settings(void) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;
        v[strcspn(v, "\r\n")] = 0;
        apply_setting(k, v);
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(settings_path, "w");
    if (!f) return;
    fprintf(f, "# Colin McRae Rally PC settings (also changeable with hotkeys and command-line options)\n");
    fprintf(f, "fullscreen = %d\nwindow = %d\nscale = %d\nhires = %d\nfilter = %d\nwidescreen = %d\n",
            opts.fullscreen, opts.scale, render_cfg.scale, render_cfg.hw, render_cfg.filter, render_cfg.widescreen);
    fprintf(f, "interpolation = %d\n", render_cfg.interpolate);
    fprintf(f, "pgxp = %d\nvsync = %d\ndither = %d\ncontroller = %s\ncpu = %u\n", pgxp_enabled, vsync,
            gpu_dither_enabled, controller_names[controller_type], cycles_per_insn);
    fclose(f);
}

static void usage(void) {
    fprintf(stderr,
            "usage: cmrally [options] [game.cue]\n"
            "  --fullscreen        start fullscreen (toggle with F11 / Alt+Enter)\n"
            "  --window N          window size as a multiple of 320x240 (default 3)\n"
            "  --controller TYPE   auto (NeGcon with a gamepad, else digital), negcon, digital,\n"
            "                      dualshock\n"
            "  --cpu N             CPU cycles per instruction (default 2; 1 = faster CPU)\n"
            "  --no-dither         disable PlayStation dithering (toggle with F3)\n"
            "  --software          show the original 1x software renderer (toggle with F4)\n"
            "  --scale N           internal resolution multiplier (default: match the window)\n"
            "  --filter            bilinear texture filtering (toggle with F2)\n"
            "  --widescreen        16:9 with a widened 3D projection\n"
            "  --no-pgxp           disable sub-pixel vertex precision (toggle with F5)\n"
            "  --no-vsync          do not sync presentation to the display\n"
            "  --no-interpolation  show only the game's own frames (30 fps in races)\n"
            "  --log N             debug log level\n"
            "keys: arrows=d-pad X=cross C=circle Z=square S=triangle Q/W=L1/R1 A/D=L2/R2\n"
            "      Enter=start RShift=select Tab=fast-forward F11=fullscreen Shift+Esc=quit\n");
}

int main(int argc, char **argv) {
    char *pref = SDL_GetPrefPath("cmrally", "Colin McRae Rally");
    if (pref) { snprintf(save_dir, sizeof save_dir, "%s", pref); SDL_free(pref); }
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--save-dir")) snprintf(save_dir, sizeof save_dir, "%s/", argv[i + 1]);
    snprintf(settings_path, sizeof settings_path, "%scmrally.ini", save_dir);
    load_settings();
    bool testing = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) opts.fullscreen = true;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) opts.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) render_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--software")) render_cfg.hw = false;
        else if (!strcmp(argv[i], "--filter")) render_cfg.filter = true;
        else if (!strcmp(argv[i], "--widescreen")) render_cfg.widescreen = true;
        else if (!strcmp(argv[i], "--no-pgxp")) pgxp_enabled = false;
        else if (!strcmp(argv[i], "--no-vsync")) vsync = false;
        else if (!strcmp(argv[i], "--no-interpolation")) render_cfg.interpolate = false;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) frame_interval_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav_out = fopen(argv[++i], "wb");
            if (wav_out) { wav_header(); atexit(close_wav); }
        }
        else if (!strcmp(argv[i], "--controller") && i + 1 < argc) {
            const char *t = argv[++i];
            for (int k = 0; k < 4; k++) if (!strcmp(t, controller_names[k])) controller_type = k;
        }
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cycles_per_insn = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-dither")) gpu_dither_enabled = false;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) opts.exit_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) opts.shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) opts.shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) opts.script = argv[++i];
        else if (!strcmp(argv[i], "--turbo")) opts.turbo = true;
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) i++;
        else if (!strcmp(argv[i], "--presents") && i + 1 < argc) presents_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (argv[i][0] != '-') opts.cue = argv[i];
        else { usage(); return 1; }
    }
    if (!opts.cue) opts.cue = find_cue();
    if (!opts.cue || !cdrom_open(opts.cue)) {
        fprintf(stderr, "Cannot open the game disc. Build with tools/build_game.py or pass the path to your\n"
                        "Colin McRae Rally .cue file.\n");
        usage();
        return 1;
    }
    // Hotkey changes persist; test runs (scripted/turbo) leave the settings alone.
    testing = opts.turbo || opts.script || opts.exit_after;
    if (!testing) atexit(save_settings);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int win_w = (render_cfg.widescreen ? 427 : 320) * opts.scale;
    window = SDL_CreateWindow("Colin McRae Rally", win_w, 240 * opts.scale,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (opts.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window || !render_init(window)) {
        fprintf(stderr, "Cannot create an OpenGL 3.3 window: %s\n", SDL_GetError());
        return 1;
    }
    if (opts.turbo) vsync = false;
    SDL_GL_SetSwapInterval(vsync && !getenv("CMR_TEST_NONBLOCKING_SWAP") ? 1 : 0);
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    float hz = dm ? dm->refresh_rate : 60.0f;
    if (vsync && hz >= 89.0f) presents_per_vblank = (int)(hz / 59.94f + 0.5f) > 4 ? 4 : (int)(hz / 59.94f + 0.5f);
    if (presents_override > 0) presents_per_vblank = presents_override > 4 ? 4 : presents_override;
    float target = 59.94f * presents_per_vblank;
    vsync_paces = vsync && hz > target - 2.0f && hz < target + 2.0f;
    if (vsync_paces) vblank_ns = (uint64_t)(1e9 * presents_per_vblank / hz);
    LOG("[host] display %.2f Hz, %d image%s per vblank, %s\n", hz, presents_per_vblank,
        presents_per_vblank > 1 ? "s" : "", vsync_paces ? "vsync paces emulation" : "timer paces emulation");
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else LOG("audio: %s\n", SDL_GetError());
    pad_analog_mode = controller_type == CTRL_DUALSHOCK;
    assign_gamepads();

    memset(psx_ram, 0, sizeof psx_ram);
    sched_init();
    bios_init();
    timers_init();
    gpu_init();
    gte_init();
    spu_init();
    cdrom_init();
    mdec_init();

    uint32_t entry, sp, gp;
    char boot[256] = "\\SCUS_944.74;1";
    if (bios_load_exe(boot, &entry, &sp, &gp)) {
        fprintf(stderr, "Cannot load the boot executable from the disc.\n");
        return 1;
    }
    next_vblank = CYCLES_PER_FRAME;
    sched_at(EV_VBLANK, next_vblank, vblank_event);
    LOG("[boot] %s entry=%08x sp=%08x save dir: %s\n", opts.cue, entry, sp, save_dir);
    psx_start(entry, 0x801FFF00, gp);
    return 0;
}
