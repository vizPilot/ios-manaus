#!/bin/bash
# Step 3c: finish the build from the already-written full.simg (img2simg done).
#   - quick breakfills self-test on a small synthetic image
#   - breakfills the real 12 GB image into the flashable MTK-safe sparse
#   - diag4 (libsparse import+resparse) MTK-safety check
#   - verify the result expands to exactly raw-new.img (sha256 57fe6c74...)
set -euo pipefail
OUT=/mnt/d/manaus/ios16-root
TMP=$OUT/_build
REPO="/mnt/c/Users/vicen/OneDrive/Documents/Default Project/ios-sweet"
BF="$REPO/tools/breakfills.py"
VP="$REPO/tools/verify-simg.py"
LIBS=/home/vicente/libsparse
EXPECT=57fe6c749c88eccb52dba1ee21e509fcfc4ebd05c4f9e55a4d17c4353519920d

FULL=$TMP/full.simg
BF_IMG=$OUT/ios16-root-20H392-manaus-fstab-bf.simg
DIFF_IMG=$OUT/ios16-root-20H392-manaus-fstab-diff.simg

echo "=== breakfills self-test (small synthetic image) $(date) ==="
ST=$TMP/selftest
rm -rf "$ST"; mkdir -p "$ST"
python3 - "$ST/raw.img" <<'PY'
import sys
mb = 1 << 20
data = (b"\x00" * (4 * mb) + b"\xff" * (4 * mb)
        + bytes((i * 7 + 3) & 0xff for i in range(4 * mb)) + b"\xaa" * (4 * mb))
open(sys.argv[1], "wb").write(data)
PY
"$LIBS/img2simg" "$ST/raw.img" "$ST/raw.simg" 4096
python3 "$BF" "$ST/raw.simg" "$ST/raw-bf.simg" 64
"$LIBS/simg2img" "$ST/raw.simg"    "$ST/a.img"
"$LIBS/simg2img" "$ST/raw-bf.simg" "$ST/b.img"
if cmp -s "$ST/raw.img" "$ST/a.img" && cmp -s "$ST/a.img" "$ST/b.img"; then
  echo "SELFTEST: OK (byte-identical)"
else
  echo "SELFTEST: FAIL"; exit 1
fi
rm -rf "$ST"

echo "=== breakfills -> $BF_IMG $(date) ==="
ls -l "$FULL"
rm -f "$BF_IMG"
python3 "$BF" "$FULL" "$BF_IMG"
ls -l "$BF_IMG"

echo "=== diag4 (MTK-safety: max chunk after import+resparse) $(date) ==="
"$LIBS/diag4" "$BF_IMG" 2>&1 | tail -20 || true

echo "=== verify: must expand to raw-new $(date) ==="
python3 "$VP" "$BF_IMG" "$EXPECT"

echo "=== drop the 12 GB intermediate $(date) ==="
rm -f "$FULL"

echo "=== final artifacts $(date) ==="
sha256sum "$BF_IMG" "$DIFF_IMG"
ls -l "$BF_IMG" "$DIFF_IMG"
df -h / /mnt/d
echo "=== done $(date) ==="
