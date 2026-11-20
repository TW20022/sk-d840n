# 101. 那张 XG-PON 图像一直在 boot 分区里 —— 2026-11-20

这一轮把 PON SerDes 停在 SGMII 这件事查到底了，结论不是"没还原出来"，
而是**从来没有去找过**：板子自己的 U-Boot 就在 bootm 之前把这条 lane 配成
XG-PON，而那份 49 字图像是 boot 分区里的一段直线存储，不加密、不压缩、
不需要反编译工具以外的任何东西。

## 1. 两个二进制证据

`boot.img`（mtd1，2 MiB）里有两张表，都在 `.rodata` 里以字符串形式出现：

| 地址 | 内容 |
|---|---|
| `0x9f12b` | `mode_xgpon_nsyn_cfg_133` |
| `0x9f223` | `enter gpon pon pll cfg ` |
| `0x9f255` | `serdes init failed because the pll or cdr is not lock` |
| `0x9f28c` | `pon serdes init succeed` |
| `0x9cfd5` / `0x9cfe9` / `0x9f1fa` | `com_pll_lock_ready` / `rx los =0 rx data in` / `cdr_lock_ready` |

按 `adrp`+`add` 反查引用，这五条串各有一个写者：

- `0x528f4`：`mode_xgpon_nsyn_cfg_133` —— 49 个 `str`/`stur`，写
  `0x16000000..0x160000c0`；
- `0x5380c`：通用 serdes 初始化，按模式号跳 `0x937b4` 的 17 项表，
  **mode 6 → `0x528f4`**，随后写激活位、轮询 `0xd0` bit0、读 `0xe4` bit0、
  轮询 `0xe4` bit1；
- `0x53938`：PLL 配置，`sub w0,w19,#5; cmp #2` 走 **GPON 分支**，
  `sub w19,#8; cmp #8` 走 Ethernet 分支。

和 U-Boot 自己的日志逐行对得上（docs/92 §69.2）：

```
enter gpon pon pll cfg
mode_xgpon_nsyn_cfg_133
com_pll_lock_ready
rx los =0 rx data in
cdr_lock_ready
pon serdes init succeed
np init ok
```

也就是说：**这块板子在 bootm 之前，PON lane 跑的是 XG-PON，而且 RX 是通的**
（`rx los =0 rx data in` 是 U-Boot 自己读 `0x160000e4` bit0 打印的）。

## 2. 那份 49 字图像（mode 6）

从 `0x528f4` 的直线存储按顺序还原，`0x00..0xc0`：

```
0x00 0xe0000004  0x04 0x50a840a1  0x08 0x013e860f  0x0c 0x0210c073
0x10 0x00000048  0x14 0x00000000  0x18 0x00ff8000  0x1c 0x00238520
0x20 0x80000200  0x24 0x00030000  0x28 0x0c633830  0x2c 0x00000007
0x30 0x00000020  0x34 0x00002000  0x38 0x00000000  0x3c 0x00ff0000
0x40 0x05a85100  0x44 0x00000000  0x48 0x0f002b2a  0x4c 0x60002200
0x50 0x34000003  0x54 0x00000100  0x58 0x00000080  0x5c 0x00000000
0x60 0x200554a8  0x64 0x63f48091  0x68 0x01000000  0x6c 0x10cb3f00
0x70 0xa9404000  0x74 0x10670002  0x78 0x000000da  0x7c 0xf20000c8
0x80 0x103f103f  0x84 0x01000200  0x88 0x00020001  0x8c 0x10000000
0x90 0x0000001c  0x94 0x802dc100  0x98 0x000000ff  0x9c 0x55555500
0xa0 0x55555555  0xa4 0x00555555  0xa8 0x30000818  0xac 0x40002000
0xb0 0x0000000c  0xb4 0x01000000  0xb8 0x00000080  0xbc 0x00010000
0xc0 0x00000000
```

