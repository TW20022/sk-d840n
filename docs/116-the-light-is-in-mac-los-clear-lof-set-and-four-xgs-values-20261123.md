# 116 — The light is in: the PON MAC's own LOS clear, LOF set, and four wrong XGS values

Date: 2026-11-23 (evening, fibre plugged in for the whole round)

This round was run against the board with the fibre connected and no reboot
between measurements.  Everything below was read or written live through
`/sys/class/net/eth0/pon_reg`, `/sys/class/net/eth0/pon_hw` and the
`zx279133-9132` debug console, so nothing in it depends on a firmware that
has not been built yet.

## 1. The physical layer is not in question any more

The SerDes state register is `0x160000e4`.  The vendor's own
`pon_serdes_init()` (np_133.ko, `0x72b0`) decides what the two low bits mean,
and the two strings it prints are the authority:

```
0x24bc  "rx los =0 rx data in\n"      <- printed when bit 0 is clear
0x24d2  "rx los =1 no  rx data in\n"  <- printed when bit 0 is set
0x24fe  "cdr_lock_ready\n"            <- bit 1
0x2492  "com pll lock failed!\n"
0x24a8  "com_pll_lock_ready\n"        <- 0xd0 bit 0
```

On the board, sampled three times with the fibre in:

```
0x160000e4 = 0x01000706   bit0 = 0  -> "rx los =0 rx data in"
                          bit1 = 1  -> "cdr_lock_ready"
0x160000d0 = 0xc86fbb83   bit0 = 1  -> "com_pll_lock_ready"
```

So by the vendor driver's own criteria the lane has light, the PLL is locked
and the CDR is locked.  **Any conclusion of the form "no light / OLT not
transmitting / fibre fault" contradicts the chip.**  `opsd` reads low and the
LOS LED stays red because the BOSA front end is never initialised in a
mainline boot (that is the known, documented effect in `zx279133-pon.c`), not
because there is no light.

The SerDes mode image is also the vendor's, word for word: dumping
`0x16000000..0x160000c0` matches `zx279133_pon_serdes_xgpon_nsyn_profile[]`
in all 49 words (`0x63f48091` at +0x64, `0x802dc100` at +0x94, `0x401c` at
+0x90, the post-activate `0x20200000` / `0x00808020` at +0xb0 / +0xb4).

## 2. What the MAC says: LOS clear, LOF set

`0x16105` is the downstream LOS/LOF status word.  The vendor's O1/O2-3
handlers read it and nothing else:

```
p_o1_clear_los_or_lof_event        if (!(v & 0x10) && state == O1) -> O2-3
p_o2_3_detect_los_or_lof_event     if ( (v & 0x10) && state == O2-3) -> O1
p_o4/o5/o6_*                       same bit
fi_Configure_Ponmac_Get_LOS_Status reads 0x16105
```

On the board:

```
0x16105 = 0x00000010
```

bit0 = 0 (no LOS — the MAC agrees with the SerDes that light is present),
bit4 = 1 (**loss of frame**: the bit stream arrives but the XG-PON framer
cannot find its frame boundary).  Writing the register back is a no-op
(`w 16105 0` reads 0x10 again), so it is a live status, not a latch we can
clear by hand.

That is the shape of the remaining problem: **light in, no framing**.

## 3. XPCS1 has been sitting in SGMII, not 10GBASE-R

