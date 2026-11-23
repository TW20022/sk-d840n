# 117 — The PLL was on the Ethernet branch, and the framer still has no input (2026-11-23, evening)

Follow-up to docs/116. Fiber is in the whole time. Every claim below is either a
register read on the board (`soc`/`xpcs`/`pon_reg`) or an instruction sequence in
the vendor's own modules (`np_133.ko`, `zte_xgpon.ko`), not an inference.

## 1. What got ruled out this round

| Suspect | How it was checked | Result |
| --- | --- | --- |
| PON SerDes → XMAC1 routing | `pon_route` (0x17000080) = 0, `NPPT+0x2438` bit2 = 0 | connected |
| Shared SerDes/XMAC1 clock | `NPPT+0x19c` bit0 = 1 | on |
| PON block clocks | `clk_summary`: `pon_mac_wclk` 250M, `pon_tm_aclk`, `pon_idm_aclk`, `pon_smac_wclk`, `pon_serdes_pclk` all enabled | on |
| PON reset | `NPPT+0x2c0004` = 0xffffffff | released |
| XPCS1 existence / identity | `xpcs 0 3 7` = 0 (10GBASE-R), `xpcs 1 3 7` = 1 (SGMII) — the two lanes answer differently, so the verb really does reach two devices, and XPCS1 is the PON lane as `pcs-handle = <&xpcs1>` says | confirmed |
| PON SerDes image | all 49 words of 0x00..0xc0 match `mode_xgpon_nsyn_cfg` word for word, plus the driver's two post-activation words (0xb0 = 0x20200000, 0xb4 = 0x00808020) | correct |

So the wiring, the clocks, the reset and the lane image are all in place, and
`0x160000e4` still says `rx los = 0, cdr_lock = 1`.

## 2. The one real defect found: Linux was overwriting the PON PLL

`np_133.ko`'s `pon_pll_cfg(mode)` has three branches, and they are not
interchangeable:

```
mode <= 4   (EPON family)   +0x10 bits5:4 = 2, +0xc0 = 0x20101054, +0xc4 = 0x04000000
mode 5..7   (GPON / XG-PON) +0x10 bits5:4 = 2, +0xc0 = 0x00202054, +0xc4 = 0x042673e2
mode 8..15  (Ethernet 10G/5G/2.5G/1G) +0x10 bits5:4 = 0, +0xc0 = 0x20106454, +0xc4 = 0x04000000
```

All three finish with the same strobe: `+0x0c |= BIT(9)`, `+0x0c &= ~BIT(9)`.

`phy-zx279133-pon-serdes.c` was writing the **mode 8..15** pair unconditionally,
i.e. the Ethernet profile, even when the lane had been put into the XG-PON NSYN
image. That is a 10.3125 GHz / 9.95328 GHz mismatch on the very PLL the lane
runs from.

The boot log now prints both sides of the write, which is what proves the
bootloader had it right and Linux was the one breaking it:

```
zx279133-pon-serdes 16000000.phy: PON PLL PON branch:
    cfg0 0x40202054 -> 0x40202054, cfg1 0x42673e2 -> 0x42673e2, mux 0x6713277
```

`0x40202054` is the mode 5..7 value with the hardware's read-only bit 30 on top
(that is the `ZX279133_PON_PLL_CFG0_FW_BIT` the old code preserved). So U-Boot
leaves the PON branch in TOPCRM, and the old driver replaced it with
`0x60106454` / `0x04000000` on every boot.

Fixed in `zx279133_pon_serdes_configure_pll()`: `priv->xgpon` now takes the PON
branch (ref select 2, `0x00202054` / `0x042673e2`, mode field cleared and
reloaded by the bit-9 strobe); the Ethernet branch is unchanged, so the
validated 2.5GBASE-X / LAN path is untouched.

## 3. …and it was not enough

After flashing that build and rebooting:

```
16105 = 00000010      LOF still set, LOS still clear
16504 = 00000000      superframe counter frozen (read twice, 2 s apart)
5d0..5e6 = 0          no frame/error counters at all
xpcs 0 3 1 = 0x00c2   block lock bit 0 = 0, Hi BER = 1
pon_state = O2-3, rx = 0
```

