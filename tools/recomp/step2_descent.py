#!/usr/bin/env python3
"""Benefactor recompiler – step 2: recursive descent with branch validation"""
import sys
from capstone import *
from capstone.m68k import *

def is_invalid_target(addr, data, base, md):
    """Check if an address is clearly invalid code (data region)."""
    off = addr - base
    if off < 0 or off + 4 > len(data):
        return True
    try:
        decoded = list(md.disasm(data[off:off+16], addr))
    except:
        return True
    if not decoded:
        return True
    insn = decoded[0]
    # Instruction should start exactly at this address
    if insn.address != addr:
        return True
    # Reject obvious data patterns: ORI.B #$00, d0 repeated
    m = insn.mnemonic.lower().split('.')[0]
    if m in ('ori', 'andi') and insn.group(M68K_GRP_JUMP) == 0:
        # Check if this is followed by many similar ORI/ANDI — indicative of data
        count = 0
        a = addr + insn.size
        while count < 5 and a - base + 16 <= len(data):
            try:
                dd = list(md.disasm(data[a-base:a-base+16], a))
                if not dd: break
                ni = dd[0]
                nm = ni.mnemonic.lower().split('.')[0]
                if nm == m and ni.group(M68K_GRP_JUMP) == 0:
                    count += 1; a += ni.size
                else:
                    break
            except:
                break
        if count >= 3:
            return True  # data region detected
    return False

def main():
    if len(sys.argv) < 3:
        print("Usage: step2_descent.py <binary> <base_hex> <entry_hex>...")
        sys.exit(1)

    path = sys.argv[1]
    base = int(sys.argv[2], 16)
    entries = [int(a, 16) for a in sys.argv[3:]]

    with open(path, 'rb') as f:
        data = f.read()

    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    md.detail = True

    # Collector: address -> list of instructions
    functions = {}
    # Queue: addresses to explore
    from collections import deque
    queue = deque(entries)
    visited = set()

    while queue:
        entry = queue.popleft()
        if entry in visited:
            continue
        visited.add(entry)

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
                break
            insn = decoded[0]
            insns.append(insn)
            addr += insn.size

            # Collect branch/call targets
            m = insn.mnemonic.lower().split('.')[0]
            for op in insn.operands:
                if op.type == M68K_OP_BR_DISP:
                    target = insn.address + 2 + op.br_disp.disp
                    if not is_invalid_target(target, data, base, md):
                        if target not in visited:
                            queue.append(target)
                elif op.type == M68K_OP_IMM:
                    # Check if this might be an absolute address
                    val = op.value.imm
                    if m in ('jsr', 'jmp'):
                        if base <= val <= base + len(data) - 4:
                            if not is_invalid_target(val, data, base, md):
                                if val not in visited:
                                    queue.append(val)

            # Stop at terminal instructions
            if m in ('rts', 'rte', 'illegal'):
                break
            if m == 'jmp' and not any(o.type == M68K_OP_REG for o in insn.operands):
                break
            if insn.size < 2:
                break

        functions[entry] = insns
        print(f"${entry:06X}: end=${addr:06X} ({len(insns)} insns)")

    print(f"\nTotal functions: {len(functions)}")

if __name__ == '__main__':
    main()
