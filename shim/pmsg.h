#pragma once
#include <stdint.h>

/*
 * Persistent log for bring-up when the panel is dark.
 *
 * manaus hands over with /chosen atag,videolfb-islcm_inited = 0: the
 * framebuffer carve-out exists but the LCM is not initialized, so drawing to
 * it is invisible (and writing 10 MB of Device memory with the MMU off is
 * pointlessly slow). Instead, every console line is also appended to a valid
 * ramoops persistent_ram record; the next boot's kernel saves it and exposes
 * it as /sys/fs/pstore/{pmsg,console}-ramoops-0.
 *
 * Layout derived from the live /proc/cmdline and /chosen/bootargs (measured
 * 2026-11-08):
 *   ramoops.mem_address=0x48090000 mem_size=0xe0000
 *   record_size=4096 console_size=0x40000 ftrace_size=0x1000 pmsg_size=0x10000
 * ramoops_register_dummy() lays the zones out in this order:
 *   143 dmesg zones * 0x1000 = 0x8f000  -> 0x48090000..0x4811f000
 *   console 0x40000                     -> 0x4811f000..0x4815f000
 *   8 ftrace zones * 0x200 = 0x1000     -> 0x4815f000..0x48160000
 *   pmsg 0x10000                        -> 0x48160000..0x48170000
 *
 * persistent_ram_buffer (no ECC: ramoops.ecc is unset, so ecc_size = 0):
 *   u32 sig = 0x43474244 ("DBGC"), u32 start, u32 size, data[]
 * The kernel stores sig = (passed sig) ^ PERSISTENT_RAM_SIG and ram.c passes
 * 0 for dmesg/console/pmsg, so 0x43474244 is correct for both zones.
 */
void pmsg_reset(void);
void pmsg_str(const char *name, const char *val);
void pmsg_hex(const char *name, uint64_t val);
void pmsg_hex2(const char *label, const char *a, uint64_t va, const char *b, uint64_t vb);
void pmsg_dec(const char *name, uint64_t val);
void pmsg_skip(void);
void pmsg_flush(void);
