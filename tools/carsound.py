#!/usr/bin/env python3
"""Make a car's engine sound (SOUNDS.BFX) from a disc car's, read from your own disc.

Usage: carsound.py <Colin McRae Rally .cue> <disc car> <out SOUNDS.BFX> [--pitch X] [--whine X]
                   [--blowoff V]

The engine loops are resampled to play --pitch times higher (the game plays them faster with
engine speed and tops out near 8000 rpm, so 1.19 makes that 9500), and --whine mixes in a
supercharger whine at that level (0.2 is clearly audible) that rises with engine speed.
--blowoff sets the volume (0-127, 0 silent) of the turbo dump valve the game plays when the
throttle closes.
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_game import data_track, read_file  # noqa: E402

RATE = 44100           # what the game's pitch 0x1000 plays at
TOP_PITCH = 0.69       # the engine loops' pitch at the top of the rev range
WHINE_HZ = 1600        # supercharger whine at the top of the rev range
ENGINE_PROGRAMS = (0, 1, 2, 3, 5, 6)
BLOWOFF_PROGRAMS = (78, 81)   # played when the throttle closes
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


# ------------------------------------------------------------------ SPU ADPCM
def decode(data):
    """Samples and per-block flags of an ADPCM sample."""
    out, flags = [], []
    s1 = s2 = 0
    for i in range(0, len(data), 16):
        shift, f = data[i] & 15, min(data[i] >> 4, 4)
        k0, k1 = FILTERS[f]
        flags.append(data[i + 1])
        for j in range(28):
            n = (data[i + 2 + j // 2] >> (4 * (j & 1))) & 15
            s = ((n - 16 if n & 8 else n) << 12) >> shift
            s = max(-32768, min(32767, s + ((s1 * k0 + s2 * k1 + 32) >> 6)))
            s2, s1 = s1, s
            out.append(s)
    return out, flags


def encode_block(samples, s1, s2):
    """The best of every filter and shift for 28 samples: (bytes 2..15, s1, s2, error)."""
    best = None
    for f, (k0, k1) in enumerate(FILTERS):
        for shift in range(13):
            a, b, err, nibs = s1, s2, 0, []
            for x in samples:
                p = (a * k0 + b * k1 + 32) >> 6
                n = max(-8, min(7, round(((x - p) << shift) / 4096)))
                s = max(-32768, min(32767, ((n << 12) >> shift) + p))
                err += (x - s) ** 2
                if best and err >= best[3]:
                    break
                nibs.append(n & 15)
                b, a = a, s
            else:
                body = bytes([shift | f << 4, 0]) + bytes(nibs[k] | nibs[k + 1] << 4 for k in range(0, 28, 2))
                best = (body, a, b, err)
    return best


def encode(samples, loop_start):
    """ADPCM in the disc's layout: a silent block, then the sample looping from loop_start."""
    blocks = [bytes(16)]
    s1 = s2 = 0
    n = len(samples) // 28
    for i in range(n):
        body, s1, s2, _ = encode_block(samples[28 * i:28 * i + 28], s1, s2)
        flag = 3 if i == n - 1 else 6 if i * 28 == loop_start else 2
        blocks.append(body[:1] + bytes([flag]) + body[2:])
    return b"".join(blocks)


# ------------------------------------------------------------------ the sound bank
def chunks(bfx):
    out, pos = [], 0
    while pos + 4 <= len(bfx):
        n = struct.unpack_from("<I", bfx, pos)[0]
        out.append(bfx[pos + 4:pos + 4 + n])
        pos += 4 + n
    return out


def engine_vags(vab):
    """VAG numbers the engine programs play."""
    used = [p for p in range(128) if vab[32 + 16 * p]]
    tones = 32 + 16 * 128
    return sorted({struct.unpack_from("<H", vab, tones + 32 * 16 * i + 22)[0]
                   for i, p in enumerate(used) if p in ENGINE_PROGRAMS})


