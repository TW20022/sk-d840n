# 118 · 光在 SerDes、framer 仍 LOF：FEC / 时钟 / 路由 / 初始化链逐项排除（2026-11-24）

## 0. 本轮结论一句话

10G 下行码流**确定已进入 PON SerDes**（`0x160000e8` 计数器以 ≈3.14e8 字/秒增长，正好 = 10.3125Gbps ÷ 32bit），
但 XMAC1 framer `0x16105` 恒 `0x10`(LOF)、下行 PM 全 0、`pon_hw rx=0`。
本轮把 **FEC 使能/模式、XMAC1 时钟门、SerDes→XMAC1 路由、SDET PON FIFO、完整 `fi_Configure_Hw_Init` 重放**
全部逐项排除；唯一新增的实质发现是 **驱动初始化时序与厂商不一致（21ms 握手等待被压缩成 0.1ms）+ 漏写 GMAC `0x1000`**。

> ⚠️ 本轮最后一次实验 `regw 0x4000 0x10000`（试图补厂商 `Hal_WriteGmacReg(0x1000, |= 0x10000)`）
> 导致设备 **SSH SIGTERM 后完全离线**（ping 100% 丢包 >5 分钟），见 §8。该地址禁止再写。

---

## 1. 硬证据：SerDes 侧确实在收 10G 连续码流

三次采样（间隔 2s，`soc 0x160000d0 10`）：

```
0x160000e4 = 0x01000706   (bit0 RX_LOS=0, bit1 CDR_LOCK=1) 恒定
0x160000d0 = 0xc86fbb83 → 0xca6fbb83 → 0xc76fbb83   (PLL lock 状态字)
0x160000e8 = 0x0ac0c3b6 → 0x303a7262 → 0x55c11eb6   (自由运行计数)
```

`0x160000e8` 每 2s 增量 ≈ `0x2579AEAC` ≈ 6.28e8，即 **3.14e8 字/秒**。
10.3125 Gbps ÷ 32 bit = 3.22e8 字/秒 —— 完全吻合，这是 **SerDes RX 32-bit 字计数器**。
⇒ 断点被严格限定在 **SerDes → XMAC1 framer 之间**，不是"外部无光"（该结论早已作废）。

对照：`0x16000..0x16020` XMAC 侧计数三次采样**完全不变**
（`16005=324b24ba`、`16006=32`、`16008..0f=00000088`、`16010..=00004bef`），
说明 XMAC 侧没有对应的自由运行计数 —— 数据没有推进到 framer。

SerDes 控制区 `0x16000000..0x3c` 现场（preserve U-Boot 镜像）：
```
00=e0000004 04=50a840a1 08=013e860f 0c=0210c073
10=00000048 14=00000000 18=00ff8000 1c=00238520
20=80000200 24=00030000 28=0c633830 2c=00000007
30=00000020 34=00002000 38=00000000 3c=00ff0000
```
与 `mode_xgpon_nsyn_cfg_133` 49 字 profile 一致，未发现 PRBS/loopback 使能迹象。

---

## 2. FEC 方向：完整排除（8 种组合 + NPPT 使能位）

### 新解出的映射
- `red_set_fec_enable(1)`（`np_133.ko @0x61924`）= `np_onu_reg_write(id=0x12, val, which=0, table=0x9c20, fpga_which=2)`。
  RED/FEC 逻辑表 `@0x9c20` 项 `0x12`：`base=0x08020` → **NPPT+0x20080 bit0**（mask 1, shift 0）。
- `zxic_api_gp_get_fec_state`（`zte_xgpon.ko @0x133c8`）用 `ubfx #2,#2` 读 **`0x16100` bits[3:2]** 作为 FEC 状态，
  并读 `0x1610b` bit18 / `0x1610c`。
- `p_setOltSupportFecOff @0xd524`：按 OLT 能力把 `0x16100` byte0 的 bits[3:2] 写 `0xc`(值3) 或 `0x8`(值2)。

现场：`NPPT+0x20080 = 0`（FEC 使能位未置）、`0x16100=0x63b`（bits[3:2]=2）、`0x1610b/c=0`。

