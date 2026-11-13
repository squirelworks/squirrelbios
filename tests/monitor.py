#!/usr/bin/env python3
# Drive a QEMU HMP monitor socket for a smoke case. Original code.
#   monitor.py SOCKET LOG TRIGGER COMMAND...
# Waits (up to 60 s) for TRIGGER to appear in the serial LOG, then sends
# each COMMAND to the monitor at SOCKET, half a second apart.
import socket
import sys
import time

sock_path, log, trigger = sys.argv[1:4]
commands = sys.argv[4:]
deadline = time.time() + 60
while time.time() < deadline:
    try:
        with open(log, 'rb') as f:
            if trigger.encode() in f.read():
                break
    except OSError:
        pass
    time.sleep(0.1)
else:
    sys.exit(1)

s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit(1)
time.sleep(0.3)
for c in commands:
    s.sendall(c.encode() + b'\n')
    time.sleep(0.5)
s.close()
