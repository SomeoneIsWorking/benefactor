#!/usr/bin/env python3
"""Analyze the Benefactor game binary: identify code vs data regions."""
import sys

def analyze(path):
    with open(path, 'rb') as f:
        data = f.read()

    code_regions = []
    data_regions = []
    current_start = None
    current_type = None
    consecutive_invalid = 0
    consecutive_valid = 0

    for offset in range(0, len(data), 2):
        if offset + 1 >= len(data): break
        w = (data[offset] << 8) | data[offset + 1]

        # Detect patterns that indicate code vs data
        is_data = False

        # Data pattern: ORI.B #$nn, d0/d1 (0x0000 xxxx)
        # Valid code rarely starts with these specific ORI sequences
        if w in (0x0000, 0x0002, 0x0010, 0x0013, 0x0040, 0x0042, 0x004A,
                 0x004E, 0x0062, 0x006A, 0x0070, 0x0072, 0x007F, 0x0080,
                 0x008A, 0x0090, 0x0092, 0x00A8, 0x00B2, 0x00BA, 0x00C0,
                 0x00D2, 0x00DA, 0x00FA):
            # These ORI.B immediates are almost always data, not code
            is_data = True

        # Check for runs of same instruction (data tables)
        if not current_start:
            current_start = offset
            current_type = 'data' if is_data else 'code'
            consecutive_valid = 0 if is_data else 1
            consecutive_invalid = 1 if is_data else 0
        elif is_data:
            consecutive_invalid += 1
            consecutive_valid = 0
        else:
            consecutive_valid += 1
            consecutive_invalid = 0

        # Transition from code to data or vice versa
        # After 8 consecutive data words, it's a data region
        # After 4 consecutive code words, it's a code region
        if current_type == 'code' and consecutive_invalid >= 8:
            if offset - current_start >= 16:
                code_regions.append((current_start, offset - current_start))
            current_start = offset
            current_type = 'data'
            consecutive_valid = 0
        elif current_type == 'data' and consecutive_valid >= 4:
            if offset - current_start >= 4:
                data_regions.append((current_start, offset - current_start))
            current_start = offset
            current_type = 'code'
            consecutive_invalid = 0

    # Print results
    print(f"Binary size: {len(data)} bytes")
    print(f"\nCode regions ({len(code_regions)}):")
    for start, size in code_regions:
        if size > 32:
            print(f"  ${start:06X}-${start+size:06X} ({size} bytes)")

    print(f"\nData regions ({len(data_regions)}):")
    for start, size in data_regions:
        if size > 16:
            print(f"  ${start:06X}-${start+size:06X} ({size} bytes)")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_binary.py <chip_dump_code_region.bin>")
        sys.exit(1)
    analyze(sys.argv[1])
