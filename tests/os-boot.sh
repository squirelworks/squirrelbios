#!/bin/sh
# Boot a Squirrel-OS/2 hard-disk image on the x86 firmware and check the
# shell banner. Not part of make test: the image is built by that repo
# (tools/build.sh writes build/os.img) and is not stored here.
#
# Pinned partner (Phase G M6 / os-boot-sync): tests/os-boot.pin names the
# Squirrel-OS/2 release tag, tip SHA, and os.img SHA-256 that this script
# expects. HANDOFF.md cites the same tag. Mismatch failures name both.
#
#   tests/os-boot.sh path/to/os.img
#   tests/os-boot.sh --check-pin          # host smoke: pin + mismatch text
#   tests/os-boot.sh --fetch              # download the pinned os.img, then boot
#   make os-boot OS_IMG=path/to/os.img
#   make os-boot-csm OS_IMG=path/to/os.img   (the x86_64-uefi image's CSM)
#
# OS_BIOS picks another firmware image (default x86/squirrel-bios.rom),
# QEMU_X86 another QEMU, OS_DRIVE=floppy boots the image as A: and
# OS_DRIVE=cdrom as an IDE CD (Squirrel-OS/2's cd.iso).
# OS_SKIP_PIN=1 skips the SHA-256 check (local tip / untagged builds).
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

root=$(cd "$(dirname "$0")/.." && pwd)
pin_file=$root/tests/os-boot.pin

die() { echo "os-boot: $*" >&2; exit 1; }

load_pin() {
    OS2_REPO=
    OS2_TAG=
    OS2_TIP=
    OS_IMG_NAME=
    OS_IMG_SHA256=
    OS_IMG_URL=
    UEFI_IMG_NAME=
    UEFI_IMG_SHA256=
    UEFI_IMG_URL=
    [ -f "$pin_file" ] || die "missing pin file $pin_file"
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in
        ''|'#'*) continue ;;
        esac
        key=${line%%=*}
        val=${line#*=}
        case $key in
        OS2_REPO) OS2_REPO=$val ;;
        OS2_TAG) OS2_TAG=$val ;;
        OS2_TIP) OS2_TIP=$val ;;
        OS_IMG_NAME) OS_IMG_NAME=$val ;;
        OS_IMG_SHA256) OS_IMG_SHA256=$val ;;
        OS_IMG_URL) OS_IMG_URL=$val ;;
        UEFI_IMG_NAME) UEFI_IMG_NAME=$val ;;
        UEFI_IMG_SHA256) UEFI_IMG_SHA256=$val ;;
        UEFI_IMG_URL) UEFI_IMG_URL=$val ;;
        *) die "unknown pin key '$key' in $pin_file" ;;
        esac
    done < "$pin_file"
    [ -n "$OS2_TAG" ] || die "pin missing OS2_TAG"
    [ -n "$OS2_TIP" ] || die "pin missing OS2_TIP"
    [ -n "$OS_IMG_SHA256" ] || die "pin missing OS_IMG_SHA256"
    [ -n "$OS_IMG_URL" ] || die "pin missing OS_IMG_URL"
    [ -n "$OS_IMG_NAME" ] || OS_IMG_NAME=os.img
}

sha256_of() {
    # Portable: sha256sum (GNU) or shasum -a 256 (BSD).
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "need sha256sum or shasum to verify the pinned image"
    fi
}

# verify_img PATH EXPECTED_SHA LABEL — fail naming the pin mismatch.
verify_img() {
    path=$1
    want=$2
    label=$3
    [ -f "$path" ] || die "$path is missing"
    got=$(sha256_of "$path")
    if [ "$got" != "$want" ]; then
        echo "os-boot: SHA-256 mismatch for $label" >&2
        echo "os-boot: expected $want (Squirrel-OS/2 $OS2_TAG tip $OS2_TIP)" >&2
        echo "os-boot: got      $got ($path)" >&2
        echo "os-boot: pin file $pin_file; set OS_SKIP_PIN=1 to bypass for a local tip build" >&2
        exit 1
    fi
}

