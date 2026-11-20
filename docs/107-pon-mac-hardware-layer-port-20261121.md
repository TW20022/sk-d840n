# The PON MAC hardware layer is now wired into the 6.18 driver

Date: 2026-11-21
Baseline: tag `baseline-20261121-pre-pon-mac-port` = `11e1cf1`
Board state at baseline: `pon_state=O2-3`, `omci_transport error=-67`, no MAC registers written.

## Why this change exists

`docs/106` proved the software activation state machine in `zxpon.c` had no
hardware layer: all PLOAM registers in the NPPT window read zero, the five
PLOAM/OMCI take/rx functions had no production callers, and the vendor's
`zte_xgpon.ko` carried the missing MAC path.  A software-only machine can walk
from O1 to O2-3 but can never receive the OLT's `ASSIGN_ONU_ID`, so O5 is
unreachable.

This change ports the **minimum vendor MAC bring-up and PLOAM transport** into
the in-tree driver.  It does not port the full 104-function vendor stack:
T-CONT/GEM provisioning, OMCI over the data GEM, encryption key programming
and the PPU interrupt path remain separate work.

## What was transcribed

All indices are `Hal_ReadXmacReg`/`Hal_WriteXmacReg` indices from the
AArch64 4.19.136 `zte_xgpon.ko`; byte offset is `idx * 4` inside the NPPT
window already mapped as `eth->base` (`0x19000000`, 16 MiB).  The new helpers
in `zx279133-pon.c` never touch the shared TOPCRM PLL or the PON SerDes lane
image; that side is owned by `phy-zx279133-pon-serdes.c`.

| Function | Vendor source | Registers |
|---|---|---|
| XG-PON mode 7 | `fi_Configure_Pon_Mode(7)` | `0x6`, `0x1688e`, `0x1611d` |
| MAC enable | `fi_Configure_Hw_Init` | `0x16000`, `0x1610b`, `0x16100`, `0x16644`, `0x16600`, `0x1689f`, `0x1a`, `0x16001`, `0x16501`, `0x16402`, `0x3000a`, `0x16635` |
| ONU serial | `fi_Configure_Ponmac_Set_Sn` | `0x16636`, `0x16637` (two byte-swapped words) |
| Data plane | `fi_Configure_Ponmac_Set_Gpon_Enable` | `0x16600`, `0x16100` (only at O5) |
| PLOAM RX | `fi_Configure_Interrupt_Read_Ploam_Msg` | pending `0x16147`, 12 data words `0x16148..0x16153`, clear pending |
| PLOAM TX | `fi_Configure_Ponmac_Send_Ploam_Msg` | kick/status `0x16642`, 12 data words `0x166c2..0x166cd` |

The RX/TX loops are byte-for-byte the vendor protocol: poll up to 16 times,
`rev32()` every word, clear the RX pending flag with a zero write, kick TX
with a one write, and fail TX when the ready bit never clears.  Frames are
only removed from the software queue **after** the MAC accepts them; a new
`zxpon_peek_ploam()` API makes that possible.  `zxpon_retry_sn()` re-queues
the serial number once per second while the ONU sits in O2-3, matching the
standard retry behaviour without flooding the eight-entry queue.

## New observability

`/sys/class/net/eth0/pon_hw` reports the mapped flag, the two live register
words and cumulative counters:

```
mapped=1 rx_pending=00000000 tx_kick=00000001 rx=0 tx=3 txerr=0 last_rx_id=00 data=0 alloc=0000 alloc_valid=0
```

`rx` incrementing with `last_rx_id=03` is the direct proof that an OLT answered
the serial number with `ASSIGN_ONU_ID`; O4/O5 should follow in the same tick.
`txerr` incrementing with `rx_pending=0` means the MAC is not accepting frames.

## Validation criteria after flashing

1. `pon_hw` shows `mapped=1`, and the old all-zero PON window is no longer
   zero for the programmed registers.
2. `pon_state` reaches O4 then O5 after the fibre is connected and the HX5 is
   offline; `onu_id` leaves `0x3ff`.
3. `omci_transport` leaves `error=-67`.
4. `br-lan`, LAN1..LAN4 and the PPPoE configuration are unchanged.

If `rx` stays zero while `tx` grows, the remaining suspects are the vendor
interrupt masks at `0x16005/0x16006` (intentionally not written here) or
T-CONT/GEM provisioning, not the optical path.

## Rollback

Everything is in three source files plus two reverse-engineering tools:
`kernel-overlays/drivers/net/ethernet/zte/{zxpon.h,zxpon.c,zx279133-pon.c}`,
`tools/{dis_annot.py,xmac_writes.py}`.  `git checkout
baseline-20261121-pre-pon-mac-port -- <paths>` restores the exact baseline;
the previous firmware image is recorded in
`pon_revive/baseline_20261121/BASELINE.md`.
