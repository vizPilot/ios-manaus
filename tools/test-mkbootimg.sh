#!/bin/bash
# Smoke test for tools/mkbootimg-manaus.sh and tools/bootimg.py.
#
# Needs bash, cpio, gzip, python3 and mkbootimg. Never touches a device: it
# packages random bytes and checks that everything round-trips byte for byte.
#
#   bash tools/test-mkbootimg.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v mkbootimg >/dev/null || { echo "SKIP: mkbootimg not installed" >&2; exit 0; }
command -v python3   >/dev/null || { echo "SKIP: python3 not installed" >&2; exit 0; }

# Some distros ship mkbootimg without the `gki` helper module; it is only used
# with --gki_signing_key, so a stub is enough.
if ! python3 -c 'import gki.generate_gki_certificate' 2>/dev/null; then
    mkdir -p "$WORK/stub/gki"
    : > "$WORK/stub/gki/__init__.py"
    printf '%s\n' \
        'def generate_gki_certificate(*a, **k):' \
        '    raise RuntimeError("stub")' \
        > "$WORK/stub/gki/generate_gki_certificate.py"
    export PYTHONPATH="$WORK/stub${PYTHONPATH:+:$PYTHONPATH}"
fi

cd "$WORK"
head -c 102400 /dev/urandom > shim.bin
head -c 51200  /dev/urandom > payload.bin
head -c 40960  /dev/urandom > mt6879.dtb

bash "$HERE/mkbootimg-manaus.sh" "$WORK/shim.bin" "$WORK/payload.bin" \
     "$WORK/mt6879.dtb" "$WORK/out" --mode both >"$WORK/build.log" 2>&1 || {
        echo "FAIL: packager exited non-zero" >&2; tail -30 "$WORK/build.log" >&2; exit 1; }

# v4 must carry the raw payload in BOTH ramdisks: boot.img's generic ramdisk
# and vendor_boot's vendor ramdisk. The payload is not wrapped in a cpio.
python3 "$HERE/bootimg.py" info "$WORK/out/v4/shim-boot.img" > "$WORK/v4-boot.txt"
python3 "$HERE/bootimg.py" info "$WORK/out/v4/shim-vendor_boot.img" > "$WORK/v4-vb.txt"
python3 "$HERE/bootimg.py" info "$WORK/out/v2/shim-boot.img" > "$WORK/v2-boot.txt"

check() {  # check FILE PATTERN MESSAGE
    grep -q "$2" "$1" || { echo "FAIL: $3" >&2; cat "$1" >&2; exit 1; }
}
check "$WORK/v4-boot.txt" '^header_version *4$'          "v4 boot.img is not header v4"
check "$WORK/v4-boot.txt" '^kernel_size *102400$'       "v4 boot.img kernel size wrong"
check "$WORK/v4-boot.txt" '^ramdisk_size *51200$'       "v4 boot.img must carry the raw payload"
check "$WORK/v4-vb.txt"   '^dtb_size *40960$'           "v4 vendor_boot dtb size wrong"
check "$WORK/v4-vb.txt"   '^vendor_ramdisk_size *51200$' "v4 vendor_boot ramdisk must be the payload"
# LK loads the vendor ramdisk at ramdisk_addr: these must be the stock addresses.
check "$WORK/v4-vb.txt"   '^kernel_addr *0x40000000$'   "v4 vendor_boot kernel_addr not stock"
check "$WORK/v4-vb.txt"   '^ramdisk_addr *0x66f00000$'  "v4 vendor_boot ramdisk_addr not stock"
check "$WORK/v4-vb.txt"   '^tags_addr *0x47c80000$'     "v4 vendor_boot tags_addr not stock"
check "$WORK/v4-vb.txt"   '^dtb_addr *0x47c80000$'      "v4 vendor_boot dtb_addr not stock"
check "$WORK/v2-boot.txt" '^header_version *2$'         "v2 boot.img is not header v2"
check "$WORK/v2-boot.txt" '^dtb_size *40960$'           "v2 boot.img dtb missing"
check "$WORK/v2-boot.txt" '^ramdisk_size *51200$'       "v2 boot.img ramdisk must be the payload"

# Byte-for-byte round-trip (kernel, dtb, ramdisk).
python3 "$HERE/bootimg.py" unpack "$WORK/out/v4/shim-boot.img" "$WORK/x/v4-boot" >/dev/null
python3 "$HERE/bootimg.py" unpack "$WORK/out/v4/shim-vendor_boot.img" "$WORK/x/v4-vb" >/dev/null
python3 "$HERE/bootimg.py" unpack "$WORK/out/v2/shim-boot.img" "$WORK/x/v2-boot" >/dev/null

same() {  # same A B MESSAGE
    cmp -s "$1" "$2" || { echo "FAIL: $3" >&2; exit 1; }
}
same "$WORK/shim.bin" "$WORK/x/v4-boot/kernel"          "v4 boot kernel != shim"
same "$WORK/shim.bin" "$WORK/x/v2-boot/kernel"          "v2 boot kernel != shim"
same "$WORK/mt6879.dtb" "$WORK/x/v4-vb/dtb"             "v4 vendor_boot dtb != input"
same "$WORK/mt6879.dtb" "$WORK/x/v2-boot/dtb"           "v2 boot dtb != input"
same "$WORK/payload.bin" "$WORK/x/v4-boot/ramdisk"      "v4 boot ramdisk != raw payload"
same "$WORK/payload.bin" "$WORK/x/v4-vb/vendor_ramdisk" "v4 vendor ramdisk != raw payload"
same "$WORK/payload.bin" "$WORK/x/v2-boot/ramdisk"      "v2 boot ramdisk != raw payload"

echo "OK: mkbootimg-manaus.sh + bootimg.py round-trip clean"
