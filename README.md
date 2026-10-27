# Colin McRae Rally — PC port

A native PC port of the PlayStation release of *Colin McRae Rally* (NTSC-U, SCUS-94474).
The game's four MIPS executables are statically recompiled to C and run on a native runtime
that replaces the PlayStation hardware and BIOS. Rendering, audio and input go through SDL3
and OpenGL.

No game data is included or distributed: you build the port from your own disc image.

## Features

- Native-resolution rendering: every primitive is redrawn at the window's pixel resolution
  in true colour (up to 16x the original 320x240)
- Sub-pixel vertex precision and perspective-correct texturing (no polygon wobble or
  texture warping), from the GTE's unrounded results
- Optional 16:9 widescreen with a widened 3D field of view
- Optional bilinear texture filtering
- Controllers through SDL3 (any gamepad SDL knows), plus keyboard; DualShock protocol
  emulated, including rumble commands
- Memory card saves in the standard 128 KB `.mcd` format (compatible with emulators)
- Full audio: SPU with hardware reverb, CD-DA music, XA speech/streams
- Presentation independent of the display refresh rate (vsync on 60 Hz displays, timer
  pacing on high-refresh displays, dynamic audio rate control)
- Settings persisted in `cmrally.ini`

## Building

Requirements: CMake 3.20+, a C11 compiler (Clang or GCC), Python 3, SDL3, OpenGL 3.3.

```bash
brew install sdl3 cmake ninja      # macOS; use your package manager elsewhere
```

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

| PlayStation | Gamepad | Keyboard |
|---|---|---|
| D-pad | D-pad / left stick | Arrow keys |
| Cross | South (A) | X |
| Circle | East (B) | C |
| Square | West (X) | Z |
| Triangle | North (Y) | S |
| L1 / R1 | Shoulders | Q / W |
| L2 / R2 | Triggers | A / D |
| Start / Select | Start / Back | Enter / Right Shift |

Hotkeys: F11 or Alt+Enter fullscreen, F2 texture filtering, F3 dithering (original
renderer), F4 high-resolution vs original renderer, F5 sub-pixel precision, Tab
fast-forward, Shift+Esc quit.

## Options

```
--fullscreen      start fullscreen
--window N        window size as a multiple of 320x240 (default 3)
--scale N         internal resolution multiplier (default: match the window)
--widescreen      16:9 output with a widened 3D projection
--filter          bilinear texture filtering
--software        original 1x software renderer
--no-pgxp         disable sub-pixel vertex precision
--no-vsync        do not sync presentation to the display
--analog          present an analog controller (DualShock) instead of a digital pad
--cpu N           emulated CPU cost per instruction (default 2; 1 = faster CPU)
```

Settings and memory cards live in the user preferences folder
(`~/Library/Application Support/cmrally/Colin McRae Rally/` on macOS).

## How it works

- `tools/recomp.py` translates each executable (boot loader, frontend, game, rally school)
  into C. Functions are discovered from a Ghidra analysis (`tools/functions/`), jal targets
  and code pointers; branches and delay slots map to gotos, `jal` to C calls, jump tables
  to `switch`. `tools/hooks.txt` attaches native code to specific game functions.
- `runtime/` replaces the console: HLE BIOS (interrupt chains, events, memory card
  filesystem, `DoExecute` switching between executables), GTE, GPU (software rasterizer
  for the authoritative 1x VRAM), SPU, CD-ROM controller (XA-ADPCM, CD-DA), MDEC, DMA,
  timers and the controller port.
- `runtime/render_gl.c` mirrors all drawing at high resolution; `runtime/pgxp.c` carries
  precise vertex positions from the GTE to the renderer.
- `ghidra/` holds the headless Ghidra scripts used to produce the function lists.

## Status

Boots through the FMVs and menus into championship stages. The game logic runs at its
original 30 frames per second; see the commit history for current work.

Development aids: `--log N`, `--turbo`, `--exit-after N`, `--shot-every N --shot-dir D`,
`--script "from-to:buttons;..."`, `--wav file.wav`, and the recent-function trace printed on
fatal errors.
