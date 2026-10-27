#!/usr/bin/env python3
"""Survey the executables: BIOS calls, syscalls and hardware registers touched."""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Insn  # noqa: E402

MODULES = [("scus", 0x80013000), ("frontend", 0x80130000),
           ("game", 0x80013000), ("rschool", 0x80013000)]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "game/bins"
    for name, base in MODULES:
        d = open(f"{root}/{name}.bin", "rb").read()
        n = len(d) // 4
        w = struct.unpack("<%dI" % n, d[:n * 4])
        calls = []
        for k in range(n - 3):
            if w[k] in (0x240A00A0, 0x240A00B0, 0x240A00C0) and w[k + 1] == 0x01400008:
                t = w[k + 2]
                if (t >> 16) == 0x2409:
                    calls.append((base + k * 4, w[k] & 0xFF, t & 0xFFFF))
        sysc = sum(1 for x in w if (x & 0xFC00003F) == 0x0000000C)
        print(f"{name}: {len(calls)} BIOS stubs, {sysc} syscall insns")
        print("   " + " ".join(f"{a:02X}:{f:02X}@{p:08x}" for p, a, f in sorted(calls, key=lambda c: (c[1], c[2]))))
        # hardware register offsets: lui rX,0x1f80 followed by a load/store using rX
        hw = collections.Counter()
        for k in range(n):
            if (w[k] & 0xFFE0FFFF) == 0x3C001F80:
                r = (w[k] >> 16) & 31
                for j in range(k + 1, min(k + 12, n)):
                    i = Insn(base + j * 4, w[j])
                    if i.op in ("lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw") and i.rs == r:
                        hw[0x1F800000 + i.simm] += 1
                    if r in i.writes():
                        break
        print("   hw:", " ".join(f"{a:08x}" for a in sorted(hw)))


if __name__ == "__main__":
    main()
