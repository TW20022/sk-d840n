# 115 — The PON block at 0x17000000 is real, clocked, and read/write (bench verified)

Date: 2026-11-23
Board: SK-D840N (zx279133), firmware `ImmortalWrt 25.25.1` kernel `6.18.60+`
Image: `720142fe1eb08c18c7a784b67f28657634dae849fa36ece2a7940ee56bf64623`

## Why this document exists

Everything this project wrote to "PON registers" between docs/107 and docs/113
went to the wrong block. The symptom was always the same: reads came back zero
from `0x16100` upwards, the GEM table's done bit never rose, and the PLOAM
window looked dead. The cause was not missing hardware support — it was the
base address.

This document records the decode that settled it, the two failed attempts, and
the register-level proof that the block now works, all of it with no fibre in
the jack.

## The decode

Three independent sources agree, and none of them is a guess:

1. **Vendor module `zx_ponreg_133.ko`.** `fpga_read_reg_one()` is
   `cmp w0,#0x3fffff` → `ubfiz x0,x0,#2` → `ldr x1,[base]` → `ldr w0,[x1,x0]`.
   The register number is a **word index**: byte address = `pon_base + reg * 4`.
2. **Vendor modules `zte_xgpon.ko` / `gpondrv_xg.ko`.** `Hal_ReadXmacReg()` and
   `Hal_WriteXmacReg()` are thin tails into `fpga_read_reg()` / `fpga_write_reg()`,
   so every `0x16xxx` number in the vendor PLOAM/T-CONT/GEM code goes through
   that same word-index decode.
3. **The stock device tree** (`decompiled/sk-d840n-stock.dts`, node at line 1864):

   ```
   pon@17000000 {
       compatible = "zte,zx279133-pon";
       reg = <0x0 0x17000000 0x0 0x1000000   /* PON MAC, 16 MiB */
              0x0 0x10e00000 0x0 0x1000
              0x0 0x10e10000 0x0 0x10000
              0x0 0x10e20000 0x0 0x1000
              0x0 0x14f11000 0x0 0x1000>;
       status = "okay";
   };
   ```

   `nppt@19000000` is a **separate** node.

Our own tree already carries one register inside that window:
`pon_route: syscon@17000080`, which the NPPT driver writes through a regmap on
every boot (`zx279133-np.c`: `regmap_write(eth->pon_route, 0, enabled ? 0 : 1)`).

## Why the first two attempts hung the board

The first attempt mapped `0x17000000` and ran the vendor bring-up in probe; the
board went off the bus with no serial output and needed a power cycle. The
second attempt wrongly blamed the clocks and poked `topcrm` directly. Both were
wrong, and the clock theory is now disproved by measurement:

- `zx279133-main.c` already calls `clk_bulk_prepare_enable()` on all six PON
  clocks (`idm-aclk`, `tm-aclk`, `pclk`, `smac-wclk`, `woe1-wclk`, `mac-wclk`),
  which are listed in the `nppt` node's `clocks` property.
- Measured after arming: idm 250 MHz, tm 250 MHz, pclk 125 MHz, smac 125 MHz,
  woe1 416.67 MHz, mac 250 MHz. All six are `on`.
- `pon_route` writes at boot already prove the window is decoded and clocked.

So the block was never unclocked. What took the board down was the bring-up
sequence itself, which writes the MAC config words (`0x16644`, `0x16635`,
`0x16000 |= 0x1f`, …) on top of state the **bootloader already programmed**.

## What the bootloader leaves behind

Read after a plain boot, before any write of ours:

| register | value | note |
|---|---|---|
| `0x16000` | `00000000` | |
| `0x16001` | `00000001` | |
| `0x16100` | `0000061a` | downstream bit 0 clear |
| `0x1611d` | `000fffff` | preamble/mode word already set |
| `0x16402` | `00000040` | |
| `0x1650f` | `00000000` | T-CONT bank empty |
| `0x16600` | `00000005` | |
| `0x16635` | `00000001` | enable bit set |
| `0x16644` | `00000f02` | low half `0f02` |
| `0x166c2` | `03ff0100` | a serial-number PLOAM still queued |

`0x166c2 = 0x03ff0100` is the smoking gun: ONU-ID `0x3ff` (broadcast) plus
message id `0x01` (Serial_Number_ONU). The bootloader queued that frame and
kicked it; `0x16642` read back `1` (busy, waiting for an upstream grant) for as
long as no OLT was present. After a reboot the window came back idle
(`0x16642 = 0`).

Two consequences:

- The transmit window is wired and the bootloader's own use of it works. It
  does not need our init.
- Re-running the vendor init would overwrite `0x16644`'s low half from `0x0f02`
  to `0x1301`, i.e. change the MAC mode the bootloader chose. It is now behind a
  separate `force` verb and is not part of normal arming.

