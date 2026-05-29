#!/usr/bin/env python3
"""test_frame_ptrs.py — Check what chip RAM values drive d1 (BLTDPT) in the $0BFA blit.

The M68K code at $0041C2:
    lea.l $419a(pc), a0   ; a0 = $419A (animation frame pointer table)
    move.l (a0)+, d0       ; d0 = chip[$419A]
    move.l (a0), d1        ; d1 = chip[$419E]

Then outer loop (100 iterations), each calling bsr $4236:
    addi.w #$f78, d1       ; d1.w += $F78 (before loop)
    ... per-iteration subi.w #$28, d1

BLTDPT = a1 = d1 + d3 per call (d3 = shape width, varies)

T7: Read PUAE chip_ram[$419A] and [$419E] (the frame pointer table).
    Show the raw values and compute what d1_initial PUAE had.
T8: Compute how many bytes off the PC's d1_initial would need to be
    to produce dpt=$07EC98 vs PUAE's $07EC88.
"""
import struct, sys

CHIPRAM = "logs/harness_puae_chipram.bin"

try:
    chip = open(CHIPRAM, "rb").read()
except FileNotFoundError:
    print(f"ERROR: {CHIPRAM} not found. Run harness first.")
    sys.exit(1)

# The frame pointer table at $419A:
# chip[$419A] = d0 source (one animation buffer pointer)
# chip[$419E] = d1 source (other animation buffer pointer)
FRAME_TABLE = 0x419A
d0_src = struct.unpack_from(">I", chip, FRAME_TABLE)[0]
d1_src = struct.unpack_from(">I", chip, FRAME_TABLE + 4)[0]

print(f"T7: Animation frame pointer table at ${FRAME_TABLE:06X}")
print(f"    chip[${FRAME_TABLE:06X}] = ${d0_src:08X}  (d0 = source buffer)")
print(f"    chip[${FRAME_TABLE+4:06X}] = ${d1_src:08X}  (d1 = dest buffer)")
print()

# After addi.w #$FA0, d0 and addi.w #$F78, d1 (16-bit adds):
d0_after = (d0_src & 0xFFFF0000) | ((d0_src + 0xFA0) & 0xFFFF)
d1_after = (d1_src & 0xFFFF0000) | ((d1_src + 0xF78) & 0xFFFF)
print(f"    d0 after addi.w #$FA0 = ${d0_after:08X}")
print(f"    d1 after addi.w #$F78 = ${d1_after:08X}")
print()

# The outer loop runs 100 iterations (dbra d7, where d7=99).
# Per iteration: d1 -= $28 (after bsr $4236 returns).
# The blit that clobbers is triggered inside bsr $4236 when d1+d3 = $07EC98 (PC).
# PUAE's version produces d1+d3 = $07EC88.
# If d3 is the same for that iteration, d1_PUAE - d1_PC = -16.

TARGET_PC   = 0x07EC98
TARGET_PUAE = 0x07EC88

# d1 at iteration i (0=first): d1_i = d1_after - i * 0x28
# bsr $4236 executes BEFORE the subtract at the end of each iteration
# So at the start of bsr $4236 for iteration i: d1 = d1_after - i * 0x28
# (The subtract happens AFTER bsr returns)

# Low 24 bits of d1_after (only low 24 bits matter for chip RAM addressing):
d1_lo = d1_after & 0xFFFFFF
print(f"T8: Searching for which iteration produces BLTDPT near $07EC88-$07EC98")
print(f"    d1_initial_lo24 = ${d1_lo:06X}")
for i in range(100):
    d1_i = (d1_after - i * 0x28) & 0xFFFFFF
    # d3 contribution needed: target - d1_i
    d3_needed_pc   = (TARGET_PC   - d1_i) & 0xFFFF
    d3_needed_puae = (TARGET_PUAE - d1_i) & 0xFFFF
    if 0 <= d3_needed_puae <= 100 or 0 <= d3_needed_pc <= 100:
        print(f"    iter {i:3d}: d1_i=${d1_i:06X}  d3 needed for PC=${d3_needed_pc}  for PUAE=${d3_needed_puae}")

print()
# T9: Show the actual chip RAM area at $419A to $41B0 (frame pointer swap table)
print(f"T9: Chip RAM at $419A..$41AF (animation frame pointer region):")
for addr in range(0x4190, 0x41B0, 2):
    w = (chip[addr] << 8) | chip[addr+1]
    marker = " <-- frame ptr entry" if 0x419A <= addr <= 0x419F else ""
    print(f"    ${addr:06X}: ${w:04X}{marker}")

if __name__ == "__main__":
    pass
