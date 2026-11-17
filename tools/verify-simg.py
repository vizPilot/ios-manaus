#!/usr/bin/env python3
"""Verify a flashable sparse image without expanding it to disk.

  verify-simg.py SPARSE.simg [EXPECTED_SHA256]

Expands the image logically (RAW bytes copied, FILL repeated, DONT_CARE zeroed)
and prints the sha256 of the resulting raw partition image plus its size.  With
an expected sha256 it exits non-zero on mismatch.  Read-only.
"""
import hashlib
import struct
import sys

MAGIC = 0xED26FF3A
RAW, FILL, DC, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    simg = sys.argv[1]
    expected = sys.argv[2].lower() if len(sys.argv) > 2 else None

    h = hashlib.sha256()
    total = 0
    counts = {RAW: 0, FILL: 0, DC: 0, CRC: 0}
    with open(simg, "rb") as f:
        magic, major, minor, fhs, chs, blk, total_blks, total_chunks, ck = \
            struct.unpack("<IHHHHIIII", f.read(28))
        if magic != MAGIC:
            sys.exit("not an Android sparse image (magic 0x%08x)" % magic)
        if fhs > 28:
            f.read(fhs - 28)
        for _ in range(total_chunks):
            ct, _r, cs, ts = struct.unpack("<HHII", f.read(12))
            if chs > 12:
                f.read(chs - 12)
            counts[ct] = counts.get(ct, 0) + 1
            nbytes = cs * blk
            if ct == RAW:
                left = nbytes
                while left:
                    buf = f.read(min(left, 8 << 20))
                    if not buf:
                        sys.exit("truncated RAW chunk")
                    h.update(buf)
                    left -= len(buf)
            elif ct == FILL:
                val = f.read(4)
                block = val * blk
                left = nbytes
                while left:
                    chunk = block[: min(left, len(block))]
                    h.update(chunk)
                    left -= len(chunk)
            elif ct == DC:
                zero = b"\x00" * (1 << 20)
                left = nbytes
                while left:
                    chunk = zero[: min(left, len(zero))]
                    h.update(chunk)
                    left -= len(chunk)
            elif ct == CRC:
                f.read(4)
            else:
                sys.exit("unknown chunk type 0x%04x" % ct)
            total += nbytes

    digest = h.hexdigest()
    print("size   : %d bytes (%.3f GiB)" % (total, total / 2**30))
    print("blocks : %d @ %d" % (total_blks, blk))
    print("chunks : total=%d raw=%d fill=%d dont_care=%d crc=%d"
          % (total_chunks, counts[RAW], counts[FILL], counts[DC], counts[CRC]))
    print("sha256 : %s" % digest)
    if expected:
        print("expect : %s" % expected)
        print("result : %s" % ("MATCH" if digest == expected else "MISMATCH"))
        sys.exit(0 if digest == expected else 2)


if __name__ == "__main__":
    main()
