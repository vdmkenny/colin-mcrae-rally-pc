#!/usr/bin/env python3
"""One-step build: extract the executables from your disc image, recompile them, build.

Usage: python3 tools/build_game.py "/path/to/Colin McRae Rally.cue" [--build-dir build]

Only the NTSC-U release (SCUS-94474) is supported; the executables are checked by hash.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXES = {
    # disc path -> (module, sha1 of the code body)
    "SCUS_944.74": ("scus", "894ac453d7d90c8a171930ed94511cbbefcc3d8a"),
    "CMRALLY/FRONTEND.EXE": ("frontend", "6080b6de81601e613c6c4908870a3b010d9eab95"),
    "CMRALLY/GAME.EXE": ("game", "756651d9c92c2d3dfb024595449e4ebeb50015f4"),
    "CMRALLY/R_SCHOOL.EXE": ("rschool", "940404d5cef94f7c509f8c3095cef8f92eaf16ad"),
}


def data_track(cue):
    base = os.path.dirname(os.path.abspath(cue))
    for line in open(cue, encoding="latin-1"):
        line = line.strip()
        if line.upper().startswith("FILE"):
            name = line[line.index('"') + 1:line.rindex('"')]
            return os.path.join(base, name)
    sys.exit("no FILE entry in the cue sheet")


def read_file(img, path):
    def sector(lba):
        img.seek(lba * 2352 + 24)
        return img.read(2048)
    pvd = sector(16)
    if pvd[1:6] != b"CD001":
        sys.exit("track 1 is not an ISO9660 MODE2/2352 image")
    lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    for part in path.split("/"):
        data = b"".join(sector(lba + i) for i in range((size + 2047) // 2048))
        found = None
        pos = 0
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            name = data[pos + 33:pos + 33 + data[pos + 32]].decode("ascii", "replace").split(";")[0]
            if name.upper() == part.upper():
                found = struct.unpack_from("<I", data, pos + 2)[0], struct.unpack_from("<I", data, pos + 10)[0]
                break
            pos += ln
        if not found:
            sys.exit(f"{path} not found on the disc - is this Colin McRae Rally (NTSC-U)?")
        lba, size = found
    return b"".join(sector(lba + i) for i in range((size + 2047) // 2048))[:size]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cue")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--config", default="Release")
    args = ap.parse_args()

    bins = os.path.join(ROOT, "game", "bins")
    os.makedirs(bins, exist_ok=True)
    with open(data_track(args.cue), "rb") as img:
        for path, (module, sha1) in EXES.items():
            body = read_file(img, path)[0x800:]
            digest = hashlib.sha1(body).hexdigest()
            if digest != sha1:
                sys.exit(f"{path} does not match the supported release (sha1 {digest})")
            with open(os.path.join(bins, f"{module}.bin"), "wb") as f:
                f.write(body)
            print(f"extracted {path}")

    gen = os.path.join(ROOT, "generated")
    shutil.rmtree(gen, ignore_errors=True)
    subprocess.check_call([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp.py"), bins, gen])

    cmake = ["cmake", "-S", ROOT, "-B", args.build_dir, f"-DCMAKE_BUILD_TYPE={args.config}"]
    if shutil.which("ninja"):
        cmake += ["-G", "Ninja"]
    if sys.platform == "darwin" and os.path.isdir("/opt/homebrew/opt/sdl3"):
        cmake.append("-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/sdl3")
    subprocess.check_call(cmake)
    subprocess.check_call(["cmake", "--build", args.build_dir, "--parallel", "--config", args.config])

    # single-config generators put the program in the build directory, Visual Studio in a
    # per-configuration subdirectory; the disc location goes next to it
    exe = None
    for sub in ("", args.config):
        for name in ("cmrally", "cmrally.exe"):
            candidate = os.path.join(args.build_dir, sub, name)
            if os.path.isfile(candidate):
                exe = candidate
    if not exe:
        sys.exit("build finished but the cmrally executable was not found")
    with open(os.path.join(os.path.dirname(exe), "disc_path.txt"), "w") as f:
        f.write(os.path.abspath(args.cue) + "\n")
    print(f"\nDone. Run: {exe}")


if __name__ == "__main__":
    main()
