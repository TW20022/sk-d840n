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
#include <linux/hex.h>
#include <linux/if_arp.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/leds.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/random.h>
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
 * driven until the BOSA analogue front end is initialised.  On an untouched
 * module the pin floats low; once the UX3361 MCU is loaded it drives the pin
 * as RX_LOS (hi = dark fiber, lo = light present), which is why the binding
 * is GPIO_ACTIVE_LOW.
 *
 * A mainline boot that never loads the BOSA datas therefore reads the pin as
 * "light present", note_rxsd(1) clears z->los, and the PLOAM machine can try
 * O2/O3 against a fiber that may not be there at all - the O1 timer falls
 * back when downstream frames never arrive.
 *
 * Once an identity is configured we arm the activation and send the serial
 * number, letting the PLOAM state machine find out whether the OLT is really
 * above.  The LED still follows the raw GPIO, so the panel lamp stays honest.
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
 * Per-message counters for the only downstream messages that mark a step
 * forward.  npu.ko's own PON status block keeps the same two (key_ctrl_rx,
 * key_report_tx) alongside the serial-number counters, and for a good reason:
 * once the OLT has assigned an ONU-ID, whether it followed up with Key
 * Control is the difference between "registered" and "registered and
 * provisioned".  Counting the IDs where the frames actually arrive keeps the
 * number honest even when the state machine rejects the frame (wrong MIC,
 * wrong ONU-ID) - the raw histogram in pon_rxraw shows the same thing, but a
 * single counter is what a one-line verdict can use.
 */
static u32 zx_ploam_assign_n;		/* DS 0x03 Assign_ONU-ID seen */
static u32 zx_ploam_key_ctrl_n;		/* DS 0x0d Key Control seen */
static u32 zx_ploam_key_rep_n;		/* US 0x05 Key Report handed to MAC */
static u32 zx_ploam_regid_n;		/* US 0x02 Registration handed to MAC */

/*
 * Every downstream PLOAM frame the state machine was handed, from either
 * source: the MAC receive ring or the bench injection verb.  zx_ploam_rx_n
 * counts only the former, so a stage verdict built on it would read
 * "no_downstream_ploam" while a rehearsal was happily learning profiles -
 * which is exactly what happened the first time this file was exercised.
 */
static u32 zx_ploam_ds_n;

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

/*
 * The same idea for the PLOAM channel, and here it is not a convenience.
 * Message id 0x01 (Profile, G.987.3 11.3.3.1 - the GPON name
 * Upstream_Overhead does not apply to XG-PON) is the only thing on the wire
 * that says how an upstream burst has to be timed, and every field of it has
 * to be read from a real OLT at least once.  A session with a fibre lasts
 * minutes and nobody is watching at that moment, so the frames are kept
 * instead: 32 deep covers a full O2-3 -> O4 -> O5 walk with room to spare, and
 * the file is plain hex so it can be pasted straight back into
 * "zxponctl ploam" for a replay on the bench with no fibre at all.
 */
#define ZX_PLOAM_LOG_N		32
static u8 zx_ploam_log[ZX_PLOAM_LOG_N][ZXPON_PLOAM_LEN];
static u8 zx_ploam_log_dir[ZX_PLOAM_LOG_N];
static u32 zx_ploam_log_head, zx_ploam_log_n, zx_ploam_log_drop_n;

/*
 * Raw downstream message-ID histogram, taken at the hardware RX ring before
 * MIC verification and zxpon_for_us() filtering.  pon_ploam only shows frames
 * that passed the CMAC check, so a Serial-Number Request arriving on a
 * different key would be invisible there.  This counter cannot lie about it.
 */
static u32 zx_rx_raw_id_hist[256];
static u8 zx_rx_raw_last[256][8];

/*
 * Bookkeeping for one downstream frame, whatever brought it in.  Both the MAC
 * receive loop and the bench injection verb call this, so the counters and
 * the raw histogram mean the same thing in a rehearsal and on a live fibre.
 * The first cut of pon_stage counted inside the receive loop only and then
 * reported "no Assign_ONU-ID" while the state machine was already in O5.
 */
static void zx_ploam_note_ds(const u8 *frame)
{
	zx_ploam_ds_n++;
	zx_last_rx_id = frame[2];
	zx_rx_raw_id_hist[frame[2]]++;
	if (frame[2] == ZXPON_DS_ASSIGN_ONUID)
		zx_ploam_assign_n++;
	else if (frame[2] == ZXPON_DS_KEY_CONTROL)
		zx_ploam_key_ctrl_n++;
	memcpy(zx_rx_raw_last[frame[2]], frame, 8);
}

/*
 * Bench override for the optical signal detect.
 *
 * With no fibre the opsd GPIO reads dark, note_rxsd() keeps los == 1, and
 * zxpon_rx_ploam() drops every frame before the state machine can see it.
 * That is correct on a live line and useless on a desk: the whole
 * O2-3 -> O4 -> O5 path stays untestable the moment the fibre is pulled.
 * "light" asserts exactly the los == 0 a lit fibre produces, "dark" the
 * opposite, "auto" hands control back to the GPIO.  It only moves the software
 * flag, never the optic, so it cannot fake a working link by itself.
 */
