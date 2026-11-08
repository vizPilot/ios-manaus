/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/*
 * The bootloader decides which of the two ramdisks (boot.img generic or
 * vendor_boot vendor) becomes /chosen/linux,initrd-start, and it may relocate
 * or concatenate them. We package the payload in both, so scan for the magic
 * instead of assuming offset 0. Only 8-byte offsets: the payload header has
 * 64-bit fields and this runs with the MMU off (-mstrict-align).
 */
static const struct payload *find_payload(uintptr_t rd_start, uint64_t span,
    uint64_t *off_out)
{
	for (uint64_t off = 0; off + sizeof(struct payload) <= span; off += 8) {
		if (magic_ok((const char *)(rd_start + off))) {
			*off_out = off;
			return (const struct payload *)(rd_start + off);
		}
	}
	return 0;
}

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	if (rd_end <= rd_start || (rd_start & 7))
		return "bad initrd range";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	uint64_t span = rd_end - rd_start;
	uint64_t off = 0;
	const struct payload *p = find_payload(rd_start, span, &off);
	if (!p)
		return "no SWEETXNU payload";
	con_hex2("PAYLOAD", "off=", off, "nsegs=", p->nsegs);

	uint64_t plen = span - off;
	if (p->version != 1 || p->nsegs == 0 || p->nsegs > 64 ||
	    (p->adt_off & 7) || p->adt_size == 0 ||
	    p->adt_off > plen || p->adt_size > plen - p->adt_off)
		return "bad payload header";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		if ((s->file_off & 7) || s->file_size > s->vmsize ||
		    s->file_off > plen || s->file_size > plen - s->file_off)
			return "bad payload segment";
	}

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	struct fb_info fb;
	con_fb_get(&fb);
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);
	con_hex("FBBASE", fb.base);
	con_hex("FBSIZE", fb.size);

	/*
	 * Refuse to boot only if the payload source overlaps the *destination*
	 * (kernel segments plus the ADT and boot_args placed after them), not
	 * the whole managed RAM window: the bootloader normally loads the
	 * ramdisk inside that window, and the kernel reuses it as free memory
	 * after we jump.
	 */
	uint64_t dst_lo = ~0ULL, dst_hi = 0;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uint64_t dst = s->vmaddr + pa_of_va_base;
		if (dst < dst_lo)
			dst_lo = dst;
		if (dst + s->vmsize > dst_hi)
			dst_hi = dst + s->vmsize;
	}
	{
		uint64_t adt_pa = p->kernel_end + pa_of_va_base;
		uint64_t top = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
		top += PAGE_16K;	/* boot_args */
		if (adt_pa < dst_lo)
			dst_lo = adt_pa;
		if (top > dst_hi)
			dst_hi = top;
	}
	if (rd_start < dst_hi && rd_end > dst_lo)
		return "ramdisk overlaps kernel destination";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + off + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + off + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = fb.base;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = fb.stride;
	ba->Video.v_width = fb.width;
	ba->Video.v_height = fb.height;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
