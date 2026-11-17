#!/usr/bin/env python3
# Fix for the MTK bootloader's 32-bit sparse writer.
#
# Symptom: flashing a large userdata sparse image fails at the last fragment
# with "sparse image write failure".  Device-side write_sparse_image()
# accumulates total_blocks per chunk and finally compares it to
# sparse_header->total_blks.  A single chunk whose BYTE size exceeds 4 GiB
# overflows 32-bit arithmetic on the (32-bit) bootloader, corrupting
# total_blocks -> the check fails.
#
# Fix: emit no FILL chunk larger than CAP blocks.  Each run of N fill blocks is
# rewritten as alternating FILL(cap-1) + RAW(1) units (the RAW block holds the
# same fill value), so:
#   * every FILL chunk stays far below the 4 GiB overflow threshold, and
#   * the total block count is preserved exactly (cap-1 + 1 == cap per unit),
#     so sparse_header->total_blks still matches the partition size, and
#   * libsparse's merge_bb() (which merges only same-type *adjacent* chunks)
#     cannot re-merge the pieces back into one huge FILL chunk.
# The raw data written is byte-for-byte identical.
#
#   breakfills.py IN OUT [CAP_BLOCKS]      (CAP_BLOCKS default 0x4000 = 64 MiB)
import struct
import sys

MAGIC = 0xED26FF3A
RAW, FILL, DC, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
DEFAULT_CAP = 0x4000  # 64 MiB at 4 KiB blocks


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

        # Pass 1: read all chunk headers, split oversized FILLs, count chunks.
        chunks = []          # (type, blocks, payload_or_fillvalue)
        max_bytes = {RAW: 0, FILL: 0, DC: 0, CRC: 0}
        n_fill_split = 0
        blocks_seen = 0
        for _ in range(total_chunks):
            ct, _r, cs, ts = struct.unpack("<HHII", f.read(12))
            if chs > 12:
                f.read(chs - 12)
            if ct == RAW:
                chunks.append((RAW, cs, None))
                max_bytes[RAW] = max(max_bytes[RAW], cs * blk)
                f.seek(cs * blk, 1)
            elif ct == FILL:
                val = f.read(4)
                max_bytes[FILL] = max(max_bytes[FILL], cs * blk)
                if cs > cap:
                    n_fill_split += 1
                    remaining = cs
                    while remaining > cap:
                        chunks.append((FILL, cap - 1, val))
                        chunks.append((RAW, 1, val))
                        remaining -= cap
                    if remaining:
                        chunks.append((FILL, remaining, val))
                else:
                    chunks.append((FILL, cs, val))
            elif ct == DC:
                chunks.append((DC, cs, None))
                max_bytes[DC] = max(max_bytes[DC], cs * blk)
            elif ct == CRC:
                crc = f.read(4)
                chunks.append((CRC, cs, crc))
            else:
                sys.exit("unknown chunk type 0x%04x" % ct)
            blocks_seen += cs

        if blocks_seen != total_blks:
            sys.exit("chunk sizes sum to %d blocks, header says %d"
                     % (blocks_seen, total_blks))

    # Pass 2: write the new image, re-reading RAW payloads from IN.
    with open(inp, "rb") as f, open(outp, "wb") as out:
        out.write(struct.pack("<IHHHHIIII", MAGIC, major, minor, fhs, chs,
                              blk, total_blks, len(chunks), cksum))
        # Re-scan the input for RAW payload offsets.
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
                    # Original RAW chunk: copy its bytes.
                    off = raw_offsets.pop()
                    f.seek(off)
                    out.write(struct.pack("<HHII", RAW, 0, cs, 12 + cs * blk))
                    left = cs * blk
                    while left:
                        buf = f.read(min(left, 8 << 20))
                        out.write(buf)
                        left -= len(buf)
                else:
                    # Separator RAW chunk holding the fill value: the payload
                    # must be cs * blk bytes, not just the 4-byte value.
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

    print("breakfills: %d chunks -> %d chunks, %d blocks, %d FILL chunk(s) split"
          % (total_chunks, len(chunks), total_blks, n_fill_split))
    print("breakfills: largest input chunk: RAW %.1f MiB, FILL %.1f MiB "
          "(cap %d blocks = %.1f MiB)"
          % (max_bytes[RAW] / 2**20, max_bytes[FILL] / 2**20,
             cap, cap * blk / 2**20))
    if max_bytes[RAW] > 0x100000000:
        print("WARNING: a RAW chunk exceeds 4 GiB and will still overflow the "
              "32-bit MTK writer")


if __name__ == "__main__":
    main()