static int zx_bench_light = -1;

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

	/* XG-PON: mode nibble, preamble repeats, preamble pattern.
	 *
	 * History: we briefly tried forcing the mode nibble to 3 by writing
	 * 0x16006 (the vendor passes index 6 to Hal_WriteXmacReg, which could
	 * arguably alias 0x16006).  Measured on a working HX5-9saLite at O5
	 * on the same fibre: 0x16006 reads 0x12, i.e. the mode nibble is 2,
	 * not 3.  Forcing 3 is therefore wrong and has been reverted; only
	 * the legacy index-6 write stays, exactly as the vendor brings the
	 * block up. */
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
	/*
	 * Downstream must be ON from the moment the MAC is armed, not from O5.
	 * 0x16100 bit 0 is the downstream enable, and the OLT's broadcast
	 * PLOAM (burst profile, message id 0x01) is what has to arrive on it
	 * for O3/O4 to happen at all.
	 *
	 * What 2026-11-26 actually established, separated from what it did not:
	 *   PROVEN   with bit 0 clear the register read 0x63a; with light on the
	 *            fibre 0x16105 bit 4 (downstream lost) was set at 0x10.
	 *   PROVEN   with bit 0 set 0x16100 reads 0x63b and, while the feed was
	 *            live (-17.08..-17.21 dBm), 0x16105 read 0x02 - the same
	 *            value the working HX5 reports at O5. Both were read back
	 *            in the same boot (bosa_boot.log t=251/255/279 s).
	 *   NOT LOGGED  "rx started climbing" was seen on the serial console but
	 *            no capture file from that light-present boot survives. The
	 *            surviving ploam_capture.log belongs to a different, dark
	 *            boot (state=O1 throughout) and must NOT be used to argue
	 *            either way. pon_rxwatch.sh exists to settle this on the
	 *            next fibre insertion.
	 * The vendor also sets the two config bits (0x28) here.
	 */
	zx_xmac_rmw(0x16100, 0, 0x29);
	/* Vendor writes byte0=2 then byte1=0x0f here, which is exactly the
	 * value this board's loader already leaves (0x0f02). */
	val = (zx_xmac_r(0x16644) & ~0xffffu) | 0x0f02u;
	zx_xmac_w(0x16644, val);
	/* Upstream mode: REGISTRATION, not data.
	 *
	 * fi_Configure_Ponmac_Set_Upstream_Enable() has two modes:
	 *   arg 0 -> clear bit1, set bit0     (PLOAM/registration uplink)
	 *   arg 1 -> set bit1, clear bit0/3   (user data uplink)
	 * and states 0x15 / 0x16 respectively once bit2+bit4 are set.
	 *
	 * Measured today on a working HX5-9saLite on the same fibre:
	 *   O1 / O2-3 (unregistered): 0x16600 = 0x15
	 *   O4 / O5   (registered):   0x16600 = 0x16
	 * This driver used to write the arg-1 (data) word right here in
	 * Hw_Init, so bit0 - the PLOAM uplink enable - was never set during
	 * registration.  That is why the serial number latch 0x16611 bit0
	 * stayed pending for ever and bit1 (hardware "a burst really went
	 * out") never latched: the MAC had no uplink to put a burst in.
	 *
	 * bit4 (0x10) is part of both words - Hw_Init sets it and HX5 keeps
	 * it in every state - so it is set here as well.
	 */
	zx_xmac_rmw(0x16600, 0x2, 0x1 | 0x10);
	/* fi_power_init() also sets 0x1666f bit0; HX5 reads 0x1 here for the
	 * whole session, this board read 0x0. */
	zx_xmac_rmw(0x1666f, 0, 0x1);
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

/*
 * The vendor activation state machine keeps a hardware shadow of the software
 * state in 0x16500: low five bits are the O-state (O2-3 is 3), bits 16..25
 * the current ONU-ID.  Our board left that word at 0x03ff0000 for the whole
 * registration attempt, even though the software was in O2-3, and the MAC
 * never consumed an upstream grant.  Writing the same shadow the vendor
 * writes, together with its Alloc-ID 0x3fe and the default profile control
 * word, made the very first correctly addressed serial number leave the MAC.
 */
static int zx_hw_state_applied = -1;
static u32 zx_prof_hash;

static u32 zx_prof_mix(u32 h, u32 v)
{
	h ^= v;
	return h * 16777619u;
}

static u32 zx_profiles_hash(void)
{
	u32 h = 2166136261u;
	int i, j;

	for (i = 0; i < 4; i++) {
		const struct zxpon_profile *p = &zx_state.profile[i];

		h = zx_prof_mix(h, p->valid);
		if (!p->valid)
			continue;
		h = zx_prof_mix(h, p->version);
		h = zx_prof_mix(h, p->fec);
		h = zx_prof_mix(h, p->delimiter_len);
		h = zx_prof_mix(h, p->preamble_len);
		h = zx_prof_mix(h, p->preamble_repeat);
		for (j = 0; j < 8; j++) {
			h = zx_prof_mix(h, p->delimiter[j]);
			h = zx_prof_mix(h, p->preamble[j]);
		}
	}
	return h;
}

/* fi_Configure_Ponmac_Set_Profile(): the four words are byte-swapped. */
static void zx_pon_hw_put_bytes(u32 reg, const u8 *b)
{
	u32 v;

	memcpy(&v, b, 4);
	zx_xmac_w(reg, __swab32(v));
}

static void zx_pon_hw_apply_profiles_locked(void)
{
	u32 h = zx_profiles_hash(), bits = 0, pre_len = 0;
	int i;

	if (!zx_pon_hw || h == zx_prof_hash)
		return;
	zx_prof_hash = h;
	for (i = 0; i < 4; i++) {
		const struct zxpon_profile *p = &zx_state.profile[i];
		u32 ctrl;

		if (!p->valid)
			continue;
		/* Vendor: preamble_len | delimiter_len<<8 | repeat<<16. */
		ctrl = (p->preamble_len & 0xf) |
		       ((p->delimiter_len & 0xf) << 8) |
		       ((p->preamble_repeat & 0x1f) << 16);
		zx_xmac_w(0x16620 + 5 * i, ctrl);
		zx_pon_hw_put_bytes(0x16621 + 5 * i, p->preamble);
		zx_pon_hw_put_bytes(0x16622 + 5 * i, p->preamble + 4);
		zx_pon_hw_put_bytes(0x16623 + 5 * i, p->delimiter);
		zx_pon_hw_put_bytes(0x16624 + 5 * i, p->delimiter + 4);
		/* Valid bit at 2*i, FEC bit at 2*i+1 in byte 0 of 0x1650a. */
		bits |= 1u << (2 * i);
		if (p->fec)
			bits |= 1u << (2 * i + 1);
		pre_len = p->preamble_len & 0xf;
	}
	/* Byte 1 low nibble is the preamble length of the last profile. */
	zx_xmac_rmw(0x1650a, 0xffff, bits | (pre_len << 8));
}

