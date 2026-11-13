#!/usr/bin/env python3
"""Writes the device tree make test hands QEMU's raspi3b machine, the
stand-in for a Raspberry Pi (boards/rpi4): a flattened device tree
(DTB, version 17) with only what the firmware uses. Original code,
written from the Devicetree Specification's description of the format
and the BCM2835/BCM2837 address map; no dtc needed. On a real Pi the
firmware (start*.elf) passes its own, much fuller tree. QEMU fills in
/memory and /chosen.

    tests/mkrpidtb.py out.dtb
"""
import struct
import sys

def u32(*v):
    return b"".join(struct.pack(">I", x) for x in v)

def s(text):
    return text.encode() + b"\0"

def strs(*texts):
    return b"".join(s(t) for t in texts)

UART = 1    # phandle of the PL011, for /aliases

def cpu(n):
    return ("cpu@%d" % n, [
        ("device_type", s("cpu")),
        ("compatible", s("arm,cortex-a53")),
        ("reg", u32(n)),
        ("enable-method", s("spin-table")),
        ("cpu-release-addr", u32(0, 0xD8 + 8 * n)),
    ], [])

TREE = ("", [
    ("compatible", strs("raspberrypi,3-model-b", "brcm,bcm2837")),
    ("model", s("QEMU raspi3b (SquirrelBIOS test tree)")),
    ("#address-cells", u32(1)),
    ("#size-cells", u32(1)),
], [
    ("chosen", [("stdout-path", s("serial0:115200n8"))], []),
    ("aliases", [("serial0", s("/soc/serial@7e201000"))], []),
    ("memory@0", [("device_type", s("memory")), ("reg", u32(0, 0x3C000000))], []),
    ("cpus", [("#address-cells", u32(1)), ("#size-cells", u32(0))], [cpu(n) for n in range(4)]),
    ("soc", [
        ("compatible", s("simple-bus")),
        ("#address-cells", u32(1)),
        ("#size-cells", u32(1)),
        # the peripherals' bus addresses (0x7E...) as the ARM sees them
        ("ranges", u32(0x7E000000, 0x3F000000, 0x01000000)),
    ], [
        ("watchdog@7e100000", [
            ("compatible", strs("brcm,bcm2835-pm", "brcm,bcm2835-pm-wdt")),
            ("reg", u32(0x7E100000, 0x114)),
        ], []),
        ("serial@7e201000", [
            ("compatible", strs("arm,pl011", "arm,primecell")),
            ("reg", u32(0x7E201000, 0x200)),
            ("phandle", u32(UART)),
        ], []),
        ("mmc@7e300000", [
            ("compatible", s("brcm,bcm2835-sdhci")),
            ("reg", u32(0x7E300000, 0x100)),
        ], []),
    ]),
])

def build(tree):
    structure = bytearray()
    strings = bytearray()
    offsets = {}

    def name_off(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(s(name))
        return offsets[name]

    def node(n):
        name, props, children = n
        structure.extend(u32(1))                        # FDT_BEGIN_NODE
        raw = s(name)
        structure.extend(raw + b"\0" * (-len(raw) % 4))
        for key, value in props:
            structure.extend(u32(3, len(value), name_off(key)))     # FDT_PROP
            structure.extend(value + b"\0" * (-len(value) % 4))
        for c in children:
            node(c)
        structure.extend(u32(2))                        # FDT_END_NODE

    node(tree)
    structure.extend(u32(9))                            # FDT_END
    header_len = 40
    rsv = u32(0, 0, 0, 0)                               # empty reserve map
    off_rsv = header_len
    off_struct = off_rsv + len(rsv)
    off_strings = off_struct + len(structure)
    total = off_strings + len(strings)
    header = u32(0xD00DFEED, total, off_struct, off_strings, off_rsv,
                 17, 16, 0, len(strings), len(structure))
    return header + rsv + bytes(structure) + bytes(strings)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "wb") as f:
        f.write(build(TREE))
