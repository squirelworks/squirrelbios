#!/usr/bin/env python3
"""Replay a TCG2 (crypto-agile) event log against TPM PCR values (C5).
Original code, written from the TCG PC Client Platform Firmware Profile.

    tests/tcglog.py SERIAL.LOG
        reads the TPMLOG and TPMPCR lines tests/x86_64-tpm.asm prints
    tests/tcglog.py --image APP.efi SERIAL.LOG
        the same, and a PCR 4 application event must carry APP's
        Authenticode SHA-256 (tests/mksigned.py computes it)
    tests/tcglog.py --linux SERIAL.LOG
        reads the TCGLOG (od -An -tx1 of binary_bios_measurements) and
        PCR<n>=<hex> lines of the linux-uefi.sh init

Replays the SHA-256 bank, checks it gives PCRs 0-7 and that every event
digest of a type whose digest is the hash of its data matches, and prints
    tcglog: N events, SHA-256 PCRs 0-7 match the log (types ...)
or what differs, exiting 1.
"""
import hashlib
import re
import struct
import sys

SIZES = {0x0004: 20, 0x000B: 32, 0x000C: 48, 0x000D: 64}
HASH = {0x0004: hashlib.sha1, 0x000B: hashlib.sha256, 0x000C: hashlib.sha384, 0x000D: hashlib.sha512}
NAMES = {0x3: "NO_ACTION", 0x4: "SEPARATOR", 0x6: "EVENT_TAG", 0x8: "S_CRTM_VERSION", 0xD: "IPL",
         0x80000001: "EFI_VARIABLE_DRIVER_CONFIG", 0x80000003: "EFI_BOOT_SERVICES_APPLICATION",
         0x80000007: "EFI_ACTION", 0x80000008: "EFI_PLATFORM_FIRMWARE_BLOB"}
DATA_HASHED = {0x4, 0x8, 0x80000001, 0x80000007}


def parse(log, last=None):
    """The events of a crypto-agile log: (pcr, type, {alg: digest}, data)"""
    pcr, etype = struct.unpack_from("<II", log, 0)
    size = struct.unpack_from("<I", log, 28)[0]
    spec = log[32:32 + size]
    if etype != 3 or not spec.startswith(b"Spec ID Event03\0"):
        raise ValueError("no Spec ID Event03 header")
    n = struct.unpack_from("<I", spec, 24)[0]
    algs = [struct.unpack_from("<HH", spec, 28 + 4 * i) for i in range(n)]
    events, at = [], 32 + size
    while at + 12 <= len(log):
        start = at
        pcr, etype, count = struct.unpack_from("<III", log, at)
        if etype == 0 and pcr == 0 and count == 0:
            break                               # zeroed space after the log
        at += 12
        digests = {}
        for _ in range(count):
            alg = struct.unpack_from("<H", log, at)[0]
            if alg not in SIZES:
                raise ValueError("unknown algorithm 0x%x at offset %d" % (alg, start))
            digests[alg] = log[at + 2:at + 2 + SIZES[alg]]
            at += 2 + SIZES[alg]
        esize = struct.unpack_from("<I", log, at)[0]
        data = log[at + 4:at + 4 + esize]
        at += 4 + esize
        if at > len(log):
            raise ValueError("an event runs past the log")
        events.append((pcr, etype, digests, data))
        if last is not None and start == last:
            break
    return algs, events


IMAGE = None


def check(log, pcrs, last=None):
    algs, events = parse(log, last)
    if IMAGE:
        sys.dont_write_bytecode = True
        sys.path.insert(0, __import__("os").path.dirname(__file__))
        from mksigned import authenticode_hash
        img = open(IMAGE, "rb").read()
        want = authenticode_hash(img + b"\x00" * (-len(img) % 8))
        if not any(e[1] == 0x80000003 and e[0] == 4 and e[2].get(0x000B) == want for e in events):
            print("tcglog: no PCR 4 application event with %s's Authenticode SHA-256" % IMAGE)
            return 1
    if (0x000B, 32) not in algs:
        print("tcglog: no SHA-256 bank in the log")
        return 1
    bad = 0
    replay = {i: bytes(32) for i in range(24)}
    for i, (pcr, etype, d, data) in enumerate(events):
        if 0x000B not in d:
            print("tcglog: event %d has no SHA-256 digest" % i)
            bad = 1
            continue
        for alg, dig in d.items():
            if etype in DATA_HASHED and HASH[alg](data).digest() != dig:
                print("tcglog: event %d (PCR %d, %s): digest is not the hash of its data" %
                      (i, pcr, NAMES.get(etype, hex(etype))))
                bad = 1
        if etype != 3:
            replay[pcr] = hashlib.sha256(replay[pcr] + d[0x000B]).digest()
    for i in range(8):
        if replay[i] != pcrs[i]:
            print("tcglog: PCR %d is %s, the log gives %s" % (i, pcrs[i].hex(), replay[i].hex()))
            bad = 1
    if not bad:
        kinds = sorted({NAMES.get(e[1], hex(e[1])) for e in events})
        print("tcglog: %d events, SHA-256 PCRs 0-7 match the log (%s)" % (len(events), ", ".join(kinds)))
    return bad


def from_app(text):
    m = re.search(r"TPMLOG (\d+) ([0-9a-f]+)", text)
    p = re.search(r"TPMPCR ([0-9a-f]+)", text)
    if not m or not p:
        print("tcglog: no TPMLOG/TPMPCR lines")
        return 1
    log = bytes.fromhex(m.group(2))
    rsp = bytes.fromhex(p.group(1))
    if struct.unpack_from(">I", rsp, 6)[0] != 0:
        print("tcglog: TPM2_PCR_Read failed")
        return 1
    at = 10 + 4 + 10
    count = struct.unpack_from(">I", rsp, at)[0]
    at += 4
    pcrs = []
    for _ in range(count):
        size = struct.unpack_from(">H", rsp, at)[0]
        pcrs.append(rsp[at + 2:at + 2 + size])
        at += 2 + size
    if len(pcrs) != 8:
        print("tcglog: %d PCR values read, not 8" % len(pcrs))
        return 1
    return check(log, pcrs, int(m.group(1)))


def from_linux(text):
    hexes = re.findall(r"TCGLOG ([0-9a-f ]+)", text)
    pcrs = {}
    for n, v in re.findall(r"PCR(\d+)=([0-9A-Fa-f]{64})", text):
        pcrs[int(n)] = bytes.fromhex(v)
    if not hexes or len(pcrs) < 8:
        print("tcglog: no TCGLOG or PCR0-7 lines")
        return 1
    log = bytes.fromhex("".join(hexes).replace(" ", ""))
    return check(log, [pcrs[i] for i in range(8)])


def main():
    global IMAGE
    linux = sys.argv[1] == "--linux"
    if sys.argv[1] == "--image":
        IMAGE = sys.argv[2]
    text = open(sys.argv[-1], "rb").read().decode("latin-1").replace("\r", "")
    try:
        sys.exit(from_linux(text) if linux else from_app(text))
    except (ValueError, struct.error) as e:
        print("tcglog: %s" % e)
        sys.exit(1)


if __name__ == "__main__":
    main()
