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
void sio_analog_button(int port);
extern uint8_t pad_motor_small[2], pad_motor_large[2];
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
static bool fast_forward;     // Tab held
static bool fast_loading = true;   // fast-forward while the game loads from the CD
static bool loading_ff;       // ... and it is loading now
static bool fast(void) { return fast_forward || loading_ff; }
static bool vsync = true;
static bool vsync_paces;     // the display runs at a multiple of ~60 Hz, so buffer swaps pace emulation

static struct {
    const char *cue;
    bool fullscreen;
    int scale;
    int exit_after;          // testing: quit after this many frames
    int shot_every;          // testing: save a screenshot every N frames
    int shot_from;           // ... starting at this frame
    const char *shot_dir;
    const char *script;      // testing: scripted input "frame:buttons,..."
    const char *script2;     // testing: scripted input for a pad on port 2
    bool turbo;              // testing: no frame pacing
    bool mute;               // no audio output (WAV capture still works)
    bool hidden;             // testing: no visible window
} opts = {.scale = 3, .shot_dir = "."};

static int frame_count;

static uint16_t script_buttons(const char *script, int frame, PadState *pad) {
    // Format: "<start>-<end>:<hex buttons>[:<lx hex>]...;" e.g. "300-310:8;600-605:4000:20"
    if (!script) return 0;
    uint16_t b = 0;
    const char *p = script;
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

static void poll_gamepad(PadState *p, SDL_Gamepad *gp, bool negcon, bool dualshock) {
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
    if (!dualshock) {
        // digital pad: the left stick doubles as the d-pad
        const int dead = 14000;
        if (lx < -dead) p->buttons |= PAD_LEFT;
        if (lx > dead) p->buttons |= PAD_RIGHT;
        if (ly < -dead) p->buttons |= PAD_UP;
        if (ly > dead) p->buttons |= PAD_DOWN;
    }
}

// Controller type per port. "auto" presents a DualShock where a gamepad is connected (analog
// steering once in analog mode, and vibration), else a digital pad for the keyboard. The game
// also supports Namco's NeGcon, which adds analog throttle and brake but has no vibration.
static bool port_negcon[2];
static int port_type[2] = {-1, -1};

static void update_controller_types(void) {
    static int shown[2] = {-1, -1};
    for (int port = 0; port < 2; port++) {
        int t = controller_type;
        if (t == CTRL_AUTO) t = gamepads[port] ? CTRL_DUALSHOCK : CTRL_DIGITAL;
        if (shown[port] != t) LOG("[input] port %d: %s\n", port + 1, controller_names[t]);
        if (port_type[port] != t) sio_set_port(port, t == CTRL_NEGCON, t == CTRL_DUALSHOCK);
        shown[port] = port_type[port] = t;
        port_negcon[port] = t == CTRL_NEGCON;
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

// DualShock vibration: the large motor maps to the gamepad's low-frequency rumble, the small
// (on/off) motor to the high-frequency one. Renewed every vblank while running.
static bool rumble_enabled = true;

static void update_rumble(void) {
    static bool active[2];
    for (int port = 0; port < 2; port++) {
        SDL_Gamepad *gp = gamepads[port];
        uint8_t small = pad_motor_small[port], large = pad_motor_large[port];
        bool on = rumble_enabled && port_type[port] == CTRL_DUALSHOCK && (small || large) && !fast();
        if (!gp) { active[port] = false; continue; }
        if (on) {
            Uint16 low = (Uint16)(large * 257), high = small ? 0xC000 : 0;
            SDL_RumbleGamepad(gp, low, high, 100);
        } else if (active[port]) {
            SDL_RumbleGamepad(gp, 0, 0, 0);
        }
        active[port] = on;
    }
}

static void update_display_timing(void);

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            LOG("quit\n");
            exit(0);
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:      // moved to another monitor
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            update_display_timing();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            assign_gamepads();
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            // the Guide/Home button is the DualShock's ANALOG button
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                for (int port = 0; port < 2; port++)
                    if (gamepads[port] && SDL_GetGamepadID(gamepads[port]) == e.gbutton.which) sio_analog_button(port);
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
            } else if (e.key.scancode == SDL_SCANCODE_F8) {
                render_cfg.msaa = render_cfg.msaa >= 8 ? 1 : render_cfg.msaa * 2;
                LOG("[render] anti-aliasing %s\n", render_cfg.msaa > 1 ? (render_cfg.msaa == 2 ? "2x" : render_cfg.msaa == 4 ? "4x" : "8x") : "off");
            } else if (e.key.scancode == SDL_SCANCODE_F3) {
                gpu_dither_enabled = !gpu_dither_enabled;
            } else if (e.key.scancode == SDL_SCANCODE_F4) {
                render_cfg.hw = !render_cfg.hw;
                LOG("[render] %s renderer\n", render_cfg.hw ? "high-resolution" : "original 1x");
            } else if (e.key.scancode == SDL_SCANCODE_F7) {
                sio_analog_button(0);
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
            p->buttons |= script_buttons(opts.script, frame_count, p);
            if (port_negcon[0]) {
                // keyboard on a NeGcon: arrows twist fully, X/Z are I/II
                if (p->buttons & PAD_LEFT) p->lx = 0x00;
                if (p->buttons & PAD_RIGHT) p->lx = 0xFF;
                if (p->buttons & PAD_CROSS) p->negcon_i = 255;
                if (p->buttons & PAD_SQUARE) p->negcon_ii = 255;
            }
        }
        if (port == 1 && opts.script2) {
            p->connected = true;
            p->buttons |= script_buttons(opts.script2, frame_count, p);
        }
        poll_gamepad(p, gamepads[port], port_negcon[port], port_type[port] == CTRL_DUALSHOCK);
        if (port_type[port] == CTRL_DUALSHOCK && p->lx == 0x80) {
            // in analog mode the game steers only with the stick; let the d-pad steer too
            if (p->buttons & PAD_LEFT) p->lx = 0x00;
            if (p->buttons & PAD_RIGHT) p->lx = 0xFF;
        }
    }
}

