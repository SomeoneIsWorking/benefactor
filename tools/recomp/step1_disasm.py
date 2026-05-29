#!/usr/bin/env python3
"""Benefactor recompiler – step 1: disassemble entry points"""
import sys, struct
from capstone import *
from capstone.m68k import *

def main():
    if len(sys.argv) < 3:
        print("Usage: recomp_step1.py <binary> <base_hex> <entry_hex>...")
        sys.exit(1)

    path = sys.argv[1]
    base = int(sys.argv[2], 16)
    entries = [int(a, 16) for a in sys.argv[3:]]

    with open(path, 'rb') as f:
        data = f.read()

    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    md.detail = True

    # Simple disassembly: just disassemble from each entry until a terminating
    # instruction (rts, rte, jmp, illegal) or until we hit data (invalid decode).
    # Store what we find as (start_addr, end_addr, list_of_instructions).

    functions = {}
    for entry in entries:
        addr = entry
        insns = []
        while True:
            off = addr - base
            if off < 0 or off + 4 > len(data):
                break
            try:
                decoded = list(md.disasm(data[off:off+16], addr))
            except:
                break
            if not decoded or decoded[0].address != addr:
                break  # can't decode, treat as end of function
            insn = decoded[0]
            insns.append(insn)
            addr += insn.size
            # Stop at terminal instructions
            m = insn.mnemonic.lower().split('.')[0]
            if m in ('rts', 'rte', 'illegal'):
                break
            if m == 'jmp' and not any(o.type == M68K_OP_REG for o in insn.operands):
                break
            if insn.size < 2:
                break
        functions[entry] = insns
        print(f"${entry:06X}: {len(insns)} instructions, ends at ${addr:06X}")

if __name__ == '__main__':
    main()
