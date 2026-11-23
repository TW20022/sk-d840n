// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define ZX279133_TOPCRM_PON_MUX_CTRL		0x0c
#define ZX279133_TOPCRM_PON_PLL_SEL		0x10
#define ZX279133_TOPCRM_PON_ETH_SEL		BIT(9)
#define ZX279133_TOPCRM_PON_MODE_MASK		GENMASK(9, 8)
#define ZX279133_TOPCRM_PON_PLL_SEL_MASK	GENMASK(5, 4)

#define ZX279133_PON_PLL_CFG0			0x0
#define ZX279133_PON_PLL_CFG1			0x4
#define ZX279133_PON_PLL_CFG0_ETH		0x20106454
#define ZX279133_PON_PLL_CFG0_FW_BIT		BIT(30)
#define ZX279133_PON_PLL_CFG1_LATCH		BIT(28)
#define ZX279133_PON_PLL_CFG1_ETH		0x04000000
/*
 * The two constants above are the vendor's own Ethernet branch: np_133.ko's
 * pon_pll_cfg() takes the 0x20106454/0x04000000 pair (and PLL ref select 0)
 * for modes 8..15, which are the 10G/5G/2.5G/1G BASE-R and BASE-X Ethernet
 * images.  Its PON branch - modes 5..7, i.e. GPON and both XG-PON images - is
 * a different pair with ref select 2:
 *
 *	mov w1, #0x2054 ; movk w1, #0x20   -> TOPCRM+0xc0 = 0x00202054
 *	mov w1, #0x73e2 ; movk w1, #0x426  -> TOPCRM+0xc4 = 0x042673e2
 *	and w1, w1, #0xffffffcf ; orr #0x20 -> +0x10 bits 5:4 = 2
 *
 * and it finishes by pulsing +0x0c bit 9 rather than leaving a mode field
 * set.  Drawing 9.95328 GHz out of a PLL programmed for 10.3125 GHz is a
 * 3.6% rate error, which is enough for the CDR to report lock while every
 * 66-bit block the PCS sees is wrong.
 */
#define ZX279133_PON_PLL_CFG0_PON		0x00202054
#define ZX279133_PON_PLL_CFG1_PON		0x042673e2
#define ZX279133_PON_PLL_REF_SEL_PON		2
#define ZX279133_TOPCRM_PON_PLL_LOAD		BIT(9)

#define ZX279133_PON_SERDES_ACTIVATE0		0x40
#define ZX279133_PON_SERDES_ACTIVATE0_EN	BIT(15)
#define ZX279133_PON_SERDES_ACTIVATE1		0x54
#define ZX279133_PON_SERDES_ACTIVATE1_EN	BIT(0)
#define ZX279133_PON_SERDES_MODE_CTRL		0x90
#define ZX279133_PON_SERDES_MODE_MASK		GENMASK(14, 13)
#define ZX279133_PON_SERDES_MODE_EN		BIT(14)
/*
 * Two registers the vendor's generic serdes init writes after the image and
 * the MODE/ACTIVATE bits.  They are inside the PON lane's own 0x16000000
 * window, not the shared TOPCRM clock block, so they cannot disturb the
 * Ethernet reference clock.  Only the XG-PON board image applies them.
 */
#define ZX279133_PON_SERDES_POST_B0		0xb0
#define ZX279133_PON_SERDES_POST_B0_VALUE	0x20200000
#define ZX279133_PON_SERDES_POST_B4		0xb4
#define ZX279133_PON_SERDES_POST_B4_VALUE	0x00808020
#define ZX279133_PON_SERDES_PLL_STATUS		0xd0
#define ZX279133_PON_SERDES_PLL_LOCK		BIT(0)
#define ZX279133_PON_SERDES_RX_STATUS		0xe4
#define ZX279133_PON_SERDES_RX_LOS		BIT(0)
#define ZX279133_PON_SERDES_CDR_LOCK		BIT(1)

