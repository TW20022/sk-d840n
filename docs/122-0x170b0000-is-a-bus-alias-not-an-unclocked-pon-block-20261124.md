# 122 — 0x170B0000 不是"没上时钟的 PON 子块"，它是总线常量；片内已无可配项

日期：2026-11-24 19:55–20:15（无纤，全程只读 + 两次 RAM 热写，未刷机、未改 flash）

接 docs/121 第 6 节留下的唯一线索：

> 本板 `0x17090000–0x170FFFFF` 整段读 0、写不进，HX5 的 `0x170B0010 = 0x170B0060 = 0x07D0000C`
> ⇒ 疑似一个未上时钟的 PON 子块，下一步给它找时钟门。

这一轮把这条线索查到底。结论：**线索本身是误读，那个子块不是寄存器阵列，不存在"时钟门没开"。**
同时顺手把「0x16105 bit4 到底是什么」从"我的推断"升级成"厂商二进制里可引用的定义"。

## 1. TOPCRM（0x10e10000）整段对比：没有一个门是关着的

PON 节点的 reg 第 3 项就是 `0x10e10000`，而 `zx279133_soc_block_ok()` 的白名单里本来就有
`{ 0x10e10000, 0x100 }`（`soc` / `socw` 命令），所以**不用改驱动、不用刷机**就能读它。

- 本板：`soc 0x10e10000 64`，输出在 dmesg，存在 `pon_revive/sk_topcrm_20261124.txt`
- HX5：`devmem2` 逐字读同段（登录后是 uid=108 的普通 shell，`devmem2` 报
  `Error at line 42 ... [Permission denied]`，必须先 `su` 拿 root，提示符变成 `root@zxic:~ #`），
  存在 `pon_revive/hx5_topcrm_20261124.txt`，脚本 `pon_revive/hx5_topcrm_read.py`

64 个字里只有 4 个不同：

| 地址 | 本板 | HX5 | 差异位 | 方向 |
|---|---|---|---|---|
| `0x10e10008` | `0x00003711` | `0x00003511` | bit9 | 本板置、HX5 清 |
| `0x10e1000c` | `0x06713077` | `0x06713277` | bit9 | 本板清、HX5 置 |
| `0x10e10038` | `0x33dbf77f` | `0x33fbf77f` | bit21 | **本板清、HX5 置** |
| `0x10e100b0` | `0x60103031` | `0x40103031` | bit29 | 本板置、HX5 清 |

`0x08` / `0x0c` 的 bit9 是互补的一对——同一路时钟的两个候选源，两块板选了不同的一个，
属于板级差异（`0x10e100b0` bit29 同理，方向还相反）。真正"HX5 开、本板关"的只有 **`0x38` bit21** 一个位。

实测把它置上：

```
socw 0x10e10038 0x33fbf77f   → 回读 0x33fbf77f（写进去了）
rdreg 0x170B0010             → 仍然 0x00000000
rdreg 0x170B0060             → 仍然 0x00000000
pon_state                    → 仍 O2-3，板子正常
```

无变化。已恢复成 `0x33dbf77f`。

## 2. 内核时钟树：PON 时钟全在硬件使能状态

比猜 TOPCRM 位更权威的是内核自己的账本：

```
cat /sys/kernel/debug/clk/clk_summary | grep -i pon
    clk_pon_125m      0  0  0  125000000 ...  Y  deviceless
       pon_nppt_wclk_mux  1  1  0  416666666  Y
          pon_woe1_wclk   1  1  0  416666666  Y  19000000.ethernet
          pon_mac_wclk    1  1  0  250000000  Y  19000000.ethernet
          pon_tm_aclk     1  1  0  250000000  Y  19000000.ethernet
          pon_idm_aclk    1  1  0  250000000  Y  19000000.ethernet
             pon_pclk     1  1  0  125000000  Y  19000000.ethernet
             pon_serdes_pclk 2  2  0 125000000  Y  16000000.phy
             uni_serdes_pclk 2  2  0 125000000  Y  16100000.phy
       pon_smac_wclk   1  1  0  125000000  Y  19000000.ethernet
```

最后一列是 **hardware enable**，全是 `Y`。`clk_pon_125m` 的 enable/prepare 计数是 0，
但 hardware enable 仍是 `Y`——它只是没有 consumer（`pll_2000m_clk`、`clk1000m_cpu` 等也是
同样的 0/0/0 + `Y`），不是被关掉。

结论：**没有任何 PON 时钟处于门控关闭状态**，所以"给子块找时钟门"这个方向从一开始就不成立。

## 3. 0x170B0000 的真面目：整页返回同一个常量

