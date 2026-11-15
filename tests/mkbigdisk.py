#!/usr/bin/env python3
# Build a large sparse raw disk for the x86-edd smoke test.
# Original code.
#
# Usage: mkbigdisk.py BOOT.bin OUT.img SIZE_GIB LBA...
#
# BOOT.bin goes at LBA 0 (it may span several sectors). Each LBA listed
# gets 'LBA:' and its LBA as a little-endian dword at the start of the
# sector. Everything else is a hole, so a 200 GiB disk takes a few KB.
import struct
import sys


def main():
    if len(sys.argv) < 4:
        sys.exit('usage: mkbigdisk.py BOOT.bin OUT.img SIZE_GIB LBA...')
    boot = open(sys.argv[1], 'rb').read()
    size = int(sys.argv[3]) << 30
    with open(sys.argv[2], 'wb') as f:
        f.truncate(size)
        f.write(boot)
        for arg in sys.argv[4:]:
            lba = int(arg, 0)
            assert (lba + 1) * 512 <= size, lba
            f.seek(lba * 512)
            f.write(b'LBA:' + struct.pack('<I', lba))


if __name__ == '__main__':
    main()
