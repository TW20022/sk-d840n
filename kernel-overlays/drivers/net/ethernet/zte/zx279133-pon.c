// SPDX-License-Identifier: GPL-2.0-only
/*
 * XG-PON activation glue for the ZX279133 WAN netdev.
 *
 * The 49-word xgpon_nsyn image is programmed by phy-zx279133-pon-serdes.c
 * from the bootloader's own mode-6 table; dumping 0x16000000..0x160000c0 on
 * the board matches it word for word (docs/116).  Everything this file
 * writes is named by a Hal_WriteXmacReg trace disassembled from
 * zte_xgpon.ko (4.19.136) and decoded in docs/106, and resolves against the
 * PON MAC block at 0x17000000 (see ZX_PON_MAC_PHYS): the vendor accessors are
 * fpga_read_reg(reg) / fpga_write_reg(reg), which index pon_base + reg * 4.
 *
 * The software activation state machine used to have no hardware layer at
 * all: it could walk to O2-3 in memory while the PON MAC registers were all
 * zero.  The hardware side is armed on request rather than at probe, because
 * the first attempt to drive it took the board off the bus: the block's clocks
 * are already on (the NPPT driver enables all six), so the failure was the
 * transmit side running before the IDM rings were set up.  pon_arm_store()
 * therefore has a read-only stage and a bring-up stage, and the 100 ms tick
 * only touches PLOAM RX/TX once the block has been armed at all.
 *
 * The management channel is closed here too: downstream OMCI frames enter
 * zxpon_rx_omci(), whose replies are drained into the IDM queue 0 by the same
 * tick.  With no fibre the receive side never sees a frame, so a privileged
 * verb lets the bench inject one (docs/113 does the same for PLOAM).
 *
 * GEM port and T-CONT provisioning follow docs/109: an indirect one-byte
 * table for GEM presence and a packed halfword bank for AllocID/T-CONT.  Both
 * are programmed from the real PLOAM assignment as soon as it lands, and read
 * back so a bench run can show the register took the value.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/if_arp.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/leds.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/swab.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include <linux/stdarg.h>

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

/*
 * Downstream GEM port table, docs/109 section 1.  One byte per GEM id behind
 * an indirect port: write the address (bit 31 asks for a read), wait for the
 * done word to go non-zero, then read or write the data word.  The vendor
 * spends 4096 polls on each wait and sets bit 1 as the "present" flag of an
 * entry; nothing else in the byte is touched.
 */
#define ZX_GEM_TABLE_ADDR	0x16205
#define ZX_GEM_TABLE_DONE	0x16206
#define ZX_GEM_TABLE_DATA	0x16207
#define ZX_GEM_TABLE_READ	0x08000000u
#define ZX_GEM_TABLE_PRESENT	0x02
#define ZX_GEM_TABLE_POLL	0x1000

/*
 * T-CONT / AllocID bank, docs/109 section 2.  Word 0x1650f + n packs two
 * entries: the low halfword is T-CONT 2n-1 and the high halfword 2n, with
 * T-CONT 0 alone in the low half of the first word.  Bits [13:0] of a
 * halfword are the AllocID and bit 14 marks it valid; the vendor clears all
 * of [13:0] and then ORs 0x4000, which is what the mask below reproduces.
 */
#define ZX_TCONT_BANK		0x1650f
#define ZX_TCONT_ALLOC_MASK	0x3fffu
#define ZX_TCONT_VALID		0x4000
#define ZX_TCONT_OMCC		0

#define ZX_TCONT_MAX		8
#define ZX_GEM_MAX		8

struct zx_tcont_row {
	bool used;
	bool high;
	u16 tcont;
	u16 alloc;
	u32 index;
	u32 readback;
};

struct zx_gem_row {
	bool used;
	u16 gem;
	u8 value;
	int status;
};

/*
 * The PON MAC is its own 16 MiB block at 0x17000000 - the stock device tree
 * calls it "pon@17000000" (compatible "zte,zx279133-pon") and the vendor's
 * zx_ponreg_133.ko decodes every PON register number as pon_base + reg * 4.
 * The NPPT window at 0x19000000, which the ethernet node maps, is a different
 * block: reading the PON registers through it returns zero for everything past
 * 0x16048, which is exactly what the first bench runs showed.
 */
#define ZX_PON_MAC_PHYS		0x17000000
#define ZX_PON_MAC_SIZE		0x1000000


static void __iomem *zx_pon_hw;
static u32 zx_ploam_rx_n, zx_ploam_tx_n, zx_ploam_txerr_n, zx_ploam_busy_n;
static u8 zx_last_rx_id;
static int zx_hw_data_on;

/*
 * Management channel counters and a small ring of the frames that crossed it.
 * The ring is what makes the bench able to check a reply byte by byte without
 * attaching a socket to the management netdev.
 */
#define ZX_OMCI_LOG_N		8
static u8 zx_omci_log[ZX_OMCI_LOG_N][ZXPON_OMCI_LEN];
static u8 zx_omci_log_dir[ZX_OMCI_LOG_N];
static u32 zx_omci_log_head, zx_omci_log_n;
static u32 zx_omci_rx_n, zx_omci_tx_n, zx_omci_drop_n;

