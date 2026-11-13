#!/bin/sh
# Boot a real Linux arm64 kernel through its EFI stub on the AArch64
# UEFI-class firmware (aarch64-uefi), all the way to a shell.
#
#   make linux-arm                   run the check (not part of make test)
#   tests/arm-linux.sh --interactive same image, but you get the shell
#
# Nothing here is checked into the repo. The script downloads two pinned
# Ubuntu 24.04 arm64 packages (the generic kernel and busybox-static),
# checks their SHA-256, and keeps them in build/linux-arm/. It builds a
# tiny busybox initramfs and starts QEMU virt with -bios
# arm/uefi/squirrel-bios.fd -kernel Image -initrd ... -append ..., which
# the firmware reads from fw_cfg: the Image runs as an EFI application,
# the command line arrives as its LoadOptions and the initrd through the
# Linux initrd LoadFile2 device path.
#
# The check passes when, in order:
#   1. the firmware starts the kernel's EFI stub and it calls
#      ExitBootServices,
#   2. the kernel reaches the shell prompt and a typed command runs,
#   3. poweroff makes QEMU exit by itself (PSCI SYSTEM_OFF, status 0).
#
# LINUX_TIMEOUT (seconds, default 300), LINUX_SMP (default 1), LINUX_MEM
# (default 512M), LINUX_MACHINE (default virt; e.g. virt,gic-version=3 or
# virt,virtualization=on to enter at EL2) and LINUX_CPU (default
# cortex-a57) set up the machine. The serial log is kept in
# build/linux-arm/linux-arm.log.

set -eu

