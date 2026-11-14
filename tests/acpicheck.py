#!/usr/bin/env python3
"""Host-side ACPI table checks for F1-scale (roadmap f1-scale).
Original code. Reads APIC.bin / SRAT.bin / MCFG.bin written by
tests/x86_64-tables and checks checksums and topology.

  acpicheck.py 1024cpu DIR   # 1024 distinct x2APIC IDs, SRAT per node
  acpicheck.py 2seg DIR      # two MCFG allocations with their bus ranges
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def load_table(path: Path, sig: bytes) -> bytes:
    data = path.read_bytes()
    if len(data) < 36:
        raise SystemExit(f"{path}: too short")
    if data[0:4] != sig:
        raise SystemExit(f"{path}: signature {data[0:4]!r}, want {sig!r}")
    length = struct.unpack_from("<I", data, 4)[0]
    if length != len(data):
        raise SystemExit(f"{path}: length field {length}, file {len(data)}")
    if checksum(data) != 0:
        raise SystemExit(f"{path}: bad checksum")
    return data


def walk_subtables(data: bytes, start: int):
    end = len(data)
    off = start
    while off < end:
        if off + 2 > end:
            raise SystemExit("truncated subtable header")
        typ, length = data[off], data[off + 1]
        if length < 2 or off + length > end:
            raise SystemExit(f"bad subtable type={typ} len={length} at {off}")
        yield typ, data[off : off + length]
        off += length


def check_1024cpu(d: Path) -> None:
    madt = load_table(d / "APIC.bin", b"APIC")
    srat = load_table(d / "SRAT.bin", b"SRAT")
    load_table(d / "MCFG.bin", b"MCFG")

    x2_ids = []
    for typ, body in walk_subtables(madt, 44):
        if typ == 9:
            if len(body) < 16:
                raise SystemExit("short x2APIC MADT entry")
            apic_id = struct.unpack_from("<I", body, 4)[0]
            flags = struct.unpack_from("<I", body, 8)[0]
            if flags & 1:
                x2_ids.append(apic_id)
        elif typ == 0:
            raise SystemExit("unexpected local APIC entry in 1024cpu MADT")

    if len(x2_ids) != 1024:
        raise SystemExit(f"MADT x2APIC count {len(x2_ids)}, want 1024")
    if len(set(x2_ids)) != 1024:
        raise SystemExit("MADT x2APIC IDs are not distinct")

    cpu_prox = []
    mem_nodes = set()
    for typ, body in walk_subtables(srat, 48):
        if typ == 2:
            if len(body) < 24:
                raise SystemExit("short x2APIC SRAT entry")
            prox = struct.unpack_from("<I", body, 4)[0]
            apic_id = struct.unpack_from("<I", body, 8)[0]
            flags = struct.unpack_from("<I", body, 12)[0]
            if flags & 1:
                cpu_prox.append((apic_id, prox))
        elif typ == 0:
            raise SystemExit("unexpected local APIC affinity in 1024cpu SRAT")
        elif typ == 1:
            if len(body) < 40:
                raise SystemExit("short memory affinity")
            prox = struct.unpack_from("<I", body, 2)[0]
            flags = struct.unpack_from("<I", body, 28)[0]
            if flags & 1:
                mem_nodes.add(prox)

    if len(cpu_prox) != 1024:
        raise SystemExit(f"SRAT x2APIC affinity count {len(cpu_prox)}, want 1024")
    nodes = {p for _, p in cpu_prox}
    if not nodes:
        raise SystemExit("SRAT has no CPU proximity domains")
    if not mem_nodes:
        raise SystemExit("SRAT has no memory affinity")
    if nodes != mem_nodes:
        raise SystemExit(
            f"SRAT affinity mismatch: cpus={sorted(nodes)} mem={sorted(mem_nodes)}"
        )
    # four nodes, 256 CPUs each (builder layout)
    if len(nodes) != 4:
        raise SystemExit(f"SRAT node count {len(nodes)}, want 4")
    for n in range(4):
        ids = [a for a, p in cpu_prox if p == n]
        if len(ids) != 256:
            raise SystemExit(f"SRAT node {n} has {len(ids)} CPUs, want 256")

    print("acpicheck: 1024cpu sums=ok madt=1024x2 srat=1024cpu/4node")


def check_2seg(d: Path) -> None:
    madt = load_table(d / "APIC.bin", b"APIC")
    srat = load_table(d / "SRAT.bin", b"SRAT")
    mcfg = load_table(d / "MCFG.bin", b"MCFG")

    # sanity: small MADT/SRAT still checksum
    n_lapic = sum(1 for typ, _ in walk_subtables(madt, 44) if typ == 0)
    if n_lapic < 1:
        raise SystemExit("2seg MADT has no local APIC entries")
    n_cpu = sum(1 for typ, _ in walk_subtables(srat, 48) if typ in (0, 2))
    if n_cpu < 1:
        raise SystemExit("2seg SRAT has no CPU affinity")

    if len(mcfg) < 44 + 32:
        raise SystemExit(f"MCFG length {len(mcfg)}, want at least two allocations")
    nalloc = (len(mcfg) - 44) // 16
    if nalloc != 2:
        raise SystemExit(f"MCFG allocations {nalloc}, want 2")

    seen_seg = []
    buses = []
    for i in range(2):
        off = 44 + 16 * i
        base, seg, start, end = struct.unpack_from("<QHBB", mcfg, off)
        seen_seg.append(seg)
        buses.append((start, end))
        if end < start:
            raise SystemExit(f"MCFG[{i}] bus range {start}-{end} inverted")
        if base == 0:
            raise SystemExit(f"MCFG[{i}] base is zero")
    if len(set(seen_seg)) != 2:
        raise SystemExit(f"MCFG segments not distinct: {seen_seg}")
    if buses[0] == buses[1] and seen_seg[0] == seen_seg[1]:
        raise SystemExit("MCFG allocations look identical")

    print(
        f"acpicheck: 2seg sums=ok mcfg=2"
        f" seg={seen_seg[0]}:{buses[0][0]:02x}-{buses[0][1]:02x}"
        f",{seen_seg[1]}:{buses[1][0]:02x}-{buses[1][1]:02x}"
    )


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[1] not in ("1024cpu", "2seg"):
        print(f"usage: {argv[0]} 1024cpu|2seg DIR", file=sys.stderr)
        return 2
    d = Path(argv[2])
    if not d.is_dir():
        print(f"{d}: not a directory", file=sys.stderr)
        return 1
    if argv[1] == "1024cpu":
        check_1024cpu(d)
    else:
        check_2seg(d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
