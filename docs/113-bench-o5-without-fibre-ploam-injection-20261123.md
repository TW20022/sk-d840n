# 没有光纤也能跑到 O5：注入下行 PLOAM 的台架演练

日期：2026-11-23
平台：SK-D840N / ZX279133 / Linux 6.18.60
相关：docs/107（PON MAC 硬件层）、docs/108（GEM/T-CONT 间接表）、docs/109（寄存器配方）

## 结论

1. **`omci_transport` 那个 `error=-67` 不是硬件故障**，是 PON 从来没到过 O5：
   `pon.pon.enabled` 是 `0`，zxpond 因此不下发身份，`has_identity=0`，状态机停在 O1，
   下行通道压根没建立。把 `enabled` 设成 1、重启 zxpond 之后，`pon_state` 立刻从 O1
   变成 `O2-3`，SN 按 1 秒重发（`pon_hw` 的 `tx` 从 0 涨起来，`txerr=0`）。
2. 新增一个**只用于台架的注入动词**，把 OLT 该发的下行 PLOAM 直接喂进协议栈。
   没有光纤、没有 OLT，就能把 O1 → O2/O3 → O4 → O5 整条路走完，并观察板子用
   真实硬件发送路径回应。
3. 走完之后 `omci_transport` 从 `ready=0 error=-67` 变成 **`ready=1 error=0`**；
   `eth0` 拿到 carrier（`LOWER_UP`），`pon_hw` 的 `data=1`、`alloc=0001 alloc_valid=1`，
   绿 LED 点亮。**OMCI 发送也通了**：`submitted 0→1 completed 1 error=0`。

也就是说，插纤之前，除"光真的出去、OLT 真的回应"以外的整条链路都已经验过一遍。

## 起因：`enabled=0`

```
$ uci show pon | head -2
pon.pon=pon
pon.pon.enabled='0'
```

`enabled=0` 时 zxpond 启动后什么都不做，于是：

| 观测 | 值 | 含义 |
|---|---|---|
| `pon_state` | `O1` | 停在初始态 |
| `pon_hw` | `tx=0 rx=0` | **SN 一帧都没发** |
| `omci_transport` | `ready=0 error=-67` | 通道未建立（-67 = ENOLINK） |

这里要留意一个容易误判的地方：`pon_hw` 的 `mapped=1` 只表示寄存器映射成功，
**不代表身份下发了**。真正判"身份到了没到"的是 `tx` 会不会在 O2-3 下增长。

```
$ uci set pon.pon.enabled='1'; uci commit pon; /etc/init.d/zxpond restart
$ cat /sys/class/net/eth0/pon_state
O2-3
$ cat /sys/class/net/eth0/pon_hw
mapped=1 ... rx=0 tx=4 txerr=0 ...
```

`tx=4` 是 SN 重发计数在走。发送侧是活的。

## 注入动词

`ZXPON_CMD_PLOAM_RX`（genl 命令 8，属性 9 = 48 字节帧），`GENL_ADMIN_PERM`：

- MIC 由内核用 `zxpon_ploam_seal()` 重算，所以调用方只要给 40 字节有效内容，
  不用自带 AES-CMAC（CMAC 本身由 `tests/zxpon_test.c` 覆盖）。
- 注入之后走的是**完全正常的代码路径**：`zxpon_rx_ploam()` → 状态机 →
  排队上行 → 100 ms tick 里的 `zx_pon_hw_ploam_tx()` 真写硬件。
- 首次使用时打一行 `pr_info`，提醒"现在的下行帧来自 genl，不是光纤"，避免
  以后有人在真插纤的环境里被这行日志误导。
- 没有光纤时 `z->los` 为什么不是 1：`zxpon_set_identity` 会 `set_lods(0)` +
  `set_los(0)`，并把 `zx_activation_armed` 置 1，之后 `note_rxsd(0)` 被忽略。
  这不是为注入开的特例，是既有行为。

工具在 `tools/zxponctl.c`（不依赖 libnl，直接组 NETLINK_GENERIC 报文）：

```
zxponctl family                      # 解析 family id
zxponctl ploam <96 hex chars>        # 注入一条下行 PLOAM
zxponctl omci  <96 hex chars>        # 发一条 OMCI（baseline 帧）
```

