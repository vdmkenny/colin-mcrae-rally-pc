# Colin McRae Rally — PC port

A native PC port of the PlayStation release of *Colin McRae Rally* (NTSC-U, SCUS-94474).
The game's four MIPS executables are statically recompiled to C and run on a native runtime
that replaces the PlayStation hardware and BIOS. Rendering, audio and input go through SDL3;
the renderer uses SDL's GPU API, which runs on Vulkan (Linux, Windows) or Metal (macOS).

No game data is included or distributed: you build the port from your own disc image, and
the recompiled code (`generated/`) is derived from it, so it is not part of this repository.
This is an unofficial fan project, not affiliated with Codemasters or Sony.

> **A quick and dirty LLM port.** Nearly all of the code, tooling and documentation here was
> written by an LLM coding agent in a short time, with little human review of the code. It
> was checked by running the game (scripted runs, screenshots, comparisons between renderers
> and builds), not by reading every line. Expect rough edges, hacks and code that would not
> pass a careful review; treat it as an experiment rather than a reference implementation.

## Getting started

You need your own copy of the game: a disc image of the North American PlayStation release
(SCUS-94474), ripped from your disc as a `.cue` file with its `.bin` tracks. Nothing from the
game is in this repository. The build script checks the image, extracts and recompiles the
game's executables, builds the program and remembers where the image is.

### macOS

```bash
xcode-select --install                 # the compiler, if you do not have it yet
brew install git sdl3 cmake ninja      # Homebrew: https://brew.sh
git clone https://github.com/vdmkenny/colin-mcrae-rally-pc.git
cd colin-mcrae-rally-pc
python3 tools/build_game.py "/path/to/Colin McRae Rally.cue"
./build/cmrally
```

### Ubuntu and Debian

Ubuntu 26.04 or Debian 13 (trixie) and newer, with a working Vulkan driver (Mesa's, which
desktop installs include, is fine):

```bash
sudo apt install git cmake ninja-build gcc python3 libsdl3-dev
git clone https://github.com/vdmkenny/colin-mcrae-rally-pc.git
cd colin-mcrae-rally-pc
python3 tools/build_game.py "/path/to/Colin McRae Rally.cue"
./build/cmrally
```

