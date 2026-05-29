#!/usr/bin/env python3
import capstone, sys

fname = sys.argv[1]
offset = int(sys.argv[2], 0)
base = int(sys.argv[3], 0)
length = int(sys.argv[4], 0)

with open(fname, 'rb') as f:
    f.seek(offset)
    data = f.read(length)

md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
for i in md.disasm(data, base):
    print("0x%06x:  %-12s %s" % (i.address, i.mnemonic, i.op_str))
