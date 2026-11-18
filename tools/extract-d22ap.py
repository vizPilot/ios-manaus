#!/usr/bin/env python3
"""Extract the real iPhone X (D22AP) device tree from an IPSW, offline.

The device tree builder (tools/mkadt.py, tools/mkadt-manaus.py) copies the
`/product` node from research/ipsw-x/d22ap.adt. MobileGestalt reads the artwork
traits (artwork-device-idiom, artwork-scale-factor, artwork-device-subtype,
device-perf-memory-class, graphics-featureset-class, ...) from that node; with
no `/product` node they are empty and SpringBoard aborts on
`Failed to load bounding path bitmap data ... BoundingPathBitmap-...`.

README documents the canonical recipe:

    python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
    ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p \
        -o research/ipsw-x/d22ap.adt

This script is the offline equivalent when the IPSW is already on disk and you
do not have `ipsw`/`remotezip` (e.g. on Windows). It unwraps the IM4P, LZFSE-
decompresses the `bvx2` payload and writes the raw ADT.

    extract-d22ap.py IPSE_OR_IM4P OUT.adt
    extract-d22ap.py --from-ipsw IPSE OUT.adt        # pulls the im4p via 7-Zip

`lzfse` may be the Python module (pip install lzfse) or the WSL CLI
(apt install lzfse, /usr/bin/lzfse).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys


def der(buf, off):
    tag = buf[off]
    ln = buf[off + 1]
    off += 2
    if ln & 0x80:
        n = ln & 0x7f
        ln = int.from_bytes(buf[off:off + n], 'big')
        off += n
    return tag, off, off + ln


def im4p_payload(buf):
    tag, s, e = der(buf, 0)
    assert tag == 0x30, 'not a DER sequence'
    items, off = [], s
    while off < e:
        t, vs, ve = der(buf, off)
        items.append((t, buf[vs:ve]))
        off = ve
    assert items[0][1] == b'IM4P', items[0]
    return items[3][1]


def decompress(payload):
    if payload[:4] != b'bvx2':
        return payload
    try:
        import lzfse  # type: ignore
        return lzfse.decompress(payload)
    except ImportError:
        pass
    try:
        return subprocess.run(['lzfse', '-decode'], input=payload,
                              stdout=subprocess.PIPE, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit('need LZFSE: pip install lzfse, or apt install lzfse '
                         '(%s)' % exc)


def extract_im4p_from_ipsw(ipsw, out_dir):
    """Pull Firmware/all_flash/DeviceTree.d22ap.im4p out of the IPSW with 7-Zip."""
    entry = 'Firmware\\all_flash\\DeviceTree.d22ap.im4p'
    os.makedirs(out_dir, exist_ok=True)
    for sevenzip in (r'C:\Program Files\7-Zip\7z.exe', '7z'):
        try:
            subprocess.run([sevenzip, 'e', '-y', '-o' + out_dir, ipsw, entry],
                           check=True, stdout=subprocess.DEVNULL)
            return os.path.join(out_dir, 'DeviceTree.d22ap.im4p')
        except (OSError, subprocess.CalledProcessError):
            continue
    raise SystemExit('could not extract %s from %s (need 7-Zip)' % (entry, ipsw))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', help='IPSW, or a DeviceTree.d22ap.im4p')
    ap.add_argument('out', help='output raw ADT')
    ap.add_argument('--from-ipsw', action='store_true',
                    help='treat INPUT as an IPSW and pull the im4p with 7-Zip')
    args = ap.parse_args()

    src = args.input
    if args.from_ipsw:
        src = extract_im4p_from_ipsw(args.input,
                                     os.path.join(os.path.dirname(args.out), 'dt'))
        print('im4p: %s' % src)

    buf = open(src, 'rb').read()
    payload = decompress(im4p_payload(buf))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, 'wb').write(payload)
    print('wrote %s (%d bytes, sha256 %s)'
          % (args.out, len(payload), hashlib.sha256(payload).hexdigest()))


if __name__ == '__main__':
    main()