static struct zx_tcont_row zx_tcont_rows[ZX_TCONT_MAX];
static struct zx_gem_row zx_gem_rows[ZX_GEM_MAX];
static bool zx_provision_noted;
static unsigned int zx_gem_retry_delay;
static unsigned int zx_gem_notes;

/* Defined next to the sysfs view; the drain below needs it before that. */
static bool zxpon_omci_channel(u16 *gem);

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
 * fi_Configure_Pon_Mode(3), transcribed from the AArch64 vendor module.
 * No shared-PLL or SerDes register appears here: that side was brought up
 * separately in phy-zx279133-pon-serdes.c and is deliberately untouched.
 *
 * XG-PON is pon mode 3, not 7.  fi_Configure_Pon_Mode() branches on
 * (mode & ~8) and the two branches differ in the mode nibble AND in the
 * upstream burst preamble: 0x1688e is 0x20 with 0x3ff for XG-PON and 5
 * with 0x3fe for XGS-PON.  The earlier transcription followed the
 * g_up_mode == 0x80 branch, which is XGS, while this board's up_mode is
 * 0x40 - the same module prints "pon mode is xg" on that path.  docs/116.
 */
static void zx_pon_hw_bringup(void)
{
	u32 val;

	if (!zx_pon_hw)
		return;

	/* XG-PON (mode 3): mode nibble, preamble repeats, preamble pattern. */
	zx_xmac_rmw(0x6, 0xf, 3);
	zx_xmac_w(0x1688e, 0x20);
	val = (zx_xmac_r(0x1611d) & ~0x3ffu) | 0x3ffu | 0xffc00u;
	zx_xmac_w(0x1611d, val);

	/* Top-level PON enable. Vendor writes this ONCE, then spins
	 * 21 iterations of __const_udelay(0x418958) ~= 21ms waiting for
	 * the internal init to complete before touching anything else.
	 * (fi_Configure_Hw_Init @0x28fc disassembly; the old udelay(100)
	 * was 200x too short and was misread as a 21x write loop.) */
	zx_xmac_rmw(0x16000, 0, 0x1f);
	usleep_range(21000, 25000);
	zx_xmac_rmw(0x1610b, 0, 0x80000);
	/* Downstream disabled, but the two vendor config bits set. */
	zx_xmac_rmw(0x16100, 0x1, 0x28);
	/* Vendor writes byte0=2 then byte1=0x0f here, which is exactly the
	 * value this board's loader already leaves (0x0f02). */
	val = (zx_xmac_r(0x16644) & ~0xffffu) | 0x0f02u;
	zx_xmac_w(0x16644, val);
	/* Upstream PLOAM path enabled, normal data upstream held off. */
	zx_xmac_rmw(0x16600, 0x2 | 0x8, 0x11);
	zx_xmac_rmw(0x1689f, 0, 0x1);
	zx_xmac_rmw(0x1a, 0xc, 0);
	zx_xmac_rmw(0x16001, 0, 0x1);
	/* Bytes are addressed explicitly by the vendor here: byte 2, not
	 * byte 0 - set bit 16 and clear bit 17. */
	zx_xmac_rmw(0x16501, 0x20000, 0x10000);
	zx_xmac_rmw(0x16402, 0x3fff, 0x8);
	zx_xmac_rmw(0x3000a, 0, 0x1);
	zx_xmac_rmw(0x16635, 0, 0x1);

	/* The tail of fi_Configure_Hw_Init(): the PON-mode word.  For
	 * up_mode 0x40 byte1 is 0x1b and for 0x80 it is 0x13; byte0 is then
	 * set to 1.  The old constant 0x1301 was the XGS word, and writing
	 * an XGS PON-mode word to an XG-PON MAC is what took this board off
	 * the bus in docs/107 - the board survives 0x1b01. */
	val = (zx_xmac_r(0x16644) & ~0xff00u) | 0x1b00u;
	zx_xmac_w(0x16644, val);
	val = (zx_xmac_r(0x16644) & ~0xffu) | 0x1u;
	zx_xmac_w(0x16644, val);
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

/*
 * fi_Configure_Ponmac_Send_Ploam_Msg(): wait, 12 words, kick, wait.
 *
 * The vendor carries on and writes the frame even if the window never frees
 * up.  On this board that happens for a real reason - with no OLT there is no
 * upstream grant, so the window stays busy indefinitely - and re-writing the
 * same 12 words every 100 ms only churns registers and hides the state.  So a
 * window that is still busy is reported as -EBUSY and the queued frame stays
 * where it is, to be retried on a later tick.
 */
static int zx_pon_hw_ploam_tx(const u8 frame[ZXPON_PLOAM_LEN])
{
	int i, ok = 0;

	for (i = 0; i < 16; i++) {
		if (!(zx_xmac_r(ZX_PON_TX_KICK) & 1))
			break;
	}
	if (i == 16)
		return -EBUSY;
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

/*
 * The three indirect-table accessors.  They are only entered with zx_pon_lock
 * held (the tick and every genl verb take it), which is this driver's stand-in
 * for the vendor's fpga_io_lock plus local IRQ disable: no interrupt handler
 * on this path touches the PON window.
 */
static int zx_gem_table_wait(void)
{
	int i;

	for (i = 0; i < ZX_GEM_TABLE_POLL; i++)
		if (zx_xmac_r(ZX_GEM_TABLE_DONE))
			return 0;
	return -ETIMEDOUT;
}

static int zx_gem_table_get(u16 gem, u8 *value)
{
	if (!zx_pon_hw || gem > 0xfffe)
		return -EINVAL;
	zx_xmac_w(ZX_GEM_TABLE_ADDR, gem | ZX_GEM_TABLE_READ);
	if (zx_gem_table_wait())
		return -ETIMEDOUT;
	*value = (u8)zx_xmac_r(ZX_GEM_TABLE_DATA);
	return 0;
}

static int zx_gem_table_put(u16 gem, u8 value)
{
	if (!zx_pon_hw || gem > 0xfffe)
		return -EINVAL;
	zx_xmac_w(ZX_GEM_TABLE_ADDR, gem);
	zx_xmac_w(ZX_GEM_TABLE_DATA, value);
	return zx_gem_table_wait();
}

/* Read-modify-write one GEM entry and check the byte came back. */
static int zx_gem_table_program(u16 gem, u8 *readback)
{
	u8 value;
	int ret;

	ret = zx_gem_table_get(gem, &value);
	if (ret)
		return ret;
	value |= ZX_GEM_TABLE_PRESENT;
	ret = zx_gem_table_put(gem, value);
	if (ret)
		return ret;
	ret = zx_gem_table_get(gem, &value);
	if (ret)
		return ret;
	if (readback)
		*readback = value;
	return (value & ZX_GEM_TABLE_PRESENT) ? 0 : -EIO;
}

static u32 zx_tcont_index(u16 tcont, bool *high)
{
	u32 next = (u32)tcont + 1;
	u32 word = next >> 1;

	/*
	 * T-CONT 0 is the OMCC and the vendor does not derive it: the dedicated
	 * fi_Congigure_Creat_Omcc_Tcont writes the first bank word outright.  Its
	 * generic raw-T-CONT formula would send id 0 to the catch-all slot 0x1e,
	 * which this board reads back as zero - so follow the dedicated path.
	 */
	if (tcont == ZX_TCONT_OMCC) {
		*high = false;
		return ZX_TCONT_BANK;
	}
	*high = tcont && (next & 1);
	if ((u32)(tcont - 0x23) <= 4)
		word += 0xa;
	else if (tcont != 0x40)
		word = 0x1e;
	return ZX_TCONT_BANK + word;
}

static int zx_tcont_program(u16 tcont, u16 alloc, u32 *readback)
{
	bool high;
	u32 index, word, rb;
	u16 half;

	if (!zx_pon_hw || alloc > ZX_TCONT_ALLOC_MASK)
		return -EINVAL;
	index = zx_tcont_index(tcont, &high);
	word = zx_xmac_r(index);
	half = high ? (u16)(word >> 16) : (u16)word;
	half = (u16)((half & 0x8000) | (alloc & ZX_TCONT_ALLOC_MASK) |
		     ZX_TCONT_VALID);
	if (high)
		word = (word & 0xffffu) | ((u32)half << 16);
	else
		word = (word & 0xffff0000u) | half;
	zx_xmac_w(index, word);
	rb = zx_xmac_r(index);
	if (readback)
		*readback = rb;
	return ((high ? (u16)(rb >> 16) : (u16)rb) == half) ? 0 : -EIO;
}

static struct zx_tcont_row *zx_tcont_row(u16 tcont)
{
	int i;

	for (i = 0; i < ZX_TCONT_MAX; i++)
		if (zx_tcont_rows[i].used && zx_tcont_rows[i].tcont == tcont)
			return &zx_tcont_rows[i];
	return NULL;
}

static struct zx_gem_row *zx_gem_row(u16 gem)
{
	int i;

	for (i = 0; i < ZX_GEM_MAX; i++)
		if (zx_gem_rows[i].used && zx_gem_rows[i].gem == gem)
			return &zx_gem_rows[i];
	return NULL;
}

/* Pick the next free T-CONT id: 0 belongs to the OMCC, then 1, 2, ... */
static int zx_tcont_alloc_id(u16 *tcont)
{
	int i;

	for (i = 0; i < ZX_TCONT_MAX; i++)
		if (!zx_tcont_rows[i].used) {
			*tcont = i;
			return 0;
		}
	return -ENOSPC;
}

static int zx_tcont_record(u16 tcont, u16 alloc, u32 readback)
{
	struct zx_tcont_row *row = zx_tcont_row(tcont);

	if (!row) {
		int i;

		for (i = 0; i < ZX_TCONT_MAX; i++)
			if (!zx_tcont_rows[i].used) {
				row = &zx_tcont_rows[i];
				break;
			}
		if (!row)
			return -ENOSPC;
		row->used = true;
		row->tcont = tcont;
	}
	row->alloc = alloc;
	row->readback = readback;
	row->index = zx_tcont_index(tcont, &row->high);
	return 0;
}

static int zx_gem_record(u16 gem, int status, u8 value)
{
	struct zx_gem_row *row = zx_gem_row(gem);

	if (!row) {
		int i;

		for (i = 0; i < ZX_GEM_MAX; i++)
			if (!zx_gem_rows[i].used) {
				row = &zx_gem_rows[i];
				break;
			}
		if (!row)
			return -ENOSPC;
		row->used = true;
		row->gem = gem;
	}
	row->status = status;
	row->value = value;
	return 0;
}

static void zx_omci_log_put(u8 dir, const u8 *frame)
{
	memcpy(zx_omci_log[zx_omci_log_head], frame, ZXPON_OMCI_LEN);
	zx_omci_log_dir[zx_omci_log_head] = dir;
	zx_omci_log_head = (zx_omci_log_head + 1) % ZX_OMCI_LOG_N;
	if (zx_omci_log_n < ZX_OMCI_LOG_N)
		zx_omci_log_n++;
}

/*
 * Push every queued OMCI reply through the real IDM queue 0.  A failure stops
 * the drain instead of spinning: the frame that could not leave stays where
 * it is, and the counter says so.
 */
static int zxpon_drain_omci(void)
{
	u8 frame[ZXPON_OMCI_LEN];
	u16 gem;
	int sent = 0, ret;

	if (!zx_wan)
		return 0;
	for (;;) {
		if (!zxpon_omci_channel(&gem))
			break;
		mutex_lock(&zx_pon_lock);
		ret = zxpon_take_omci(&zx_state, frame);
		mutex_unlock(&zx_pon_lock);
		if (!ret)
			break;
		ret = zx279133_omci_send(netdev_priv(zx_wan), gem, frame,
					 ZXPON_OMCI_LEN);
		if (ret) {
			zx_omci_drop_n++;
			break;
		}
		zx_omci_log_put('T', frame);
		zx_omci_tx_n++;
		sent++;
	}
	return sent;
}

/*
 * Provision the data path from whatever the OLT actually assigned.  There is
 * no userspace step here on purpose: the PLOAM AllocID frame already carries
 * the AllocID and, with the OMCC flag set, the management GEM port, so the
 * T-CONT bank and the GEM table can follow it directly - exactly the pair the
 * vendor programs in fi_Congigure_Creat_Omcc_Tcont and
 * fi_Configure_Gem_Create_Downstream_GemPort.
 */
static void zxpon_provision_locked(void)
{
	struct zx_tcont_row *row;
	struct zx_gem_row *gem_row;
	u16 alloc, gem;
	u32 readback = 0;
	u8 value = 0;
	int ret;

	if (!zx_pon_hw)
		return;
	/*
	 * The vendor programs these two tables only once the OLT has accepted
	 * the ONU: the AllocID and the OMCC GEM port arrive in the O5
	 * assignment, and the indirect table engine behind 0x16205 does not
	 * answer at all while the downstream half of the MAC is held off.  Both
	 * were wrong before: the tables were written at O2-3 and the failures
	 * were then latched for the lifetime of the driver.
	 */
	if (zxpon_state(&zx_state) != ZXPON_O5)
		return;
	if (!zxpon_alloc_id(&zx_state, &alloc))
		return;
	row = zx_tcont_row(ZX_TCONT_OMCC);
	if (!row || row->alloc != alloc) {
		/*
		 * 0x1650f is write-only on this part: a read comes back as zero
		 * even after the vendor's own write, so the readback is kept for
		 * sysfs and never treated as a failure.
		 */
		zx_tcont_program(ZX_TCONT_OMCC, alloc, &readback);
		zx_tcont_record(ZX_TCONT_OMCC, alloc, readback);
	}
	if (!zxpon_omcc_gem(&zx_state, &gem))
		return;
	gem_row = zx_gem_row(gem);
	if (gem_row && !gem_row->status)
		return;
	/* One attempt per second: a refusal here is not transient. */
	if (++zx_gem_retry_delay < 10)
		return;
	zx_gem_retry_delay = 0;
	ret = zx_gem_table_program(gem, &value);
	zx_gem_record(gem, ret, value);
	if (ret) {
		if (zx_gem_notes++ < 3)
			pr_warn("zxpon: GEM %u table write failed (%d), readback %02x\n",
				gem, ret, value);
		return;
	}
	if (!zx_provision_noted) {
		zx_provision_noted = true;
		pr_info("zxpon: provisioned T-CONT 0 alloc %u and GEM %u on the PON MAC tables\n",
			alloc, gem);
	}
}

static void zxpon_poll_ploam(void)
{
	int i;
	u8 frame[ZXPON_PLOAM_LEN];

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
		int ret;

		if (!zxpon_peek_ploam(&zx_state, frame))
			break;
		ret = zx_pon_hw_ploam_tx(frame);
		if (ret) {
			/* -EBUSY with no fibre is the normal state, not a fault. */
			if (ret == -EBUSY)
				zx_ploam_busy_n++;
			else
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
		zxpon_provision_locked();
	}
	mutex_unlock(&zx_pon_lock);
	/*
	 * Turn the downstream half on first: the PON MAC's indirect table engine
	 * refuses every transaction while 0x16100 bit 0 is still clear.
	 */
	zxpon_apply_carrier();
	if (zx_alive) {
		mutex_lock(&zx_pon_lock);
		zxpon_provision_locked();
		mutex_unlock(&zx_pon_lock);
		zxpon_drain_omci();
		schedule_delayed_work(&zx_tick, msecs_to_jiffies(100));
	}
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
		"rx=%u tx=%u txerr=%u busy=%u last_rx_id=%02x data=%u "
		"alloc=%04x alloc_valid=%u\n",
		zx_pon_hw ? 1 : 0, zx_xmac_r(ZX_PON_RX_PENDING),
		zx_xmac_r(ZX_PON_TX_KICK), zx_ploam_rx_n, zx_ploam_tx_n,
		zx_ploam_txerr_n, zx_ploam_busy_n, zx_last_rx_id, zx_hw_data_on,
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
	int queued;

	/* Read the queue depth before the datapath lock: the drain takes them
	 * in the other order and neither side should ever hold both.
	 */
	mutex_lock(&zx_pon_lock);
	queued = zx_state.omci_tx_n;
	mutex_unlock(&zx_pon_lock);
	mutex_lock(&eth->datapath_lock);
	ready = ready && eth->tx_prepared && !eth->tx_stopping;
	length = sysfs_emit(buf, "implemented=1 ready=%u rx_ready=1 mic_ready=0 rx=%u tx=%u drop=%u q=%d submitted=%llu completed=%llu error=%d\n",
		ready, zx_omci_rx_n, zx_omci_tx_n, zx_omci_drop_n, queued,
		eth->omci_submitted, eth->omci_completed,
		ready ? eth->omci_error : -ENOLINK);
	mutex_unlock(&eth->datapath_lock);
	return length;
}
static DEVICE_ATTR_RO(omci_transport);

/*
 * The provisioned tables, as the MAC now holds them.  readback is the word or
 * byte the register returned straight after the write, so "the POST landed"
 * and "the model agrees with itself" can be told apart from the bench.
 */
static ssize_t pon_tables_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	ssize_t len = 0;
	int i;

	mutex_lock(&zx_pon_lock);
	for (i = 0; i < ZX_TCONT_MAX && len < (int)PAGE_SIZE - 96; i++) {
		struct zx_tcont_row *row = &zx_tcont_rows[i];

		if (!row->used)
			continue;
		len += sysfs_emit_at(buf, len,
			"tcont=%u alloc=%04x index=%05x high=%u readback=%08x\n",
			row->tcont, row->alloc, row->index, row->high,
			row->readback);
	}
	for (i = 0; i < ZX_GEM_MAX && len < (int)PAGE_SIZE - 64; i++) {
		struct zx_gem_row *row = &zx_gem_rows[i];

		if (!row->used)
			continue;
		len += sysfs_emit_at(buf, len, "gem=%u value=%02x status=%d\n",
			row->gem, row->value, row->status);
	}
	mutex_unlock(&zx_pon_lock);
	return len;
}
static DEVICE_ATTR_RO(pon_tables);

/*
 * Bench probe for the GEM indirect table.  The transaction follows the vendor
 * sequence exactly (address word on 0x16205, done on 0x16206, data on 0x16207)
 * and reports every step, so a timeout can be split into "the address did not
 * stick", "the engine never raised done" or "done came back but the data is
 * wrong" instead of one opaque -ETIMEDOUT.
 */
/*
 * Arming the PON block is deliberately a userspace action, not something probe
 * does.  Booting must not depend on a block that once took the board off the
 * bus: the board comes up with the driver in "no hardware" mode, and
 * /sys/class/net/eth0/pon_arm is what maps 0x17000000.  Plain "on" is
 * read-only - it maps, reports the state the bootloader left behind, and lets
 * the tick poll.  Only "force" writes the vendor init, which is not needed on
 * this board and is kept for bench work.  Keep pon_revive/uboot_kernel_recover.py
 * and the TFTP image ready when experimenting; a stall there costs a power
 * cycle, not a brick, because boot never touches this block.
 */
static bool zx_pon_armed;

/*
 * Arming is staged, and the first stage only reads.
 *
 * The block does answer plain MMIO: the NPPT driver writes pon_route
 * (0x17000080) through a regmap on every boot, and the six PON clocks are
 * already running because zx279133-main.c calls clk_bulk_prepare_enable() on
 * them.  What took the board down in the first attempt was therefore not a
 * missing clock - it was the vendor bring-up driving the MAC transmit side
 * before the IDM rings existed.  So:
 *
 *   echo map > pon_arm    map the window, list the clock rates, read 5 words
 *   echo on  > pon_arm    map (if needed), then run the vendor bring-up
 *
 * Every read is bracketed by two pr_emerg() lines, so a stall on the serial
 * console names the exact register that did it instead of leaving a blank
 * screen.
 */
static void zx_pon_read(u32 idx)
{
	pr_emerg("zxpon: read %05x ...\n", idx);
	pr_emerg("zxpon: read %05x = %08x\n", idx, zx_xmac_r(idx));
}

static ssize_t pon_arm_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct zx279133_eth *eth = netdev_priv(ndev);
	bool do_on;
	int i;

	if (sysfs_streq(buf, "force"))
		do_on = true;
	else if (sysfs_streq(buf, "on") || sysfs_streq(buf, "1") ||
		 sysfs_streq(buf, "map"))
		do_on = false;
	else
		return -EINVAL;

	if (!zx_pon_hw) {
		phys_addr_t phys = ZX_PON_MAC_PHYS;

		for (i = 0; i < ZX279133_NUM_CLOCKS; i++) {
			struct clk *clk = eth->clocks[i].clk;

			pr_emerg("zxpon: clk %s %s %lu Hz\n",
				 eth->clocks[i].id, clk ? "on" : "ABSENT",
				 clk ? clk_get_rate(clk) : 0UL);
		}

		if (!request_mem_region(phys, ZX_PON_MAC_SIZE, "zxpon-pon-mac"))
			pr_warn("zxpon: %pa is claimed elsewhere, mapping anyway\n",
				&phys);
		zx_pon_hw = devm_ioremap(dev, phys, ZX_PON_MAC_SIZE);
		if (!zx_pon_hw)
			return -ENOMEM;
		pr_emerg("zxpon: mapped %pa size %x\n", &phys, ZX_PON_MAC_SIZE);
		zx_pon_armed = true;
	}

	/* Read-only first: the words the vendor's own poll touches. */
	zx_pon_read(0x16000);
	zx_pon_read(0x16001);
	zx_pon_read(0x16048);
	zx_pon_read(0x16100);
	zx_pon_read(0x16147);

	if (!do_on)
		return count;

	/*
	 * Only on an explicit "force".  The board's own bootloader already
	 * programs this MAC: 0x16644 reads 0x0f02, 0x16635 has its enable bit,
	 * 0x1611d is 0x000fffff, and 0x166c2 still holds the serial-number
	 * PLOAM it queued (0x03ff 0100...).  Re-running the init on top of that
	 * would overwrite the mode word - 0x16644 low half is 0x0f02, while the
	 * bring-up writes 0x1301 - so it stays behind a verb of its own.
	 */
	pr_emerg("zxpon: running vendor bring-up (force)\n");
	zx_pon_hw_bringup();
	return count;
}
static DEVICE_ATTR_WO(pon_arm);

