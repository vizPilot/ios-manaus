#!/usr/bin/env python3
"""Report the logical FILL runs of a sparse image (headers only, no payload reads).

libsparse's import merges *adjacent* FILL chunks that carry the same 4-byte
value, so a long run of zero blocks split by img2simg into many 64 MiB FILL
chunks reappears as one multi-GiB chunk on the wire -- the exact thing the MTK
32-bit writer cannot handle.  This prints the largest merged run, which is what
`diag4` reports as `max_block_len` (before resparse fragmenting).

  simg-chunkruns.py IMG.simg [--block 4096]
"""
import struct
import sys

MAGIC = 0xED26FF3A
RAW, FILL, DC, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
NAMES = {RAW: "RAW", FILL: "FILL", DC: "DC", CRC: "CRC"}


def main():
    path = sys.argv[1]
    blk = 4096
    if "--block" in sys.argv:
        blk = int(sys.argv[sys.argv.index("--block") + 1], 0)

    with open(path, "rb") as f:
        magic, major, minor, fhs, chs, fblk, total_blks, total_chunks, ck = \
            struct.unpack("<IHHHHIIII", f.read(28))
        if magic != MAGIC:
            sys.exit("not an Android sparse image (magic 0x%08x)" % magic)
        blk = fblk or blk
        if fhs > 28:
            f.read(fhs - 28)

        max_raw = max_fill = 0
        run_val, run_blocks, run_chunks = None, 0, 0
        max_run = max_run_chunks = 0
        max_run_val = None
        n_fill_runs = 0
        for _ in range(total_chunks):
            ct, _r, cs, ts = struct.unpack("<HHII", f.read(12))
            if chs > 12:
                f.read(chs - 12)
            if ct == RAW:
                max_raw = max(max_raw, cs * blk)
                f.seek(cs * blk, 1)
                if run_blocks and run_blocks > max_run:
                    max_run, max_run_chunks, max_run_val = run_blocks, run_chunks, run_val
                run_val, run_blocks, run_chunks = None, 0, 0
            elif ct == FILL:
                val = f.read(4)
                max_fill = max(max_fill, cs * blk)
                if run_val == val and run_val is not None:
                    run_blocks += cs
                    run_chunks += 1
                else:
                    if run_blocks and run_blocks > max_run:
                        max_run, max_run_chunks, max_run_val = run_blocks, run_chunks, run_val
                    run_val, run_blocks, run_chunks = val, cs, 1
                    n_fill_runs += 1
            elif ct == DC:
                f.seek(0, 1)
            elif ct == CRC:
                f.read(4)
            else:
                sys.exit("unknown chunk type 0x%04x" % ct)
        if run_blocks and run_blocks > max_run:
            max_run, max_run_chunks, max_run_val = run_blocks, run_chunks, run_val

    print("file       : %s" % path)
    print("block size : %d" % blk)
    print("chunks     : %d, blocks %d (%.3f GiB)"
          % (total_chunks, total_blks, total_blks * blk / 2**30))
    print("largest single chunk : RAW %.1f MiB, FILL %.1f MiB"
          % (max_raw / 2**20, max_fill / 2**20))
    print("largest merged FILL run : %d blocks (%.3f GiB) over %d chunks, val 0x%s"
          % (max_run, max_run * blk / 2**30, max_run_chunks,
             max_run_val.hex() if max_run_val else "--"))
    print("merged FILL runs : %d" % n_fill_runs)
    print("over 4 GiB merged run : %s"
          % ("YES (MTK writer will overflow)" if max_run * blk > 0x100000000
             else "no"))


if __name__ == "__main__":
    main()
