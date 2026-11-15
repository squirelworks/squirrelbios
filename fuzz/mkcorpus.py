#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/fat_harness.c.

Original code. Writes synthetic FAT12/16/32-ish images and a few
deliberately broken BPBs under the given directory (default:
fuzz/corpus). No QEMU; no vendor tools.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def wr16(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<H", buf, off, val & 0xFFFF)


def wr32(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)


def boot_jump(buf: bytearray) -> None:
    buf[0] = 0xEB
    buf[1] = 0x3C
    buf[2] = 0x90
    buf[510] = 0x55
    buf[511] = 0xAA


def fat12_tiny() -> bytes:
    """Minimal FAT12 volume: 1 reserved, 2 FATs, 16 root entries, a few clusters."""
    bps, spc, reserved, fats, root_ent = 512, 1, 1, 2, 16
    fat_secs = 1
    root_secs = (root_ent * 32 + bps - 1) // bps
    data_clus = 8
    total = reserved + fats * fat_secs + root_secs + data_clus * spc
    img = bytearray(total * bps)

    boot_jump(img)
    img[3:11] = b"MSWIN4.1"  # OEM; not a vendor copy, just the BPB field
    wr16(img, 11, bps)
    img[13] = spc
    wr16(img, 14, reserved)
    img[16] = fats
    wr16(img, 17, root_ent)
    wr16(img, 19, total)
    img[21] = 0xF8
    wr16(img, 22, fat_secs)
    wr16(img, 24, 32)
    wr16(img, 26, 2)

    # FAT copy 0 and 1: media + EOC for cluster 2 (README.TXT)
    for copy in range(fats):
        base = (reserved + copy * fat_secs) * bps
        img[base + 0] = 0xF8
        img[base + 1] = 0xFF
        img[base + 2] = 0xFF
        # cluster 2 -> EOC (0xFFF), little-endian packed FAT12
        img[base + 3] = 0xFF
        img[base + 4] = 0x0F

    root = (reserved + fats * fat_secs) * bps
    name = b"README  TXT"
    img[root : root + 11] = name
    img[root + 11] = 0x20
    wr16(img, root + 26, 2)  # first cluster
    wr32(img, root + 28, 5)  # size

    data = (reserved + fats * fat_secs + root_secs) * bps
    img[data : data + 5] = b"hello"

    return bytes(img)


def fat16_small() -> bytes:
    bps, spc, reserved, fats, root_ent = 512, 1, 1, 2, 32
    fat_secs = 2
    root_secs = (root_ent * 32 + bps - 1) // bps
    # enough clusters for FAT16 (>= 4085)
    data_clus = 4096
    total = reserved + fats * fat_secs + root_secs + data_clus * spc
    # Cap the image for the seed: claim many sectors in BPB but only
    # materialise a short prefix; the harness pads / truncates by length.
    # Better: keep a real but compact image — use 4200 clusters is ~2 MB.
    # For seeds, shrink: use FAT16 with just-above-FAT12 cluster count on
    # a truncated file that still has valid reserved+FAT+root.
    data_clus = 64
    total = reserved + fats * fat_secs + root_secs + data_clus * spc
    img = bytearray(total * bps)
    boot_jump(img)
    img[3:11] = b"SQUIRREL"
    wr16(img, 11, bps)
    img[13] = spc
    wr16(img, 14, reserved)
    img[16] = fats
    wr16(img, 17, root_ent)
    wr16(img, 19, total)
    img[21] = 0xF8
    wr16(img, 22, fat_secs)
    # Force FAT16 by claiming enough clusters even if we only store 64:
    # fat.c derives kind from (total - data_start) / spc. With 64 clusters
    # that is FAT12. Put total_secs high while keeping the on-disk image
    # short so reads past the end fail through Block I/O — a useful case.
    wr16(img, 19, 0)
    wr32(img, 32, reserved + fats * fat_secs + root_secs + 5000)

    for copy in range(fats):
        base = (reserved + copy * fat_secs) * bps
        wr16(img, base, 0xFFF8)
        wr16(img, base + 2, 0xFFFF)
    return bytes(img)


def fat32_bpb_only() -> bytes:
    """FAT32 BPB with FSInfo pointer; short image (mount may fail later)."""
    bps, spc, reserved, fats = 512, 1, 8, 2
    fat_secs = 4
    total = 2048
    img = bytearray(64 * 1024)
    boot_jump(img)
    img[3:11] = b"SQUIRREL"
    wr16(img, 11, bps)
    img[13] = spc
    wr16(img, 14, reserved)
    img[16] = fats
    wr16(img, 17, 0)  # FAT32: root entries 0
    wr16(img, 19, 0)
    img[21] = 0xF8
    wr16(img, 22, 0)  # FAT32: fat16 size 0
    wr32(img, 32, total)
    wr32(img, 36, fat_secs)
    wr32(img, 44, 2)  # root cluster
    wr16(img, 48, 1)  # FSInfo sector
    img[66] = 0x29
    img[71:82] = b"NO NAME    "
    img[82:90] = b"FAT32   "
    # FSInfo signature
    fs = 1 * bps
    wr32(img, fs, 0x41615252)
    wr32(img, fs + 484, 0x61417272)
    wr32(img, fs + 488, 0xFFFFFFFF)
    wr32(img, fs + 492, 3)
    img[fs + 510] = 0x55
    img[fs + 511] = 0xAA
    return bytes(img)


def bad_signature() -> bytes:
    img = bytearray(512)
    img[0] = 0xEB
    img[1] = 0x3C
    img[2] = 0x90
    wr16(img, 11, 512)
    img[13] = 1
    wr16(img, 14, 1)
    img[16] = 2
    wr16(img, 17, 16)
    wr16(img, 19, 100)
    wr16(img, 22, 1)
    img[510] = 0x00  # not 55 AA
    img[511] = 0x00
    return bytes(img)


def bad_bps() -> bytes:
    img = bytearray(fat12_tiny())
    wr16(img, 11, 513)  # not a power-of-two sector size
    return bytes(img)


def truncated() -> bytes:
    return fat12_tiny()[:64]


def emptyish() -> bytes:
    return b"\x00" * 64


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent / "corpus",
    )
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    seeds = {
        "fat12_tiny.img": fat12_tiny(),
        "fat16_claim.img": fat16_small(),
        "fat32_bpb.img": fat32_bpb_only(),
        "bad_signature.img": bad_signature(),
        "bad_bps.img": bad_bps(),
        "truncated.img": truncated(),
        "emptyish.bin": emptyish(),
    }
    for name, data in seeds.items():
        path = args.out / name
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