### 实验
1. `regw 0x20080 1` → 读回 1；等 3s，`0x16105/PM/rx` 全部不变。
2. `0x16100 bits[3:2]` 扫 `0/4/8/c` × FEC 使能位 `1/0` 共 **8 种组合**，每种 2s：
   `16100` 依次为 `0x633/0x637/0x63b/0x63f`，`0x16105` 恒 `0x10`、`16106..0a` 恒 0、`rx=0`。
3. 已恢复 `NPPT+0x20080=0`、`0x16100=0x63b`。

⇒ **FEC 使能/模式不是 LOF 原因**。

---

## 3. XMAC1 时钟门：这次是真正验证过的排除

`low_power_api_nppt_open_xmac1_clk`（`np_133.ko @0x12c828`）：
```
agc_set_reg_by_attr(0x10, 1, 0x0001000b)   // attr0x10 → NPPT+0x2c0004 bit11 = 1
agc_set_reg_by_attr(0x0f, 0x3f, 0x00060010) // attr0x0f → NPPT+0x2c0018 bits[21:16] = 0x3f
```
`agc_set_reg_by_attr(attr, val, 0xWWWWSSSS)` 编码解出：**低16位=shift、高16位=位宽**；
`agc_reg_read/write` 用 `auto_gate_control_regs_table @0x27948`（已用重定位 addend 核对）：
```
attr 0x0b → NPPT+0xb8  bit19 (set_sdet_auto_gate_en)
attr 0x0f → NPPT+0x2c0018
attr 0x10 → NPPT+0x2c0004
```
现场：`0x2c0004=0xffffffff`（bit11=1 ✓）、`0x2c0018=0x0fff0fff`（bits[21:16]=0x3f ✓）。
**两条开时钟动作现场已满足** ⇒ XMAC1 数据面时钟门正式排除（此前只是推测，这次有物理映射证据）。

---

## 4. SerDes→XMAC1 路由：地址纠正 + 正确重放，仍排除

`ponserdes_to_xmac1_en_set(en)`（`np_133.ko @0x1c4`）完整解出：
```c
if (en==1 || en==0) greg_sdet_share_clk_cfg(en);   // NPPT+0x19c bit0 = en
w1 = (en != 1);                                    // cset ne
*(pon_base + 0x80) = w1;                          // 字节偏移 0x80
NPPT[0x2438] bit2 = w1;
g_ponserdes_to_xmac1 = (en == 1);
```
`greg_sdet_share_clk_cfg @0xccbc`：`NPPT+0x19c = (old & ~1) | en`。

**★ 地址纠正**：`pon_reg` 用字索引（`ZX_XMAC_REG(idx) = base + idx<<2`），
所以物理 `0x17000080` 对应 **`pon_reg` 索引 `0x20`**，不是 `0x80`。
实测 `w 20 1` 读回 1（可写），`w 80 1` 读回 0（写不进）—— 证实 **`0x20` 才是厂商目标**，
此前用 `0x80` 翻转等价于没碰路由位。

用正确索引重放 en=0 完整组合（`0x19c=0`、`0x2438=7`(bit2=1)、`0x20=1`），
3s 后 `0x16105=0x10`、PM 全 0、`rx=0` 不变；已恢复 `0x19c=1`、`0x2438=3`、`0x20=0`。
⇒ **路由方向正式排除**。

---

## 5. `fi_Configure_Hw_Init` 与驱动逐项比对

厂商完整写入序列（`xmac_writes.py 0x28fc 1348` + 反汇编 0x29c0..0x2e40）：
```
16000 |= 0x1f  →  21× __const_udelay(0x418958)   (≈21ms)
1650f  T-CONT 高半字 bfxil 0x3ff
fi_Configure_Pon_Mode(3)   [0x6 低4=3, 1688e=0x20, 1611d |=0x3ff|0xffc00]
1610b |= 0x80000
16100 |= 0x8
16644 byte0=2 / byte1=0x0f      (current_cputype==6/8)
Set_Upstream_Enable(0), Set_Downstream_Enable(0)
16100 byte0 |= 0x20
16600 byte0 |= 0x10
1689f byte0 |= 1
1a 清 bit2 / bit3
9  byte0 清 bit0 / byte2 清 bit0
16001 |= 1
16501 byte2: 置 bit0、清 bit1
16402 低14位 = 8
3000a |= 1
16635 |= 1
★ Hal_WriteGmacReg(0x1000, |= 0x10000)     ← 驱动完全没有
16644 PON-mode word 0x1b01 (XG-PON)
```

