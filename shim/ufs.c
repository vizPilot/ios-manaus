/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 2 (the main GPT; on MTK
 * LUN 0/1 are the preloader boot LUNs). Nothing is ever written to the device:
 * there is no SCSI WRITE in this file.
 *
 * v2 (2026-11-12): ABL leaves the controller enabled but the UniPro link
 * half-broken (PWR_LOCAL, UECPA pending), so a bare DME_LINK_STARTUP on that
 * leftover state returns FAILURE and every UPIU times out with OCS=5. First
 * replicate the Linux/MTK probe path: reference clock request, MTK host reset
 * through the syscon reset lines, HCE stop/start, DME_RESET + DME_ENABLE,
 * ufs_mtk_pre_link DME writes, then DME_LINK_STARTUP with the retry loop.
 * Only then touch the transfer request list.
 *
 * v3 (2026-11-12): ungate the UFS AO/PDN clock gates (ufs_mtk_init_clocks).
 * Result: ABL already left every gate open (ao_sta/pdn_sta = 0) so the link
 * failure was byte-identical. Disproven.
 *
 * v4 (2026-11-13): the missing step is the *device* reset. Linux
 * ufshcd_init() calls ufshcd_device_reset() unconditionally before
 * ufshcd_hba_enable(): ufs_mtk_device_reset() stops HCE, drives RST_n low via
 * the MTK SiP SMC UFS_MTK_SIP_DEVICE_RESET, waits ~12 us, drives it high and
 * waits ~12 ms. The live /proc/ufs_debug records exactly that event
 * (dev_reset[0] = 0x0 at ~1.43 s), and the shim has only ever reset the host,
 * never the device. Attempt 1 below is now Linux-exact (device reset + host
 * reset + HCE + pre_link + LINK_STARTUP, with NO DME_RESET/DME_ENABLE, which
 * the MTK HCE already performs in hardware); attempts 2/3 add those extras and
 * a second device reset so the outcomes can be compared inside one boot.
 *
 * The M-PHY is confirmed a red herring: ufshci@112b0000 has no `phys` node,
 * host->mphy is NULL and ufs_mtk_mphy_power_on() early-returns, so Linux never
 * writes REG_UFS_MPHYCTRL/REG_UFS_EXTREG. The host reset itself is correct:
 * ti,reset-bits flags 0x1c = ASSERT_SET|DEASSERT_SET|STATUS_NONE, so asserting
 * sets the bit at 0x44 and deasserting sets the bit at 0x48.
 *
 * v5 (2026-11-13): with the device reset in place the link came up on the first
 * attempt (VS_LINK_UP, HCS.DP set) and the UTP path works, but every SCSI
 * command returned CHECK CONDITION. The shim jumps straight from link startup
 * to SCSI; Linux ufshcd_probe_hba() first sends a NOP OUT (verify_dev_init)
 * and then sets and polls the fDeviceInit flag with QUERY REQUESTS
 * (complete_dev_init), which is what makes the logical units usable. v5 adds
 * device_init() for that handshake and dumps the response UPIU header and the
 * 18-byte SCSI sense data on a CHECK CONDITION so the sense key/ASC/ASCQ is
 * visible. The UTRD command type stays UTP_CMD_TYPE_UFS_STORAGE (1): that is
 * what Linux uses for UFSHCI 2.0 for BOTH SCSI and device-management commands
 * (ufshcd_comp_scsi_upiu/ufshcd_compose_devman_upiu branch on
 * ufs_version <= ufshci_version(1, 1) = 0x110, and the shim reads ver=0x200),
 * so the earlier "set the SCSI command type to 0" idea was wrong. SCSI commands
 * now drop the interrupt bit to match ufshcd_prepare_req_desc_hdr(), which
 * sets it only for device-management commands. Still read-only (no SCSI
 * WRITE).
 *
 * v6 (2026-11-13): factor the proven v5 bring-up into ufs_bringup(), so the
 * payload path can run it (MMU off) right before the jump to XNU when the
 * cmdline says rd=ufsroot0. XNU's sweet_ufs.c cannot bring the link up itself
 * (the bootloader leaves it half-broken and XNU has no MTK SiP SMC/syscon
 * code); it only takes over the transfer request list and issues SCSI, so the
 * shim must leave HCE=1, HCS.DP=1 and fDeviceInit clear. test_ufs() now calls
 * ufs_bringup() and keeps the TUR/CAP/READ/GPT probe on top of it.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x112b0000UL	/* ufshci@112b0000, manaus live FDT */
#define UFS_LUN		2		/* sdc = 0:0:0:2 (measured 2026-11-08) */

/*
 * Linux uses ALIGNED_UPIU_SIZE = 512 B and declares exactly that (128 DWORDs)
 * in the UTRD dw6[15:0]; the response offset is the UCD's response_upiu field
 * in DWORDs (512/4 = 128). Match it.
 *
 * The 2026-11-09/12 `ocs/st=0x0500` is OCS=5 = OCS_PEER_COMM_FAILURE (the
 * device did not answer the UPIU), NOT a response-size mismatch: declaring
 * 8 DWORDs behaved exactly like 128. The fault is a layer below the UTRD.
 */
#define RESP_UPIU_OFF	512u			/* response UPIU offset in the UCD, in DWORDs: 128 */
#define RESP_UPIU_BYTES	512u
#define RESP_UPIU_DW	(RESP_UPIU_BYTES / 4u)
#define STATUS_BYTE	7u			/* SCSI status in the response UPIU header */
#define RESP_CODE_BYTE	6u			/* UPIU response code in the response header */