So the PLL fix removed a real defect but the framer still receives nothing.
A framer that produces zero error counts is not rejecting frames, it is not
being fed.

## 4. Next suspect: the XMAC1 clock gate

`np_133.ko` has the lower-layer clock API the vendor uses around the PON MAC:

```
low_power_api_nppt_open_xmac1_clk():
    agc_set_reg_by_attr(0x10, 1,    0x0001000b)   -> AGC reg 0x10, bit 11 = 1
    agc_set_reg_by_attr(0x0f, 0x3f, 0x00060010)   -> AGC reg 0x0f, bits 21:16 = 0x3f
low_power_api_nppt_close_xmac1_clk():
    same two, both to 0, in the opposite order
```

`agc_set_reg_by_attr(attr, val, field)` is a read-modify-write where `field` is
`(width << 16) | shift`. The AGC table (parsed out of `.data+0x27948`, 28-byte
entries, same layout as `sdet_reg_table`) resolves the two ids:

```
attr 0x10 -> devad 3, reg 0xb0001, mask 0xffffffff
attr 0x0f -> devad 3, reg 0xb0006, mask 0x0fffffff
```

`agc_reg_read()` picks the accessor's `which` from the attr: 0..9 → 0,
0xa..0x13 → **2**, 0x14..0x18 → 1. So XMAC1's clock gate lives on the third
register window, not on either of the two the debug `xpcs`/`xpcsw` verbs reach.
That is the next thing to open up.

Consistent with this: `0x16000` — the register `fi_Configure_Hw_Init` does
`read | 0x1f` on before anything else — will not hold its bits on this board.
Written as `0x1f` it reads back `0x07`, then `0x05`, then `0` two seconds later.
Bits that decay like that are a handshake the block never completes, which is
what a stopped datapath clock looks like from the register file side.

## 5. Corrections to earlier notes

* `0x16000` and `0x1610b` are **not** missing configuration. The vendor's
  `fi_Configure_Hw_Init` writes `0x16000 |= 0x1f` and `0x1610b |= 0x80000`, and
  neither value survives on this board; they behave as self-clearing
  reset/ack bits, not as sticky enables. Do not spend another round on them.
* `fi_Configure_Pon_Mode` confirmed again for mode 3: `0x1688e = 0x20` and
  `0x1611d |= 0x3ff`. The two values fixed in docs/116 are what the vendor
  writes; nothing else in that function differs by mode.
* `0x16010000` is not a separate PLL: words 0x00..0x3f read back identical to
  0x16000000, and `0x160100c0/c8/cc` are read-only (writes of `0x00102371` to
  c0 stick; c4/c8/cc do not). The `pon_set_pll_pon_cfg_with_ref_clk_25M()`
  sequence in np_133.ko therefore cannot be replayed from the `socw` verb, and
  is probably not the path this board uses.

## 6. Tools added

```
tools/sdet_table.py    parse sdet_reg_table (.data+0x23c8) -> sdet_reg_table.txt
tools/serdes_cfg.py    <addr> <len>  extract (reg, value) pairs from a straight-line
                                     SerDes config function; used to diff
                                     mode_xgpon_nsyn_cfg against mode_xgpon_syn_cfg
tools/xmac_writes.py   <addr> <len>  extract (index, value) writes issued through
                                     Hal_WriteXmacReg, with computed values marked
```

`tools/xmac_writes.py fi_Configure_Hw_Init` prints the whole 60-step init in
order, which is how the 0x16000/0x1610b pair above was settled.

## 7. State after this round

* Kernel on flash: `9fde5c3f8e9ef65b142fb0010fb1e29d2babf96b870fc465a58eeb1685a5983e`
  (7559483 bytes), read back from `/dev/mtd2` and matched before rebooting.
* Rollback image untouched: `/root/mtd2-prearm.bin` (8388608 bytes).
* Board: `pon_state = O2-3`, `rx = 0`, `tx = 3`, `txerr = 0`, `16105 = 0x10`,
  `16504 = 0`, XPCS1 10GBASE-R without block lock.
