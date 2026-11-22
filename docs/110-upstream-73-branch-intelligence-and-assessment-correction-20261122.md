# 上游同源仓库 7.3 分支情报，以及 PON 攻关评估的修正（2026-11-22）

> 触发：用户指出「我们的仓库来源于 woodnaonly 的源码」。核实后确认属实，并在上游
> `woodnaonly/zx279133-ponwrt-build` 的 `feat/kernel-7.3` 分支发现当天（11-22）刚推送的
> 11 个新提交。这些情报**修正了 11-21 给出的悲观评估**：NPU 驱动不是「没人驱动」，
> 而是**源码公开、且有一份做到 O5 的参照固件存在**。本文记录证据与影响。
>
> 本文只做分析，不改代码、不碰设备、不 push。逆向产物离线留存于
> `pon_revive/upstream_73_intel/`。

## 0. 同源关系确认

| 项 | 证据 |
|---|---|
| 本仓库 initial import | `b43cf89`（2026-11-18）「initial import of the fixed firmware source tree」，一次导入 6 项硬件/配置修复 + 2 项构建兼容修复 |
| 上游 main 停滞点 | `7995c29`（2026-11-10）「docs: add Chinese and English SK-D840N usage guides」 |
| 基底一致 | 双方均用 `cnjn/ImmortalWrt` `1b50aaf4` + ZX279133 `96e5a241` 驱动 + kernel 6.18.60 |
| 本仓库独有的增量 | 11-20/21 的 PON MAC 硬件层移植（`d825f09`+`3562022`）、LAN1 100M 分支、本地 docs/109 |

结论：本仓库是 woodnaonly 源码树的**分叉 + 独立 PON 攻关线**。上游 11-10 后 main 停更，
但 11-22 在 `feat/kernel-7.3` 分支恢复了高强度开发。

## 1. 上游 11-22 新提交（feat/kernel-7.3，11 个）

从头 `deb9145`（11-22 11:22Z）往前，核心提交：

- `830d9e7` / `8962600`：**vendor 了 cnjn 的 39 个 `patches-7.3` 补丁 + NPU 逆向产物**，
  新建 Linux 7.3.4 构建线。基线 sha256 与 kernel.org 一致，39/39 补丁干净 apply。
- `3dbf300` / `9433561` / `8bca295`：把本仓库实测的 IDM 寄存器值、RX frame 0x80 offset、
  BP refill 协议重打到 7.3 树（4 个 local patch：100/110/120/130）。
- `deb9145`：把 9132 switch 驱动移植进 7.3 树（尚未编译，本机无交叉工具链）。
- `edc3db7`：修 CI workflow 解析错误（7.3 线 run 45014852319 零 job）。

## 2. ★ 决定性的好消息：参照固件做到 O5，密钥/MIC/GEM 全实现

上游逆向的「别人的固件」= `cnjn` 的 7.3.4 `star_sg350h` initramfs：

```
sha256 c6e5010d4b59dbb28d94b6ead8c44d94024c912d761b93fd5186b2d1855fc046
```

其 `npu-pon.ko` 的 `.rodata` 字符串（离线留存 `pon_revive/upstream_73_intel/
pon_npu_reverse/npu-pon.ko.rodata.txt`）证明这份固件的 PON 协议栈**走到了 O5，
并且实现了我们卡住的全部环节**：

| 环节 | rodata 铁证 | 对我方的意义 |
|---|---|---|
| PLOAM 注册 | `Assigned ONU-ID %u, state -> O4` / `Ranging EqD=%u complete, state -> O5` | O4/O5 状态机是真实实现，非伪装 |
| **密钥生命周期** | `OMCIIntegrityKey PLOAMIntegrtyKey KeyEncryptionKey`（两组）/ `zx279133_pon_handle_key_control` / `keys_derived=%u` | ★ 我们 `omci error=-67` 卡的就是这块，别人已实现 |
| **OMCI MIC 校验** | `Downstream OMCI-MIC verified OK` / `mismatch (rx=%08x calc=%08x)` / `omci_mic_verified=%u` | ★ 我们 zxpond 里 `transport_ready:false` 写死的那一层 |
| OMCI RX/TX 多路径 | `omci_ik_mode/omci_rx_mode/omci_tx_mode` modparam；`idm48`/`lp_flag`/`lp_oem`/`lp_shift17`/`dual_auto`/`immediate`/`after_next_tx` | 对方做了多套 OMCI TX 路径 fallback，说明这块当年也不好调 |
| GEM/T-CONT 流表 | `flow[%u]=alloc:%u gem:%u uni:%u ani:%u act:%u tcont:%u mc:%u aes:%u` / `ugem_tcont_mon` / `omcc_gem_mon` | ★ 正是我们 docs/109 逆向的下一关，别人已跑通 |
| O5 停滞恢复 | `o5_stall_recoveries=%u` / `deactivate_cnt` / `Disable_Serial_Number emergency stop` | 插纤后可能的坑，对方有恢复逻辑 |
| 光层状态 | `rx_los` / `cdr_locked` / `ds_synced` / `pll_locked` / `bosa_opsd` / `PON SerDes PLL lock pending` | 印证 docs/91 的「假失光/BOSA」怀疑是真实工程问题 |