static ssize_t pon_probe_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	ssize_t len = 0;
	u32 mode, gem_word = 0;
	u16 gem = 1348;

	len += sysfs_emit_at(buf, len, "armed=%u mapped=%u\n",
			     zx_pon_armed, zx_pon_hw ? 1 : 0);
	if (!zx_pon_hw)
		return len + sysfs_emit_at(buf, len, "no-mmio\n");

	/*
	 * Read-only on purpose.  The first attempt to poke this block took the
	 * board off the bus, so the diag attribute must never write a register:
	 * it reports what the wire says right now.
	 */
	mode = zx_xmac_r(0x16644);
	if (zxpon_omcc_gem(&zx_state, &gem) && gem <= 0xfffe) {
		zx_xmac_w(ZX_GEM_TABLE_ADDR, gem | ZX_GEM_TABLE_READ);
		gem_word = zx_xmac_r(ZX_GEM_TABLE_DATA);
	}
	len += sysfs_emit_at(buf, len,
		"16644=%08x mode=%u 16635=%08x 16100=%08x 16000=%08x 16001=%08x\n",
		mode, mode & 0xf, zx_xmac_r(0x16635), zx_xmac_r(0x16100),
		zx_xmac_r(0x16000), zx_xmac_r(0x16001));
	len += sysfs_emit_at(buf, len,
		"tcont0=%08x ploam_tx_kick=%08x rx_pending=%08x gem%u=%02x\n",
		zx_xmac_r(ZX_TCONT_BANK), zx_xmac_r(ZX_PON_TX_KICK),
		zx_xmac_r(ZX_PON_RX_PENDING), gem, gem_word);
	len += sysfs_emit_at(buf, len,
		"1611d=%08x 1610b=%08x 16206=%08x 1688e=%08x 16600=%08x\n",
		zx_xmac_r(0x1611d), zx_xmac_r(0x1610b), zx_xmac_r(0x16206),
		zx_xmac_r(0x1688e), zx_xmac_r(0x16600));
	return len;
}
static DEVICE_ATTR_RO(pon_probe);