static int zx_vendor_state(int s)
{
	switch (s) {
	case ZXPON_O1:	return 1;
	case ZXPON_O23:	return 3;
	case ZXPON_O4:	return 4;
	case ZXPON_O5:	return 5;
	case ZXPON_O6:	return 6;
	case ZXPON_O7:	return 7;
	default:	return 0;
	}
}

/*
 * 0x16600 upstream enable, re-asserted on every tick.
 *
 * Measured on this board: the MAC zeroes 0x16600 by itself the moment the
 * downstream locks and the ONU moves into O2-3.  The persisted monitor log
 * caught exactly that - one O2-3 sample read
 *
 *     st=O2-3 ... 16600=00000000 16500=00000003 sn_kick=0
 *
 * while the state was already correct.  This driver used to program this
 * register only when the PON state *changed*, so that single write landed
 * before the MAC cleared it and nothing ever restored it: the PLOAM uplink
 * stayed dead for the whole registration attempt.
 *
 * HX5-9saLite on the same fibre reads 0x15 in O1/O2-3 and 0x16 in O5, so
 * the whole byte is determined by the state:
 *     0x15 = bit0 (PLOAM uplink) | bit2 | bit4
 *     0x16 = bit1 (data uplink)  | bit2 | bit4
 * Only write when the live value differs, since this runs every 100 ms.
 */
static u32 zx_up_fix_n;

static void zx_pon_hw_upstream_ensure(int s)
{
	u32 cur, mask, set;

	if (!zx_pon_hw)
		return;
	cur = zx_xmac_r(0x16600);
	if (s == ZXPON_O5 || s == ZXPON_O6) {
		if ((cur & 0x1f) == 0x16)
			return;
		mask = 0x1 | 0x8;
		set  = 0x2 | 0x4 | 0x10;
	} else {
		if ((cur & 0x1f) == 0x15)
			return;
		mask = 0x2 | 0x8;
		set  = 0x1 | 0x4 | 0x10;
	}
	zx_xmac_rmw(0x16600, mask, set);
	zx_up_fix_n++;
}

static void zx_pon_hw_apply_state_locked(void)
{
	int s = zxpon_state(&zx_state);
	u32 onu, aid, val;

	zx_pon_hw_apply_profiles_locked();
	if (!zx_pon_hw)
		return;
	/* Before the "state unchanged" early-out on purpose: the MAC can
	 * clear the upstream word without the PON state moving at all. */
	zx_pon_hw_upstream_ensure(s);
	if (s == zx_hw_state_applied)
		return;
	zx_hw_state_applied = s;

	onu = zx_state.onu_id & 0x3ff;
	zx_xmac_rmw(0x16500, 0x03ff001f, (onu << 16) | zx_vendor_state(s));

	if (s == ZXPON_O4 || s == ZXPON_O5 || s == ZXPON_O6)
		zx_xmac_w(0x1650f, (zx_state.onu_id & 0x3fff) | 0x4000);
	else
		zx_xmac_w(0x1650f, 0);

	/*
	 * fi_Configure_SN_AllocId_Set(), decoded from zte_xgpon.ko at 0x7aac
	 * (the g_up_mode == 0x40 branch; the 0x80 branch above it is XGS):
	 *
	 *     addr = 0x16000 + 0x50f + (cputype == 5 ? 0x20 : 0x1e)
	 *     half = read(addr) & ~0x3fff | 0x3ff ; half |= valid << 14
	 *     write(addr, half)
	 *
	 * Two things here differed from what this driver used to do, and both
	 * stopped the MAC from ever recognising the OLT's serial-number grant:
	 *
	 *   - the broadcast Alloc-ID for an unregistered ONU is 0x3ff, not
	 *     0x3fe (0x3fe is the XGS-PON value in the other branch);
	 *   - the entry needs the valid bit, and the vendor writes exactly one
	 *     word - 0x1652d on this SoC - never the 0x1651f/0x1652e pair.
	 *
	 * Measured before this fix: latch 0x16611 stuck at 1, tx=0, sn_kick
	 * climbing with sn_done frozen at 0 for minutes on a live fibre.
	 */
	if (s == ZXPON_O5 || s == ZXPON_O6) {
		aid = zx_state.alloc_valid ? (zx_state.alloc_id & 0x3fff) : 0x3ff;
		val = (aid << 16) | 0x40000000u;
		zx_xmac_rmw(0x1652d, 0x7fff0000, val);
		zx_xmac_rmw(0x16520, 0x7fff0000, val);
	} else if (s == ZXPON_O23 || s == ZXPON_O4) {
		/* 0x43ff = valid (0x4000) | broadcast Alloc-ID 0x3ff.
		 * Only 0x1652d: the vendor writes that single word, and a
		 * working HX5 reads 0x0 at 0x1651f while unregistered. */
		zx_xmac_rmw(0x1652d, 0x7fff0000, 0x43ff0000u);
	}

	/* 0x16600 is handled by zx_pon_hw_upstream_ensure() above, which runs
	 * every tick instead of only on a state change - see the comment there
	 * for why a one-shot write is not enough. */
}

static int zx_profiles_valid(void)
{
	int i;

	for (i = 0; i < 4; i++)
		if (zx_state.profile[i].valid)
			return 1;
	return 0;
}

/*
 * The serial-number answer is not a normal PLOAM kick.  The vendor's
 * setSnMIC() writes the CMAC into 0x1663e/f and then sets 0x16611 bit 0;
 * the SN octets themselves live in 0x16636/7, written when the identity
 * was installed.  Kicking 0x16642 with a hand-built SN frame made the MAC
 * wait forever for a grant that never came, while the real path left once
 * as soon as these dedicated registers were filled.  It is only armed
 * after a Profile frame, exactly as p_o2_3_profile_msg() does it.
 */
static u32 zx_sn_kick_n, zx_sn_rearm_n, zx_sn_done_n, zx_sn_noprof_n;
static int zx_sn_armed, zx_sn_hw_sent;

/*
 * fi_Configure_Ponmac_Set_Random_Delay() in the vendor module mixes
 * get_random_bytes() with jiffies and two XMAC registers and reduces the
 * result modulo 0x1d28f before writing it to 0x16503.  The ONU uses that
 * value to place its serial-number burst at a random offset inside the
 * OLT's registration grant, so two ONUs powering up together do not
 * collide.
 */
