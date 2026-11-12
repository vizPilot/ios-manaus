#!/bin/bash
# Post-patch debug: add stage 21 marker after kprintf startup complete in arm_init.c
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
p = "xnu/osfmk/arm/arm_init.c"
s = open(p).read()
# Find the kprintf startup complete marker (flexible search)
import re
idx = s.find('kprintf startup complete')
print("found 'kprintf startup complete' at", idx)
if idx < 0:
    print("NOT FOUND")
    raise SystemExit(1)
# Find the end of that line
line_end = s.find('\n', idx)
# Insert marker after that line
marker = '\tsweet_early_mark(21, "stage 21 reached");\n'
s = s[:line_end+1] + marker + s[line_end+1:]
open(p, "w").write(s)
print("done: stage 21 marker added")
PY