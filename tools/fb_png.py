#!/usr/bin/env python3
"""Convert an ARGB uint32 framebuffer dump to PNG at an explicit width.
Usage: fb_png.py <in.bin> <out.png> <width> [height=282]"""
import sys, struct, zlib

inp, out = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]); H = int(sys.argv[4]) if len(sys.argv) > 4 else 282
data = open(inp, "rb").read()
px = struct.unpack("<%dI" % (W * H), data[: W * H * 4])
raw = bytearray()
for y in range(H):
    raw.append(0)
    for x in range(W):
        v = px[y * W + x]
        raw += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
def chunk(typ, d):
    c = typ + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
sig = b"\x89PNG\r\n\x1a\n"
ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)
idat = zlib.compress(bytes(raw), 9)
open(out, "wb").write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
print("wrote", out, W, "x", H)
