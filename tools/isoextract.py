#!/usr/bin/env python3
"""Extract the ISO9660 filesystem from a PS1 MODE2/2352 track image.

Usage: isoextract.py <track1.bin> <outdir>
Writes every file plus a listing (files.txt) with LBA and size, which the
runtime uses to map CD reads back to files.
"""
import os
import struct
import sys

RAW = 2352


def read_sector(f, lba):
    f.seek(lba * RAW)
    raw = f.read(RAW)
    # Mode 2 Form 1: 12 sync + 4 header + 8 subheader + 2048 data
    return raw[24:24 + 2048], raw


def parse_dir(f, lba, size, path, out, listing):
    data = b""
    for i in range((size + 2047) // 2048):
        data += read_sector(f, lba + i)[0]
    pos = 0
    while pos < size:
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + ln]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace")
        if ";" in name:
            name = name.split(";")[0]
        full = path + "/" + name
        if flags & 2:
            listing.append((full + "/", ext_lba, ext_size, 0))
            os.makedirs(out + full, exist_ok=True)
            parse_dir(f, ext_lba, ext_size, full, out, listing)
        else:
            # detect form2 (XA/STR) files by subheader of first sector
            _, raw = read_sector(f, ext_lba)
            submode = raw[18]
            form2 = bool(submode & 0x20)
            listing.append((full, ext_lba, ext_size, 1 if form2 else 0))
            with open(out + full, "wb") as o:
                nsec = (ext_size + 2047) // 2048
                for i in range(nsec):
                    d, raw = read_sector(f, ext_lba + i)
                    if raw[18] & 0x20:
                        # form 2: keep full 2336 bytes after header for STR/XA
                        o.write(raw[16:16 + 2336])
                    else:
                        o.write(d)
                if not form2:
                    o.truncate(ext_size)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    with open(src, "rb") as f:
        pvd = read_sector(f, 16)[0]
        assert pvd[1:6] == b"CD001", "not ISO9660"
        root = pvd[156:156 + 34]
        rlba = struct.unpack_from("<I", root, 2)[0]
        rsize = struct.unpack_from("<I", root, 10)[0]
        listing = []
        parse_dir(f, rlba, rsize, "", out, listing)
    with open(os.path.join(out, "files.txt"), "w") as o:
        for name, lba, size, form2 in listing:
            o.write(f"{lba:8d} {size:10d} {form2} {name}\n")
    for name, lba, size, form2 in listing:
        print(f"{lba:8d} {size:10d} {'XA' if form2 else '  '} {name}")


if __name__ == "__main__":
    main()
