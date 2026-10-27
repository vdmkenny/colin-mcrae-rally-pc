// SDL3 host: window, presentation, audio, input and frame pacing.
#include "psx.h"
#include "pgxp.h"
#include "render.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <sys/stat.h>

PadState pads[2];
bool pad_analog_mode = false;
char save_dir[1024] = ".";

extern bool gpu_dither_enabled;
int spu_take(int16_t *dst, int frames);
void trace_dump(void);

static SDL_Window *window;
static SDL_AudioStream *audio;
static SDL_Gamepad *gamepad;
static uint32_t frame_rgba[640 * 512];

static uint64_t next_vblank;
static uint64_t pace_ns;
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

static uint16_t script_buttons(int frame) {
    // Format: "<start>-<end>:<hex buttons>;..." e.g. "300-310:8;600-605:4000"
    if (!opts.script) return 0;
    uint16_t b = 0;
    const char *p = opts.script;
    while (*p) {
        int a = 0, e = 0;
        unsigned m = 0;
        if (sscanf(p, "%d-%d:%x", &a, &e, &m) == 3 && frame >= a && frame <= e) b |= (uint16_t)m;
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

static void poll_gamepad(PadState *p) {
    if (!gamepad) return;
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
        if (SDL_GetGamepadButton(gamepad, map[i].b)) p->buttons |= map[i].m;
    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p->buttons |= PAD_L2;
    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p->buttons |= PAD_R2;
    Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    p->lx = axis8(lx); p->ly = axis8(ly);
    p->rx = axis8(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
    p->ry = axis8(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    if (!pad_analog_mode) {
        // digital pad: the left stick doubles as the d-pad
        const int dead = 14000;
        if (lx < -dead) p->buttons |= PAD_LEFT;
        if (lx > dead) p->buttons |= PAD_RIGHT;
        if (ly < -dead) p->buttons |= PAD_UP;
        if (ly > dead) p->buttons |= PAD_DOWN;
    }
}

static void open_first_gamepad(void) {
    if (gamepad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        gamepad = SDL_OpenGamepad(ids[0]);
        if (gamepad) LOG("[input] gamepad: %s\n", SDL_GetGamepadName(gamepad));
    }
    SDL_free(ids);
}

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            LOG("quit\n");
            exit(0);
        case SDL_EVENT_GAMEPAD_ADDED:
            open_first_gamepad();
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad && e.gdevice.which == SDL_GetGamepadID(gamepad)) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
                open_first_gamepad();
            }
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
    PadState *p = &pads[0];
    p->buttons = 0;
    p->lx = p->ly = p->rx = p->ry = 0x80;
    p->connected = true;
    poll_keyboard(p);
    poll_gamepad(p);
    p->buttons |= script_buttons(frame_count);
}

void sio_set_analog(bool on);

// ----------------------------------------------------------------- frames
static void present(void) {
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

static void push_audio(void) {
    int16_t buf[8192 * 2];
    int n = spu_take(buf, 8192);
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
    const uint64_t frame_ns = 1000000000ull * 1001 / 60000;   // 59.94 Hz
    uint64_t now = SDL_GetTicksNS();
    if (!pace_ns || now > pace_ns + frame_ns * 4) pace_ns = now;
    pace_ns += frame_ns;
    if (fast_forward) { pace_ns = now; return; }
    if (vsync_paces) return;   // SDL_GL_SwapWindow already waited for the display
    if (pace_ns > now) SDL_DelayPrecise(pace_ns - now);
}

void host_frame(void) {
    static uint64_t fps_t0;
    frame_count++;
    handle_events();
    if (!fast_forward || (frame_count % 8) == 0) present();
    push_audio();
    if (opts.shot_every && frame_count % opts.shot_every == 0) save_screenshot();
    if (opts.exit_after && frame_count >= opts.exit_after) {
        if (log_level >= 1) trace_dump();
        exit(0);
    }
    if (log_level >= 1 && frame_count % 300 == 0) {
        uint64_t now = SDL_GetTicksNS();
        if (fps_t0) LOG("[host] frame %d, %.1f fps\n", frame_count, 300e9 / (double)(now - fps_t0));
        fps_t0 = now;
    }
    if (!opts.turbo) pace();
}

static void vblank_event(void) {
    next_vblank += CYCLES_PER_FRAME;
    sched_at(EV_VBLANK, next_vblank, vblank_event);
    irq_raise(IRQ_VBLANK);
    gpu_vblank();
    spu_run_to(cpu.cycles);
    host_frame();
}

// ------------------------------------------------------------------- setup
static const char *find_cue(void) {
    static char path[1024];
    const char *candidates[] = {"game/disc/Colin McRae Rally.cue", "Colin McRae Rally.cue", "disc/Colin McRae Rally.cue"};
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (f) { fclose(f); snprintf(path, sizeof path, "%s", candidates[i]); return path; }
    }
    return NULL;
}

static void usage(void) {
    fprintf(stderr,
            "usage: cmrally [options] [game.cue]\n"
            "  --fullscreen        start fullscreen (toggle with F11 / Alt+Enter)\n"
            "  --window N          window size as a multiple of 320x240 (default 3)\n"
            "  --analog            present a DualShock in analog mode\n"
            "  --cpu N             CPU cycles per instruction (default 2; 1 = faster CPU)\n"
            "  --no-dither         disable PlayStation dithering (toggle with F3)\n"
            "  --software          show the original 1x software renderer (toggle with F4)\n"
            "  --scale N           internal resolution multiplier (default: match the window)\n"
            "  --filter            bilinear texture filtering (toggle with F2)\n"
            "  --widescreen        16:9 with a widened 3D projection\n"
            "  --no-pgxp           disable sub-pixel vertex precision (toggle with F5)\n"
            "  --no-vsync          do not sync presentation to the display\n"
            "  --log N             debug log level\n"
            "keys: arrows=d-pad X=cross C=circle Z=square S=triangle Q/W=L1/R1 A/D=L2/R2\n"
            "      Enter=start RShift=select Tab=fast-forward F11=fullscreen Shift+Esc=quit\n");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fullscreen")) opts.fullscreen = true;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) opts.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) render_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--software")) render_cfg.hw = false;
        else if (!strcmp(argv[i], "--filter")) render_cfg.filter = true;
        else if (!strcmp(argv[i], "--widescreen")) render_cfg.widescreen = true;
        else if (!strcmp(argv[i], "--no-pgxp")) pgxp_enabled = false;
        else if (!strcmp(argv[i], "--no-vsync")) vsync = false;
        else if (!strcmp(argv[i], "--analog")) pad_analog_mode = true;
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cycles_per_insn = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-dither")) gpu_dither_enabled = false;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) opts.exit_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) opts.shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) opts.shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) opts.script = argv[++i];
        else if (!strcmp(argv[i], "--turbo")) opts.turbo = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (argv[i][0] != '-') opts.cue = argv[i];
        else { usage(); return 1; }
    }
    if (!opts.cue) opts.cue = find_cue();
    if (!opts.cue || !cdrom_open(opts.cue)) {
        fprintf(stderr, "Cannot open the game disc. Pass the path to your Colin McRae Rally .cue file.\n");
        usage();
        return 1;
    }
    char *pref = SDL_GetPrefPath("cmrally", "Colin McRae Rally");
    if (pref) { snprintf(save_dir, sizeof save_dir, "%s", pref); SDL_free(pref); }

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
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    vsync_paces = vsync && dm && dm->refresh_rate > 58.5f && dm->refresh_rate < 61.5f;
    LOG("[host] display %.2f Hz, %s\n", dm ? dm->refresh_rate : 0.0f,
        vsync_paces ? "vsync paces emulation" : "timer paces emulation");
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else LOG("audio: %s\n", SDL_GetError());
    open_first_gamepad();
    sio_set_analog(pad_analog_mode);

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
