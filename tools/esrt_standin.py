#!/usr/bin/env python3
"""Host-side ESRT stand-in for Phase G M4 (fwupd-shaped list/update).

Original code. Reads a QEMU serial log from the x86_64-capsule-key guest
(or any log with the same APP esrt-* lines) and checks the shape fwupd-
class tooling expects under QEMU:

  * FwResourceVersion == 1
  * one SYSTEMFIRMWARE entry (type 1)
  * FwClass == SquirrelBIOS system-firmware GUID
  * CapsuleFlags includes PERSIST_ACROSS_RESET (0x00010000)
  * after UpdateCapsule, LastAttemptStatus SUCCESS and FwVersion matches
    the project capsule target (0x00020400)

Usage:
  python3 tools/esrt_standin.py path/to/x86_64-capsule-key.log
  python3 tools/esrt_standin.py --self-test

Exit 0 on success; non-zero with a one-line reason on failure.
"""
from __future__ import annotations

import re
import sys

SYSFW = "a7c4e9f2-3b1d-4e8a-9c55-12846f91de30"
PERSIST = 0x00010000
TARGET = 0x00020400

LIST_RE = re.compile(
    r"APP esrt-list count=(\d+) version=(\d+) max=(\d+)"
)
ENTRY_RE = re.compile(
    r"APP esrt-entry0 class=([0-9a-fA-F-]{36}) type=(\d+) "
    r"fw=(0x[0-9a-fA-F]+) lowest=(0x[0-9a-fA-F]+) "
    r"flags=(0x[0-9a-fA-F]+) last_ver=(0x[0-9a-fA-F]+) "
    r"last_status=(\d+)"
)
SUMMARY_RE = re.compile(
    r"APP capsule-key list=ok update=ok esrt=ok"
)

SAMPLE = """\
capsule: ESRT at 0x0, 1 resource (system firmware 0x00020300)
capsule: trust ESL 100 bytes (1 key(s))
APP esrt-list count=1 version=1 max=1
APP esrt-entry0 class=a7c4e9f2-3b1d-4e8a-9c55-12846f91de30 type=1 fw=0x00020300 lowest=0x00020300 flags=0x00010000 last_ver=0x00000000 last_status=0
capsule: apply ok version=0x00020400 (flash rewrite via NV)
APP esrt-list count=1 version=1 max=1
APP esrt-entry0 class=a7c4e9f2-3b1d-4e8a-9c55-12846f91de30 type=1 fw=0x00020400 lowest=0x00020300 flags=0x00010000 last_ver=0x00020400 last_status=0
APP capsule-key list=ok update=ok esrt=ok
"""


def fail(msg: str) -> int:
    print(f"esrt_standin: FAIL {msg}", file=sys.stderr)
    return 1


def check(text: str) -> int:
    lists = LIST_RE.findall(text)
    entries = ENTRY_RE.findall(text)
    if len(lists) < 2:
        return fail(f"need >=2 esrt-list lines, got {len(lists)}")
    if len(entries) < 2:
        return fail(f"need >=2 esrt-entry0 lines, got {len(entries)}")
    if not SUMMARY_RE.search(text):
        return fail("missing APP capsule-key list=ok update=ok esrt=ok")

    # First dump = before UpdateCapsule; last dump = after.
    c0, v0, m0 = int(lists[0][0]), int(lists[0][1]), int(lists[0][2])
    c1, v1, m1 = int(lists[-1][0]), int(lists[-1][1]), int(lists[-1][2])
    if c0 != 1 or c1 != 1:
        return fail(f"FwResourceCount want 1, got before={c0} after={c1}")
    if v0 != 1 or v1 != 1:
        return fail(f"FwResourceVersion want 1, got before={v0} after={v1}")
    if m0 < 1 or m1 < 1:
        return fail(f"FwResourceCountMax too small before={m0} after={m1}")

    e0 = entries[0]
    e1 = entries[-1]
    for label, e in (("before", e0), ("after", e1)):
        cls, typ, _fw, _lowest, flags, _last_ver, _last_status = e
        if cls.lower() != SYSFW:
            return fail(f"{label}: FwClass {cls} != {SYSFW}")
        if int(typ) != 1:
            return fail(f"{label}: FwType {typ} != 1 (SYSTEMFIRMWARE)")
        if (int(flags, 16) & PERSIST) == 0:
            return fail(f"{label}: CapsuleFlags missing PERSIST_ACROSS_RESET")

    fw_after = int(e1[2], 16)
    last_ver = int(e1[5], 16)
    last_status = int(e1[6])
    if fw_after != TARGET:
        return fail(f"after: FwVersion 0x{fw_after:08x} != 0x{TARGET:08x}")
    if last_ver != TARGET or last_status != 0:
        return fail(
            f"after: LastAttempt version=0x{last_ver:08x} status={last_status}"
        )

    print(
        "esrt_standin: OK list=1 version=1 "
        f"class={SYSFW} type=SYSTEMFIRMWARE "
        f"fw=0x{fw_after:08x} flags=PERSIST update=ok"
    )
    return 0


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        return check(SAMPLE)
    if len(sys.argv) != 2:
        print("usage: esrt_standin.py LOG | --self-test", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        text = open(path, "r", errors="replace").read()
    except OSError as e:
        return fail(f"cannot read {path}: {e}")
    return check(text)


if __name__ == "__main__":
    sys.exit(main())
