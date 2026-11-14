#!/usr/bin/env python3
# Build a synthetic coreboot LBIO table blob for QEMU smoke tests (C5).
# Original code. Layout follows the public coreboot table ABI
# (doc.coreboot.org "ABI data consumption"; BSD coreboot_tables.h tags):
# signature "LBIO", header/table checksums, CB_TAG_MEMORY ranges,
# CB_TAG_SERIAL, CB_TAG_ACPI_RSDP. No coreboot source was copied.
import argparse
import struct
import sys

CB_TAG_MEMORY = 0x0001
CB_TAG_SERIAL = 0x000F
CB_TAG_ACPI_RSDP = 0x0043
CB_MEM_RAM = 1
CB_MEM_TABLE = 16
CB_SERIAL_IO = 1

# Tables live here under QEMU (-device loader); below FW_BASE (2 MB).
TABLES_ADDR = 0x0007F000


def ip_checksum(data: bytes) -> int:
    if len(data) % 2:
        data = data + b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += data[i] | (data[i + 1] << 8)
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def u32(v):
    return struct.pack("<I", v)


def u64(v):
    return struct.pack("<Q", v)


def mem_range(start, size, typ):
    return u64(start) + u64(size) + u32(typ)


def acpi_checksum(data: bytes) -> int:
    return (-sum(data)) & 0xFF


def build_acpi(rsdp_addr: int) -> bytes:
    """Minimal RSDP 2.0 + XSDT + empty-ish FACP so the firmware can publish them."""
    # Layout inside the blob after the LBIO records; addresses are absolute.
    # We build relative first, then patch pointers.
    facp = bytearray(76)
    facp[0:4] = b"FACP"
    facp[4:8] = u32(76)
    facp[8] = 1
    facp[10:16] = b"SQRLWK"
    facp[16:24] = b"SQCBTEST"
    facp[28:32] = b"SQRL"
    facp[9] = 0
    facp[9] = acpi_checksum(facp)

    xsdt_len = 36 + 8
    xsdt = bytearray(xsdt_len)
    xsdt[0:4] = b"XSDT"
    xsdt[4:8] = u32(xsdt_len)
    xsdt[8] = 1
    xsdt[10:16] = b"SQRLWK"
    xsdt[16:24] = b"SQCBTEST"
    xsdt[28:32] = b"SQRL"

    rsdt_len = 36 + 4
    rsdt = bytearray(rsdt_len)
    rsdt[0:4] = b"RSDT"
    rsdt[4:8] = u32(rsdt_len)
    rsdt[8] = 1
    rsdt[10:16] = b"SQRLWK"
    rsdt[16:24] = b"SQCBTEST"
    rsdt[28:32] = b"SQRL"

    rsdp = bytearray(36)
    rsdp[0:8] = b"RSD PTR "
    rsdp[9:15] = b"SQRLWK"
    rsdp[15] = 2
    rsdp[20:24] = u32(36)

    # Absolute placement: RSDP, then RSDT, XSDT, FACP
    rsdt_addr = rsdp_addr + 36
    xsdt_addr = rsdt_addr + rsdt_len
    facp_addr = xsdt_addr + xsdt_len

    rsdp[16:20] = u32(rsdt_addr)
    rsdp[24:32] = u64(xsdt_addr)
    rsdp[8] = 0
    rsdp[8] = acpi_checksum(rsdp[:20])
    rsdp[32] = 0
    rsdp[32] = acpi_checksum(rsdp)

    xsdt[36:44] = u64(facp_addr)
    xsdt[9] = 0
    xsdt[9] = acpi_checksum(xsdt)

    rsdt[36:40] = u32(facp_addr)
    rsdt[9] = 0
    rsdt[9] = acpi_checksum(rsdt)

    return bytes(rsdp) + bytes(rsdt) + bytes(xsdt) + bytes(facp)


def build(addr: int = TABLES_ADDR) -> bytes:
    # Memory map: ~95 MB usable, tables carved out as CB_MEM_TABLE.
    # Distinct from QEMU's default 128 MB fw_cfg map so the smoke test
    # can see we took the LBIO path.
    ranges = [
        mem_range(0x00000000, 0x0007F000, CB_MEM_RAM),
        mem_range(0x0007F000, 0x00001000, CB_MEM_TABLE),
        mem_range(0x00080000, 0x00020000, CB_MEM_RAM),
        mem_range(0x00100000, 0x05F00000, CB_MEM_RAM),
    ]
    mem_body = b"".join(ranges)
    mem_rec = u32(CB_TAG_MEMORY) + u32(8 + len(mem_body)) + mem_body

    serial_rec = (
        u32(CB_TAG_SERIAL)
        + u32(32)
        + u32(CB_SERIAL_IO)
        + u32(0x3F8)
        + u32(115200)
        + u32(1)
        + u32(1843200)
        + u32(0)
    )

    # RSDP sits after the LBIO header + records; size of records first.
    # header 24 + mem + serial + rsdp_tag(16) = known once we fix rsdp tag.
    rsdp_tag_size = 16
    header_bytes = 24
    table_bytes_wo_rsdp = len(mem_rec) + len(serial_rec) + rsdp_tag_size
    rsdp_addr = addr + header_bytes + table_bytes_wo_rsdp

    rsdp_rec = u32(CB_TAG_ACPI_RSDP) + u32(rsdp_tag_size) + u64(rsdp_addr)
    table = mem_rec + serial_rec + rsdp_rec
    assert len(table) == table_bytes_wo_rsdp

    header = bytearray(24)
    header[0:4] = b"LBIO"
    header[4:8] = u32(header_bytes)
    header[12:16] = u32(len(table))
    header[16:20] = u32(ip_checksum(table))
    header[20:24] = u32(3)  # memory, serial, acpi_rsdp
    header[8:12] = u32(0)
    header[8:12] = u32(ip_checksum(header))

    acpi = build_acpi(rsdp_addr)
    blob = bytes(header) + table + acpi
    # Pad to 4 KB so the loader mapping is a clean page.
    if len(blob) < 0x1000:
        blob = blob + bytes(0x1000 - len(blob))
    return blob


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument(
        "--addr",
        type=lambda s: int(s, 0),
        default=TABLES_ADDR,
        help="load address (default %#x)" % TABLES_ADDR,
    )
    args = ap.parse_args()
    data = build(args.addr)
    with open(args.output, "wb") as f:
        f.write(data)
    print("mkcbtables: %u bytes for load address %#x" % (len(data), args.addr), file=sys.stderr)


if __name__ == "__main__":
    main()