比对结论：
- 驱动 `zx_pon_hw_bringup()` 覆盖了绝大部分 XMAC 项。
- `xmac_writes.py` 报的 "`0x10`" 是对 `0x1650f`（T-CONT）的误识别，不是独立寄存器。
- `0x9`（现场 `0xc8`，byte0/byte2 bit0 均已清）、`0x1a`（byte0=0x50，bit2/3 已清）**现场已满足厂商期望**。
- `0x16003/5/6` 属 `fi_Configure_Interrupt_Init` 的事件映射组，只影响中断上报，不影响硬件搜帧。
- **仍存在的实质差异**：
  1. **时序**：厂商写一次 `0x16000 |= 0x1f` 后 **循环 21 次 `__const_udelay(0x418958)` ≈ 21ms**；
     驱动只有 `udelay(100)`（0.1ms），差约 200 倍。**此前"21 次循环持续写 0x1f"是误读**。
  2. **漏写 `Hal_WriteGmacReg(0x1000, |= 0x10000)`**（GMAC 侧，物理 = `gmac_base + 0x4000`）。
  3. `fi_Configure_Init` 后半段（`Set_Sn` / `Set_OnuState(1)` / `Interrupt_Init` / `hw_power_optx_set` /
     `pon_plat_pinctrl_set` / `Debug_Init` / 清 `0x6` bit8 / kthread）驱动未复刻。

`fi_Configure_Ponmac_Set_OnuState(1)` 写 `0x16500` 低5位=1；`fi_set_ONU_activation_state` 的 case1
还额外 `|= 0x3ff` 到 `0x16500` 高半字（现场 `0x03ff0001` 已满足）。

---

## 6. SDET PON FIFO / 完整初始化链重放：均无效

- `fi_Configure_Tmp_Disable_Downstream @0x239c` 在 LOF 路径通过
  `fpga_read/write_reg_one_module(0x815, ..., which=2)` 操作 `gamc_disable_sdetponfifo`（SDET PON FIFO）。
  `zx_ponreg_133.ko @0x8/@0xa8` 解出：`which` 只做路由/权限分支，**实际地址一律 `pon_base + idx*4`**；
  `fpga_module_init` 里 `sym6[0]=pon_base`、`sym6[0x10]=nppt_base`。
  ⇒ `0x815` = `pon_reg` 索引 `0x815`，现场 **0**（不是 NPPT+0x2054）。
- 按厂商两步写入 `m 815 1f 8` + `m 815 1ffc00 ffc00` → 读回 `0x000ffc08`，
  3s 后 `0x16105=0x10`、PM 全 0、`rx=0` 不变。
- 带 `sleep 1`（≫21ms）**完整重放 `fi_Configure_Hw_Init`** 全部 XMAC 项（含 `Set_OnuState`、清 bit8、
  末尾开下行），3s 后 `0x16105=0x10`、`16106..0a=0`、`5d0..5e6` 全 0、`rx=0` 不变。

⇒ **运行时寄存器重放无法让 framer 活过来**；问题更可能出在**驱动 probe 阶段的初始化顺序/时序**
（而非运行期补写），或需要驱动内的 TOPCRM/时钟链配合。

---

## 7. 本轮新增的"勿重复"清单

| 方向 | 结论 |
|---|---|
| FEC 使能 `NPPT+0x20080 bit0` / `0x16100 bits[3:2]` 8 组合 | 排除 |
| XMAC1 时钟 `0x2c0004 bit11` + `0x2c0018 bits[21:16]` | 现场已开，排除 |
| `ponserdes_to_xmac1_en_set` en=0/en=1（正确索引 `0x20`） | 排除 |
| `0x815` SDET PON FIFO 配置 | 排除 |
| 完整 `fi_Configure_Hw_Init` 带延时重放 | 排除 |
| `0x9` / `0x1a` / `0x16000` 自清 / `0x16105` 写清除 | 排除 |
| `low_power_api_nppt_clk_sw_ctrl_en_init` 阈值（参数多为 0，属动态调频） | 与搜帧无关 |

