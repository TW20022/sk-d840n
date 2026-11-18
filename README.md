# SK-D840N 固件修复说明

本仓库是 woodnaonly/zx279133-ponwrt-build 的 SK-D840N 修复分支：主线 ImmortalWrt 25.12.1 / 内核 6.18.60。
共修复 6 项硬件与配置问题（A–F），并在编译流程中固化了 2 处编译兼容修复。所有修复已合入源码树，按本仓库的 yml 工作流重新编译即可复现。

## 一、修复了哪些内容

| # | 问题 | 根因 | 修复落点 | 所在镜像 / 分区 |
|---|---|---|---|---|
| A | LAN2/3/4 百兆口链路 up 但 rx 恒 0、拿不到 IP | SMAC 的 `an_ctrl` 速率码停留在 1G 默认，未按 PHY 协商结果（100M）更新 | `zx279133-switch.c`：phylink 回调里按实际速率写速率字 | flash-uImage / **kernel** |
| B | LAN1 2.5G 口 speed=2500 但 rx 恒 0 | XPCS 的 VEND2 四字残留开机 1G SGMII 值，PCS 仍按 1G 发码 | `zx279133-switch.c`：2.5G 分支写入正确的四字（BMCR/DIG_CTRL1/AN_CTRL） | flash-uImage / **kernel** |
| C | USB 第二个物理口完全无反应 | 主线 DTS 只有 `usb@15010000`，缺 `usb@15008000` 节点 | 板级 DTS 增加第二个 xHCI 控制器节点 | flash-dtb.bin / **dtb** |
| D | RTL8188ETV WiFi 插上无 wlan0 | 镜像未编入 rtl8xxxu 驱动、8188 固件与无线栈 | base config 注入 5 行 `CONFIG_PACKAGE_*`，编译期做进镜像 | flash-rootfs.jffs2 / **root**（内核符号在 kernel） |
| E | `reboot` 挂死，必须物理断电 | 板级 DTS 把 watchdog0/1 都 override 成 `disabled`，失去硬件复位源 | 把 `&watchdog0`（14f09000，带 restart 属性）改回 `okay` | flash-dtb.bin / **dtb** |
| F | 开机偶发 `br-lan` 只剩 `lan-cpu0`，用户口被踢出 | DSA conduit 口 `lan-cpu0` 被当普通 LAN 成员入桥 | `sk-bridge` 与 `02_network` 两处排除 lan-cpu0 | flash-rootfs.jffs2 / **root** |

编译兼容修复（保证 CI 能编出含上述修复的固件，无需手工干预）：

- `patches/compat/422-sk-d840n-6.18-ieee80211-ptr.patch`：本定制内核缺 OpenWrt 的 CFG80211_HEADERS 补丁，backports 需要把 `net_device->ieee80211_ptr` 字段在 `CONFIG_WIRELESS` 下暴露，否则 mac80211 编译失败。工作流已幂等应用。
- `patches/compat/421-sk-d840n-6.18-sockwq-enum.patch`：backports 在 6.18 上错误重定义 `SOCKWQ_ASYNC_*` 枚举，已作为标准 build patch 随 `overlays/` 自动应用。

> 相关补丁与成品源码见 `patches/`（`handover/`、`compat/`、`1001-*.patch`），幂等修复脚本见 `scripts/`（`apply_100m_fix.py`、`ef_fix.py`、`dtb_check.py`）。

## 二、怎么完整刷机

**关键：A–F 的修复分落三个分区，必须完整更新 root + dtb + kernel。只刷 kernel 不会修复 C/E/F（以及 D 的用户态部分）。**

刷机前先备份现有配置（刷 root 会覆盖 `/overlay` 中的配置）。推荐用仓库自带脚本：kexec 进入 RAM 系统后，先 `sh templates/flash-sk-d840n.sh` 干跑核对，再 `sh templates/flash-sk-d840n.sh --write`，脚本按 **root → dtb → kernel** 顺序逐分区校验，kernel 最后刷，便于出错时恢复。

手动 U-Boot 方式（在 U-Boot 命令行逐条执行，三个镜像放到 tftp 目录）：

```sh
# 1) rootfs —— 含 F（lan-cpu0 修复）+ D 的 WiFi 驱动/固件
tftpboot 0x88000000 flash-rootfs.jffs2
mtd erase root
mtd write root 0x88000000 0 0x720004

# 2) dtb —— 含 E（watchdog0=okay）+ C（usb@15008000 第二 USB 口）
tftpboot 0x88000000 flash-dtb.bin
mtd erase dtb
mtd write dtb 0x88000000 0 0x3623

# 3) kernel —— 含 A/B 驱动修复 + D 的无线内核栈（最后刷）
tftpboot 0x88000000 flash-uImage
mtd erase kernel
mtd write kernel 0x88000000 0 0x6edcf6
boot
```

长度以 `tftpboot` 输出里的 `Bytes transferred` 为准换算：flash-rootfs.jffs2 7,471,108 = `0x720004`；flash-dtb.bin 13,859 = `0x3623`；flash-uImage 7,265,526 = `0x6edcf6`。

最终镜像（SHA256）：

| 文件 | 大小(字节) | SHA256 |
|---|---:|---|
| flash-uImage | 7,265,526 | `6bd5b61d107687a1ed032574fe97ba773a881a069bb18fb5337f67310fcb10a8` |
| flash-dtb.bin | 13,859 | `34e624988edc5330f61c4e4f212aaf129929f9670ec53aa5dd75f1b008d50c6c` |
| flash-rootfs.jffs2 | 7,471,108 | `b3b20ee425cffa96b89baeaf9482d87bf3f212f13925ae2fe4ecfda67197e687` |
| flash-sysupgrade.bin | 14,756,117 | `cc4a467703e50c79b3cf20d6af283be2d06dddcceb9a9c0aa165abf94ed417d9` |

刷机后验证（要点）：

- A：`dmesg | grep 'rate word'`、`cat /sys/class/net/lan3/speed` 应为 100，rx 计数持续增长。
- B：`cat /sys/class/net/lan1/speed` 应为 2500，`dmesg | grep 2500BASE-X`，rx 增长。
- C：`ls /sys/bus/platform/devices/ | grep -iE 'usb|xhci'` 应同时出现 `15008000` 与 `15010000`；两个物理口分别插 U 盘确认。
- D：`sh wifi-sta-setup.sh "<SSID>" "<密码>"` 后 `ip link show wlan0` 为 UP、`iw dev wlan0 link` 已连接。
- E：`sync; reboot` 后 120s 内串口应重新出现 U-Boot banner，不再挂死等断电。
- F：`brctl show` 的 `br-lan` 成员应含 lan0~lan4 且无 lan-cpu0；多次 reboot / 冷启动均不复现。
