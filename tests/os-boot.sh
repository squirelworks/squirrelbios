#!/bin/sh
# Boot a Squirrel-OS/2 hard-disk image on the x86 firmware and check the
# shell banner. Not part of make test: the image is built by that repo
# (tools/build.sh writes build/os.img) and is not stored here.
#
#   tests/os-boot.sh path/to/os.img
#   make os-boot OS_IMG=path/to/os.img
#
# The firmware line is on COM1. The OS writes its banner and the "> "
# prompt to the VGA text buffer, so this reads that buffer back. Passes
# when both are on screen. OS_TIMEOUT (seconds, default 30) bounds the wait.
# The serial log and a text copy of the screen are kept next to each other
# in build/os-boot/.
#
# By hand, with the screen instead of this check:
#   qemu-system-i386 -bios x86/squirrel-bios.rom \
#       -drive format=raw,file=os.img,if=ide -serial stdio

set -eu

img=${1:-${OS_IMG:-}}
if [ -z "$img" ] || [ ! -f "$img" ]; then
    echo "usage: tests/os-boot.sh path/to/os.img" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU_X86:-qemu-system-i386}
TIMEOUT=${OS_TIMEOUT:-30}
ROM=$root/x86/squirrel-bios.rom
work=$root/build/os-boot
mkdir -p "$work"
log=$work/serial.log
vga=$work/vga.bin
screen=$work/screen.txt
sock=$work/mon.sock
rm -f "$log" "$vga" "$screen" "$sock"

"$QEMU" -bios "$ROM" \
    -drive format=raw,file="$img",if=ide \
    -display none -serial "file:$log" \
    -monitor "unix:$sock,server,nowait" \
    -no-reboot >/dev/null 2>&1 &
qemu=$!
trap 'kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null || true' EXIT INT TERM

python3 - "$sock" "$vga" "$screen" "$log" "$TIMEOUT" << 'PY'
import socket, sys, time

sock_path, vga_path, screen_path, log_path, timeout = sys.argv[1:]
timeout = float(timeout)
deadline = time.time() + timeout

def rows_of(blob):
    chars = bytes(blob[i] for i in range(0, len(blob), 2))
    return [chars[i:i + 80].decode("latin1").rstrip() for i in range(0, len(chars), 80)]

def dump():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect(sock_path)
    # Drop the monitor greeting. Readline echoes the command back.
    time.sleep(0.1)
    try:
        s.recv(4096)
    except socket.timeout:
        pass
    s.sendall(('memsave 0xb8000 4000 "%s"\n' % vga_path).encode())
    time.sleep(0.2)
    s.settimeout(2)
    buf = b""
    try:
        while b"(qemu)" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    s.close()
    if b"invalid" in buf.lower() or b"error" in buf.lower() and b"(qemu)" not in buf:
        raise SystemExit("monitor rejected memsave: %r" % buf[-300:])

def screen_text():
    try:
        blob = open(vga_path, "rb").read()
    except OSError:
        return ""
    if len(blob) < 4000:
        return ""
    rows = rows_of(blob[:4000])
    return "\n".join(rows).rstrip() + "\n"

# The monitor socket comes up with QEMU. Retry the connect briefly.
connected = False
while time.time() < deadline:
    try:
        dump()
        connected = True
        break
    except (ConnectionRefusedError, FileNotFoundError):
        time.sleep(0.1)
if not connected:
    raise SystemExit("QEMU monitor did not come up")

text = ""
while time.time() < deadline:
    dump()
    text = screen_text()
    rows = text.split("\n")
    banner = "Squirrel-OS/2" in text
    prompt = any(row.startswith("> ") or row == ">" for row in rows)
    serial = ""
    try:
        serial = open(log_path, "r", errors="replace").read()
    except OSError:
        serial = ""
    if banner and prompt and "kernel loaded at 0x00008000, jumping" in serial:
        open(screen_path, "w").write(text)
        sys.stdout.write(text)
        print("PASS os-boot")
        raise SystemExit(0)
    time.sleep(0.5)

open(screen_path, "w").write(text)
sys.stdout.write(text)
print("FAIL os-boot", file=sys.stderr)
try:
    sys.stderr.write(open(log_path, errors="replace").read())
except OSError:
    pass
raise SystemExit(1)
PY
