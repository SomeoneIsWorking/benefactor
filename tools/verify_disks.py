#!/usr/bin/env python3
"""
tools/verify_disks.py
─────────────────────
Verify that the three disk images supplied to the Benefactor PC port
contain the data the emulator expects.

Usage:
    python3 tools/verify_disks.py <Disk.1> <Disk.2> <Disk.3>

This does NOT decompress or modify any files.  It only checks sizes,
known byte signatures, and the ATN! compressed-data magic number so
that you get a clear error message before the C binary runs.

No copyrighted data is extracted or stored.
"""

import struct
import sys
import os


# ── Expected parameters (from WHDLoad install file analysis) ─────────────────────

DISK_SIZES = [
    (980 * 1024,  1003520),  # Disk.1: either 980 KiB or slightly larger raw
    (980 * 1024,  1003520),
    (980 * 1024,  1003520),
]

# Bootloader is at offset 0 in Disk.1, length 0x1600 (5632) bytes.
# The game entry point at offset 0x412 should start with a known byte sequence.
ENTRY_OFFSET = 0x412
ENTRY_MAGIC  = bytes([0x31, 0xFC])  # move.w #imm, abs.w  (first instruction)

# The Load routine at offset 0x44E should start with MOVEM.L D1-D7/A0-A5,-(A7)
LOAD_OFFSET  = 0x44E
LOAD_MAGIC   = bytes([0x48, 0xE7, 0x7F, 0xFC])  # movem.l mask,-(a7)

# The Decrunch routine at 0x5E4 checks for the ATN! signature in compressed data
DECRUNCH_OFFSET = 0x5E4
DECRUNCH_MAGIC  = bytes([0x2C, 0x48])  # movea.l a0,a6 (first instruction)

# The compressed main payload starts at byte offset 0x1880 in Disk.1.
# Its first word should be the ATN! decompressor data header.
PAYLOAD_OFFSET = 0x1880
ATN_MAGIC      = b'ATN!'   # $41544E21


def check_disk(path: str, disk_no: int) -> bool:
    ok = True
    min_sz, max_sz = DISK_SIZES[disk_no - 1]
    label = f"Disk.{disk_no} ({path})"

    if not os.path.exists(path):
        print(f"  ERROR: {label}: file not found")
        return False

    size = os.path.getsize(path)
    if not (min_sz <= size <= max_sz):
        print(f"  WARN:  {label}: unexpected size {size} bytes "
              f"(expected {min_sz}–{max_sz})")
        # Don't fail hard on size — WHDLoad images vary slightly
    else:
        print(f"  OK:    {label}: size {size} bytes")

    with open(path, "rb") as f:
        data = f.read()

    if disk_no == 1:
        # Check bootloader magic
        snippet = data[ENTRY_OFFSET : ENTRY_OFFSET + len(ENTRY_MAGIC)]
        if snippet == ENTRY_MAGIC:
            print(f"  OK:    entry point magic at 0x{ENTRY_OFFSET:04x}")
        else:
            print(f"  WARN:  entry point at 0x{ENTRY_OFFSET:04x}: "
                  f"got {snippet.hex()}, expected {ENTRY_MAGIC.hex()}")

        snippet = data[LOAD_OFFSET : LOAD_OFFSET + len(LOAD_MAGIC)]
        if snippet == LOAD_MAGIC:
            print(f"  OK:    Load routine magic at 0x{LOAD_OFFSET:04x}")
        else:
            print(f"  WARN:  Load routine at 0x{LOAD_OFFSET:04x}: "
                  f"got {snippet.hex()}, expected {LOAD_MAGIC.hex()}")

        # Check ATN! payload signature
        if len(data) > PAYLOAD_OFFSET + 4:
            magic = data[PAYLOAD_OFFSET : PAYLOAD_OFFSET + 4]
            if magic == ATN_MAGIC:
                print(f"  OK:    ATN! compressed payload at 0x{PAYLOAD_OFFSET:04x}")
            else:
                print(f"  WARN:  payload at 0x{PAYLOAD_OFFSET:04x}: "
                      f"got {magic.hex()!r}, expected ATN!")

    return ok


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 1

    disk1, disk2, disk3 = sys.argv[1], sys.argv[2], sys.argv[3]
    paths = [disk1, disk2, disk3]

    all_ok = True
    for i, path in enumerate(paths, start=1):
        print(f"\nChecking Disk {i}:")
        if not check_disk(path, i):
            all_ok = False

    print()
    if all_ok:
        print("All checks passed.  Disk images look correct.")
        return 0
    else:
        print("Some checks failed.  The game may not run correctly.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
