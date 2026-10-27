#!/usr/bin/env python3
"""Disassemble a range of a module: dis.py <module> <start-hex> [count]"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Image  # noqa: E402
from recomp import MODULES  # noqa: E402

name, start = sys.argv[1], int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 32
bins = os.environ.get("CMR_BINS", "game/bins")
for n, fn, base, entry in MODULES:
    if n == name:
        img = Image(os.path.join(bins, fn), base, entry, n)
        for k in range(count):
            a = start + k * 4
            print(f"{a:08x}: {img.word(a):08x}  {img.insn(a)}")
