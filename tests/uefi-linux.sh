#!/bin/sh
# Boot a real Linux kernel through its EFI stub on the UEFI-class
# firmware of the device-tree machines, all the way to a shell:
# aarch64-uefi (arm/uefi) or riscv64-uefi (riscv/uefi).
#
#   make linux-arm                     AArch64 (not part of make test)
#   make linux-riscv-uefi              RISC-V
#   tests/uefi-linux.sh aarch64|riscv64 [--interactive]
#
# Nothing here is checked into the repo. The script downloads two pinned
# Ubuntu 24.04 packages for the architecture (the generic kernel and
# busybox-static), checks their SHA-256, and keeps them in
# build/linux-<arch>-uefi/. It builds a tiny busybox initramfs and starts
# QEMU virt with -bios <firmware> -kernel Image -initrd ... -append ....
# On AArch64 the firmware reads them from fw_cfg; on RISC-V QEMU puts them
# in RAM and /chosen. Either way the Image runs as an EFI application, the
# command line arrives as its LoadOptions and the initrd through the
# Linux initrd LoadFile2 device path.
#
# The check passes when, in order:
#   1. the firmware starts the kernel's EFI stub and it calls
#      ExitBootServices,
#   2. the kernel reaches the shell prompt with /sys/firmware/efi, and a
#      typed command runs,
#   3. poweroff makes QEMU exit by itself (PSCI SYSTEM_OFF or SBI SRST,
#      status 0).
#
# LINUX_TIMEOUT (seconds, default 300), LINUX_SMP (default 1), LINUX_MEM
# (default 512M), LINUX_MACHINE (default virt; on AArch64 e.g.
# virt,gic-version=3 or virt,virtualization=on to enter at EL2) and
# LINUX_CPU (AArch64 only, default cortex-a57) set up the machine. The
# serial log is kept in build/linux-<arch>-uefi/linux.log.

set -eu

ARCH=${1:-}
shift || true
case $ARCH in
aarch64)
    QEMU=${QEMU_ARM:-qemu-system-aarch64}
    FIRMWARE=arm/uefi/squirrel-bios.fd
    CONSOLE=ttyAMA0
    # noble (24.04) release pocket; SHA-256 from its Packages index
    KERNEL_DEB=pool/main/l/linux/linux-image-unsigned-6.8.0-31-generic_6.8.0-31.31_arm64.deb
    KERNEL_SHA=370c09d374978f7bd6b157adc8dfae9b4498d9a4ae24aa8068000f0c61a0273e
    BUSYBOX_DEB=pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_arm64.deb
    BUSYBOX_SHA=1a8c03948f99dfbb90efd93d9214418b416ac0737ed4c90938ebbcd645f10682 ;;
riscv64)
    QEMU=${QEMU_RISCV:-qemu-system-riscv64}
    FIRMWARE=riscv/uefi/squirrel-bios.bin
    CONSOLE=ttyS0
    # the same packages tests/linux-shell.sh pins
    KERNEL_DEB=pool/main/l/linux-riscv/linux-image-6.8.0-31-generic_6.8.0-31.31.1_riscv64.deb
    KERNEL_SHA=dffba9df38826a26320c41dac556bd5bda2b447c8ea71433176bde601117d6e9
    BUSYBOX_DEB=pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_riscv64.deb
    BUSYBOX_SHA=c567d8dc47320ad4b17cc20a5a91d583b76a35da618d3b389f1770a358fafd41 ;;
*)  echo "usage: $0 aarch64|riscv64 [--interactive]" >&2; exit 2 ;;
esac
TIMEOUT=${LINUX_TIMEOUT:-300}
SMP=${LINUX_SMP:-1}
MEM=${LINUX_MEM:-512M}
MACHINE=${LINUX_MACHINE:-virt}
CPU=${LINUX_CPU:-cortex-a57}
WORK=${LINUX_WORK:-build/linux-$ARCH-uefi}
# LINUX_ACPI=1: boot with acpi=force, so Linux describes the machine from
# the firmware's ACPI tables instead of the device tree, and check it did
ACPI=${LINUX_ACPI:-0}
MIRROR=${UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports}
OLD_MIRROR=${UBUNTU_OLD_RELEASES:-http://old-releases.ubuntu.com/ubuntu}

KERNEL_VER=6.8.0-31-generic

interactive=0
[ "${1:-}" = "--interactive" ] && interactive=1
NAME=linux-$ARCH-uefi
[ "$ACPI" = 1 ] && NAME=$NAME-acpi

die() { echo "uefi-linux: $*" >&2; exit 1; }

[ -f "$FIRMWARE" ] || die "$FIRMWARE is missing; run make arm-uefi or make riscv-uefi first"
command -v "$QEMU" >/dev/null || die "$QEMU not found"
command -v cpio >/dev/null || die "cpio not found"
mkdir -p "$WORK"

fetch() {
    file=$WORK/$(basename "$1")
    if [ ! -f "$file" ]; then
        echo "uefi-linux: downloading $(basename "$1")"
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

# vmlinuz is the gzipped Image, which is itself a PE32+ EFI application
# (the EFI stub). Unpack it, so the firmware sees what an ESP would hold.
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

set -- "$QEMU" -M "$MACHINE"
[ "$ARCH" = aarch64 ] && set -- "$@" -cpu "$CPU"
set -- "$@" -m "$MEM" -smp "$SMP" -no-reboot \
    -bios "$FIRMWARE" -kernel "$image" -initrd "$initrd" \
    -append "console=$CONSOLE earlycon panic=-1$([ "$ACPI" = 1 ] && echo " acpi=force")"

if [ "$interactive" -eq 1 ]; then
    echo "uefi-linux: Ctrl-A X quits QEMU; poweroff shuts Linux down"
    exec "$@" -nographic
fi

log=$WORK/linux.log
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
    echo "FAIL  $NAME ($1)"
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
printf 'echo uefi-linux=$((40+2))\n' >&3
wait_for "uefi-linux=42" "command at the prompt"
if [ "$ACPI" = 1 ]; then
    grep -qa "ACPI: Interpreter enabled" "$log" || fail "Linux did not enable ACPI"
    printf 'echo "acpi-tables=$((6*7)) $(ls /sys/firmware/acpi/tables | tr "\\n" " ")"\n' >&3
    wait_for "acpi-tables=42" "the ACPI tables listed"
    tables=$(grep -a "acpi-tables=42" "$log" | tail -n 1 | tr -d '\r')
    for t in APIC DBG2 DSDT FACP GTDT MCFG SPCR; do
        echo "$tables" | grep -qw "$t" || fail "no $t in /sys/firmware/acpi/tables"
    done
    echo "ok    Linux runs on the ACPI tables (${tables#acpi-tables=42 })"
fi
printf 'poweroff -f\n' >&3
while running; do
    [ "$(date +%s)" -lt "$deadline" ] || fail "QEMU still running after poweroff"
    sleep 1
done
wait "$qemu" && status=0 || status=$?
[ "$status" -eq 0 ] || fail "QEMU exited with status $status after poweroff"
echo "ok    poweroff (QEMU exited with status 0)"
echo "PASS  $NAME"