def resample(x, length, wrap):
    """x stretched to `length` samples (Catmull-Rom; a loop wraps around)."""
    n = len(x)
    at = (lambda k: x[k % n]) if wrap else (lambda k: x[max(0, min(n - 1, k))])
    out = []
    for i in range(length):
        t = i * n / length
        k = int(t)
        u = t - k
        p0, p1, p2, p3 = at(k - 1), at(k), at(k + 1), at(k + 2)
        v = p1 + 0.5 * u * (p2 - p0 + u * (2 * p0 - 5 * p1 + 4 * p2 - p3 + u * (3 * (p1 - p2) + p3 - p0)))
        out.append(max(-32768, min(32767, round(v))))
    return out


def change_loop(data, pitch, whine):
    samples, flags = decode(data)
    start = next(i for i, f in enumerate(flags) if f & 4) * 28
    blocks = lambda n: max(28, round(n / pitch / 28) * 28)
    head = resample(samples[28:start], blocks(start - 28), False) if start > 28 else []
    loop = resample(samples[start:], blocks(len(samples) - start), True)
    out = head + loop
    if whine:
        # whole cycles over the loop, so it repeats without a click
        cycles = max(1, round(WHINE_HZ / TOP_PITCH * len(loop) / RATE))
        peak = max(abs(x) for x in loop) or 1
        w = [math.sin(2 * math.pi * cycles * (i - len(head)) / len(loop)) for i in range(len(out))]
        out = [max(-32768, min(32767, round(x * (1 - whine) + peak * whine * (s + 0.3 * (2 * s * s - 1)) / 1.3)))
               for x, s in zip(out, w)]
    return encode(out, len(head))


def make(bfx, pitch, whine, blowoff=None):
    vab, vb = chunks(bfx)[:2]
    nprogs = struct.unpack_from("<H", vab, 18)[0]
    if blowoff is not None:
        vab = bytearray(vab)
        used = [p for p in range(128) if vab[32 + 16 * p]]
        for i, p in enumerate(used):
            if p in BLOWOFF_PROGRAMS:
                for t in range(vab[32 + 16 * p]):
                    vab[32 + 16 * 128 + 32 * (16 * i + t) + 2] = blowoff   # tone volume
        vab = bytes(vab)
    table = 32 + 16 * 128 + 32 * 16 * nprogs
    sizes = list(struct.unpack_from("<256H", vab, table))
    vags, pos = [], 0
    for s in sizes:
        vags.append(vb[pos:pos + s * 8])
        pos += s * 8
    for v in engine_vags(vab):
        if vags[v] and decode(vags[v])[1][-1] & 3 == 3:
            vags[v] = change_loop(vags[v], pitch, whine)
            sizes[v] = len(vags[v]) // 8
    vab = bytearray(vab)
    struct.pack_into("<256H", vab, table, *sizes)
    vb = b"".join(vags)
    struct.pack_into("<I", vab, 12, len(vab) + len(vb))
    return struct.pack("<I", len(vab)) + bytes(vab) + struct.pack("<I", len(vb)) + vb


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cue")
    ap.add_argument("car", help="disc car folder, e.g. VWGOLF")
    ap.add_argument("out")
    ap.add_argument("--pitch", type=float, default=1.0)
    ap.add_argument("--whine", type=float, default=0.0)
    ap.add_argument("--blowoff", type=int, help="dump valve volume 0-127 (default: unchanged)")
    a = ap.parse_args()
    with open(data_track(a.cue), "rb") as img:
        bfx = read_file(img, f"CMRALLY/CARS/{a.car.upper()}/SOUNDS.BFX")
    out = make(bfx, a.pitch, a.whine, a.blowoff)
    with open(a.out, "wb") as f:
        f.write(out)
    print(f"{a.out}: {len(out)} bytes")


if __name__ == "__main__":
    main()
