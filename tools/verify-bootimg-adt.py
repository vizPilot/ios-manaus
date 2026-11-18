#!/usr/bin/env python3
"""Verify the ADT embedded in a built shim boot image.

The shim boot images (tools/mkbootimg-manaus.sh) carry the raw SWEETXNU payload
as their ramdisk (boot.img) and vendor ramdisk (vendor_boot.img). This scans the
image for the payload, extracts the embedded ADT and checks:

  * the payload magic / version / segment count,
  * the embedded ADT is byte-identical to a reference ADT (--adt),
  * the ADT carries a /product node with the artwork traits MobileGestalt needs
    (missing /product => SpringBoard aborts on the bounding-path asset),
  * the boot cmdline (optionally compared to an expected one).

Usage:
    verify-bootimg-adt.py IMG [IMG...] [--adt REF.adt] [--cmdline "expected"]
"""
import argparse
import hashlib
import os
import struct
import sys

TRAITS = [b'product-description', b'artwork-device-idiom',
          b'artwork-device-subtype', b'artwork-scale-factor',
          b'artwork-dynamic-displaymode', b'device-perf-memory-class',
          b'graphics-featureset-class', b'graphics-featureset-fallbacks',
          b'compatible-device-fallback', b'artwork-display-gamut']


def find_payload(buf):
    off = buf.find(b'SWEETXNU')
    return off if off >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('images', nargs='+')
    ap.add_argument('--adt', help='reference ADT to compare against')
    ap.add_argument('--cmdline', help='expected cmdline')
    args = ap.parse_args()

    ref = open(args.adt, 'rb').read() if args.adt else None
    refsha = hashlib.sha256(ref).hexdigest() if ref else None
    if ref is not None:
        print('reference ADT: %d bytes sha256 %s' % (len(ref), refsha))

    rc = 0
    for path in args.images:
        print('=' * 72)
        print(path)
        buf = open(path, 'rb').read()
        poff = find_payload(buf)
        if poff is None:
            print('  !! no SWEETXNU payload found')
            rc = 1
            continue
        print('  image %d bytes, payload @ %d' % (len(buf), poff))
        magic, ver, nsegs = struct.unpack_from('<8sII', buf, poff)
        adt_off, adt_size = struct.unpack_from('<QQ', buf, poff + 0x20)
        print('  magic=%r ver=%d nsegs=%d adt_off=%d adt_size=%d'
              % (magic, ver, nsegs, adt_off, adt_size))
        blob = buf[poff + adt_off: poff + adt_off + adt_size]
        sha = hashlib.sha256(blob).hexdigest()
        print('  embedded ADT sha256 %s' % sha)
        if ref is not None:
            ok = sha == refsha
            print('  matches reference: %s' % ('YES' if ok else 'NO'))
            rc |= 0 if ok else 1
        missing = [t.decode() for t in TRAITS if t not in blob]
        if b'product\x00' not in blob:
            print('  !! NO /product NODE -> SpringBoard will abort')
            rc = 1
        elif missing:
            print('  !! /product present but missing traits: %s' % ', '.join(missing))
            rc = 1
        else:
            print('  /product node present with all artwork traits')
        cl = buf[poff + 0x48: poff + 0x48 + 608].split(b'\x00')[0]
        print('  cmdline (%d): %s' % (len(cl), cl.decode()))
        if args.cmdline is not None and cl.decode() != args.cmdline:
            print('  !! cmdline differs from expected')
            rc = 1
    sys.exit(rc)


if __name__ == '__main__':
    main()