/*
 * Bench register access for the PON block.
 *
 * Bringing this block up cannot be done blind: the first attempt to drive it
 * took the whole board off the bus, and the vendor's init is a 60-step
 * sequence whose effect on each status register is unknown here.  So instead
 * of a monolithic bring-up, this attribute lets the bench do one thing at a
 * time and look at the answer:
 *
 *   echo "r 16100"          > pon_reg    read one register
 *   echo "w 16100 28"       > pon_reg    write one register
 *   echo "m 16100 0 1"      > pon_reg    read-modify-write (clear, set)
 *   echo "s 16000 16100 4"  > pon_reg    scan a range, step 4
 *   cat pon_reg                          show what the last command saw
 *
 * Every access is also printed at KERN_EMERG, so a stall shows up on the
 * serial console with the exact register in front of it.
 */
static char zx_reg_log[4096];
static size_t zx_reg_log_len;

static void zx_reg_emit(const char *fmt, ...)
{
	va_list args;
	int room;
	int n;

	room = (int)sizeof(zx_reg_log) - (int)zx_reg_log_len - 1;
	if (room <= 1)
		return;
	va_start(args, fmt);
	n = vscnprintf(zx_reg_log + zx_reg_log_len, room, fmt, args);
	va_end(args);
	zx_reg_log_len += n;
}

