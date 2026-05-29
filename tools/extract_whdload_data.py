#!/usr/bin/env python3
"""extract_whdload_data.py — Extract WHDLoad-placed data from a chip RAM dump.

The chip RAM dump at sync time contains:
  - The game code region ($3000–$2742D)
  - WHDLoad-placed data at various other addresses (sprite tables, level data)
  - WHDLoad patches to the game code region (~50 bytes)
  - Zero padding

Load order in HLE boot:
  1. Zero chip RAM
  2. Load clean code image at $3000
  3. Apply whdload_data.bin         (WHDLoad data + patches overwrite where needed)
  Result = chip RAM dump exactly

Output format (whdload_data.bin):
  magic[4]      "WHLD"
  n_segs[4]     number of segments (big-endian u32)
  for each segment:
    addr[4]     chip RAM address (big-endian u32)
    size[4]     byte count (big-endian u32)
    data[size]  raw bytes

Usage:
  python3 tools/extract_whdload_data.py \
      --chip logs/harness_puae_chipram.bin \
      --code logs/clean_code_region.bin \
      --game-base 0x3000 \
      --out logs/whdload_data.bin [--verify]
"""
import argparse, struct, sys, os

GAME_BASE_DEFAULT = 0x3000
CHIP_SIZE = 0x080000
MAGIC = b"WHLD"


def find_segments(data, block_size=512):
    segs, in_seg, seg_start = [], False, 0
    for i in range(0, len(data), block_size):
        if any(b != 0 for b in data[i:i+block_size]):
            if not in_seg: seg_start = i; in_seg = True
        else:
            if in_seg: segs.append((seg_start, i)); in_seg = False
    if in_seg: segs.append((seg_start, len(data)))
    return segs


def make_delta(chip, game, game_base):
    """Zero chip RAM bytes that exactly match the clean code image; keep everything else."""
    delta = bytearray(chip)
    for i in range(len(game)):
        idx = game_base + i
        if idx < len(chip) and chip[idx] == game[i]:
            delta[idx] = 0
    return bytes(delta)


def pack_whld(data):
    segs = find_segments(data)
    out = bytearray(MAGIC) + struct.pack(">I", len(segs))
    for s, e in segs:
        out += struct.pack(">II", s, e - s)
        out += data[s:e]
    return bytes(out)


def load_whld(packed):
    assert packed[:4] == MAGIC
    n = struct.unpack_from(">I", packed, 4)[0]
    buf = bytearray(CHIP_SIZE)
    off = 8
    for _ in range(n):
        addr, sz = struct.unpack_from(">II", packed, off)
        buf[addr:addr+sz] = packed[off+8:off+8+sz]
        off += 8 + sz
    return buf


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chip",      required=True)
    ap.add_argument("--code",      required=True, help="clean code image for $3000 region")
    ap.add_argument("--game-base", default=hex(GAME_BASE_DEFAULT))
    ap.add_argument("--out",       required=True)
    ap.add_argument("--verify",    action="store_true")
    args = ap.parse_args()

    game_base = int(args.game_base, 0)
    chip = open(args.chip, "rb").read()
    game = open(args.code, "rb").read()

    n_patch = sum(1 for i in range(min(len(game), len(chip)-game_base))
                  if chip[game_base+i] != game[i])
    if n_patch:
        print(f"Note: {n_patch} WHDLoad-patched bytes in game binary region", file=sys.stderr)

    delta  = make_delta(chip, game, game_base)
    packed = pack_whld(delta)
    segs   = find_segments(delta)
    total  = sum(e-s for s,e in segs)

    print(f"Chip RAM    : {len(chip):,} bytes")
    print(f"Code image  : {len(game):,} bytes @ ${game_base:06X}–${game_base+len(game)-1:06X}")
    print(f"WHDLoad data: {total:,} bytes in {len(segs)} segments")
    for s, e in segs:
        print(f"  ${s:06X}–${e-1:06X}  ({e-s:,} bytes)")
    print(f"Packed      : {len(packed):,} bytes → {args.out}")

    os.makedirs(os.path.dirname(args.out) if os.path.dirname(args.out) else ".", exist_ok=True)
    open(args.out, "wb").write(packed)

    if args.verify:
        print("\nVerifying...")
        # HLE load order: clean code image first, then whdload_data on top
        restored = bytearray(CHIP_SIZE)
        restored[game_base:game_base+len(game)] = game
        whld = load_whld(open(args.out, "rb").read())
        for i in range(CHIP_SIZE):
            if whld[i] != 0:
                restored[i] = whld[i]
        bad = [(i, chip[i], restored[i]) for i in range(min(len(chip), CHIP_SIZE))
               if chip[i] != restored[i]]
        if bad:
            print(f"VERIFY FAILED: {len(bad)} mismatches")
            for i, exp, got in bad[:10]:
                print(f"  ${i:06X}: expected ${exp:02X}, got ${got:02X}")
        else:
            print("VERIFY OK: round-trip matches chip RAM dump")


if __name__ == "__main__":
    main()
