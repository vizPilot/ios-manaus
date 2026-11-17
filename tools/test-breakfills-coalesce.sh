#!/bin/bash
# Unit-test the coalescing path of breakfills.py with a hand-built sparse image
# whose FILL chunks are adjacent and share a value -- the exact structure that
# libsparse re-merges into a multi-GiB chunk on the wire.
set -euo pipefail
REPO="/mnt/c/Users/vicen/OneDrive/Documents/Default Project/ios-sweet"
BF="$REPO/tools/breakfills.py"
LIBS=/home/vicente/libsparse
T=/mnt/d/manaus/ios16-root/_build/bftest-coalesce
rm -rf "$T"; mkdir -p "$T"

echo "=== build a synthetic sparse image with adjacent same-value FILL chunks ==="
python3 - "$T/in.simg" "$T/expect.raw" <<'PY'
import struct, sys
MAGIC = 0xED26FF3A
RAW, FILL = 0xCAC1, 0xCAC2
BLK = 4096

def raw_fill(val, blocks):
    return val * (blocks * BLK // len(val))

# (type, blocks, payload) with deliberately adjacent equal-value FILLs.
chunks = [
    (FILL, 1000, b"\x00\x00\x00\x00"),
    (FILL, 1000, b"\x00\x00\x00\x00"),   # adjacent, same value -> must coalesce
    (FILL, 500,  b"\x00\x00\x00\x00"),
    (RAW,  2,    bytes((i * 13 + 1) & 0xff for i in range(2 * BLK))),
    (FILL, 300,  b"\xef\xbe\xad\xde"),
    (FILL, 300,  b"\xef\xbe\xad\xde"),   # adjacent, same value -> must coalesce
    (FILL, 70,   b"\x11\x22\x33\x44"),
]
total = sum(c[1] for c in chunks)
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK, total,
                        len(chunks), 0))
    for ct, blocks, payload in chunks:
        f.write(struct.pack("<HHII", ct, 0, blocks, 12 + len(payload)))
        f.write(payload)
with open(sys.argv[2], "wb") as g:
    for ct, blocks, payload in chunks:
        if ct == FILL:
            g.write(raw_fill(payload, blocks))
        else:
            g.write(payload)
print("chunks:", len(chunks), "blocks:", total)
PY

echo "=== input chunk runs ==="
python3 "$REPO/tools/simg-chunkruns.py" "$T/in.simg"

echo "=== breakfills (cap 64 blocks, forces splitting) ==="
python3 "$BF" "$T/in.simg" "$T/out.simg" 64

echo "=== output chunk runs (no merged run may exceed the cap) ==="
python3 "$REPO/tools/simg-chunkruns.py" "$T/out.simg"

echo "=== diag4 on the output (max chunk must be <= 256 KiB here) ==="
"$LIBS/diag4" "$T/out.simg" 2>&1 | tail -5

echo "=== expand and compare byte-for-byte ==="
"$LIBS/simg2img" "$T/out.simg" "$T/out.raw"
sha256sum "$T/expect.raw" "$T/out.raw"
if cmp -s "$T/expect.raw" "$T/out.raw"; then
  echo "RESULT: OK (byte-identical)"
else
  echo "RESULT: MISMATCH"; exit 1
fi