static u32 zx_sn_rand_delay(void)
{
	u32 r = get_random_u32();

	r ^= (u32)jiffies;
	r ^= zx_xmac_r(0x16637);
	r ^= zx_xmac_r(0x16521);
	return r % 0x1d28fu;
}

static int zx_pon_hw_ploam_tx_sn(const u8 frame[ZXPON_PLOAM_LEN])
{
	u32 lo, hi, latched, reg;

	if (!zx_pon_hw)
		return -ENODEV;
	zx_pon_hw_apply_profiles_locked();
	/*
	 * The vendor's setSnMIC() never consults the burst profiles: it writes
	 * SN/MIC/slot and kicks.  Bailing out with -EBUSY when no profile had
	 * been received yet was wrong and fatal during O2-3 - the OLT may not
	 * have delivered one before it asks for the serial number, and the
	 * persisted log shows exactly that (st=O2-3 with sn_kick=0, i.e. not a
	 * single attempt was ever made).  Hw_Init already programmed the
	 * default registration profile, which is what this burst uses.
	 */
	if (!zx_profiles_valid())
		zx_sn_noprof_n++;
	reg = zx_xmac_r(0x16611);
	latched = reg & 1;
	/* bit1 is a sticky "the MAC has actually put a serial number on the
	 * fibre at least once" flag: a working HX5-9saLite reads 0x3 here on
	 * the same fibre, while this board never leaves 0x1.  It survives a
	 * LOS/O1 fallback, so it is the honest proof we are looking for. */
	if (reg & 2)
		zx_sn_hw_sent = 1;
	/* The latch dropping between two kicks is the only proof that the
	 * MAC really put a serial number on the fibre. */
	if (zx_sn_armed && !latched)
		zx_sn_done_n++;
	zx_sn_armed = 1;
	/*
	 * 0x16611 bit0 is a latch, not a pulse register.  The MAC clears it
	 * once it has actually put the burst on the fibre; with no grant it
	 * stays set forever.  The vendor calls setSnMIC() once per Profile, so
	 * if that single window is missed the MAC is never asked again.
	 * We re-arm on every Profile and once a second, but writing 1 to a bit
	 * that is already 1 is not a new request - measured on the live line
	 * the SN was kicked exactly once (rx climbing, tx=0, 0x16611 stuck at
	 * 1).  Drop the stale latch before re-arming so each Profile really
	 * produces a 0->1 edge.  The clear and the set are a few register
	 * writes apart, far inside the OLT's SN window, so an in-flight burst
	 * is not endangered.
	 */
	if (latched) {
		zx_xmac_rmw(0x16611, 0x1, 0);
		zx_sn_rearm_n++;
	}
	/*
	 * Program the random delay every time, exactly like the vendor's
	 * Set_Random_Delay() right before setSnMIC().  This driver used to
	 * leave 0x16503 at whatever the bootloader left (0 on this board);
	 * the O2-3 snapshot of a working HX5-9saLite on the same fibre shows
	 * 0x7391 there.  A zero delay is not a "send immediately" request -
	 * the MAC simply never schedules the burst.
	 */
	zx_xmac_w(0x16503, zx_sn_rand_delay());
	memcpy(&lo, frame + 40, 4);
	memcpy(&hi, frame + 44, 4);
	zx_xmac_w(0x1663e, __swab32(lo));
	zx_xmac_w(0x1663f, __swab32(hi));
	zx_xmac_rmw(0x16611, 0, 0x1);
	zx_sn_kick_n++;
	return 0;
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
		/*
		 * Only the upstream data half is closed here.  0x16100 bit 0
		 * (downstream enable) stays set: leaving O5 must not blind the
		 * receiver, or the OLT's next burst profile - the only thing
		 * that can bring the ONU back - could never be heard again.
		 */
		zx_xmac_rmw(0x16600, 0x2, 0x1);
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
/*
 * Upstream PLOAM transmission is a request, not a register write that
 * completes on its own.  Writing TX_DATA and setting TX_KICK hands the burst
 * to the MAC; the MAC then waits for the OLT to open a grant window for it.
 * That window can be a whole superframe away, and for an unregistered ONU it
 * is the only thing that makes the burst possible at all.
 *
 * Polling the bit a few times inside the same call and clearing it on failure
 * therefore cancels the burst before it can ever be sent.  Measured on a live
 * line on 2026-11-26: tx froze at 10 while txerr climbed by one per tick, the
 * OLT kept broadcasting burst profiles and never once addressed our ONU-ID -
 * it had never seen a serial number, because every one of them was withdrawn
 * microseconds after it was requested.
 *
 * So kick and return.  zx_pon_hw_tx_reap() watches the bit on later ticks and
 * counts the burst when the MAC clears it.  The bit is only cleared here once
 * the MAC has evidently dropped it; the vendor's own completion path is an
 * interrupt, and five seconds is many thousand superframes.
 */
#define ZX_TX_REAP_TICKS 50	/* 5 s at the 100 ms tick */

static int zx_tx_inflight;
static unsigned int zx_tx_wait_ticks;

static int zx_pon_hw_ploam_tx(const u8 frame[ZXPON_PLOAM_LEN])
{
	int i;

	if (zx_xmac_r(ZX_PON_TX_KICK) & 1)
		return -EBUSY;		/* previous burst still waiting */
	for (i = 0; i < ZXPON_PLOAM_LEN / 4; i++) {
		u32 word;

		memcpy(&word, frame + i * 4, 4);
		zx_xmac_w(ZX_PON_TX_DATA + i, __swab32(word));
	}
	zx_xmac_w(ZX_PON_TX_KICK, 1);
	zx_tx_inflight = 1;
	zx_tx_wait_ticks = 0;
	return 0;
}

/* Did the burst we kicked last time actually leave?  Called once per tick. */
static void zx_pon_hw_tx_reap(void)
{
	if (!zx_tx_inflight || !zx_pon_hw)
		return;
	if (!(zx_xmac_r(ZX_PON_TX_KICK) & 1)) {
		zx_tx_inflight = 0;
		zx_ploam_tx_n++;	/* the MAC took it out on a grant */
		return;
	}
	if (++zx_tx_wait_ticks >= ZX_TX_REAP_TICKS) {
		zx_xmac_w(ZX_PON_TX_KICK, 0);
		zx_tx_inflight = 0;
		zx_ploam_txerr_n++;
	}
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

/* 'R' = receiver, 'T' = transmitter, 'I' = injected from the bench.
 *
 * A live OLT repeats its broadcast messages several times a second, so storing
 * every copy would fill the ring with one identical frame and bury the rare
 * one - Assign_ONU_ID arrives once and matters most.  A frame already in the
 * ring is therefore counted and dropped, which leaves 32 distinct frames.
 */
static void zx_ploam_log_put(u8 dir, const u8 *frame)
{
	u32 i;

	for (i = 0; i < zx_ploam_log_n; i++) {
		u32 slot = (zx_ploam_log_head + ZX_PLOAM_LOG_N - 1 - i) %
			ZX_PLOAM_LOG_N;

		if (!memcmp(zx_ploam_log[slot], frame, ZXPON_PLOAM_LEN)) {
			zx_ploam_log_drop_n++;
			return;
		}
	}
	memcpy(zx_ploam_log[zx_ploam_log_head], frame, ZXPON_PLOAM_LEN);
	zx_ploam_log_dir[zx_ploam_log_head] = dir;
	zx_ploam_log_head = (zx_ploam_log_head + 1) % ZX_PLOAM_LOG_N;
	if (zx_ploam_log_n < ZX_PLOAM_LOG_N)
		zx_ploam_log_n++;
	else
		zx_ploam_log_drop_n++;
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
	zx_pon_hw_tx_reap();
	for (i = 0; i < 4; i++) {
		if (!zx_pon_hw_ploam_rx(frame))
			break;
		zx_ploam_rx_n++;
		zx_ploam_note_ds(frame);
		zx_ploam_log_put('R', frame);
		zxpon_rx_ploam(&zx_state, frame);
	}
	for (i = 0; i < 4; i++) {
		int ret;

		if (!zxpon_peek_ploam(&zx_state, frame))
			break;
		/* Serial Number has its own hardware path; see above. */
		ret = frame[2] == ZXPON_US_SERIAL_NUMBER ?
			zx_pon_hw_ploam_tx_sn(frame) : zx_pon_hw_ploam_tx(frame);
		if (ret) {
			/* -EBUSY between bursts is the normal state, not a fault. */
			if (ret == -EBUSY)
				zx_ploam_busy_n++;
			else
				zx_ploam_txerr_n++;
			break;
		}
		zxpon_take_ploam(&zx_state, frame);
		if (frame[2] == ZXPON_US_KEY_REPORT)
			zx_ploam_key_rep_n++;
		else if (frame[2] == ZXPON_US_REGISTRATION)
			zx_ploam_regid_n++;
		zx_ploam_log_put('T', frame);
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
	static int prev_state;

	mutex_lock(&zx_pon_lock);
	if (zx_alive) {
		int st;

		zxpon_tick(&zx_state, 100);
		/*
		 * The per-message counters below describe one activation, not the
		 * life of the driver.  An OLT that deactivates us and starts over
		 * must not inherit "key_ctrl_rx=1" from the previous round: that
		 * is precisely the flag a live session will be read for, and a
		 * stale one would make a broken round look complete.
		 */
		st = zxpon_state(&zx_state);
		if (st == ZXPON_O1 && prev_state != ZXPON_O1) {
			zx_ploam_assign_n = 0;
			zx_ploam_key_ctrl_n = 0;
			zx_ploam_key_rep_n = 0;
			zx_ploam_regid_n = 0;
		}
		prev_state = st;
		if (++sn_tick >= 10) {
			sn_tick = 0;
			zxpon_retry_sn(&zx_state);
		}
		zxpon_poll_ploam();
		/* Mirror the state and every learned profile into the MAC in
		 * the same tick that changed them: the vendor does this inside
		 * its activation handlers, and without it the MAC may see a
		 * grant window but has no profile or Alloc-ID to match.
		 */
		zx_pon_hw_apply_state_locked();
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
	int alloc_ok, hs;
	u32 prof;

	mutex_lock(&zx_pon_lock);
	alloc_ok = zxpon_alloc_id(&zx_state, &alloc);
	hs = zx_hw_state_applied;
	prof = zx_xmac_r(0x1650a);
	mutex_unlock(&zx_pon_lock);
	return sysfs_emit(buf,
		"mapped=%u rx_pending=%08x tx_kick=%08x "
		"rx=%u tx=%u txerr=%u busy=%u last_rx_id=%02x data=%u micbad=%u "
		"alloc=%04x alloc_valid=%u hwstate=%d profbits=%04x "
		"sn_kick=%u sn_rearm=%u sn_done=%u sn_latch=%u sn_sent=%u "
		"up_fix=%u noprof=%u\n",
		zx_pon_hw ? 1 : 0, zx_xmac_r(ZX_PON_RX_PENDING),
		zx_xmac_r(ZX_PON_TX_KICK), zx_ploam_rx_n, zx_ploam_tx_n,
		zx_ploam_txerr_n, zx_ploam_busy_n, zx_last_rx_id, zx_hw_data_on,
		zx_state.mic_bad_n, alloc, alloc_ok, hs, prof & 0xffff,
		zx_sn_kick_n, zx_sn_rearm_n, zx_sn_done_n,
		zx_pon_hw ? (zx_xmac_r(0x16611) & 1) : 0, zx_sn_hw_sent,
		zx_up_fix_n, zx_sn_noprof_n);
}
static DEVICE_ATTR_RO(pon_hw);

/*
 * Activation verdict on one screen.
 *
 * npu.ko (the ZWRT build of this same driver) keeps a debugfs status block
 * whose fields read like a checklist: init_stage, serdes_sequence,
 * sn_burst_tx_cnt, xgmac_sn_reg, last_ploam_id, key_ctrl_rx, key_report_tx,
 * regid_set.  Read in order they are the G.987.3 walk itself, and the value
 * of that layout is not the individual numbers - pon_hw already prints them -
 * but the fact that the first one that stops moving names the step that is
 * broken.
 *
 * This attribute does the same job for this board, from what it can actually
 * observe.  It deliberately does not invent a serdes_sequence: the 49-word
 * lane image lives at 0x16000000, outside the PON MAC mapping, and is owned
 * by phy-zx279133-pon-serdes; reporting a fake field would hide the one case
 * that matters (the image did not take).  Everything below is either a
 * counter that only moves when the hardware moved, or a register the MAC
 * itself maintains.
 *
 * Two registers are worth knowing:
 *   0x16611 bit0  SN kick latch - the MAC clears it once the burst left
 *                 (or, on a fibre with no grant, leaves it set forever);
 *                 bit1 is sticky "a serial number has been on the fibre".
 *   0x16500       the MAC's own shadow of the O-state (bits 4:0) and ONU-ID
 *                 (bits 25:16).  Comparing it with the software state is the
 *                 fastest way to see "software thinks O5, MAC never left
 *                 O2-3", which is exactly the failure this board had.
 */
static const char *zx_stage_name(int st, int los, u32 ds, int nprof, u32 kick,
				 int sent, u16 onu, u32 key_ctrl, u32 key_rep)
{
	bool onu_ok = onu != 0 && onu != ZXPON_BCAST_ONUID;

	if (los)
		return "o1_dark";
	if (!ds)
		return "o2_3_listen";
	if (!nprof)
		return "o2_3_no_profile";
	/*
	 * An assigned ONU-ID is proof on its own that the serial number left the
	 * ONU: the OLT only answers it after decoding a burst.  Checking the
	 * local latch first reported "sn_never_left" during a bench rehearsal
	 * that had reached O5, because without a real grant the MAC never clears
	 * 0x16611 bit 0.  On a live fibre both agree; with an injected OLT only
	 * this order is right.
	 */
	if (onu_ok) {
		if (st != ZXPON_O5)
			return "o4_ranging";
		if (!key_ctrl)
			return "o5_no_key_control";
		if (!key_rep)
			return "o5_key_report_missing";
		return "o5_keyed";
	}
	if (!kick)
		return "o2_3_sn_not_kicked";
	if (!sent)
		return "o2_3_sn_never_left";
	return "o3_sn_accepted";
}

/* The single next event the ONU is waiting for.  "none" means fully done. */
static const char *zx_stage_blocker(int st, int los, u32 ds, int nprof,
				    u32 kick, int sent, u16 onu, u32 key_ctrl,
				    u32 key_rep)
{
	bool onu_ok = onu != 0 && onu != ZXPON_BCAST_ONUID;

	if (los)
		return "no_light";
	if (!ds)
		return "no_downstream_ploam";
	if (!nprof)
		return "no_burst_profile";
	if (onu_ok) {
		if (st != ZXPON_O5)
			return "ranging_pending";
		if (!key_ctrl)
			return "no_key_control";
		if (!key_rep)
			return "no_key_report";
		return "none";
	}
	if (!kick)
		return "sn_not_queued";
	if (!sent)
		return "sn_never_on_fibre";
	return "no_onu_id";
}

static ssize_t pon_stage_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	const char *name, *blocker, *swname;
	u32 x16100 = 0, x16105 = 0, x16611 = 0, x16500 = 0, shadow_st = 0;
	u32 shadow_onu = 0, kick = 0, key_ctrl, key_rep, nprof = 0, bits = 0;
	int st, los, lods, n, sent;
	u16 onu, alloc;
	int alloc_ok;
	char line[9];

	if (!zx_pon_hw)
		return sysfs_emit(buf,
			"stage=hw_not_mapped blocker=hw_not_mapped\n"
			"hint: echo map > pon_arm\n");

	mutex_lock(&zx_pon_lock);
	st = zxpon_state(&zx_state);
	los = zx_state.los;
	lods = zx_state.lods;
	onu = zx_state.onu_id;
	kick = zx_sn_kick_n;
	sent = zx_sn_hw_sent;
	key_ctrl = zx_ploam_key_ctrl_n;
	key_rep = zx_ploam_key_rep_n;
	for (n = 0; n < 4; n++) {
		if (!zx_state.profile[n].valid)
			continue;
		nprof++;
		bits |= 1u << n;
	}
	x16100 = zx_xmac_r(0x16100);
	x16105 = zx_xmac_r(0x16105);
	x16611 = zx_xmac_r(0x16611);
	x16500 = zx_xmac_r(0x16500);
	name = zx_stage_name(st, los, zx_ploam_ds_n, nprof, kick, sent, onu,
			     key_ctrl, key_rep);
	blocker = zx_stage_blocker(st, los, zx_ploam_ds_n, nprof, kick, sent,
				   onu, key_ctrl, key_rep);
	swname = zxpon_state_name(&zx_state);
	alloc = zx_state.alloc_id;
	alloc_ok = zx_state.alloc_valid;
	/* The tick may hold this lock; the shadow is a plain read, so take it
	 * here rather than re-locking around four more accesses.
	 */
	mutex_unlock(&zx_pon_lock);

	for (n = 0; n < 8; n++)
		line[n] = (x16105 >> n) & 1 ? '1' : '0';
	line[8] = 0;
	shadow_st = x16500 & 0x1f;
	shadow_onu = (x16500 >> 16) & 0x3ff;

	return sysfs_emit(buf,
		"stage=%s blocker=%s\n"
		"sw state=%s los=%d lods=%d onu=%04x alloc=%04x/%d\n"
		"hw 16100=%08x 16105=%08x (%s b4=ds_lost)\n"
		"hw 16500=%08x mac_o_state=%u mac_onu=%03x\n"
		"sn kick=%u rearm=%u done=%u sent=%u latch=%u noprof=%u\n"
		"ploam ds=%u ring_rx=%u tx=%u busy=%u txerr=%u last_id=%02x\n"
		"ploam assign=%u key_ctrl_rx=%u key_report_tx=%u regid_set=%u\n"
		"profile learned=%u mask=%x\n",
		name, blocker, swname, los, lods, onu, alloc, alloc_ok,
		x16100, x16105, line,
		x16500, shadow_st, shadow_onu,
		kick, zx_sn_rearm_n, zx_sn_done_n, sent,
		(x16611 & 1), zx_sn_noprof_n,
		zx_ploam_ds_n, zx_ploam_rx_n, zx_ploam_tx_n, zx_ploam_busy_n,
		zx_ploam_txerr_n, zx_last_rx_id,
		zx_ploam_assign_n, key_ctrl, key_rep, zx_ploam_regid_n > 0,
		nprof, bits);
}
static DEVICE_ATTR_RO(pon_stage);

/* The OLT's burst profiles are the one input still missing from our upstream
 * path.  Expose exactly what G.987.3 gives us so a live capture can be read
 * without guessing a vendor register mapping. */
static ssize_t pon_profile_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct zxpon_profile p;
	char delim[32], pre[32], tag[32];
	int i, n = 0;

	for (i = 0; i < 4; i++) {
		mutex_lock(&zx_pon_lock);
		if (!zxpon_get_profile(&zx_state, i, &p)) {
			mutex_unlock(&zx_pon_lock);
			continue;
		}
		mutex_unlock(&zx_pon_lock);
		hex_dump_to_buffer(p.delimiter, 8, 8, 1, delim, sizeof(delim), 0);
		hex_dump_to_buffer(p.preamble, 8, 8, 1, pre, sizeof(pre), 0);
		hex_dump_to_buffer(p.pon_tag, 8, 8, 1, tag, sizeof(tag), 0);
		n += sysfs_emit_at(buf, n,
			"idx=%u ver=%u fec=%u delim_len=%u pre_len=%u "
			"pre_repeat=%u delim=%s pre=%s pon_tag=%s\n",
			p.index, p.version, p.fec, p.delimiter_len,
			p.preamble_len, p.preamble_repeat, delim, pre, tag);
	}
	if (!n)
		n = sysfs_emit(buf, "no profiles learned\n");
	return n;
}
static DEVICE_ATTR_RO(pon_profile);

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

/*
 * Every PLOAM frame that crossed the hardware channel, oldest first.  Read it
 * with a live fibre and it is a recording of what the OLT actually sends;
 * feed the same lines back through "zxponctl ploam" and the whole O2-3 ->
 * O4 -> O5 walk runs again with nothing plugged in.
 */
static ssize_t pon_ploam_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	static const char hex[] = "0123456789abcdef";
	ssize_t len = 0;
	u32 start, i, j;

	mutex_lock(&zx_pon_lock);
	len += sysfs_emit_at(buf, len,
		"rx=%u tx=%u busy=%u txerr=%u last_id=%02x log=%u drop=%u\n",
		zx_ploam_rx_n, zx_ploam_tx_n, zx_ploam_busy_n,
		zx_ploam_txerr_n, zx_last_rx_id, zx_ploam_log_n,
		zx_ploam_log_drop_n);
	start = (zx_ploam_log_head + ZX_PLOAM_LOG_N - zx_ploam_log_n) %
		ZX_PLOAM_LOG_N;
	for (i = 0; i < zx_ploam_log_n && len < (int)PAGE_SIZE - 112; i++) {
		u32 slot = (start + i) % ZX_PLOAM_LOG_N;

		len += sysfs_emit_at(buf, len, "%c ", zx_ploam_log_dir[slot]);
		for (j = 0; j < ZXPON_PLOAM_LEN; j++)
			len += sysfs_emit_at(buf, len, "%c%c",
				hex[zx_ploam_log[slot][j] >> 4],
				hex[zx_ploam_log[slot][j] & 0xf]);
		len += sysfs_emit_at(buf, len, "\n");
	}
	mutex_unlock(&zx_pon_lock);
	return len;
}
static DEVICE_ATTR_RO(pon_ploam);

/* Raw RX message-ID histogram: no MIC filter, no zxpon_for_us() filter. */
static ssize_t pon_rxraw_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	static const char hex[] = "0123456789abcdef";
	ssize_t len = 0;
	int i, j;

	len += sysfs_emit_at(buf, len, "raw downstream message IDs (pre-MIC):\n");
	for (i = 0; i < 256; i++) {
		if (!zx_rx_raw_id_hist[i])
			continue;
		len += sysfs_emit_at(buf, len, "id=%02x n=%u last=", i,
				     zx_rx_raw_id_hist[i]);
		for (j = 0; j < 8; j++)
			len += sysfs_emit_at(buf, len, "%c%c",
				hex[zx_rx_raw_last[i][j] >> 4],
				hex[zx_rx_raw_last[i][j] & 0xf]);
		len += sysfs_emit_at(buf, len, "\n");
	}
	return len;
}
static DEVICE_ATTR_RO(pon_rxraw);

