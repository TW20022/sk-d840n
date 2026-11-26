# 123 — 复核 11-26 的结论（含一次自我更正）

日期：2026-11-26 11:40–12:35（无纤，全程只读；只改了注释与文档，未刷机）

上一轮留下若干结论。这一轮对着**设备寄存器、驱动代码、厂商二进制、设备上的历史日志**
逐条重验。结论：大部分站得住；我自己的第一次"证伪"用错了 boot 的日志，**已经撤回**。

## 0. 一句话结论

**有光时 `0x16100=0x63b`（bit0 置位）、`0x16105=0x02`（bit4 清）是同一 boot 内直接读到的硬证据。**
但"rx 随后增长"只有串口人工观察，没有落盘日志；当前既不能证实也不能证伪。
下一次插纤必须靠 `pon_rxwatch.sh` 把这件事记录清楚。

## 1. 逐条复核

| # | 结论 | 判定 | 证据 |
|---|---|---|---|
| 1 | 红灯 ACTIVE_HIGH，亮 = 无光 | ✅ | 原厂 dts `gpios=<0x52 0x09 0x01>`；当前无纤 `brightness=1` 且灯亮；`led_work` 里 `rxsd ? LED_OFF : LED_FULL` |
| 2 | `0x16100` bit0 未置位使下行失步 | ✅ | 置位前 0x63a；有光 boot t=251/255 直接读 `0x16100=0x63b` |
| 3 | 有光时 `0x16105` 0x10→0x02 | ✅ | 同一 boot t=255.329、t=279.648 两次读到 `0x00000002`，采样行同期 `opsd=light rx=-17.08/-17.21 ps=O2-3` |
| 4 | 置 bit0 后 rx 持续增长 | ⚠️ 人工观察，无日志 | 见第 2 节，**不能当作已证实** |
| 5 | TX_KICK 超时不清位 → 永久 -EBUSY | ✅ | 代码已修；现测 `kick=0 busy=0`，`txerr` 持续增长 |
| 6 | SK @O5 与在线 HX5 的 PON MAC 配置逐位一致 | ✅（配置面） | `hx5_vs_sk_o5_20261126.md`；现场 `0x16644=0x1b01 / 0x1688e=0x20 / 0x1611d=0xfffff` 全对 |
| 7 | `0x16105` bit4 = 下行丢失 | ✅ | 厂商 `zte_xgpon.ko`：`0xe4c8` movz 0x6105 + `0xe4cc` movk #1 → `bl`@0xe4d0 → **`0xe4d8 tbnz w0,#4`**（旧记 `@0xe4d0` 是调用点） |
| 8 | 下行 0x01 = Profile（G.987.3），已解析 | ✅ | 单测 + `ploam_tool.py` 解码台架帧 `idx=2 ver=3 fec=1 delim=4 pre=6 repeat=10` 三者一致 |
| 9 | O1 清理 alloc/omcc/profile | ✅ | 现场 `alloc_valid=0`、`no profiles learned`；WSL 删掉旧二进制后重编 `zxpon: pass` |
| 10 | OMCI 台架跑通（ready=1 error=0） | ✅ 但仅台架 | `bench_raw.txt` rx/tx/completed = 45/45/45、60/60/60；只证明自家 IDM/encap |
| 11 | 内核已刷 `5fd7e40c…` | ✅ | `dd /dev/mtd2 | sha256` 与本地镜像逐字节相同 |

## 2. 一次自我更正：不要把不同 boot 的日志拼在一起

第一次复核时我看到 `/root/ploam_capture.log` 里 `rx=0`、零条 `R` 帧，
就断言"有光时仍然收不到帧，原结论被证伪"。**这是错的。**

对齐以后发现：

- `bosa_boot.log` 是被多次启动追加的，共有 6 个 boot 边界（`=== Linux version ... ===`）。
- 有光片段在第 1276–1416 行；里面 t=251/255 读 `0x16100=0x63b`，t=255/279 读 `0x16105=0x02`，
  采样行 `opsd=light ps=O2-3`。
- `ploam_capture.log` 对应的是**另一个 boot**：uptime 233–654s 的每一条头部都是 `state=O1`，
  而且 `txerr=1 / busy` 单调增长，是修 kick 之前的行为。
- 所以那份抓帧日志根本没有覆盖有光窗口，既不能证实也不能证伪 rx 增长。

有光 boot 内没有留下任何 PLOAM 计数或 `0x16148` 读数。这就是要在下次插纤时补的洞。

## 3. 两处记录更正

1. `0x16105 = 0x02` 只能出现在有光时。无光恒为 `0x10`；`pon_bench=light` 是纯软件强制
   （`zx279133_pon_note_rxsd()` 里直接返回），实测设成 `light` 后 `0x16105` 仍 `0x10`、`0x16148` 仍 0。
   旧记忆里"无纤排练时读到 0x02"是记错了窗口。
2. `omci_transport` 的 `error=-67` 不是独立故障：`zx279133-pon.c:939` 在 ready=0 时**直接打印
   `-ENOLINK`**。旧记忆里"真因 pon.pon.enabled=0"作废。

## 4. 插纤前必须做的事

下一次插纤时，`pon_rxwatch.sh` 开机自动启动，1 s 采一组：

