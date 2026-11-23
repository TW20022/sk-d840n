// SPDX-License-Identifier: GPL-2.0-only
/*
 * XG-PON activation glue for the ZX279133 WAN netdev.
 *
 * The 49-word xgpon_nsyn image is still not programmed: the vendor modules
 * on hand have mode_xgpon_nsyn_cfg, not mode_xgpon_nsyn_cfg_133.  Everything
 * this file writes is named by a Hal_WriteXmacReg trace disassembled from
 * zte_xgpon.ko (4.19.136) and decoded in docs/106, and stays inside the
 * NPPT window the driver already maps.
 *
 * The software activation state machine used to have no hardware layer at
 * all: it could walk to O2-3 in memory while the PON MAC registers were all
 * zero.  This file now brings up the XG-PON MAC, writes the ONU serial into
 * the hardware register, and polls the vendor PLOAM RX/TX windows from the
 * existing 100 ms tick, feeding received frames into zxpon_rx_ploam() and
 * draining zxpon's TX queue into the MAC.  GEM/T-CONT provisioning and OMCI
 * over the data GEM are still separate work.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/swab.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include "zx279133.h"
#include "zxpon.h"

static const struct nla_policy zxpon_policy[ZXPON_A_MAX] = {
	[ZXPON_A_STATE] = { .type = NLA_NUL_STRING },
	[ZXPON_A_SN] = NLA_POLICY_EXACT_LEN(ZXPON_SN_LEN),
	[ZXPON_A_REGID] = NLA_POLICY_EXACT_LEN(ZXPON_REGID_LEN),
	[ZXPON_A_ALLOC] = { .type = NLA_U16 },
	[ZXPON_A_GEM] = { .type = NLA_U16 },
	[ZXPON_A_OMCC] = { .type = NLA_U8 },
	[ZXPON_A_KEY] = NLA_POLICY_EXACT_LEN(ZXPON_KEY_LEN),
	[ZXPON_A_OMCI_FRAME] = NLA_POLICY_EXACT_LEN(ZXPON_OMCI_LEN),
	[ZXPON_A_PLOAM_FRAME] = NLA_POLICY_EXACT_LEN(ZXPON_PLOAM_LEN),
};

static struct zxpon zx_state;
static DEFINE_MUTEX(zx_pon_lock);
static struct net_device *zx_wan;
static struct net_device *zx_omci;
static struct led_trigger *zx_green;

/*
 * This board's optical RX signal detect (opsd-gpio = gpio2 pin 1) is not
 * driven until the BOSA analogue front end is initialised.  In a mainline
 * boot that never happens, so pon_led_work() reads it low forever and calls
 * note_rxsd(0), which keeps z->los = 1.  Nothing in the real driver path
 * ever clears z->lods either.  Both bits gate zxpon_try_o23(), so a valid
 * identity cannot move the machine out of O1 even with a live OLT above.
 *
 * Once an identity is configured we arm the activation and stop letting a
 * low opsd kick us back to O1: send the serial number and let the PLOAM
 * state machine find out whether the OLT is really there (the standard
 * O1 timer still falls back if downstream frames never arrive).  The LED
 * still follows the raw GPIO, so the panel lamp remains honest.
 */
static int zx_activation_armed;

/*
 * The family is filled in further down, next to the ops table that has to
 * exist before it can be named - and the reply path below needs the family
 * before that.  A tentative definition is the C way of saying "it comes
 * later": the tentative one here, the real one next to zxpon_ops.
 */
static struct genl_family zxpon_family;
static struct delayed_work zx_tick;
static int zx_alive;
static int zx_family_ok;
static int zx_nb_ok;

/*
 * The PON MAC window is the same 16 MiB NPPT mapping the driver already
 * owns.  Every index below is the vendor's Hal_ReadXmacReg /
 * Hal_WriteXmacReg index; the byte offset is idx * 4, so 0x16147 is
 * nppt + 0x5851c, exactly where the earlier register probe read zero.
 */
#define ZX_XMAC_REG(idx)	(zx_pon_hw + ((idx) << 2))
#define ZX_PON_RX_PENDING	0x16147
#define ZX_PON_RX_DATA		0x16148
#define ZX_PON_TX_KICK		0x16642
#define ZX_PON_TX_DATA		0x166c2

