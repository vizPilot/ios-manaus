#!/usr/bin/env python3
# Fix for the MTK bootloader's 32-bit sparse writer.
#
# Symptom: flashing a large userdata sparse image fails at the last fragment
# with "sparse image write failure".  The device-side write_sparse_image()
# accumulates total_blocks per chunk and finally compares it to
# sparse_header->total_blks.  A chunk whose BYTE size exceeds 4 GiB overflows
# 32-bit arithmetic on the (32-bit) bootloader, corrupting total_blocks.
#
# The host fastboot does not send the file as written: it imports the sparse
# image into a sparse_file, which MERGES adjacent same-value FILL chunks, and
# then re-sparsifies.  img2simg emits long runs of zero as many adjacent
# FILL(0) chunks, so those runs reappear as one multi-GiB FILL chunk on the
# wire.  (tools/verify-simg.py checks the file's bytes; `diag4` checks what the
# host would actually send.)
#
# Fix: coalesce adjacent FILL chunks of equal value into one logical run, then
# rewrite each run of N fill blocks as k = N // cap units of
# FILL(cap-1) + RAW(1) followed by FILL(N % cap).  The RAW block holds the same
# fill value, so:
#   * every FILL chunk is <= cap-1 blocks (default 64 MiB), far below 4 GiB;
#   * libsparse's merge_bb() (same-type *adjacent* only) cannot re-merge the
#     pieces, so the wire chunks stay small;
#   * the block count is preserved exactly (cap-1 + 1 == cap per unit), so
#     sparse_header->total_blks still matches the partition;
#   * the raw data written is byte-for-byte identical.
#
#   breakfills.py IN OUT [CAP_BLOCKS]      (CAP_BLOCKS default 0x4000 = 64 MiB)
import struct
import sys

MAGIC = 0xED26FF3A
RAW, FILL, DC, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
DEFAULT_CAP = 0x4000  # 64 MiB at 4 KiB blocks


def split_fill(blocks, val, cap):
    """Emit (type, blocks, payload) chunks for a fill run of `blocks` blocks."""
    out = []
    k, r = divmod(blocks, cap)
    for _ in range(k):
        out.append((FILL, cap - 1, val))
        out.append((RAW, 1, val))
    if r:
        out.append((FILL, r, val))
    return out


def main():
    if len(sys.argv) < 3:
        print("usage: breakfills.py IN OUT [CAP_BLOCKS]")
        sys.exit(1)
    inp, outp = sys.argv[1], sys.argv[2]
    cap = int(sys.argv[3], 0) if len(sys.argv) > 3 else DEFAULT_CAP
    if cap < 2:
        sys.exit("CAP_BLOCKS must be >= 2")

    with open(inp, "rb") as f:
        magic, major, minor, fhs, chs, blk, total_blks, total_chunks, cksum = \
            struct.unpack("<IHHHHIIII", f.read(28))
        if magic != MAGIC:
            sys.exit("not an Android sparse image (magic 0x%08x)" % magic)
        if fhs > 28:
            f.read(fhs - 28)

        # Pass 1: coalesce FILL runs and split them; keep other chunks as-is.
        chunks = []          # (type, blocks, payload_or_fillvalue)
        max_in = {RAW: 0, FILL: 0}
        max_out_fill = 0
        blocks_seen = 0
        raw_count = 0
        pending_val = None
        pending_blocks = 0

        def flush():
            nonlocal max_out_fill, pending_val, pending_blocks
            if pending_blocks:
                for c in split_fill(pending_blocks, pending_val, cap):
                    if c[0] == FILL:
                        max_out_fill = max(max_out_fill, c[1])
                    chunks.append(c)
            pending_val, pending_blocks = None, 0

        for _ in range(total_chunks):
            ct, _r, cs, ts = struct.unpack("<HHII", f.read(12))
            if chs > 12:
                f.read(chs - 12)
            if ct == RAW:
                flush(); pending_val = None
                chunks.append((RAW, cs, None))
                raw_count += 1
                max_in[RAW] = max(max_in[RAW], cs * blk)
                f.seek(cs * blk, 1)
            elif ct == FILL:
                val = f.read(4)
                max_in[FILL] = max(max_in[FILL], cs * blk)
                if pending_val is not None and pending_val == val:
                    pending_blocks += cs
                else:
                    flush()
                    pending_val, pending_blocks = val, cs
            elif ct == DC:
                flush(); pending_val = None
                chunks.append((DC, cs, None))
            elif ct == CRC:
                flush(); pending_val = None
                crc = f.read(4)
                chunks.append((CRC, cs, crc))
            else:
                sys.exit("unknown chunk type 0x%04x" % ct)
            blocks_seen += cs
        flush()

        if blocks_seen != total_blks:
            sys.exit("chunk sizes sum to %d blocks, header says %d"
                     % (blocks_seen, total_blks))

    # Pass 2: write the new image, re-reading RAW payloads from IN.
    with open(inp, "rb") as f, open(outp, "wb") as out:
        out.write(struct.pack("<IHHHHIIII", MAGIC, major, minor, fhs, chs,
                              blk, total_blks, len(chunks), cksum))
        f.seek(fhs)
        raw_offsets = []
        for _ in range(total_chunks):
            ct, _r, cs, ts = struct.unpack("<HHII", f.read(12))
            if chs > 12:
                f.read(chs - 12)
            if ct == RAW:
                raw_offsets.append(f.tell())
                f.seek(cs * blk, 1)
            elif ct == FILL:
                f.seek(4, 1)
            elif ct == CRC:
                f.seek(4, 1)
        raw_offsets.reverse()

        out_blocks = 0
        for ct, cs, payload in chunks:
            if ct == RAW:
                if payload is None:
                    off = raw_offsets.pop()
                    f.seek(off)
                    out.write(struct.pack("<HHII", RAW, 0, cs, 12 + cs * blk))
                    left = cs * blk
                    while left:
                        buf = f.read(min(left, 8 << 20))
                        out.write(buf)
                        left -= len(buf)
                else:
                    # Separator RAW chunk holding the fill value: cs * blk bytes.
                    out.write(struct.pack("<HHII", RAW, 0, cs, 12 + cs * blk))
                    out.write(payload * (cs * blk // len(payload)))
            elif ct == FILL:
                out.write(struct.pack("<HHII", FILL, 0, cs, 12 + 4))
                out.write(payload)
            elif ct == DC:
                out.write(struct.pack("<HHII", DC, 0, cs, 12))
            elif ct == CRC:
                out.write(struct.pack("<HHII", CRC, 0, cs, 12 + 4))
                out.write(payload)
            out_blocks += cs

        if out_blocks != total_blks:
            sys.exit("output has %d blocks, expected %d" % (out_blocks, total_blks))

    print("breakfills: %d input chunks -> %d output chunks, %d blocks "
          "(cap %d blocks = %.1f MiB)"
          % (total_chunks, len(chunks), total_blks, cap, cap * blk / 2**20))
    print("breakfills: largest input chunk RAW %.1f MiB / FILL %.1f MiB -> "
          "largest output FILL %.1f MiB"
          % (max_in[RAW] / 2**20, max_in[FILL] / 2**20, max_out_fill * blk / 2**20))
    if max_in[RAW] > 0x100000000:
        print("WARNING: a RAW chunk exceeds 4 GiB and will still overflow the "
              "32-bit MTK writer")


if __name__ == "__main__":
    main()
