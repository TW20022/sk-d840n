#!/bin/sh
# SK-D840N USB WiFi (RTL8188ETV, USB 0bda:0179) STA 客户端配置
# 适用于已按 USB 修复重编、驱动/固件已编译进固件的镜像。
#
# 用法（设备 SSH 上，root）：
#   WIFI_SSID="你的WiFi名" WIFI_KEY="你的密码" sh wifi-sta-setup.sh
#
# 前置：
#   - 编译期已选 kmod-rtl8xxxu + rtl8188eu-firmware + wpad + iw（见 configs/usb-wifi.config）
#   - 网卡插在"能枚举"的那个口：标准镜像是 15010000 口；若该口报 error -71，
#     换插另一物理口（15008000，本修复已补驱动）。哪个口有 wlan0 用哪个。
#   - 不需要运行时 apk/opkg 装驱动：本固件的软件源没有这些 kmod，装了也是错 ABI。

set -e
SSID="${WIFI_SSID:-}"
KEY="${WIFI_KEY:-}"

echo "======================================================"
echo " RTL8188ETV STA 配置  内核 $(uname -r)"
echo "======================================================"
[ -z "$SSID" ] && { echo "ERROR: 先设置 WIFI_SSID / WIFI_KEY 环境变量"; exit 1; }

# ---- 1. 驱动是否已在内核 ----
echo "[1/6] 检查 rtl8xxxu 是否已编译进内核..."
if grep -q '^rtl8xxxu' /proc/modules 2>/dev/null || [ -d /sys/module/rtl8xxxu ]; then
  echo "  rtl8xxxu 已加载"
else
  modprobe rtl8xxxu 2>/dev/null && echo "  rtl8xxxu modprobe 成功" \
    || echo "  提示: 未见 rtl8xxxu 模块；确认编译期已选 CONFIG_PACKAGE_kmod-rtl8xxxu"
fi
echo "  固件: $(ls /lib/firmware/rtlwifi/rtl8188eufw.bin 2>/dev/null || echo '缺失 /lib/firmware/rtlwifi/rtl8188eufw.bin')"

# ---- 2. 枚举 ----
echo "[2/6] 等待 wlan 接口..."
WLAN=""
for i in $(seq 1 15); do
  WLAN=$(ls /sys/class/net/ 2>/dev/null | grep -m1 '^wlan' || true)
  [ -n "$WLAN" ] && break
  sleep 1
done
if [ -z "$WLAN" ]; then
  echo "  !! 没有 wlan 接口。"
  echo "  USB 口状态: $(ls /sys/bus/platform/devices/ | grep -i usb | tr '\n' ' ')"
  echo "  dmesg 关键行:"
  dmesg | grep -iE 'xhci|rtl8xxxu|0bda|error -71' | tail -15
  echo "  处理: 换插另一 USB 口重来；或确认两个 xHCI 控制器都在（应见 15008000.usb 与 15010000.usb）。"
  exit 1
fi
echo "  wlan 接口: $WLAN"

# ---- 3. 生成无线配置骨架 ----
echo "[3/6] 准备 /etc/config/wireless ..."
[ -s /etc/config/wireless ] || wifi config 2>/dev/null || true
DEV=$(uci show wireless 2>/dev/null | grep -m1 '=wifi-device' | sed 's/^wireless\.//; s/\..*//')
[ -z "$DEV" ] && {
  uci add wireless wifi-device >/dev/null
  DEV=$(uci show wireless 2>/dev/null | grep '=wifi-device' | tail -1 | sed 's/^wireless\.//; s/\..*//')
}
echo "  radio: $DEV"

# ---- 4. 配置 STA ----
echo "[4/6] 写入 STA 配置..."
IFACE=$(uci show wireless 2>/dev/null | grep '=wifi-iface' | grep "^wireless\.$DEV\." | grep -m1 -v '@' | sed 's/^wireless\.//; s/\..*//')
[ -z "$IFACE" ] && {
  uci add wireless wifi-iface >/dev/null
  IFACE=$(uci show wireless 2>/dev/null | grep '=wifi-iface' | tail -1 | sed 's/^wireless\.//; s/\..*//')
  uci set "wireless.$IFACE.device=$DEV"
}
uci -q delete "wireless.$IFACE.network"
uci -q delete "wireless.$IFACE.ssid"
uci -q delete "wireless.$IFACE.key"
uci set "wireless.$IFACE.device=$DEV"
uci set "wireless.$IFACE.network=wwan"
uci set "wireless.$IFACE.mode=sta"
uci set "wireless.$IFACE.ssid=$SSID"
uci set "wireless.$IFACE.encryption=psk2"
uci set "wireless.$IFACE.key=$KEY"
uci set "wireless.$DEV.disabled=0"
uci set "wireless.$DEV.band=2g"
uci set "wireless.$DEV.channel=auto"
uci -q delete network.wwan
uci set network.wwan=interface
uci set network.wwan.proto=dhcp
uci commit wireless
uci commit network

# ---- 5. 重启无线 ----
echo "[5/6] 重启无线..."
wifi down 2>/dev/null || true
wifi up 2>/dev/null || wifi reload 2>/dev/null || true
sleep 10

# ---- 6. 验证 ----
echo "[6/6] 验证..."
echo "  --- 链路 ---"
iw dev "$WLAN" link 2>/dev/null || echo "  未关联（检查 SSID/密码/信号）"
echo "  --- wwan 地址 ---"
ip -4 addr show wwan 2>/dev/null | grep inet || echo "  wwan 未拿到 IP"
echo "  --- 外网 ---"
ping -c 2 -W 2 223.5.5.5 2>/dev/null && echo "  可达" || echo "  暂不通"

echo ""
echo "完成。排查: iw dev $WLAN station dump / logread | grep -i wpa / ifstatus wwan"
