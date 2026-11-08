#!/usr/bin/env python3
"""Build the Apple Device Tree for manaus (Motorola Edge 40 Neo, MT6879).

Same shape as tools/mkadt.py (the sweet builder), but every hardware number
comes from hw/manaus.hw.json instead of being hardcoded. That file is filled
from the live device tree; see docs/manaus/01-DELTA-HARDWARE.md.

Until the values exist this script only runs with --qemu (fixed GIC/timer) and a
complete config, which is how the build gets validated without the phone.

Usage:
    mkadt-manaus.py OUT.adt [--hw FILE] [--qemu] [--dump] [--check]

--check prints the missing hardware facts and exits 1 without writing anything.
"""
import hashlib, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_HW = os.path.join(ROOT, 'hw', 'manaus.hw.json')
D22AP_ADT = os.path.join(ROOT, 'research', 'ipsw-x', 'd22ap.adt')

# QEMU virt (-M virt,gic-version=3). Same values as qemu-sweet.py; the RAM base
# and size still come from the config so the test payload matches the real one.
QEMU_GICD = 0x08000000
QEMU_GICR = 0x080a0000
QEMU_GICR_SIZE = 0x20000
QEMU_TIMEBASE = 62500000
QEMU_VTIMER_INTID = 27

MANAUS_ECID = 0x4d414e4155530001  # 'MANAUS' + 1, synthetic (never the phone's)
BOOT_MANIFEST_HASH = hashlib.sha384(b'manaus boot manifest').digest()
CHIP_ID = 0x8015  # A11, what the iPhone X userland expects

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def num(v):
    if isinstance(v, str):
        return int(v, 0)
    return v


def get(hw, path):
    cur = hw
    for part in path.split('.'):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


# Paths the real-machine build cannot start without. QEMU covers gic/timer/cpus.
REQUIRED = [
    'soc_base',
    'root_compatible',
    'gic.gicd', 'gic.gicr', 'gic.gicr_size', 'gic.virtual_timer_intid',
    'timer.timebase_hz',
    'ram.base', 'ram.size',
    'fb.base', 'fb.size', 'fb.stride',
    'ufs.base', 'ufs.size',
]


def missing(hw, qemu):
    out = []
    for path in REQUIRED:
        if qemu and path.split('.')[0] in ('gic', 'timer', 'ufs'):
            continue
        if get(hw, path) is None:
            out.append(path)
    if not qemu:
        for i, cpu in enumerate(hw.get('cpus') or []):
            if cpu.get('mpidr') is None:
                out.append(f'cpus[{i}].mpidr')
    return out


