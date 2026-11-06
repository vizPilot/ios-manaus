#!/bin/bash
# Package the manaus shim as an Android boot image.
#
# The stock device uses boot header v4 (GKI): kernel in `boot`, ramdisk in
# `vendor_boot`, DTB in the boot image at dtb_offset. ios-sweet's shim puts its
# payload (XNU + ADT + boot-args) in the RAM disk, so there are two ways to try
# it, and only the phone can tell which one LK accepts:
#
#   v2  single boot.img with kernel + ramdisk + dtb (what sweet uses)
#   v4  boot.img (shim as kernel) + vendor_boot.img (payload ramdisk)
#
# UNTESTED on hardware. Do not flash anything until `fastboot boot` (or a
# throwaway boot partition) has been tried with a stock image first.
#
# usage: mkbootimg-manaus.sh SHIM_IMAGE PAYLOAD_BIN DTB OUTDIR [--mode v2|v4|both]
set -euo pipefail
cd "$(dirname "$0")/.."

SHIM=${1:?shim Image}
PAYLOAD=${2:?payload.bin}
DTB=${3:?mt6879.dtb}
OUT=${4:?output dir}
shift 4
MODE=v2
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
CMDLINE="bootopt=64S3,32N2,64N2 console=null"

mkdir -p "$OUT"

# newc cpio is what LK expects to hand over as initrd.
(cd "$(dirname "$PAYLOAD")" && echo "$(basename "$PAYLOAD")" | cpio -o -H newc --quiet | gzip -9) > "$OUT/payload.cpio.gz"

mk() {
	set -x
	mkbootimg --header_version "$1" --kernel "$SHIM" --dtb "$DTB" \
		--base "$BASE" --kernel_offset "$KERNEL_OFF" --ramdisk_offset "$RAMDISK_OFF" \
		--tags_offset "$TAGS_OFF" --dtb_offset "$TAGS_OFF" --pagesize "$PAGESIZE" \
		--os_version 13.0.0 --os_patch_level 2035-06 --cmdline "$CMDLINE" "${@:2}"
	set +x
}

case "$MODE" in
v2)
	mk 2 --ramdisk "$OUT/payload.cpio.gz" -o "$OUT/shim-boot.img"
	echo "built $OUT/shim-boot.img (header v2, kernel+ramdisk+dtb)"
	;;
v4)
	mk 4 -o "$OUT/shim-boot.img"
	printf 'androidboot.selinux=permissive\n' > "$OUT/bootconfig"
	mkbootimg --vendor_boot "$OUT/shim-vendor_boot.img" --header_version 4 \
		--vendor_ramdisk "$OUT/payload.cpio.gz" --dtb "$DTB" \
		--vendor_bootconfig "$OUT/bootconfig" --pagesize "$PAGESIZE" \
		--vendor_cmdline "$CMDLINE"
	echo "built $OUT/shim-boot.img + $OUT/shim-vendor_boot.img (header v4)"
	;;
both)
	"$0" "$SHIM" "$PAYLOAD" "$DTB" "$OUT/v2" --mode v2
	"$0" "$SHIM" "$PAYLOAD" "$DTB" "$OUT/v4" --mode v4
	;;
*)
	echo "mode must be v2, v4 or both" >&2
	exit 2
	;;
esac

cat <<EOF

Next (read docs/manaus/04-M1.md first):
  fastboot boot $OUT/shim-boot.img                 # v2, preferred
  # or, only if v2 is rejected and you accept flashing boot:
  fastboot flash boot $OUT/shim-boot.img
  fastboot flash vendor_boot $OUT/shim-vendor_boot.img

Keep a stock boot.img and vendor_boot.img to restore.
EOF