static ssize_t pon_reg_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	char cmd[16];
	unsigned int a, b, step;
	int n;

	if (!zx_pon_hw)
		return -ENODEV;

	n = sscanf(buf, "%15s %x %x %x", cmd, &a, &b, &step);
	if (n < 2)
		return -EINVAL;

	zx_reg_log_len = 0;
	zx_reg_log[0] = 0;

	if (!strcmp(cmd, "r")) {
		u32 v = zx_xmac_r(a);

		zx_reg_emit("%05x = %08x\n", a, v);
		pr_emerg("zxpon: r %05x = %08x\n", a, v);
	} else if (!strcmp(cmd, "w") && n >= 3) {
		pr_emerg("zxpon: w %05x <- %08x\n", a, b);
		zx_xmac_w(a, b);
		zx_reg_emit("%05x <- %08x (now %08x)\n", a, b, zx_xmac_r(a));
		pr_emerg("zxpon: w %05x now %08x\n", a, zx_xmac_r(a));
	} else if (!strcmp(cmd, "m") && n >= 4) {
		u32 was = zx_xmac_r(a);

		pr_emerg("zxpon: m %05x was %08x\n", a, was);
		zx_xmac_w(a, (was & ~b) | step);
		zx_reg_emit("%05x %08x -> %08x (clear %08x set %08x)\n",
			    a, was, zx_xmac_r(a), b, step);
		pr_emerg("zxpon: m %05x now %08x\n", a, zx_xmac_r(a));
	} else if (!strcmp(cmd, "s")) {
		unsigned int i;

		if (!step)
			step = 4;
		if (b < a || b - a > 0x800)
			return -EINVAL;
		pr_emerg("zxpon: s %05x..%05x/%x\n", a, b, step);
		for (i = a; i <= b; i += step) {
			u32 v = zx_xmac_r(i);

			zx_reg_emit("%05x=%08x%s", i, v,
				    (((i - a) / step) % 4 == 3) ? "\n" : " ");
		}
		zx_reg_emit("\n");
		pr_emerg("zxpon: scan done\n");
	} else {
		return -EINVAL;
	}
	return count;
}

