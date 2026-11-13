#!/usr/bin/env python3
"""Put a kernel into the MIPS Malta firmware's flash image. Original code.

    mips/mkimage.py FIRMWARE KERNEL OUT [--load ADDR] [--entry ADDR] [--cmdline TEXT]

QEMU's -kernel on Malta replaces the boot flash's contents, so the
firmware (mips/squirrel-bios.S) finds its kernel in the flash instead:
at 1 MB, "SQKN", the kernel's size, load address and entry point (big-
endian), the command line (256 bytes, NUL-terminated), and the kernel
itself at 1 MB + 0x200. KERNEL is raw bytes to be copied to the load
address (default 0x80100000, a kseg0 address); the entry point defaults
to the load address. The result is padded to 4 MB, the size of the flash.
"""
import argparse
import struct

PAYLOAD = 0x100000
KERNEL = 0x200
FLASH = 0x400000

ap = argparse.ArgumentParser()
ap.add_argument('firmware')
ap.add_argument('kernel')
ap.add_argument('out')
ap.add_argument('--load', type=lambda v: int(v, 0), default=0x80100000)
ap.add_argument('--entry', type=lambda v: int(v, 0))
ap.add_argument('--cmdline', default='')
a = ap.parse_args()

fw = open(a.firmware, 'rb').read()
kernel = open(a.kernel, 'rb').read()
cmd = a.cmdline.encode()
if len(fw) > PAYLOAD:
    raise SystemExit('the firmware is larger than 1 MB')
if len(cmd) > 255:
    raise SystemExit('the command line is longer than 255 bytes')
if PAYLOAD + KERNEL + len(kernel) > FLASH:
    raise SystemExit('the kernel does not fit in the 4 MB flash')
entry = a.load if a.entry is None else a.entry
head = b'SQKN' + struct.pack('>III', len(kernel), a.load, entry) + cmd.ljust(256, b'\0')
img = bytearray(b'\xff' * FLASH)
img[:len(fw)] = fw
img[PAYLOAD:PAYLOAD + len(head)] = head
img[PAYLOAD + KERNEL:PAYLOAD + KERNEL + len(kernel)] = kernel
open(a.out, 'wb').write(img)
