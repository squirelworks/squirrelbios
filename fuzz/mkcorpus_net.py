#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/net_harness.c.

Original code. Synthetic DHCP / TFTP / HTTP blobs from RFC 2131/2132,
RFC 1350/2347/2348/2349 and RFC 9112. No packet captures from real nets.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def dhcp_reply(
    msgtype: int = 5,
    yiaddr: bytes = b"\xc0\xa8\x00\x0a",
    siaddr: bytes = b"\xc0\xa8\x00\x01",
    file: bytes = b"BOOTX64.EFI",
    opts: bytes | None = None,
) -> bytes:
    pkt = bytearray(240)
    pkt[0] = 2  # BOOTREPLY
    pkt[1] = 1
    pkt[2] = 6
    pkt[16:20] = yiaddr
    pkt[20:24] = siaddr
    pkt[44:44 + min(63, len(b"pxe.example"))] = b"pxe.example"[:63]
    pkt[108:108 + min(127, len(file))] = file[:127]
    pkt[236:240] = bytes([99, 130, 83, 99])
    if opts is None:
        opts = bytes(
            [
                53,
                1,
                msgtype,
                1,
                4,
                255,
                255,
                255,
                0,
                3,
                4,
                192,
                168,
                0,
                1,
                6,
                4,
                192,
                168,
                0,
                1,
                54,
                4,
                192,
                168,
                0,
                1,
                67,
                len(file),
            ]
        ) + file
    pkt.extend(opts)
    pkt.append(255)
    return bytes(pkt)


def tftp_data(block: int = 1, payload: bytes = b"abc") -> bytes:
    return struct.pack("!HH", 3, block) + payload


def tftp_oack(blksize: int = 1468, tsize: int = 100) -> bytes:
    body = b"blksize\x00" + str(blksize).encode() + b"\x00tsize\x00" + str(tsize).encode() + b"\x00"
    return struct.pack("!H", 6) + body


def tftp_error(msg: str = "no such file") -> bytes:
    return struct.pack("!HH", 5, 1) + msg.encode() + b"\x00"


def http_clen(body: bytes = b"efi-bytes") -> bytes:
    return (
        b"HTTP/1.1 200 OK\r\nContent-Length: "
        + str(len(body)).encode()
        + b"\r\n\r\n"
        + body
    )


def http_chunked() -> bytes:
    return (
        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        b"4\r\nWiki\r\n0\r\n\r\n"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--out", type=Path, default=Path("fuzz/corpus-net"))
    args = ap.parse_args()
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    (out / "dhcp_ack.bin").write_bytes(dhcp_reply(5))
    (out / "dhcp_offer.bin").write_bytes(dhcp_reply(2))
    (out / "dhcp_bad_cookie.bin").write_bytes(dhcp_reply(5)[:236] + b"\x00\x00\x00\x00" + b"\xff")
    (out / "dhcp_trunc.bin").write_bytes(dhcp_reply(5)[:100])
    (out / "dhcp_overflow_opt.bin").write_bytes(
        dhcp_reply(5, opts=bytes([67, 200]) + b"A" * 50)  # claims 200, only 50
    )

    (out / "tftp_data.bin").write_bytes(tftp_data())
    (out / "tftp_oack.bin").write_bytes(tftp_oack())
    (out / "tftp_error.bin").write_bytes(tftp_error())

    (out / "http_clen.bin").write_bytes(http_clen())
    (out / "http_chunked.bin").write_bytes(http_chunked())
    (out / "http_redirect.bin").write_bytes(
        b"HTTP/1.1 302 Found\r\nLocation: http://10.0.0.1/x\r\nContent-Length: 0\r\n\r\n"
    )
    (out / "http_url.txt").write_bytes(b"http://192.0.2.1:8080/EFI/BOOT/BOOTX64.EFI")
    (out / "http_url_name.txt").write_bytes(b"http://boot.example/file")

    (out / "emptyish.bin").write_bytes(b"\x00" * 16)
    (out / "randomish.bin").write_bytes(bytes(range(256)))

    print(f"wrote {len(list(out.iterdir()))} seeds under {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
