#!/bin/sh
#
# SK-D840N · LAN 热修（内核补丁 1002 / 1003 的 shell 版）
#
# 作用：按每个口的实际链路速率，把 9132 的 SMAC 速率字写对；LAN1 是 2.5G 时
#       补写 XPCS 的四个 VEND2 字。断电后寄存器回落到厂商默认值（全部按 1G
#       采样），这个脚本负责把它们改回来。
#
# 用法：
#   lan-hotfix.sh once     立刻跑一遍，救活当前会话（100M 口马上能收包）
#   lan-hotfix.sh watch    常驻轮询（默认 5 秒一次），开机脚本用
#
# 环境变量：INTERVAL=秒（默认 5）
#
# 注意：这是权宜方案。它会和驱动抢这两个寄存器，长期正确做法仍是编译带
#       1002 + 1003 的内核并刷入（见 handover-4lan/README.md）。

D=/sys/kernel/debug/zx279133-9132
INTERVAL=${INTERVAL:-5}
RUN=/tmp/lan-hotfix

# 9132 的 DSA 口 4..7 对应 mac 0..3，an_ctrl 在 NPPT 偏移 0x40000*(mac+1)。
# netdev 名来自 DTB 的 label：lan0/lan2/lan3/lan4。
PORTS="lan0:0x040000 lan2:0x080000 lan3:0x0c0000 lan4:0x100000"

# an_ctrl 的不变位（厂商稳态 0x00fa2203 去掉 bit15:13 之后的部分）
BASE=0x00fa0203

[ -w "$D" ] || { echo "lan-hotfix: $D 不可写（debugfs 未挂载或缺驱动）" >&2; exit 1; }
mkdir -p "$RUN"

log() { logger -t lan-hotfix "$*" 2>/dev/null; }

# 一个口：读 sysfs 的 speed/duplex → 算 an_ctrl → 变了才写
apply_port() {
	dev=$1
	off=$2

	[ -e "/sys/class/net/$dev" ] || return 0

	sp=$(cat "/sys/class/net/$dev/speed" 2>/dev/null)
	case "$sp" in
	1000) code=0x2000 ;;
	100)  code=0xa000 ;;
	10)   code=0xe000 ;;
	*)    return 0 ;;	# 无链路 / 未知速率：不动
	esac

	dp=$(cat "/sys/class/net/$dev/duplex" 2>/dev/null)
	[ "$dp" = "half" ] && code=$((code & ~0x2000))

	want=$((BASE | code))
	mark=$(printf '%s:%s' "$sp" "$dp")

	if [ "$(cat "$RUN/$dev" 2>/dev/null)" = "$mark" ]; then
		return 0	# 没变化，不重复写
	fi

	printf 'regw %s 0x%08x\n' "$off" "$want" > "$D" 2>/dev/null || return 1
	echo "$mark" > "$RUN/$dev"
	log "$dev ($sp/$dp) an_ctrl -> $(printf '0x%08x' "$want")"
}

# LAN1（2.5G 口）：铜口是 2500 时，XPCS 的四个 VEND2 字必须是 2500BASE-X profile
apply_2p5g() {
	[ -e /sys/class/net/lan1 ] || return 0

	if [ "$(cat /sys/class/net/lan1/speed 2>/dev/null)" != "2500" ]; then
		rm -f "$RUN/2p5g"
		return 0
	fi

	[ -e "$RUN/2p5g" ] && return 0

	echo 'xpcsw 1 0x1f 0x0000 0x2140' > "$D" 2>/dev/null
	echo 'xpcsw 1 0x1f 0x0004 0x0020' > "$D" 2>/dev/null
	echo 'xpcsw 1 0x1f 0x8000 0x2000' > "$D" 2>/dev/null
	echo 'xpcsw 1 0x1f 0x8001 0x0000' > "$D" 2>/dev/null
	touch "$RUN/2p5g"
	log "lan1 2500BASE-X: XPCS VEND2 -> 2140/0020/2000/0000"
}

apply_all() {
	for pair in $PORTS; do
		apply_port "${pair%%:*}" "${pair##*:}"
	done
	apply_2p5g
}

case "${1:-watch}" in
once)
	apply_all
	;;
watch)
	while :; do
		apply_all
		sleep "$INTERVAL"
	done
	;;
*)
	echo "usage: $0 [once|watch]" >&2
	exit 1
	;;
esac