static void __iomem *zx_pon_hw;
static u32 zx_ploam_rx_n, zx_ploam_tx_n, zx_ploam_txerr_n;
static u8 zx_last_rx_id;
static int zx_hw_data_on;

static inline u32 zx_xmac_r(u32 idx)
{
	return zx_pon_hw ? readl(ZX_XMAC_REG(idx)) : 0;
}

static inline void zx_xmac_w(u32 idx, u32 val)
{
	if (zx_pon_hw)
		writel(val, ZX_XMAC_REG(idx));
}

static inline void zx_xmac_rmw(u32 idx, u32 clear, u32 set)
{
	u32 val = zx_xmac_r(idx);

	zx_xmac_w(idx, (val & ~clear) | set);
}

/* The read-modify-write list below is fi_Configure_Hw_Init() plus
 * fi_Configure_Pon_Mode(7), transcribed from the AArch64 vendor module.
 * No shared-PLL or SerDes register appears here: that side was brought up
 * separately in phy-zx279133-pon-serdes.c and is deliberately untouched.
 */
static void zx_pon_hw_bringup(void)
{
	u32 val;

	if (!zx_pon_hw)
		return;

	/* XG-PON (mode 7): mode nibble, mode flag and the 0x3fe preamble. */
	zx_xmac_rmw(0x6, 0xf, 7);
	zx_xmac_w(0x1688e, 5);
	val = (zx_xmac_r(0x1611d) & ~0x3ffu) | 0x3feu | 0xffc00u;
	zx_xmac_w(0x1611d, val);

	/* Top-level PON enable, held across vendor's 21-step delay loop. */
	zx_xmac_rmw(0x16000, 0, 0x1f);
	udelay(100);
	zx_xmac_rmw(0x1610b, 0, 0x80000);
	/* Downstream disabled, but the two vendor config bits set. */
	zx_xmac_rmw(0x16100, 0x1, 0x28);
	/* Vendor writes byte0=2 then byte1=0x0f, then selects XGS and enables. */
	val = (zx_xmac_r(0x16644) & ~0xffffu) | 0x1301u;
	zx_xmac_w(0x16644, val);
	/* Upstream PLOAM path enabled, normal data upstream held off. */
	zx_xmac_rmw(0x16600, 0x2 | 0x8, 0x11);
	zx_xmac_rmw(0x1689f, 0, 0x1);
	zx_xmac_rmw(0x1a, 0xc, 0);
	zx_xmac_rmw(0x16001, 0, 0x1);
	zx_xmac_rmw(0x16501, 0x2, 0x1);
	zx_xmac_rmw(0x16402, 0x3fff, 0x8);
	zx_xmac_rmw(0x3000a, 0, 0x1);
	zx_xmac_rmw(0x16635, 0, 0x1);
}

/* fi_Configure_Ponmac_Set_Sn(): two byte-swapped words at 0x16636/7. */
static void zx_pon_hw_set_sn(const u8 sn[ZXPON_SN_LEN])
{
	u32 lo, hi;

	if (!zx_pon_hw)
		return;
	memcpy(&lo, sn, sizeof(lo));
	memcpy(&hi, sn + sizeof(lo), sizeof(hi));
	zx_xmac_w(0x16636, __swab32(lo));
	zx_xmac_w(0x16637, __swab32(hi));
}

/* fi_Configure_Ponmac_Set_Gpon_Enable(1): data upstream plus downstream. */
static void zx_pon_hw_data(bool on)
{
	if (!zx_pon_hw || zx_hw_data_on == on)
		return;
	if (on) {
		zx_xmac_rmw(0x16600, 0x1, 0x2);
		zx_xmac_rmw(0x16100, 0, 0x1);
	} else {
		zx_xmac_rmw(0x16600, 0x2, 0x1);
		zx_xmac_rmw(0x16100, 0x1, 0);
	}
	zx_hw_data_on = on;
}

/* fi_Configure_Interrupt_Read_Ploam_Msg(): pending bit, 12 words, clear. */
static int zx_pon_hw_ploam_rx(u8 frame[ZXPON_PLOAM_LEN])
{
	int i;

	for (i = 0; i < 16; i++) {
		if (zx_xmac_r(ZX_PON_RX_PENDING) & 1)
			break;
	}
	if (i == 16)
		return 0;
	for (i = 0; i < ZXPON_PLOAM_LEN / 4; i++) {
		u32 word = __swab32(zx_xmac_r(ZX_PON_RX_DATA + i));

		memcpy(frame + i * 4, &word, 4);
	}
	zx_xmac_w(ZX_PON_RX_PENDING, 0);
	return 1;
}