/*
 * AN1 PLL: the PON reference clock.  np_133.ko's pon_serdes_init() calls
 * an1_pll_clk_set(6) -> an1_pll_gpon_cfg() *before* serdes_mode_set() writes
 * the 49-word lane image.  The two DTS nodes (pon_serdes@16000000 and
 * pon_serdes_pll@16010000) are 64K apart but A[16] is not decoded on this
 * SoC, so they alias: the vendor writes the PLL recipe first, latches it,
 * then lets the lane image overwrite words 0..7.  The PLL keeps its
 * configuration internally.
 *
 * an1_pll_gpon_cfg() differs from the Ethernet AN1 setup in exactly two
 * places: +0x08 0x01050700 (not ...707) and +0x1c 0x00130000 (not
 * 0x00130200).  Those select the 155.52 MHz PON reference instead of the
 * 156.25 MHz Ethernet one.  Without it the CDR locks to the 9.95328G line
 * but the PCS deskew runs on a 3.6%-wrong parallel clock: XPCS1 never gets
 * block lock, the XMAC sits in LOF (0x16105 = 0x10) and rx stays at zero.
 */
#define ZX279133_AN1_PLL_STATUS			0x20
#define ZX279133_AN1_PLL_LOCK			BIT(0)
#define ZX279133_AN1_PLL_LATCH			BIT(0)
#define ZX279133_AN1_PLL_POLL_US		1000
#define ZX279133_AN1_PLL_LOCK_TIMEOUT_US	200000

static const u32 zx279133_an1_pll_gpon_cfg[8] = {
	0x2af8c074, 0x447424a0, 0x01050700, 0x00000107,
	0x00003100, 0x4000003a, 0x00000002, 0x00130000,
};

#define ZX279133_PON_SERDES_RESET_US		10000
#define ZX279133_PON_SERDES_PROFILE_WORDS	49
#define ZX279133_PON_SERDES_POLL_US		2000
#define ZX279133_PON_SERDES_LOCK_TIMEOUT_US	2000000

struct zx279133_pon_serdes {
	struct device *dev;
	void __iomem *serdes;
	void __iomem *pll;
	void __iomem *topcrm_mode;
	struct regmap *topcrm;
	struct clk *pclk;
	struct reset_control_bulk_data resets[2];
	/* Protects interface selection and the powered/configured lifecycle. */
	struct mutex lock;
	phy_interface_t interface;
	u32 saved_mux;
	u32 saved_pll_sel;
	u32 saved_pll_cfg0;
	u32 saved_pll_cfg1;
	bool configured;
	bool powered;
	/* Board image: PON lane only. Never changes the shared clock path. */
	bool xgpon;
};

/* CPU133 mode 9 and mode 15 share this recovered 2.5GBASE-X profile. */
static const u32 zx279133_pon_serdes_2500basex_profile[] = {
	0xe0000004, 0x4fa8c0a2, 0x013e8604, 0x1f51c8f3,
	0x00000044, 0x00194000, 0x0b080000, 0x00238220,
	0x80000100, 0x00050000, 0x0c633830, 0x00000007,
	0x00000020, 0x00002000, 0x00000000, 0x00ff0000,
	0x05a85100, 0x00000000, 0x04005b6a, 0x60002220,
	0x34000003, 0x00000408, 0x00000080, 0x00000000,
	0x200574a8, 0x00649052, 0x01000000, 0x40007700,
	0xa9004000, 0x01108002, 0x000000da, 0xf20000c8,
	0x10371037, 0x01000200, 0x00020001, 0x10000000,
	0x0000001c, 0x802dc000, 0x000000ff, 0x55555500,
	0x55555555, 0x00555555, 0x30000818, 0x40002000,
	0x0000000c, 0x01000000, 0x00000080, 0x00010000,
	0x00000000,
};

/*
 * CPU133 Uni mode 7 (1G SGMII) maps to PON SerDes mode 8 through
 * uni_eth_mode_change(); this is the recovered 49-word mode-8 profile from
 * uni_mode_eth_1gbase_x_cfg().
 */
