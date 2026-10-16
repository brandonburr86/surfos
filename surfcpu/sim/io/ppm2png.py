#!/usr/bin/env python3
"""SurfCPU PPM to PNG
--------------------
File: ppm2png.py Date: 10/15/26 (SurfCPU)
--------------------
Turns the binary PPM (P6) frames VgaCapture writes into PNG files, with the Python standard
library only (zlib), so captured VGA frames can be looked at and kept as evidence.

    python3 sim/io/ppm2png.py IN.ppm OUT.png [--scale N]
"""

import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:                      # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b"P6" or int(fields[3]) != 255:
        sys.exit(f"{path}: not an 8-bit binary PPM")
    w, h = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + 3 * w * h]
    if len(pixels) != 3 * w * h:
        sys.exit(f"{path}: truncated")
    return w, h, pixels


def write_png(path, w, h, pixels, scale=1):
    rows = []
    for y in range(h):
        line = pixels[3 * w * y:3 * w * (y + 1)]
        if scale > 1:
            line = b"".join(line[3 * x:3 * x + 3] * scale for x in range(w))
        rows.extend([b"\x00" + line] * scale)
    raw = b"".join(rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main(argv):
    if len(argv) not in (3, 5) or (len(argv) == 5 and argv[3] != "--scale"):
        sys.exit(__doc__)
    scale = int(argv[4]) if len(argv) == 5 else 1
    w, h, pixels = read_ppm(argv[1])
    write_png(argv[2], w, h, pixels, scale)
    print(f"ppm2png: {argv[2]} ({w * scale}x{h * scale})")


if __name__ == "__main__":
    main(sys.argv)
