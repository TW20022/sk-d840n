#!/usr/bin/env python3
"""Disassemble named functions from a vendor .ko using capstone.
Usage: python tools/dis_ko.py <ko> <func-substr-regex>
"""
import sys, re
from capstone import *
from capstone.arm64 import *
from ponelf import Ko

path, pat = sys.argv[1], sys.argv[2]
rx = re.compile(pat)
ko = Ko(path)
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); md.detail = True
text = ko.bytes_of('.text')
for f in ko.funcs():
    if not rx.search(f['name']):
        continue
    off, size = f['value'], f['size'] or 0x40
    code = text[off:off+size]
    print("\n===== %s @ .text+0x%x size=0x%x =====" % (f['name'], off, size))
    for ins in md.disasm(code, off):
        note = ""
        m = ins.mnemonic
        # Highlight immediate loads, calls (via reloc we can't see here), and mem ops
        if m in ('mov','movk','movz','movn','adrp','add','sub','ldr','str','ldrb','strb','ldrh','strh','blr','bl','cbz','cbnz','ret','and','orr','lsl','lsr'):
            note = "  *"
        print("  0x%x: %-8s %-38s%s" % (ins.address, m, ins.op_str, note))
