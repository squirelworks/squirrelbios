#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a tiny seed corpus for fuzz/usb_harness.c.

Original code. Synthetic USB device / configuration descriptors from
USB 2.0 chapter 9 and USB Mass Storage BBB 1.0. No vendor captures.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def device_desc(vid: int = 0x1234, pid: int = 0x5678, mps0: int = 64) -> bytes:
    # bLength, bDescriptorType=1, bcdUSB, class/sub/proto, bMaxPacketSize0,
    # idVendor, idProduct, bcdDevice, iManufacturer, iProduct, iSerial, bNumConfigurations
    return bytes(
        [
            18,
            1,
            0x00,
            0x02,
            0,
            0,
            0,
            mps0,
            vid & 0xFF,
            vid >> 8,
            pid & 0xFF,
            pid >> 8,
            0x00,
            0x01,
            0,
            0,
            0,
            1,
        ]
    )


def config_bot(ep_out: int = 1, ep_in: int = 2, mps: int = 512) -> bytes:
    # Config (9) + Interface BBB (9) + EP OUT (7) + EP IN (7)
    iface = bytes([9, 4, 0, 0, 2, 8, 6, 0x50, 0])
    ep_o = bytes([7, 5, ep_out & 0x0F, 2, mps & 0xFF, (mps >> 8) & 0x07, 0])
    ep_i = bytes([7, 5, 0x80 | (ep_in & 0x0F), 2, mps & 0xFF, (mps >> 8) & 0x07, 0])
    total = 9 + len(iface) + len(ep_o) + len(ep_i)
    cfg = bytes([9, 2, total & 0xFF, total >> 8, 1, 1, 0, 0x80, 50])
    return cfg + iface + ep_o + ep_i


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--out", type=Path, default=Path("fuzz/corpus-usb"))
    args = ap.parse_args()
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    (out / "device_std.bin").write_bytes(device_desc())
    (out / "device_short.bin").write_bytes(device_desc()[:8])
    (out / "config_bot.bin").write_bytes(config_bot())
    (out / "config_bot_mps64.bin").write_bytes(config_bot(mps=64))

    # HID-looking interface (should not match BOT)
    hid = bytes([9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 1, 1, 0])
    (out / "config_hid.bin").write_bytes(hid)

    # Bad lengths / types
    (out / "bad_type.bin").write_bytes(bytes([18, 99]) + b"\x00" * 16)
    (out / "zero_len.bin").write_bytes(bytes([0, 2]) + b"\x00" * 20)
    (out / "emptyish.bin").write_bytes(b"\x00" * 8)
    (out / "randomish.bin").write_bytes(bytes(range(128)))

    # Truncated BOT config
    (out / "config_trunc.bin").write_bytes(config_bot()[:12])

    print(f"wrote {len(list(out.iterdir()))} seeds under {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