static ssize_t pon_bench_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	if (zx_bench_light < 0)
		return sysfs_emit(buf, "auto\n");
	return sysfs_emit(buf, "%s\n", zx_bench_light ? "light" : "dark");
}

static ssize_t pon_bench_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int v;

	if (sysfs_streq(buf, "auto"))
		v = -1;
	else if (sysfs_streq(buf, "light"))
		v = 1;
	else if (sysfs_streq(buf, "dark"))
		v = 0;
	else
		return -EINVAL;

	zx_bench_light = v;
	if (zx_alive && v >= 0) {
		mutex_lock(&zx_pon_lock);
		zxpon_set_los(&zx_state, v ? 0 : 1);
		mutex_unlock(&zx_pon_lock);
		zxpon_apply_carrier();
	}
	return count;
}
static DEVICE_ATTR_RW(pon_bench);

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
	/*
	 * These are downstream frames, so the MIC has to be computed with the
	 * downstream direction prefix.  zxpon_ploam_seal() is the upstream
	 * helper (used when queuing our own replies) and sealing with it here
	 * made every injected frame fail zxpon_ploam_mic_ok() and come back as
	 * -EBADMSG, which is why the bench walk to O5 stopped working once the
	 * strict MIC check was restored on 2026-11-26.
	 */
	zxpon_ploam_seal_dir(frame, ZXPON_MIC_DS);
	mutex_lock(&zx_pon_lock);
	zx_ploam_note_ds(frame);
	zx_ploam_log_put('I', frame);
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
	if (zx_bench_light >= 0)
		light = zx_bench_light;
	/* opsd-gpios is bound GPIO_ACTIVE_LOW (measured: pin lo = light,
	 * pin hi = RX_LOS), so gpiod already hands us the logical signal
	 * detect and light == 1 means "frames can be received".
	 *
	 * Honour it unconditionally.  The old guard skipped light == 0 once
	 * the activation was armed, because with the previous ACTIVE_HIGH
	 * binding a dark fibre read as "lit" and would have kicked the PLOAM
	 * machine back to O1 every 200 ms.  With the polarity correct that
	 * guard would instead ignore a genuinely unplugged fiber, leaving a
	 * stale los == 0 behind.
	 */
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
	if (device_create_file(&ndev->dev, &dev_attr_pon_stage))
		dev_warn(&ndev->dev, "pon_stage sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_profile))
		dev_warn(&ndev->dev, "pon_profile sysfs not created\n");
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
	if (device_create_file(&ndev->dev, &dev_attr_pon_ploam))
		dev_warn(&ndev->dev, "pon_ploam sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_rxraw))
		dev_warn(&ndev->dev, "pon_rxraw sysfs not created\n");
	if (device_create_file(&ndev->dev, &dev_attr_pon_bench))
		dev_warn(&ndev->dev, "pon_bench sysfs not created\n");
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
		device_remove_file(&zx_wan->dev, &dev_attr_pon_stage);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_profile);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_tables);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_probe);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_arm);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_reg);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_omci);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_ploam);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_rxraw);
		device_remove_file(&zx_wan->dev, &dev_attr_pon_bench);
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

