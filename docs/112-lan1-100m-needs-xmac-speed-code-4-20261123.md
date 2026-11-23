# LAN1 接 100M 对端：XMAC 的速率码必须写 4，不是 3

日期：2026-11-23
平台：SK-D840N / ZX279133 / Linux 6.18.60
前置：docs/111（SOPC ready 是读即清，已修）

## 结论

100M 对端的真正卡点是 **XMAC0 速率码**，不是 XPCS 的四个字，也不是 SOPC 发布。

`TX_CTRL`（`nppt+0x140000`）的 bit31:29 是 MAC 的速率选择，厂商
`xmac_set_speed_sel()` 的表是：

| 值 | 速率 |
|---|---|
| 0 | USXGMII（10G 镜像） |
| 3 | 1G SGMII |
| 4 | 100M |
| 6 | 2.5G |
| 7 | 10M |

驱动在 100M 分支里写的是 **3**（依据是厂商 `xmac_sgmii_conf()` 传 3 给
`xamc_init_conf_by_speed()` 的旧解读），于是 MAC 通过 SGMII 码字告诉 RTL8226
"对端是 1G"，而铜口协商的是 100M。PHY 按 100M 做速率适配（每个字节重复十次），
MAC 按 1G 收发 —— 两个方向都收不到合法帧。

表现出来就是：链路 up、`carrier=1`、`speed=100`、`lane carries code`、
SOPC 已发布，**但双向零帧**。

## 决定性测量（2026-11-23，板上运行期）

速率码是 `TX_CTRL` 的 bit31:29，可以直接用 `wrreg` 在运行期改，所以不必重编内核
就能把八个码全扫一遍。脚本：`pon_revive/push/scan_code2.sh`，每档写入后 ping
对端三次。

```
code=3 txctrl=0x60010001 replies=0 rx=0     ← 驱动当时的值
code=0 txctrl=0x00010001 replies=0 rx=0
code=1 txctrl=0x20010001 replies=0 rx=0
code=2 txctrl=0x40010001 replies=0 rx=0
code=4 txctrl=0x80010001 replies=3 rx=5     ← 通
code=5 txctrl=0xa0010001 replies=0 rx=5
code=7 txctrl=0xe0010001 replies=3 rx=9
code=6 txctrl=0xc0010001 replies=0 rx=9
```

码 4 一写下去，`ping` 立刻 4/4 回包（0.5 ms），`lan1 rx_packets` 开始增长；
保持码 4 不动，PC 通过 lan1 的 DHCP 拿到 `192.168.9.150/24`。

## 代码改动

`kernel-overlays/drivers/net/ethernet/zte/zx279133-switch.c`：

- 新增 `ZX279133_XMAC_SPEED_100 = 4`（并补 `_10 = 7`，与厂商表对齐）；
- `zx279133_switch_outerphy_apply_speed_locked()` 的 `SPEED_100` 分支改为
  `speed_code = ZX279133_XMAC_SPEED_100`；
- 100M 仍然需要 XMAC0 复位脉冲（docs/111 的 ready 握手）+ publish，保持不变。

`tests/sk_lan1_power_harness.h` 的断言同步改为 100M 档期望码 4。

## 验证顺序

1. 启动后 `dmesg | grep '100M SGMII'` 应显示 `the XMAC takes the 100M code (4)`；
2. `rdreg 0x19140000` 高三位应为 `100`（`0x8001xxxx`）；
3. `cat /sys/class/net/lan1/statistics/rx_packets` 持续增长；
4. PC 通过 lan1 DHCP 拿到 192.168.9.150，`ping 192.168.9.1` 通；
5. 回归：1G 对端仍走码 3，2.5G 仍走码 6，LAN2/3/4 不受影响。

## 启动态验证（2026-11-23 12:23，刷入后重启）

```
[    9.365] the 2.5G jack's PHY: system side at 100M SGMII; the XMAC takes the 100M code (4)
[    9.418] the 2.5G jack: lane programmed for 100M SGMII (BMCR 0x1140)
[    9.418] the 2.5G jack: lane programmed for 100M SGMII (XMAC speed 4)

rdreg 0x19140000  ->  0x80010001          # bit31:29 = 4
carrier / speed   ->  1 / 100
```

结果：