extern int frame_interval_override;
extern bool race_60fps;
extern float draw_distance, cull_margin;
extern bool unlock_all;

// ----------------------------------------------------------------- frames
// With vsync, images are presented to the refresh of the monitor the window is on. When its
// rate is a multiple of ~60 Hz (60, 120, 240 ...) each emulated vblank lasts exactly that many
// refreshes, the image is shown for each of them, and the buffer swaps pace emulation. At other
// rates (75, 144, 165 ...) a timer paces emulation at 59.94 Hz and each new image waits for the
// next refresh.
static int refreshes_per_vblank = 1;   // display-locked: refreshes per emulated vblank; 0: timer-paced
static int presents_counted;           // for the fps log

static void update_display_timing(void) {
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    float hz = dm && dm->refresh_rate > 0.0f ? dm->refresh_rate : 60.0f;
    if (getenv("CMR_REFRESH_HZ")) hz = (float)atof(getenv("CMR_REFRESH_HZ"));   // testing other monitors
    int k = (int)(hz / 59.94f + 0.5f);
    float off = hz - 59.94f * (float)k;
    refreshes_per_vblank = k >= 1 && k <= 8 && off > -2.0f && off < 2.0f ? k : 0;
    vsync_paces = vsync && refreshes_per_vblank;
    vblank_ns = vsync_paces ? (uint64_t)(1e9 * k / hz) : 1000000000ull * 1001 / 60000;
    LOG("[host] display %.2f Hz, %s\n", hz, vsync_paces ? "vsync paces emulation" : "timer paces emulation");
}

static void present(void) {
    presents_counted++;
    DisplayInfo d;
    gpu_display_info(&d);
    if (!render_cfg.hw || d.rgb24) {
        int w, h, stride;
        gpu_display(frame_rgba, &w, &h, &stride);
        render_present(&d, frame_rgba);
    } else {
        render_present(&d, NULL);
    }
}