static const u32 zx279133_pon_serdes_sgmii_profile[] = {
	0xe0000004, 0x4fa840a3, 0x013e860f, 0x0210c073,
	0x00000048, 0x00000000, 0x00ff8000, 0x00238620,
	0x80000000, 0x00050000, 0x0c633830, 0x00000007,
	0x00000020, 0x00002000, 0x00000000, 0x00ff0000,
	0xc5a85100, 0x00000000, 0x01045b2a, 0x66002200,
	0x34000003, 0x00000100, 0x00000080, 0x00000000,
	0x200554a8, 0x00648091, 0x01000000, 0x00003700,
	0xa9004000, 0x10640002, 0x000000da, 0xf20000c8,
	0x10371037, 0x01000200, 0x00020001, 0x10000000,
	0x0000001c, 0x8000c000, 0x000000ff, 0x55555500,
	0x55555555, 0x00555555, 0x30000818, 0x40002000,
	0x0000000c, 0x01000000, 0x00000080, 0x00010000,
	0x00000000,
};

/*
 * CPU133 mode 6, the XG-PON NSYN image, as written by this board's own
 * U-Boot (mode table entry 6 -> mode_xgpon_nsyn_cfg_133).  Recovered from the
 * bootloader's straight-line stores to 0x16000000..0x160000c0, in order.
 *
 * This changes the PON lane only.  It is selected by the board property
 * "zte,xgpon-nsyn" and is the one image the optical lane is meant to run.
 * It differs from SGMII in 25 of 49 words; word 1 alone (0x50a840a1 against
 * 0x4fa840a3) is a different PLL/post-cursor setting that no host interface
 * choice can express.
 */
static const u32 zx279133_pon_serdes_xgpon_nsyn_profile[] = {
	0xe0000004, 0x50a840a1, 0x013e860f, 0x0210c073,
	0x00000048, 0x00000000, 0x00ff8000, 0x00238520,
	0x80000200, 0x00030000, 0x0c633830, 0x00000007,
	0x00000020, 0x00002000, 0x00000000, 0x00ff0000,
	0x05a85100, 0x00000000, 0x0f002b2a, 0x60002200,
	0x34000003, 0x00000100, 0x00000080, 0x00000000,
	0x200554a8, 0x63f48091, 0x01000000, 0x10cb3f00,
	0xa9404000, 0x10670002, 0x000000da, 0xf20000c8,
	0x103f103f, 0x01000200, 0x00020001, 0x10000000,
	0x0000001c, 0x802dc100, 0x000000ff, 0x55555500,
	0x55555555, 0x00555555, 0x30000818, 0x40002000,
	0x0000000c, 0x01000000, 0x00000080, 0x00010000,
	0x00000000,
};

static_assert(ARRAY_SIZE(zx279133_pon_serdes_2500basex_profile) ==
	      ZX279133_PON_SERDES_PROFILE_WORDS);
static_assert(ARRAY_SIZE(zx279133_pon_serdes_sgmii_profile) ==
	      ZX279133_PON_SERDES_PROFILE_WORDS);
static_assert(ARRAY_SIZE(zx279133_pon_serdes_xgpon_nsyn_profile) ==
	      ZX279133_PON_SERDES_PROFILE_WORDS);

static const u32 *zx279133_pon_serdes_profile(struct zx279133_pon_serdes *priv)
{
	if (priv->xgpon)
		return zx279133_pon_serdes_xgpon_nsyn_profile;

	switch (priv->interface) {
	case PHY_INTERFACE_MODE_SGMII:
		return zx279133_pon_serdes_sgmii_profile;
	case PHY_INTERFACE_MODE_2500BASEX:
		return zx279133_pon_serdes_2500basex_profile;
	default:
		return NULL;
	}
}

static const char *zx279133_pon_serdes_mode_name(struct zx279133_pon_serdes *priv)
{
	if (priv->xgpon)
		return "XG-PON NSYN";

	switch (priv->interface) {
	case PHY_INTERFACE_MODE_SGMII:
		return "SGMII";
	case PHY_INTERFACE_MODE_2500BASEX:
		return "2.5GBASE-X";
	default:
		return "unknown";
	}
}

/*
 * Rebuild the AN1 PLL PON reference after the SerDes resets.  U-Boot already
 * did this once, but our reset_control_bulk_assert/deassert clears it, and
 * writing only the 49-word lane image leaves the PCS on the Ethernet
 * 156.25 MHz reference.  This is the one step our power_on() was missing.
 *
 * The vendor treats a missing lock as non-fatal (it only printk's), so do
 * the same: a timeout must not prevent the lane image from being loaded.
 */
