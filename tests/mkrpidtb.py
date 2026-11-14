#!/usr/bin/env python3
"""Writes the device tree make test hands QEMU's raspi3b / raspi4b
machines, the stand-in for a Raspberry Pi (boards/rpi4): a flattened
device tree (DTB, version 17) with only what the firmware uses.
Original code, written from the Devicetree Specification's description
of the format and the BCM2835/BCM2837/BCM2711 address maps; no dtc
needed. On a real Pi the firmware (start*.elf) passes its own, much
fuller tree. QEMU may fill in /memory and /chosen.

    tests/mkrpidtb.py out.dtb           # raspi3b (default)
    tests/mkrpidtb.py --rpi4 out.dtb    # raspi4b (BCM2711)
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
GIC = 2     # phandle of the GIC-400 (Pi 4)

def cpu(n, compat):
    return ("cpu@%d" % n, [
        ("device_type", s("cpu")),
        ("compatible", s(compat)),
        ("reg", u32(n)),
        ("enable-method", s("spin-table")),
        ("cpu-release-addr", u32(0, 0xD8 + 8 * n)),
    ], [])

def tree_rpi3():
    return ("", [
        ("compatible", strs("raspberrypi,3-model-b", "brcm,bcm2837")),
        ("model", s("QEMU raspi3b (SquirrelBIOS test tree)")),
        ("#address-cells", u32(1)),
        ("#size-cells", u32(1)),
    ], [
        ("chosen", [("stdout-path", s("serial0:115200n8"))], []),
        ("aliases", [("serial0", s("/soc/serial@7e201000"))], []),
        ("memory@0", [("device_type", s("memory")), ("reg", u32(0, 0x3C000000))], []),
        ("cpus", [("#address-cells", u32(1)), ("#size-cells", u32(0))],
         [cpu(n, "arm,cortex-a53") for n in range(4)]),
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

def tree_rpi4():
    # BCM2711 "low peripheral" map: bus 0x7E... at 0xFE000000, the
    # ARM-local / GIC window (bus 0x40000000) at 0xFF800000. EMMC2 is the
    # SD card host on a real Pi 4; GIC-400 is the interrupt controller.
    return ("", [
        ("compatible", strs("raspberrypi,4-model-b", "brcm,bcm2711")),
        ("model", s("QEMU raspi4b (SquirrelBIOS test tree)")),
        ("#address-cells", u32(1)),
        ("#size-cells", u32(1)),
    ], [
        ("chosen", [("stdout-path", s("serial0:115200n8"))], []),
        ("aliases", [("serial0", s("/soc/serial@7e201000"))], []),
        # Low 960 MB: VideoCore carve-out at the top of the first GB.
        # QEMU raspi4b (2 GB) adds memory@40000000 for the upper GB.
        ("memory@0", [("device_type", s("memory")), ("reg", u32(0, 0x3C000000))], []),
        ("cpus", [("#address-cells", u32(1)), ("#size-cells", u32(0))],
         [cpu(n, "arm,cortex-a72") for n in range(4)]),
        ("soc", [
            ("compatible", s("simple-bus")),
            ("#address-cells", u32(1)),
            ("#size-cells", u32(1)),
            ("ranges", u32(
                0x7E000000, 0xFE000000, 0x01800000,
                0x40000000, 0xFF800000, 0x00800000,
            )),
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
            # GIC-400: distributor at bus 0x40041000 → 0xFF841000, CPU
            # interface at 0x40042000 → 0xFF842000 (BCM2711 / QEMU bcm2838)
            ("interrupt-controller@40041000", [
                ("compatible", s("arm,gic-400")),
                ("reg", u32(
                    0x40041000, 0x1000,
                    0x40042000, 0x2000,
                )),
                ("interrupt-controller", b""),
                ("#interrupt-cells", u32(3)),
                ("phandle", u32(GIC)),
            ], []),
            # EMMC2: the Pi 4 SD card host on real hardware
            # (brcm,bcm2711-emmc2). Base clock 100 MHz when the
            # controller reports none. QEMU raspi4b wires the SD slot
            # to the legacy Arasan host below, so this node is empty
            # under QEMU and the card is on mmc@7e300000.
            ("mmc@7e340000", [
                ("compatible", s("brcm,bcm2711-emmc2")),
                ("reg", u32(0x7E340000, 0x100)),
                ("clock-frequency", u32(100000000)),
                ("status", s("okay")),
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
    args = sys.argv[1:]
    board = "rpi3"
    if args and args[0] in ("--rpi3", "--rpi4"):
        board = args[0][2:]
        args = args[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    tree = tree_rpi4() if board == "rpi4" else tree_rpi3()
    with open(args[0], "wb") as f:
        f.write(build(tree))
