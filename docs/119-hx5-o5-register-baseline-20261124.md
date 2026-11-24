# docs/119 — HX5-9saLite O5 态寄存器全量基线（SK-D840N 的答案卷）

- 日期：2026-11-24 08:14
- 采集：`pon_revive/hx5_o5_regdump.py`（telnet + devmem2，**全只读**）
- 原始数据：`pon_revive/hx5_o5_regdump_20261124_081416.txt`（15474 B）
- 现场条件：HX5-9saLite 插同一根入户纤，`sigpon -gstate` = **O5**，
  RXPower=129（≈ -18.7 dBm），PPPoE 在线，GEM/T-CONT 表活跃
- 遗留：telnet cmd=0 返回 Success 但端口 23 仍开（与 11-14 行为一致，
  断电自动复位，未写任何配置）

## 1. 为什么这是决定性数据

SK-D840N 的断点一直钉在「SerDes 并行输出 → SDET FIFO 入口之间」，
但从来没有任何一份"同一颗 ZX279133 在真实 O5 态"的寄存器参考。
HX5 与 SK 同平台（ZX279133）、同 BOSA（GN28L95）、同制式（upmode 0x40
XG-PON）、同 OLT 同纤——它的 O5 运行态就是 SK 驱动的标准答案。

## 2. PON MAC 块（0x17000000 + reg*4）— SK 驱动逐项对照

| reg | 物理地址 | HX5 O5 | SK 驱动现状 | 判定 |
|---|---|---|---|---|
| 0x17000080 (pon_route) | 0x17000080 | 0x00000001 | NPPT 每开机写入 | ✓ |
| 0x16000 (handshake) | 0x17058000 | **0x00000000** | bring-up 写 0x1f 后自清? | ⚠ 待比对 |
| 0x16100 (mode/FEC) | 0x17058400 | **0x0000063B** | rmw 0x28 + data bit0 | ⚠ 差 0x612 |
| 0x16105 (LOS/LOF) | 0x17058414 | **0x00000002** | SK 停 O1 时含 LOF 位 | ★目标值 |
| 0x1611d | 0x17058474 | **0x000FFFFF** | 写 0x3ff \| 0xffc00 = 同值 | ✓ |
| 0x16147 (RX pending) | 0x1705851C | 0x00000000 | 0（空闲时为 0 正常） | ✓ |
| 0x16148 (RX data) | 0x17058520 | 0x03FF0127 | — | 参考 |
| 0x1650f (T-CONT bank) | 0x1705943C | **0x00004044** | T-CONT0: bit14 valid + AllocID 68 | ★ |
| 0x16644 (PON mode word) | 0x17059910 | **0x00001B01** | bring-up 终值 0x1B01 | ✓ 完全一致 |
| 0x166c2 (PLOAM tx data) | 0x17059B08 | 0x00440933 | — | 参考 |
| 0x1688e (前导码) | 0x1705A238 | **0x00000020** | 写 0x20（XG-PON 分支） | ✓ |
| 0x2c0004 | 0x170B0010 | 0x07D0000C | SK 读过 bit11=1 | ⚠ SK 与 HX5 不同 |

关键确认：**MAC 侧 bring-up 序列的三个决定性寄存器（0x16644 / 0x1611d /
0x1688e）与 O5 基线完全一致**（docs/116 的 XG-PON 修正被真机证实）。
0x16100 的差距 = O5 多出 bits {1,4,9,10}（0x612），其中 bit9/10 可能是
FEC 相关；bit0（data en）由 zx_pon_hw_data(true) 设置。

## 3. SDET PON FIFO 计数器（NPPT 0x19000000+0x2440..）— 断点验证

| 地址 | HX5 O5 | SK |
|---|---|---|
| 0x19002440 | **0x08040804** | 0x00000000 |
| 0x19002444 | **0x08040804** | 0x00000000 |
| 0x19002448 | 0x00000000 | 0x00000000 |
| 0x1900244C | 0x00000000 | 0x00000000 |
| 0x19002450 | **0x00000002** | 0x00000000 |

