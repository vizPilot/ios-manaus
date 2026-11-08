#pragma once
#include <stdint.h>

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

/*
 * fb_base/fb_size are the live splash framebuffer (con_fb_get()). The FB is
 * mapped Normal non-cacheable so the text console keeps working after the
 * MMU is on. Pass 0/0 to skip it.
 */
int mmu_enable(struct mmu_info *info, uint64_t fb_base, uint64_t fb_size);
uint64_t mmu_probe(uintptr_t va);
