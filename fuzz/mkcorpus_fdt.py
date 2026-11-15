#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/fdt_harness.c.

Original code. Writes synthetic flattened device trees under the given
directory (default: fuzz/corpus-fdt). Layouts follow the Devicetree
Specification v0.4 chapter 5 only. No vendor DTBs.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_END = 9


def be32(v: int) -> bytes:
    return struct.pack(">I", v & 0xFFFFFFFF)


def align4(b: bytes) -> bytes:
    pad = (4 - (len(b) % 4)) % 4
    return b + b"\x00" * pad


def build_dtb(nodes: list[tuple[str, list[tuple[str, bytes]]]]) -> bytes:
    """nodes: list of (name, [(prop_name, prop_data), ...]) flat (no nesting)."""
    strings = bytearray()
    stroff: dict[str, int] = {}

    def stro(name: str) -> int:
        if name not in stroff:
            stroff[name] = len(strings)
            strings.extend(name.encode("ascii") + b"\x00")
        return stroff[name]

    struct_blk = bytearray()
    for name, props in nodes:
        struct_blk.extend(be32(FDT_BEGIN_NODE))
        struct_blk.extend(align4(name.encode("ascii") + b"\x00"))
        for pname, pdata in props:
            struct_blk.extend(be32(FDT_PROP))
            struct_blk.extend(be32(len(pdata)))
            struct_blk.extend(be32(stro(pname)))
            struct_blk.extend(align4(pdata))
        struct_blk.extend(be32(FDT_END_NODE))
    struct_blk.extend(be32(FDT_END))

    # Nest everything under root "" then the listed children as siblings
    # by rebuilding with a root wrapper.
    child = bytes(struct_blk)
    # child already ends with FDT_END; wrap: BEGIN root, children without END, END_NODE, END
    inner = child[: -4]  # drop trailing FDT_END
    struct_blk = bytearray()
    struct_blk.extend(be32(FDT_BEGIN_NODE))
    struct_blk.extend(align4(b"\x00"))  # root name ""
    struct_blk.extend(inner)
    struct_blk.extend(be32(FDT_END_NODE))
    struct_blk.extend(be32(FDT_END))

    strings_b = align4(bytes(strings))
    rsv = be32(0) + be32(0) + be32(0) + be32(0)  # empty reserve + terminator
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_strings = off_struct + len(struct_blk)
    total = off_strings + len(strings_b)

    hdr = b"".join(
        [
            be32(FDT_MAGIC),
            be32(total),
            be32(off_struct),
            be32(off_strings),
            be32(off_rsv),
            be32(17),  # version
            be32(16),  # last compact
            be32(0),  # boot cpuid
            be32(len(strings_b)),
            be32(len(struct_blk)),
        ]
    )
    assert len(hdr) == 40
    return hdr + rsv + bytes(struct_blk) + strings_b


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--out", type=Path, default=Path("fuzz/corpus-fdt"))
    args = ap.parse_args()
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    tiny = build_dtb(
        [
            (
                "memory@0",
                [
                    ("device_type", b"memory\x00"),
                    ("reg", be32(0) + be32(0) + be32(0) + be32(0x40000000)),
                ],
            ),
            (
                "uart@9000000",
                [
                    ("compatible", b"arm,pl011\x00arm,primecell\x00"),
                    ("reg", be32(0) + be32(0x09000000) + be32(0) + be32(0x1000)),
                    ("status", b"okay\x00"),
                ],
            ),
            (
                "virtio_mmio@a000000",
                [
                    ("compatible", b"virtio,mmio\x00"),
                    ("reg", be32(0) + be32(0x0A000000) + be32(0) + be32(0x200)),
                ],
            ),
        ]
    )
    (out / "minimal.dtb").write_bytes(tiny)

    # Root-only empty tree
    empty = build_dtb([])
    (out / "root_only.dtb").write_bytes(empty)

    # Bad magic
    bad = bytearray(tiny)
    bad[0] = 0
    (out / "bad_magic.dtb").write_bytes(bad)

    # Truncated header
    (out / "trunc_hdr.bin").write_bytes(tiny[:20])

    # Random-ish
    (out / "emptyish.bin").write_bytes(b"\x00" * 16)
    (out / "randomish.bin").write_bytes(bytes(range(64)) + b"FDT?" * 8)

    # totalsize lie
    lie = bytearray(tiny)
    lie[4:8] = be32(8)
    (out / "bad_totalsize.dtb").write_bytes(lie)

    print(f"wrote {len(list(out.iterdir()))} seeds under {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
