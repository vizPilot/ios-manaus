#!/usr/bin/env python3
"""Make an HFS+ image acceptable to the Linux hfsplus driver.

The image's alternate volume header sits at (totalBlocks-1)*blockSize-1024
(== image size - 5120 when the image is one block larger than the volume), but
Linux (fs/hfsplus/wrapper.c: hfsplus_read_wrapper) reads it at part_size-2
sectors == size-1024 and requires its signature to match the primary's.  This
writes a copy of the (journal-bit-cleared) primary volume header at size-1024,
and clears the journaled bit in the header at size-5120 as well, so primary,
Linux's alternate and the on-disk alternate all agree.

  fix-alt-vh.py IMG
"""
import os
import struct
import sys

JOURNALED = 0x2000
HFS_SIGS = (0x482B, 0x4858)  # 'H+', 'HX'


def clear_journal(buf):
    attr = struct.unpack(">I", buf[4:8])[0]
    buf[4:8] = struct.pack(">I", attr & ~JOURNALED)


def show(buf, label):
    sig, ver, attr = struct.unpack(">HHI", buf[:8])
    print("%-26s sig=0x%04x ver=%d attr=0x%08x journaled=%s"
          % (label, sig, ver, attr, bool(attr & JOURNALED)))


def main():
    path = sys.argv[1]
    size = os.path.getsize(path)
    with open(path, "r+b") as f:
        f.seek(1024)
        prim = bytearray(f.read(512))
        if struct.unpack(">H", prim[:2])[0] not in HFS_SIGS:
            sys.exit("primary volume header not found at 1024")
        clear_journal(prim)
        f.seek(1024)
        f.write(prim)

        # Linux's alternate location.
        f.seek(size - 1024)
        f.write(prim)

        # The image's own alternate location: keep it consistent too.
        f.seek(size - 5120)
        alt = bytearray(f.read(512))
        if struct.unpack(">H", alt[:2])[0] in HFS_SIGS:
            clear_journal(alt)
            f.seek(size - 5120)
            f.write(alt)
        else:
            print("note: no volume header at size-5120")

        f.flush()
        f.seek(1024)
        show(bytearray(f.read(512)), "primary @1024")
        f.seek(size - 1024)
        show(bytearray(f.read(512)), "alt @size-1024 (Linux)")
        f.seek(size - 5120)
        show(bytearray(f.read(512)), "alt @size-5120 (on-disk)")


if __name__ == "__main__":
    main()
