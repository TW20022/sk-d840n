#!/usr/bin/env python3
"""Disassemble a vendor .ko range and annotate relocations with symbol names.

Usage: PYTHONPATH=tools python tools/dis_annot.py <ko> <section> <start> <size>
"""
import os
import sys

from capstone import CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, Cs

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from ponelf import Ko

path, sec, start, size = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0)
ko = Ko(path)
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
md.detail = False
code = ko.bytes_of(sec)[start:start + size]
relocs = {}
for r in ko.relocs():
    if r["rsec"] == sec:
        relocs[r["off"]] = r
for ins in md.disasm(code, start):
    r = relocs.get(ins.address)
    note = ""
    if r:
        note = "  ; -> %s +0x%x [%s]" % (r["sym"], r["add"], r["sname"][:70])
    print("0x%-7x %-9s %-42s%s" % (ins.address, ins.mnemonic, ins.op_str, note))
