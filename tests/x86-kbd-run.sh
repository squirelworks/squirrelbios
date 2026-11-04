#!/bin/sh
# Boot x86-kbd under QEMU, inject the 'a' key via QMP, and check the
# serial log for kbd=ok. Invoked from tests/smoke.sh.
#
# KBD_NAME names the case (and its log), KBD_ARGS adds QEMU arguments
# and KBD_EXPECT adds lines the log must hold. QEMU sends the key to the
# keyboard added last: with a usb-kbd in KBD_ARGS, that one. KBD_KEYS
# sends that many 'a' keys, 100 ms apart, for an image built to wait for
# them (KBD_IMG).
set -u

TEST_DIR=${TEST_DIR:-build/tests}
TIMEOUT=${SMOKE_TIMEOUT:-10}
QEMU_X86=${QEMU_X86:-qemu-system-i386}
ROM=${ROM:-x86/squirrel-bios.rom}
IMG=${KBD_IMG:-$TEST_DIR/x86-kbd.bin}
KEYS=${KBD_KEYS:-1}
NAME=${KBD_NAME:-x86-kbd}
LOG=$TEST_DIR/$NAME.log
QMP=$TEST_DIR/$NAME.qmp
expect="SquirrelBIOS x86|${KBD_EXPECT:+$KBD_EXPECT|}OS-ENTRY x86 kbd=ok"

rm -f "$LOG" "$QMP"
: > "$LOG"

"$QEMU_X86" -display none -monitor none -serial "file:$LOG" \
    -qmp "unix:$QMP,server,nowait" \
    -bios "$ROM" \
    -drive format=raw,file="$IMG",if=ide ${KBD_ARGS:-} \
    >/dev/null 2>&1 &
qemu=$!

ticks=$((TIMEOUT * 10))
sent=0
ok=0
status=
while [ "$ticks" -gt 0 ]; do
    case $(ps -o stat= -p "$qemu" 2>/dev/null) in
    ""|Z*) wait "$qemu" 2>/dev/null; status=$? ;;
    esac
    if [ "$sent" -eq 0 ] && grep -qF 'OS-ENTRY' "$LOG" 2>/dev/null; then
        if [ -S "$QMP" ]; then
            python3 - "$QMP" "$KEYS" <<'PY'
import json, socket, sys, time
path = sys.argv[1]
keys = int(sys.argv[2])
deadline = time.time() + 2
s = None
while time.time() < deadline:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(path)
        break
    except OSError:
        time.sleep(0.05)
if s is None:
    sys.exit(1)
s.settimeout(2)

def one():
    data = b""
    while b"\n" not in data:
        data += s.recv(4096)
    return data

one()  # greeting
s.sendall(b'{"execute":"qmp_capabilities"}\n')
one()
ev = {
    "execute": "input-send-event",
    "arguments": {
        "events": [
            {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "a"}}},
            {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "a"}}},
        ]
    },
}
for _ in range(keys):
    s.sendall((json.dumps(ev) + "\n").encode())
    one()
    time.sleep(0.1)
s.close()
PY
            sent=1
        fi
    fi
    ok=1
    old_ifs=$IFS
    IFS='|'
    for want in $expect; do
        grep -qF -- "$want" "$LOG" || ok=0
    done
    IFS=$old_ifs
    [ "$ok" -eq 1 ] && break
    [ -n "$status" ] && break
    sleep 0.1
    ticks=$((ticks - 1))
done
if [ -z "$status" ]; then
    kill "$qemu" 2>/dev/null
    wait "$qemu" 2>/dev/null
fi

if [ "$ok" -eq 1 ]; then
    echo "PASS  $NAME"
    exit 0
fi
echo "FAIL  $NAME (expected: $expect)"
sed 's/^/      | /' "$LOG"
exit 1
