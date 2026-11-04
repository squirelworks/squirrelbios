#!/bin/sh
# Boot a real Linux x86-64 bzImage on the x86 firmware.
#
#   make linux-x86        run the check (not part of make test)
#
# Nothing here is checked into the repo. The script downloads one pinned
# Ubuntu 24.04 package (the amd64 generic kernel, from the noble release
# pocket), checks its SHA-256, and keeps it in build/linux-x86/. The
# vmlinuz in it is written raw to a disk image, which the firmware reads
# over ATA PIO, copies to 1 MB and enters through the 32-bit boot
# protocol. There is no initrd, so the kernel cannot reach a shell yet.
#
# The check passes when the serial log shows, in any order:
#   1. the kernel's banner (it was decompressed and is running),
#   2. the command line the firmware put in boot_params,
#   3. the E820 map the firmware passed, with usable RAM at 1 MB,
#   4. the kernel's memory summary (it accepted that map).
#
# LINUX_TIMEOUT (seconds, default 300) bounds the run, and LINUX_MEM
# (default 512M) sets the guest RAM. The log is build/linux-x86/linux-x86.log.

set -eu

QEMU_X86=${QEMU_X86:-qemu-system-x86_64}
FIRMWARE=${FIRMWARE:-x86/squirrel-bios.rom}
WORK=${LINUX_WORK:-build/linux-x86}
TIMEOUT=${LINUX_TIMEOUT:-300}
MEM=${LINUX_MEM:-512M}
MIRROR=${UBUNTU_ARCHIVE:-http://archive.ubuntu.com/ubuntu}
OLD_MIRROR=${UBUNTU_OLD_RELEASES:-http://old-releases.ubuntu.com/ubuntu}

# noble (24.04) release pocket; SHA-256 from its Packages index
KERNEL_VER=6.8.0-31-generic
KERNEL_DEB=pool/main/l/linux/linux-image-unsigned-6.8.0-31-generic_6.8.0-31.31_amd64.deb
KERNEL_SHA=35115ceb5ddc1e73034a73d6b064b9c6d38c534f20279fd7229c510515b931d8

die() { echo "linux-x86: $*" >&2; exit 1; }

[ -f "$FIRMWARE" ] || die "$FIRMWARE is missing; run make x86 first"
command -v "$QEMU_X86" >/dev/null || die "$QEMU_X86 not found"
mkdir -p "$WORK"

deb=$WORK/$(basename "$KERNEL_DEB")
if [ ! -f "$deb" ]; then
    echo "linux-x86: downloading $(basename "$KERNEL_DEB")"
    curl -fsSL -o "$deb.part" "$MIRROR/$KERNEL_DEB" ||
        curl -fsSL -o "$deb.part" "$OLD_MIRROR/$KERNEL_DEB" ||
        die "download of $KERNEL_DEB failed"
    mv "$deb.part" "$deb"
fi
echo "$KERNEL_SHA  $deb" | sha256sum -c --quiet - || {
    rm -f "$deb"
    die "$(basename "$deb") does not match its pinned SHA-256"
}

img=$WORK/vmlinuz-$KERNEL_VER.img
if [ ! -f "$img" ]; then
    rm -rf "$WORK/deb"
    mkdir -p "$WORK/deb"
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$deb" "$WORK/deb"
    else
        (cd "$WORK/deb" && ar x "$OLDPWD/$deb" && tar xf data.tar.* &&
            rm -f data.tar.* control.tar.* debian-binary)
    fi
    cp "$WORK/deb/boot/vmlinuz-$KERNEL_VER" "$img.part"
    rm -rf "$WORK/deb"
    mv "$img.part" "$img"
fi

log=$WORK/linux-x86.log
: > "$log"
"$QEMU_X86" -bios "$FIRMWARE" -m "$MEM" -no-reboot \
    -drive format=raw,file="$img",if=ide \
    -display none -monitor none -serial "file:$log" >/dev/null 2>&1 &
qemu=$!

want() {
    grep -qF "Linux version $KERNEL_VER" "$log" &&
        grep -qF "Command line: console=ttyS0,115200" "$log" &&
        grep -qE "BIOS-e820: \[mem 0x0+100000-0x[0-9a-f]+\] usable" "$log" &&
        grep -qE "Memory: [0-9]+K/[0-9]+K available" "$log"
}

ok=0
ticks=$((TIMEOUT * 2))
while [ "$ticks" -gt 0 ]; do
    if want; then ok=1; break; fi
    case $(ps -o stat= -p "$qemu" 2>/dev/null) in ""|Z*) break ;; esac
    sleep 0.5
    ticks=$((ticks - 1))
done
kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true
want && ok=1

if [ "$ok" = 1 ]; then
    echo "PASS  linux-x86 ($KERNEL_VER started with the firmware's E820 map)"
else
    echo "FAIL  linux-x86 (see $log)"
    tail -n 20 "$log" | sed 's/^/      | /'
    exit 1
fi
