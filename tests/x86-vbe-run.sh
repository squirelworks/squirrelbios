#!/bin/sh
# Boot a VBE test image under QEMU, wait for vbe=ok, then take a
# screendump through QMP and check what QEMU shows. Invoked from
# tests/smoke.sh.
#
# VBE_NAME names the case (and its log), VBE_IMG the image, VBE_ARGS adds
# QEMU arguments, and VBE_SCREEN is what the screen must be: its width
# and height, then any number of x,y=rrggbb pixels, all space-separated.
set -u

TEST_DIR=${TEST_DIR:-build/tests}
TIMEOUT=${SMOKE_TIMEOUT:-10}
QEMU_X86=${QEMU_X86:-qemu-system-i386}
ROM=${ROM:-x86/squirrel-bios.rom}
NAME=${VBE_NAME:-x86-vbe}
IMG=${VBE_IMG:-$TEST_DIR/$NAME.bin}
LOG=$TEST_DIR/$NAME.log
QMP=$TEST_DIR/$NAME.qmp
PPM=$TEST_DIR/$NAME.ppm

rm -f "$LOG" "$QMP" "$PPM"
: > "$LOG"

"$QEMU_X86" -display none -monitor none -serial "file:$LOG" \
    -qmp "unix:$QMP,server,nowait" -bios "$ROM" \
    -drive format=raw,file="$IMG",if=ide ${VBE_ARGS:-} \
    >/dev/null 2>&1 &
qemu=$!

ticks=$((TIMEOUT * 10))
while [ "$ticks" -gt 0 ] && ! grep -qF 'vbe=' "$LOG" 2>/dev/null; do
    sleep 0.1
    ticks=$((ticks - 1))
done

result=FAIL
why="no vbe=ok"
if grep -qF 'vbe=ok' "$LOG"; then
    why=$(python3 - "$QMP" "$PPM" ${VBE_SCREEN:-} <<'PY'
import json, os, socket, sys, time
qmp, ppm, want = sys.argv[1], sys.argv[2], sys.argv[3:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(qmp)
f = s.makefile("rw")
f.readline()
for cmd in ({"execute": "qmp_capabilities"},
            {"execute": "screendump", "arguments": {"filename": os.path.abspath(ppm)}}):
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    while "return" not in (r := json.loads(f.readline())) and "error" not in r:
        pass
    if "error" in r:
        print(r["error"].get("desc", "QMP error"))
        sys.exit(0)
data = open(ppm, "rb").read()
magic, size, depth, pixels = data.split(b"\n", 3)
w, h = map(int, size.split())
if [w, h] != [int(want[0]), int(want[1])]:
    print("screen is %dx%d" % (w, h))
    sys.exit(0)
for spec in want[2:]:
    xy, rgb = spec.split("=")
    x, y = map(int, xy.split(","))
    got = pixels[(y * w + x) * 3:(y * w + x) * 3 + 3].hex()
    if got != rgb:
        print("pixel %d,%d is %s" % (x, y, got))
        sys.exit(0)
print("ok")
PY
)
    [ "$why" = ok ] && result=PASS
fi
kill "$qemu" 2>/dev/null
wait "$qemu" 2>/dev/null
rm -f "$QMP"
if [ "$result" = PASS ]; then
    echo "PASS  $NAME"
    exit 0
fi
echo "FAIL  $NAME ($why)"
exit 1
