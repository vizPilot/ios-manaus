#pragma once
#include <stdint.h>

/*
 * Motorola Edge 40 Neo (manaus). The vendor DTB in vendor_boot.img ships
 * /chosen atag,videolfb-fb_base_l = 0x7e605000 and atag,videolfb-vramSize =
 * 0x1be0000, but LK rewrites both from the real splash allocation at boot.
 * The live values (2026-11-05) are 0xfe0d0000 and 0x1df0000. The shim reads
 * them from the live FDT in con_init(); the constants below are only a
 * fallback for a missing/!valid property.
 *
 * The panel is 1080x2400 (nt36672e / csot vtdr6115), 32bpp. No stride
 * property exists in the DTB; 1080*4 is the natural row length. If the early
 * console looks sheared on hardware, the LK pitch may be 64-byte aligned
 * (4352), so change FB_STRIDE here and in the XNU 0003/0010/0012 overrides.
 */
#define FB_BASE_FALLBACK	0x7e605000UL
#define FB_SIZE_FALLBACK	0x1be0000UL
#define FB_W			1080
#define FB_H			2400
#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

struct fb_info {
	uint64_t base;		/* physical address of the visible buffer */
	uint64_t size;		/* atag,videolfb-vramSize */
	uint32_t width;
	uint32_t height;
	uint32_t stride;	/* bytes per line */
};

extern volatile uint32_t *con_fb;

/* Read the live framebuffer from /chosen; safe to call with dtb == 0. */
void con_init(uintptr_t dtb);
void con_fb_get(struct fb_info *out);
/* 0 when LK handed over with atag,videolfb-islcm_inited = 0 (panel off). */
int con_active(void);

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
