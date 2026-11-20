#!/usr/bin/env python3
"""Extract Hal_WriteXmacReg(idx, value) call sites from the vendor .ko.

Usage:
  PYTHONPATH=tools python tools/xmac_writes.py <ko> [func-regex]

For every relocation to Hal_WriteXmacReg the script walks backwards over a
small window of AArch64 instructions and reconstructs the constants passed in
w0 (idx) and w1 (value), including the movz/movk pairs the compiler emits for
XMAC indices such as 0x16147.  Results are printed as hex plus the name of
the containing function, which makes the vendor init tables reviewable.
"""
import os
import re
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, Cs
from capstone.arm64 import (ARM64_INS_MOV, ARM64_INS_MOVK, ARM64_INS_MOVZ,
                            ARM64_OP_IMM, ARM64_OP_REG, ARM64_REG_W0,
                            ARM64_REG_W1)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from ponelf import Ko

path = sys.argv[1]
pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else ".")
ko = Ko(path)
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
md.detail = True
text = ko.bytes_of(".text")
funcs = sorted((f for f in ko.funcs() if f["value"] is not None),
               key=lambda f: f["value"])


def owner(off):
    name = "<unknown>"
    for f in funcs:
        if f["value"] <= off < f["value"] + (f["size"] or 0):
            name = f["name"]
        elif f["value"] > off:
            break
    return name


def decode_consts(window):
    """Reconstruct w0/w1 immediately before the call, walking backwards."""
    vals = {ARM64_REG_W0: None, ARM64_REG_W1: None}
    for ins in reversed(window):
        ops = ins.operands
        if ins.id in (ARM64_INS_MOV, ARM64_INS_MOVZ) and len(ops) == 2:
            dst, src = ops[0], ops[1]
            if dst.type == ARM64_OP_REG and dst.reg in vals:
                if src.type == ARM64_OP_IMM:
                    vals[dst.reg] = src.imm & 0xffffffff
                elif src.type == ARM64_OP_REG and src.reg in vals:
                    vals[dst.reg] = vals[src.reg]
        elif ins.id == ARM64_INS_MOVK and len(ops) == 2:
            dst, src = ops[0], ops[1]
            if dst.type == ARM64_OP_REG and dst.reg in vals and src.type == ARM64_OP_IMM:
                shift = ops[2].imm if len(ops) > 2 else 0
                old = vals[dst.reg] or 0
                vals[dst.reg] = (old & ~(0xffff << shift) & 0xffffffff) | (src.imm << shift)
        if vals[ARM64_REG_W0] is not None and vals[ARM64_REG_W1] is not None:
            break
    return vals[ARM64_REG_W0], vals[ARM64_REG_W1]


writes = []
for r in ko.relocs():
    if r["rsec"] != ".text" or r["sym"] != "Hal_WriteXmacReg":
        continue
    off = r["off"]
    name = owner(off)
    if not pat.search(name):
        continue
    lo = max(0, off - 100)
    window = list(md.disasm(text[lo:off], lo))
    idx, val = decode_consts(window)
    writes.append((off, name, idx, val))

for off, name, idx, val in sorted(writes):
    i = "0x%x" % idx if idx is not None else "?"
    v = "0x%x" % val if val is not None else "?"
    print(".text+0x%-6x %-44s idx=%-10s val=%s" % (off, name, i, v))
print("total=%d" % len(writes))