/* ------------------------------------------------------------------------
 * BOSA control bus: bit-banged I2C on the two pins the module's MCU is wired
 * to.  ZWRT-Beta-1.4.0 (f7005mv3, same SoC) does exactly this in device tree
 * with "i2c-gpio" and scl-gpios/sda-gpios; on this board the same two pads are
 * p0-6 (SCL) and p0-7 (SDA), i.e. gpio 519/520 (bank0 offset = pin + 1).  The
 * vendor's own firmware reaches the module at 0x50/0x51 over this bus.
 *
 * Registered as adapter 0 so the vendor userspace tool (zte-core: "ddm",
 * "calibrate") finds it exactly as it does on ZWRT, where /dev/i2c-0 is the
 * BOSA bus.  Open drain is emulated the way the pads need it: a logical 1 is
 * "release the line" (input, pull-up does the rest), a logical 0 drives low.
 * ---------------------------------------------------------------------- */
#define ZX_BOSA_SCL_GPIO	519
#define ZX_BOSA_SDA_GPIO	520

static struct gpio_desc *zx_bosa_scl_desc;
static struct gpio_desc *zx_bosa_sda_desc;
static struct i2c_algo_bit_data zx_bosa_algo;
static struct i2c_adapter zx_bosa_adap = {
	.owner		= THIS_MODULE,
	.class		= I2C_CLASS_HWMON,
	.nr		= 0,
	.name		= "zx279133-bosa",
};