结论：**这条 PON 路线是可通的，有做到 O5 的完整参照实现**。这推翻了 11-21 评估里
「第 4/5 关硬骨头、没现成方案」的悲观论断——现成的**二进制和逆向产物**在，只是源码未公开。

## 3. 但必须守住的三个边界（避免 again 乐观）

1. **源码未公开**：`npu-pon.ko` 只有二进制。逆向出的 30 个 PON 钩子（`zx279133_pon_prepare`、
   `pon_rx_fixup_hook`、`pon_rx_omci_hook`、`idm_omci_xmit_ppu`、`omcc_gem_id` 等）
   公开 master 里根本没有。上游 docs/100 明确说「等 cnjn 公开源码，要么对齐要么让位」。
2. **不是我们的板**：参照固件是 `star_sg350h`（SR1010 系），DTS 与 SK-D840N 不同；
   `npu.ko` 用的 mcode_intel.bin 倒是和我们**同名同尺寸**（103,456 B）——微码同族。
3. **7.3 线自己也没搬 PON**：上游 docs/101 明确「PON/OMCI 56 个函数仍未移植，
   cnjn 公开树里没有 PON 模块」，9132 驱动也**尚未编译**。这条线当前不能产出可用固件。

## 4. 对我方「已刷 6.18 PON MAC 固件」的影响：零直接影响

- 7.3 是**独立构建线，明确不发布固件**（release line 仍在 6.18）。当前设备固件不受影响。
- 上游在 7.3 重打的「RX frame 0x80 offset」——**我方 6.18 overlay 早就有**
  （docs/77、`ZX279133_IDM_RX_FRAME_OFFSET`，commit `1ddcc4b`/`ecfe452`）。不是新东西。
- IDM 五条实测值（0x124=0x80、TX_CFG_DEPTH=0x400 等）——我方 6.18 也早已实测落地。
- 所以：**不触发任何重编译/重刷决策**。当前验证通过的固件状态保持。

## 5. 评估修正（对照 11-21 的关卡图）

| 关卡 | 11-21 判断 | 11-22 修正后 |
|---|---|---|
| 第 1 关 PON MAC 硬件层 | 已过 | 已过（不变） |
| 第 2 关 光路/LOS | 待插纤，怀疑假失光 | 待插纤；对方 `bosa_opsd`/`PLL lock pending` 证实该怀疑真实，但其驱动有对应处理 |
| 第 3 关 PLOAM→O5 | 较乐观 | **有完整参照实现**（O4/O5 字符串确凿），信心上调 |
| 第 4 关 OMCI 通道 | 「半个 transport，1-2 周」 | **别人已实现 MIC+密钥+多 TX 路径**；有逆向产物可参照，但源码未公开，仍需自研/逆向 |
| 第 5 关 GEM/T-CONT+PPU | 「数周无把握、可能动微码」 | **别人已跑通到 service_ready/flow_count**；微码同族。有 `flow[]`/`omcc_gem_mon` 字符串和寄存器线索。难度下调但非零 |
| 第 6 关 PPPoE+LAN | 未攻克 | 不变（LAN1 数据面仍是我们独有难题） |

**总判断**：打通的**可行性上调**（有做到 O5 的参照），但**源码未公开 + 异板 + 7.3 线自身未搬 PON**
意味着仍不能「抄一下就通」。48 小时后的第一道门仍然是插纤跑 `fiber_check.py` 判 O5，
这一条不因本情报改变。本情报的价值是：万一插纤后卡在第 4/5 关，我们手上有
`pon_revive/upstream_73_intel/` 的逆向产物可作寄存器/状态机参照，不必从零逆向。

## 6. 留存物

```
pon_revive/upstream_73_intel/
  docs100-npu-driver-7.3-20261122.md       NPU 驱动来源、99 API、逆向方法、overlay 差异
  docs101-7.3.4-baseline-20261122.md       7.3.4 基线、迁移面、IDM/RX 补丁、未移植项
  pon_npu_reverse/
    npu.ko.api.txt / funcs.txt / rodata.txt
    npu-pon.ko.rodata.txt ★                 PON 做到 O5 的铁证（本节核心）
    npu-pon.ko.funcs.txt / zx279133-ge.ko.rodata.txt
```