在 HX5 上把这一段一个字节一个字地读完（脚本见上，全部 `devmem2` 只读）：

```
0x170b0000 = 0x07d0000c
0x170b0004 = 0x07d0000c
...
0x170b007c = 0x07d0000c      ← 32 个字，全部相同
```

再往两边探：

| 地址 | HX5 | 本板 |
|---|---|---|
| `0x17080000` | `0x60` | — |
| `0x17090000` | `0x0` | `0x0` |
| `0x170A0000` | `0x0` | `0x0` |
| **`0x170B0000`** | **`0x07D0000C`（整页）** | **`0x0`（整页）** |
| `0x170C0000` … `0x170F0000` | `0x0` | `0x0` |

三点足以定性：

1. **32 个"配置寄存器"的内容完全相同，这在硬件里不存在。** 一个真正的寄存器阵列不会这样；
   整页同值是地址未译码 / 单一寄存器被整页别名（alias）的典型表现。
2. **厂商代码从不访问这个索引。** 新写的 `pon_revive/tools/xmac_write_list.py` 把
   `zte_xgpon.ko` 全 `.text` 里每次 `Hal_WriteXmacReg` 的索引反推出来（回溯最近的 `mov w0,#imm`
   / `movz+movk w0`），索引只落在三段：`0x16xxx`（XMAC1 本体）、`0x30xxx`（`0x170C0000`）、
   `0x50xxx`（`0x17140000`）。**没有任何 `0x2C0xx`**（`0x2C004 × 4 = 0xB0010`）。
3. **本板写它被丢弃**：`wrreg 0x170B0010 0x07d0000c` → 回读 `0x00000000`；
   再写 `0xdeadbeef` → 回读仍 `0x00000000`，板子不受影响。

所以 docs/121 第 6 节那句"HX5 的 0x170B0010 是本板缺的配置"**作废**。
那块"唯一剩下的硬件差异"不存在，XMAC1 侧确实已经逐位对齐。

## 4. `0x16105` bit4 的定义：厂商二进制里可引用，不是我猜的

驱动代码里其实**没有任何 LOF 判断**（`grep -n "0x16105\|LOF" zxpon.c zx279133-pon.c` 为空），
状态机只认 PLOAM/OMCI。之前"bit4 = 实时 LOF"是我从观测推的。这回在厂商模块里找到了原文：

```
zte_xgpon.ko @0x00e4c8  mov  w0, #0x6105
             @0x00e4cc  movk w0, #1, lsl #16      ; w0 = 0x16105
             @0x00e4d0  bl   Hal_ReadXmacReg
             @0x00e4d8  tbnz w0, #4, 0x00e534     ; bit4 置位 → 跳过
             ...                                  ; bit4 清零才走：
             @0x00e520  bl   printk
             @0x00e528  bl   fi_Configure_Ponmac_Set_Upstream_Enable(1)
             @0x00e530  bl   fi_Configure_SN_AllocId_Set(1)
```

另一处（`@0x010180`）：

```
             @0x010178  bl   Hal_ReadXmacReg        ; 0x16105
             @0x010180  tbz  w0, #4, 0x00e1c8      ; bit4 清 → 跳过 O5 分支
             @0x010190  ldrb w0, [XgponDataBase+0x2642]
             @0x010194  cmp  w0, #5                ; 状态 5 = O5
```

即：**`0x16105` bit4 置位时，厂商自己的代码也不开上行、不发 SN/AllocID。**
这和本板的行为完全一致（bit4 = `0x10` → 上行被压住，只在 O2-3）。

HX5 在 O5 时 `0x16105 = 0x2`（bit4 清），本板 `0x10`（bit4 置）。

## 5. 「配置寄存器」与「信号派生寄存器」的分界

把 `0x16300–0x1633f`、`0x16400–0x1641f` 两边都读回来（本板用 `pon_reg` 的 `s` 扫描，
HX5 用 `devmem2`，脚本输出在 `hx5_framer_20261124.txt`），差异干净地分成两类：

**配置类——逐字相同（说明配置真的是对的）：**

| 索引 | 本板 | HX5 |
|---|---|---|
| `0x16401` | `0x000007bc` | `0x000007bc` |
| `0x16402` | `0x00000008` | `0x00000008` |
| `0x16403` | `0x00003fff` | `0x00003fff` |
| `0x16404` | `0x00000040` | `0x00000040` |
| `0x16412–0x16416` | `0xffffffff` | `0xffffffff` |

**信号派生类——本板全 0，HX5 有活值（只有收到下行才会有）：**