它与树里那张 SGMII 图像差 **25/49 字**，第一个字就不同
（`0x50a840a1` 对 `0x4fa840a3`）。这也解释了 docs/07 补五十七那次
`profilecheck.py 1g` 只有 21/49 相符的读数——现场根本不是那张图。

## 3. 驱动缺的三样东西

`phy-zx279133-pon-serdes.c` 之前只接受 SGMII / 2.5GBASE-X，于是
`&nppt { phy-mode = "sgmii" }` 一路把这条 lane 按 1G 配下去。真正缺的是：

1. **XG-PON 图像**（上表）；
2. **GPON 的 PLL 分频**。`TOPCRM_PON_PLL_SEL (0x10e10010) bits[5:4]`：
   vendor 对 GPON 写 **2**、对 Ethernet 写 **1**；驱动写的是 **0**，两个都不是。
   `PON_PLL_CFG0 (0x10e100c0)`：GPON `0x00202054`、Ethernet `0x20106454`；
3. **激活之后、轮询之前的两笔写**：`0xb4 = 0x00808020`、`0xb0 = 0x20200000`。
   它们不在任何 49 字图像里，所以对着图像做寄存器 diff 永远看不见。

顺序也是证据的一部分，vendor 是
`图像 → 0x90 bit14 → 0x40 bit15 → 0xb4 → 0xb0 → 0x54 bit0 → 轮询`，
驱动原来是 `图像 + 三个激活位`，中间那两笔没有。

## 4. 本轮改动

- 新增 `kernel-overlays/drivers/phy/zte/phy-zx279133-pon-serdes.c`
  （遮蔽上游同名文件）。加 `zx279133_pon_serdes_xgpon_nsyn_profile[]`
  （49 字，`static_assert` 卡长度）、`configure_pll(priv, gpon)`、
  `activate()`（按 vendor 顺序，含 0xb4/0xb0）、`struct` 里加 `bool xgpon`。
- 板级 dts：`&pon_serdes` 加 `zte,xgpon-nsyn;`。**没有**动 `&nppt` 的
  `phy-mode`，因为图像与 host interface 无关；没有这个属性时驱动行为与原来
  完全一致，别的板子不会误继承这张图。
- 注释里那句"xgpon_nsyn 不是选项，因为拿不到 49 字图像"已按事实改写。

## 5. 刷进去看什么

一条 dmesg 就能判：

```
zx279133-pon-serdes 16000000.phy: lane profile: XG-PON NSYN (board property)
zx279133-pon-serdes 16000000.phy: XG-PON NSYN SerDes ready, status 0x......../0x........
```

寄存器两条：

| 读数 | 期望 | 说明 |
|---|---|---|
| `0x16000004` | `0x50a840a1` | 图像真的换了；若是 `0x4fa840a3` 就是还在 SGMII |
| `0x10e10010` bits[5:4] | `2` | GPON 分频 |

状态：`cat /sys/class/net/eth0/pon_state`。**插着光纤**时，`0x160000e4`
bit0 应为 0（有光）、bit1 应为 1（CDR 锁），`pon_state` 应从 `O1` 往前走。
没插纤时 bit0 为 1 是正常的，这一列不能当判决。

## 6. 还没证的事

- 这条 lane 在 Linux 下跑 9.953G 之后，XPCS1 / XMAC1 仍按 `phy-mode = "sgmii"`
  配着（`zx279133-mac.c` 只认 SGMII / 2500BASEX）。PON MAC 的接收侧与 GEM
  数据面不经过这条 host PCS，所以注册（O5）该是独立的；但 eth0 的转发能不能
  同时工作，未知。
- `pon_state` 的前进还依赖 `mcode_intel.bin` 微码与 `zxpon.c` 的 PLOAM，
  这两块这一轮没动。
- 管理口在 `lan1..lan4`（`02_network` + `sk-bridge`），换 PON lane 的配置
  不会把 192.168.9.1 弄丢；`eth0` 只是 `wan` 槽位与兜底，这一条决定了刷机
  风险可控。