static ssize_t pon_reg_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	if (!zx_reg_log_len)
		return sysfs_emit(buf, "no command yet\n");
	return sysfs_emit(buf, "%s", zx_reg_log);
}
static DEVICE_ATTR_RW(pon_reg);

/* The last few management frames, request and reply, in order. */
static ssize_t pon_omci_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	static const char hex[] = "0123456789abcdef";
	ssize_t len = 0;
	u32 start, i, j;

	mutex_lock(&zx_pon_lock);
	len += sysfs_emit_at(buf, len, "rx=%u tx=%u drop=%u q=%d n=%u\n",
		zx_omci_rx_n, zx_omci_tx_n, zx_omci_drop_n,
		zx_state.omci_tx_n, zx_omci_log_n);
	start = (zx_omci_log_head + ZX_OMCI_LOG_N - zx_omci_log_n) %
		ZX_OMCI_LOG_N;
	for (i = 0; i < zx_omci_log_n && len < (int)PAGE_SIZE - 112; i++) {
		u32 slot = (start + i) % ZX_OMCI_LOG_N;

		len += sysfs_emit_at(buf, len, "%c ", zx_omci_log_dir[slot]);
		for (j = 0; j < ZXPON_OMCI_LEN; j++)
			len += sysfs_emit_at(buf, len, "%c%c", hex[zx_omci_log[slot][j] >> 4],
				hex[zx_omci_log[slot][j] & 0xf]);
		len += sysfs_emit_at(buf, len, "\n");
	}
	mutex_unlock(&zx_pon_lock);
	return len;
}
static DEVICE_ATTR_RO(pon_omci);

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
	/* A new identity is a new provisioning attempt. */
	zx_gem_retry_delay = 0;
	zx_gem_notes = 0;
	mutex_unlock(&zx_pon_lock);
	zxpon_apply_carrier();
	return 0;
}

