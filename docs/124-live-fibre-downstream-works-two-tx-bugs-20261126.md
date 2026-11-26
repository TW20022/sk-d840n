# 124 — 有光实测：下行确认可用，上行卡在两个自己写的 bug 后面

日期：2026-11-26 19:40–20:30
设备：SK-D840N / ImmortalWrt 6.18.60+ / 光纤插在 SK 上，OLT 为华为（PON-TAG `HWTC PON`）

## 1. 光路确认

| 项 | 值 | 来源 |
|---|---|---|
| RX 光功率 | **-22.92 dBm** | `/usr/sbin/bosa_diag`（UX3361，status 0xc0） |
| 模块温度 / 电压 | 16.7 ℃ / 3.33 V | 同上 |
| LOS 脚 | `opsd` in **lo**（ACTIVE_LOW）= light | 同上 |
| LOS LED | `losled=0` | `pon_rxwatch.log` |
| 下行状态 | `0x16105 = 0x00000002`（bit4 下行丢失 = 0） | 同上 |
| 下行数据寄存器 | `0x16148` 持续变化，如 `03ff01d9` | 同上 |
| 下行 PLOAM 计数 | `rx` 单调增长（20 分钟 172 帧） | `pon_hw` |

**结论：下行接收链路（光→SerDes→XMAC→PLOAM FIFO）是通的**，不是之前怀疑的"收帧通路有问题"。

OLT 只发一种下行消息：`id=0x01`（Burst Profile），20 分钟 760 帧，从未出现任何
发给本 ONU 的定向消息。也就是说 OLT 从来没有识别出我们的序列号。

## 2. Bug 1：广播帧被 MIC 全部丢弃

`zxpon_rx_ploam()` 第一件事是 `zxpon_ploam_mic_ok()`，不通过就 `return -1`。
未注册的 ONU 没有密钥，OLT 广播帧尾那 8 字节用任何能猜的密钥都算不出来：

- 用默认密钥 `0x55×16` 以及 `0x00×16`；
- 对消息的每一种前缀（16…44 字节）和每一个起始偏移（0/4）；
- 对 4 字节与 8 字节两种截断、对尾部每个 4 字节窗口；

全部试过，与真实帧尾 `36 0f 50 fd 25 5d c7 30` 无一对上。而 CMAC 实现本身是对的
（NIST SP 800-38B 的空消息与 40 字节向量都逐字节匹配）。

后果：`rx=152` 时 `pon_profile` 仍是 `no profiles learned`——状态机一帧都没见到。

修法：广播帧（ONU-ID `ZXPON_BCAST_ONUID = 0x03ff`）不做 MIC，计入 `micbad`；定向消息保持严格校验
（那时已经有密钥）。改完现场立刻学到 4 个真实 profile：

```
idx=0 ver=0 fec=0 delim_len=4 pre_len=4 pre_repeat=9  delim=ad 4c c3 0f  pre=aa*8  tag="HWTC PON"
idx=1 ver=0 fec=1 delim_len=4 pre_len=4 pre_repeat=9  delim=a5 66 79 e0  pre=aa*8  tag="HWTC PON"
idx=2 ver=0 fec=0 delim_len=8 ...                     delim=ad 4c c3 0f  pre=aa*8
idx=3 ver=0 fec=1 ...                                 delim=a5 66 79 e0  pre=aa*8
```

## 3. Bug 2：上行 kick 在同一次调用里就被撤销

旧 `zx_pon_hw_ploam_tx()` 写数据 → 置 `0x16642` bit0 → **原地轮询 16 次（几微秒）**
→ 不落位就 `zx_xmac_w(0x16642, 0)` 并返回 `-ETIMEDOUT`。

上行突发是"请求"不是"寄存器写"：MAC 拿到请求后要等 OLT 在 BWmap 里开出授权窗口，
最坏要等一个超帧。几微秒后清 kick 等于每次都把突发在授权到来前撤销。
现场表现完全吻合：有光后 `tx` 只涨到 10 就再也不动，`txerr` 每 tick +1。

改法：kick 后立即返回，`zx_pon_hw_tx_reap()` 在后续 tick 观察完成位；只有连续 50 个
tick（5 s）仍不落位才清位并计一次真错误。

改完后 `tx_kick` 变成稳定 1、`txerr` 每 5 s +1——即**每一次都等满 5 秒也没有授权窗口**。
这说明 kick 本身不再是问题，问题是 MAC 根本没认出授权。

## 4. 剩下的卡点（下一步）

现场与厂商 `zte_xgpon.ko` 的对比结果：

- 厂商 `fi_Configure_Ponmac_Send_Sr_Ploam_Msg`（`0x5af8`）在 kick 前会读改写 `0x1666f`
  （先等 bit0 清，再写 `(param&3)<<1 | 8`，通常 `0x0a`），随后写数据到 `0x166c2`、
  置 `0x16642=1`、再轮询 `0x16642`。我们缺 `0x1666f` 这一步，但现场手工写入 `0x0a`
  后 kick 仍不落位，所以它不是唯一原因。
- 厂商还有 `fi_configure_Set_Bwmap_Record_Mode` / `fi_configure_Reset_Bwmap_Ram_List`，
  我们的驱动完全没有实现 BWmap 记录模式与分配表。XG-PON 的授权是按 Alloc-ID 匹配的，
  表里没有条目时硬件不会认领任何窗口——这与"kick 永远等不到"最吻合。

下一步按这个顺序做：
1. 用 `0x1666f` 的读写序列替换现在裸 kick（对齐厂商流程）；
2. 实现 BWmap 记录模式 + 入门的 Alloc-ID 1023（未注册 ONU 的 SN 授权）条目；
3. 授权能认领后，再把学到的 profile（delimiter / preamble / FEC）写进
   `0x16630..0x16633` 与 `0x1688e`，让 SN 突发能被 OLT 解出来。

## 5. 复现命令

```
cat /sys/class/net/eth0/pon_state
cat /sys/class/net/eth0/pon_profile
cat /sys/class/net/eth0/pon_hw
tail -3 /root/rxwatch.log
/usr/sbin/bosa_diag
```

`pon_hw` 现在多一个字段 `micbad=`，表示因广播帧 MIC 无法校验而放行的帧数。