/*
 * UPIU transaction types (ufs.h). A SCSI command is answered by a RESPONSE
 * UPIU, a NOP OUT by a NOP IN and a QUERY REQUEST by a QUERY RESPONSE.
 */
#define UPIU_TRANSACTION_NOP_OUT	0x00
#define UPIU_TRANSACTION_COMMAND	0x01
#define UPIU_TRANSACTION_QUERY_REQ	0x16
#define UPIU_TRANSACTION_NOP_IN		0x20
#define UPIU_TRANSACTION_RESPONSE	0x21
#define UPIU_TRANSACTION_QUERY_RSP	0x36
#define UPIU_TRANSACTION_REJECT	0x3f

/* Command UPIU flags (ufs.h). */
#define UPIU_CMD_FLAGS_NONE	0x00
#define UPIU_CMD_FLAGS_READ	0x40

/*
 * UTRD DW0 (ufshci.h). UFSHCI 2.0 (the shim reads ver=0x200) uses
 * UTP_CMD_TYPE_UFS_STORAGE (1) for BOTH SCSI and device-management commands;
 * only UFSHCI <= 1.1 uses UTP_CMD_TYPE_SCSI (0) / UTP_CMD_TYPE_DEV_MANAGE (2).
 * Linux sets the interrupt bit only for device-management commands
 * (ufshcd_prepare_req_desc_hdr() sets UTP_REQ_DESC_INT_CMD when lrbp->intr_cmd).
 */
#define UPIU_COMMAND_TYPE_OFFSET	28
#define UTP_CMD_TYPE_SCSI	0x0u
#define UTP_CMD_TYPE_UFS_STORAGE	0x1u
#define UTP_CMD_TYPE_DEV_MANAGE	0x2u
#define UTP_NO_DATA_TRANSFER	0x00000000u
#define UTP_HOST_TO_DEVICE	0x02000000u
#define UTP_DEVICE_TO_HOST	0x04000000u
#define UTP_REQ_DESC_INT_CMD	0x01000000u

/* Query requests (ufs.h): opcode/idn at qr[0..1], query_func at header byte 5. */
#define UPIU_QUERY_OPCODE_READ_FLAG	0x5
#define UPIU_QUERY_OPCODE_SET_FLAG	0x6
#define UPIU_QUERY_FUNC_STANDARD_READ_REQUEST	0x01
#define UPIU_QUERY_FUNC_STANDARD_WRITE_REQUEST	0x81
#define QUERY_FLAG_IDN_FDEVICEINIT	0x01
#define UPIU_COMMAND_SET_TYPE_SCSI	0x0

/* Response UPIU: sense_data_len (BE16) then the 18-byte fixed-format sense. */
#define RSP_SENSE_LEN_OFF	32u
#define RSP_SENSE_DATA_OFF	34u

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_IE		0x24
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UECPA	0x38	/* UIC error code, PHY adapter layer */
#define REG_UECDL	0x3c	/* UIC error code, data link layer */
#define REG_UECN	0x40	/* UIC error code, network layer */
#define REG_UECT	0x44	/* UIC error code, transport layer */
#define REG_UECDME	0x48	/* UIC error code, DME */
#define REG_UIC_CMD	0x90
#define REG_UIC_ARG1	0x94
#define REG_UIC_ARG2	0x98
#define REG_UIC_ARG3	0x9c

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

/* Interrupt Status (JESD223 IS, 20h): write 1 to clear. */
#define IS_UTRCS	(1u << 0)	/* UTP transfer request completed */
#define IS_UIC_ERR	(1u << 2)
#define IS_UIC_PWR	(1u << 4)
#define IS_UIC_LINKUP	(1u << 8)
#define IS_UIC_COMPL	(1u << 10)	/* UIC command completed */
#define IS_DEV_FATAL	(1u << 11)

/* UIC DME commands (JESD223 uic_cmd_dme). */
#define UIC_DME_GET		0x01
#define UIC_DME_SET		0x02
#define UIC_DME_ENABLE		0x12
#define UIC_DME_RESET		0x14
#define UIC_DME_LINK_STARTUP	0x16
#define UIC_DME_HIBER_EXIT	0x18

/* UniPro attribute IDs (include/ufs/unipro.h); ARG1 = (attr & 0xffff) << 16. */
#define ATTR_PA_LOCAL_TX_LCC_ENABLE	0x155E
#define ATTR_PA_ACTIVETXDATALANES	0x1560
#define ATTR_PA_TXGEAR			0x1568
#define ATTR_PA_PWRMODE			0x1571
#define ATTR_PA_ACTIVERXDATALANES	0x1580
#define ATTR_PA_RXGEAR			0x1583
#define ATTR_PA_LOCALVERINFO		0x15A9
#define ATTR_VS_DEBUGCLOCKENABLE	0xD0A1
#define ATTR_VS_SAVEPOWERCONTROL	0xD0A6
#define ATTR_VS_UNIPROPOWERDOWNCONTROL	0xD0A8

/* MediaTek UFS wrapper registers (drivers/ufs/host/ufs-mediatek.h). */
#define REG_MTK_XOUFS_CTRL	0x140
#define REG_MTK_REFCLK_CTRL	0x144
#define REG_MTK_MMIO_OPT_CTRL_0	0x160
#define REG_MTK_EXTREG		0x2100
#define REG_MTK_MPHYCTRL	0x2200
#define REG_MTK_IP_VER		0x2240
#define REG_MTK_REJECT_MON	0x22ac
#define REG_MTK_DEBUG_SEL	0x22c0
#define REG_MTK_PROBE		0x22c8
#define REG_MTK_DEBUG_SEL_B0	0x22d0
#define REG_MTK_DEBUG_SEL_B1	0x22d4
#define REG_MTK_DEBUG_SEL_B2	0x22d8
#define REG_MTK_DEBUG_SEL_B3	0x22dc

