# Colin McRae Rally — PC port

A native PC port of the PlayStation release of *Colin McRae Rally* (NTSC-U, SCUS-94474),
built by statically recompiling the game's MIPS R3000 executables to C and running them on
a native runtime that replaces the PlayStation hardware and BIOS. SDL3 provides the window,
rendering, audio and controller input.

No game data is included. You need your own disc image (`.cue` + `.bin` tracks).

Work in progress — see the commit history for the current state.