check_pin() {
    load_pin
    # Required fields already checked by load_pin.
    [ ${#OS_IMG_SHA256} -eq 64 ] || die "OS_IMG_SHA256 must be 64 hex digits"
    case $OS_IMG_SHA256 in
    *[!0-9a-fA-F]*) die "OS_IMG_SHA256 must be hex" ;;
    esac
    [ ${#OS2_TIP} -eq 40 ] || die "OS2_TIP must be a 40-char commit SHA"
    echo "os-boot-sync: pin ok tag=$OS2_TAG tip=$OS2_TIP img=$OS_IMG_NAME"

    # Failure message must name the mismatch (expected tag/sha vs got).
    work=$root/build/os-boot
    mkdir -p "$work"
    bogus=$work/pin-mismatch.bin
    printf 'not-the-pinned-os-img\n' > "$bogus"
    rc=0
    msg=$(verify_img "$bogus" "$OS_IMG_SHA256" "$OS_IMG_NAME" 2>&1) || rc=$?
    [ "$rc" -ne 0 ] || die "internal: expected SHA-256 mismatch for bogus file"
    echo "$msg" | grep -q "SHA-256 mismatch" || die "mismatch text missing 'SHA-256 mismatch'"
    echo "$msg" | grep -q "$OS2_TAG" || die "mismatch text missing OS2_TAG $OS2_TAG"
    echo "$msg" | grep -q "$OS_IMG_SHA256" || die "mismatch text missing expected SHA"
    echo "$msg" | grep -q "got" || die "mismatch text missing got line"
    rm -f "$bogus"
    echo "os-boot-sync: mismatch message names tag=$OS2_TAG and expected SHA"
    echo "PASS os-boot-sync"
}

fetch_pinned() {
    load_pin
    work=$root/build/os-boot
    mkdir -p "$work"
    dest=$work/$OS_IMG_NAME
    if [ -f "$dest" ]; then
        got=$(sha256_of "$dest")
        if [ "$got" = "$OS_IMG_SHA256" ]; then
            echo "os-boot: using cached $dest (Squirrel-OS/2 $OS2_TAG)" >&2
            echo "$dest"
            return 0
        fi
        echo "os-boot: cached $dest does not match pin; re-fetching" >&2
        rm -f "$dest"
    fi
    command -v curl >/dev/null 2>&1 || die "curl required to fetch $OS_IMG_URL"
    echo "os-boot: fetching Squirrel-OS/2 $OS2_TAG $OS_IMG_NAME" >&2
    curl -fsSL -o "$dest.part" "$OS_IMG_URL" || die "download of $OS_IMG_URL failed"
    mv "$dest.part" "$dest"
    verify_img "$dest" "$OS_IMG_SHA256" "$OS_IMG_NAME"
    echo "$dest"
}

case ${1:-} in
--check-pin)
    check_pin
    exit 0
    ;;
--fetch)
    img=$(fetch_pinned)
    shift
    ;;
*)
    img=${1:-${OS_IMG:-}}
    if [ -z "$img" ]; then
        echo "usage: tests/os-boot.sh path/to/os.img" >&2
        echo "       tests/os-boot.sh --fetch" >&2
        echo "       tests/os-boot.sh --check-pin" >&2
        echo "pinned: see tests/os-boot.pin (Squirrel-OS/2 tag)" >&2
        exit 2
    fi
    [ -f "$img" ] || die "usage: tests/os-boot.sh path/to/os.img (missing: $img)"
    ;;
esac

load_pin
if [ "${OS_SKIP_PIN:-0}" != 1 ]; then
    verify_img "$img" "$OS_IMG_SHA256" "$OS_IMG_NAME"
else
    echo "os-boot: OS_SKIP_PIN=1; not checking SHA-256 (pin is Squirrel-OS/2 $OS2_TAG)" >&2
fi

QEMU=${QEMU_X86:-qemu-system-i386}
TIMEOUT=${OS_TIMEOUT:-30}
ROM=${OS_BIOS:-$root/x86/squirrel-bios.rom}
DRIVE=${OS_DRIVE:-ide}
case $DRIVE in
cdrom) drive_opts="if=ide,media=cdrom" ;;
*) drive_opts="if=$DRIVE,snapshot=on" ;;
esac
work=$root/build/os-boot
mkdir -p "$work"
log=$work/serial.log
vga=$work/vga.bin
screen=$work/screen.txt
sock=$work/mon.sock
rm -f "$log" "$vga" "$screen" "$sock"

"$QEMU" -bios "$ROM" \
    -drive format=raw,file="$img",$drive_opts \
    -display none -serial "file:$log" \
    -monitor "unix:$sock,server,nowait" \
    -no-reboot >/dev/null 2>&1 &
qemu=$!
trap 'kill "$qemu" 2>/dev/null; wait "$qemu" 2>/dev/null || true' EXIT INT TERM

python3 - "$sock" "$vga" "$screen" "$log" "$TIMEOUT" "$DRIVE" "$OS2_TAG" << 'PY'
import socket, sys, time

sock_path, vga_path, screen_path, log_path, timeout, drive, os2_tag = sys.argv[1:]
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
    # A hard disk is loaded by the firmware (S2OK); a floppy by its own boot sector
    loaded = {"floppy": "booting from floppy A:",
              "cdrom": "booting from CD-ROM"}.get(drive, "kernel loaded at 0x00008000, jumping")
    if banner and prompt and loaded in serial:
        open(screen_path, "w").write(text)
        sys.stdout.write(text)
        print("PASS os-boot (pinned %s)" % os2_tag)
        raise SystemExit(0)
    time.sleep(0.5)

open(screen_path, "w").write(text)
sys.stdout.write(text)
print("FAIL os-boot (pinned %s)" % os2_tag, file=sys.stderr)
try:
    sys.stderr.write(open(log_path, errors="replace").read())
except OSError:
    pass
raise SystemExit(1)
PY
