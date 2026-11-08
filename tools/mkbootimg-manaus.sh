#!/bin/bash
# Package the manaus shim as an Android boot image.
#
# IMPORTANT: this LK has no `fastboot boot` (verified 2026-11-05), so the only
# way to start a kernel is to flash a partition. Never run this against a phone
# without a verified backup of boot_b (and vendor_boot_b for --mode v4).
# See docs/manaus/05-TOMAR-DATOS.md.
#
# The stock device uses boot header v4 (GKI): kernel in `boot`, generic ramdisk
# in `boot`, vendor ramdisk + DTB in `vendor_boot`. Two layouts are produced:
#
#   v4  boot.img = shim + PAYLOAD as its ramdisk; vendor_boot.img = PAYLOAD as
#       vendor ramdisk + mt6879 dtb. Same shape as stock, plus the payload is
#       duplicated so it works whichever ramdisk this LK publishes as
#       /chosen/linux,initrd-start (the shim scans for the payload magic).
#   v2  single boot.img with kernel + ramdisk + dtb (what sweet uses). Only one
#       partition to write, but not a layout this LK is known to exercise.
#
# The ramdisk is the *raw* payload (tools/mkpayload.py), exactly like sweet.
# Do NOT wrap it in a cpio: shim/loader.c reads the payload header directly,
# and the bootloader does not decompress the ramdisk before handing it over.
#
# AOSP mkbootimg silently ignores --dtb/--dtb_offset/--pagesize for header v4
# (checked against mkbootimg.py from system/tools/mkbootimg): the DTB really
# ends up in vendor_boot only. tools/bootimg.py prints what was actually built.
#
# usage: mkbootimg-manaus.sh SHIM_IMAGE PAYLOAD_BIN DTB OUTDIR [--mode v4|v2|both]
set -euo pipefail
cd "$(dirname "$0")/.."

SHIM=${1:?shim Image}
PAYLOAD=${2:?payload.bin}
DTB=${3:?mt6879.dtb}
OUT=${4:?output dir}
shift 4
MODE=v4
while [ $# -gt 0 ]; do
	case "$1" in
	--mode) MODE=${2:?v2|v4|both}; shift 2 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

# From android_device_motorola_mt6879-common/BoardConfigCommon.mk
BASE=0x3fff8000
KERNEL_OFF=0x00008000
TAGS_OFF=0x07c88000
RAMDISK_OFF=0x26f08000
PAGESIZE=4096
# Stock split: empty boot.img cmdline, MTK boot options in vendor_boot.
CMDLINE_BOOT=""
CMDLINE_VENDOR="bootopt=64S3,32N2,64N2 loglevel=4"

command -v mkbootimg >/dev/null || {
	echo "mkbootimg not found (apt install mkbootimg / pip install mkbootimg)" >&2
	exit 1
}
[ -f "$SHIM" ] || { echo "missing shim image: $SHIM" >&2; exit 1; }
[ -f "$PAYLOAD" ] || { echo "missing payload: $PAYLOAD" >&2; exit 1; }
[ -f "$DTB" ] || { echo "missing dtb: $DTB" >&2; exit 1; }

mkdir -p "$OUT"

mk_boot() {  # mk_boot HEADER CMDLINE [extra mkbootimg args...]
	set -x
	mkbootimg --header_version "$1" --kernel "$SHIM" \
		--base "$BASE" --kernel_offset "$KERNEL_OFF" --ramdisk_offset "$RAMDISK_OFF" \
		--tags_offset "$TAGS_OFF" --dtb_offset "$TAGS_OFF" --pagesize "$PAGESIZE" \
		--os_version 13.0.0 --os_patch_level 2035-06 --cmdline "$2" "${@:3}"
	set +x
}

case "$MODE" in
v4)
	# Header v4 ignores the dtb: boot.img carries shim + payload, vendor_boot
	# carries payload + dtb. No bootconfig: stock has bootconfig_size 0.
	mk_boot 4 "$CMDLINE_BOOT" --ramdisk "$PAYLOAD" -o "$OUT/shim-boot.img"
	mkbootimg --vendor_boot "$OUT/shim-vendor_boot.img" --header_version 4 \
		--vendor_ramdisk "$PAYLOAD" --dtb "$DTB" \
		--pagesize "$PAGESIZE" --vendor_cmdline "$CMDLINE_VENDOR"
	IMAGES="$OUT/shim-boot.img $OUT/shim-vendor_boot.img"
	;;
v2)
	mk_boot 2 "$CMDLINE_VENDOR" --ramdisk "$PAYLOAD" --dtb "$DTB" \
		-o "$OUT/shim-boot.img"
	IMAGES="$OUT/shim-boot.img"
	;;
both)
	"$0" "$SHIM" "$PAYLOAD" "$DTB" "$OUT/v4" --mode v4
	"$0" "$SHIM" "$PAYLOAD" "$DTB" "$OUT/v2" --mode v2
	IMAGES=""
	;;
*)
	echo "mode must be v2, v4 or both" >&2
	exit 2
	;;
esac

if [ -n "${IMAGES:-}" ]; then
	echo
	echo "--- what was built (verify before flashing) ---"
	for img in $IMAGES; do
		python3 tools/bootimg.py info "$img" || exit 1
		shasum -a 256 "$img" 2>/dev/null || sha256sum "$img"
		size=$(wc -c < "$img")
		# max-download-size from getvar is 256 MiB
		[ "$size" -lt 268435456 ] || { echo "$img exceeds fastboot max-download-size" >&2; exit 1; }
	done
	cat <<EOF

The header above is the truth: header_version, kernel_size, ramdisk_size and
dtb_size must all be non-zero. The payload is duplicated on purpose: v4 boot.img
and v4 vendor_boot both carry the same raw payload, so the shim finds it in
whichever ramdisk LK advertises as linux,initrd-start. It is not a cpio.

Next (docs/manaus/05-TOMAR-DATOS.md, section 4): with boot_b.img and
vendor_boot_b.img backed up and checked with tools/bootimg.py,
EOF
	case "$MODE" in
	v4)
		cat <<EOF
  fastboot getvar current-slot        # must be b
  fastboot flash boot_b         $OUT/shim-boot.img
  fastboot flash vendor_boot_b  $OUT/shim-vendor_boot.img
  fastboot reboot

Restore:
  fastboot flash boot_b         boot_b.img
  fastboot flash vendor_boot_b  vendor_boot_b.img
EOF
		;;
	v2)
		cat <<EOF
  fastboot getvar current-slot        # must be b
  fastboot flash boot_b         $OUT/shim-boot.img
  fastboot reboot

Restore:
  fastboot flash boot_b         boot_b.img
EOF
		;;
	esac
fi
