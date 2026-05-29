#!/usr/bin/env python3
"""Validate: can Capstone decode this address as a valid 68k instruction?"""
import sys
from capstone import *
from capstone.m68k import *

def is_valid_insn(binary_path, base_addr, test_addrs):
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    with open(binary_path, 'rb') as f:
        data = f.read()

    for addr in test_addrs:
        off = addr - base_addr
        if off < 0 or off + 2 > len(data):
            print(f"${addr:06X}: out of range")
            continue
        try:
            insns = list(md.disasm(data[off:off+16], addr))
            if insns:
                i = insns[0]
                if i.address == addr:
                    print(f"${addr:06X}: {i.mnemonic} {i.op_str}")
                else:
                    print(f"${addr:06X}: gap (first insn at ${i.address:06X})")
            else:
                print(f"${addr:06X}: no valid instruction")
        except:
            print(f"${addr:06X}: decode error")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 validate.py <binary> <base_hex> <addr_hex>...")
        sys.exit(1)
    path = sys.argv[1]
    base = int(sys.argv[2], 16)
    addrs = [int(a, 16) for a in sys.argv[3:]]
    is_valid_insn(path, base, addrs)
