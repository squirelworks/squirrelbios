#!/usr/bin/env python3
"""Fill the blob after the SQBLOBMK marker in an ELF file with the
xorshift32 stream from seed 1 (tests/x86_64-bigload.c). Original code.

  tests/fillblob.py FILE SIZE
"""
import sys

path, size = sys.argv[1], int(sys.argv[2], 0)
data = bytearray(open(path, 'rb').read())
at = data.find(b'SQBLOBMK')
if at < 0 or data.find(b'SQBLOBMK', at + 1) >= 0:
    sys.exit('fillblob: want exactly one SQBLOBMK in ' + path)
x = 1
out = bytearray(size - 8)
for i in range(size - 8):
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    out[i] = x & 0xFF
data[at + 8:at + size] = out
open(path, 'wb').write(data)
