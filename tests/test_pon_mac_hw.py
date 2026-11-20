#!/usr/bin/env python3
"""Guard the PON MAC hardware layer added in docs/107.

This is a source-level contract: the reverse-engineered register indices,
the non-destructive queue API and the new read-only sysfs view must stay in
place.  Runtime success still needs a live OLT and is checked on the board.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / "kernel-overlays/drivers/net/ethernet/zte"

hdr = (KERNEL / "zxpon.h").read_text(encoding="utf-8")
proto = (KERNEL / "zxpon.c").read_text(encoding="utf-8")
glue = (KERNEL / "zx279133-pon.c").read_text(encoding="utf-8")

for name in ("zxpon_peek_ploam", "zxpon_retry_sn", "zxpon_alloc_id"):
    assert name in hdr, name
    assert ("int " + name) in proto or ("void " + name) in proto, name

for token in ("ZX_PON_RX_PENDING", "ZX_PON_RX_DATA",
              "ZX_PON_TX_KICK", "ZX_PON_TX_DATA"):
    assert token in glue, token

assert "static void zx_pon_hw_bringup(void)" in glue
assert "static int zx_pon_hw_ploam_rx(" in glue
assert "static int zx_pon_hw_ploam_tx(" in glue
assert "zxpon_peek_ploam(&zx_state, frame)" in glue
assert "zxpon_take_ploam(&zx_state, frame)" in glue
assert "dev_attr_pon_hw" in glue
assert "mapped=%u" in glue

# The queue must not be dropped on a failed MAC write.
peek_at = glue.index("zxpon_peek_ploam(&zx_state, frame)")
take_at = glue.index("zxpon_take_ploam(&zx_state, frame)")
assert peek_at < take_at, "TX queue is dequeued before the MAC accepts the frame"

assert (ROOT / "docs/107-pon-mac-hardware-layer-port-20261121.md").is_file()
print("PASS: PON MAC hardware layer and PLOAM queue contract")
