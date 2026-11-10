#!/usr/bin/env python3
"""Patch a property inside the FDT(s) of an Android DTBO table or a plain FDT.

Used for manaus bring-up: move the reserved pstore region so it survives a
fastboot visit (fastboot clobbers 0x48080000-0x4817ffff, where LK puts the
ramoops zones).

  python3 tools/fdtpatch.py IN OUT NODE PROP NEWVALUE

NODE is matched by its last path component (e.g. mblock-11-pstore). PROP is
matched by name (e.g. reg). NEWVALUE is a big-endian hex string that must be
exactly as long as the existing property value, so sizes never change:

  python3 tools/fdtpatch.py stock-dtbo new-dtbo mblock-11-pstore reg 000000014000000000000000000e0000

Prints the old and new bytes for every match.
"""
import struct
import sys

DTBO_MAGIC = 0xD7B7AB1E
FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def walk_and_patch(b, base, node_name, prop_name, newval):
    """Patch every matching property in the FDT at `base`. Returns match count."""
    assert be32(b, base) == FDT_MAGIC, "not an FDT"
    off_struct = base + be32(b, base + 8)
    off_strings = base + be32(b, base + 12)
    p = off_struct
    path = []
    matches = 0

    def string_at(idx):
        end = b.index(b"\0", off_strings + idx)
        return b[off_strings + idx:end].decode("ascii", "replace")

    while True:
        tok = be32(b, p)
        p += 4
        if tok == FDT_BEGIN_NODE:
            end = b.index(b"\0", p)
            name = b[p:end].decode("ascii", "replace")
            p = (end + 4) & ~3
            path.append(name)
        elif tok == FDT_END_NODE:
            path.pop()
        elif tok == FDT_PROP:
            length = be32(b, p)
            nameoff = be32(b, p + 4)
            p += 8
            pname = string_at(nameoff)
            if path and path[-1] == node_name and pname == prop_name:
                old = bytes(b[p:p + length])
                new = bytes.fromhex(newval)
                if len(new) != length:
                    raise SystemExit(
                        "%s/%s is %d bytes, new value is %d"
                        % (node_name, prop_name, length, len(new)))
                print("  %s/%s @0x%x: %s -> %s"
                      % ("/".join(path), pname, p, old.hex(), new.hex()))
                b[p:p + length] = new
                matches += 1
            p = (p + length + 3) & ~3
        elif tok == FDT_NOP:
            pass
        elif tok == FDT_END:
            break
        else:
            raise SystemExit("bad FDT token 0x%x at 0x%x" % (tok, p - 4))
    return matches


def main(argv):
    if len(argv) != 6:
        raise SystemExit(__doc__)
    src, dst, node, prop, newval = argv[1:]
    b = bytearray(open(src, "rb").read())
    total = 0

    if be32(b, 0) == DTBO_MAGIC:
        entry_size = be32(b, 12)
        entry_count = be32(b, 16)
        entries_offset = be32(b, 20)
        for i in range(entry_count):
            off = entries_offset + i * entry_size
            size = be32(b, off)
            doff = be32(b, off + 4)
            if be32(b, doff) != FDT_MAGIC:
                print("[%d] not an FDT, skipped" % i)
                continue
            print("[%d] fdt @0x%x size %d" % (i, doff, size))
            total += walk_and_patch(b, doff, node, prop, newval)
    elif be32(b, 0) == FDT_MAGIC:
        print("fdt size %d" % be32(b, 4))
        total = walk_and_patch(b, 0, node, prop, newval)
    else:
        raise SystemExit("neither DTBO (0x%08x) nor FDT (0x%08x)"
                         % (DTBO_MAGIC, FDT_MAGIC))

    if not total:
        raise SystemExit("no %s/%s property found" % (node, prop))
    open(dst, "wb").write(b)
    print("patched %d propert%s -> %s" % (total, "y" if total == 1 else "ies", dst))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