| 索引 | 本板 | HX5 | 说明 |
|---|---|---|---|
| `0x16148` | `0x00000000` | `0x03FF0127` | 收到的下行字（`0x3FF` = 广播 ONU-ID + 消息号 01，即 OLT 的 SN 请求） |
| `0x16120` | `0x00000000` | — | 下行对齐候选 |
| `0x1632e` | `0x00000000` | `0x00820082` | 下行测量块 |
| `0x1632f` | `0x00000000` | `0x00003001` | 同上 |
| `0x16332` | `0x00000000` | `0x005b005b` | 同上 |
| `0x16333` | `0x00000000` | `0x0000cd40` | 同上 |
| `0x16334` | `0x00000000` | `0x0000310a` | 同上 |
| `0x16335` | `0x01000001` | `0x01008601` | 同上（bit9/10/13 是收到信号后才有） |
| `0x16336` | `0x00000880` | `0x054418a0` | 同上 |
| `0x1640a` / `0x1640b` | `0x0` / `0x0` | `0x3385087b` / `0x00923d38` | 收比特计数器，只有收到才走 |

这张表就是"为什么不能靠抄寄存器解决"的直接原因：剩下不同的那些字，是**结果**，不是**配置**。
抄过来下一次读还是 0。

## 6. 物理层的直接判读：模块的 SD 引脚

```
cat /sys/kernel/debug/gpio
 gpio-9   ( |skyworth:red:los ) out lo ACTIVE LOW    ← ACTIVE LOW + out lo = LOS 灯亮
 gpio-1   ( |opsd             ) in  lo               ← 光模块 Signal Detect = lo
 gpio-15  ( |skyworth:green:pon) out hi ACTIVE LOW   ← PON 灯灭
```

`opsd` 是光模块的 SD（Signal Detect）输入，低电平 = 模块自己认为没有光。
这条与第 4 节的 `0x16105` bit4、第 5 节那些全 0 的接收计数器互不依赖，三条都指向同一件事。

## 7. 结论

A（找 0x170B0000 的时钟门）**查完，结果是负的，而且是三重否证**：

1. TOPCRM 里没有"HX5 开、本板关"的门；唯一候选位写上去无任何变化；
2. 内核时钟树里 PON 相关时钟 hardware enable 全为 `Y`；
3. 那个"子块"是地址别名返回的常量，厂商代码也从不访问它——它从来就不是配置块。

于是 docs/121 的"唯一剩余硬件差异"被删掉，**XMAC1/PCS 侧已无可配项**。
剩下的差异全部是"收到下行之后才会出现的测量值"，本板为 0 是因为没有下行。

下一步只有一件事：**插纤**。插上之后一条命令：

```
python pon_first_light.py            # 默认跑 300 秒，每个采样点打印
```

脚本已升级（`pon_revive/pon_first_light.py`），会在报告里直接给出三条互斥结论之一：

| 现象 | 结论 |
|---|---|
| `opsd=lo` 且 `0x16105` bit4 置位 | 模块 SD 没拉起 → 纤没到位/光功率不足，寄存器侧已无可动项 |
| `opsd=hi` 但 bit4 仍置位 | **真正的片内问题**，看 PCS/Psync 段 |
| bit4 清零但没到 O5 | 下行帧进来了，问题在 O3/O4 握手，看 rx/tx 与 PLOAM 计数 |

## 8. 复现命令

```sh
# 本板 TOPCRM（不用改驱动，白名单里就有）
D=/sys/kernel/debug/zx279133-9132
dmesg -c >/dev/null; echo "soc 0x10e10000 64" > $D; dmesg | grep "soc reg"

# 本板 PON 时钟
cat /sys/kernel/debug/clk/clk_summary | grep -i pon

# 那个"子块"写不进
wrreg 0x170B0010 0x07d0000c; rdreg 0x170B0010     # 恒 0

# 厂商写过的 XMAC 索引全集
python pon_revive/tools/xmac_write_list.py oem_rootfs/kmodule/zte_xgpon.ko

# 厂商对 0x16105 bit4 的判断
python pon_revive/tools/dis_ko.py oem_rootfs/kmodule/zte_xgpon.ko 0xe4a0 90

# HX5 只读对照（登录后必须 su）
python pon_revive/hx5_topcrm_read.py
```

新增文件：`pon_revive/tools/xmac_write_list.py`、`pon_revive/hx5_topcrm_read.py`、
`pon_revive/sk_topcrm_20261124.txt`、`pon_revive/hx5_topcrm_20261124.txt`、
`pon_revive/hx5_framer_20261124.txt`、`pon_revive/sk_framer_20261124.txt`。