另：`0x815` / `0x20` 等 XMAC 索引与 NPPT 字节偏移**不是同一空间**，
`fpga_*_reg_one_module` 用 `pon_base + idx*4`，`reg/regw` 用 NPPT 字节偏移，勿混用。

---

## 8. ⚠️ 事故：`regw 0x4000 0x10000` 导致设备离线

为补 §5 的 GMAC 漏项，尝试 `regw 0x4000 0x10000`（推测 `gmac_base=NPPT`）：
- 写入瞬间 SSH **SIGTERM**；
- 之后 ping `192.168.9.1` **100% 丢包**、SSH 超时，累计 >5 分钟未恢复；
- PC 侧正常（以太网 `192.168.9.150`、链路 100M Up），排除 PC 网卡问题；
- COM4 (CH341) 存在但被其它进程独占，无法读 U-Boot 输出。

判断：`NPPT+0x4000` 不是普通 GMAC 配置寄存器，而是**复位/时钟门控类**，
写 `0x10000` 触发了比 `NPPT+0x2c0004 bit31` 更彻底的挂起（后者 1 分钟内能自恢复）。

**处置**：
- 该写入是**运行时 MMIO**，不落盘、不改固件/配置；**断电重启即可完全恢复**。
- 需要用户对 SK-D840N 做一次断电再上电（光纤保持插着）。
- 恢复后确认 `uptime`、`mapped=0`、boot log 仍是 preserve U-Boot 镜像，再 `on` / `force` 建基线。
- **禁止再写 `NPPT+0x4000`**，也禁止运行期写任何未验证的 NPPT 复位/门控寄存器。

---

## 9. 恢复后的下一步（按优先级）

1. **改驱动而不是运行期补写**：把厂商 `fi_Configure_Hw_Init` 的 **21ms 握手等待**
   （`0x16000 |= 0x1f` 后 21×1ms 延时）补进 `zx_pon_hw_bringup()`，用快路径
   `pon_revive/push/rebuild_kernel.sh` 重编 + 秒级刷机验证。这是本轮唯一有实证依据的时序差异。
2. GMAC `0x1000 |= 0x10000` 若要做，**只在驱动 probe 期**按厂商位置写（且先在串口可观测条件下），
   绝不再用运行时 `regw`。
3. 补 `fi_Configure_Interrupt_Init` 的 `0x16003/5/6` 事件映射（虽然不影响搜帧，但影响 PLOAM 上报）。
4. 若要继续查硬件通路，优先做 **TOPCRM `top_crm_base+0x60/+0x70` 复位/时钟门链**
   （厂商 `zx_pon_clk_reset_init`），需在驱动内加访问，当前 `soc` 白名单不含 `0x10e10000`。
5. 恢复后先跑 `pon_revive/fiber_check.py` 判读，再决定是否需要回滚到 `/root/mtd2-prearm.bin`。

---

## 10. 工具/映射速查（本轮新增）

```
red_set_fec_enable   → np_onu_reg_write(0x12,...) → NPPT+0x20080 bit0
RED/FEC 表           → .data+0x9c20 (28B/项: ident,+4 attr,+8 base,+0xc mask,+0x10 shift,+0x14 stride,+0x18 cnt)
                       index=base+which*stride; 物理 NPPT byte = index*4
auto_gate 表          → .data+0x27948 (attr 0x0b→0xb8, 0x0f→0x2c0018, 0x10→0x2c0004)
agc_set_reg_by_attr  → 0xWWWWSSSS: 低16=shift 高16=width
ponserdes_to_xmac1_en_set → pon_base+0x80 = pon_reg idx 0x20
Hal_WriteGmacReg(0x1000)  → gmac_base + 0x4000
fpga_*_reg_one_module(idx,which) → pon_base + idx*4 (which 只做路由)
```
