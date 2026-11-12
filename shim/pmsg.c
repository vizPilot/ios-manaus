/*
 * Write a ramoops persistent_ram record from the shim. Runs with the MMU off,
 * so every access is Device-nGnRnE and reaches DRAM directly; a final dsb
 * makes the record visible before any reset.
 *
 * The record is written to both the pmsg and the console zone. pmsg is not
 * touched by the kernel during boot, so it is the primary channel; console is
 * a fallback in case the zone layout differs from the one computed in pmsg.h.
 */
#include "pmsg.h"

#define PMSG_ADDR	0x48160000UL
#define CONSOLE_ADDR	0x4811f000UL
#define PMSG_SIG	0x43474244U	/* PERSISTENT_RAM_SIG ^ 0 */
#define PMSG_MAX	4096

static char logbuf[PMSG_MAX];
static uint32_t loglen;

static void p_putc(char c)
{
	if (loglen < PMSG_MAX)
		logbuf[loglen++] = c;
}

static void p_puts(const char *s)
{
	while (*s)
		p_putc(*s++);
}

static void p_puthex(uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";

	for (int i = digits - 1; i >= 0; i--)
		p_putc(hex[(v >> (4 * i)) & 0xf]);
}

static void p_putdec(uint64_t v)
{
	char tmp[20];
	int n = 0;

	if (!v) {
		p_putc('0');
		return;
	}
	while (v) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (n)
		p_putc(tmp[--n]);
}

void pmsg_reset(void)
{
	loglen = 0;
}

void pmsg_str(const char *name, const char *val)
{
	p_puts(name);
	p_putc('=');
	p_puts(val);
	p_putc('\n');
}

void pmsg_hex(const char *name, uint64_t val)
{
	p_puts(name);
	p_putc('=');
	p_puts("0x");
	p_puthex(val, 16);
	p_putc('\n');
}

void pmsg_hex2(const char *label, const char *a, uint64_t va, const char *b, uint64_t vb)
{
	p_puts(label);
	p_putc(' ');
	p_puts(a);
	p_putc('=');
	p_puts("0x");
	p_puthex(va, 8);
	if (b) {
		p_putc(' ');
		p_puts(b);
		p_putc('=');
		p_puts("0x");
		p_puthex(vb, 8);
	}
	p_putc('\n');
}

void pmsg_dec(const char *name, uint64_t val)
{
	p_puts(name);
	p_putc('=');
	p_putdec(val);
	p_putc('\n');
}

void pmsg_skip(void)
{
	p_putc('\n');
}

/*
 * Keep this out of line: with -fno-pic the inliner folds the constant zone
 * address and the .bss source into an absolute offset, which is wrong because
 * the image is linked at 0 but loaded at 0x40000000.
 */
static __attribute__((noinline)) void write_zone(uintptr_t addr, const char *s, uint32_t len)
{
#if defined(__aarch64__)
	volatile uint32_t *hdr = (volatile uint32_t *)addr;
	volatile char *data = (volatile char *)(addr + 12);

	/* Publish the payload first and the signature last, so an interrupted
	 * write never leaves a header pointing at half-written bytes. */
	for (uint32_t i = 0; i < len; i++)
		data[i] = s[i];
	hdr[1] = 0;		/* start: data begins at offset 0 */
	hdr[2] = len;		/* valid bytes */
	__asm__ volatile("dsb sy" ::: "memory");
	hdr[0] = PMSG_SIG;
	__asm__ volatile("dsb sy" ::: "memory");
#else
	/* Host console-render test: format only, never touch the fixed zone. */
	(void)addr;
	(void)s;
	(void)len;
#endif
}

void pmsg_flush(void)
{
	if (!loglen)
		return;
#if defined(SHIM_MILESTONE_A)
	/*
	 * Bring-up probe (2026-11-12): the pmsg zone address is derived from the
	 * ramoops cmdline layout, not verified on hardware. Write the record to
	 * every candidate zone so at least one lands in a real pstore zone; a
	 * hit in the "wrong" zone shows up as its own pstore file and still
	 * proves the shim ran. Overwriting a console/dmesg zone is harmless.
	 */
	write_zone(0x48160000UL, logbuf, loglen);	/* pmsg, record_size-9-zone layout */
	write_zone(0x480d0000UL, logbuf, loglen);	/* pmsg, console-first layout */
	write_zone(0x48090000UL, logbuf, loglen);	/* region start canary */
#else
	write_zone(PMSG_ADDR, logbuf, loglen);
	write_zone(CONSOLE_ADDR, logbuf, loglen);
#endif
}
