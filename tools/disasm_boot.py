#!/usr/bin/env python3
"""Disassemble key parts of Disk.1 to understand the boot sequence."""

import capstone, struct, sys, os

disk = os.path.join(os.path.dirname(__file__), '../Disk.1')

def disasm(data, base, label=""):
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    if label:
        print(f"\n=== {label} (base=0x{base:06x}) ===")
    for i in md.disasm(data, base):
        print(f"  0x{i.address:06x}:  {i.mnemonic:<12} {i.op_str}")

with open(disk, 'rb') as f:
    all_boot = f.read(0x1600)

# Full bootloader from $76000 to $77600
disasm(all_boot, 0x76000, "Full bootloader (Disk.1 0x000-0x1600)")
