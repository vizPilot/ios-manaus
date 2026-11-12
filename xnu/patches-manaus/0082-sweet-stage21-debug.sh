#!/bin/bash
# Add stage 21 marker after kprintf startup complete in arm_init.c
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
p = "xnu/osfmk/arm/arm_init.c"
s = open(p).read()
old = '\tsweet_early_mark(20, "kprintf startup complete");\n'
new = ('\tsweet_early_mark(20, "kprintf startup complete");\n'
       '\tsweet_early_mark(21, "stage 21 reached");\n')
assert old in s, "stage 20 marker not found"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("done: stage 21 marker added")
PY