| 项 | 实测 |
|---|---|
| `ping 192.168.9.150`（PC 主动） | 4/4，0.57 ms |
| `ping`（设备主动） | 5/5，`lan1 rx` 持续增长 |
| 1448 字节大包 | 10/10，0% 丢包 |
| TCP 下载 7.55 MB（PC→设备） | 0.74 s ≈ **82 Mbps 载荷**（约 87% 线速） |
| DHCP | `DHCPREQUEST(br-lan) 192.168.9.150` / `DHCPACK … CQ45`，租约落盘 |
| lan1 down/up 翻转 | 重新 up 100M，速率码保持 4，ping 继续通 |

`lan1 down/up` 之后没有第二次复位脉冲（`applied_outer_speed` 未变，速率跟随逻辑去重），
所以"再次脉冲把已 armed 的 MAC 拆掉"那条老坑在这条路径上不会被触发；只有 PCS 被
外部复位、lane watcher 走恢复分支时才会重新脉冲。

## 正式产物刷入验证（2026-11-23 13:35，CI run 19）

前面几轮验证用的是本地编译内核（与 CI 同源、未压缩 Image 逐字节同尺寸）。最后
把 CI run 19（`662c66c`，含 `da2cfb7` 速率码 + `662c66c` 日志）的正式产物刷进去
再验一遍，留档。

- 产物：`skd840n_flash_19_45110801012`，`flash-uImage` 7267774 B，
  sha256 `6b0ed3080caf452105b321d1edca294982fa7c6600f6561aaebaa33d28d2ade3`；
  `flash-dtb.bin` 13886 B，`e68949b7…`。
- 刷法：PC 起 `http.server`，设备 `wget` 到 `/tmp` 后 **sha256 比对一致**，
  `mtd write … kernel`，回读前 7267774 B 再比一次，两次都对。
- 启动态（dmesg）：

```
the 2.5G jack: XMAC0 reset pulsed for the 100M peer (reset word 0xffffffff)
SOPC mac 1 never became ready (0x00000000); not publishing it
the 2.5G jack: lane programmed for 100M SGMII (BMCR 0x1140)
the 2.5G jack: lane programmed for 100M SGMII (XMAC speed 4)
```

  mac 0 的 `never became ready` 不再出现（只剩从来不用通道的 mac 1）；
  `rdreg 0x19140000` = `0x80010001`；carrier=1、speed=100。
- 数据面：ICMP 5/5 零丢（RTT 0.94 ms）、1400 B 大包 5/5、TCP 7.27 MB / 0.83 s
  ≈ 70 Mbps、`lan1 rx 5246 / tx 2717` 持续增长；PC 侧 `PrefixOrigin=Dhcp`
  持 192.168.9.150。
- 配置持久化（重启后读）：`pon.pon.serial_number=CMDCA133E348`、
  `network.wan.proto=pppoe / device=eth0.99 / disabled=1`；`pppd` 未启动、
  `pon_state=O1`（无光，符合预期）。

## 一个容易再犯的坑

`lane *_snapshot` 诊断里那段 `SOPC mac %d ready=0x%08x` 也是**消耗性读**
（`nppt+0x342a4 + 4*i`）。当前调用顺序是"快照在前、脉冲在后、publish 在最后"，
所以不影响；但任何把快照插到「脉冲」与 `sopc_publish()` 之间的改动，都会
原样重现 docs/111 那个 bug —— ready 被打印读掉，publish 读到 0。改这段路径前
先看这条。

## 留给下一次的两件事

1. **10M 对端未验证**。厂商表里 7 = 10M，但运行期扫描时码 7 在 100M 对端上也能通，
   说明该字段的编码还没完全弄清；`SPEED_10` 目前仍落进 "其它" 分支按 1G 处理，
   接 10M 对端大概率同样不通。真要支持再单独测。
2. **`XMAC0 reset pulsed` 与 `SOPC send-enable` 是 100M 的必要条件**（docs/111），
   本次没有改动这条握手，只在它之后补上了正确的速率码。

## 与 docs/111 的关系

111 修的是"ready 被日志读掉"，那一步让 SOPC 发布成功、`never became ready`
消失，但数据面仍然零帧。两个 bug 独立：**111 决定端口能不能被发布，112 决定
发布之后帧能不能过去**。
