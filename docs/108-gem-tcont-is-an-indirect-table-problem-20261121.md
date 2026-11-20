# GEM/T-CONT provisioning is an indirect-table problem, not another PLOAM loop

Date: 2026-11-21 (while CI run 44832838189 builds the PLOAM port)
Status: analysis only — no source change in this document.

`docs/107` wires PLOAM so the ONU can reach O5.  The next thing needed for
PPPoE is a data GEM port mapped to a T-CONT.  The software MIB already learns
the OLT's `ASSIGN_ALLOCID` and the OMCI-created GEM port
(`zxpon_add_tcont()` / `zxpon_map_gem()`), but the hardware side is still
unimplemented: the four netlink commands `ADD_TCONT`, `ADD_GEM`, `MAP_GEM`
and `SET_KEY` return `-EOPNOTSUPP`.

## What the vendor module actually does

Disassembling `zte_xgpon.ko` with `tools/dis_annot.py` and
`tools/xmac_writes.py` gives a clear split:

* `fi_Configure_Gem_Create_Upstream_GemPort` (`.text+0xfb8`, 0x148 bytes)
  touches **no XMAC register at all**.  It only allocates a slot in
  `XgponDataBase`, stores the GEM id at `DB + idx*0x68 + 0xc17`, and marks
  the entry in use.  Upstream GEM classification lives in the packet-engine
  tables (`np_133.ko`), not in the PON MAC register window.
* `fi_Configure_Gem_Create_Downstream_GemPort` (`.text+0x1100`, 0x2d8 bytes)
  uses an **indirect table**: write the target address to XMAC `0x16205`
  (with a `0x08000000` flag bit), poll `0x16206` for completion, then
  read/write the data word through `0x16207`.  Both directions carry a
  4096-iteration timeout and log `indirect_table read/write time out!`.
* `fi_Configure_Gem_Create_Tcont` (`.text+0xb20`, 0x38c bytes) stores the
  AllocID into a bank of registers based at `0x1650f`, using
  `idx = tcont_id/2` (or `tcont_id/2 + 0xa` past id 0x23), the low 14 bits
  for the AllocID and bit 6 of the upper byte as a valid flag.
  It also maintains the same `XgponDataBase` mirror.
* `fi_Congigure_Creat_Omcc_Tcont` (`.text+0x3d4`) uses the same register bank
  for the OMCC T-CONT.
* `fi_Configure_SN_AllocId_Set` (`.text+0x79ac`) writes the initial broadcast
  AllocID `0x3ff` at XMAC `0x10`.

This is why a few more `writel()` calls cannot finish the job: the missing
piece is the indirect-table protocol plus the PPU/NPC table entries that
classify downstream GEM IDs and route upstream traffic.

## Concretely, before PPPoE can work

1. Record the OLT-assigned AllocID in `struct zxpon` (currently
   `zxpon_add_tcont()` discards it) and expose it to the glue layer.
2. Implement the `0x16205/0x16206/0x16207` indirect read/write with bounded
   polling, then transcribe the downstream GEM table entry layout from
   `fi_Configure_Gem_Create_Downstream_GemPort`.
3. Program the T-CONT AllocID bank at `0x1650f + idx`, including the valid bit.
4. Find the PPU table writes that map a data GEM to the CPU TX/RX queues —
   likely in `np_133.ko`'s `np_user_table_init()` path, not `zte_xgpon.ko`.
   The old 4.19 firmware already knew the real Jiangsu mapping:
   VLAN99 → GEM port 1348 / T-CONT 1, with a dynamic AllocID.
5. Only then replace the four `-EOPNOTSUPP` netlink handlers and use the OMCI
   over GEM path instead of the current raw PLOAM transport.

Nothing here changes the conclusion from `docs/107`: the first firmware to
come out of CI validates PLOAM-only bring-up.  Seeing `rx` increment and the
state reach O5 is the gate before investing in the indirect tables.

## Tools added

* `tools/dis_annot.py` — disassembles a `.ko` range and annotates each
  relocation with its symbol name.
* `tools/xmac_writes.py` — reconstructs `Hal_WriteXmacReg(idx, value)` call
  sites, including `movz/movk` index pairs such as `0x16147`.

Generated traces are kept under `pon_revive/vendor_*.dis` (local working
data, not shipped in the repo).
