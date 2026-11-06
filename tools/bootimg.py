#!/usr/bin/env python3
"""Inspect and unpack Android boot / vendor_boot images, header v0-v4.

Pure Python, no Android SDK needed. Use it to (a) verify the stock boot.img
backup before flashing anything and (b) check that tools/mkbootimg-manaus.sh
produced what the LK will actually read.

  python3 tools/bootimg.py info  boot.img
  python3 tools/bootimg.py unpack boot.img outdir
  python3 tools/bootimg.py info  vendor_boot.img

Header layouts: system/tools/mkbootimg/include/bootimg/bootimg.h
"""
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
VENDOR_MAGIC = b"VNDRBOOT"
V3_PAGESIZE = 4096


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def u64(b, off):
    return struct.unpack_from("<Q", b, off)[0]


def cstr(b, off, size):
    return b[off:off + size].split(b"\0")[0].decode("utf-8", "replace")


def pad(n, page):
    return (n + page - 1) // page * page


def parse_boot(b):
    if b[:8] != BOOT_MAGIC:
        raise SystemExit("not a boot image: magic is %r, expected ANDROID!" % b[:8])
    ver = u32(b, 40)
    h = {"kind": "boot", "header_version": ver}
    if ver in (0, 1, 2):
        page = u32(b, 36)
        h.update(
            page_size=page,
            kernel_size=u32(b, 8), kernel_addr=hex(u32(b, 12)),
            ramdisk_size=u32(b, 16), ramdisk_addr=hex(u32(b, 20)),
            second_size=u32(b, 24),
            tags_addr=hex(u32(b, 32)),
            os_version=hex(u32(b, 44)),
            name=cstr(b, 48, 16),
            cmdline=(cstr(b, 64, 512) + cstr(b, 608, 1024)).strip(),
        )
        h["parts"] = [("kernel", u32(b, 8)), ("ramdisk", u32(b, 16)), ("second", u32(b, 24))]
        if ver >= 1:
            h.update(recovery_dtbo_size=u32(b, 1632), header_size=u32(b, 1644))
            h["parts"].append(("recovery_dtbo", u32(b, 1632)))
        if ver == 2:
            h.update(dtb_size=u32(b, 1648), dtb_addr=hex(u64(b, 1652)))
            h["parts"].append(("dtb", u32(b, 1648)))
    elif ver in (3, 4):
        page = V3_PAGESIZE
        # v3/v4 have no load addresses in the header: the bootloader uses
        # fixed physical addresses from its own config.
        h.update(
            page_size=page,
            kernel_size=u32(b, 8),
            ramdisk_size=u32(b, 12),
            os_version=hex(u32(b, 16)),
            header_size=u32(b, 20),
            cmdline=cstr(b, 44, 1536).strip(),
        )
        if ver == 4:
            h["signature_size"] = u32(b, 1580)
        h["parts"] = [("kernel", u32(b, 8)), ("ramdisk", u32(b, 12))]
    else:
        raise SystemExit("unknown boot header_version %d" % ver)
    # offsets, in file order
    off = page
    h["offsets"] = {}
    for name, size in h["parts"]:
        h["offsets"][name] = off
        off += pad(size, page)
    return h


def parse_vendor(b):
    if b[:8] != VENDOR_MAGIC:
        raise SystemExit("not a vendor_boot image: magic is %r" % b[:8])
    ver = u32(b, 8)
    page = u32(b, 12)
    h = {
        "kind": "vendor_boot", "header_version": ver, "page_size": page,
        "kernel_addr": hex(u32(b, 16)), "ramdisk_addr": hex(u32(b, 20)),
        "vendor_ramdisk_size": u32(b, 24),
        "cmdline": cstr(b, 28, 2048).strip(),
        "tags_addr": hex(u32(b, 2076)), "name": cstr(b, 2080, 16),
        "header_size": u32(b, 2096),
        "dtb_size": u32(b, 2100), "dtb_addr": hex(u64(b, 2104)),
    }
    if ver >= 4:
        h.update(
            ramdisk_table_size=u32(b, 2112),
            ramdisk_table_entries=u32(b, 2116),
            ramdisk_table_entry_size=u32(b, 2120),
            bootconfig_size=u32(b, 2124),
        )
    h["parts"] = [("vendor_ramdisk", u32(b, 24)), ("dtb", u32(b, 2100))]
    off = pad(h["header_size"], page)
    h["offsets"] = {}
    for name, size in h["parts"]:
        h["offsets"][name] = off
        off += pad(size, page)
    h["extra_sizes"] = {}
    if ver >= 4:
        h["offsets"]["ramdisk_table"] = off
        h["extra_sizes"]["ramdisk_table"] = h["ramdisk_table_size"]
        off += pad(h["ramdisk_table_size"], page)
        h["offsets"]["bootconfig"] = off
        h["extra_sizes"]["bootconfig"] = h["bootconfig_size"]
    return h


def main(argv):
    if len(argv) < 3 or argv[1] not in ("info", "unpack"):
        raise SystemExit(__doc__)
    cmd, src = argv[1], argv[2]
    b = open(src, "rb").read()
    h = parse_boot(b) if b[:8] == BOOT_MAGIC else parse_vendor(b)

    sizes = {n: s for n, s in h["parts"]}
    sizes.update(h.get("extra_sizes", {}))
    extents = [(n, h["offsets"][n], sizes.get(n, 0)) for n in h["offsets"]]
    parts_end = max(off + size for _, off, size in extents)

    if cmd == "info":
        for k, v in h.items():
            if k not in ("parts", "extra_sizes"):
                print("%-22s %s" % (k, v))
        print("%-22s %s" % ("file_size", len(b)))
        print("%-22s %s" % ("parts_end", parts_end))
        if len(b) > parts_end:
            print("%-22s %d  (MTK header, AVB footer or padding)"
                  % ("trailing_bytes", len(b) - parts_end))
        return 0

    out = argv[3] if len(argv) > 3 else "bootimg-out"
    os.makedirs(out, exist_ok=True)
    for name, off, size in extents:
        if not size:
            continue
        data = b[off:off + size]
        if len(data) != size:
            print("WARNING: %s truncated (%d of %d bytes)" % (name, len(data), size))
        open(os.path.join(out, name), "wb").write(data)
        print("%-16s %8d bytes  @0x%08x" % (name, size, off))
    with open(os.path.join(out, "header.txt"), "w", encoding="utf-8") as f:
        for k, v in h.items():
            if k not in ("parts", "extra_sizes"):
                f.write("%s: %s\n" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