QEMU_ARM=${QEMU_ARM:-qemu-system-aarch64}
TIMEOUT=${LINUX_TIMEOUT:-300}
SMP=${LINUX_SMP:-1}
MEM=${LINUX_MEM:-512M}
MACHINE=${LINUX_MACHINE:-virt}
CPU=${LINUX_CPU:-cortex-a57}
FIRMWARE=arm/uefi/squirrel-bios.fd
WORK=${LINUX_WORK:-build/linux-arm}
MIRROR=${UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports}
OLD_MIRROR=${UBUNTU_OLD_RELEASES:-http://old-releases.ubuntu.com/ubuntu}

# noble (24.04) release pocket; SHA-256 from its Packages index
KERNEL_VER=6.8.0-31-generic
KERNEL_DEB=pool/main/l/linux/linux-image-unsigned-6.8.0-31-generic_6.8.0-31.31_arm64.deb
KERNEL_SHA=370c09d374978f7bd6b157adc8dfae9b4498d9a4ae24aa8068000f0c61a0273e
BUSYBOX_DEB=pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_arm64.deb
BUSYBOX_SHA=1a8c03948f99dfbb90efd93d9214418b416ac0737ed4c90938ebbcd645f10682

interactive=0
[ "${1:-}" = "--interactive" ] && interactive=1

die() { echo "linux-arm: $*" >&2; exit 1; }

[ -f "$FIRMWARE" ] || die "$FIRMWARE is missing; run make arm-uefi first"
command -v "$QEMU_ARM" >/dev/null || die "$QEMU_ARM not found"
command -v cpio >/dev/null || die "cpio not found"
mkdir -p "$WORK"

fetch() {
    file=$WORK/$(basename "$1")
    if [ ! -f "$file" ]; then
        echo "linux-arm: downloading $(basename "$1")"
        curl -fsSL -o "$file.part" "$MIRROR/$1" ||
            curl -fsSL -o "$file.part" "$OLD_MIRROR/$1" ||
            die "download of $1 failed"
        mv "$file.part" "$file"
    fi
    echo "$2  $file" | sha256sum -c --quiet - || {
        rm -f "$file"
        die "$(basename "$1") does not match its pinned SHA-256"
    }
}

unpack() {
    rm -rf "$2"
    mkdir -p "$2"
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$1" "$2"
    else
        (cd "$2" && ar x "$OLDPWD/$1" && tar xf data.tar.* && rm -f data.tar.* control.tar.* debian-binary)
    fi
}

fetch "$KERNEL_DEB" "$KERNEL_SHA"
fetch "$BUSYBOX_DEB" "$BUSYBOX_SHA"

# The arm64 vmlinuz is the gzipped Image, which is itself a PE32+ EFI
# application (the EFI stub). QEMU unpacks a gzipped -kernel by itself,
# but the firmware should see exactly what an ESP would hold, so unpack.
image=$WORK/Image-$KERNEL_VER
if [ ! -f "$image" ]; then
    unpack "$WORK/$(basename "$KERNEL_DEB")" "$WORK/kernel-deb"
    vmlinuz=$WORK/kernel-deb/boot/vmlinuz-$KERNEL_VER
    [ -f "$vmlinuz" ] || die "no vmlinuz-$KERNEL_VER in the kernel package"
    if gzip -t "$vmlinuz" 2>/dev/null; then
        gzip -dc "$vmlinuz" > "$image.part"
    else
        cp "$vmlinuz" "$image.part"
    fi
    mv "$image.part" "$image"
    rm -rf "$WORK/kernel-deb"
fi
[ "$(dd if="$image" bs=1 count=2 2>/dev/null)" = MZ ] || die "$image is not a PE (EFI stub) image"

root=$WORK/initramfs
unpack "$WORK/$(basename "$BUSYBOX_DEB")" "$WORK/busybox-deb"
rm -rf "$root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/tmp"
cp "$WORK/busybox-deb/usr/bin/busybox" "$root/bin/busybox"
rm -rf "$WORK/busybox-deb"
ln -s busybox "$root/bin/sh"
cat > "$root/init" <<'INIT'
#!/bin/sh
/bin/busybox mount -t devtmpfs devtmpfs /dev
exec 0</dev/console 1>/dev/console 2>&1
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "squirrel: busybox shell on $(uname -srm), $(grep -c ^processor /proc/cpuinfo) CPU(s), EFI $([ -d /sys/firmware/efi ] && echo yes || echo no)"
export PS1='squirrel# ' HOME=/ PATH=/bin
while true; do
    setsid cttyhack sh
done
INIT
chmod 755 "$root/init"
initrd=$WORK/initramfs.cpio.gz
(cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$initrd"

set -- "$QEMU_ARM" -M "$MACHINE" -cpu "$CPU" -m "$MEM" -smp "$SMP" -no-reboot \
    -bios "$FIRMWARE" -kernel "$image" -initrd "$initrd" \
    -append "console=ttyAMA0 earlycon panic=-1"

if [ "$interactive" -eq 1 ]; then
    echo "linux-arm: Ctrl-A X quits QEMU; poweroff shuts Linux down"
    exec "$@" -nographic
fi

log=$WORK/linux-arm.log
pipe=$WORK/serial
rm -f "$log" "$pipe.in" "$pipe.out"
mkfifo "$pipe.in" "$pipe.out"
"$@" -display none -monitor none \
    -chardev "pipe,id=con,path=$pipe" -serial chardev:con \
    >/dev/null 2>"$WORK/qemu.err" &
qemu=$!
cat "$pipe.out" > "$log" &
reader=$!
exec 3<>"$pipe.in"
cleanup() {
    exec 3>&-
    kill "$qemu" "$reader" 2>/dev/null || true
    rm -f "$pipe.in" "$pipe.out"
}
trap cleanup EXIT
deadline=$(( $(date +%s) + TIMEOUT ))

running() {
    case $(ps -o stat= -p "$qemu" 2>/dev/null) in
    ""|Z*) return 1 ;;
    esac
}

fail() {
    echo "FAIL  linux-arm ($1)"
    tail -n 40 "$log" | sed 's/^/      | /'
    [ -s "$WORK/qemu.err" ] && sed 's/^/      qemu: /' "$WORK/qemu.err"
    echo "      full log: $log"
    exit 1
}

wait_for() {
    while ! grep -qaF -- "$1" "$log"; do
        running || fail "QEMU exited before $2"
        [ "$(date +%s)" -lt "$deadline" ] || fail "no $2 within ${TIMEOUT}s"
        sleep 1
    done
    echo "ok    $2"
}

wait_for "efi: ExitBootServices" "EFI stub reaching ExitBootServices"
wait_for "squirrel# " "shell prompt"
grep -qa "EFI yes" "$log" || fail "Linux does not see EFI (/sys/firmware/efi)"
echo "ok    Linux booted through EFI"
printf 'echo linux-arm=$((40+2))\n' >&3
wait_for "linux-arm=42" "command at the prompt"
printf 'poweroff -f\n' >&3
while running; do
    [ "$(date +%s)" -lt "$deadline" ] || fail "QEMU still running after poweroff"
    sleep 1
done
wait "$qemu" && status=0 || status=$?
[ "$status" -eq 0 ] || fail "QEMU exited with status $status after poweroff"
echo "ok    poweroff (QEMU exited with status 0)"
echo "PASS  linux-arm"
