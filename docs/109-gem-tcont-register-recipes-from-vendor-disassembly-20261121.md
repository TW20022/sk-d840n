# GEM/T-CONT register recipes, decoded from the vendor .ko

Date: 2026-11-21 (48 h fibre wait; analysis only — no source change)

`docs/108` established that GEM/T-CONT provisioning is an indirect-table
problem and listed the entry points.  This document goes one level deeper: it
decodes the exact XMAC register programming from the vendored
`zte_xgpon.ko`, so that if the flashed PLOAM-only firmware reaches O5 with
`rx` incrementing, the data-plane bring-up can be implemented without
re-doing the reverse engineering.  **Nothing here is compiled or flashed.**

Sources (all local, under `pon_revive/`):
`vendor_dgem.dis` (downstream GEM create), `vendor_rawtcont.dis`
(raw T-CONT create), `vendor_omcctcont.dis` (OMCC T-CONT),
`vendor_gem_writes.txt` (all `Hal_WriteXmacReg` call sites),
`vendor_int_init.dis` / `vendor_int_handle.dis` / `vendor_rxploam.dis`.
Register index `idx` maps to byte offset `idx*4` in the NPPT window already
mapped by the port driver (`eth->base`, `0x19000000`, 16 MiB).

## 1. Downstream GEM create — the indirect table read-modify-write

`fi_Configure_Gem_Create_Downstream_GemPort` (`.text+0x1100`, 0x2d8 bytes),
decoded from `vendor_dgem.dis`.  The indirect table is three XMAC registers:

| idx | role |
|---|---|
| `0x16205` | address / trigger (write `addr | 0x08000000` to *read*, `addr` to *write*) |
| `0x16206` | completion flag: poll until non-zero, 4096-iteration bound |
| `0x16207` | data in/out |

Sequence (guarded by `fpga_io_lock` + `daifset #2`, i.e. a spinlock with local
IRQ disable — the port driver must use the same lock discipline):

1. `gem_id = w0 & 0xffff`; reject `gem_id > 0xfffe`.
2. Resolve a database index via `fi_database_gemport_to_index()` (or allocate
   a free slot with `fi_database_get_empty_gemport_index()`); abort if the
   slot is already in use (`XgponDataBase + idx*4 + 0x215 & 1`).
3. **Read**: write `0x16205 = gem_id | 0x08000000`; poll `0x16206` up to
   0x1000 times for non-zero (else log `indirect_table read time out!`);
   read the data word from `0x16207` into a local byte (`[x29,#0x68]`).
4. Modify that byte: `value |= 0x02`  (bit 1 set — this is the entry's
   present/enable bit in the table word).
5. **Write**: write `0x16205 = gem_id` (flag bit cleared via
   `and w1, w24, #0xf7ffffff`); write `0x16207 = value`; poll `0x16206`
   again up to 0x1000 times (else `indirect_table write time out!`).
6. Update the software mirror `XgponDataBase` under its semaphore: at
   `base + idx*0x68`: `+0x214 = 1`, `+0x215 |= 1`, `+0x217 = gem_id`
   (`sturh`).  For `idx <= 0x3f` also `base + idx*0x68 + 0xc14 = 1`,
   `+0xc16 = gem_id`.

Note the two poll loops both read `0x16206`; a zero read with the counter
exhausted is the timeout.  The 4096 bound and the exact flag bit
`0x08000000` are the parts most likely to be got wrong by a from-scratch
implementation.

## 2. Raw T-CONT create — AllocID bank at `0x1650f`

`fi_Configure_Gem_Create_Raw_Tcont` (`.text+0x5ec`, 0x290 bytes), decoded from
`vendor_rawtcont.dis`.  Contrary to the prose in `docs/108`, the address is
**not** a plain `0x1650f + idx`: the compiler emits
`add w20, w0, #0x16, lsl #12` then `add w20, w20, #0x50f`, i.e.
`idx_word = 0x16000 + 0x50f + w0 = 0x1650f + w0`.  So the base is right;
what matters is how `w0` (the register-word index) is derived from the
T-CONT id `t = w19`:

