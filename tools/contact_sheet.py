#!/usr/bin/env python3
"""Make a PNG contact sheet from frameNNNNN.bmp screenshots: contact_sheet.py <dir> <first> <last> [out]"""
import glob
import os
import struct
import sys
import zlib


def load(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        ry = (abs(h) - 1 - y) if h > 0 else y
        rows.append(d[off + ry * stride: off + ry * stride + w * bpp])
    return w, abs(h), bpp, rows


def main():
    d, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    out = sys.argv[4] if len(sys.argv) > 4 else os.path.join(d, "sheet.png")
    files = [f for f in sorted(glob.glob(os.path.join(d, "frame*.bmp")))
             if first <= int(os.path.basename(f)[5:10]) <= last]
    tw, th, cols = 213, 160, 4
    tiles = []
    for f in files:
        w, h, bpp, rows = load(f)
        t = []
        for ty in range(th):
            r = rows[ty * h // th]
            line = bytearray()
            for tx in range(tw):
                sx = tx * w // tw * bpp
                line += bytes((r[sx + 2], r[sx + 1], r[sx]))
            t.append(line)
        tiles.append(t)
    n = len(tiles)
    W, H = tw * cols, th * ((n + cols - 1) // cols)
    img = [bytearray(W * 3) for _ in range(H)]
    for i, t in enumerate(tiles):
        cx, cy = (i % cols) * tw, (i // cols) * th
        for y in range(th):
            img[cy + y][cx * 3:(cx + tw) * 3] = t[y]
    raw = b"".join(b"\0" + bytes(r) for r in img)

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    print(out, n, "frames")


if __name__ == "__main__":
    main()