static void zx279133_pon_serdes_configure_an1_pll(
	struct zx279133_pon_serdes *priv)
{
	u32 val;
	int i, ret;

	if (!priv->xgpon)
		return;

	for (i = 0; i < ARRAY_SIZE(zx279133_an1_pll_gpon_cfg); i++)
		writel(zx279133_an1_pll_gpon_cfg[i],
		       priv->pll + i * sizeof(u32));

	/* an1_pll_gpon_cfg() writes 0x00003100 then sets bit0 of +0x10. */
	val = readl(priv->pll + 0x10);
	writel(val | ZX279133_AN1_PLL_LATCH, priv->pll + 0x10);

	ret = readl_poll_timeout(priv->pll + ZX279133_AN1_PLL_STATUS,
				 val, val & ZX279133_AN1_PLL_LOCK,
				 ZX279133_AN1_PLL_POLL_US,
				 ZX279133_AN1_PLL_LOCK_TIMEOUT_US);
	if (ret)
		dev_warn(priv->dev,
			 "AN1 PLL (155.52M PON ref) no lock, status %#x\n", val);
	else
		dev_info(priv->dev,
			 "AN1 PLL locked on 155.52M PON ref, status %#x\n", val);
}

static void zx279133_pon_serdes_write_profile(struct zx279133_pon_serdes *priv,
					      const u32 *profile, size_t count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		writel(profile[i], priv->serdes + i * sizeof(u32));

	writel((readl(priv->serdes + ZX279133_PON_SERDES_MODE_CTRL) &
		~ZX279133_PON_SERDES_MODE_MASK) | ZX279133_PON_SERDES_MODE_EN,
	       priv->serdes + ZX279133_PON_SERDES_MODE_CTRL);
	writel(readl(priv->serdes + ZX279133_PON_SERDES_ACTIVATE0) |
	       ZX279133_PON_SERDES_ACTIVATE0_EN,
	       priv->serdes + ZX279133_PON_SERDES_ACTIVATE0);
	writel(readl(priv->serdes + ZX279133_PON_SERDES_ACTIVATE1) |
	       ZX279133_PON_SERDES_ACTIVATE1_EN,
	       priv->serdes + ZX279133_PON_SERDES_ACTIVATE1);

	/* The vendor's generic init adds these two after activation.  Both are
	 * inside the PON lane window; the shared TOPCRM clock is untouched.
	 */
	if (priv->xgpon) {
		writel(ZX279133_PON_SERDES_POST_B4_VALUE,
		       priv->serdes + ZX279133_PON_SERDES_POST_B4);
		writel(ZX279133_PON_SERDES_POST_B0_VALUE,
		       priv->serdes + ZX279133_PON_SERDES_POST_B0);
	}
}