/*
 * Add a T-CONT: the AllocID comes from the OLT, the T-CONT id does not, so the
 * driver picks the next free slot.  Attribute "omcc" pins the management
 * T-CONT, which the vendor also hardwires to id 0.
 */
static int zxpon_add_tcont_cmd(struct sk_buff *skb, struct genl_info *info)
{
	u16 alloc, tcont;
	u32 readback = 0;
	bool omcc = false;
	int ret;

	if (!info->attrs[ZXPON_A_ALLOC])
		return -EINVAL;
	alloc = nla_get_u16(info->attrs[ZXPON_A_ALLOC]);
	if (info->attrs[ZXPON_A_OMCC])
		omcc = nla_get_u8(info->attrs[ZXPON_A_OMCC]) != 0;
	mutex_lock(&zx_pon_lock);
	if (omcc) {
		tcont = ZX_TCONT_OMCC;
	} else {
		ret = zx_tcont_alloc_id(&tcont);
		if (ret) {
			mutex_unlock(&zx_pon_lock);
			return ret;
		}
	}
	ret = zx_tcont_program(tcont, alloc, &readback);
	if (!ret)
		ret = zx_tcont_record(tcont, alloc, readback);
	mutex_unlock(&zx_pon_lock);
	if (ret)
		pr_warn("zxpon: T-CONT %u alloc %u rejected (%d), readback %08x\n",
			tcont, alloc, ret, readback);
	return ret;
}

