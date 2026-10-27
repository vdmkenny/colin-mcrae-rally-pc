#!/usr/bin/env python3
"""Check a RAM dump for code the program rewrote that the recompiler does not handle.

Usage: smc_check.py <ram.bin> <module>   (dump with CMR_DUMP_RAM=ram.bin while <module> runs)

Lists every recompiled instruction whose word in RAM differs from the executable, marking
the ones the generated code reads from RAM (see Module.find_smc in recomp.py).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recomp import MODULES, Module, load_extra, load_ghidra  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))


def main():
    ram = open(sys.argv[1], "rb").read()
    name = sys.argv[2]
    bins = os.environ.get("CMR_BINS", "game/bins")
    for n, fn, base, entry in MODULES:
        if n != name:
            continue
        m = Module(n, os.path.join(bins, fn), base, entry,
                   load_ghidra(os.path.join(TOOLS, "functions", f"{n}.txt")),
                   load_extra(os.path.join(TOOLS, "entries", f"{n}.txt")))
        m.discover()
        m.find_smc()
        code = set()
        for e in m.entries:
            code |= m.walk(e)[0]
        missing = 0
        for a in sorted(code):
            now = struct.unpack_from("<I", ram, a & 0x1FFFFC)[0]
            disc = m.words[(a - base) >> 2]
            if now != disc:
                kind = m.smc.get(a)
                missing += kind is None
                print(f"{a:08x} {disc:08x} -> {now:08x}  {m.insn(a)!s:28} {kind or 'NOT HANDLED'}")
        print(f"{missing} rewritten instructions not handled")
        return 1 if missing else 0
    sys.exit(f"unknown module {name}")


if __name__ == "__main__":
    sys.exit(main())
