#!/usr/bin/env python3
# Build a small El Torito no-emulation CD image for the x86-cd smoke test.
# Original code.
#
# Usage: mkiso.py BOOT.bin OUT.iso
#
# Layout, in 2048-byte blocks (ISO 9660 and El Torito 1.0):
#   16  Primary Volume Descriptor (just enough to be one)
#   17  Boot Record Volume Descriptor -> catalog at block 19
#   18  Volume Descriptor Set Terminator
#   19  Boot catalog: validation entry, bootable no-emulation default
#       entry (load segment 0 = 07C0h, BOOT.bin's length in 512-byte
#       sectors, image at block 20)
#   20  BOOT.bin
#   other blocks up to 63 carry 'LBA:' and the block number
import struct
import sys

BLOCK = 2048
BLOCKS = 64
IMAGE = 20


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkiso.py BOOT.bin OUT.iso')
    boot = open(sys.argv[1], 'rb').read()
    sectors = (len(boot) + 511) // 512
    image_blocks = (len(boot) + BLOCK - 1) // BLOCK
    iso = bytearray(BLOCKS * BLOCK)

    def put(block, data, at=0):
        o = block * BLOCK + at
        iso[o:o + len(data)] = data

    for b in range(BLOCKS):
        if b < 16 or b >= IMAGE + image_blocks:
            put(b, b'LBA:' + struct.pack('<I', b))

    pvd = bytearray(BLOCK)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[40:72] = b'SQUIRRELBIOS_TEST'.ljust(32)
    pvd[80:88] = struct.pack('<I', BLOCKS) + struct.pack('>I', BLOCKS)
    pvd[120:124] = struct.pack('<H', 1) + struct.pack('>H', 1)
    pvd[124:128] = struct.pack('<H', 1) + struct.pack('>H', 1)
    pvd[128:132] = struct.pack('<H', BLOCK) + struct.pack('>H', BLOCK)
    pvd[881] = 1
    put(16, pvd)

    brvd = bytearray(BLOCK)
    brvd[0:7] = b'\x00CD001\x01'
    brvd[7:39] = b'EL TORITO SPECIFICATION'.ljust(32, b'\x00')
    brvd[0x47:0x4B] = struct.pack('<I', 19)
    put(17, brvd)

    put(18, b'\xffCD001\x01')

    val = bytearray(32)
    val[0] = 1                                  # header
    val[1] = 0                                  # platform: 80x86
    val[4:28] = b'SquirrelBIOS test'.ljust(24, b'\x00')
    val[30:32] = b'\x55\xaa'
    words = struct.unpack('<16H', bytes(val))
    val[28:30] = struct.pack('<H', (-sum(words)) & 0xFFFF)
    entry = bytearray(32)
    entry[0] = 0x88                             # bootable
    entry[1] = 0                                # no emulation
    entry[2:4] = struct.pack('<H', 0)           # load segment: default
    entry[6:8] = struct.pack('<H', sectors)
    entry[8:12] = struct.pack('<I', IMAGE)
    put(19, bytes(val) + bytes(entry))

    put(IMAGE, boot)
    open(sys.argv[2], 'wb').write(iso)


if __name__ == '__main__':
    main()