```
w1 = tcont_id
w2 = w1 + 1
w23 = (w1 != 0) ? (w2 & 1) : 0        # parity/offset selector
w0 = w2 >> 1                          # (t+1)/2
if (w1 - 0x23) <= 4:  w0 += 0xa       # t in 0x23..0x27
elif w1 != 0x40:      w0 = 0x1e       # catch-all for large t
addr = 0x1650f + w0
```

Each 32-bit bank register holds two T-CONT entries (low halfword and high
halfword); `w23` selects which halfword the RMW touches:

- if `w23 == 0`: use `[x29,#0x40]` (low half) / byte `+1`
- if `w23 != 0`: use `[x29,#0x42]` (high half) / byte `+3`

The value programming (identical in both the "existing AllocID" path at
0x6ac and the "new T-CONT" path at 0x780):

```
old = Hal_ReadXmacReg(addr)
half = bfxil(old_halfword, alloc_id, 0, 14)   # low 14 bits = AllocID
byte = (half >> 8) | 0x40                     # bit 6 of upper byte = valid
Hal_WriteXmacReg(addr, new_word)
```

So: **AllocID occupies bits [13:0] of the halfword, and bit 6 of the
upper byte of that halfword is the valid flag.**  `bfxil` preserves all
other bits of the read-modify-write.  For the OMCC T-CONT this is hardwired
to `t = 0` (`addr = 0x650f` word index, low half, valid bit set) — see
`vendor_omcctcont.dis` `fi_Congigure_Creat_Omcc_Tcont` at 0x3d4, which uses
`bfxil(...,0,14)` + `orr #0x40` on `0x650f` with no parity branch.

The same halfword layout appears in `fi_Configure_SN_AllocId_Set`
(`vendor_gem_writes.txt`): `idx=0x10 val=0x3ff` writes the initial broadcast
AllocID `0x3ff` at XMAC `0x10`.  `0x3ff` is the sentinel `docs/107` expects
`onu_id`/AllocID to leave once the OLT answers.

## 3. Interrupt masks 0x16005/0x16006 — status of the lead

`docs/107` names `0x16005/0x16006` as the "intentionally not written" vendor
interrupt masks, a suspect for `rx=0` with `tx` growing.  I searched all
`vendor_*.dis` traces: **no `0x16005`/`0x16006` literal appears**.  The PLOAM
RX path (`vendor_rxploam.dis`, `fi_Configure_Interrupt_Handle_Ploam_And_Event`)
polls the RX-pending word `0x16147` + 12 data words `0x16148..0x16153`
directly, which is what the port already transcribed.  Therefore there is
currently **no decoded evidence that 0x16005/0x16006 must be written** for
PLOAM RX; the lead is a hypothesis in `docs/107`, not a confirmed missing
register.  Do not patch it speculatively.  If post-fibre `rx` stays 0 while
`tx` grows and LOS is clear, the next step is to disassemble the vendor's
interrupt-enable setup around `Hal_ReadXmacReg`/`Hal_WriteXmacReg` in
`fi_Configure_Interrupt_*` with `tools/xmac_writes.py` and confirm the
indices before touching the driver.

## 4. Decision gate (unchanged from docs/107/108)

These recipes are **not** to be implemented on spec.  The gate is: flashing
the current PLOAM-only firmware, connecting fibre with HX5 offline, and
observing `rx` increment + `pon_state` O5.  Only if that passes does the
data-plane work (indirect table + T-CONT bank + the PPU/NPC table entries in
`np_133.ko` that classify downstream GEM ids) become worth doing.  Use
`pon_revive/fiber_check.py` for the post-fibre read-only judgement.

## Reproduce

```
cd pon_revive
PYTHONPATH=../repo/tools python ../repo/tools/xmac_writes.py \
    oem_rootfs/kmodule/zte_xgpon.ko 'Gem_Create|Raw_Tcont|Omcc_Tcont'
```
The `vendor_*.dis` files are the captured annotated disassembly of these
functions; regenerate with `tools/dis_annot.py` if needed.
