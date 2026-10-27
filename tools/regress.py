#!/usr/bin/env python3
"""Regression test, run against your own disc (local only).

Usage: regress.py <cmrally> record|check [--ref DIR]

Drives the game into a race with a scripted controller and compares screenshots, with and
without unlock-all, against hashes recorded earlier on this machine (renderers differ between
GPUs, so references are not shared). Also checks that the car's position at every race update
is the same at 30 and 60 fps. Exits 77 (skipped) when no disc is set up.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

RACE = ("1100-1105:8;1600-1605:8;2000-2005:4000;2400-2405:4000;3200-3205:4000;3500-3505:4000;"
        "3800-3805:4000;4100-4105:4000;4300-5400:4000")
COMMON = ["--mute", "--hidden", "--turbo", "--scale", "1", "--controller", "digital", "--exit-after", "5000",
          "--script", RACE]
SHOTS = {"race": [], "race-unlock": ["--unlock-all"]}
TICKS = {"ticks-30": ["--no-race-60fps"], "ticks-60": ["--race-60fps"]}


def start(exe, tmp, name, args, env=None):
    save = os.path.join(tmp, name)
    os.makedirs(os.path.join(save, "shots"))
    log = os.path.join(tmp, name + ".log")
    with open(log, "w") as f:
        p = subprocess.Popen([exe, "--save-dir", save] + COMMON + args, env=env, stdout=f, stderr=subprocess.STDOUT)
    return name, p, save, log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("mode", choices=["record", "check"])
    ap.add_argument("--ref", help="reference directory (default: regress/ next to the executable)")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    ref = a.ref or os.path.join(os.path.dirname(exe), "regress")
    if not os.path.exists(os.path.join(os.path.dirname(exe), "disc_path.txt")):
        print("no disc set up (run tools/build_game.py); skipped")
        return 77
    if a.mode == "check" and not os.path.isdir(ref):
        sys.exit(f"no references in {ref}: run 'regress.py {a.exe} record' on a known good build first")

    shutil.rmtree(os.path.join(ref, "failed"), ignore_errors=True)
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        runs = [start(exe, tmp, n, args + ["--shot-every", "25", "--shot-from", "500",
                                           "--shot-dir", os.path.join(tmp, n, "shots")])
                for n, args in SHOTS.items()]
        runs += [start(exe, tmp, n, args, dict(os.environ, CMR_TRACE_60="1")) for n, args in TICKS.items()]
        for name, p, _, log in runs:
            if p.wait():
                sys.exit(f"{name}: cmrally exited with {p.returncode}\n" + open(log, errors="replace").read()[-2000:])
        out = {name: (save, open(log, errors="replace").read()) for name, _, save, log in runs}

        for name in SHOTS:
            d = os.path.join(out[name][0], "shots")
            hashes = {f: hashlib.sha1(open(os.path.join(d, f), "rb").read()).hexdigest() for f in sorted(os.listdir(d))}
            path = os.path.join(ref, name + ".txt")
            if a.mode == "record":
                os.makedirs(ref, exist_ok=True)
                with open(path, "w") as f:
                    f.write("".join(f"{h} {n}\n" for n, h in hashes.items()))
                print(f"{name}: recorded {len(hashes)} screenshots")
                continue
            want = dict(line.split()[::-1] for line in open(path))
            diff = sorted(n for n in set(want) | set(hashes) if want.get(n) != hashes.get(n))
            if diff:
                failed = True
                keep = os.path.join(ref, "failed", name)
                os.makedirs(keep, exist_ok=True)
                for n in diff:
                    if n in hashes:
                        os.replace(os.path.join(d, n), os.path.join(keep, n))
                print(f"{name}: {len(diff)} of {len(want)} screenshots differ, first {diff[0]} (kept in {keep})")
            else:
                print(f"{name}: {len(hashes)} screenshots match")

        # the game's own output can share a line with ours
        t30, t60 = ([line[line.index("tick pos"):] for line in out[n][1].splitlines() if "tick pos" in line]
                    for n in TICKS)
        if not t30 or t30 != t60:
            failed = True
            n = next((i for i, (x, y) in enumerate(zip(t30, t60)) if x != y), min(len(t30), len(t60)))
            print(f"ticks: 30 and 60 fps differ at update {n} of {len(t30)}")
        else:
            print(f"ticks: {len(t30)} updates match at 30 and 60 fps")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