#define REFCLK_REQUEST		0x1
#define REFCLK_ACK		0x2

/*
 * MediaTek SiP SMC (ufs-mediatek.h): MTK_SIP_UFS_CONTROL = MTK_SIP_SMC_CMD(0x276)
 * and MTK_SIP_SMC_CMD() = ARM_SMCCC_CALL_VAL(FAST_CALL, SMC_64, OWNER_SIP, fn)
 * = 0xC2000000 | fn. The command selector goes in x1 and its argument in x2.
 */
#define MTK_SIP_UFS_CONTROL		0xC2000276UL
#define UFS_MTK_SIP_DEVICE_RESET	(1u << 1)
#define UFS_MTK_SIP_REF_CLK_NOTIFICATION (1u << 3)

/* REG_UFS_PROBE >> 28 link states (ufs-mediatek.h enum). */
#define VS_LINK_DISABLED	0
#define VS_LINK_DOWN		1
#define VS_LINK_UP		2
#define VS_LINK_HIBERN8		3
#define VS_LINK_LOST		4
#define VS_LINK_CFG		5

/*
 * syscon reset controllers (manaus live FDT): unipro_rst lives in
 * ufs_ao_config, crypto_rst/hci_rst in ufs_pdn_cfg. Each line is
 * ti,reset-bits <assert_off assert_bit deassert_off deassert_bit status_off
 * status_bit flags> with flags = ASSERT_SET|DEASSERT_SET|STATUS_NONE (0x1c),
 * so a reset is done by writing BIT(bit) to the assert offset and BIT(bit)
 * to the deassert offset. These are what ufs_mtk_host_reset() toggles.
 */
#define SYSCON_AO	0x112b8000UL	/* ufs_ao_config: unipro_rst (bit 1) */
#define SYSCON_PDN	0x112bc000UL	/* ufs_pdn_cfg: crypto_rst (bit 0), hci_rst (bit 1) */
#define RST_ASSERT	0x44
#define RST_DEASSERT	0x48

/*
 * UFS clock gates (drivers/clk/mediatek/clk-mt6879-peri.c, clk-mt6879-peri).
 * Both syscons are set/clr/status gate banks at byte offsets 0x4/0x8/0xc.
 * With mtk_clk_gate_ops_setclr a gate is *enabled* by writing BIT to the clr
 * register (enable -> mtk_cg_clr_bit) and the status bit reads 1 while gated.
 * Linux enables all of these in ufs_mtk_init_clocks before it touches the
 * controller; ABL may leave the MPHY clocks gated after loading the kernel,
 * which would explain a UniPro that answers UIC but a PHY that never links.
 */
#define UFSAO_GATE_CLR	0x8
#define UFSAO_GATE_STA	0xc
#define UFSPDN_GATE_CLR	0x8
#define UFSPDN_GATE_STA	0xc
/* ufsao: bit1 phy_sap, bit3 tx_symbol, bit4 rx_symbol, bit5 rx_sym1 */
#define UFSAO_GATE_MASK	((1u << 1) | (1u << 3) | (1u << 4) | (1u << 5))
/* ufspdn: bit1 unipro_sck, bit2 unipro_tick, bit3 u_ck, bit4 aes_u, bit5 u_tick */
#define UFSPDN_GATE_MASK ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5))

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/*
 * Raw SMC, same convention as shim.c: x0 = function id, x1..x3 = arguments.
 * Used for the MediaTek SiP UFS control calls Linux makes and the shim has so
 * far skipped (device RST_n, reference clock notification).
 */