// Shows this vblank's image: once per refresh it lasts when vsync paces emulation.
static void present_vblank(void) {
    int n = vsync_paces && !opts.turbo ? refreshes_per_vblank : 1;
    uint64_t deadline = pace_ns + vblank_ns;   // this vblank's share of real time
    for (int p = 0; p < n; p++) {
        // Repeats are optional: skip them rather than slow the game down.
        if (p > 0 && SDL_GetTicksNS() + vblank_ns / (uint64_t)n / 2 > deadline) break;
        present();
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
    if (!audio || !n || fast()) return;
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
    if (fast()) { pace_ns = now; return; }
    // With vsync the swaps normally take us right up to the deadline; the timer is the safety
    // net for when they do not block (occluded window, vsync not honoured by the system).
    if (pace_ns > now + (vsync_paces ? 1000000u : 0u)) SDL_DelayPrecise(pace_ns - now);
}

void host_frame(void) {
    static uint64_t fps_t0;
    frame_count++;
    handle_events();
    if (!fast() || (frame_count % 8) == 0) present_vblank();
    else render_vblank();
    push_audio();
    unlock_all_vblank();
    if (opts.shot_every && frame_count >= opts.shot_from && frame_count % opts.shot_every == 0) save_screenshot();
    {
        // testing: CMR_ANALOG_AT=<frame> presses the DualShock ANALOG button on port 1
        static long at = -2;
        if (at == -2) at = getenv("CMR_ANALOG_AT") ? atol(getenv("CMR_ANALOG_AT")) : -1;
        if (at == frame_count) sio_analog_button(0);
    }
    update_rumble();
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
        if (fps_t0)
            LOG("[host] frame %d, %.1f fps emulated, %.1f presented\n", frame_count, 300e9 / (double)(now - fps_t0),
                presents_counted * 1e9 / (double)(now - fps_t0));
        presents_counted = 0;
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
            addrs[n] = (uint32_t)strtoul(e, (char **)&e, 16) & RAM_MASK & ~3u;
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

// Debug aid: CMR_POKE=frame:addr=value[,...] writes RAM words at a vblank, e.g. to skip ahead
// in a stage. The value may be @addr to copy another word.
static void poke_ram(void) {
    const char *e = getenv("CMR_POKE");
    while (e && *e) {
        long f = strtol(e, (char **)&e, 10);
        uint32_t a = (uint32_t)strtoul(e + 1, (char **)&e, 16) & RAM_MASK & ~3u, v;
        if (e[1] == '@') {
            uint32_t src = (uint32_t)strtoul(e + 2, (char **)&e, 16) & RAM_MASK & ~3u;
            memcpy(&v, psx_ram + src, 4);
        } else {
            v = (uint32_t)strtoul(e + 1, (char **)&e, 16);
        }
        if (f == frame_count) {
            memcpy(psx_ram + a, &v, 4);
            LOG("[poke] frame %d: %06x = %08x\n", frame_count, a, v);
        }
        if (*e == ',') e++;
    }
}

// What the CD, MDEC and GTE did this vblank, smoothed over a second: loading is reading data
// with no streamed audio or video and no 3D scene around it (a race is never sped up).
enum { ACT_IDLE, ACT_LOADING, ACT_STREAM, ACT_MUSIC, ACT_3D };
static int activity;

static void update_activity(void) {
    extern int mdec_activity, gte_projections;
    static int since_data = 1000, since_stream = 1000, since_music = 1000, since_3d = 1000;
    int data, xa, cdda;
    cdrom_activity(&data, &xa, &cdda);
    since_data = data ? 0 : since_data + 1;
    since_stream = xa || mdec_activity ? 0 : since_stream + 1;
    since_music = cdda ? 0 : since_music + 1;
    since_3d = gte_projections > 300 ? 0 : since_3d + 1;   // a race or a 3D scene is running
    mdec_activity = gte_projections = 0;
    int a = since_stream < 60 ? ACT_STREAM : since_music < 60 ? ACT_MUSIC
          : since_data < 8 ? (since_3d < 60 ? ACT_3D : ACT_LOADING) : ACT_IDLE;
    // Debug aid: CMR_TRACE_LOAD=1 logs the changes.
    static int trace = -1, run_start;
    if (trace < 0) trace = getenv("CMR_TRACE_LOAD") != NULL;
    if (trace && a != activity) {
        static const char *names[] = {"idle", "loading", "stream", "music", "reading during 3D"};
        LOG("[load] frames %d-%d %s (%s)\n", run_start, frame_count - 1, names[activity],
            module_active() ? module_active()->name : "-");
        run_start = frame_count;
    }
    activity = a;
    bool ff = fast_loading && !opts.turbo && a == ACT_LOADING;
    if (ff != loading_ff) DBG(1, "[host] frame %d: %s\n", frame_count, ff ? "loading, fast-forward" : "normal speed");
    loading_ff = ff;
}

static void vblank_event(void) {
    watch_ram();
    update_activity();
    poke_ram();
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

// Version 2 made texture filtering, widescreen and anti-aliasing the defaults; older files saved
// the previous defaults without anyone choosing them.
#define SETTINGS_VERSION 2
static int settings_version;

static int msaa_setting(int n) { return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }

static void apply_setting(const char *k, const char *v) {
    int n = atoi(v);
    if (!strcmp(k, "fullscreen")) opts.fullscreen = n;
    else if (!strcmp(k, "window")) opts.scale = n > 0 ? n : 3;
    else if (!strcmp(k, "scale")) render_cfg.scale = n;
    else if (!strcmp(k, "hires")) render_cfg.hw = n;
    else if (!strcmp(k, "filter")) render_cfg.filter = n;
    else if (!strcmp(k, "widescreen")) render_cfg.widescreen = n;
    else if (!strcmp(k, "msaa")) render_cfg.msaa = msaa_setting(n);
    else if (!strcmp(k, "settings_version")) settings_version = n;
    else if (!strcmp(k, "pgxp")) pgxp_enabled = n;
    else if (!strcmp(k, "vsync")) vsync = n;
    else if (!strcmp(k, "dither")) gpu_dither_enabled = n;
    else if (!strcmp(k, "rumble")) rumble_enabled = n;
    else if (!strcmp(k, "fast_loading")) fast_loading = n;
    else if (!strcmp(k, "race_60fps")) race_60fps = n;
    else if (!strcmp(k, "draw_distance")) draw_distance = (float)atof(v);
    else if (!strcmp(k, "unlock_all")) unlock_all = n;
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
    if (settings_version < 2) {
        render_cfg.filter = true;
        render_cfg.widescreen = true;
    }
}

static void save_settings(void) {
    FILE *f = fopen(settings_path, "w");
    if (!f) return;
    fprintf(f, "# Colin McRae Rally PC settings (also changeable with hotkeys and command-line options)\n");
    fprintf(f, "settings_version = %d\nmsaa = %d\n", SETTINGS_VERSION, render_cfg.msaa);
    fprintf(f, "fullscreen = %d\nwindow = %d\nscale = %d\nhires = %d\nfilter = %d\nwidescreen = %d\n",
            opts.fullscreen, opts.scale, render_cfg.scale, render_cfg.hw, render_cfg.filter, render_cfg.widescreen);
    fprintf(f, "rumble = %d\nfast_loading = %d\nrace_60fps = %d\ndraw_distance = %.2f\nunlock_all = %d\n", rumble_enabled,
            fast_loading, race_60fps, draw_distance, unlock_all);
    fprintf(f, "pgxp = %d\nvsync = %d\ndither = %d\ncontroller = %s\ncpu = %u\n", pgxp_enabled, vsync,
            gpu_dither_enabled, controller_names[controller_type], cycles_per_insn);
    fclose(f);
}

static void usage(void) {
    fprintf(stderr,
            "usage: cmrally [options] [game.cue]\n"
            "  --fullscreen        start fullscreen (toggle with F11 / Alt+Enter)\n"
            "  --window N          window size as a multiple of 320x240 (default 3)\n"
            "  --controller TYPE   auto (DualShock with a gamepad, else digital), dualshock,\n"
            "                      negcon (analog throttle/brake, no vibration), digital\n"
            "  --no-rumble         disable DualShock vibration\n"
            "  --no-fast-loading   load at the original CD speed\n"
            "  --no-race-60fps     races at the game's own 30 fps (default: 60)\n"
            "  --draw-distance X   draw distance multiplier on clear stages, 1-2 (default 2)\n"
            "  --unlock-all        every country, stage, car and expert mode available (not saved)\n"
            "  --no-unlock-all     only what the player has unlocked (default)\n"
            "  --mute              no audio output\n"
            "  --cpu N             CPU cycles per instruction (default 2; 1 = faster CPU)\n"
            "  --no-dither         disable PlayStation dithering (toggle with F3)\n"
            "  --software          show the original 1x software renderer (toggle with F4)\n"
            "  --scale N           internal resolution multiplier (default: match the window)\n"
            "  --no-filter         no bilinear texture filtering (toggle with F2)\n"
            "  --no-widescreen     4:3 as on the console (16:9 with a widened 3D view by default)\n"
            "  --msaa N            anti-aliasing samples: 0 (off), 2, 4 (default) or 8 (cycle with F8)\n"
            "  --no-pgxp           disable sub-pixel vertex precision (toggle with F5)\n"
            "  --no-vsync          do not sync presentation to the display\n"
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
    if (getenv("CMR_CULL_MARGIN")) cull_margin = (float)atof(getenv("CMR_CULL_MARGIN"));   // testing
    bool testing = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) opts.fullscreen = true;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) opts.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) render_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--software")) render_cfg.hw = false;
        else if (!strcmp(argv[i], "--filter")) render_cfg.filter = true;
        else if (!strcmp(argv[i], "--no-filter")) render_cfg.filter = false;
        else if (!strcmp(argv[i], "--widescreen")) render_cfg.widescreen = true;
        else if (!strcmp(argv[i], "--no-widescreen")) render_cfg.widescreen = false;
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) render_cfg.msaa = msaa_setting(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-pgxp")) pgxp_enabled = false;
        else if (!strcmp(argv[i], "--no-vsync")) vsync = false;
        else if (!strcmp(argv[i], "--no-rumble")) rumble_enabled = false;
        else if (!strcmp(argv[i], "--no-fast-loading")) fast_loading = false;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) frame_interval_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--race-60fps")) race_60fps = true;
        else if (!strcmp(argv[i], "--no-race-60fps")) race_60fps = false;
        else if (!strcmp(argv[i], "--draw-distance") && i + 1 < argc) draw_distance = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--unlock-all")) unlock_all = true;
        else if (!strcmp(argv[i], "--no-unlock-all")) unlock_all = false;
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
        else if (!strcmp(argv[i], "--shot-from") && i + 1 < argc) opts.shot_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) opts.shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) opts.script = argv[++i];
        else if (!strcmp(argv[i], "--script2") && i + 1 < argc) opts.script2 = argv[++i];
        else if (!strcmp(argv[i], "--turbo")) opts.turbo = true;
        else if (!strcmp(argv[i], "--mute")) opts.mute = true;
        else if (!strcmp(argv[i], "--hidden")) opts.hidden = true;
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) i++;
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
    int win_w = (render_cfg.widescreen ? 427 : 320) * opts.scale;
    window = SDL_CreateWindow("Colin McRae Rally", win_w, 240 * opts.scale,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (opts.fullscreen ? SDL_WINDOW_FULLSCREEN : 0) |
                                  (opts.hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window || !render_init(window)) {
        fprintf(stderr, "Cannot start the GPU renderer (needs Vulkan or Metal): %s\n", SDL_GetError());
        return 1;
    }
    if (opts.turbo) vsync = false;
    if (!render_set_vsync(vsync && !getenv("CMR_TEST_NONBLOCKING_SWAP")) && vsync) vsync = false;
    update_display_timing();
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    if (!opts.mute) {
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (audio) SDL_ResumeAudioStreamDevice(audio);
        else LOG("audio: %s\n", SDL_GetError());
    }
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
