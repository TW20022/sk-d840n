# LAN1 / 2.5G jack：SOPC ready 是读即清，诊断读把握手吃掉（2026-11-23）

## 一句话

`nppt+0x342a4`（XMAC0 的 SOPC ready）是**读即清**的锁存位。修复前驱动在复位脉冲后立刻
`dev_info(... ready=0x%08x, read(0x342a4))` 打印它，那一次读就把脉冲换来的 clear-over-ready
吃掉了，于是随后的 `sopc_publish()` 十次全读 0，永远走 "never became ready" 分支。
commit `dc36e86` 删掉该诊断读后，mac 0 的 ready 握手成立、send-enable 发布。

但这**不是** LAN1 100M 不通的全部原因：publish 成功后数据面仍然双向为零（见下）。

## 1. 决定性实测（同一块板，TTL 控制台，`rdreg`/`wrreg` 直写 NPPT）

NPPT 基址 `0x19000000`（`nppt+0x140000 = 0x19140000`）。寄存器工具是设备上的
`rdreg <0xaddr>` / `wrreg <0xaddr> <0xval>`；debugfs 的 `zx279133-9132` 现在是**单文件**
（SPA 属性表，动词 `r/w/m/mw`），不碰 NPPT。

```
wrreg 0x192c0004 0xfffffbff   # 清 bit10 = XMAC0 复位
wrreg 0x192c0004 0xffffffff   # 置回
# 之后完全不读 0x190342a4
sleep 1
rdreg 0x190342a4   ->  0x00000001     # 1 秒后仍在
rdreg 0x190342a4   ->  0x00000000     # 再读就没了
```

结论：ready 不会自己落回，**只会被读清**。厂商 `nppt_smac_init()` 的十次轮询之所以有效，
是因为它的第一次成功读就发生在 publish 之前，且没有别的读插进来。

## 2. 修复前后（同一块板，PC 100M 直连 lan1）

修复前（run 16 / `31494d7` 内核）：

```
[ 9.607094] the 2.5G jack: XMAC0 reset pulsed for the 100M peer (ready=0x00000001)
[ 9.655519] SOPC mac 0 never became ready (0x00000000); not publishing it
[ 9.699621] SOPC mac 1 never became ready (0x00000000); not publishing it
```

修复后（run 17 / `dc36e86` 内核，uImage sha256 `94698c94…`）：

```
[ 9.376764] the 2.5G jack: XMAC0 reset pulsed for the 100M peer (reset word 0xffffffff)
[ 9.429600] SOPC mac 1 never became ready (0x00000000); not publishing it
[ 9.429620] SOPC send-enable published for 2 MAC(s)
```

mac 0 **不再报** never ready —— 握手成立。

## 3. 但数据面仍然死

同一时刻 lane 层是健康的：

```
[ 7.337821] the 2.5G jack's PHY: SerDes mode (devad 0x1e reg 0x7580) = 0x9402
[ 7.337913] the 2.5G jack's PHY: SerDes ctrl (devad 0x1e reg 0x758d) = 0x801e
[11.534865] the 2.5G jack's PHY: the lane's word is 0x801e and its mode word is 0x9402 - the lane carries code
[ 9.429684] the 2.5G jack: followed a 100 Mbps link
```

端口映射也确认无误：`ip -d link show lan1` → `portname p8`（DSA port 8 = outerphy port 5），
`ethtool lan1` → `Supported link modes: 2500baseT/Full`。

双向实测（`lan1 carrier=1 speed=100`）：

| 方向 | 观测 |
|---|---|
| 设备 → PC | 设备 `tx_packets=28`，PC `ReceivedBroadcastPackets=0`（一个 ARP 都没收到） |
| PC → 设备 | PC `SentBroadcastPackets=998`（Windows 一直在发 DHCP/ARP），设备 `rx_packets=0` |

也就是说 **TX 也没出去**，不是单纯接收问题。手工重放（关 MAC → 脉冲 → 重编程 → publish →
使能）同样双向为零，与历史 §1.3 的"运行期脉冲会 tear down 已 armed 的 MAC"一致。

## 4. 当前判断

1. ready/publish 这一层是真 bug，已修、已验证（mac 0 发布成功）。
2. 剩下的断点在更外面：100M 时 XPCS/PHY 的 SGMII 编排，或者 9132 端口 5 的 egress 队列。
   PPU5 的 SPA 属性表已与厂商值逐字一致（`a0=a6a5 a10=0fa0000e a13=8008 a17=05400410
   a18=01400940 a19=96040001`），说明端口配置不是卡点。
3. 历史证据（docs/92 §96.44）里 LAN1 通流用的是 **1G 对端**；本机 PC 是 Realtek PCIe FE
   **100M** 网卡。100M 分支是这套驱动里最脆弱的一段。

## 5. 下一步（按性价比排序）

1. **换 1G 对端验 LAN1**：若 1G 通、100M 不通，则问题被限定在 100M 分支（XPCS BMCR /
   SGMII code / RTL8226 100M 模式），不必再碰 publish 与端口表。
2. 100M 分支逐项对照：把 `BMCR=0x1140` 与厂商 100M 路径的实际值、以及 XMAC `TX_CTRL`
   的 speed 字段（当前 3 = 1G SGMII code）在"链路已 up"状态下做一次成对采样。
3. 若继续走软件路线，需要一个能在**链路 up 之后**重放 XPCS 四字 + XMAC0 program + publish
   的运行期入口（现在是 probe/建链路径里做，手工重放不可靠）。

## 6. 复现与回滚

- 刷机：串口传 kernel（`pon_revive/serial_push.py`，~10.9 KB/s，25 分钟）+
  `mtd write /tmp/sk-kernel.bin kernel`，rootfs 与配置不动；回读 sha256 校验。
- 回滚：`/root/mtd2-backup-3acf2c.bin`（8 MB，sha256 `e4970de0…`），
  `mtd write /root/mtd2-backup-3acf2c.bin kernel` 后重启。
- 配置：本次刷后重写了三码（SN `CMDCA133E348`、RegID hex）与 `network.wan` 的 PPPoE
  （`eth0.99` / `18852308006` / `disabled=1` / `wan_vlan99` vid 99），只 commit 不重启网络。
