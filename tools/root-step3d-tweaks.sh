#!/bin/bash
# Apply the proven SWEET bring-up recipe (tools/sweet-root-tweaks.py) to the
# manaus root image, then rebuild the MTK-safe sparse artifact.
set -euo pipefail
REPO="/mnt/c/Users/vicen/OneDrive/Documents/Default Project/ios-sweet"
OUT=/mnt/d/manaus/ios16-root
IMG=$OUT/_mnt/raw.img
M=$OUT/_mnt/root
TWEAKS="$REPO/tools/sweet-root-tweaks.py"
FULL=$OUT/_build/full-tweaks.simg
NEW=$OUT/ios16-root-20H392-manaus-tweaks-bf.simg
LIBS=/home/vicente/libsparse

echo "=== 0. inputs $(date) ==="
ls -l "$IMG" "$TWEAKS"

echo
echo "=== 1. mount rw + apply sweet-root-tweaks.py $(date) ==="
mkdir -p "$M"
mountpoint -q "$M" && umount "$M"
mount -t hfsplus -o loop,rw,force "$IMG" "$M"
python3 "$TWEAKS" "$M"
echo "--- re-check key artifacts ---"
ls -l "$M/private/var/mobile/Library/Preferences/com.apple.migration.plist"
ls -ld "$M/private/var/containers" "$M/private/var/containers/Bundle" "$M/private/var/containers/Data/System"
grep -c "_PanicOnCrasX" "$M/System/Library/xpc/launchd.plist"
grep -c "com.apple.CommCenter" "$M/System/Library/xpc/launchd.plist" || true
ls -d "$M/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin" 2>/dev/null || echo "VirtualAudio moved (good)"
cat "$M/private/etc/fstab" | tail -1
sync
umount "$M"
echo "unmounted"

echo
echo "=== 2. fsck $(date) ==="
fsck.hfsplus -f -y "$IMG" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$IMG"
sha256sum "$IMG"

echo
echo "=== 3. img2simg -> full-tweaks.simg $(date) ==="
rm -f "$FULL"
"$LIBS/img2simg" "$IMG" "$FULL" 4096
ls -l "$FULL"

echo
echo "=== 4. breakfills -> $NEW $(date) ==="
rm -f "$NEW"
python3 "$REPO/tools/breakfills.py" "$FULL" "$NEW"
ls -l "$NEW"

echo
echo "=== 5. diag4 (MTK-safety) $(date) ==="
"$LIBS/diag4" "$NEW" 2>&1 | tail -20 || true

echo
echo "=== 6. verify expansion == raw.img $(date) ==="
RAW_SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
python3 "$REPO/tools/verify-simg.py" "$NEW" "$RAW_SHA"

echo
echo "=== 7. drop 12 GB intermediate $(date) ==="
rm -f "$FULL"

echo
echo "=== 8. final $(date) ==="
sha256sum "$NEW"
ls -l "$NEW"
df -h / /mnt/d
echo "=== done $(date) ==="