def d22ap_product(path=D22AP_ADT):
    """`/product` of the real iPhone X tree; MobileGestalt reads it."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def build(hw, qemu=False):
    soc_base = num(hw['soc_base'])
    phys_base = num(get(hw, 'ram.base'))
    mem_size = num(get(hw, 'ram.size'))
    panic_size = num(get(hw, 'ram.panic_log_size') or 0x100000)
    fb_base = num(get(hw, 'fb.base'))
    fb_size = num(get(hw, 'fb.size'))
    fb_w = int(get(hw, 'fb.width') or 1080)
    fb_h = int(get(hw, 'fb.height') or 2400)
    fb_stride = num(get(hw, 'fb.stride') or fb_w * 4)

    if qemu:
        gicd, gicr, gicr_size = QEMU_GICD, QEMU_GICR, QEMU_GICR_SIZE
        timebase, vtimer = QEMU_TIMEBASE, QEMU_VTIMER_INTID
    else:
        gicd, gicr = num(get(hw, 'gic.gicd')), num(get(hw, 'gic.gicr'))
        gicr_size = num(get(hw, 'gic.gicr_size'))
        timebase = num(get(hw, 'timer.timebase_hz'))
        vtimer = num(get(hw, 'gic.virtual_timer_intid'))

    # Carve the panic log off the top; XNU maps /pram but never allocates there.
    panic_base = phys_base + mem_size - panic_size
    mem_size -= panic_size

    seed = os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(panic_size)),
        ('firmware-version', cstr('manaus-shim')),
        ('system-firmware-version', cstr('manaus-shim')),
        # libimg4/mount -P 1 need it; iBoot normally supplies a SHA-384.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        # 1080x2400 @3x = 360x800 pt, iPhone X class. Must match the display node.
        ('display-scale', u32(int(get(hw, 'display.scale') or 3))),
        # Synthetic ECID + A11 chip-id: the userland is the iPhone X one.
        ('unique-chip-id', struct.pack('<Q', MANAUS_ECID)),
        ('chip-id', u32(CHIP_ID)),
        # Dev-fused, undemoted board: takes mobileactivationd through the native
        # "Hactivation" path (no Apple servers, no SEP, no baseband).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # lp-sip0 = csrutil disable; lp-sip1 = unsealed root. The shared cache is
        # in no trust cache, so dyld needs unrestricted FS to map it.
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # CPU nodes. `reg` is the real MPIDR (Aff1:Aff0), not a logical index: on
    # MT6879 the A78 cluster and the A55 cluster have different Aff1 values, so
    # read them from the FDT instead of assuming 0..7.
    cpu_nodes = []
    cpus_cfg = hw.get('cpus') or []
    if qemu:
        cpus_cfg = [{'mpidr': 0, 'clock_hz': None}]
    for core, cpu in enumerate(cpus_cfg):
        mpidr = 0 if qemu else num(cpu['mpidr'])
        clock = num(cpu.get('clock_hz')) if cpu.get('clock_hz') else timebase
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', b''.join(cstr(c) for c in (hw.get('cpu_compatible') or ['ARM,v8']))),
            ('reg', u32(mpidr)),
            ('cpu-id', u32(core)),
            # One homogeneous SMP cluster, like sweet: ml_parse_cpu_topology
            # uses cluster-type 0 (SMP), and patch 0078 already accepts the
            # A55 (0x000-0x500) and A76 (0x600/0x700) MPIDRs as one set.
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(clock)),
            ('bus-frequency', u32(timebase)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer)),
        ('reg', u64(gicd - soc_base, num(get(hw, 'gic.gicd_size') or 0x10000),
                    gicr - soc_base, gicr_size)),
    ])

    # Keep the node/property names the shared sweet patches look for
    # ("sweet-io", "sweet,display", "sweet-fbcon", "sweet-ufs"): only the values
    # change. Rewriting those names means rewriting every patch that matches
    # them, which is the whole point of the override-by-number fork.
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, soc_base, 0x100000000 - soc_base)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(int(get(hw, 'display.canvas_width') or 1125))),
            ('canvas-height', u32(int(get(hw, 'display.canvas_height') or 2436))),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb_base, fb_size)),
        ('width', u32(fb_w)),
        ('height', u32(fb_h)),
        ('stride', u32(fb_stride)),
    ])

    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr(get(hw, 'ufs.compatible') or 'mediatek,ufshc')),
        ('reg', u64(num(get(hw, 'ufs.base')), num(get(hw, 'ufs.size')))),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(panic_size))])

    return node('device-tree', [
        ph(),
        ('compatible', b''.join(cstr(c) for c in (hw.get('root_compatible') or ['D22AP']))),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [ufs]))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    flags = [a for a in argv[1:] if a.startswith('--')]
    hw_path = DEFAULT_HW
    if '--hw' in flags:
        hw_path = argv[argv.index('--hw') + 1]
        args = [a for a in args if a != hw_path]
    qemu = '--qemu' in flags
    hw = json.load(open(hw_path))

    gaps = missing(hw, qemu)
    if '--check' in flags:
        if gaps:
            print(f'{hw_path}: {len(gaps)} hardware facts still unknown:', file=sys.stderr)
            for g in gaps:
                print(f'  - {g}', file=sys.stderr)
            return 1
        print(f'{hw_path}: complete for {"qemu" if qemu else "hardware"}', file=sys.stderr)
        return 0

    if gaps:
        print('error: incomplete config; run --check to list what is missing', file=sys.stderr)
        for g in gaps:
            print(f'  - {g}', file=sys.stderr)
        return 1

    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    out = args[0]
    blob = adt.serialize(build(hw, qemu=qemu))
    open(out, 'wb').write(blob)
    if '--dump' in flags:
        adt.dump(adt.parse(blob)[0])
    print(f'{out}: {len(blob)} bytes', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
