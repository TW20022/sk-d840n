# 120 — Three NP registers that explain the silent PON datapath

Date: 2026-11-24
Board: SK-D840N / ZX279133, ImmortalWrt 6.18.60, PON port without light during the
measurement (HX5-9saLite held the fibre).

## How the measurement was taken

The vendor userspace for this exact board had been sitting inside the retail
firmware package all along, as a FIT triple: `kernel_133` (the same bytes as our
`stock_Image`), `fdt_133` and `initramfs_133`.  `initramfs_133` is an LZMA
(`FORMAT_ALONE`) cpio holding the complete vendor system — 42 modules under
`/kmodule`, `bin/sigpon`, `bin/cspd`, `bin/devmem2`, `etc/mcode/mcode_133.bin`.

Booting it from RAM on the SK, without touching flash:

```
loadx 0x88000000   uImage-stock-gz-8008      (stock Image, load/entry 0x80080000)
loadx 0x8b800000   stock.dtb
loadx 0x8c000000   uInitramfs-lzma           (-T ramdisk -C none)
setenv bootargs 'console=ttyAMA0,115200n8 rdinit=/bin/sh printk.time=1 \
                 UpModeInfo=0x00000040 regioncode=0x0'
bootm 0x88000000 0x8c000000 0x8b800000
```

Three things had to be right and each one cost a run:

* the kernel must be loaded at `0x80080000` — the vendor Image has
  `text_offset = 0x80000` and is not relocatable, unlike ours (`0`);
* the initramfs must be **LZMA** — the vendor kernel has no `CONFIG_RD_GZIP`
  (`rootfs image is not initramfs (compression method gzip not configured)`);
* `rdinit=/bin/sh` skips `/etc/init.d/rcS`, so nothing mounts `mtdblock8`, nothing
  writes a config partition, and the only thing that runs is the shell we type at.
  `setenv` without `saveenv` keeps the environment change in RAM.

Then, in the vendor shell, mount `/proc` and `/sys`, and load the chain from
`/etc/rcS.d/S99modules` by hand:

```
insmod bspdriver.ko
insmod np_133.ko          # NPPT, BMU, BPPE, hash table, PPU 0x0108.0x31ba6a03
insmod zx_ponreg_133.ko   # vendor's /dev/fpga0 register window, major 222
insmod ffe.ko
insmod zte_xgpon.ko       # "pon mode is xg"
insmod gpondrv_xg.ko      # "gpon_Init ok"
insmod netdriver.ko       # needs zte_xgpon's xpon_netif_rx / Xpon_Setpdtdevname
...
```

`np_133.ko` reports `init np success` on our board, and the PON netdevices `pon`,
`omci`, `sw` appear.  `sigpon -gstate` reports `O1`, which is correct: no fibre on
this port, and no identity was configured (`-sreginfo_xg` segfaults without the
cspd context).

The PON MAC block itself cannot be read from user space under the vendor kernel:
`/dev/mem` at `0x17000000` faults (`Bus error`), because the vendor reaches it
through its own `zx_ponreg_133` mapping.  The NPPT window `0x19000000` reads fine.

## The comparison

Full `0x19002000`–`0x19003000` dump (1021 words) via `devmem`, against the scan we
keep of our own driver on the same board, and against the live HX5 at O5:

| NPPT reg | our driver | vendor `np_133` on SK | HX5 live (O5, traffic) |
|---|---|---|---|
| `0x2438` ROUTE_CTRL | `0x3` | **`0x7`** | **`0x7`** |
| `0x2040` SDET_FRAME_CFG | `0x0200000c` | **`0x0fa0001f`** | **`0x0fa0001f`** |
| `0x2044` SDET_DOWN_FRAME_CFG | `0x4800000c` | **`0x7e800001`** | **`0x7e800001`** |
| `0x2440` / `0x2444` SDET FIFO | `0` / `0` | `0` / `0` (no light) | `0x2e152e15` / same |

`0x190021c0`/`0x190021c4` also differed between the scans, but repeated reads show
they are live counters, not configuration.

Decoding with the layout the driver already uses, `(max << shift) | min`:

* `0x2040`: max 16000, min 31 (was 2048 / 12)
* `0x2044`: max 32384, min 1 (was 2048 / 12)

So the downstream window our driver programmed was 2 KiB wide where the working
systems use 32 KiB — every GEM frame past 2 KiB fell out of the SDET window.

And `ROUTE_CTRL` bit 2 (`ROUTE_XMAC1`) **must be set**: our `zx279133_route_set()`
cleared it on enable (`value & ~ROUTE_XMAC1`), which leaves the PON MAC unrouted
out of the NP.  Both the vendor module on this board and the working HX5 keep all
three bits (`0x7`) while the PON path is live.

## What was changed

`zx279133-np.c`, `zx279133_route_set()`: bit 2 is now set on enable and cleared on
disable.  `zx279133.h`: the four SDET length constants carry the measured values.

Verified on the flashed kernel: after a cold boot, without any manual write,

```
0x19002438 = 0x00000007
0x19002040 = 0x0fa0001f
0x19002044 = 0x7e800001
```

Firmware: `zxpon-inject-uImage`, 7 559 511 bytes,
sha256 `6b268ba480f0df64857ffbc5c42e86a66644628ce69360ed99ab2943d3e5de72`.

## Open

`rx` still reads 0 and the SDET FIFO stays empty, because there is no light on this
port — the decisive check needs the fibre moved back to the SK (or an upstream
signal), which is the next step.  The runtime form of the same test is available
without a rebuild: `wrreg 0x19002438 0x7`, `wrreg 0x19002040 0x0FA0001F`,
`wrreg 0x19002044 0x7E800001` after `echo force > /sys/class/net/eth0/pon_arm`.
