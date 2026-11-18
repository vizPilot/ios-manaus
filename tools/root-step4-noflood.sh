#!/bin/bash
# STEP 4 (option B): remove the console-flooding daemons from launchd.plist so
# the SpringBoard crash text stays visible on the panel, then rebuild the
# MTK-safe sparse image.
#
# Removed jobs (all useless without the matching hardware on manaus):
#   com.apple.bluetoothd, com.apple.bluetoothuserd, com.apple.BluetoothUIService,
#   com.apple.BTServer.{avrcp,le,map,pbap}, com.apple.BlueTool  (Bluetooth family)
#   com.apple.fairplayd.H2                                       (DRM, KeepAlive=true tight loop)
# Everything else (SpringBoard, backboardd, mediaserverd, mobileactivationd, ...) is kept.
set -euo pipefail
REPO="/mnt/c/Users/vicen/OneDrive/Documents/Default Project/ios-sweet"
OUT=/mnt/d/manaus/ios16-root
IMG=$OUT/_mnt/raw.img
M=$OUT/_mnt/root
FULL=$OUT/_build/full-noflood.simg
NEW=$OUT/ios16-root-20H392-manaus-noflood-bf.simg
LIBS=/home/vicente/libsparse

echo "=== 0. inputs $(date) ==="
ls -l "$IMG"

echo
echo "=== 1. mount rw + strip flooders $(date) ==="
mkdir -p "$M"
mountpoint -q "$M" && umount "$M"
mount -t hfsplus -o loop,rw,force "$IMG" "$M"
python3 - "$M" <<'PY'
import plistlib, sys, os
root = sys.argv[1]
p = os.path.join(root, 'System/Library/xpc/launchd.plist')
st = os.stat(p)
d = plistlib.load(open(p, 'rb'))
jobs = d['LaunchDaemons']
before = len(jobs)
labels = {'com.apple.bluetoothd', 'com.apple.bluetoothuserd',
          'com.apple.BluetoothUIService', 'com.apple.fairplayd.H2',
          'com.apple.BlueTool'}
removed = []
for k in list(jobs):
    lab = jobs[k].get('Label', '')
    if lab in labels or lab.startswith('com.apple.BTServer'):
        removed.append((k, lab))
        del jobs[k]
print('LaunchDaemons before=%d after=%d' % (before, len(jobs)))
for k, lab in removed:
    print('  removed %-32s %s' % (lab, k))
for want in ('com.apple.SpringBoard', 'com.apple.backboardd', 'com.apple.mediaserverd'):
    assert any(j.get('Label') == want for j in jobs.values()), want + ' missing!'
print('sanity: SpringBoard/backboardd/mediaserverd still present')
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
os.chown(p, st.st_uid, st.st_gid)
os.chmod(p, st.st_mode & 0o7777)
plistlib.load(open(p, 'rb'))  # re-read: valid plist
print('wrote', p, st.st_mode & 0o7777, st.st_uid, st.st_gid)
PY
sync
umount "$M"
echo "unmounted"

echo
echo "=== 2. fsck $(date) ==="
fsck.hfsplus -f -y "$IMG" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$IMG"

echo
echo "=== 3. img2simg -> full-noflood.simg $(date) ==="
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
echo "raw.img sha256 $RAW_SHA"
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