## 演练序列与结果

ONU-ID 分配 1，AllocID 1，GEM 1348（HX5 上实测的 OMCC GEM）。

| 步骤 | 注入（message id） | `pon_state` | `pon_hw` 变化 |
|---|---|---|---|
| — | — | O2-3 | tx=38 |
| 1 | `ASSIGN_ONUID` (0x03) | **O4** | tx=44 |
| 2 | `RANGING_TIME` (0x04) | **O5** | `data=1`，tx=45 |
| 3 | `ASSIGN_ALLOCID` (0x0a) | O5 | `alloc=0001 alloc_valid=1`，tx=46 |
| 4 | `KEY_CONTROL` (0x0d) | O5 | tx=47（生成密钥并回 KEY_REPORT）|
| 5 | `REQUEST_REG` (0x09) | O5 | tx=48（回 Registration）|

每一步 `tx` 都 +1，说明协议栈排队的上行帧**确实被写进了硬件发送环形**，
不是只在软件里记了个状态。

O5 之后：

```
$ cat /sys/class/net/eth0/omci_transport
implemented=1 ready=1 rx_ready=0 mic_ready=0 submitted=0 completed=0 error=0
$ ip -br link show eth0
eth0   UP   <BROADCAST,MULTICAST,UP,LOWER_UP>     # carrier 由 O5 打开
$ cat /sys/class/leds/*pon*/brightness
1                                                  # 绿 LED 点亮
```

## OMCI 发送验证

`zx279133_omci_send()` 不查 GEM 表，它直接把 GEM 号写进 IDM TX 描述符，门是
`eth->tx_prepared`。构造一条通过 `zxpon_omci_tx_cmd()` 校验的 baseline 帧
（`frame[3]=0x0a`、`frame[43]=0x28`）发一次：

```
$ zxponctl omci 0a28000a00...2800000000
$ echo $?
0
$ cat /sys/class/net/eth0/omci_transport
implemented=1 ready=1 ... submitted=1 completed=1 error=0
```

`completed=1` 表示帧真的进了 DMA 环形并拿到完成中断。**上行方向（PLOAM 与 OMCI、
带 GEM 号）是完整的**，缺的只剩"光有没有出去、OLT 有没有回"。

## 这次演练暴露的两个问题

1. **`rx_ready=0 mic_ready=0` 是写死的字面量**，不是真实状态：

   ```c
   sysfs_emit(buf, "implemented=1 ready=%u rx_ready=0 mic_ready=0 ...", ...)
   ```

   它跟 docs/111、docs/112 里那种"日志说谎"是同一类问题：一个恒为 0 的字段
   会让人以为接收侧永远是坏的。要么改成真实判据（GEM 表是否就绪），要么直接删掉。

2. **接收方向仍未验证，也没有配置**。发送通不代表接收通：进来的 GEM 帧要靠
   硬件 GEM/T-CONT 表才能 demux 到对的端口，而 `ZXPON_CMD_ADD_TCONT` 现在
   直接 `return -EOPNOTSUPP`（注释："No ZX279133 T-CONT table backend yet"）。
   这就是 docs/108 那个间接表问题，插纤后第一堵墙会在这里。

## 复现步骤

```sh
# 0. 板子上（zspon 已启用）
uci set pon.pon.enabled='1'; uci commit pon; /etc/init.d/zxpond restart
# 1. 把工具送上去（HTTP 已通时可秒级）
wget -q -O /tmp/zxponctl http://<pc>:8000/zxponctl && chmod +x /tmp/zxponctl
# 2. 依次注入，每步看 pon_state / pon_hw
/tmp/zxponctl ploam 03ff03 000001 0000...
# 3. 收尾看 omci_transport 是否 ready=1 error=0
```

重启即恢复：注入只改运行期状态，`pon.*` 配置没有被动过（`enabled=1` 是有意保留，
插纤需要它）。

## 与 docs/111、112 的关系

- 111/112 修的是**以太网侧**（SOPC ready 读即清、100M 速率码），与 PON 无关，
  但暴露了同一个思维陷阱：**一个读操作、一个恒值字段，都能让诊断结论跑偏**。
- 本文是 PON 侧的第一次"端到端"（不含光），把 -67 从一个"神秘硬件错误"
  降级成"配置没开"。
