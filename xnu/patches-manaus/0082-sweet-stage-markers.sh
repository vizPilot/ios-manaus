#!/bin/bash
# Post-patch debug: force sweet_early markers to draw even after disable,
# and add stage markers 21+ after the stage-20 point in arm_init.c.
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
import re, sys

# 1) Force sweet_early to draw even when "disabled" (markers only; the
#    early_disabled flag is set by sweet_early_disable() at stage 20).
p = "osfmk/arm64/sweet_early.c"
s = open(p).read()
old = "if (early_disabled || early_fb == NULL || stage > 20 || early_height < 32u) {"
new = "if (early_fb == NULL || stage > 30 || early_height < 32u) {"
if old in s:
    s = s.replace(old, new, 1)
    print("sweet_early: mark() gate relaxed")
old = "if (early_disabled || early_fb == NULL) {"
new = "if (early_fb == NULL) {"
if old in s:
    s = s.replace(old, new, 1)
    print("sweet_early: putc() gate relaxed")
open(p, "w").write(s)

# 2) Add markers 21..28 in arm_init.c after the stage-20 point.
p2 = "osfmk/arm/arm_init.c"
s2 = open(p2).read()

def after(anchor, marker):
    global s2
    if marker in s2:
        print("arm_init: %s already present" % marker)
        return
    if anchor not in s2:
        print("arm_init: ANCHOR NOT FOUND for %s: %r" % (marker, anchor[:60]))
        return
    s2 = s2.replace(anchor, anchor + marker, 1)
    print("arm_init: inserted %s" % marker.strip())

after('\tif (serial_exists) {\n\t\tsweet_early_disable();\n\t}\n',
      '\tsweet_early_mark(21, "post-disable");\n')
after('\tPE_create_console();\n',
      '\tsweet_early_mark(22, "PE_create_console");\n')
after('\tPE_init_printf(FALSE);\n',
      '\tsweet_early_mark(23, "PE_init_printf");\n')
after('\tcpu_machine_idle_init(TRUE);\n',
      '\tsweet_early_mark(24, "cpu_machine_idle_init");\n')
after('\tPE_init_platform(TRUE, &BootCpuData);\n',
      '\tsweet_early_mark(25, "PE_init_platform");\n')
after('\tcpu_timebase_init(TRUE);\n',
      '\tsweet_early_mark(26, "cpu_timebase_init");\n')
after('\tPE_init_cpu();\n\tfiq_context_init(TRUE);\n',
      '\tsweet_early_mark(27, "PE_init_cpu+fiq");\n')
after('\tmachine_startup(args);\n',
      '\tsweet_early_mark(28, "machine_startup");\n')
open(p2, "w").write(s2)
print("done")
PY