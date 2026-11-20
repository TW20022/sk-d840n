# XG-PON lane image without touching the shared clock (2026-11-20)

## Why this replaces the first attempt

The first XG-PON patch (`88d2584`) changed two things at once:

1. it selected the XG-PON NSYN 49-word image for the PON lane, and
2. it rewrote the shared TOPCRM PLL/MUX (`PLL_SEL` bits[5:4] 0 -> 2,
   `PLL_CFG0` `0x20106454` -> `0x00202054`) to a GPON divider.

Item 2 is the mistake. `topcrm` is the SoC-wide clock controller
(`clock-controller@10e10000`), not a PON-private block. The "PON" in the
register names is only the driver's point of view. Rewriting the divider
disturbs the reference clock that the RJ45/switch path also derives from,
and that build lost br-lan networking (confirmed by the board owner: the
same tree at `52cb590` builds a firmware whose br-lan is fine).

This patch keeps item 1 and drops item 2 entirely.

## What is measured

On a running `52cb590` system (br-lan up, `192.168.9.1/24`), read back:

```
0x10e10010 (TOPCRM PON PLL_SEL) = 0x00000001   -> bits[5:4] = 0  (ETH)
0x10e100c0 (TOPCRM PON PLL_CFG0) = 0x60106454  -> ETH divider
```

So the stock driver's `configure_pll()` deliberately leaves the shared clock
in its Ethernet setting.  Any firmware that must keep br-lan working has to
write those two registers exactly the way `52cb590` does.

A live UART test confirmed the safe half: writing a single word into the PON
lane's own window (`0x16000004`: `0x4fa840a3` -> `0x50a840a1`, the XG-PON
word 1) left the system alive (uptime kept advancing) and br-lan `up`.
The PON lane window `0x16000000..0x160000c0` is safe to program; the shared
clock block is not.

## What this patch changes

- New file `kernel-overlays/drivers/phy/zte/phy-zx279133-pon-serdes.c`,
  byte-for-byte the in-tree driver plus:
  - the recovered XG-PON NSYN 49-word image (U-Boot mode 6,
    `mode_xgpon_nsyn_cfg_133`),
  - a `bool xgpon` set from the board property `zte,xgpon-nsyn`,
  - `profile()`/`mode_name()` select the XG-PON image when that property is
    present, otherwise behave exactly as stock,
  - two vendor POST writes (`0xb0`, `0xb4`) that are inside the PON lane
    window, applied only in XG-PON mode.
- `configure_pll()` and `restore_pll()` are **unchanged** from the in-tree
  driver: they still write the Ethernet divider.  No build of this tree
  touches `PLL_SEL`/`PLL_CFG0`/`PLL_CFG1`/`PLL_MUX` with GPON values.
- Board DTS `&pon_serdes` gains `zte,xgpon-nsyn` and an explanatory comment;
  the old comment claiming no 49-word image existed is corrected.

Net effect: the only runtime difference from `52cb590` is the contents of the
PON lane's private register window.  The shared clock path, and therefore
br-lan, is identical to the known-good tree.

## Verification criteria

After booting a firmware built from this tree, on the UART/SSH console:

```
dmesg | grep -i 'PON SerDes'      # expect: "PON SerDes image: XG-PON NSYN"
                                  # and:     "XG-PON NSYN SerDes ready"
mdread/memread 0x16000004 4       # expect 0x50a840a1  (was 0x4fa840a3)
mdread/memread 0x160000b0 4       # expect 0x20200000
mdread/memread 0x160000b4 4       # expect 0x00808020
cat /sys/class/net/eth0/pon_state # O1 without signal; O2/O5 only with OLT light
```

Invariant that must hold regardless of PON result:

```
cat /sys/class/net/br-lan/operstate    # up
ip -4 addr show br-lan                 # 192.168.9.1/24
0x10e10010 bits[5:4]                    # still 0 (ETH), never 2
0x10e100c0                              # still ETH divider, never 0x00202054
```

If br-lan is up and those two clock registers are unchanged, the regression
from `88d2584` is gone.  Whether XG-PON reaches O5 still depends on an OLT
being present on the fibre and the user-side `zxpond` closing the OMCI loop;
that is a separate question from keeping the Ethernet side alive.
