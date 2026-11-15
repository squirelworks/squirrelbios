#!/usr/bin/env python3
# The far end of the UEFI firmware's console mirror, for the smoke tests.
# Original code.
#
# Usage: solcon.py PORT LOG TRIGGER KEYS
#
# Listens for UDP on 127.0.0.1:PORT (QEMU's user network shows it to the
# guest as 10.0.2.2:PORT) and appends every datagram to LOG. Once the
# text received so far contains TRIGGER, it sends the bytes of the file
# KEYS back to whoever sent it, one datagram, and keeps logging. Exits on
# SIGTERM or after 60 s without a datagram.
import socket
import sys

port, log, trigger, keys = int(sys.argv[1]), sys.argv[2], sys.argv[3].encode(), sys.argv[4]
with open(keys, 'rb') as f:
    reply = f.read()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
s.settimeout(60)
seen = b''
sent = False
with open(log, 'ab', buffering=0) as out:
    while True:
        try:
            data, peer = s.recvfrom(4096)
        except socket.timeout:
            break
        out.write(data)
        seen += data
        if not sent and trigger in seen:
            s.sendto(reply, peer)
            sent = True
