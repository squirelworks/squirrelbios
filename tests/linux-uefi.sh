#!/bin/sh
# Boot a real Linux x86-64 kernel through the x86_64-uefi firmware, to a
# busybox shell, with the firmware's ACPI and SMBIOS tables (roadmap C2).
#
#   make linux-uefi       run the check (not part of make test)
#
# Nothing here is checked into the repo. The script downloads two pinned
# Ubuntu 24.04 packages (the amd64 generic kernel and busybox-static,
# from the noble release pocket), checks their SHA-256 and keeps them in
# build/linux-uefi/. The kernel's EFI stub is handed to the firmware as
# -fw_cfg opt/org.squirrelworks/boot.efi, its command line as
# opt/org.squirrelworks/cmdline (LoadOptions) and a tiny busybox
# initramfs as opt/org.squirrelworks/initrd (LoadFile2 on the Linux
# initrd media device path). /init prints what the kernel found and
# powers off, which goes through the DSDT's \_S5.
#
# It runs on q35 and on i440fx (pc). Each passes when the serial log shows:
#   1. the kernel's ACPI table list naming our RSDP (OEM SQRLWK),
#   2. "ACPI: Interpreter enabled" and the PCI root bridge from the DSDT,
#   3. the shell's line with the kernel and CPU count,
#   4. the DMI vendor from our SMBIOS tables,
#   5. an ACPI power-off (S5) that ends QEMU,
# and no ACPI errors ("ACPI Error", "ACPI BIOS Error", "Firmware Bug").
#
# LINUX_TIMEOUT (seconds, default 300) bounds each run, LINUX_MEM
# (default 512M) and LINUX_SMP (default 2) set the guest. LINUX_NUMA=N
# (F1) splits the CPUs and memory into N QEMU -numa nodes; the runs then
# also need the kernel to find N nodes through the SRAT and its SLIT.
# LINUX_PXB=1 (F1) adds an extra PCI root bus 0x40 (pxb-pcie with a root
# port on q35, pxb on pc) with a virtio-rng behind it; the kernel must
# find the PC40 host bridge and the device on bus 0x41. LINUX_TPM=1 (C5)
# gives each machine a TPM 2.0 from swtpm: the kernel must find it
# through the DSDT's MSFT0101 device, and the event log it reads (the
# TPM2 table's log area) must replay to its SHA-256 PCRs 0-7
# (tests/tcglog.py). Logs are
# build/linux-uefi/linux-uefi-<machine>.log.

set -eu