static uint64_t smc4(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t ticks(void);
static uint32_t be32(const uint8_t *p);

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

/* State of the last failed command, dumped by ufs_diag_dump(). */
static struct {
	uint32_t is, hcs, rsr, ocs;
	uint8_t resp[16];
} diag;

/*
 * con_str()/con_hex2() already mirror every line into the ramoops pmsg log
 * (console.c), so these helpers must NOT also call pmsg_* or every line lands
 * twice in the record.
 */
static void ufs_log2(const char *label, const char *a, uint64_t va, const char *b, uint64_t vb)
{
	con_hex2(label, a, va, b, vb);
}

static void ufs_log1(const char *label, const char *val)
{
	con_str(label, val);
}

/*
 * One UIC command (DME_*): arguments into ARG_1..3, opcode into UIC_CMD, then
 * wait for the completion interrupt bit. Returns 0 on completion (even when
 * the DME result is an error), 1 on timeout. *cmd_out carries the raw UIC_CMD
 * register (command status in bits [31:24]), *arg2_out the DME result
 * (MASK_UIC_COMMAND_RESULT = low byte) and *arg3_out the generic error code.
 */
static uint32_t uic_cmd(uint32_t op, uint32_t a1, uint32_t a2, uint32_t a3,
    uint32_t *cmd_out, uint32_t *arg2_out, uint32_t *arg3_out)
{
	ufs_wr(REG_IS, ufs_rd(REG_IS));		/* RW1C: clear whatever is pending */
	ufs_wr(REG_UIC_ARG1, a1);
	ufs_wr(REG_UIC_ARG2, a2);
	ufs_wr(REG_UIC_ARG3, a3);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UIC_CMD, op & 0xff);
	uint64_t start = ticks();
	while (!(ufs_rd(REG_IS) & IS_UIC_COMPL)) {
		if (ticks() - start > 19200000) {	/* 1 s */
			*cmd_out = ufs_rd(REG_UIC_CMD);
			*arg2_out = ufs_rd(REG_UIC_ARG2);
			*arg3_out = ufs_rd(REG_UIC_ARG3);
			return 1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*cmd_out = ufs_rd(REG_UIC_CMD);
	*arg2_out = ufs_rd(REG_UIC_ARG2);
	*arg3_out = ufs_rd(REG_UIC_ARG3);
	ufs_wr(REG_IS, IS_UIC_COMPL);
	return 0;
}

static void uic_log(const char *tag, uint32_t op, uint32_t rc, uint32_t cmd, uint32_t a2, uint32_t a3)
{
	ufs_log2(tag, "op=", op, "rc=", rc);
	ufs_log2(tag, "cmd=", cmd, "arg2=", a2);
	ufs_log2(tag, "arg3=", a3, "hcs=", ufs_rd(REG_HCS));
	ufs_log2(tag, "is=", ufs_rd(REG_IS), "uecpa=", ufs_rd(REG_UECPA));
	ufs_log2(tag, "uecdl=", ufs_rd(REG_UECDL), "uecdme=", ufs_rd(REG_UECDME));
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static uint32_t timer_hz(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return (uint32_t)f;
}

/* Busy-wait; cntvct_el0 counts at cntfrq_el0 Hz. */
static void delay_us(uint32_t us)
{
	uint64_t n = (uint64_t)timer_hz() * us / 1000000u;
	uint64_t s = ticks();
	while (ticks() - s < n)
		;
}

static inline uint32_t sc_rd(uintptr_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void sc_set(uintptr_t base, uint32_t off, uint32_t mask)
{
	*(volatile uint32_t *)(base + off) = sc_rd(base, off) | mask;
}

static inline void sc_wr(uintptr_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* One DME_SET / DME_GET (ARG1 = attr<<16; DME_SET value in ARG3). */
static uint32_t dme_set(uint32_t attr, uint32_t val, uint32_t *a2)
{
	uint32_t cmd, a3;
	return uic_cmd(UIC_DME_SET, (attr & 0xffff) << 16, 0, val, &cmd, a2, &a3);
}

static uint32_t dme_get(uint32_t attr, uint32_t *val, uint32_t *a2)
{
	uint32_t cmd, a3;
	uint32_t rc = uic_cmd(UIC_DME_GET, (attr & 0xffff) << 16, 0, 0, &cmd, a2, &a3);
	*val = a3;
	return rc;
}

/*
 * ufs_mtk_host_reset(): assert hci/crypto/unipro (mphy has no reset on
 * manaus), wait 110 us, deassert unipro/crypto/hci. The reset_control
 * calls in Linux are real register writes here (flags ASSERT_SET |
 * DEASSERT_SET), not the no-ops the "NONE" flags would imply.
 */
static void mtk_host_reset(void)
{
	sc_set(SYSCON_PDN, RST_ASSERT, 1u << 1);	/* hci_rst assert */
	sc_set(SYSCON_PDN, RST_ASSERT, 1u << 0);	/* crypto_rst assert */
	sc_set(SYSCON_AO, RST_ASSERT, 1u << 1);		/* unipro_rst assert */
	delay_us(110);
	sc_set(SYSCON_AO, RST_DEASSERT, 1u << 1);	/* unipro_rst deassert */
	sc_set(SYSCON_PDN, RST_DEASSERT, 1u << 0);	/* crypto_rst deassert */
	sc_set(SYSCON_PDN, RST_DEASSERT, 1u << 1);	/* hci_rst deassert */
}

/* HCE stop: clear CONTROLLER_ENABLE and wait for the controller to stop. */
static uint32_t hce_stop(void)
{
	if (!(ufs_rd(REG_HCE) & 1))
		return 0;
	ufs_wr(REG_HCE, 0);
	for (int i = 0; i < 100; i++) {
		if (!(ufs_rd(REG_HCE) & 1))
			return 0;
		delay_us(10);
	}
	return 1;
}

/* HCE start: set CONTROLLER_ENABLE, wait (>= 600 us) for it to self-set. */
static uint32_t hce_start(void)
{
	ufs_wr(REG_HCE, 1);
	delay_us(600);
	for (int i = 0; i < 100; i++) {
		if (ufs_rd(REG_HCE) & 1)
			return 0;
		delay_us(10);
	}
	return 1;
}

/*
 * ufs_mtk_hce_enable_notify(PRE_CHANGE) + HCE start: host reset, turn on
 * CLK_CG (XOUFS_CTRL |= 0x80) to bypass the abnormal ERR_CHK hang, then
 * re-enable the controller. Mirrors ufshcd_hba_enable() on MTK.
 */
static uint32_t mtk_hba_enable(void)
{
	hce_stop();
	mtk_host_reset();
	ufs_wr(REG_MTK_XOUFS_CTRL, ufs_rd(REG_MTK_XOUFS_CTRL) | 0x80);
	return hce_start();
}

/*
 * ufs_mtk_device_reset(): Linux drives the device RST_n through the MTK SiP
 * SMC before it enables the host controller (ufshcd_init -> ufshcd_device_reset
 * -> ufs_mtk_device_reset). The signal is active low: hold it low for at least
 * 10 us, raise it, then let the device settle for 10-15 ms. This is the step
 * v2/v3 skipped and the live /proc/ufs_debug records at boot.
 */
static void mtk_device_reset(void)
{
	uint64_t lo, hi;
	hce_stop();
	lo = smc4(MTK_SIP_UFS_CONTROL, UFS_MTK_SIP_DEVICE_RESET, 0, 0);	/* RST_n low */
	delay_us(12);
	hi = smc4(MTK_SIP_UFS_CONTROL, UFS_MTK_SIP_DEVICE_RESET, 1, 0);	/* RST_n high */
	ufs_log2("UFS DEVRST", "lo=", lo, "hi=", hi);
	delay_us(12000);
}

/*
 * ufs_mtk_dbg_sel() + REG_UFS_PROBE >> 28: the link state machine Linux polls
 * in ufs_mtk_wait_link_state(). Writing DEBUG_SEL is a read-only observation.
 */
static uint32_t probe_state(void)
{
	uint32_t hw = ufs_rd(REG_MTK_IP_VER);
	if (((hw >> 16) & 0xff) >= 0x36) {
		ufs_wr(REG_MTK_DEBUG_SEL, 0x820820);
		ufs_wr(REG_MTK_DEBUG_SEL_B0, 0x00000000);
		ufs_wr(REG_MTK_DEBUG_SEL_B1, 0x55555555);
		ufs_wr(REG_MTK_DEBUG_SEL_B2, 0xaaaaaaaa);
		ufs_wr(REG_MTK_DEBUG_SEL_B3, 0xffffffff);
	} else {
		ufs_wr(REG_MTK_DEBUG_SEL, 0x20);
	}
	return ufs_rd(REG_MTK_PROBE) >> 28;
}

/* ufs_mtk_setup_ref_clk(true): request the reference clock, wait for ACK. */
static uint32_t refclk_on(void)
{
	if (!(ufs_rd(REG_MTK_REFCLK_CTRL) & REFCLK_REQUEST))
		ufs_wr(REG_MTK_REFCLK_CTRL, REFCLK_REQUEST);
	for (int i = 0; i < 300; i++) {			/* <= 3 ms */
		uint32_t v = ufs_rd(REG_MTK_REFCLK_CTRL);
		if (((v & REFCLK_ACK) >> 1) == (v & REFCLK_REQUEST))
			return 0;
		delay_us(10);
	}
	return 1;
}

/*
 * ufs_mtk_init_clocks(): ungate the UFS AO (MPHY: phy_sap/tx_symbol/rx_symbol/
 * rx_sym1) and PDN (unipro_sck/unipro_tick/u_ck/aes_u/u_tick) clocks. A gate
 * bit reads 1 while gated, so log the status before and after to see whether
 * ABL really left any of them off.
 */
static void ufs_clocks_on(void)
{
	ufs_log2("UFS CLK", "ao_sta=", sc_rd(SYSCON_AO, UFSAO_GATE_STA), "pdn_sta=", sc_rd(SYSCON_PDN, UFSPDN_GATE_STA));
	sc_wr(SYSCON_AO, UFSAO_GATE_CLR, UFSAO_GATE_MASK);
	sc_wr(SYSCON_PDN, UFSPDN_GATE_CLR, UFSPDN_GATE_MASK);
	delay_us(20);
	ufs_log2("UFS CLK", "ao_sta=", sc_rd(SYSCON_AO, UFSAO_GATE_STA), "pdn_sta=", sc_rd(SYSCON_PDN, UFSPDN_GATE_STA));
}

/* ufs_mtk_pre_link: non-LPM, disable host TX LCC, clear deep stall. */
static void pre_link(void)
{
	uint32_t a2 = 0, v = 0;
	dme_set(ATTR_VS_UNIPROPOWERDOWNCONTROL, 0, &a2);
	dme_set(ATTR_PA_LOCAL_TX_LCC_ENABLE, 0, &a2);
	if (!dme_get(ATTR_VS_SAVEPOWERCONTROL, &v, &a2)) {
		v &= ~(1u << 6);
		dme_set(ATTR_VS_SAVEPOWERCONTROL, v, &a2);
	}
}

/* ufs_mtk_cfg_unipro_cg(true): enable UniPro clock gating after link up. */
static void post_link(void)
{
	uint32_t a2 = 0, v = 0;
	if (!dme_get(ATTR_VS_SAVEPOWERCONTROL, &v, &a2)) {
		v |= (1u << 0) | (1u << 2) | (1u << 3);
		dme_set(ATTR_VS_SAVEPOWERCONTROL, v, &a2);
	}
	if (!dme_get(ATTR_VS_DEBUGCLOCKENABLE, &v, &a2)) {
		v &= ~(1u << 5);
		dme_set(ATTR_VS_DEBUGCLOCKENABLE, v, &a2);
	}
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Snapshot the last failed command for ufs_diag_dump(). */
static void diag_capture(void)
{
	diag.is = ufs_rd(REG_IS);
	diag.hcs = ufs_rd(REG_HCS);
	diag.rsr = ufs_rd(REG_UTRLRSR);
	diag.ocs = utrl[0].dw2 & 0xff;
	for (unsigned i = 0; i < 16; i++)
		diag.resp[i] = ucd.resp[i];
}

/*
 * Fill UTRD slot 0 for the command UPIU already built in ucd.cmd, and (for a
 * data command) the single-entry PRDT for buf/len. cmd_type is the UTP command
 * type, dir the data direction and intr the interrupt-command bit; Linux's
 * ufshcd_prepare_req_desc_hdr() sets intr only for device-management commands.
 * Clears the response buffer and the PRDT but leaves ucd.cmd to the caller.
 */
static void prep_utrd(uint32_t cmd_type, uint32_t dir, uint32_t intr, void *buf, uint32_t len)
{
	zero(&utrl[0], sizeof(utrl[0]));
	zero(ucd.resp, sizeof(ucd.resp));
	zero(&ucd.prdt[0], sizeof(ucd.prdt[0]));

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = dir | (cmd_type << UPIU_COMMAND_TYPE_OFFSET) | (intr ? UTP_REQ_DESC_INT_CMD : 0);
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (RESP_UPIU_OFF / 4) << 16 | RESP_UPIU_DW;
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
}

/*
 * Ring slot 0 and wait for it to complete. Returns 1 on timeout and 2 on an
 * OCS error (both captured for ufs_diag_dump), 0 when the host wrote OCS=0;
 * *ocs_out and *status_out carry the OCS and SCSI status bytes.
 */
static uint32_t utrd_wait(uint32_t *ocs_out, uint32_t *status_out)
{
	__asm__ volatile("dsb sy" ::: "memory");

	/* An OCS error can halt the list (UTRLRSR -> 0): re-arm it before ringing. */
	if (!(ufs_rd(REG_UTRLRSR) & 1)) {
		ufs_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
		ufs_wr(REG_UTRLBAU, (uint32_t)((uintptr_t)utrl >> 32));
		ufs_wr(REG_UTRLRSR, 1);
	}

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			diag_capture();
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			*ocs_out = 0;
			*status_out = 0;
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[STATUS_BYTE];	/* SCSI status in the response UPIU */
	if (*ocs_out) {
		diag_capture();
		return 2;
	}
	return 0;
}

/* Dump the response UPIU header and the fixed-format sense data of a failure. */
static void sense_dump(void)
{
	uint32_t slen = (uint32_t)ucd.resp[RSP_SENSE_LEN_OFF] << 8 | ucd.resp[RSP_SENSE_LEN_OFF + 1];
	ufs_log2("UFS SENSE", "rsp=", ucd.resp[0], "code=", ucd.resp[RESP_CODE_BYTE]);
	ufs_log2("UFS SENSE", "len=", slen, "key=", ucd.resp[RSP_SENSE_DATA_OFF + 2] & 0x0f);
	ufs_log2("UFS SENSE", "asc=", ucd.resp[RSP_SENSE_DATA_OFF + 12], "ascq=", ucd.resp[RSP_SENSE_DATA_OFF + 13]);
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(ucd.cmd, sizeof(ucd.cmd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = UPIU_TRANSACTION_COMMAND;
	ucd.cmd[1] = len ? UPIU_CMD_FLAGS_READ : UPIU_CMD_FLAGS_NONE;
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[4] = UPIU_COMMAND_SET_TYPE_SCSI;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	prep_utrd(UTP_CMD_TYPE_UFS_STORAGE, len ? UTP_DEVICE_TO_HOST : UTP_NO_DATA_TRANSFER, 0, buf, len);
	uint32_t rc = utrd_wait(ocs_out, status_out);
	if (rc)
		return rc;
	if (ucd.resp[0] != UPIU_TRANSACTION_RESPONSE)
		return 3;
	if (*status_out) {
		sense_dump();
		return 4;
	}
	return 0;
}

/*
 * One device-management UPIU (NOP OUT or QUERY REQUEST) in slot 0, with the
 * interrupt bit Linux sets for dev commands. On success *val_out gets the
 * query response value (UPIU offset 20) and *rsp_type_out the response UPIU
 * transaction type (NOP IN or QUERY RESPONSE). Returns the utrd_wait code.
 */
static uint32_t dev_upiu(uint8_t upiu_type, uint8_t query_func, uint8_t opcode, uint8_t idn,
    uint32_t *val_out, uint32_t *rsp_type_out, uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	*val_out = 0;
	zero(ucd.cmd, sizeof(ucd.cmd));

	ucd.cmd[0] = upiu_type;
	ucd.cmd[3] = ++tag;
	if (upiu_type == UPIU_TRANSACTION_QUERY_REQ) {
		ucd.cmd[5] = query_func;	/* query_func lives in header DW1 byte 1 */
		ucd.cmd[12] = opcode;		/* qr.opcode */
		ucd.cmd[13] = idn;		/* qr.idn */
		/* qr.index/selector/length/value stay 0 */
	}

	prep_utrd(UTP_CMD_TYPE_UFS_STORAGE, UTP_NO_DATA_TRANSFER, 1, 0, 0);
	uint32_t rc = utrd_wait(ocs_out, status_out);
	*rsp_type_out = ucd.resp[0];
	if (!rc && *rsp_type_out == UPIU_TRANSACTION_QUERY_RSP)
		*val_out = be32(ucd.resp + 20);	/* query response value */
	return rc;
}

/*
 * ufshcd_probe_hba()'s device handshake, which the shim skipped: a NOP OUT
 * UPIU (ufshcd_verify_dev_init) proves the device answers, then the host sets
 * the fDeviceInit flag and polls it until the device clears it
 * (ufshcd_complete_dev_init). Only then are the logical units usable, so a
 * SCSI command issued straight after link startup returns CHECK CONDITION.
 * Returns 0 once fDeviceInit reads back clear.
 */
static uint32_t device_init(void)
{
	uint32_t ocs = 0, st = 0, rt = 0, val = 0, rc;

	rc = dev_upiu(UPIU_TRANSACTION_NOP_OUT, 0, 0, 0, &val, &rt, &ocs, &st);
	ufs_log2("UFS NOPOUT", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
	ufs_log2("UFS NOPOUT", "rsp=", rt, "val=", val);

	rc = dev_upiu(UPIU_TRANSACTION_QUERY_REQ, UPIU_QUERY_FUNC_STANDARD_WRITE_REQUEST,
	    UPIU_QUERY_OPCODE_SET_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, &val, &rt, &ocs, &st);
	ufs_log2("UFS SETFDI", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
	ufs_log2("UFS SETFDI", "rsp=", rt, "val=", val);

	/* Poll READ fDeviceInit until the device clears it (usually a few ms). */
	for (int i = 0; i < 100; i++) {
		val = 0;
		rc = dev_upiu(UPIU_TRANSACTION_QUERY_REQ, UPIU_QUERY_FUNC_STANDARD_READ_REQUEST,
		    UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, &val, &rt, &ocs, &st);
		if (i < 2 || rc || !(val & 1)) {
			ufs_log2("UFS RDFDI", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
			ufs_log2("UFS RDFDI", "rsp=", rt, "val=", val);
		}
		if (!rc && !(val & 1))
			return 0;
		delay_us(2000);
	}
	return 1;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(UFS_LUN, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		ufs_log2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			ufs_log1(" fs", fs_type((uint32_t)first));
	}
}

static void ufs_diag_dump(uint32_t rc)
{
	ufs_log2("UFS DIAG", "is=", diag.is, "hcs=", diag.hcs);
	ufs_log2("UFS DIAG", "rsr=", diag.rsr, "ocs=", diag.ocs);
	ufs_log2("UFS DIAG", "r0=", diag.resp[0], "r2=", diag.resp[2]);
	ufs_log2("UFS DIAG", "r6=", diag.resp[6], "r7=", diag.resp[7]);
	ufs_log1("UFS DIAG", rc == 1 ? "timeout" : "ocs error");
}

/*
 * Bring the internal UFS link up and make the device usable, with the MMU off.
 * This is the exact sequence proven on shim-ufs6 (v5): clock gates, reference
 * clock, device RST_n (the missing step), host reset + HCE, pre_link,
 * DME_LINK_STARTUP, post_link, take over the transfer request list, then the
 * NOP OUT + fDeviceInit handshake ufshcd_probe_hba() does (the missing step
 * for the logical units to be usable). No SCSI command is issued here.
 *
 * The payload path calls this before the jump when its cmdline says
 * rd=ufsroot0: XNU's sweet_ufs.c reuses the state but can only take over the
 * request list and issue SCSI (it does not know the MTK SiP SMC/syscon
 * bring-up). Leaves HCE=1, HCS.DP=1 and fDeviceInit clear. Returns 1 when the
 * link is up and the request list was taken, 0 otherwise.
 */
int ufs_bringup(void)
{
	ufs_clocks_on();

	uint32_t rc = refclk_on();
	ufs_log2("UFS REFCLK", "rc=", rc, "reg=", ufs_rd(REG_MTK_REFCLK_CTRL));

	/* v4: the device RST_n toggle Linux does before it enables the host. */
	mtk_device_reset();

	uint32_t ucmd = 0, ua2 = 0, ua3 = 0, urc;
	uint32_t link = 0;

	for (int attempt = 0; attempt < 3 && !link; attempt++) {
		const char *tag = attempt == 0 ? "UFS L1"
		    : attempt == 1 ? "UFS L2" : "UFS L3";

		if (attempt == 2)
			mtk_device_reset();	/* fresh RST_n before the last try */

		rc = mtk_hba_enable();
		ufs_log2(tag, "hce=", rc, "hcs=", ufs_rd(REG_HCS));
		ufs_log2(tag, "xoufs=", ufs_rd(REG_MTK_XOUFS_CTRL), "probe=", probe_state());
		ufs_wr(REG_IS, ufs_rd(REG_IS));	/* RW1C: drop stale UIC errors */
		if (!(ufs_rd(REG_HCE) & 1)) {
			ufs_log1(tag, "HCE did not set");
			continue;
		}

		/*
		 * Attempt 1 is Linux-exact: the MTK HCE already resets and
		 * enables the local UniPro in hardware, so Linux issues no
		 * DME_RESET/DME_ENABLE here. Attempts 2/3 add them (the v2/v3
		 * path) to compare inside one boot.
		 */
		if (attempt >= 1) {
			urc = uic_cmd(UIC_DME_RESET, 0, 0, 0, &ucmd, &ua2, &ua3);
			uic_log("UFS DMERST", UIC_DME_RESET, urc, ucmd, ua2, ua3);
			urc = uic_cmd(UIC_DME_ENABLE, 0, 0, 0, &ucmd, &ua2, &ua3);
			uic_log("UFS DMEEN", UIC_DME_ENABLE, urc, ucmd, ua2, ua3);
		}

		pre_link();
		urc = uic_cmd(UIC_DME_LINK_STARTUP, 0, 0, 0, &ucmd, &ua2, &ua3);
		uic_log(tag, UIC_DME_LINK_STARTUP, urc, ucmd, ua2, ua3);
		link = !urc && (ua2 & 0xff) == 0;
		ufs_log2(tag, "probe=", probe_state(), "dp=", (ufs_rd(REG_HCS) & HCS_DP) ? 1u : 0u);
	}
	ufs_log2("UFS LINK", "up=", link, "dev=", (ufs_rd(REG_HCS) & HCS_DP) ? 1u : 0u);

	uint32_t v = 0;
	if (!dme_get(ATTR_PA_TXGEAR, &v, &ua2)) {
		uint32_t rx = 0;
		dme_get(ATTR_PA_RXGEAR, &rx, &ua2);
		ufs_log2("UFS GEAR", "tx=", v, "rx=", rx);
	}
	if (!dme_get(ATTR_PA_PWRMODE, &v, &ua2)) {
		uint32_t lanes = 0;
		dme_get(ATTR_PA_ACTIVETXDATALANES, &lanes, &ua2);
		ufs_log2("UFS PWR", "mode=", v, "lanes=", lanes);
	}
	post_link();

	/* If the link is still down, try waking it out of Hibern8 once. */
	if (!link) {
		urc = uic_cmd(UIC_DME_HIBER_EXIT, 0, 0, 0, &ucmd, &ua2, &ua3);
		uic_log("UFS H8EXIT", UIC_DME_HIBER_EXIT, urc, ucmd, ua2, ua3);
		link = !urc && (ua2 & 0xff) == 0;
		ufs_log2("UFS LINK2", "up=", link, "hcs=", ufs_rd(REG_HCS));
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	ufs_log2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		ufs_log1("UFS", "list base not taken: stop");
		return 0;
	}

	/*
	 * Linux ufshcd_probe_hba() sends a NOP OUT and then sets/polls
	 * fDeviceInit before it issues any SCSI command; without it the
	 * logical units are not ready and every SCSI command is CHECK
	 * CONDITION. Do that handshake now.
	 */
	if (link) {
		rc = device_init();
		ufs_log2("UFS DEVTINIT", "rc=", rc, "hcs=", ufs_rd(REG_HCS));
	}
	return link;
}

void test_ufs(void)
{
	ufs_log1("UFS", "milestone 5 probe v6: device reset + link + fDeviceInit handshake (read-only)");
	ufs_log2("UFSBASE", "base=", UFS_BASE, "lun=", UFS_LUN);
	ufs_log2("UFS TIMER", "cntfrq=", timer_hz(), "hz", 0);

	uint32_t cap = ufs_rd(REG_CAP);
	ufs_log2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	ufs_log2("UFS", "hce=", ufs_rd(REG_HCE), "hcs=", ufs_rd(REG_HCS));
	ufs_log2("UFS", "is=", ufs_rd(REG_IS), "ie=", ufs_rd(REG_IE));
	ufs_log2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	ufs_log2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	ufs_log2("UFS UEC", "pa=", ufs_rd(REG_UECPA), "dl=", ufs_rd(REG_UECDL));
	ufs_log2("UFS UEC", "net=", ufs_rd(REG_UECN), "tr=", ufs_rd(REG_UECT));
	ufs_log2("UFS UEC", "dme=", ufs_rd(REG_UECDME), "is=", ufs_rd(REG_IS));
	ufs_log2("UFS MTK", "ipver=", ufs_rd(REG_MTK_IP_VER), "xoufs=", ufs_rd(REG_MTK_XOUFS_CTRL));
	ufs_log2("UFS MTK", "refclk=", ufs_rd(REG_MTK_REFCLK_CTRL), "opt0=", ufs_rd(REG_MTK_MMIO_OPT_CTRL_0));
	ufs_log2("UFS MTK", "mphyctrl=", ufs_rd(REG_MTK_MPHYCTRL), "hcs=", ufs_rd(REG_HCS));
	ufs_log2("UFS MPHY", "ext=", ufs_rd(REG_MTK_EXTREG), "mphy=", ufs_rd(REG_MTK_MPHYCTRL));
	ufs_log2("UFS REJ", "mon=", ufs_rd(REG_MTK_REJECT_MON), "probe=", probe_state());
	ufs_log2("UFS RST", "ao44=", sc_rd(SYSCON_AO, RST_ASSERT), "ao48=", sc_rd(SYSCON_AO, RST_DEASSERT));
	ufs_log2("UFS RST", "pdn44=", sc_rd(SYSCON_PDN, RST_ASSERT), "pdn48=", sc_rd(SYSCON_PDN, RST_DEASSERT));

	/*
	 * ABL leaves the controller enabled but the UniPro link half-broken
	 * (PWR_LOCAL, UECPA pending). ufs_bringup() replicates the Linux/MTK
	 * probe sequence (clocks, reference clock, device reset, host reset +
	 * HCE, pre_link, DME_LINK_STARTUP, post_link, UTRL takeover, then the
	 * NOP OUT + fDeviceInit handshake) and leaves the link up for the SCSI
	 * probes below. The payload path calls the same function before it
	 * jumps to XNU.
	 */
	int up = ufs_bringup();
	ufs_log2("UFS BRINGUP", "up=", up, "hcs=", ufs_rd(REG_HCS));

	uint32_t rc;
	uint32_t ocs = 0, st = 0;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(UFS_LUN, tur, sizeof(tur), 0, 0, &ocs, &st);
		ufs_log2("UFS TUR", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(UFS_LUN, cap10, sizeof(cap10), data, 8, &ocs, &st);
	ufs_log2("UFS CAP", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
	if (!rc)
		ufs_log2("UFS LUN2", "lastlba=", be32(data), "blk=", be32(data + 4));
	else
		ufs_diag_dump(rc);

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(UFS_LUN, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	ufs_log2("UFS READ", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
	if (rc) {
		ufs_diag_dump(rc);
		return;
	}
	ufs_log2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	ufs_log1("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	ufs_log2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}
