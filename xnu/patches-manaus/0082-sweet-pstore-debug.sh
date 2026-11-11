#!/bin/bash
# Add pstore marker to sweet_early.c for manaus debug
FILE=osfmk/arm64/sweet_early.c
if [ -f "$FILE" ]; then
    python3 - <<'PY'
p = "osfmk/arm64/sweet_early.c"
s = open(p).read()
old = "\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n}"
new = ("\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n"
       "\t/* PSTORE: write XNU alive marker to ramoops pmsg zone */\n"
       "\tvolatile uint32_t *pmsg = (volatile uint32_t *)0x48160000UL;\n"
       "\tpmsg[0] = 0x43474244u;\n"
       "\tpmsg[1] = 0;\n"
       "\tpmsg[2] = 4;\n"
       "\tpmsg[3] = 0x584e5521u;\n"
       "}")
assert old in s, "pattern not found"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("done")
PY
fi
