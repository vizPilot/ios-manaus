#!/bin/bash
# Post-patch debug: map pstore in bootstrap + write marker to pstore from XNU
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
import os

# 1) Map pstore (0x48000000) in start.s bootstrap page tables.
p = "xnu/osfmk/arm64/start.s"
if os.path.exists(p):
    s = open(p).read()
    old = "\tLsweet_fb_map_done:\n"
    new = ("\tLsweet_fb_map_done:\n"
           "#if defined(SWEET)\n"
           "\t/* Map pstore (0x48000000, 32MB) as Normal-NC for debug. */\n"
           "\tmov\t\tx10, #0x48000000\n"
           "\tlsr\t\tx12, x10, #(ARM_TT_L2_SHIFT)\n"
           "\tlsl\t\tx10, x12, #(ARM_TT_L2_SHIFT)\n"
           "\tMOV64\tx11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)\n"
           "\torr\t\tx11, x11, x10\n"
           "\tadd\t\tx13, x25, #(2 * PGBYTES)\n"
           "\tstr\t\tx11, [x13, x12, lsl #TTE_SHIFT]\n"
           "#endif\n")
    assert old in s, "Lsweet_fb_map_done not found"
    s = s.replace(old, new, 1)
    open(p, "w").write(s)
    print("start.s: pstore mapping added")

# 2) Add pstore write to sweet_early_init.
p2 = "xnu/osfmk/arm64/sweet_early.c"
if os.path.exists(p2):
    s2 = open(p2).read()
    old2 = "\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n}"
    new2 = ("\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n"
            "\t/* PSTORE: write XNU alive marker to ramoops pmsg zone */\n"
            "\tvolatile uint32_t *pmsg = (volatile uint32_t *)0x48160000UL;\n"
            "\tpmsg[0] = 0x43474244u;\n"
            "\tpmsg[1] = 0;\n"
            "\tpmsg[2] = 4;\n"
            "\tpmsg[3] = 0x584e5521u;\n"
            "}")
    assert old2 in s2, "pattern not found in sweet_early.c"
    s2 = s2.replace(old2, new2, 1)
    open(p2, "w").write(s2)
    print("sweet_early.c: pstore marker added")
print("done")
PY