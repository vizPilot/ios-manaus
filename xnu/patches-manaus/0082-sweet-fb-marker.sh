#!/bin/bash
# Post-patch debug: write marker to FB (already mapped) instead of pstore
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
# Add FB marker to sweet_early_init (FB is mapped in bootstrap, unlike pstore).
p = "xnu/osfmk/arm64/sweet_early.c"
s = open(p).read()
old = "\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n}"
new = ("\tearly_stride = (uint32_t)(ba->Video.v_rowBytes / 4);\n"
       "\t/* FB marker: write XNU alive to top-left corner of FB */\n"
       "\tif (early_fb != NULL) {\n"
       "\t\tearly_fb[0] = 0xff00ff00u;\n"
       "\t\tearly_fb[1] = 0xff00ff00u;\n"
       "\t\tearly_fb[2] = 0xff00ff00u;\n"
       "\t}\n"
       "}")
assert old in s, "pattern not found in sweet_early.c"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("sweet_early.c: FB marker added")
print("done")
PY