QEMU_X86=${QEMU_X86_64:-qemu-system-x86_64}
FIRMWARE=${FIRMWARE:-x86_64/squirrel-bios.fd}
WORK=${LINUX_WORK:-build/linux-uefi}
TIMEOUT=${LINUX_TIMEOUT:-300}
MEM=${LINUX_MEM:-512M}
SMP=${LINUX_SMP:-2}
NUMA=${LINUX_NUMA:-0}
PXB=${LINUX_PXB:-0}
TPM=${LINUX_TPM:-0}
MIRROR=${UBUNTU_ARCHIVE:-http://archive.ubuntu.com/ubuntu}
OLD_MIRROR=${UBUNTU_OLD_RELEASES:-http://old-releases.ubuntu.com/ubuntu}

KERNEL_VER=6.8.0-31-generic
KERNEL_DEB=pool/main/l/linux/linux-image-unsigned-6.8.0-31-generic_6.8.0-31.31_amd64.deb
KERNEL_SHA=35115ceb5ddc1e73034a73d6b064b9c6d38c534f20279fd7229c510515b931d8
BUSYBOX_DEB=pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_amd64.deb
BUSYBOX_SHA=9936a597976a42088f977bfb204b84f02615feb42bb732ff1fdb8bef5b5c13f4

die() { echo "linux-uefi: $*" >&2; exit 1; }

[ -f "$FIRMWARE" ] || die "$FIRMWARE is missing; run make x86_64 first"
command -v "$QEMU_X86" >/dev/null || die "$QEMU_X86 not found"
command -v cpio >/dev/null || die "cpio not found"
mkdir -p "$WORK"

fetch() {
    deb=$WORK/$(basename "$1")
    if [ ! -f "$deb" ]; then
        echo "linux-uefi: downloading $(basename "$1")"
        curl -fsSL -o "$deb.part" "$MIRROR/$1" ||
            curl -fsSL -o "$deb.part" "$OLD_MIRROR/$1" ||
            die "download of $1 failed"
        mv "$deb.part" "$deb"
    fi
    echo "$2  $deb" | sha256sum -c --quiet - || {
        rm -f "$deb"
        die "$(basename "$deb") does not match its pinned SHA-256"
    }
}

unpack() {
    rm -rf "$2"
    mkdir -p "$2"
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$1" "$2"
    else
        (cd "$2" && ar x "$1" && tar xf data.tar.* &&
            rm -f data.tar.* control.tar.* debian-binary)
    fi
}

fetch "$KERNEL_DEB" "$KERNEL_SHA"
fetch "$BUSYBOX_DEB" "$BUSYBOX_SHA"

kernel=$WORK/vmlinuz-$KERNEL_VER.efi
if [ ! -f "$kernel" ]; then
    unpack "$(pwd)/$WORK/$(basename "$KERNEL_DEB")" "$WORK/kernel-deb"
    cp "$WORK/kernel-deb/boot/vmlinuz-$KERNEL_VER" "$kernel"
    rm -rf "$WORK/kernel-deb"
fi

# Initramfs: busybox and an /init that reports and powers off
root=$WORK/initramfs
unpack "$(pwd)/$WORK/$(basename "$BUSYBOX_DEB")" "$WORK/busybox-deb"
rm -rf "$root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys"
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
echo "squirrel: busybox shell on $(uname -sr), $(grep -c ^processor /proc/cpuinfo) CPU(s)"
echo "squirrel: acpi tables: $(ls /sys/firmware/acpi/tables | tr '\n' ' ')"
echo "squirrel: numa nodes: $(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l)"
echo "squirrel: dmi: $(cat /sys/class/dmi/id/bios_vendor) / $(cat /sys/class/dmi/id/product_name)"
if [ -e /sys/class/tpm/tpm0 ]; then
    mount -t securityfs securityfs /sys/kernel/security
    echo "squirrel: tpm: $(cat /sys/class/tpm/tpm0/tpm_version_major)"
    for i in 0 1 2 3 4 5 6 7; do echo "PCR$i=$(cat /sys/class/tpm/tpm0/pcr-sha256/$i)"; done
    od -An -tx1 -v /sys/kernel/security/tpm0/binary_bios_measurements | sed 's/^/TCGLOG /'
fi
poweroff -f
INIT
chmod 755 "$root/init"
initrd=$WORK/initramfs.cpio.gz
(cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$initrd"

# -numa: N memory backends and nodes, CPUs dealt out in equal runs
numa_args=
if [ "$NUMA" -gt 0 ]; then
    mb=$(echo "$MEM" | sed 's/M$//')
    per=$((mb / NUMA))
    cpn=$(( (SMP + NUMA - 1) / NUMA ))
    MEM=$((per * NUMA))M
    n=0
    while [ "$n" -lt "$NUMA" ]; do
        first=$((n * cpn))
        last=$((first + cpn - 1))
        [ "$last" -lt "$SMP" ] || last=$((SMP - 1))
        numa_args="$numa_args -object memory-backend-ram,id=nm$n,size=${per}M"
        numa_args="$numa_args -numa node,nodeid=$n,cpus=$first-$last,memdev=nm$n"
        n=$((n + 1))
    done
fi

fail=0
for machine in q35 pc; do
    pxb_args=
    if [ "$PXB" = 1 ]; then
        if [ "$machine" = q35 ]; then
            pxb_args="-device pxb-pcie,id=pxb1,bus_nr=0x40,bus=pcie.0 -device pcie-root-port,id=rp1,bus=pxb1,chassis=1 -device virtio-rng-pci,bus=rp1"
        else
            pxb_args="-device pxb,id=pxb1,bus_nr=0x40,bus=pci.0 -device virtio-rng-pci,bus=pxb1"
        fi
    fi
    log=$WORK/linux-uefi-$machine.log
    : > "$log"
    tpm_args=
    if [ "$TPM" = 1 ]; then
        tpm=$WORK/tpm-$machine
        rm -rf "$tpm"
        mkdir -p "$tpm/state"
        swtpm_setup --tpm2 --tpmstate "$tpm/state" --pcr-banks sha256 --overwrite >/dev/null ||
            die "swtpm_setup failed"
        swtpm socket --tpm2 --tpmstate dir="$tpm/state" --ctrl type=unixio,path="$tpm/sock" --daemon ||
            die "swtpm failed"
        tpm_args="-chardev socket,id=chrtpm,path=$tpm/sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0"
    fi
    "$QEMU_X86" -M "$machine" -bios "$FIRMWARE" -m "$MEM" -smp "$SMP" $numa_args $pxb_args $tpm_args -no-reboot \
        -fw_cfg name=opt/org.squirrelworks/boot.efi,file="$kernel" \
        -fw_cfg name=opt/org.squirrelworks/initrd,file="$initrd" \
        -fw_cfg "name=opt/org.squirrelworks/cmdline,string=console=ttyS0 panic=-1" \
        -display none -monitor none -serial "file:$log" >/dev/null 2>&1 &
    qemu=$!
    ticks=$((TIMEOUT * 2))
    exited=0
    while [ "$ticks" -gt 0 ]; do
        case $(ps -o stat= -p "$qemu" 2>/dev/null) in ""|Z*) exited=1; break ;; esac
        sleep 0.5
        ticks=$((ticks - 1))
    done
    [ "$exited" = 1 ] || kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true

    ok=1
    grep -qE "ACPI: RSDP 0x[0-9A-F]+ 000024 \(v02 SQRLWK\)" "$log" || ok=0
    grep -qF "ACPI: Interpreter enabled" "$log" || ok=0
    grep -qE "ACPI: PCI Root Bridge \[PCI0\]" "$log" || ok=0
    grep -qF "squirrel: busybox shell on Linux $KERNEL_VER, $SMP CPU(s)" "$log" || ok=0
    grep -qF "squirrel: dmi: SquirrelWorks" "$log" || ok=0
    grep -qF "ACPI: PM: Preparing to enter system sleep state S5" "$log" || ok=0
    [ "$exited" = 1 ] || ok=0
    if [ "$NUMA" -gt 0 ]; then
        grep -qF "squirrel: numa nodes: $NUMA" "$log" || ok=0
        grep -qE "ACPI: SRAT 0x[0-9A-F]+ [0-9A-F]+ \(v03 SQRLWK" "$log" || ok=0
        grep -qE "ACPI: SLIT 0x" "$log" || ok=0
    fi
    if [ "$PXB" = 1 ]; then
        grep -qF "ACPI: PCI Root Bridge [PC40] (domain 0000 [bus 40-ff])" "$log" || ok=0
        grep -qE "pci 0000:41:[0-9a-f]{2}\.0: \[1af4:10(05|44)\]" "$log" || ok=0
    fi
    if [ "$TPM" = 1 ]; then
        grep -qF "squirrel: tpm: 2" "$log" || ok=0
        grep -qE "ACPI: TPM2 0x[0-9A-F]+ 00004C \(v04 SQRLWK" "$log" || ok=0
        python3 tests/tcglog.py --linux "$log" | sed 's/^/linux-uefi-'"$machine"': /'
        python3 tests/tcglog.py --linux "$log" >/dev/null || ok=0
    fi
    grep -qE "ACPI Error|ACPI BIOS Error|Firmware Bug" "$log" && ok=0
    if [ "$ok" = 1 ]; then
        echo "PASS  linux-uefi-$machine ($KERNEL_VER to a shell with ACPI, powered off through S5)"
    else
        echo "FAIL  linux-uefi-$machine (see $log)"
        tail -n 20 "$log" | sed 's/^/      | /'
        fail=1
    fi
done
exit "$fail"
