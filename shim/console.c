/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 *
 * The framebuffer base and size are not compile-time constants on manaus:
 * LK rewrites /chosen/atag,videolfb-* on every boot. con_init() reads the
 * live FDT before the first con_clear(); the constants in console.h are only
 * a fallback.
 */
#include "console.h"
#include "fdt.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE_FALLBACK;
static uint64_t con_fb_size = FB_SIZE_FALLBACK;
static uint32_t con_w = FB_W, con_h = FB_H, con_stride = FB_STRIDE;
static uint32_t con_bg;
static int row;

void con_init(uintptr_t dtb)
{
	uint64_t base = 0, hi = 0, size = 0;

	if (fdt_chosen_u64(dtb, "atag,videolfb-fb_base_l", &base)) {
		if (fdt_chosen_u64(dtb, "atag,videolfb-fb_base_h", &hi))
			base |= hi << 32;
	} else {
		base = FB_BASE_FALLBACK;
	}
	if (!fdt_chosen_u64(dtb, "atag,videolfb-vramSize", &size) || !size)
		size = FB_SIZE_FALLBACK;

	/* Geometry is fixed by the panel; only the buffer moves. */
	uint32_t w = FB_W, h = FB_H, stride = FB_STRIDE;
	if (!base || size < stride) {
		base = FB_BASE_FALLBACK;
		size = FB_SIZE_FALLBACK;
	}
	/* Never draw past the carve-out if a future panel has less VRAM. */
	if (size / stride < h)
		h = (uint32_t)(size / stride);

	con_fb = (volatile uint32_t *)(uintptr_t)base;
	con_fb_size = size;
	con_w = w;
	con_h = h;
	con_stride = stride;
}

void con_fb_get(struct fb_info *out)
{
	out->base = (uint64_t)(uintptr_t)con_fb;
	out->size = con_fb_size;
	out->width = con_w;
	out->height = con_h;
	out->stride = con_stride;
}

void con_clear(uint32_t bg)
{
	for (uint32_t y = 0; y < con_h; y++) {
		volatile uint32_t *line = con_fb + y * (con_stride / 4);
		for (uint32_t x = 0; x < con_w; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > (int)con_w || y0 + CH_H > (int)con_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (con_stride / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
