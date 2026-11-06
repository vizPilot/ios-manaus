#!/system/bin/sh
# manaus-intake.sh - read-only hardware inventory for Motorola Edge 40 Neo (manaus / MT6879)
#
# Purpose: collect everything needed to port XNU (ios-sweet) to this SoC without
# guessing addresses. Nothing here writes to partitions or changes settings.
#
# How to run (root shell required for /proc/device-tree, dmesg and /dev/block):
#   adb push manaus-intake.sh /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/manaus-intake.sh /sdcard/manaus-intake'
#   adb pull /sdcard/manaus-intake.tgz
#
# In Lineage Recovery / OrangeFox, "adb shell" is already root:
#   adb push manaus-intake.sh /tmp/
#   adb shell sh /tmp/manaus-intake.sh /tmp/manaus-intake
#   adb pull /tmp/manaus-intake.tgz
#
# Output: a directory plus <dir>.tgz with one file per probe.
# Review it before sharing: it may contain serial numbers (ro.serialno, ro.boot.serialno).

OUT=${1:-/sdcard/manaus-intake}
export OUT
mkdir -p "$OUT" 2>/dev/null || { echo "cannot create $OUT"; exit 1; }

run() {
    name=$1
    shift
    sh -c "$*" > "$OUT/$name" 2>&1
}

echo "== manaus-intake =="
run 00-id            'id; uname -a; date'
run 01-props         'getprop'
run 02-boot-props    'getprop | grep -E "ro\.boot|ro\.product|ro\.hardware|ro\.board|ro\.soc|ro\.serialno"'
run 03-cmdline       'cat /proc/cmdline'
run 04-cpuinfo       'cat /proc/cpuinfo'
run 05-meminfo       'cat /proc/meminfo'
run 06-iomem         'cat /proc/iomem'
run 07-interrupts    'cat /proc/interrupts'
run 08-partitions    'cat /proc/partitions; echo; ls -l /dev/block/by-name 2>/dev/null; ls -l /dev/block/platform 2>/dev/null'
run 09-mounts        'cat /proc/mounts; echo; df -h 2>/dev/null'
run 10-input         'cat /proc/bus/input/devices'
run 11-dmesg         'dmesg'
run 12-dmesg-hw      'dmesg | grep -iE "ufs|ufshc|scsi|mmc|dsi|panel|drm|mtkfb|backlight|goodix|focaltech|novatek|touch|i2c|pwrap|pmic|mt635|charger|gauge|regulator|gic|timer|psci|uart|ramoops|pstore"'
run 13-device-tree   'cp -r /proc/device-tree "$OUT/device-tree" 2>/dev/null || cp -r /sys/firmware/devicetree/base "$OUT/device-tree" 2>/dev/null'
run 14-fdt-blob      'cat /sys/firmware/fdt > "$OUT/fdt.dtb" 2>/dev/null; ls -l "$OUT/fdt.dtb" 2>/dev/null'
run 15-sys-firmware  'ls -lR /sys/firmware 2>/dev/null | head -200'
run 16-platform-dev  'ls -l /sys/bus/platform/devices 2>/dev/null'
run 17-i2c-devices   'for d in /sys/bus/i2c/devices/*; do echo "== $d"; cat "$d/name" 2>/dev/null; cat "$d/of_node/compatible" 2>/dev/null | tr "\\000" " "; echo; done'
run 18-drm           'for d in /sys/class/drm/*; do echo "== $d"; cat "$d/status" 2>/dev/null; cat "$d/modes" 2>/dev/null; done'
run 19-backlight     'for d in /sys/class/backlight/* /sys/class/leds/*; do echo "== $d"; cat "$d/max_brightness" 2>/dev/null; done'
run 20-power         'for d in /sys/class/power_supply/*; do echo "== $d"; cat "$d/uevent" 2>/dev/null; done'
run 21-gic-nodes     'for d in /sys/bus/platform/devices/*interrupt* /sys/bus/platform/devices/*gic*; do echo "== $d"; cat "$d/of_node/compatible" 2>/dev/null | tr "\\0" " "; echo; done 2>/dev/null'
run 22-pstore        'ls -l /sys/fs/pstore 2>/dev/null; for f in /sys/fs/pstore/*; do echo "== $f"; head -c 200000 "$f" 2>/dev/null; done 2>/dev/null'
run 23-block-dev     'ls -l /dev/block 2>/dev/null | head -100'
run 24-slot          'getprop ro.boot.slot_suffix; getprop ro.boot.verifiedbootstate; getprop ro.boot.flash.locked'
run 25-ufs-sysfs     'for d in /sys/devices/platform/*ufs* /sys/bus/platform/drivers/ufshcd* /sys/class/scsi_host/*; do echo "== $d"; ls -l "$d" 2>/dev/null | head -30; done 2>/dev/null'
run 26-proc-config   'cat /proc/config.gz 2>/dev/null | gzip -dc 2>/dev/null | grep -E "CONFIG_(MTK|UFS|SCSI_UFS|DRM|DRM_MEDIATEK|I2C_MT65XX|PINCTRL|MT635|PSTORE|RAMOOPS|SERIAL_8250)" | head -200'
# CPU clocks (ADT clock-frequency) - cpufreq is readable without root on most builds.
run 27-cpufreq       'for d in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do echo "== $d"; cat "$d/cpuinfo_max_freq" "$d/cpuinfo_min_freq" 2>/dev/null; done'
# Panel geometry as the bootloader/Android leaves it (stride, bpp, smem).
run 28-fb            'for d in /sys/class/graphics/fb*; do echo "== $d"; cat "$d/virtual_size" "$d/stride" "$d/bits_per_pixel" "$d/smem_start" "$d/modes" 2>/dev/null; done'
# Boot image headers only (4 KiB each): MTK header v4, kernel base, dtb offset/size.
run 29-bootimg-hdr   'for p in boot_a boot_b vendor_boot_a; do echo "== $p"; dd if=/dev/block/by-name/$p bs=4096 count=1 2>/dev/null | od -A x -t x1z | head -40; done'

# Bundle it. Toybox tar supports -czf.
if tar -czf "$OUT.tgz" -C "$(dirname "$OUT")" "$(basename "$OUT")" 2>/dev/null; then
    echo "bundle: $OUT.tgz"
else
    echo "tar failed; pull the directory $OUT instead"
fi

echo
echo "== summary =="
echo "device tree: $([ -d "$OUT/device-tree" ] && echo yes || echo NO)"
echo "dmesg bytes: $(wc -c < "$OUT/11-dmesg" 2>/dev/null)"
echo "ufs lines:   $(grep -ic ufs "$OUT/11-dmesg" 2>/dev/null)"
echo "panel lines: $(grep -icE 'panel|dsi|drm' "$OUT/11-dmesg" 2>/dev/null)"
echo "touch lines: $(grep -icE 'goodix|focaltech|novatek|touch' "$OUT/11-dmesg" 2>/dev/null)"
echo
echo "Next: fill hw/manaus.inventory.json with the values you find."
echo "Remove ro.serialno / ro.boot.serialno before sharing 01-props and 02-boot-props."
