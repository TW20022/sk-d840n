#!/usr/bin/env python3
"""Apply handover-supplement fixes E (watchdog reboot) and F (lan-cpu0 in br-lan).
Idempotent. Edits both the source (overlays/kernel-overlays) and the live
(openwrt / external kernel tree) copies so a later sync cannot revert them.
"""
import sys

R = "/home/opc/zx279133-ponwrt-build"
KT = R + "/linux-mainline-zte-zxslc-sr1010/linux-6.18.38"

DTS_FILES = [
    R + "/kernel-overlays/arch/arm64/boot/dts/zte/zx279133-sk-d840n.dts",
    KT + "/arch/arm64/boot/dts/zte/zx279133-sk-d840n.dts",
]
SK_FILES = [
    R + "/overlays/target/linux/zte/zx279133/base-files/etc/init.d/sk-bridge",
    R + "/openwrt/target/linux/zte/zx279133/base-files/etc/init.d/sk-bridge",
]
NW_FILES = [
    R + "/overlays/target/linux/zte/zx279133/base-files/etc/board.d/02_network",
    R + "/openwrt/target/linux/zte/zx279133/base-files/etc/board.d/02_network",
]


def read(p):
    with open(p, "r", encoding="utf-8") as f:
        return f.read()


def write(p, s):
    with open(p, "w", encoding="utf-8") as f:
        f.write(s)


def fix_wdt0(p):
    old = '&watchdog0 {\n\tstatus = "disabled";\n};'
    new = ('&watchdog0 {\n'
           '\t/* E-fix: watchdog0 is the reboot reset source */\n'
           '\tstatus = "okay";\n'
           '\n};')
    s = read(p)
    if old in s:
        write(p, s.replace(old, new, 1))
        return "FIXED(disabled->okay)"
    if new in s or '&watchdog0 {\n\tstatus = "okay";\n};' in s:
        return "ALREADY(okay)"
    return "ERROR: watchdog0 pattern not found"


def fix_skbridge(p):
    old = "for p in lan0 lan-cpu0; do"
    new = "for p in lan0; do  # F-fix: DSA conduit lan-cpu0 must not be a br-lan member"
    s = read(p)
    if old in s:
        write(p, s.replace(old, new, 1))
        return "FIXED(dropped lan-cpu0)"
    if "for p in lan0;" in s:
        return "ALREADY(lan0 only)"
    return "ERROR: sk-bridge for-loop not found"


def fix_02network(p):
    old = '\t[ -n "$lan_dev" ] || { [ -e /sys/class/net/lan-cpu0 ] && lan_dev="lan-cpu0"; }\n'
    new = '\t# F-fix: never fall back to the DSA conduit lan-cpu0 as the LAN port\n'
    s = read(p)
    if old in s:
        write(p, s.replace(old, new, 1))
        return "FIXED(dropped lan-cpu0 fallback)"
    if "# F-fix: never fall back to the DSA conduit" in s:
        return "ALREADY(fallback removed)"
    return "ERROR: 02_network fallback line not found"


def main():
    rc = 0
    for group, fn in (("DTS/watchdog0", fix_wdt0),
                      ("sk-bridge", fix_skbridge),
                      ("02_network", fix_02network)):
        files = {"DTS/watchdog0": DTS_FILES, "sk-bridge": SK_FILES,
                 "02_network": NW_FILES}[group]
        for p in files:
            try:
                r = fn(p)
            except Exception as e:
                r = "ERROR: %s" % e
            print("[%s] %s : %s" % (group, p, r))
            if r.startswith("ERROR"):
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