static int zx279133_pon_serdes_configure_pll(struct zx279133_pon_serdes *priv)
{
	u32 cfg0, cfg1, val;
	int ret;

	ret = regmap_read(priv->topcrm, ZX279133_TOPCRM_PON_MUX_CTRL,
			  &priv->saved_mux);
	if (ret)
		return ret;

	ret = regmap_read(priv->topcrm, ZX279133_TOPCRM_PON_PLL_SEL,
			  &priv->saved_pll_sel);
	if (ret)
		return ret;

	priv->saved_pll_cfg0 =
		readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG0);
	priv->saved_pll_cfg1 =
		readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG1);
	priv->configured = true;

	if (priv->xgpon) {
		/*
		 * XG-PON wants the PON branch of the vendor's table, not the
		 * Ethernet one: 9.95328 GHz comes out of 0x00202054/0x042673e2
		 * with the PON reference selected, and the PLL is reloaded by a
		 * pulse on +0x0c bit 9 instead of a static mode field.
		 */
		cfg0 = ZX279133_PON_PLL_CFG0_PON;
		cfg1 = ZX279133_PON_PLL_CFG1_PON;
		ret = regmap_update_bits(priv->topcrm,
					 ZX279133_TOPCRM_PON_PLL_SEL,
					 ZX279133_TOPCRM_PON_PLL_SEL_MASK,
					 ZX279133_PON_PLL_REF_SEL_PON << 4);
		if (ret)
			return ret;

		writel(readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG1) |
		       ZX279133_PON_PLL_CFG1_LATCH,
		       priv->topcrm_mode + ZX279133_PON_PLL_CFG1);
		writel(cfg0, priv->topcrm_mode + ZX279133_PON_PLL_CFG0);
		writel(cfg1, priv->topcrm_mode + ZX279133_PON_PLL_CFG1);

		ret = regmap_update_bits(priv->topcrm,
					 ZX279133_TOPCRM_PON_MUX_CTRL,
					 ZX279133_TOPCRM_PON_MODE_MASK, 0);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->topcrm,
					 ZX279133_TOPCRM_PON_MUX_CTRL,
					 ZX279133_TOPCRM_PON_PLL_LOAD,
					 ZX279133_TOPCRM_PON_PLL_LOAD);
		if (ret)
			return ret;
		udelay(2);
		return regmap_update_bits(priv->topcrm,
					  ZX279133_TOPCRM_PON_MUX_CTRL,
					  ZX279133_TOPCRM_PON_PLL_LOAD, 0);
	}

	ret = regmap_update_bits(priv->topcrm, ZX279133_TOPCRM_PON_PLL_SEL,
				 ZX279133_TOPCRM_PON_PLL_SEL_MASK, 0);
	if (ret)
		return ret;

	writel(readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG1) |
	       ZX279133_PON_PLL_CFG1_LATCH,
	       priv->topcrm_mode + ZX279133_PON_PLL_CFG1);
	val = readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG0);
	val &= ZX279133_PON_PLL_CFG0_FW_BIT;
	val |= ZX279133_PON_PLL_CFG0_ETH;
	writel(val, priv->topcrm_mode + ZX279133_PON_PLL_CFG0);
	writel(ZX279133_PON_PLL_CFG1_ETH,
	       priv->topcrm_mode + ZX279133_PON_PLL_CFG1);

	return regmap_update_bits(priv->topcrm, ZX279133_TOPCRM_PON_MUX_CTRL,
				  ZX279133_TOPCRM_PON_MODE_MASK,
				  ZX279133_TOPCRM_PON_ETH_SEL);
}

static void zx279133_pon_serdes_restore_pll(struct zx279133_pon_serdes *priv)
{
	if (!priv->configured)
		return;

	regmap_update_bits(priv->topcrm, ZX279133_TOPCRM_PON_MUX_CTRL,
			   ZX279133_TOPCRM_PON_MODE_MASK,
			   priv->saved_mux & ZX279133_TOPCRM_PON_MODE_MASK);
	regmap_update_bits(priv->topcrm, ZX279133_TOPCRM_PON_PLL_SEL,
			   ZX279133_TOPCRM_PON_PLL_SEL_MASK,
			   priv->saved_pll_sel & ZX279133_TOPCRM_PON_PLL_SEL_MASK);
	writel(priv->saved_pll_cfg0,
	       priv->topcrm_mode + ZX279133_PON_PLL_CFG0);
	writel(priv->saved_pll_cfg1,
	       priv->topcrm_mode + ZX279133_PON_PLL_CFG1);
	priv->configured = false;
}

static int zx279133_pon_serdes_reset(struct zx279133_pon_serdes *priv)
{
	int ret;

	ret = reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
	if (ret)
		return ret;

	usleep_range(ZX279133_PON_SERDES_RESET_US,
		     ZX279133_PON_SERDES_RESET_US + 1000);

	ret = reset_control_bulk_deassert(ARRAY_SIZE(priv->resets), priv->resets);
	if (ret) {
		reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
		return ret;
	}

	usleep_range(ZX279133_PON_SERDES_RESET_US,
		     ZX279133_PON_SERDES_RESET_US + 1000);
	return 0;
}