★ O5 时 FIFO 计数器非零（0x08040804 / 0x2）——SK 全 0 再次证实
数据没进 FIFO；同时给出了"通了长什么样"的参照值。

## 4. XMAC（XMAC0=0x19140000 / XMAC1=0x19180000）

- XMAC0 TX_CTRL = **0xC0010001** → bit31:29 速率码 **6（2.5G）**，
  与 HX5 的 yt8821 2.5G LAN 一致（vendor 表：3=1G/4=100M/6=2.5G/7=10M）。
  0xC0010001 vs SK LAN 场景的 0x80010001（码 4）——只是 LAN 速率不同，
  非 PON 问题。w4-w7 = 0x3E8000C7（计数器类）。
- XMAC1 窗口（0x19180000）**全 0** —— SK 的"XMAC1 时钟嫌疑"
  （0x2c0004 bit11）在 HX5 上反而是 0，进一步排除该路径。

## 5. PON SerDes lane 块（0x16000000，49 词）— 镜像逐词 diff

```
MATCH: 48/49 词与 zx279133_pon_serdes_xgpon_nsyn_profile[] 完全一致
唯一差异: w21 (0x16000054)
  镜像/引导器 = 0x00000100
  HX5 O5 运行态 = 0x00000101   ← bit0 被置位
```

含义：**vendor 初始化在镜像写完后把 lane word21 的 bit0 置 1**。
该 bit 的性质待定（PCS/MAC 交接使能，或链路建立后的状态位）。
注意：`apply_xgpon_runtime.sh` / `restore_pon.sh` 在旧上下文（MAC 序列
修正前）写过 0x101 没打通——但当时 0x16644 还是错的 0x1301。
**在当前已验证正确的 MAC 序列下重验 w21 bit0 是首要实验。**

PLL_STATUS(0xd0)/RX_STATUS(0xe4) 也一并抓了（见原始报告）。

## 6. /proc 与工具层（对照记录）

- `sigpon -gstate` → O5
- `opticaltst -getpara_g` → temp=11941, V=33438, TXBias=14356,
  TXPower=25452, **RXPower=129**
- `up_mode_info` → g_up_mode=0x40, g_switch_ponmode=0x50,
  g_is_auto_ponmode=0, g_multi_bosa_en=0x0
- `/proc/net/xpon/entry` → 与 11-16 GEM-HARVEST 相同：VLAN99→GEM1348→TCONT1，
  另有 GEM 1092/1604/1860/2116/2372，untag 65535
- GEM 间接表：gem_addr=0x444(=1092)、gem_data=0x02（PRESENT 位）——
  O5 时活跃 GEM 的表项值 = 0x02，与我们在 SK 上写的值一致 ✓

## 7. SK 侧下一步（按优先级）

1. **lane snapshot diff**（SK 回来后）：`lane snapshot` 49 词 vs 本基线；
   若 w21 = 0x100 则在正确 MAC 序列下补 bit0。
2. **0x16100 补位**：对比 SK 值与 0x63B，逐 bit 补（先 bits 9/10）。
3. **SDET FIFO 计数器**作为全程探针：任何改动后看 0x19002440 是否离开 0。
4. stock boot 重跑（watch_reset_boot.py 已修正 ttyAMA0 + UpModeInfo=0x40）：
   在 SK 硬件上直接看 vendor 全过程。
5. T-CONT0 AllocID：O5 时 OLT 实际分配 68（bit14 valid），印证 docs/108
   的 bank 格式（AllocID 是会话相关的，不可照抄数值，只可校验格式）。

## 8. 采集方法备忘（可复用）

- telnet 开关：`http://192.168.1.1/usr=CMCCAdmin&psw=aDm8H%25MdA&cmd=1&telnet.gch`
- 登录：CMCCAdmin / aDm8H%MdA，然后 `su`（密码同）
- telnetd 会折行 >60 字符命令 → 每条命令 <60 字符单独发
- devmem2 只读 = 只给地址参数；物理地址 = 0x17000000 + reg*4（PON MAC）
- 关闭：cmd=0（本机实测返回 Success 但不生效，断电自复位）