The GEM table and the T-CONT bank are **volatile**: after a reboot both read
zero again, so the bootloader configures the MAC but not the tables.

## Register-level verification (no fibre)

Run through `/sys/class/net/eth0/pon_reg` (see below), with the block mapped by
`echo on > /sys/class/net/eth0/pon_arm`.

**Plain reads** — sparse map, plausible content, no hangs:

| index | value |
|---|---|
| `0x16000` | `00000000` |
| `0x16001` | `00000001` |
| `0x16005` | `324b24ba` |
| `0x16008` | `00000088` |
| `0x16100` | `0000061a` |
| `0x16101` | `c5e51840` |
| `0x16147` | `00000000` (no light, nothing pending) |
| `0x20` (pon_route) | `00000000` (matches what NPPT wrote) |

**T-CONT bank `0x1650f`** — the vendor's `fi_Configure_OmccTcont` reads the
word, replaces bits [13:0] with the AllocID, sets bit 14 and writes it back.
Reproduced at the bench and it reads back:

```
w 1650f 4001  -> 1650f <- 00004001 (now 00004001)
r 1650f       -> 1650f = 00004001
w 1650f 0     -> restored
```

This also disproves the earlier note that `0x1650f` is write-only: it reads
back exactly what was written.

**GEM indirect table `0x16205/06/07`** — vendor sequence is: write
`gem | 0x8000000` to `0x16205` (bit 27 = read), wait for `0x16206` nonzero,
read the byte from `0x16207`; for a write, drop bit 27, put the byte in
`0x16207`, wait again. Measured:

| gem | `0x16207` after read |
|---|---|
| `0x0001` | `00000003` |
| `0x07d0` (2000) | `00000003` |
| `0x0544` (1348) | `00000001` |
| `0x0fff` (4095) | `00000002` |

Different address → different data, so the engine answers. And a full
write-read cycle holds:

```
w 16205 544    ; address, write mode
w 16207 2      ; data
w 16205 8000544; address, read mode
r 16207        -> 00000002
... restore to 0 -> 00000000
```

Note `0x16206` reads `1` even at idle, so it behaves as "engine ready" rather
than a one-shot done pulse. `zx_gem_table_wait()` polls it for nonzero exactly
as the vendor does; on this silicon that check returns immediately, which is
why the read data has to be judged on its own.

## Driver changes

`kernel-overlays/drivers/net/ethernet/zte/zx279133-pon.c`

- `pon_arm` is staged and **read-only by default**:
  - `echo map` — map `0x17000000`, print the six clock rates, read five words.
  - `echo on` — same, and mark the block armed so the 100 ms tick may poll.
  - `echo force` — additionally run the vendor bring-up (not needed on this
    board; kept for bench work).
  Every access prints at `KERN_EMERG`, so a stall on the serial console names
  the register that caused it.
- `pon_probe` is read-only: it now reports the bootloader-programmed state
  (`0x16644` mode nibble, `0x16635`, `0x16100`, T-CONT 0, PLOAM TX kick, RX
  pending, the OMCC GEM's table byte) instead of doing a destructive
  write-read test.
- New `pon_reg` gives the bench single-register work without a rebuild:
  `r <hex>`, `w <hex> <hex>`, `m <hex> <and> <or>`, `s <from> <to> <step>`;
  `cat pon_reg` shows what the last command saw.
- Probe no longer touches the block at all; `dmesg` says
  `PON hardware idle until pon_arm is written`.

## How to run it

```
# on the device
echo on > /sys/class/net/eth0/pon_arm
cat /sys/class/net/eth0/pon_probe
cat /sys/class/net/eth0/pon_hw
cat /sys/class/net/eth0/pon_state

# single register, read-only
printf 'r 16100\n' > /sys/class/net/eth0/pon_reg; cat /sys/class/net/eth0/pon_reg
```

Bench-side helpers: `pon_revive/pon_arm.py`, and for a stall the proven recovery
path `pon_revive/uboot_kernel_recover.py` + `tftp_server_min.py` (image in
`pon_revive/tftp/`). Booting never touches the PON block, so a stall always
costs a power cycle, never a brick.

## What is left for the fibre day

1. Insert fibre with the HX5 offline, watch `pon_state` and `0x16147` (RX
   pending) move off O2-3/O1.
2. PLOAM TX is expected to stay busy (`0x16642` bit 0) until an OLT grants an
   upstream slot — that is normal, not a fault.
3. Once the O5 assignment lands, `zxpon_provision_locked()` programs the T-CONT
   bank and the GEM table through the paths verified above; both read back, so
   `pon_tables` is a real acceptance check rather than a hope.
4. OMCI replies then leave through the IDM queue (`zx279133_omci_send`), which
   is unchanged by this work.