static int zx279133_pon_serdes_set_mode(struct phy *phy, enum phy_mode mode,
					int submode)
{
	struct zx279133_pon_serdes *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET)
		return -EOPNOTSUPP;

	if (submode != PHY_INTERFACE_MODE_2500BASEX &&
	    submode != PHY_INTERFACE_MODE_SGMII)
		return -EOPNOTSUPP;

	mutex_lock(&priv->lock);
	priv->interface = submode;
	mutex_unlock(&priv->lock);

	return 0;
}

static int zx279133_pon_serdes_power_on(struct phy *phy)
{
	struct zx279133_pon_serdes *priv = phy_get_drvdata(phy);
	const u32 *profile;
	u32 val;
	int ret;

	mutex_lock(&priv->lock);
	if (priv->powered) {
		ret = 0;
		goto out_unlock;
	}

	profile = zx279133_pon_serdes_profile(priv);
	if (!profile) {
		dev_err(priv->dev, "unsupported SerDes interface %s\n",
			phy_modes(priv->interface));
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = clk_prepare_enable(priv->pclk);
	if (ret)
		goto out_unlock;

	ret = zx279133_pon_serdes_configure_pll(priv);
	if (ret)
		goto err_disable_clk;

	dev_info(priv->dev,
		 "PON PLL %s: cfg0 %#x -> %#x, cfg1 %#x -> %#x, mux %#x\n",
		 priv->xgpon ? "PON branch" : "Ethernet branch",
		 priv->saved_pll_cfg0,
		 readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG0),
		 priv->saved_pll_cfg1,
		 readl(priv->topcrm_mode + ZX279133_PON_PLL_CFG1),
		 priv->saved_mux);

	if (priv->xgpon) {
		/*
		 * U-Boot already ran the full pon_serdes_init() for this board
		 * and logged "enter gpon pon pll cfg" -> mode_xgpon_nsyn_cfg_133
		 * -> com_pll_lock_ready -> "rx los =0 rx data in" ->
		 * cdr_lock_ready -> "pon serdes init succeed".  That sequence
		 * also owns the AN1 PON reference, the XPCS1 10GBASE-R setup
		 * and the XMAC1 FIFO/clock gates, none of which this driver
		 * has a recipe for.  Asserting the SerDes resets on every Linux
		 * boot wipes all of it; reloading only the 49-word lane image
		 * (even with the AN1 PLL recipe added) left the framer in LOF
		 * (0x16105=0x10), superframe counter frozen and rx=0.  Keep the
		 * bootloader's known-good state and only verify the lane is
		 * alive with the read-only polls below.
		 */
		dev_info(priv->dev,
			 "XG-PON: preserving U-Boot SerDes state (no reset/reload)\n");
	} else {
		ret = zx279133_pon_serdes_reset(priv);
		if (ret)
			goto err_disable_clk;

		zx279133_pon_serdes_configure_an1_pll(priv);
		zx279133_pon_serdes_write_profile(priv, profile,
			ZX279133_PON_SERDES_PROFILE_WORDS);
	}

	ret = readl_poll_timeout(priv->serdes + ZX279133_PON_SERDES_PLL_STATUS,
				 val, val & ZX279133_PON_SERDES_PLL_LOCK,
				 ZX279133_PON_SERDES_POLL_US,
				 ZX279133_PON_SERDES_LOCK_TIMEOUT_US);
	if (ret)
		dev_warn(priv->dev,
			 "common PLL status did not assert in %s mode\n",
			 zx279133_pon_serdes_mode_name(priv));

	val = readl(priv->serdes + ZX279133_PON_SERDES_RX_STATUS);
	if (!(val & ZX279133_PON_SERDES_RX_LOS)) {
		ret = readl_poll_timeout(priv->serdes + ZX279133_PON_SERDES_RX_STATUS,
					 val, val & ZX279133_PON_SERDES_CDR_LOCK,
					 ZX279133_PON_SERDES_POLL_US,
					 ZX279133_PON_SERDES_LOCK_TIMEOUT_US);
		if (ret) {
			dev_err(priv->dev, "CDR did not lock with RX signal present\n");
			goto err_assert_reset;
		}
	}

	dev_info(priv->dev, "%s SerDes ready, status %#x/%#x\n",
		 zx279133_pon_serdes_mode_name(priv),
		 readl(priv->serdes + ZX279133_PON_SERDES_PLL_STATUS),
		 readl(priv->serdes + ZX279133_PON_SERDES_RX_STATUS));
	priv->powered = true;
	ret = 0;
	goto out_unlock;

err_assert_reset:
	reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
err_disable_clk:
	zx279133_pon_serdes_restore_pll(priv);
	clk_disable_unprepare(priv->pclk);
out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int zx279133_pon_serdes_power_off(struct phy *phy)
{
	struct zx279133_pon_serdes *priv = phy_get_drvdata(phy);
	int ret = 0;

	mutex_lock(&priv->lock);
	if (!priv->powered)
		goto out_unlock;

	ret = reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
	if (ret)
		goto out_unlock;

	zx279133_pon_serdes_restore_pll(priv);
	clk_disable_unprepare(priv->pclk);
	priv->powered = false;

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static void zx279133_pon_serdes_cleanup(void *data)
{
	struct zx279133_pon_serdes *priv = data;

	mutex_lock(&priv->lock);
	if (priv->powered) {
		reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
		zx279133_pon_serdes_restore_pll(priv);
		clk_disable_unprepare(priv->pclk);
		priv->powered = false;
	}
	mutex_unlock(&priv->lock);
}

static const struct phy_ops zx279133_pon_serdes_ops = {
	.set_mode = zx279133_pon_serdes_set_mode,
	.power_on = zx279133_pon_serdes_power_on,
	.power_off = zx279133_pon_serdes_power_off,
	.owner = THIS_MODULE,
};

static int zx279133_pon_serdes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct zx279133_pon_serdes *priv;
	struct phy *phy;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);

	/* Board property selects the optical lane's image.  It affects only the
	 * 49-word image written into the PON lane at 0x16000000; the shared
	 * TOPCRM PLL path in configure_pll() is intentionally left untouched.
	 */
	priv->xgpon = device_property_read_bool(dev, "zte,xgpon-nsyn");
	dev_info(dev, "PON SerDes image: %s\n",
		 zx279133_pon_serdes_mode_name(priv));

	priv->serdes = devm_platform_ioremap_resource_byname(pdev, "serdes");
	if (IS_ERR(priv->serdes))
		return PTR_ERR(priv->serdes);

	priv->pll = devm_platform_ioremap_resource_byname(pdev, "pll");
	if (IS_ERR(priv->pll))
		return PTR_ERR(priv->pll);

	priv->topcrm_mode =
		devm_platform_ioremap_resource_byname(pdev, "topcrm-mode");
	if (IS_ERR(priv->topcrm_mode))
		return PTR_ERR(priv->topcrm_mode);

	priv->topcrm = syscon_regmap_lookup_by_phandle(dev->of_node, "zte,topcrm");
	if (IS_ERR(priv->topcrm))
		return dev_err_probe(dev, PTR_ERR(priv->topcrm),
				     "failed to get TOPCRM syscon\n");

	priv->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(priv->pclk))
		return dev_err_probe(dev, PTR_ERR(priv->pclk),
				     "failed to get PCLK\n");

	priv->resets[0].id = "serdes";
	priv->resets[1].id = "apb";
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(priv->resets),
						    priv->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get resets\n");

	ret = devm_add_action_or_reset(dev, zx279133_pon_serdes_cleanup, priv);
	if (ret)
		return ret;

	phy = devm_phy_create(dev, NULL, &zx279133_pon_serdes_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy), "failed to create PHY\n");

	phy_set_drvdata(phy, priv);
	platform_set_drvdata(pdev, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id zx279133_pon_serdes_of_match[] = {
	{ .compatible = "zte,zx279133-pon-serdes" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx279133_pon_serdes_of_match);

static struct platform_driver zx279133_pon_serdes_driver = {
	.probe = zx279133_pon_serdes_probe,
	.driver = {
		.name = "zx279133-pon-serdes",
		.of_match_table = zx279133_pon_serdes_of_match,
	},
};
module_platform_driver(zx279133_pon_serdes_driver);

MODULE_DESCRIPTION("ZTE ZX279133 PON SerDes PHY driver");
MODULE_LICENSE("GPL");
