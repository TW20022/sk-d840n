#!/bin/sh
# pon_rxwatch.sh - one-second timeline of the SK PON receive path.
#
# Why this exists: the 2026-11-26 light-present boot proved 0x16100=0x63b and
# 0x16105=0x02, but left no PLOAM counter or rx_data sample behind.  The only
# surviving capture belongs to a different, dark boot and cannot settle the
# question.  This script puts MAC receive data, downstream status and the
# PLOAM counters on one timeline so the next fibre insertion does settle it.
#
# Usage:
#   sh /usr/sbin/pon_rxwatch.sh 1800        # 30 minutes (boot-chain default)
#   OUT=/root/custom.log sh /usr/sbin/pon_rxwatch.sh 0   # until killed
#
# Reading a register: pon_reg's store overwrites one global reply buffer, so
# two writers race.  A mkdir lock keeps this reader exclusive; keep the read
# immediately after the write (no sleep between them).
#
# Interpretation guide:
#   r48 (0x16148) changing but pl rx=0   -> pending/12-word PLOAM extraction
#   r48=0 while r05=00000002 (bit4 clear) -> MAC RX enable / PSync alignment
#   both live, rx climbing, still O2-3    -> OLT grant / SN acceptance

PON=/sys/class/net/eth0
OUT=${OUT:-/root/rxwatch.log}
LOCK=/tmp/pon_rxwatch.lock
DUR=${1:-1800}

[ -e "$PON/pon_reg" ] || exit 0
mkdir "$LOCK" 2>/dev/null || exit 0
trap 'rmdir "$LOCK" 2>/dev/null' 0
trap 'exit 0' 1 2 15

read_reg() {
	echo "r $1" > "$PON/pon_reg" 2>/dev/null
	cat "$PON/pon_reg" 2>/dev/null | tail -1 | awk '{print $3}'
}

UP=$(cut -d. -f1 /proc/uptime)
echo "=== rxwatch start $(date +%H:%M:%S) up=${UP}s dur=${DUR}s kernel=$(uname -r) regs=16148/16105/16100/16642/16504 ===" >> "$OUT"

i=0
while :; do
	i=$((i + 1))
	[ "$DUR" -gt 0 ] 2>/dev/null && [ "$i" -gt "$DUR" ] && break

	R48=$(read_reg 16148)
	R05=$(read_reg 16105)
	R00=$(read_reg 16100)
	KICK=$(read_reg 16642)
	PND=$(read_reg 16504)

	PS=$(cat "$PON/pon_state" 2>/dev/null)
	BEN=$(cat "$PON/pon_bench" 2>/dev/null)
	LOS=$(cat /sys/class/leds/skyworth:red:los/brightness 2>/dev/null)
	HW=$(cat "$PON/pon_hw" 2>/dev/null)
	PL=$(head -1 "$PON/pon_ploam" 2>/dev/null)
	PRO=$(head -1 "$PON/pon_profile" 2>/dev/null)

	echo "$(date +%H:%M:%S) up=$(cut -d. -f1 /proc/uptime)s ps=$PS ben=$BEN losled=$LOS r48=$R48 r05=$R05 r00=$R00 kick=$KICK pnd=$PND pl=[$PL] pro=[$PRO] hw=[$HW]" >> "$OUT"

	# Bound the file: a full hour is ~3600 lines, keep the last 2000.
	if [ $((i % 60)) -eq 0 ]; then
		N=$(wc -l < "$OUT")
		if [ "$N" -gt 4000 ]; then
			tail -n 2000 "$OUT" > "$OUT.rot" 2>/dev/null && mv "$OUT.rot" "$OUT"
		fi
	fi

	# Every pon_reg read is a pr_emerg, so this watch alone pushes the boot
	# bring-up out of the dmesg ring within a few minutes.  Keep one copy of
	# what the kernel said before that happens (the tick starts 12 s in, so
	# ~2 min later still covers the 48-105 s bring-up).
	if [ "$i" -eq 120 ]; then
		dmesg > /root/pon_dmesg_boot.log 2>/dev/null
	fi
	sleep 1
done

echo "=== rxwatch end $(date +%H:%M:%S) samples=$i ===" >> "$OUT"
exit 0
