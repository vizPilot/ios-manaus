#!/usr/bin/env python3
"""Read or set the SWEETXNU boot cmdline embedded in a shim boot image.

The manaus shim boot images (tools/mkbootimg-manaus.sh, --mode v4) carry the
raw SWEETXNU payload twice: as the boot.img ramdisk and as the vendor_boot.img
vendor ramdisk. The payload's cmdline lives at a fixed offset (payload+0x48,
608 bytes, NUL padded), so a boot-arg can be changed in place without rebuilding
the payload or the image -- as long as the new string fits and the image keeps
its size (the ramdisk_size in the header must not change).

This is the low-risk way to test a cmdline-only change (e.g. cpus=1 -> cpus=6)
starting from an image that is already known to boot: exactly the intended bytes
change, nothing else.

Usage:
    bootimg-cmdline.py show  IMG [IMG...]
    bootimg-cmdline.py set   IMG --cmdline "full new cmdline"
    bootimg-cmdline.py sub   IMG OLD NEW        # same-length token replace
    bootimg-cmdline.py set   IMG --cpus 6       # convenience: rewrite cpus=N
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys

MAGIC = b'SWEETXNU'
CMDLINE = 0x48
BOOT_LINE_LENGTH = 608


def find_payload(buf):
    off = buf.find(MAGIC)
    if off < 0:
        raise SystemExit('no SWEETXNU payload in this image')
    if buf.find(MAGIC, off + 1) >= 0:
        raise SystemExit('more than one SWEETXNU magic; refusing to guess')
    return off


def get_cmdline(buf, poff):
    raw = buf[poff + CMDLINE:poff + CMDLINE + BOOT_LINE_LENGTH]
    return raw.split(b'\0', 1)[0].decode('latin1')


def put_cmdline(buf, poff, cl):
    enc = cl.encode('latin1')
    if len(enc) > BOOT_LINE_LENGTH - 1:
        raise SystemExit('cmdline too long: %d > %d' % (len(enc), BOOT_LINE_LENGTH - 1))
    buf[poff + CMDLINE:poff + CMDLINE + BOOT_LINE_LENGTH] = enc + b'\0' * (BOOT_LINE_LENGTH - len(enc))


def rewrite_cpus(cl, n):
    toks = cl.split()
    out, hit = [], 0
    for t in toks:
        if t.startswith('cpus='):
            out.append('cpus=%d' % n)
            hit += 1
        else:
            out.append(t)
    if hit != 1:
        raise SystemExit('expected exactly one cpus= token, found %d' % hit)
    return ' '.join(out)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('show')
    p.add_argument('images', nargs='+')

    p = sub.add_parser('set')
    p.add_argument('image')
    p.add_argument('--cmdline')
    p.add_argument('--cpus', type=int)
    p.add_argument('-o', '--out')

    p = sub.add_parser('sub')
    p.add_argument('image')
    p.add_argument('old')
    p.add_argument('new')
    p.add_argument('-o', '--out')

    a = ap.parse_args()

    if a.cmd == 'show':
        for path in a.images:
            buf = open(path, 'rb').read()
            poff = find_payload(buf)
            print('%-40s payload@%d  %s' % (path, poff, get_cmdline(buf, poff)))
        return 0

    buf = bytearray(open(a.image, 'rb').read())
    poff = find_payload(buf)
    old = get_cmdline(buf, poff)

    if a.cmd == 'set':
        if (a.cmdline is None) == (a.cpus is None):
            raise SystemExit('give exactly one of --cmdline / --cpus')
        new = a.cmdline if a.cmdline is not None else rewrite_cpus(old, a.cpus)
    else:  # sub
        if a.old not in old:
            raise SystemExit('token %r not in cmdline' % a.old)
        new = old.replace(a.old, a.new)
        if len(new) != len(old):
            raise SystemExit('sub is same-length only: %r -> %r' % (a.old, a.new))

    put_cmdline(buf, poff, new)
    out = a.out or a.image
    if os.path.abspath(out) != os.path.abspath(a.image):
        shutil.copyfile(a.image, out)
        buf2 = bytearray(open(out, 'rb').read())
        put_cmdline(buf2, find_payload(buf2), new)
        data = bytes(buf2)
    else:
        data = bytes(buf)

    # Size must be identical: a boot image's ramdisk_size is in its header.
    orig = open(a.image, 'rb').read()
    if len(data) != len(orig):
        raise SystemExit('size changed: %d -> %d' % (len(orig), len(data)))
    open(out, 'wb').write(data)

    print('image   : %s' % a.image)
    print('out     : %s' % out)
    print('old     : %s' % old)
    print('new     : %s' % new)
    print('size    : %d (unchanged)' % len(data))
    print('sha256  : %s' % hashlib.sha256(data).hexdigest())
    return 0


if __name__ == '__main__':
    sys.exit(main())