The PON lane's PCS is XPCS1 at `0x1b000000`.  It is reached through the SoC's
own MDIO controller (`xpcs 0 <devad> <reg>` in the debug console, `which 0`
= eth0's MAC PCS).

Baseline, before touching anything:

```
lane 0 (PON)                       lane 1 (LAN, working at 100M)
devad 3 reg 0x00 = 0x0440          0x0440
devad 3 reg 0x01 = 0x00c2          0x00c2
devad 3 reg 0x07 = 0x0001          0x0001     <- PCS type
devad 3 reg 0x8010 = 0x0070        0x0070
devad 0x1f reg 0x8000 = 0x2200     0x2200
devad 0x1f reg 0x8001 = 0x0015     0x0005
devad 0x1f reg 0x0000 = 0x1140     0x1140
devad 0x1f reg 0x0001 = 0x0109     0x0109
```

The two lanes read **identically** — the PON lane's PCS is programmed exactly
like a working SGMII 1G copper lane.  The vendor's XPCS layer names the PCS
type field explicitly (np_133.ko, `.data` logical-register table entry 0 =
`devad 3 / reg 7 / mask 0xf`):

```
xpcs_10gbase_r_conf()   writes 0        -> 10GBASE-R
xpcs_sgmii_mode_conf()  writes 1        -> SGMII          <- what we have
xpcs_1000base_x_conf()  writes 2        -> 1000BASE-X
xpcs_2p5gbase_x_conf()  writes 0xe      -> 2.5GBASE-X
```

and the 10GBASE-R sequence is (`0x1c3e4`):

```
xpcs_prepare_for_switch_mode(mac, 0)        /* exits sgmii/hsgmii/usxgmii */
write 0 to devad 3 reg 7
read devad 3 reg 0, or 0x800, write, udelay(858), and ~0x800, write
```

It was applied live to XPCS1 (and XPCS0 left untouched to prove the two MDIO
devices are separate — lane 1 still read 0x0001 afterwards):

```
xpcsw 0 0x1f 0 0x0140        /* AN off  */
xpcsw 0 0x1f 0x8000 0x0000   /* DIG_CTRL1 clear */
xpcsw 0 3 7 0x0000           /* PCS = 10GBASE-R */
xpcsw 0 3 0 0x0c40           /* low-power pulse */
xpcsw 0 3 0 0x0440
```

Result: the VEND2 bank (`0x1f`) reads all zero afterwards — it only exists in
the SGMII family — which confirms the switch took.  `0x16105` and the PON MAC
`rx` counter did not change.  So **the PCS type alone is not the fix**, but
the driver must not be leaving this lane in SGMII either.

## 4. Four values in our own bringup are XGS-PON values, not XG-PON

This is a real transcription bug, found by disassembling the right module.
The earlier work read `fi_Configure_Hw_Init()` out of `zte_xgpon.ko` but
followed the **`g_up_mode == 0x80`** branch, which is XGS-PON.

`fi_Configure_Pon_Mode(mode)` (zte_xgpon.ko `0x738c`) is unambiguous, and it
has a log string for each branch:

```
mode 3  (up_mode 0x40, "pon mode is xg")   0x1688e = 0x20
                                           0x1611d low 10 bits = 0x3ff
mode 7  (up_mode 0x80, "pon mode is xgs")  0x1688e = 0x05
                                           0x1611d low 10 bits = 0x3fe
```

and the tail of `fi_Configure_Hw_Init()` picks the word it leaves in
`0x16644`:

```
up_mode 0x80 -> byte1 = 0x13    (0x1301 with byte0 = 1)
up_mode 0x40 -> byte1 = 0x1b    (0x1b01 with byte0 = 1)
```

Our board had `0x1688e = 5`, `0x1611d low10 = 0x3fe` and the driver wrote
`0x16644 = 0x1301` — the XGS triple — while `pon_mode` (register 6 low
nibble) was already 3 = XG-PON, and `pon_regmap.txt` from `xmac_map.py`
shows no other writer for those registers.  The loader's own `0x16644` is
`0x0f02`, which is neither branch's value.

All four were corrected live:

```
w 1688e 00000020
m 1611d 3ff 3ff        (was 0x0ffffe, now 0x0fffff)
w 16644 00001b01       (was 0x00000f02; board did not hang)
m 16100 1 0            (restart the framer)
m 16100 0 1
```

`0x16644 = 0x1b01` wrote and read back cleanly and the board stayed up — the
hang recorded in docs/107 came from writing the XGS word `0x1301`, not from
touching the register at all.  RX still did not start, but the MAC is now
configured for the PON the OLT is actually running.

## 5. A second, smaller transcription bug

`fi_Configure_Hw_Init()` also touches `0x16501` through **byte 2**:

```
ldrb w0, [x29, #0x42]      /* byte 2 */
orr  w0, w0, #1            /* set  bit 16 */
strb w0, [x29, #0x42]
...
and  w0, w0, #0xfffffffd   /* clear bit 17 */
```

Our driver does `zx_xmac_rmw(0x16501, 0x2, 0x1)` — the same two bits, but in
byte 0 instead of byte 2.  Harmless on a register that boots zero, wrong on
principle.

## 6. Where the receive path now stands

State at the end of the round:

```
pon_state               O2-3        (software, zxpond)
0x16105                 0x00000010  LOS clear, LOF set
0x16500                 0x03ff0001  hardware ONU state = O1 (was 0)
0x16100                 0x0000063b  downstream enable bit0 set
0x16101                 0xc5e51840  XG-PON PSync, loader value, untouched by vendor
0x16102 / 0x16103/4     0xfd59bb49 / 0x0f0f0f0f
0x16644                 0x1b01      XG-PON
0x1688e                 0x20        XG-PON preamble repeats
0x1611d                 0x0fffff    preamble all ones
0x1650f                 0           T-CONT bank still empty (no allocation)
pon_hw                  rx=0 tx=10 txerr=1 busy=26800
PON MAC stats 0x5d0..0x5e6  all zero, unchanged over 10 s
XPCS1 PCS type          0 (10GBASE-R)
NPPT+0x2438             0x3  bit2 = 0 -> SerDes to XMAC1 route enabled
NPPT+0x19c              0x1  ROUTE_CLK on
NPPT+0x2c0004           0xffffffff  PON reset released
```

Every MAC-side register the vendor driver writes for XG-PON is now in place,
plus the two blocks that gate the lane (route bit and route clock), and the
framer still reports LOF with zero frames and zero error counts.  A receive
path that produces *no* error counts at all is one that is not being fed,
not one that is rejecting frames — so the next thing to prove is the data
path from the SerDes into the framer (XPCS1 placement, the XMAC1 FIFO RAM
owner, or the `0x16100` bit that arms the framer's own FIFO).

## 7. Tools added this round

Two scripts, both driving the vendor `.ko` files in `pon_revive/oem_rootfs`:

* `tools/dis_ko.py <ko> <symbol|addr> [len]` — disassembles one function with
  `.rela.text` resolved, so a call prints as `bl -> Hal_WriteXmacReg` instead
  of `bl #0x2a54`.  This is what turned `bl #0x4cd4` into a known function
  and what proved our SerDes image is the vendor's.
* `tools/xmac_map.py <ko> [index ...]` — walks every `Hal_ReadXmacReg` /
  `Hal_WriteXmacReg` call site, recovers the register index from the
  `mov`/`movk` pair, and prints which functions touch which register.  Output
  for `zte_xgpon.ko` is saved as `xmac_regmap.txt`; it is how the four XGS
  values above were found.

Both only read the vendor modules; neither touches the board.
