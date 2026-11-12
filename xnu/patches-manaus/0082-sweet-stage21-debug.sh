#!/bin/bash
# Post-patch debug: add stage 21 marker after the last sweet_early_mark in arm_init.c
set -e
cd "$(dirname "$0")/.."

# The XNU source is in dxb/xnu/
XNU_SRC="xnu"
if [ ! -d "$XNU_SRC/osfmk" ]; then
    XNU_SRC="."
fi
if [ ! -d "$XNU_SRC/osfmk" ]; then
    echo "XNU source not found"
    exit 1
fi

python3 - <<PY
p = "$XNU_SRC/osfmk/arm/arm_init.c"
s = open(p).read()
import re
matches = list(re.finditer(r'sweet_early_mark\(\d+', s))
print("num sweet_early_mark calls:", len(matches))
if matches:
    last = matches[-1]
    line_end = s.find('\\n', last.start())
    marker = '\\tsweet_early_mark(21, "stage 21 reached");\\n'
    s = s[:line_end+1] + marker + s[line_end+1:]
    open(p, "w").write(s)
    print("done: stage 21 marker added")
else:
    print("no sweet_early_mark found")
    raise SystemExit(1)
PY