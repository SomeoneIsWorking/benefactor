#!/usr/bin/env python3
"""Convert harness framebuffer dumps (352x282 ARGB uint32) to PNG, and analyze
per-row vertical scroll between consecutive frames.

The dump dimensions MUST match HW_DISPLAY_W x HW_DISPLAY_H in hw.h (352x282).
A wrong width shears every row incrementally -> diagonal "warp".

Usage:
  fb_view.py png <in.bin> <out.png>
  fb_view.py scroll <pc_prefix> <puae_prefix> <n>   # logs/<prefix><i>.bin for i in 0..n-1
"""
import sys, struct

W, H = 352, 282

def load(path):
    with open(path, "rb") as f:
        data = f.read()
    px = struct.unpack("<%dI" % (W * H), data[: W * H * 4])
    return px

def to_png(px, out):
    import zlib
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            v = px[y * W + x]
            raw += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    with open(out, "wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))

def row_lum(px, y):
    """Sum luminance of a row (background is dark, text bright)."""
    s = 0
    base = y * W
    for x in range(W):
        v = px[base + x]
        s += ((v >> 16) & 0xFF) + ((v >> 8) & 0xFF) + (v & 0xFF)
    return s

def best_shift(prev, cur, y0, y1, maxsh=8):
    """Find vertical shift (rows) that best matches cur[y0:y1] to prev (cross-corr
    on per-row luminance). Positive = content moved DOWN."""
    pl = [row_lum(prev, y) for y in range(H)]
    cl = [row_lum(cur, y) for y in range(H)]
    best, bestd = None, 1e30
    for sh in range(-maxsh, maxsh + 1):
        d = 0.0
        n = 0
        for y in range(y0, y1):
            ys = y - sh
            if 0 <= ys < H:
                d += abs(cl[y] - pl[ys])
                n += 1
        if n:
            d /= n
            if d < bestd:
                bestd, best = d, sh
    return best

def sad(a, b, dx, dy, y0, y1):
    """Sum of abs RGB diff of a vs b shifted by (dx,dy), over rows [y0,y1)."""
    s = 0
    for y in range(y0, y1):
        ys = y + dy
        if ys < 0 or ys >= H:
            continue
        for x in range(0, W):
            xs = x + dx
            if xs < 0 or xs >= W:
                continue
            va = a[y * W + x]
            vb = b[ys * W + xs]
            s += abs(((va >> 16) & 0xFF) - ((vb >> 16) & 0xFF))
            s += abs(((va >> 8) & 0xFF) - ((vb >> 8) & 0xFF))
            s += abs((va & 0xFF) - (vb & 0xFF))
    return s

def best_align(pc, puae_list, j_center, jrange=3, shiftmax=3, y0=110, y1=230):
    """Find (j, dx, dy) minimizing SAD of pc vs puae_list[j] shifted, in the
    moving-object band. Returns (j, dx, dy, sad)."""
    best = (j_center, 0, 0, 1 << 62)
    for j in range(max(0, j_center - jrange), min(len(puae_list), j_center + jrange + 1)):
        for dy in range(-2, 3):
            for dx in range(-shiftmax, shiftmax + 1):
                s = sad(pc, puae_list[j], dx, dy, y0, y1)
                if s < best[3]:
                    best = (j, dx, dy, s)
    return best

def triptych(pc, puae, dx, dy, out):
    """PC | PUAE(shifted to align) | residual-heatmap, side by side."""
    import zlib
    OW = W * 3
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):  # PC
            v = pc[y * W + x]
            raw += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
        for x in range(W):  # PUAE shifted
            xs, ys = x + dx, y + dy
            v = puae[ys * W + xs] if 0 <= xs < W and 0 <= ys < H else 0
            raw += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
        for x in range(W):  # residual (aligned): bright red where they differ
            xs, ys = x + dx, y + dy
            vp = pc[y * W + x]
            vu = puae[ys * W + xs] if 0 <= xs < W and 0 <= ys < H else 0
            dr = abs(((vp >> 16) & 0xFF) - ((vu >> 16) & 0xFF))
            dg = abs(((vp >> 8) & 0xFF) - ((vu >> 8) & 0xFF))
            db = abs((vp & 0xFF) - ((vu) & 0xFF))
            mag = dr + dg + db
            raw += bytes((min(255, mag), 0, 0)) if mag > 48 else bytes((20, 20, 20))
    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", OW, H, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    with open(out, "wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "png":
        to_png(load(sys.argv[2]), sys.argv[3])
        print("wrote", sys.argv[3])
    elif cmd == "zoom":
        # zoom <in.bin> <out.png> <x0> <y0> <w> <h> <scale>
        px = load(sys.argv[2]); out = sys.argv[3]
        x0, y0, w, h, sc = (int(sys.argv[i]) for i in range(4, 9))
        import zlib
        OW, OH = w * sc, h * sc
        raw = bytearray()
        for oy in range(OH):
            raw.append(0)
            sy = y0 + oy // sc
            for ox in range(OW):
                sx = x0 + ox // sc
                v = px[sy * W + sx] if 0 <= sx < W and 0 <= sy < H else 0
                raw += bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
        def chunk(typ, data):
            c = typ + data
            return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        with open(out, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", OW, OH, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
        print("wrote", out)
    elif cmd == "align":
        # align <pc_prefix> <puae_prefix> <lo> <hi>  (logs/fb_pc_<pre><i>.bin)
        pcp, pup, lo, hi = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
        idx = list(range(lo, hi + 1))
        pc = {i: load("logs/fb_pc_%s%d.bin" % (pcp, i)) for i in idx}
        pu = {i: load("logs/fb_puae_%s%d.bin" % (pup, i)) for i in idx}
        pulist = [pu[i] for i in idx]
        print("PCidx -> best PUAEidx  dx dy   residualSAD   (lower=better match)")
        for i in idx:
            j, dx, dy, s = best_align(pc[i], pulist, i - lo)
            jj = idx[j]
            out = "logs/align_%s%02d.png" % (pcp, i)
            triptych(pc[i], pu[jj], dx, dy, out)
            print("%5d -> %5d  %3d %3d  %12d   %s" % (i, jj, dx, dy, s, out))
    elif cmd == "scroll":
        pcp, pup, n = sys.argv[2], sys.argv[3], int(sys.argv[4])
        pc = [load("logs/fb_pc_%s%d.bin" % (pcp, i)) for i in range(n)]
        pu = [load("logs/fb_puae_%s%d.bin" % (pup, i)) for i in range(n)]
        # text crawl bands (text scrolls UP -> negative shift)
        UT0, UT1, LT0, LT1 = 105, 165, 165, 230
        print("frame |  PC up  PC low | PU up  PU low   (vert shift rows, -=up)")
        for i in range(1, n):
            pct = best_shift(pc[i-1], pc[i], UT0, UT1)
            pcb = best_shift(pc[i-1], pc[i], LT0, LT1)
            put = best_shift(pu[i-1], pu[i], UT0, UT1)
            pub = best_shift(pu[i-1], pu[i], LT0, LT1)
            print("%5d | %6s %6s | %6s %6s" % (i, pct, pcb, put, pub))
