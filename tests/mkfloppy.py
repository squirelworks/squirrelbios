#!/usr/bin/env python3
"""Build a 1.44 MB diskette image for the floppy smoke tests. Original code.

  tests/mkfloppy.py BOOT.bin OUT.img [--nosig] [--hdd]

Sector 0 (C0 H0 S1) is BOOT.bin (512 bytes). Every other sector starts
with b'LBA:' and its LBA as a little-endian word, so a test can tell which
sector it read. --nosig clears the 0xAA55 signature of sector 0. --hdd
makes a 2016-sector hard disk image instead (2 cylinders, 16 heads, 63
sectors), for El Torito hard disk emulation.
"""
import struct
import sys

SECTORS = 80 * 2 * 18
if '--hdd' in sys.argv[3:]:
    SECTORS = 2 * 16 * 63

boot = open(sys.argv[1], 'rb').read()
assert len(boot) == 512, 'boot sector must be 512 bytes'
img = bytearray(SECTORS * 512)
img[0:512] = boot
if '--nosig' in sys.argv[3:]:
    img[510:512] = b'\0\0'
for lba in range(1, SECTORS):
    img[lba * 512:lba * 512 + 6] = b'LBA:' + struct.pack('<H', lba)
open(sys.argv[2], 'wb').write(img)
