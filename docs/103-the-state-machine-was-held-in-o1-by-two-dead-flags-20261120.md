# The PON state machine was pinned in O1 by two flags that never clear

Date: 2026-11-20

## Symptom

On the clean 52cb590 firmware, with Jiangsu credentials accepted by the
kernel (`kernel_identity_error=0`) and the fibre present (the same fibre
registers on the HX5), `pon_state` stays `O1` forever. `omci_transport`
reports `error=-67`, the `omci` netdev is DOWN with zero counters, and the
panel LOS lamp is red. SerDes `RX_STATUS` at `0x160000e4` reads
`0x01000706`: RX_LOS=0, CDR_LOCK=1 -- the SerDes sees a signal.

## Root cause (code-level, confirmed on the live kernel)

The activation gate in `zxpon.c`:

```c
static void zxpon_try_o23(struct zxpon *z)
{
        if (z->los || z->lods || !z->has_identity)
                return;
        ...
        z->state = ZXPON_O23;
        zxpon_queue_sn(z);
}
```

`zxpon_init()` sets `los = 1` and `lods = 1`. To leave O1 both must be
cleared. Reading the live `struct zxpon` out of `/dev/mem` (found by
searching kernel data for the registration-id bytes; struct base
`0xffff8000812510f8`) showed:

```
state=1 (O1)  has_identity=1  los=1  lods=1  onu_id=0x3ff
```

Both gates are stuck:

1. **`los`**: `zx279133-main.c:pon_led_work()` polls `opsd-gpio` every
   200 ms and calls `zx279133_pon_note_rxsd(rxsd > 0)`. That GPIO is
   `gpio2 pin 1` (`opsd-gpios = <&gpio2 1 GPIO_ACTIVE_HIGH>`), the BOSA
   optical RX signal detect. On a mainline boot the BOSA analogue front
   end is never initialised, so the line reads low forever. The worker
   therefore calls `note_rxsd(0)` -> `zxpon_set_los(z, 1)` on every poll.
   Writing `los=0` directly into the struct is overwritten within 200 ms
   (measured). `docs/92` already established that the vendor recovery
   image registers on this same fibre: the fibre/OLT/optics are fine, and
   this GPIO reports "our image has not brought the optical module up",
   not "the fibre is dark".

2. **`lods`**: `zxpon_set_lods()` has **no caller in the real driver path**
   (only in `tests/zxpon_test.c:arm()`). Initialised to 1, it stays 1 for
   the life of the driver. In the unit test, `arm()` explicitly does
   `set_lods(z, 0)` then `set_los(z, 0)` before expecting state O23. The
   production driver never performs those two steps.

So a correct identity can never move the machine past O1: this is an
initialisation defect in the PON glue driver, independent of the fibre,
the SerDes profile, the credentials and the OMCC version.

## Fix

In `zx279133-pon.c`:

- Add `zx_activation_armed`.
- In `zxpon_nl_set_identity()`, after a successful identity write, clear
  `lods` and `los` and set the flag. `zxpon_set_los(z, 0)` then runs
  `zxpon_try_o23()` and the serial number is queued.
- In `zx279133_pon_note_rxsd()`, once armed, ignore a low opsd so the
  200 ms poll can no longer kick the state machine back to O1. A high
  opsd is still honoured, and the LED keeps following the raw GPIO, so
  the panel lamp stays truthful.
- Reset the flag in `pon_attach()`/`pon_stop()`.

The standard O1 timers in `zxpon.c` are untouched: if no downstream
PLOAM ever arrives, the machine still times out back to O1. The fix lets
the PLOAM state machine *try*, which is all it was never allowed to do.

## Verification criteria

After flashing this build with credentials already configured at boot:

- dmesg/sysfs: `pon_state` moves `O1 -> O23` shortly after zxpond pushes
  the identity (look for the upstream serial-number PLOAM being sent).
- Live struct: `los=0 lods=0` remain stable across several 200 ms polls.
- If the OLT answers: `O23 -> O4 -> O5`, `onu_id` leaves `0x3ff`, and the
  `omci` netdev comes up with non-zero counters.
- br-lan must stay up (this change does not touch TOPCRM/global clocks).
