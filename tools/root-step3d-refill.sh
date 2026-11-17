#!/bin/bash
# Step 3d: re-breakfills an existing sparse image in place.
#
# The first breakfills pass only split FILL chunks LARGER than the cap, but
# img2simg emits long runs of adjacent same-value FILL chunks that the host
# fastboot's libsparse import re-merges into one multi-GiB chunk (exactly the
# 32-bit MTK writer overflow).  The current breakfills.py coalesces such runs
# first, then splits them with 1-block RAW separators.
#
# Input is a verbatim img2simg output (the existing -fstab-bf.simg).  Output is
# built in _build/, verified to expand to raw-new.img (sha256 57fe6c74...), then
# moved over the input.
#
#   root-step3d-refill.sh [INPUT.simg]
set -euo pipefail
OUT=/mnt/d/manaus/ios16-root
TMP=$OUT/_build
REPO="/mnt/c/Users/vicen/OneDrive/Documents/Default Project/ios-sweet"
BF="$REPO/tools/breakfills.py"
VP="$REPO/tools/verify-simg.py"
LIBS=/home/vicente/libsparse
EXPECT=57fe6c749c88eccb52dba1ee21e509fcfc4ebd05c4f9e55a4d17c4353519920d

IN="${1:-$OUT/ios16-root-20H392-manaus-fstab-bf.simg}"
NEW="$TMP/refill.simg"

mkdir -p "$TMP"

echo "=== breakfills self-test (coalescing path) $(date) ==="
ST=$TMP/selftest
rm -rf "$ST"; mkdir -p "$ST"
python3 - "$ST/raw.img" <<'PY'
import sys
mb = 1 << 20
# Two adjacent 4 MiB zero runs and two adjacent 0xff runs: img2simg emits these
# as separate FILL chunks, so only a coalescing pass can keep the wire chunk
# below the cap.
data = (b"\x00" * (4 * mb) + b"\x00" * (4 * mb)
        + b"\xff" * (4 * mb) + b"\xff" * (4 * mb)
        + bytes((i * 7 + 3) & 0xff for i in range(4 * mb))
        + b"\xaa" * (8 * mb))
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
echo "--- diag4 on the self-test output (must be <= 256 KiB) ---"
"$LIBS/diag4" "$ST/raw-bf.simg" 2>&1 | tail -6
rm -rf "$ST"

echo "=== input $(date) ==="
ls -l "$IN"

echo "=== breakfills -> $NEW $(date) ==="
rm -f "$NEW"
python3 "$BF" "$IN" "$NEW"
ls -l "$NEW"

echo "=== diag4 (MTK-safety: max chunk after import+resparse) $(date) ==="
"$LIBS/diag4" "$NEW" 2>&1 | tail -20

echo "=== verify: must expand to raw-new $(date) ==="
python3 "$VP" "$NEW" "$EXPECT"

echo "=== replace input with the re-filled image $(date) ==="
mv -f "$NEW" "$IN"
sha256sum "$IN"
ls -l "$IN"
df -h / /mnt/d
echo "=== done $(date) ==="
