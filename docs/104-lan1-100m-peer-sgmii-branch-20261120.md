# LAN1 接 100M 对端：给 XPCS 补一条真正的 SGMII 100M 分支

日期：2026-11-20  
平台：SK-D840N / ZX279133 / Linux 6.18.60

## 结论

LAN1（外置 RTL8226）接 100M 对端时，铜口已经协商成 100M（`PHYSR=0x1602`，link up/full），但旧驱动把 100M 和 1G 合并成同一条“非 2.5G”分支：XPCS 最后总是写回 U-Boot 的 1G SGMII 四字 `0x1140 / 0x0020 / 0x2200 / 0x0005`，XMAC0 也保持 1G SGMII 速率码。SGMII 的 1.25 Gbaud lane 因此承载的是 1G 码流，100M 对端没有合法码可收，`rx_packets` 恒为 0。

本次修复把 100M 作为独立速率档：

- `zx279133_switch_outerphy_apply_speed_locked()` 新增 `SPEED_100` 分支；
- XMAC0 仍用 `XMAC_SPEED_1G`（=3），与厂商 `xmac_sgmii_conf()` 一致；
- lane 仍走 SGMII/1.25 Gbaud，不切 2.5G profile；
- 最后写回的四字改为 `0x3100 / 0x0020 / 0x2200 / 0x0005`：BMCR 的 bit13（100M）置位、bit6（1G）清零，AN 与 full duplex 保留，其余三字沿用 loader 的 SGMII 值；
- lane watcher 跟踪 PHYSR 协商结果，100M↔1G/2.5G 变化时重新编程，不再整轮只按开机时的档位跑；
- 2.5G profile（1003 补丁）的路径和值未改。

## 代码依据

厂商模块 `np_133.ko` 反汇编（2026-11-20）：

- `xmac_init_by_work_mode` 的 SGMII 模式调用 `xmac_sgmii_conf(port, 1, 2, 1)`；
- `xmac_sgmii_conf` 随后调用 `xamc_init_conf_by_speed(port, 3)`，即 XMAC 固定 1G SGMII 码；
- `xpcs_sgmii_mode_conf(port, speed=1, ...)` 把 speed=1 交给 `xpcs_set_sr_mii_ctrl_speed()`，得到 `0x2000`（BMCR bit13），不是 1G 的 `0x20`（bit5/6 档）；
- 厂商 SGMII 路径不改写 SR-MII speed 字 `0x0004`、DIG_CTRL1、AN_CTRL，所以这三字保持 U-Boot 的 SGMII 值。

## 验证

新固件启动后，100M 对端插 LAN1：

1. `cat /sys/class/net/lan1/statistics/rx_packets` 间隔 3 秒两次读数应持续增长；
2. `dmesg | grep -i '100M SGMII'` 应看到 lane programmed 信息；
3. `dmesg | grep -i 'lane carries no code'` 不应再持续出现；
4. LAN1 能 DHCP 拿到 IP，`arp -n` 对端 REACHABLE；
5. 回归：2.5G 对端仍能收到帧；LAN2/3/4 不受影响。

## 影响范围

只改 kernel-overlays 里的 `zx279133-switch.c` 和一个端口状态测试；rootfs、DTB、网络配置、PON 修复均未改动。
