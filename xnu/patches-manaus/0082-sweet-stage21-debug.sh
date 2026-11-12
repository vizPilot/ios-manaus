#!/bin/bash
# Post-patch debug: add stage 21 marker after the last sweet_early_mark in arm_init.c
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
p = "xnu/osfmk/arm/arm_init.c"
s = open(p).read()
# Find the last sweet_early_mark call
import re
matches = list(re.finditer(r'sweet_early_mark\(\d+', s))
print("num sweet_early_mark calls:", len(matches))
if matches:
    last = matches[-1]
    print("last marker at pos", last.start(), ":", s[last.start():last.start()+40])
    # Insert after the line containing the last marker
    line_end = s.find('\n', last.start())
    marker = '\tsweet_early_mark(21, "stage 21 reached");\n'
    s = s[:line_end+1] + marker + s[line_end+1:]
    open(p, "w").write(s)
    print("done: stage 21 marker added")
else:
    print("no sweet_early_mark found")
    raise SystemExit(1)
PY