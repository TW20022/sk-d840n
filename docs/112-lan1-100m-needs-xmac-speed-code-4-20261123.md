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

## 与 docs/111 的关系

111 修的是"ready 被日志读掉"，那一步让 SOPC 发布成功、`never became ready`
消失，但数据面仍然零帧。两个 bug 独立：**111 决定端口能不能被发布，112 决定
发布之后帧能不能过去**。