/* fi_Configure_Ponmac_Send_Ploam_Msg(): wait, 12 words, kick, wait. */
static int zx_pon_hw_ploam_tx(const u8 frame[ZXPON_PLOAM_LEN])
{
	int i, ok = 0;

	for (i = 0; i < 16; i++) {
		if (!(zx_xmac_r(ZX_PON_TX_KICK) & 1))
			break;
	}
	for (i = 0; i < ZXPON_PLOAM_LEN / 4; i++) {
		u32 word;

		memcpy(&word, frame + i * 4, 4);
		zx_xmac_w(ZX_PON_TX_DATA + i, __swab32(word));
	}
	zx_xmac_w(ZX_PON_TX_KICK, 1);
	for (i = 0; i < 16; i++) {
		if (!(zx_xmac_r(ZX_PON_TX_KICK) & 1)) {
			ok = 1;
			break;
		}
	}
	return ok ? 0 : -ETIMEDOUT;
}

static void zxpon_poll_ploam(void)
{
	u8 frame[ZXPON_PLOAM_LEN];
	int i;

	if (!zx_pon_hw)
		return;
	for (i = 0; i < 4; i++) {
		if (!zx_pon_hw_ploam_rx(frame))
			break;
		zx_ploam_rx_n++;
		zx_last_rx_id = frame[2];
		zxpon_rx_ploam(&zx_state, frame);
	}
	for (i = 0; i < 4; i++) {
		if (!zxpon_peek_ploam(&zx_state, frame))
			break;
		if (zx_pon_hw_ploam_tx(frame)) {
			zx_ploam_txerr_n++;
			break;
		}
		zxpon_take_ploam(&zx_state, frame);
		zx_ploam_tx_n++;
	}
}

static void zxpon_apply_carrier(void)
{
	int o5;

	mutex_lock(&zx_pon_lock);
	o5 = zxpon_state(&zx_state) == ZXPON_O5;
	mutex_unlock(&zx_pon_lock);
	if (zx_wan) {
		if (o5)
			netif_carrier_on(zx_wan);
		else if (netif_carrier_ok(zx_wan))
			netif_carrier_off(zx_wan);
	}
	led_trigger_event(zx_green, o5 ? LED_FULL : LED_OFF);
	zx_pon_hw_data(o5);
}

static void zxpon_tick_work(struct work_struct *work)
{
	static unsigned int sn_tick;

	mutex_lock(&zx_pon_lock);
	if (zx_alive) {
		zxpon_tick(&zx_state, 100);
		if (++sn_tick >= 10) {
			sn_tick = 0;
			zxpon_retry_sn(&zx_state);
		}
		zxpon_poll_ploam();
	}
	mutex_unlock(&zx_pon_lock);
	zxpon_apply_carrier();
	if (zx_alive)
		schedule_delayed_work(&zx_tick, msecs_to_jiffies(100));
}

static int zxpon_net_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (!zx_wan || dev != zx_wan)
		return NOTIFY_DONE;
	if (event == NETDEV_CHANGE || event == NETDEV_UP)
		zxpon_apply_carrier();
	return NOTIFY_DONE;
}

static struct notifier_block zxpon_net_nb = {
	.notifier_call = zxpon_net_event,
};

static ssize_t pon_state_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	const char *name;

	mutex_lock(&zx_pon_lock);
	name = zxpon_state_name(&zx_state);
	mutex_unlock(&zx_pon_lock);
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(pon_state);

/* Read-only view of the newly wired MAC path.  These counters are the fast
 * way to tell "no downstream PLOAM" from "software never sent a frame"
 * without attaching a packet socket to the OMCI netdev.
 */
