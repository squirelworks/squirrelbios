#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/pe_harness.c.

Original code. Writes synthetic PE32+ images (and a few deliberately
broken ones) under the given directory (default: fuzz/corpus-pe).
No QEMU; no vendor tools. Machine type 0x8664 matches fuzz/arch.h.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

# PE32+ / COFF field helpers (little-endian). Machine = AMD64.
PE_MACHINE_AMD64 = 0x8664
PE32PLUS_MAGIC = 0x20B
SUBSYSTEM_EFI_APP = 10
SUBSYSTEM_EFI_BOOT = 11


def wr16(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<H", buf, off, val & 0xFFFF)


def wr32(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)


def wr64(buf: bytearray, off: int, val: int) -> None:
    struct.pack_into("<Q", buf, off, val & 0xFFFFFFFFFFFFFFFF)


def pe32plus(
    *,
    machine: int = PE_MACHINE_AMD64,
    subsystem: int = SUBSYSTEM_EFI_APP,
    image_base: int = 0x140000000,
    image_size: int = 0x1000,
    entry: int = 0,
    nsect: int = 0,
    chars: int = 0x0022,  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    optsize: int = 112,
    ndirs: int = 0,
    reloc_rva: int = 0,
    reloc_size: int = 0,
    sect_align: int = 0x1000,
    file_align: int = 0x200,
    sections: list[tuple[bytes, int, int, bytes]] | None = None,
    pe_off: int = 0x40,
) -> bytes:
    """Build a minimal PE32+ image. sections: (name8, va, vsize, raw_bytes)."""
    if sections is None:
        sections = []
    nsect = len(sections) if sections else nsect

    # Optional header: at least 112 bytes; extend for data dirs if needed.
    if ndirs > 0 or reloc_size:
        need = 112 + max(ndirs, 6) * 8
        if optsize < need:
            optsize = need
        if ndirs < 6 and reloc_size:
            ndirs = 6

    coff_off = pe_off + 4
    opt_off = coff_off + 20
    sect_off = opt_off + optsize
    hdr_end = sect_off + nsect * 40
    # FileAlignment-style pad for SizeOfHeaders
    hdr_size = (hdr_end + file_align - 1) // file_align * file_align
    if hdr_size < hdr_end:
        hdr_size = hdr_end
    if image_size < hdr_size:
        image_size = hdr_size

    # Place section raw data after headers
    raw_cursor = hdr_size
    sect_raws: list[tuple[int, bytes]] = []
    for _name, _va, _vsize, raw in sections:
        sect_raws.append((raw_cursor, raw))
        raw_cursor += (len(raw) + file_align - 1) // file_align * file_align

    total = max(raw_cursor, hdr_size, 0x40 + 4)
    img = bytearray(total)

    img[0] = ord("M")
    img[1] = ord("Z")
    wr32(img, 0x3C, pe_off)
    img[pe_off : pe_off + 4] = b"PE\0\0"

    # COFF
    wr16(img, coff_off + 0, machine)
    wr16(img, coff_off + 2, nsect)
    wr16(img, coff_off + 16, optsize)
    wr16(img, coff_off + 18, chars)

    # Optional PE32+
    wr16(img, opt_off + 0, PE32PLUS_MAGIC)
    wr32(img, opt_off + 16, entry)
    wr64(img, opt_off + 24, image_base)
    wr32(img, opt_off + 32, sect_align)
    wr32(img, opt_off + 36, file_align)
    wr32(img, opt_off + 56, image_size)
    wr32(img, opt_off + 60, hdr_size)
    wr16(img, opt_off + 68, subsystem)
    wr32(img, opt_off + 108, ndirs)
    if ndirs > 5 and optsize >= 112 + 6 * 8:
        wr32(img, opt_off + 112 + 5 * 8, reloc_rva)
        wr32(img, opt_off + 112 + 5 * 8 + 4, reloc_size)

    for i, ((name, va, vsize, raw), (raw_off, _)) in enumerate(
        zip(sections, sect_raws)
    ):
        s = sect_off + i * 40
        nm = (name + b"\0" * 8)[:8]
        img[s : s + 8] = nm
        wr32(img, s + 8, vsize)
        wr32(img, s + 12, va)
        wr32(img, s + 16, len(raw))
        wr32(img, s + 20, raw_off)
        img[raw_off : raw_off + len(raw)] = raw

    return bytes(img)


def pe_minimal_app() -> bytes:
    """Smallest PE32+ EFI application the loader accepts (0 sections)."""
    return pe32plus()


def pe_with_section() -> bytes:
    """One .text section with a few bytes of 'code'."""
    code = b"\x48\x31\xc0\xc3"  # xor rax,rax; ret — never executed by harness
    return pe32plus(
        image_size=0x2000,
        entry=0x1000,
        sections=[(b".text", 0x1000, 0x1000, code)],
    )


def pe_boot_driver() -> bytes:
    return pe32plus(subsystem=SUBSYSTEM_EFI_BOOT, image_base=0x180000000)


def pe_with_reloc() -> bytes:
    """Image with a DIR64 reloc block (applied when base drifts)."""
    # Reloc block at RVA 0x2000: page 0x1000, one DIR64 at +0
    block = bytearray(12)
    wr32(block, 0, 0x1000)  # page RVA
    wr32(block, 4, 12)  # block size
    wr16(block, 8, (10 << 12) | 0)  # DIR64 at offset 0
    code = bytearray(8)
    wr64(code, 0, 0x140001000)  # absolute that reloc will adjust
    return pe32plus(
        image_size=0x3000,
        entry=0x1000,
        ndirs=6,
        reloc_rva=0x2000,
        reloc_size=12,
        sections=[
            (b".text", 0x1000, 0x1000, bytes(code) + b"\x00" * (0x200 - 8)),
            (b".reloc", 0x2000, 0x1000, bytes(block)),
        ],
    )


def pe_bad_mz() -> bytes:
    return b"\x00\x00" + b"\xff" * 62


def pe_bad_machine() -> bytes:
    return pe32plus(machine=0x14C)  # i386


def pe_truncated() -> bytes:
    img = bytearray(pe32plus())
    return bytes(img[:80])


def pe_stripped_relocs() -> bytes:
    """RELOCS_STRIPPED + ImageBase outside host RAM → LoadImage fails."""
    return pe32plus(chars=0x0001 | 0x0002, image_base=0x140000000)


def pe_huge_claim() -> bytes:
    """Enormous SizeOfImage; host page allocator must refuse."""
    return pe32plus(image_size=0x10000000)  # 256 MiB claim


def pe_bad_subsystem() -> bytes:
    return pe32plus(subsystem=1)  # IMAGE_SUBSYSTEM_NATIVE


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=Path("fuzz/corpus-pe"),
        help="output directory",
    )
    args = ap.parse_args()
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    seeds = {
        "pe_minimal_app.efi": pe_minimal_app(),
        "pe_with_section.efi": pe_with_section(),
        "pe_boot_driver.efi": pe_boot_driver(),
        "pe_with_reloc.efi": pe_with_reloc(),
        "pe_bad_mz.bin": pe_bad_mz(),
        "pe_bad_machine.efi": pe_bad_machine(),
        "pe_truncated.bin": pe_truncated(),
        "pe_stripped_relocs.efi": pe_stripped_relocs(),
        "pe_huge_claim.efi": pe_huge_claim(),
        "pe_bad_subsystem.efi": pe_bad_subsystem(),
        "emptyish.bin": b"\x00" * 64,
    }
    for name, data in seeds.items():
        path = out / name
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