Ubuntu 24.04 and older have no SDL3 package: build and install SDL3 from source first
([SDL's Linux guide](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md)).

### Windows (not tested yet)

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is free) with
   the *Desktop development with C++* workload, which includes CMake and Ninja, plus
   [Python 3](https://www.python.org/downloads/) and [Git](https://git-scm.com/).
2. Download `SDL3-devel-<version>-VC.zip` from
   [SDL's releases](https://github.com/libsdl-org/SDL/releases/latest) and extract it, for
   example to `C:\SDL3`.
3. In the *Developer PowerShell for VS 2022*, with `CMAKE_PREFIX_PATH` set to the extracted
   `SDL3-<version>` folder:

   ```powershell
   $env:CMAKE_PREFIX_PATH = "C:\SDL3\SDL3-3.4.2"
   git clone https://github.com/vdmkenny/colin-mcrae-rally-pc.git
   cd colin-mcrae-rally-pc
   py tools\build_game.py "C:\path\to\Colin McRae Rally.cue"
   ```

4. Run the program the script names when it finishes (`build\cmrally.exe`); `SDL3.dll` is
   copied beside it.

### Playing

A gamepad works as a DualShock; on the keyboard the arrow keys steer and X, C, Z and S are
the PlayStation's face buttons (see [Controls](#controls)). F11 toggles fullscreen and
Shift+Esc quits. Settings are saved in `cmrally.ini`; [Options](#options) lists them. To add
cars of your own, see [Extra cars](#extra-cars).

## Features

- Native-resolution rendering: every primitive is redrawn at the window's pixel resolution
  in true colour (up to 16x the original 320x240)
- Sub-pixel vertex precision and perspective-correct texturing (no polygon wobble or
  texture warping), from the GTE's unrounded results, followed through every copy the game
  makes of them
- Polygons reaching past the camera (the near edge of the road, fence posts, trees beside the
  car) are clipped at the camera plane by the GPU; the console clamps their corners to
  the screen edge, which left wedges, tears and holes near the bottom of the screen
- 16:9 widescreen in races (default): wider 3D field of view, HUD kept at its original
  proportions and anchored to the screen edges; menus and videos are shown pillarboxed
- Bilinear texture filtering (default), kept inside each primitive's texture region so
  neighbouring textures on the PlayStation's texture pages do not bleed in
- Multisample anti-aliasing, 4x by default (2x/8x/off), for clean polygon edges at any
  resolution
- Gamepads (anything SDL3 supports) act as a DualShock: analog stick steering and
  vibration, as on the original hardware; a Namco NeGcon mode adds analog throttle/brake
- 60 fps races: the game simulates a fixed 1/30 s per frame, so the race loop runs every
  vblank but the game's update only every second one; the frames in between draw the scene
  half way between the last two updates, interpolating the transforms of the game's own scene
  graph (car, wheels, camera) and moving the car shadows along, so motion is smooth and the simulation, handling and stage
  times are exactly those of 30 fps. Controllers are read on update frames only, so a press is
  never missed. Costs one vblank (17 ms) of latency; `--no-race-60fps` or `race_60fps = 0`
  restores 30 fps. Rally School stays at 30 fps
- Longer draw distance and less culling: while the game draws a scene, its far limit is raised
  up to 2x (as far as the 16-bit screen depth allows) on stages without fog in view, and its
  view cone, as wide as the 4:3 picture on the console, is widened to the 16:9 one plus a
  margin so scenery does not pop in at the edges. The depth-sorting table is resized to match,
  so distant scenery cannot wrap round and be drawn in front. The game's logic keeps its own
  values.
  `--draw-distance 1..2` or `draw_distance = ...` (1 = original); foggy and rainy stages
  keep their fog
- More memory than the console: the game sees 8 MB of RAM instead of 2 MB, and its two GPU
  primitive buffers (109 KB each, filled without bounds checks) are swapped for 3 MB ones in
  the extra space, which also lifts a renderer's detail cut-back when a frame nears the buffer
  size
- Fast loading: while the game reads data from the CD (with no video, streamed audio or 3D
  scene running) the emulation fast-forwards, so loads take a fraction of the time; the
  game itself sees normal timing. `--no-fast-loading` or `fast_loading = 0` turns it off
- Unlock all (off by default): `--unlock-all` or `unlock_all = 1` makes every country and
  its stages, the super-special, the four bonus cars (Escort Mk II, RS200, Delta Integrale,
  Quattro) and expert difficulty available in the menus. It is not saved: the memory card
  keeps only what was won, so turning it off locks them again
- Extra cars from car folders: each folder in `cars/` (beside the program or in the settings
  folder) adds a car to the end of the car menu; left and right on it step through them. See
  [Extra cars](#extra-cars)
- Memory card saves in the standard raw 128 KB `.mcd` format
- Full audio: SPU with hardware reverb, CD-DA music, XA speech/streams
- Smooth pacing on any monitor, following the window to another one: on multiples of 60 Hz
  the buffer swaps pace emulation, at other rates a timer paces it at the console's
  59.94 Hz and each image waits for the next refresh; dynamic audio rate control absorbs
  the difference
- Settings persisted in `cmrally.ini`

## Building

Requirements: CMake 3.21+, a C11 compiler, Python 3, SDL3 (3.2 or newer) and a GPU with
Vulkan (Linux, Windows) or Metal (macOS).
Developed on macOS with Clang. Linux builds with GCC are checked in a Debian container
(`tools/docker/Dockerfile`), which also runs the game on Mesa's software Vulkan driver. The
code avoids compiler-specific extensions outside the optional debug trace, so MSVC (Windows)
builds are expected to work but have not been tested yet.

```bash
brew install sdl3 cmake ninja      # macOS; use your package manager elsewhere
```

If CMake does not find SDL3 (typically on Windows), set the `CMAKE_PREFIX_PATH`
environment variable to the SDL3 install directory before running the build script.

Then point the build script at your disc image (the `.cue` of the NTSC-U release with its
`.bin` tracks):

```bash
python3 tools/build_game.py "/path/to/Colin McRae Rally.cue"
```

This extracts the executables from the disc, checks they are the supported release,
recompiles them to C (`generated/`), builds `build/cmrally`, and remembers the disc
location. Then:

```bash
./build/cmrally
```

## Controls

With a gamepad connected the game sees a **DualShock** (`controller = auto`, the default,
switches as gamepads come and go). The game puts the pad in digital mode but leaves the
ANALOG button unlocked; the port presses it for you, so the left stick steers and the game's
vibration reaches the gamepad. The Guide/Home button (or F7) is the ANALOG button if you
prefer digital mode. In analog mode the d-pad also steers (full lock).

| PlayStation | Gamepad | Keyboard |
|---|---|---|
| Steering | Left stick or d-pad | Arrow keys |
| Cross | South (A) | X |
| Circle | East (B) | C |
| Square | West (X) | Z |
| Triangle | North (Y) | S |
| L1 / R1 | Shoulders | Q / W |
| L2 / R2 | Triggers | A / D |
| Start / Select | Start / Back | Enter / Right Shift |

Without a gamepad the keyboard drives a digital pad. A second gamepad is connected to port 2
for the game's two-player split-screen mode. `--controller negcon` presents Namco's
NeGcon instead, which the game also supports: stick twist steering plus analog throttle (right
trigger) and brake (left trigger), but no vibration. The game has its own controller options
in the pause menu.

Hotkeys: F11 or Alt+Enter fullscreen, F2 texture filtering, F3 dithering (original
renderer), F4 high-resolution vs original renderer, F5 sub-pixel precision, F7 DualShock
ANALOG button, F8 anti-aliasing (off/2x/4x/8x), Tab
fast-forward, Shift+Esc quit.

## Options

```
--fullscreen      start fullscreen
--window N        window size as a multiple of 320x240 (default 3)
--scale N         internal resolution multiplier (default: match the window)
--no-widescreen   4:3 as on the console (default: 16:9 with a widened 3D projection)
--no-filter       no bilinear texture filtering
--msaa N          anti-aliasing samples: 0 (off), 2, 4 (default) or 8
--software        original 1x software renderer
--no-pgxp         disable sub-pixel vertex precision
--no-vsync        do not sync presentation to the display
--controller T    auto (DualShock with a gamepad, else digital), dualshock, negcon, digital
--no-rumble       disable vibration
--no-fast-loading load at the original CD speed
--no-race-60fps   races at the game's own 30 fps
--draw-distance X draw distance multiplier on stages without fog, 1 to 2 (default 2)
--unlock-all      every country, stage, car and expert mode (--no-unlock-all: off, default)
--mute            no audio output
--cpu N           emulated CPU cost per instruction (default 2; 1 = faster CPU)
```

Settings and memory cards live in the user preferences folder
(`~/Library/Application Support/cmrally/Colin McRae Rally/` on macOS).

## Extra cars

A car folder holds `car.ini` and any of the files the disc has for each car (`DAY.BFX`,
`NIGHT.BFX`, `MEDIUM.BFX`, `DAYTIM.BFX`, `NIGHTTIM.BFX`, `MEDIUM.TIM`, `SOUNDS.BFX`; their format
is in `docs/car-files.md`):

```
name = SUBARU VIVIO RX-RA     # car menu name
short_name = VIVIO RX-RA
base = SUBARU                 # disc car for handling, sounds and records
bhp = 63                      # shown in the car menu
wheelbase = 2310
drive = 4WD                   # 4WD, FWD or RWD, shown in the car menu
mass = 750                    # kg (optional; every disc car is 1120)
```

`tools/gt2car.py` also writes where the lamps are, for the glows and the brake lights the game
draws over them (`head_lamp`, `fog_lamp`, `tail_lamp`), and how far the base car's physics
lowers smaller wheels (`wheel_drop`, for the car menu's model, which has no physics).

Files the folder lacks come from the base car, whose class the mode's rules (kit cars, bonus
cars) also apply to. The car menu shows the car's `MEDIUM` model. The frontend saves the base
car, so a saved championship continues with it. A car keeps its base's collision box, driver's
view (steering wheel, arms, wipers) and engine sound unless it has its own. `--no-extra-cars`
ignores the folders. Two tools make car folders from your own discs; the results are not for
sharing:

- `tools/gt2car.py` converts a Gran Turismo 2 car (NTSC-U; the Simulation disc has every car).
  `--list vivio` finds its id. The body is GT2's most detailed model split into the game's
  damage panels, its night model for night stages and the middle one for split screen and
  the car menu; wheels, driver's view and engine come from the base car. `--paint RRGGBB`
  repaints it and `--rim N` resizes the base car's rims to N inches. The example, from the
  Simulation disc:
  `python3 tools/gt2car.py GT2.bin s2v4n build/cars/vivio --bhp 63 --mass 750
  --wheelbase 2310 --short-name "VIVIO RX-RA" --paint EBA92E --rim 13`
- `tools/carsound.py` makes an engine sound from a disc car's, optionally revving higher,
  supercharged and without the turbo's dump valve: `python3 tools/carsound.py
  "Colin McRae Rally.cue" VWGOLF build/cars/vivio/SOUNDS.BFX --pitch 1.19 --whine 0.2 --blowoff 0`

`tools/cmrexport.py` renders a car folder (OBJ export, views, its VRAM area) to check a car
before driving it.

## How it works

- `tools/recomp.py` translates each executable (boot loader, frontend, game, rally school)
  into C. Functions are discovered from a Ghidra analysis (`tools/functions/`), jal targets
  and code pointers; branches and delay slots map to gotos, `jal` to C calls, jump tables
  to `switch`. `tools/hooks.txt` attaches native code to specific game functions, each hook
  given the recompiled original to call: the 60 fps races (`runtime/race60.c`) hook the main
  loop, the controller poll, the race and replay updates, the scene renderer and the shadow
  drawing; the draw distance and view cone are raised around the scene renderer and the
  primitive buffers swapped in the main loop (`runtime/view.c`); unlock all (`runtime/unlock.c`)
  wraps the frontend's progress loading, merging and copying (and the BIOS card write) so saves
  keep the player's own unlocks. Extra cars (`runtime/cars.c`) hook the game's file lookup,
  serving car folder files at CD sectors past the end of the disc, the car folder path, the
  physics setup, and the frontend's car menu, rows and showroom objects. Game addresses are in
  `runtime/symbols.h`.
- The recompiler's warnings (data decoded as code, jumps through registers) are checked against
  a reviewed list in `tools/warnings/`; generation fails on a new one until it is looked at and
  accepted with `--accept-warnings`.
- The game rewrites parts of its own rendering code while it runs: the ambient light colour
  and the screen clipping bounds are patched into instruction immediates, and mirrored views
  flip a culling branch. The recompiler finds these patch sites (stores to constant code
  addresses, also through a pointer a caller passes in) and makes the generated code read
  those fields from RAM. `tools/smc_check.py` compares a RAM dump against the executable to
  confirm no rewritten instruction is missed.
- `runtime/` replaces the console: HLE BIOS (interrupt chains, events, memory card
  filesystem, `DoExecute` switching between executables), GTE, GPU (software rasterizer
  for the authoritative 1x VRAM), SPU, CD-ROM controller (XA-ADPCM, CD-DA), MDEC, DMA,
  timers and the controller port.
- `runtime/render_gpu.c` redraws each frame at high resolution when the game flips its
  display, through SDL_GPU: a frame's vertices and texture uploads go to the GPU in one copy
  pass and are drawn in a single render pass where possible. With anti-aliasing, each pass
  draws into a multisampled texture seeded from the current image and resolves back, so the
  samples never leave the GPU's tile memory on tile-based GPUs. The shaders
  (`runtime/shaders/`) are compiled to SPIR-V and Metal by `tools/build_shaders.py` (glslc,
  spirv-cross) into `runtime/gpu_shaders.h`, which is committed, so building the game does
  not need those tools. `SDL_GPU_DRIVER=vulkan` forces Vulkan where Metal is the default
  (macOS with MoltenVK installed); `runtime/pgxp.c` carries precise vertex positions and vertex identities from the
  GTE (and from where each input vertex was loaded) to the renderer, through every word the
  recompiled code loads and stores, which uses them for wobble-free geometry. Vertices the
  GTE can only clamp (too near or behind the camera) keep their true depth, so the renderer
  draws those polygons in 3D and the GPU cuts them at the camera plane; the game's own
  backface test (NCLIP) then uses the true facing.
- `ghidra/` holds the headless Ghidra scripts used to produce the function lists
  (`GHIDRA_HOME=/path/to/ghidra ghidra/run.sh`, with JDK 21 on the path or in `JAVA_HOME`).

## Status

Boots through the FMVs and menus into championship stages, pausing and quitting back to the
frontend; Rally School; the attract-mode demo; two-player split screen; memory card save and
load. Verified in real time at 60 emulated fps with the high-resolution renderer on an
Apple Silicon Mac (120 Hz display, Metal), and on Linux with Vulkan (Mesa lavapipe in a
container), with identical output and no Vulkan validation errors. Races run at 60 fps;
stage finishes, results, the next stage and two-player split screen work in that mode, and the
car's path is identical to 30 fps tick for tick. A full championship has not been played
through, so later stages and end-of-rally screens are less tested.

Regression test (needs your disc, so it runs locally): record references once on a good build
with `python3 tools/regress.py build/cmrally record`, then `ctest --test-dir build` compares
race screenshots, with and without unlock-all, and checks that the car's path is identical at
30 and 60 fps. It also checks that `tools/cmrcar.py` reads and writes every car file on the disc
byte for byte.

Development aids: `--log N`, `--turbo`, `--hidden`, `--exit-after N`, `--shot-every N --shot-dir D`,
`--script "from-to:buttons[:twist[:throttle]];..."` (`--script2` for port 2), `--wav file.wav`,
`--save-dir D`, the recent-function trace and shadow call stack printed on fatal errors, and
environment variables: `CMR_DUMP_PRIMS=<frame>` (raw GPU commands), `CMR_DUMP_GTE=<frame>`
(GTE command use and lighting registers), `CMR_TRACE_LIGHT=1` (light matrix/ambient writes
with callers), `CMR_WATCH=addr,...` (log RAM word changes per vblank),
`CMR_POKE=frame:addr=value|@addr,...` (write RAM words at a frame), `CMR_DUMP_RAM=file` (RAM
at exit), `CMR_ANALOG_AT=<frame>` (press ANALOG on port 1), `CMR_GPU_DEBUG=1` (SDL_GPU
debug mode with the Vulkan validation layers when installed), `CMR_REFRESH_HZ=<hz>` (pretend
the monitor runs at that rate), `CMR_TRACE_LOAD=1` (log when the CD is loading, streaming or
playing music, which drives fast loading), `CMR_DUMP_HW=<frame>` (the triangles of one frame
as the renderer draws them), `CMR_TRACE_60=1` (60 fps race updates and draws, and the car
position per update), `CMR_TRACE_PRIMBUF=1` (primitive buffer use per frame),
`CMR_CULL_MARGIN=<k>` (view cone margin, 1 = none).
