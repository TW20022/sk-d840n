# The PON state machine advances now, but its TX/RX glue to the MAC is missing

Date: 2026-11-20

## Result of the run7 firmware (`skd840n_flash_7_44739960279`)

Flashed by `sysupgrade -k` (config preserved: SN, RegID, PPPoE, LAN all intact).
The two previous fixes are confirmed working on the live kernel:

- SerDes: `PON SerDes image: XG-PON NSYN` / `XG-PON NSYN SerDes ready,
  status 0xc670bb83/0x1000706` in dmesg (the shared-clock-safe profile).
- State machine: `pon_state` moved `O1 -> O2-3` and stays there. The live
  `struct zxpon` reads `state=23` (O23), `los=0 lods=0 has_identity=1`,
  i.e. the two dead flags no longer pin the machine in O1.

So the earlier diagnosis (doc 103) was correct and its fix works. But the
machine does not go past O2-3, and this document records why.

## The decisive evidence: a correct SN-Request is built, yet the machine stays in O23

Reading the live `struct zxpon` out of `/dev/mem` (base `0xffff8000814410f8`,
linear map, found by the RegID bytes at struct offset 12), sampled 40 times
at 0.25 s intervals — identical every time:

```
state=23 (O23)  los=0  lods=0  has_id=1  onu_id=0x3ff
ploam_tx[0] = 00000000 03ff0100 434d4443a133e348 0000... c9364197
              ^ 8-byte PLOAM header ^ SN=CMDCA133E348       ^ MIC
ploam_tx_n  = 0          (struct offset 488; slot 0 still holds the frame body)
```

`frame[0:8] = 00 00 00 00 03 ff 01 00` is a broadcast (`onu_id=0x3ff`)
SERIAL_NUMBER message; `frame[8:16]` is the eight-byte SN `CMDCA133E348`.
It is a complete, correct upstream serial-number PLOAM — proof that the
state-machine's SN assembly works. Its slot retains the frame body while the
queue count is 0, i.e. the frame was consumed (taken or dropped) at least
once and is not being re-queued. Note: an earlier reading used offset 492
(`omci_tx`, not the counter) and wrongly reported `ploam_tx_n=1`; the
correct offset per `struct zxpon` is 488. Nothing in the live state changes
over 10 s: no periodic SN re-send, no onu_id assignment.

## Root cause: the TX and RX halves of the PON data path are not wired up

A full call-graph over `kernel-overlays/drivers/net/ethernet/zte/` shows that
these five functions have **definitions but zero callers** anywhere in the
kernel:

```
zxpon_take_ploam()   zxpon.c:480   - pop a queued upstream PLOAM for TX
zxpon_ploam_pending()zxpon.c:493   - TX queue depth
zxpon_rx_ploam()     zxpon.c:395   - feed a received downstream PLOAM
zxpon_take_omci()    zxpon.c:691   - pop a queued upstream OMCI for TX
zxpon_rx_omci()      zxpon.c:664   - feed a received downstream OMCI
```

They are declared in `zxpon.h` and unit-tested in `tests/zxpon_test.c`, but
no production driver calls them.

Consequences, all matching the live state exactly:

1. **RX (certain)**: nothing calls `zxpon_rx_ploam()` with frames from the
   PON MAC, so even if the OLT did send ASSIGN_ONU_ID / RANGING / OMCI, the
   state machine could never consume them. `onu_id` stays `0x3ff`, no
   GEM/T-CONT is ever provisioned. This alone pins the machine in O23
   regardless of the TX side.
2. **TX (strongly indicated, not fully proven)**: no production caller of
   `zxpon_take_ploam()` / `zxpon_ploam_pending()` exists. Whether the one
   assembled SN frame ever reached the PON MAC cannot be confirmed from the
   software struct; the live queue count is 0 with the frame body left in
   slot 0, and there is no periodic re-send. Either way, with RX broken the
   round trip can never complete.
3. **OMCI**: `zxpon_omci_channel()` requires state O5; we are stuck in O2-3,
   so `zxpon_omci_tx_cmd()` returns `-ENOLINK` = **-67** — the error we have
   been seeing all along. `-67` is a *symptom* of being stuck in O2-3, not a
   separate fault.

The genetlink family `zxpon` only offers TX/status/config commands
(`OMCI_TX`, `GET_STATE`, `SET_IDENTITY`, `ADD_TCONT`, `ADD_GEM`, `MAP_GEM`,
`SET_KEY`); there is no RX-inject command, and `ADD_TCONT`/`ADD_GEM`/`MAP_GEM`/
`SET_KEY` all return `-EOPNOTSUPP` (no backend).

## Why this is a software defect, not the fibre / optics / BOSA / credentials

- The SN, RegID and OMCC are accepted by the kernel (`kernel_identity_error=0`).
- The SN-Request is assembled **correctly** with the Jiangsu SN and a MIC.
- The XG-PON SerDes image loads and reports CDR lock.
- The same fibre registers on the HX5 (user-confirmed).
- The failure is a complete, reproducible software-level stall: a correct
  frame is built and then never transmitted.

This is the same class of defect as the `lods` dead flag in doc 103: a
state-machine/logic layer was written, but the glue to the hardware data path
was never completed.

## What a real fix needs

The vendor's closed-source `zte_xgpon.ko` has this path complete
(doc 20): `fi_Configure_Ponmac_Get_Ploam_Msg`,
`fi_Configure_Interrupt_Read_Ploam_Msg`, `fi_Configure_Ponmac_Send_Sr_Ploam_Msg`
— an interrupt/poll loop that moves PLOAM frames between the PON MAC hardware
registers and the G.984 state machine, plus T-CONT/GEM/Alloc-ID provisioning
after O5.

Our 6.18 `zxpon.c` reimplements the state machine logic but not that glue.
Completing it requires, on the live kernel:

1. A periodic work (extend `zxpon_tick_work`) that calls
   `zxpon_take_ploam()` and writes each frame into the PON MAC upstream
   PLOAM register/DMA, and reads downstream PLOAM registers into
   `zxpon_rx_ploam()`.
2. The PON MAC PLOAM TX/RX register addresses for the ZX279133 (to be
   recovered from `zte_xgpon.ko` disassembly — the vendor symbols above give
   the exact access pattern).
3. T-CONT / GEM-port / Alloc-ID provisioning once O5 is reached, so OMCI and
   data GEM ports open.

This is substantially more work than the two flags/SerDes fixes. It is the
honest remaining gap between "the PON state machine runs in software" and
"the ONU registers on the OLT".

## Verification criteria for the eventual fix

- A downstream PLOAM actually reaches `zxpon_rx_ploam()` (add a counter/debug).
- `onu_id` leaves `0x3ff`, GEM/T-CONT appear (`omcc_valid`, `data_valid`).
- `pon_state`: O2-3 -> O4 -> O5; `omci` netdev comes UP with nonzero counters.
- `omci_transport` error moves off -67.
- br-lan remains up (no shared-clock regression).