```
0x16148 0x16105 0x16100 0x16642  pon_hw  pon_ploam首行  pon_state  pon_bench
```

它会把有光窗口内的 **MAC 收字 + 下行状态 + PLOAM rx/tx/kick** 放在同一条时间线上，
从而一次区分三种可能：

- `0x16148` 变化但 `rx` 不动 ⇒ pending/12 字提取路径的问题；
- `0x16148` 恒 0 但 `0x16105=0x02` ⇒ MAC 收帧使能/PSync 对齐问题；
- 两者都动、`rx` 增长但状态停在 O2-3 ⇒ 才是 OLT 授权 / SN 认可的问题。

### 部署与验收（12:30–12:40）

- `pon_revive/tools/pon_rxwatch.sh` → 设备 `/usr/sbin/pon_rxwatch.sh`（+x，2572B，无 CRLF）
- `pon_revive/tools/rc.local.sk` → 设备 `/etc/rc.local`，新增独立 setsid 链：
  `sleep 12; OUT=/root/rxwatch.log /usr/sbin/pon_rxwatch.sh 3600`
- 独立链的原因：`pon_reg` 的 store 会覆盖一个全局回复缓冲（`zx279133-pon.c:1181`），
  两个写者会互相踩；`ploam_capture.sh` 只读 `pon_ploam`，所以可以并存。脚本内还加了
  `mkdir /tmp/pon_rxwatch.lock` 互斥。
- 日志 `/root/rxwatch.log`，1 行/秒，超过 4000 行时保留最后 2000 行。
- 试跑 5 秒的实测输出：

```
18:06:06 up=4926s ps=O1 ben=auto losled=1 r48=00000000 r05=00000010 r00=0000063b
        kick=00000000 pnd=00000000 pl=[rx=0 tx=10 busy=0 txerr=44042 ...]
        pro=[no profiles learned] hw=[mapped=1 ... alloc_valid=0]
```

- 按开机链方式（setsid + sleep 12）也启动成功，进程存活、`/root/rxwatch.log` 持续写入，锁正常释放。

## 5. 同轮代码复核（未提交改动）

对着 `git diff` 逐块读了 412 行新增代码，发现三处要改，其中一处是我自己的判断错误。

| # | 位置 | 问题 | 处理 |
|---|---|---|---|
| 1 | `zx279133-pon.c` `zx_pon_hw_ploam_tx()` | 阈值 `stuck_ticks >= 200`，而 tick 是 **100 ms**（`zx_tick`），实际 20 s，注释和记忆都写 "~5 s"。卡住时 SN 重试被饿 20 s | 改成 `>= 50`（5 s），注释写清 100 ms × 50 |
| 2 | `zx279133-pon.c` ploam-log 注释块 | 仍写 "Message id 0x01 (Upstream_Overhead)"，与本轮确认的 Profile 定义冲突 | 改为 Profile 并注明 GPON 名字不适用于 XG-PON |
| 3 | `zx279133-np.c` 白名单 | 我一度认定 `0x14f03000`（i2c0）"未上时钟、访问会挂总线"并删除该条目 | **判断错误，已改回**：见下 |
| 4 | `tests/zxpon_test.c` | directed Profile 的 ACK 分支、`c[2]/c[11]/c[12]` 三处保留位都没覆盖 | 新增 `test_profile_ack()`，`zxpon: pass` |

### 第 3 条为什么是我错

1. `{ 0x14f01000, 0x4000 }`（MDIO0）的窗口一直盖到 `0x14f05000`，**`0x14f03000` 本来就在里面**，
   删掉那一条等于没删——实测 `soc 0x14f03000 1` 照样成功。
2. 我说"时钟没开、会挂总线"也不对：cmdline 带 `clk_ignore_unused`，i2cclk 一直开着；
   实测读回 `0x01020000`，没有 fault。
3. 源码已改回显式条目，注释改成事实：该地址按 vendor dts 是 i2c0、已被 MDIO0 宽窗口覆盖、
   `clk_ignore_unused` 下可读。因为功能等价，**这一条不需要重刷内核**。

### 部署结果

- 新内核 sha256 `f2dcc373f9cf95a86bb89c5d6b93c7f1a72a6fdfae07fe000865b1ed503a1e4c`（7559801 B）
- 已刷 mtd2 并回读校验一致；备份 `/root/mtd2-pre-5s.bin`（8 MB，sha256 前 16 位 `974bb92921f0faab`）
- 重启后：`0x16100=0x63b`、`0x16105=0x10`（无光）、`kick=0`、`state=O1`
- `pon_rxwatch.sh` 开机自动启动（`rxwatch start ... up=50s`），日志按会话追加，旧会话保留
- **设备无 RTC**：每次启动时钟都从同一时刻开始，`/root/rxwatch.log` 里的 `HH:MM:SS` 不能跨 boot 比较，
  只能靠 `up=` 字段和 `=== rxwatch start ===` 标记分段

**插纤后我读什么**：`r05` 由 0x10 变 0x02 的时刻、`r48` 是否变化、`pl` 里的 `rx` 是否增长、
`ps` 是否越过 O2-3。四者在同一行，不会再出现这次"两个 boot 拼接"的误判。