static ssize_t pon_hw_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	u16 alloc = 0;
	int alloc_ok;

	mutex_lock(&zx_pon_lock);
	alloc_ok = zxpon_alloc_id(&zx_state, &alloc);
	mutex_unlock(&zx_pon_lock);
	return sysfs_emit(buf,
		"mapped=%u rx_pending=%08x tx_kick=%08x "
		"rx=%u tx=%u txerr=%u last_rx_id=%02x data=%u "
		"alloc=%04x alloc_valid=%u\n",
		zx_pon_hw ? 1 : 0, zx_xmac_r(ZX_PON_RX_PENDING),
		zx_xmac_r(ZX_PON_TX_KICK), zx_ploam_rx_n, zx_ploam_tx_n,
		zx_ploam_txerr_n, zx_last_rx_id, zx_hw_data_on,
		alloc, alloc_ok);
}
static DEVICE_ATTR_RO(pon_hw);

static bool zxpon_omci_channel(u16 *gem)
{
	bool ready;

	mutex_lock(&zx_pon_lock);
	ready = zxpon_state(&zx_state) == ZXPON_O5 &&
		zxpon_omcc_gem(&zx_state, gem);
	mutex_unlock(&zx_pon_lock);
	return ready;
}

static ssize_t omci_transport_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct zx279133_eth *eth = netdev_priv(to_net_dev(dev));
	u16 gem;
	bool ready = zxpon_omci_channel(&gem);
	ssize_t length;

	mutex_lock(&eth->datapath_lock);
	ready = ready && eth->tx_prepared && !eth->tx_stopping;
	length = sysfs_emit(buf, "implemented=1 ready=%u rx_ready=0 mic_ready=0 submitted=%llu completed=%llu error=%d\n",
		ready, eth->omci_submitted, eth->omci_completed,
		ready ? eth->omci_error : -ENOLINK);
	mutex_unlock(&eth->datapath_lock);
	return length;
}
static DEVICE_ATTR_RO(omci_transport);

