# The new kernel has NO PON MAC hardware layer — and the vendor PLOAM register protocol is now fully decoded

Date: 2026-11-20 (late night, LAN4 SSH restored)
Author: reverse engineering session, task #78

## The decisive finding

`kernel-overlays/drivers/net/ethernet/zte/zx279133-pon.c` (439 lines) and
`zxpon.c` contain **no `ioremap`, no `readl`, no `writel`, no `void __iomem`**.
`zx279133_pon_attach()` only calls `zxpon_init(&zx_state)` — an in-memory
state machine. The whole PON MAC hardware layer is absent.

Live proof (LAN4 SSH, `reg` debug = `readl(eth->base + off)`):

```
PLOAM RX status  nppt+0x5851c (vendor XMAC idx 0x16147) = 0x00000000
PLOAM RX window  nppt+0x58520..0x5854c (idx 0x16148..0x16153) = all 0
PON enable region nppt+0x59f00..0x59f3c = all 0
TX status        nppt+0x59908 (idx 0x16642) = 0x00000000
```

Every PON register is zero because the new kernel never writes them. This is
not a SerDes, optical, BOSA, identity, or config problem. The hardware PON MAC
is simply never brought up. This is the deepest root cause, and it explains
why the state machine (fixed in 4b781e3) can reach O2-3 in software but can
never complete the round trip to O5.

## Vendor PLOAM protocol, decoded from zte_xgpon.ko (AArch64, 4.19.136)

The vendor accesses PON MAC registers through
`Hal_ReadXmacReg(idx, &val)` / `Hal_WriteXmacReg(idx, val)`, which resolve to
`fpga_read_reg` / `fpga_write_reg` in `zx_ponreg_133.ko` and finally to
`pon_base + (idx << 2)`. `pon_base` is the same NPPT window that the new
kernel maps as `eth->base` (base physical `0x19000000`, window `0x1000000`);
its XMAC0 at `+0x140000` matches the vendor's `xmac0_pcs_base = 0x140000`.
So `idx` maps to `eth->base + idx*4`.

### Downstream (OLT -> ONU) PLOAM receive — `fi_Configure_Interrupt_Read_Ploam_Msg` (0xc6b0)

1. Poll `Hal_ReadXmacReg(0x16147)` up to 16 times; bit0=1 means a PLOAM frame
   is pending. If the first poll's bit0 is already set, treat as "read start".
2. Guard: if pending count (a global at `.bss+0x18`) > `0x7cf` (1999), log
   "ploam out of process" and skip.
3. Allocate an skb, then read 12 consecutive registers `0x16148..0x16153`
   into it, applying `rev32()` to each word (48 bytes = one PLOAM frame).
4. Clear the pending flag: `Hal_WriteXmacReg(0x16147, 0)`.
5. Enqueue the skb onto the receive queue and bump the counter.

`fi_Configure_Ponmac_Get_Ploam_Msg` (0x5d68) is the same 12-register read
without the interrupt/skb wrapper — confirming the RX window layout.

### Upstream (ONU -> OLT) PLOAM transmit — `fi_Configure_Ponmac_Send_Ploam_Msg` (0x5c28)

1. Poll `Hal_ReadXmacReg(0x16642)` up to 16 times waiting for the TX-ready bit
   to clear; if it never clears, return `0x1022` (failure).
2. Copy the 48-byte frame to a stack buffer, then for `i = 0..11` write
   `rev32(frame[i*4..])` to `Hal_WriteXmacReg(0x166c2 + i, word)`.
3. Kick transmission: poll `Hal_WriteXmacReg(0x16642, 1)` then wait for
   completion (same 16x poll loop on `0x16642`).

### Constants (all XMAC idx, byte offset = idx*4)

| Meaning | idx | byte off (nppt+) |
|---|---|---|
| PLOAM RX pending/status | 0x16147 | 0x5851c |
| PLOAM RX data[0..11] | 0x16148..0x16153 | 0x58520..0x5854c |
| PLOAM TX ready/kick | 0x16642 | 0x59908 |
| PLOAM TX data[0..11] | 0x166c2..0x166cd | 0x59b08..0x59b34 |

## Why this matters

The vendor module exports the complete API (`zxic_api_gp_set_ponsend`,
`set_ponrcv`, `add_gemport`, `set_tcont`, `get_onuid`, ...), depends on
`np_133,zx_ponreg_133`, and its `/proc/gpon_sdk/debug` has 44 commands
(`pploam`, `pomci`, `sponen`, `stcont`, `sugemp`, ...). The full bring-up is
`fi_Configure_Init` -> `fi_Configure_Hw_Init` -> `fi_Configure_Interrupt_Init`
-> `register_xgmac_int` -> activation state machine. Reimplementing all of
that in the mainline overlay is a real driver port, not a small patch.

## Realistic paths forward

1. **Port the vendor PON MAC layer** into the new kernel: ioremap PON window,
   bring up XMAC/GPON registers, run the decoded PLOAM TX/RX loops from
   `zxpon_tick_work`, feed frames into `zxpon_rx_ploam()` /
   `zxpon_take_ploam()`. Then add T-CONT/GEM provisioning for O5. Large but
   well-defined now that the register protocol is decoded.
2. **Run the vendor modules** on the new kernel — blocked by the 4.19.136
   vermagic/ABI; needs vendor source to rebuild, which we do not have.
3. Accept the new kernel is a PON software model only and revert to a 4.19
   vendor-driver firmware for anything needing real PON.

## Files / tools produced

- `tools/dis_ko.py`, `tools/dis_range.py` — capstone disassembler for vendor .ko
- `repo/pon/zte_xgpon-api.txt`, `zte_xgpon-proc-cli.txt` — prior reverse-engineering assets
