/*
 * Minimal exception-level probe for manaus bring-up.
 *
 * The full shim writes VBAR_EL1 in start.S. If LK hands the image over at
 * EL2 with HCR_EL2.E2H == 0, that write is UNDEFINED and traps before
 * shim_main ever runs, so neither the pmsg beacon nor the watchdog gets
 * programmed and the phone just hangs. This probe touches no EL1 system
 * register at all: it records the real CurrentEL in a ramoops pmsg record
 * and then asks PSCI for a warm reset. A warm reset keeps DRAM contents,
 * so LK retries slot a and finally falls back to Android on b, whose kernel
 * exposes the record as /sys/fs/pstore/pmsg-ramoops-0.
 *
 * If the SMC returns (no PSCI), it forces the TOPRGU software reset.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mrs	x9, currentel
	lsr	x9, x9, #2		/* CurrentEL[3:2]: 1=EL1 2=EL2 3=EL3 */

	/* ramoops pmsg zone at 0x48160000: hdr { sig, start, size }, data[] */
	movz	x1, #0x4816, lsl #16
	adr	x4, probe_text
	add	x5, x1, #12
	mov	w6, #8			/* "PROBE EL" */
0:	ldrb	w7, [x4], #1
	strb	w7, [x5], #1
	subs	w6, w6, #1
	b.ne	0b
	add	w7, w9, #0x30		/* EL digit */
	strb	w7, [x5], #1
	mov	w7, #10			/* '\n' */
	strb	w7, [x5], #1
	mov	w3, #10			/* size: 8 + digit + '\n' */
	str	w3, [x1, #8]
	str	wzr, [x1, #4]		/* start = 0 */
	dsb	sy
	movz	w2, #0x4244
	movk	w2, #0x4347, lsl #16
	str	w2, [x1]		/* signature last */
	dsb	sy

	/* PSCI SYSTEM_RESET (0x84000009), method smc (DT psci node). */
	movz	x0, #0x0009
	movk	x0, #0x8400, lsl #16
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	smc	#0

	/* SMC returned: force the TOPRGU watchdog software reset. */
	movz	x1, #0x0070, lsl #16
	movk	x1, #0x1c00, lsl #16	/* WDT_BASE = 0x1c007000 */
	mov	w2, #0x1209
	str	w2, [x1, #0x14]		/* WDT_SWRST */
	dsb	sy
1:	wfe
	b	1b

	.section .rodata
probe_text:
	.ascii	"PROBE EL"