static void zx_bosa_setsda(void *data, int state)
{
	if (!zx_bosa_sda_desc)
		return;
	if (state)
		gpiod_direction_input(zx_bosa_sda_desc);
	else
		gpiod_direction_output(zx_bosa_sda_desc, 0);
}

static void zx_bosa_setscl(void *data, int state)
{
	if (!zx_bosa_scl_desc)
		return;
	if (state)
		gpiod_direction_input(zx_bosa_scl_desc);
	else
		gpiod_direction_output(zx_bosa_scl_desc, 0);
}

static int zx_bosa_getsda(void *data)
{
	return zx_bosa_sda_desc ? gpiod_get_value(zx_bosa_sda_desc) : 0;
}

static int zx_bosa_getscl(void *data)
{
	return zx_bosa_scl_desc ? gpiod_get_value(zx_bosa_scl_desc) : 0;
}

static int __init zx_bosa_i2c_init(void)
{
	int ret;

	zx_bosa_scl_desc = gpio_to_desc(ZX_BOSA_SCL_GPIO);
	zx_bosa_sda_desc = gpio_to_desc(ZX_BOSA_SDA_GPIO);
	if (!zx_bosa_scl_desc || !zx_bosa_sda_desc) {
		pr_err("zx-bosa: gpio %d/%d not found, no BOSA bus\n",
		       ZX_BOSA_SCL_GPIO, ZX_BOSA_SDA_GPIO);
		return -ENODEV;
	}
	ret = gpio_request(ZX_BOSA_SCL_GPIO, "zx-bosa-scl");
	if (ret)
		pr_warn("zx-bosa: scl request %d\n", ret);
	ret = gpio_request(ZX_BOSA_SDA_GPIO, "zx-bosa-sda");
	if (ret)
		pr_warn("zx-bosa: sda request %d\n", ret);
	gpiod_direction_input(zx_bosa_scl_desc);
	gpiod_direction_input(zx_bosa_sda_desc);

	memset(&zx_bosa_algo, 0, sizeof(zx_bosa_algo));
	zx_bosa_algo.setsda = zx_bosa_setsda;
	zx_bosa_algo.setscl = zx_bosa_setscl;
	zx_bosa_algo.getsda = zx_bosa_getsda;
	zx_bosa_algo.getscl = zx_bosa_getscl;
	zx_bosa_algo.udelay = 10;
	zx_bosa_algo.timeout = HZ / 10;
	zx_bosa_adap.algo_data = &zx_bosa_algo;

	ret = i2c_bit_add_numbered_bus(&zx_bosa_adap);
	if (ret) {
		pr_err("zx-bosa: i2c_bit_add_numbered_bus failed %d\n", ret);
		return ret;
	}
	pr_info("zx-bosa: adapter %d on gpio %d/%d (scl/sda)\n",
		zx_bosa_adap.nr, ZX_BOSA_SCL_GPIO, ZX_BOSA_SDA_GPIO);
	return 0;
}
late_initcall(zx_bosa_i2c_init);