static int zxpon_get_state(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *msg;
	void *hdr;
	const char *name;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	hdr = genlmsg_put_reply(msg, info, &zxpon_family, 0, info->genlhdr->cmd);
	if (!hdr) {
		nlmsg_free(msg);
		return -ENOMEM;
	}
	mutex_lock(&zx_pon_lock);
	name = zxpon_state_name(&zx_state);
	mutex_unlock(&zx_pon_lock);
	if (nla_put_string(msg, ZXPON_A_STATE, name)) {
		genlmsg_cancel(msg, hdr);
		nlmsg_free(msg);
		return -EMSGSIZE;
	}
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

/*
 * The netlink handler, not the setter: zxpon.c already owns
 * zxpon_set_identity(zxpon *, sn, regid), and a handler with the same name
 * made the two one function - the call below then read as "too many
 * arguments to function 'zxpon_set_identity'" and the definition as a
 * conflicting type.  The nl_ prefix says which of the two this is.
 */
static int zxpon_nl_set_identity(struct sk_buff *skb, struct genl_info *info)
{
	const uint8_t *sn, *regid;

	if (!info->attrs[ZXPON_A_SN] || !info->attrs[ZXPON_A_REGID])
		return -EINVAL;
	sn = nla_data(info->attrs[ZXPON_A_SN]);
	regid = nla_data(info->attrs[ZXPON_A_REGID]);
	mutex_lock(&zx_pon_lock);
	zxpon_set_identity(&zx_state, sn, regid);
	zx_pon_hw_set_sn(sn);
	/*
	 * Identity is the real readiness signal on this board: the opsd GPIO
	 * stays low and lods has no clear path, so clear both here and let
	 * try_o23() run.  Fail-safe O1 timers remain active in zxpon.c.
	 */
	zxpon_set_lods(&zx_state, 0);
	zxpon_set_los(&zx_state, 0);
	zx_activation_armed = 1;
	mutex_unlock(&zx_pon_lock);
	zxpon_apply_carrier();
	return 0;
}

static int zxpon_add_tcont_cmd(struct sk_buff *skb, struct genl_info *info)
{
	/* No ZX279133 T-CONT table backend yet: do not ACK a software-only write. */
	return -EOPNOTSUPP;
}

static int zxpon_map_cmd(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

static int zxpon_set_key_cmd(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

static int zxpon_omci_tx_cmd(struct sk_buff *skb, struct genl_info *info)
{
	const u8 *frame;
	u16 gem;

	if (!info->attrs[ZXPON_A_OMCI_FRAME])
		return -EINVAL;
	frame = nla_data(info->attrs[ZXPON_A_OMCI_FRAME]);
	/* Baseline response/notification with an already supplied MIC. Extended
	 * frames and physical-mode/key changes are not part of this transport.
	 */
	if (frame[3] != 0x0a || (frame[2] & 0xc0) ||
	    frame[40] || frame[41] || frame[42] || frame[43] != 0x28)
		return -EBADMSG;
	if (!zx_wan || !zxpon_omci_channel(&gem))
		return -ENOLINK;
	return zx279133_omci_send(netdev_priv(zx_wan), gem, frame, ZXPON_OMCI_LEN);
}

/*
 * Rehearsal injection of a downstream PLOAM frame.
 *
 * With no fibre in the jack the receive loop never yields a frame, so the O2/O3
 * -> O4 -> O5 half of the state machine cannot be exercised on the bench, and
 * neither can anything that hangs off O5 (carrier, the data path, the OMCI
 * channel).  This verb lets the bench feed the same 48 bytes the OLT would send
 * and watch the board answer through the real hardware transmit path.
 *
 * It is a test hook, not a second protocol implementation: the MIC is
 * recalculated here so a caller does not have to carry AES-CMAC of its own
 * (tests/zxpon_test.c covers the CMAC itself), and everything downstream of
 * zxpon_rx_ploam() is the ordinary code path.  Admin-only, like every other
 * verb that can move the machine.
 */
static int zxpon_ploam_rx_cmd(struct sk_buff *skb, struct genl_info *info)
{
	static bool noted;
	u8 frame[ZXPON_PLOAM_LEN];
	int ret;

	if (!info->attrs[ZXPON_A_PLOAM_FRAME])
		return -EINVAL;
	memcpy(frame, nla_data(info->attrs[ZXPON_A_PLOAM_FRAME]),
	       ZXPON_PLOAM_LEN);
	zxpon_ploam_seal(frame);
	mutex_lock(&zx_pon_lock);
	ret = zxpon_rx_ploam(&zx_state, frame);
	mutex_unlock(&zx_pon_lock);
	if (!noted) {
		noted = true;
		pr_info("zxpon: bench PLOAM injection in use; downstream frames now come from genl, not from the fibre\n");
	}
	if (ret < 0)
		return -EBADMSG;
	zxpon_apply_carrier();
	return 0;
}

static const struct genl_ops zxpon_ops[] = {
	{
		.cmd = ZXPON_CMD_OMCI_TX,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_omci_tx_cmd,
	},
	{
		.cmd = ZXPON_CMD_GET_STATE,
		.policy = zxpon_policy,
		.doit = zxpon_get_state,
	},
	{
		.cmd = ZXPON_CMD_SET_IDENTITY,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_nl_set_identity,
	},
	{
		.cmd = ZXPON_CMD_ADD_TCONT,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_add_tcont_cmd,
	},
	{
		.cmd = ZXPON_CMD_ADD_GEM,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_map_cmd,
	},
	{
		.cmd = ZXPON_CMD_MAP_GEM,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_map_cmd,
	},
	{
		.cmd = ZXPON_CMD_SET_KEY,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_set_key_cmd,
	},
	{
		.cmd = ZXPON_CMD_PLOAM_RX,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_ploam_rx_cmd,
	},
};

static struct genl_family zxpon_family = {
	.name = "zxpon",
	.version = 1,
	.maxattr = ZXPON_A_MAX - 1,
	.ops = zxpon_ops,
	.n_ops = ARRAY_SIZE(zxpon_ops),
	.resv_start_op = ZXPON_CMD_MAX,
	.module = THIS_MODULE,
};

/* This netdev is the raw RX endpoint. TX uses the privileged zxpon OMCI_TX
 * command, whose ACK follows IDM completion instead of packet-socket enqueue.
 */
static netdev_tx_t zxpon_omci_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb(skb);
	dev->stats.tx_dropped++;
	dev->stats.tx_errors++;
	return NETDEV_TX_OK;
}

static int zxpon_omci_open(struct net_device *dev)
{
	/* No Ethernet carrier is invented for the management endpoint. */
	netif_carrier_off(dev);
	netif_start_queue(dev);
	return 0;
}

static int zxpon_omci_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops zxpon_omci_ops = {
	.ndo_open = zxpon_omci_open,
	.ndo_stop = zxpon_omci_stop,
	.ndo_start_xmit = zxpon_omci_xmit,
};

static void zxpon_omci_setup(struct net_device *dev)
{
	dev->netdev_ops = &zxpon_omci_ops;
	dev->mtu = ZXPON_OMCI_LEN;
	dev->min_mtu = ZXPON_OMCI_LEN;
	dev->max_mtu = ZXPON_OMCI_LEN;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->needs_free_netdev = true;
}

void zx279133_pon_note_rxsd(int light)
{
	if (!zx_alive)
		return;
	mutex_lock(&zx_pon_lock);
	/* A low opsd after activation is the uninitialised BOSA signal detect,
	 * not proof of a dark fibre.  Ignore it so the PLOAM state machine is
	 * not reset to O1 on every 200 ms poll.  A high opsd is always honoured.
	 */
	if (light || !zx_activation_armed)
		zxpon_set_los(&zx_state, light ? 0 : 1);
	mutex_unlock(&zx_pon_lock);
	zxpon_apply_carrier();
}

void zx279133_pon_carrier_kick(struct net_device *ndev)
{
	if (!zx_alive)
		return;
	if (ndev)
		zx_wan = ndev;
	zxpon_apply_carrier();
}

void zx279133_pon_attach(struct net_device *ndev)
{
	struct zx279133_eth *eth = netdev_priv(ndev);
	int rc;

	if (zx_alive)
		return;
	led_trigger_register_simple("zx279133-pon", &zx_green);
	if (!zx_green)
		dev_warn(&ndev->dev, "PON LED trigger unavailable\n");
	zx_activation_armed = 0;
	zxpon_init(&zx_state);
	zx_wan = ndev;
	/* The NPPT mapping is already established at this point in probe. */
	zx_pon_hw = eth->base;
	if (zx_pon_hw)
		zx_pon_hw_bringup();
	else
		dev_warn(&ndev->dev, "PON MAC window not mapped; PLOAM disabled\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_state))
		dev_warn(&ndev->dev, "pon_state sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_omci_transport))
		dev_warn(&ndev->dev, "omci_transport sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_hw))
		dev_warn(&ndev->dev, "pon_hw sysfs not created\n");
	zx_omci = alloc_netdev(0, "omci", NET_NAME_UNKNOWN, zxpon_omci_setup);
	if (zx_omci) {
		zx_omci->dev.parent = ndev->dev.parent;
		rc = register_netdev(zx_omci);
		if (rc) {
			free_netdev(zx_omci);
			zx_omci = NULL;
			dev_warn(&ndev->dev, "omci netdev failed: %d\n", rc);
		}
	}
	if (!zx_family_ok) {
		rc = genl_register_family(&zxpon_family);
		if (!rc)
			zx_family_ok = 1;
		else
			dev_warn(&ndev->dev, "zxpon genl failed: %d\n", rc);
	}
	rc = register_netdevice_notifier(&zxpon_net_nb);
	if (rc)
		dev_warn(&ndev->dev, "pon notifier failed: %d\n", rc);
	else
		zx_nb_ok = 1;
	zx_alive = 1;
	INIT_DELAYED_WORK(&zx_tick, zxpon_tick_work);
	schedule_delayed_work(&zx_tick, msecs_to_jiffies(100));
	zxpon_apply_carrier();
}

void zx279133_pon_stop(void)
{
	if (!zx_alive)
		return;
	zx_alive = 0;
	zx_activation_armed = 0;
	cancel_delayed_work_sync(&zx_tick);
	if (zx_nb_ok) {
		unregister_netdevice_notifier(&zxpon_net_nb);
		zx_nb_ok = 0;
	}
	if (zx_wan) {
		device_remove_file(&zx_wan->dev, &dev_attr_pon_state);
		device_remove_file(&zx_wan->dev, &dev_attr_omci_transport);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_hw);
	}
	zx_pon_hw = NULL;
	zx_hw_data_on = 0;
	if (zx_omci) {
		unregister_netdev(zx_omci);
		zx_omci = NULL;
	}
	if (zx_family_ok) {
		genl_unregister_family(&zxpon_family);
		zx_family_ok = 0;
	}
	led_trigger_unregister_simple(zx_green);
	zx_green = NULL;
	zx_wan = NULL;
}
