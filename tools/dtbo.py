#!/usr/bin/env python3
"""List and extract the DTBs inside an Android DTBO image / dt_table.

Some devices (manaus among them) put a DTBO table, not a plain FDT, in
vendor_boot's dtb field. A dt_table is a header plus N entries, each pointing at
an independent flattened device tree (the base DTB or an overlay).

  python3 tools/dtbo.py list    dtbo.img
  python3 tools/dtbo.py extract dtbo.img outdir
  python3 tools/dtbo.py info    00-*.dtb      # single FDT: magic, size, strings

Layout: system/libufdt/include/dt_table.h
"""
import os
import struct
import sys

DTBO_MAGIC = 0xD7B7AB1E
FDT_MAGIC = 0xD00DFEED

# Only the fields that matter and are stable across dt_table v0/v1. v1 grows
# entries to 64 bytes with extra (custom) fields, which we keep raw.
ENTRY_V0 = 32


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def parse_table(b):
    if be32(b, 0) != DTBO_MAGIC:
        raise SystemExit("not a dt_table: magic 0x%08x" % be32(b, 0))
    h = {
        "total_size": be32(b, 4),
        "header_size": be32(b, 8),
        "entry_size": be32(b, 12),
        "entry_count": be32(b, 16),
        "entries_offset": be32(b, 20),
        "page_size": be32(b, 24),
        "version": be32(b, 28),
    }
    if h["entry_size"] < ENTRY_V0:
        raise SystemExit("unsupported dt_table entry size %d" % h["entry_size"])
    entries = []
    for i in range(h["entry_count"]):
        off = h["entries_offset"] + i * h["entry_size"]
        e = {"index": i, "size": be32(b, off), "offset": be32(b, off + 4),
             "id": be32(b, off + 8), "rev": be32(b, off + 12)}
        if h["entry_size"] >= 64:
            e["custom"] = [be32(b, off + 16 + 4 * j) for j in range(8)]
        entries.append(e)
    return h, entries


def fdt_strings(b, off, size):
    """All strings used by property names (root block), for a quick look."""
    if be32(b, off + 4) != FDT_MAGIC:
        return None
    total = be32(b, off + 8)
    off_struct = be32(b, off + 16)
    off_strings = be32(b, off + 20)
    if total + off > len(b) or off_struct > total or off_strings > total:
        return None
    s = b[off + off_strings:off + total]
    return [x.decode("ascii", "replace") for x in s.split(b"\0") if x]


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    cmd, src = argv[1], argv[2]
    b = open(src, "rb").read()

    if cmd == "info":
        if be32(b, 0) == FDT_MAGIC:
            print("fdt magic 0x%08x size %d" % (FDT_MAGIC, be32(b, 4)))
            return 0
        print("dt_table magic 0x%08x" % DTBO_MAGIC)
        return 0

    h, entries = parse_table(b)
    if cmd == "list":
        print("version        %d" % h["version"])
        print("total_size     %d" % h["total_size"])
        print("header_size    %d" % h["header_size"])
        print("entry_size     %d" % h["entry_size"])
        print("entry_count    %d" % h["entry_count"])
        for e in entries:
            is_fdt = be32(b, e["offset"]) == FDT_MAGIC
            names = fdt_strings(b, e["offset"], e["size"]) if is_fdt else None
            # board/compatible hints: look for the usual suffixes
            hint = ""
            if names:
                interesting = [n for n in names if any(
                    k in n for k in ("mt6879", "mt6855", "manaus", "mediatek",
                                     "arm,gic-v3", "qcom"))]
                hint = "  " + ",".join(dict.fromkeys(interesting))
            print("  [%2d] size %8d offset 0x%08x id 0x%08x rev %d%s"
                  % (e["index"], e["size"], e["offset"], e["id"], e["rev"], hint))
        return 0

    if cmd == "extract":
        out = argv[3] if len(argv) > 3 else "dtbo-out"
        os.makedirs(out, exist_ok=True)
        for e in entries:
            data = b[e["offset"]:e["offset"] + e["size"]]
            if be32(data, 0) != FDT_MAGIC:
                print("[%d] not an FDT, skipped" % e["index"])
                continue
            name = os.path.join(out, "%02d-0x%08x-rev%d.dtb"
                                % (e["index"], e["id"], e["rev"]))
            open(name, "wb").write(data)
            print("%-40s %8d bytes  id 0x%08x rev %d"
                  % (name, len(data), e["id"], e["rev"]))
        return 0

    raise SystemExit("unknown command %r" % cmd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
