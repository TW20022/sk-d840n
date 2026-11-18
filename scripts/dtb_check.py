#!/usr/bin/env python3
"""Minimal FDT (DTB) parser: list watchdog node status without dtc."""
import struct, sys

def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]

def parse(path):
    with open(path, "rb") as f:
        b = f.read()
    magic = u32(b, 0)
    if magic != 0xD00DFEED:
        print("NOT_A_DTB magic=0x%08x" % magic)
        return False
    totalsize = u32(b, 4)
    off_dt_struct = u32(b, 8)
    off_dt_strings = u32(b, 12)
    p = off_dt_struct
    strings = b[off_dt_strings:]
    results = []
    cur_path = []
    node_stack = []
    FDT_BEGIN_NODE=1; FDT_END_NODE=2; FDT_PROP=3; FDT_END=9
    def cstr(buf, off):
        e = buf.find(b"\0", off)
        return buf[off:e].decode("utf-8", "replace")
    while p < totalsize:
        tag = u32(b, p); p += 4
        if tag == FDT_BEGIN_NODE:
            name = cstr(b, p)
            p += (len(name) + 1 + 3) & ~3
            node_stack.append(name)
        elif tag == FDT_END_NODE:
            if node_stack: node_stack.pop()
        elif tag == FDT_PROP:
            ln = u32(b, p); p += 4
            nameoff = u32(b, p); p += 4
            propname = cstr(strings, nameoff)
            val = b[p:p+ln]; p += (ln + 3) & ~3
            pathstr = "/".join(x for x in node_stack if x)
            if "watchdog" in pathstr or "watchdog" in propname:
                v = val.rstrip(b"\0").decode("utf-8","replace") if val else ""
                results.append((pathstr, propname, v))
        elif tag == FDT_END:
            break
        else:
            print("unknown tag", tag); break
    for pathstr, propname, v in results:
        print("%s : %s = %s" % (pathstr or "/", propname, repr(v)))
    return results

if __name__ == "__main__":
    ok = parse(sys.argv[1])
    sys.exit(0 if ok else 1)
