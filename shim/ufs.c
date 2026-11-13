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

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
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
#define UIC_DME_LINK_STARTUP	0x16
#define UIC_DME_HIBER_EXIT	0x18

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

static uint64_t ticks(void);

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

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
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
			diag.is = ufs_rd(REG_IS);
			diag.hcs = ufs_rd(REG_HCS);
			diag.rsr = ufs_rd(REG_UTRLRSR);
			diag.ocs = utrl[0].dw2 & 0xff;
			for (unsigned i = 0; i < 16; i++)
				diag.resp[i] = ucd.resp[i];
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[STATUS_BYTE];	/* SCSI status in the response UPIU */
	if (*ocs_out) {
		diag.is = ufs_rd(REG_IS);
		diag.hcs = ufs_rd(REG_HCS);
		diag.rsr = ufs_rd(REG_UTRLRSR);
		diag.ocs = *ocs_out;
		for (unsigned i = 0; i < 16; i++)
			diag.resp[i] = ucd.resp[i];
	}
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
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

void test_ufs(void)
{
	ufs_log1("UFS", "milestone 5 probe (read-only)");
	ufs_log2("UFSBASE", "base=", UFS_BASE, "lun=", UFS_LUN);
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	ufs_log2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	ufs_log2("UFS", "hce=", hce, "hcs=", hcs);
	ufs_log2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	ufs_log2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	ufs_log2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	ufs_log2("UFS UEC", "pa=", ufs_rd(REG_UECPA), "dl=", ufs_rd(REG_UECDL));
	ufs_log2("UFS UEC", "net=", ufs_rd(REG_UECN), "tr=", ufs_rd(REG_UECT));
	ufs_log2("UFS UEC", "dme=", ufs_rd(REG_UECDME), "is=", ufs_rd(REG_IS));

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		ufs_log1("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		ufs_log1("UFS", "requests outstanding: stop");
		return;
	}

	/*
	 * Linux always starts the UniPro link on init (ufshcd_link_startup ->
	 * DME_LINK_STARTUP); ABL's leftover state can leave the device off the
	 * link even while HCS says the controller is ready. That is the prime
	 * suspect for OCS=5 (PEER_COMM_FAILURE) on the first command.
	 */
	uint32_t ucmd = 0, ua2 = 0, ua3 = 0, urc;
	urc = uic_cmd(UIC_DME_LINK_STARTUP, 0, 0, 0, &ucmd, &ua2, &ua3);
	uic_log("UFS LINKUP", UIC_DME_LINK_STARTUP, urc, ucmd, ua2, ua3);

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
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(UFS_LUN, tur, sizeof(tur), 0, 0, &ocs, &st);
		ufs_log2("UFS TUR", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
		if (!rc)
			break;
	}
	/* Still failing: try waking the link out of Hibern8 and retry once. */
	if (rc) {
		urc = uic_cmd(UIC_DME_HIBER_EXIT, 0, 0, 0, &ucmd, &ua2, &ua3);
		uic_log("UFS H8EXIT", UIC_DME_HIBER_EXIT, urc, ucmd, ua2, ua3);
		rc = scsi(UFS_LUN, tur, sizeof(tur), 0, 0, &ocs, &st);
		ufs_log2("UFS TUR2", "rc=", rc, "ocs/st=", (uint64_t)ocs << 8 | st);
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
