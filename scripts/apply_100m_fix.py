#!/usr/bin/env python3
"""
apply_100m_fix.py - SK-D840N LAN 10/100M auto-speed fix.

Applies the patch from "云电脑编译交付包-README.md" (sections 2.1 / 2.2) to
    kernel-overlays/drivers/net/ethernet/zte/zx279133-switch.c
using exact string replacement, which tolerates tab/line-ending drift better
than git apply.

Idempotent: running it twice is safe.  The original file is backed up to
<target>.orig before the first modification.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(
    ROOT, "kernel-overlays", "drivers", "net", "ethernet", "zte",
    "zx279133-switch.c",
)

# Anchor: end of zx279133_switch_uni_link() + declaration of outerphy_dump.
ANCHOR = (
    "\treturn !!(bmsr & BMSR_LSTATUS);\n"
    "}\n"
    "\n"
    "static void zx279133_switch_outerphy_dump(struct zx279133_switch *priv);"
)

NEW_FUNC = (
    "/*\n"
    " * The real speed/duplex of one UNI jack, from its internal PHY's BMCR.\n"
    " *\n"
    " * These PHYs (id 0x84b9:0x5032) do not expose the negotiated 1000BASE-T\n"
    " * result in MII_STAT1000 - reg 15 reads 0x3000 on both a 100M and a 1000M\n"
    " * link.  They do keep the resolved speed in the standard BMCR speed-select\n"
    " * bits, and update duplex in BMCR_FULLDPLX while autonegotiation is on:\n"
    " *\n"
    " *   BMCR_SPEED1000 (bit 6)  -> 1000M   (measured BMCR 0x1140)\n"
    " *   BMCR_SPEED100  (bit 13) -> 100M    (measured BMCR 0x3100)\n"
    " *   neither                 -> 10M\n"
    " *\n"
    " * Reading these lets a 100M-only peer be reported as 100M instead of the\n"
    " * hardcoded 1000M, which previously left phylink/MAC and the copper side\n"
    " * disagreeing and the port passing no frames.\n"
    " */\n"
    "static void zx279133_switch_uni_speed_duplex(struct zx279133_switch *priv,\n"
    "\t\t\t\t\t     int port, int *speed, int *duplex)\n"
    "{\n"
    "\tint addr, bmcr;\n"
    "\n"
    "\t/* Keep the old behaviour when the register cannot be read. */\n"
    "\t*speed = SPEED_1000;\n"
    "\t*duplex = DUPLEX_FULL;\n"
    "\n"
    "\tif (!priv->uni_mdio)\n"
    "\t\treturn;\n"
    "\n"
    "\taddr = ZX279133_SWITCH_DSA_PHY(port);\n"
    "\tbmcr = mdiobus_read(priv->uni_mdio, addr, MII_BMCR);\n"
    "\tif (bmcr < 0)\n"
    "\t\treturn;\n"
    "\n"
    "\tif (bmcr & BMCR_SPEED1000)\n"
    "\t\t*speed = SPEED_1000;\n"
    "\telse if (bmcr & BMCR_SPEED100)\n"
    "\t\t*speed = SPEED_100;\n"
    "\telse\n"
    "\t\t*speed = SPEED_10;\n"
    "\n"
    "\t*duplex = (bmcr & BMCR_FULLDPLX) ? DUPLEX_FULL : DUPLEX_HALF;\n"
    "}"
)

INSERT_REPL = (
    "\treturn !!(bmsr & BMSR_LSTATUS);\n"
    "}\n"
    "\n"
    + NEW_FUNC + "\n"
    "\n"
    "static void zx279133_switch_outerphy_dump(struct zx279133_switch *priv);"
)

FIXED_OLD = (
    "\tlink = zx279133_switch_uni_link(priv, port);\n"
    "\tstate->link = link == 1;\n"
    "\tstate->speed = SPEED_1000;\n"
    "\tstate->duplex = DUPLEX_FULL;\n"
    "}"
)

FIXED_NEW = (
    "\t{\n"
    "\t\tint speed, duplex;\n"
    "\n"
    "\t\tzx279133_switch_uni_speed_duplex(priv, port, &speed, &duplex);\n"
    "\t\tstate->speed = speed;\n"
    "\t\tstate->duplex = duplex;\n"
    "\t}\n"
    "\tlink = zx279133_switch_uni_link(priv, port);\n"
    "\tstate->link = link == 1;\n"
    "}"
)


def main():
    if not os.path.isfile(TARGET):
        sys.exit("ERROR: target file not found: " + TARGET)

    with open(TARGET, "r", encoding="utf-8", newline="") as f:
        src = f.read()

    if "zx279133_switch_uni_speed_duplex" in src:
        print("Already patched (uni_speed_duplex found). Nothing to do.")
        return 0

    orig = TARGET + ".orig"
    if not os.path.exists(orig):
        shutil.copy2(TARGET, orig)
        print("Backup written: " + orig)

    n_anchor = src.count(ANCHOR)
    n_fixed = src.count(FIXED_OLD)
    if n_anchor != 1 or n_fixed != 1:
        sys.exit(
            "ERROR: anchor mismatch (insert=%d, fixed=%d); source changed?"
            % (n_anchor, n_fixed)
        )

    out = src.replace(ANCHOR, INSERT_REPL, 1).replace(FIXED_OLD, FIXED_NEW, 1)
    if out.count("zx279133_switch_uni_speed_duplex") != 2:
        sys.exit("ERROR: unexpected helper count after patch")

    with open(TARGET, "w", encoding="utf-8", newline="") as f:
        f.write(out)

    print("Patch applied successfully.")
    print("Verify: grep -n uni_speed_duplex " + TARGET)
    return 0


if __name__ == "__main__":
    sys.exit(main())