/*
 * Mark a GEM port present in the downstream table, and (MAP_GEM only) let the
 * ordinary software mapping know it carries data.
 */
static int zxpon_map_cmd(struct sk_buff *skb, struct genl_info *info)
{
	u16 gem;
	u8 value = 0;
	int ret;

	if (!info->attrs[ZXPON_A_GEM])
		return -EINVAL;
	gem = nla_get_u16(info->attrs[ZXPON_A_GEM]);
	mutex_lock(&zx_pon_lock);
	ret = zx_gem_table_program(gem, &value);
	if (!ret) {
		zx_gem_record(gem, 0, value);
		if (info->genlhdr->cmd == ZXPON_CMD_MAP_GEM)
			zxpon_map_gem(&zx_state, gem);
	}
	mutex_unlock(&zx_pon_lock);
	if (ret)
		pr_warn("zxpon: GEM %u table write rejected (%d), readback %02x\n",
			gem, ret, value);
	return ret;
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

/*
 * Bench injection of a downstream OMCI frame, the counterpart of the PLOAM
 * verb above.  The kernel already answers OMCI (MIB upload, get, set, create),
 * but with no fibre nothing ever calls that code, so the whole management
 * exchange - including whether the reply leaves through the IDM queue - is
 * untestable on the bench without this.  The frame is handed to the ordinary
 * zxpon_rx_omci() and the reply is drained immediately, so the counters and
 * the pon_omci ring show the same bytes a live OLT would get.
 */
static int zxpon_omci_rx_cmd(struct sk_buff *skb, struct genl_info *info)
{
	static bool noted;
	u8 frame[ZXPON_OMCI_LEN];
	int ret;

	if (!info->attrs[ZXPON_A_OMCI_FRAME])
		return -EINVAL;
	memcpy(frame, nla_data(info->attrs[ZXPON_A_OMCI_FRAME]), ZXPON_OMCI_LEN);
	mutex_lock(&zx_pon_lock);
	ret = zxpon_rx_omci(&zx_state, frame);
	if (!ret) {
		zx_omci_log_put('R', frame);
		zx_omci_rx_n++;
	}
	mutex_unlock(&zx_pon_lock);
	if (ret < 0)
		return -EBADMSG;
	if (!noted) {
		noted = true;
		pr_info("zxpon: bench OMCI injection in use; downstream management frames now come from genl\n");
	}
	zxpon_drain_omci();
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
	{
		.cmd = ZXPON_CMD_OMCI_RX,
		.flags = GENL_ADMIN_PERM,
		.policy = zxpon_policy,
		.doit = zxpon_omci_rx_cmd,
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
	/*
	 * Nothing here touches the PON block at probe time.  Its clocks are off
	 * in this tree and the first access stalls the bus, so the hardware side
	 * is armed on request from userspace instead (see pon_arm_store).
	 */
	dev_info(&ndev->dev, "PON hardware idle until pon_arm is written\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_state))
		dev_warn(&ndev->dev, "pon_state sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_omci_transport))
		dev_warn(&ndev->dev, "omci_transport sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_hw))
		dev_warn(&ndev->dev, "pon_hw sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_tables))
		dev_warn(&ndev->dev, "pon_tables sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_probe))
		dev_warn(&ndev->dev, "pon_probe sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_arm))
		dev_warn(&ndev->dev, "pon_arm sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_reg))
		dev_warn(&ndev->dev, "pon_reg sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_omci))
		dev_warn(&ndev->dev, "pon_omci sysfs not created\n");
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
		device_remove_file(&zx_wan->dev, &dev_attr_pon_tables);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_probe);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_arm);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_reg);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_omci);
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
