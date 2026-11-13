#!/bin/sh
# SquirrelBIOS smoke tests: boot each firmware under QEMU and check what
# it prints on its serial console. Run through `make test`, which builds
# the firmware and the stand-in OS images in build/tests first.
#
# Each case passes when every expected string appears in the serial log
# before SMOKE_TIMEOUT seconds (default 10). Expected strings are separated
# by '|' and matched literally; one starting with '~' is an extended
# regular expression instead, and one starting with '!' must not appear.
# '@exit=N' requires QEMU to have exited by itself with status N; the
# strings are checked once more after it exits, so a case can pass then.
# With SERIAL_IN set to a file, the serial port also reads that file as
# its input.

set -u

TEST_DIR=build/tests
TIMEOUT=${SMOKE_TIMEOUT:-10}
QEMU_X86=${QEMU_X86:-qemu-system-i386}
QEMU_X86_64=${QEMU_X86_64:-qemu-system-x86_64}
QEMU_ARM=${QEMU_ARM:-qemu-system-aarch64}
QEMU_RISCV=${QEMU_RISCV:-qemu-system-riscv64}

pass=0
fail=0

# run NAME "EXPECTED|EXPECTED..." QEMU ARGS...
run() {
    name=$1
    expect=$2
    shift 2
    log=$TEST_DIR/$name.log
    rm -f "$log"
    : > "$log"

    if [ -n "${SERIAL_IN:-}" ]; then
        "$@" -display none -monitor none \
            -chardev "file,id=smokeser,path=$log,input-path=$SERIAL_IN" \
            -serial chardev:smokeser >/dev/null 2>&1 &
    else
        "$@" -display none -monitor none -serial "file:$log" >/dev/null 2>&1 &
    fi
    qemu=$!

    # kill -0 still succeeds on an exited child that has not been waited
    # for, so ask ps whether it is a zombie
    status=
    ok=0
    ticks=$((TIMEOUT * 10))
    while [ "$ticks" -gt 0 ]; do
        case $(ps -o stat= -p "$qemu" 2>/dev/null) in
        ""|Z*) wait "$qemu" 2>/dev/null; status=$? ;;
        esac
        ok=1
        old_ifs=$IFS
        IFS='|'
        for want in $expect; do
            case $want in
            "@exit="*) [ "$status" = "${want#@exit=}" ] || ok=0 ;;
            "~"*) grep -qE -- "${want#?}" "$log" || ok=0 ;;
            "!"*) grep -qF -- "${want#?}" "$log" && ok=0 ;;
            *)    grep -qF -- "$want" "$log" || ok=0 ;;
            esac
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
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name (expected: $expect)"
        sed 's/^/      | /' "$log"
        fail=$((fail + 1))
    fi
}

mkdir -p "$TEST_DIR"

# x86: boot sector reached with DL = 0x80, IF = DF = 0, and a memory map in
# ES:DI that has the required regions (an entry at 1 MB, if present, is
# usable; its length is not fixed), sorted and not overlapping
run x86-boot "SquirrelBIOS x86|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: a boot sector without an SQBL header gets today's one-sector
# hand-off: the same checks, and nothing after sector 1 was run
run x86-one-sector "OS-ENTRY x86 dl=ok memmap=ok flags=ok|!vga=" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-one-sector.bin,if=ide

# x86: no disk is reported, not hung on (master is tried, then slave)
run x86-no-drive "disk: no drive on primary slave|boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom

# x86: with 8 MB, CMOS 0x34/0x35 are zero and extended RAM comes from
# 0x30/0x31 (KB above 1 MB): 7 MB usable at 1 MB
run x86-mem-low "OS-ENTRY x86 dl=ok memmap=ok flags=ok|mm 0000000000100000 0000000000700000 1" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom -m 8 \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: with more than 4 GB, CMOS 0x5B-0x5D give a usable entry at 4 GB.
# How QEMU splits the rest around the PCI hole is not checked.
run x86-mem-high "OS-ENTRY x86 dl=ok memmap=ok flags=ok|~^mm 0000000100000000 [0-9A-F]*[1-9A-F][0-9A-F]* 1" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom -m 5120 \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: a clean read of a sector without 0xAA55 is refused once, not
# retried and not jumped to
run x86-bad-sector "disk: boot sector has no 0xAA55 signature|boot halted|!OS-ENTRY|!disk: timeout|!disk: read of LBA 0" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-bad-sector.bin,if=ide

# x86: an SQBL header in sector 0 loads 3 more sectors to 0x1000:0x0010
# before the usual jump to 0x7C00, which then jumps to them
run x86-extra "OS-ENTRY x86 extra dl=ok memmap=ok payload=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-extra.bin,if=ide

# x86: SQBL loads that would land on the memory map or the boot sector,
# or run past the EBDA (0x92000), are refused before anything is read or jumped to
run x86-extra-map "disk: load overlaps firmware data|boot halted|!OS-ENTRY" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-extra-map.bin,if=ide
run x86-extra-boot "disk: load overlaps firmware data|boot halted|!OS-ENTRY" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-extra-boot.bin,if=ide
run x86-extra-range "disk: load address is out of range|boot halted|!OS-ENTRY" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-extra-range.bin,if=ide

# x86: a disk shaped like Squirrel-OS/2 (no SQBL header, 192 sectors
# signed S2OK) is loaded to 0x8000 and entered there. INT 15h E820
# returns the SQMM entries. A short INT 13h packet is rejected.
run x86-osload "SquirrelBIOS x86|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok disk=absent|!entered-boot-sector|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-osload.bin,if=ide

# x86: a disk without the S2OK signature is entered at 0x7C00, and that
# boot sector reads with the same INT 13h calls Squirrel-OS/2 uses.
# INT 10h AH=0E writes the VGA cell the sector checks.
run x86-int13 "SquirrelBIOS x86|via-boot-sector|int13=ok tty=ok|!kernel loaded|!boot halted|!disk:" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-int13.bin,if=ide

# x86: BDA has equipment list, conventional memory KB, and a cursor
# inside 80x25 after the firmware's banner
run x86-bda "SquirrelBIOS x86|OS-ENTRY x86 bda=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-bda.bin,if=ide

# x86: A20 is on before the jump (write at 0x500000; 0x100500 != 0x000500)
run x86-a20 "SquirrelBIOS x86|OS-ENTRY x86 a20=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-a20.bin,if=ide

# x86: PIT ~18.2 Hz, PIC IRQ0 at INT 08h; BDA ticks advance with STI
run x86-pit "SquirrelBIOS x86|OS-ENTRY x86 pit=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-pit.bin,if=ide

# x86: INT 16h with IRQ1; harness injects 'a' through the QEMU monitor
if tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi

# x86: the same 'a' from a USB keyboard on UHCI, polled from IRQ0 after
# POST (QEMU sends keys to the keyboard added last). Then from one
# behind a hub, with a mouse on the other root port
if KBD_NAME=x86-usb-kbd \
   KBD_ARGS="-device piix3-usb-uhci,id=uhci,addr=5 -device usb-kbd,bus=uhci.0,port=1" \
   KBD_EXPECT="usb: keyboard is device 1" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
if KBD_NAME=x86-usb-kbd-hub \
   KBD_ARGS="-device piix3-usb-uhci,id=uhci,addr=5 -device usb-mouse,bus=uhci.0,port=1 -device usb-hub,bus=uhci.0,port=2 -device usb-kbd,bus=uhci.0,port=2.3" \
   KBD_EXPECT="usb: device 3 on hub 2 port 3: 0627:0001, HID|usb: keyboard is device 3" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# The keyboard on UHCI keeps the EBDA's USB pages, so an xHCI listed
# after it works from memory that is free only during POST
if KBD_NAME=x86-usb-kbd-xhci \
   KBD_ARGS="-device qemu-xhci,id=xhci,addr=8 -device usb-tablet,bus=xhci.0,port=3 -device piix3-usb-uhci,id=uhci,addr=5 -device usb-kbd,bus=uhci.0,port=1" \
   KBD_EXPECT="usb: keyboard is device 1|usb: xHCI at 00:08.0|usb: device 2 on root port 7: 0627:0001, HID" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# The other order: an xHCI listed first works from the EBDA's USB pages
# during POST, and a keyboard on a UHCI listed after it still starts
if KBD_NAME=x86-usb-kbd-after-xhci \
   KBD_ARGS="-device qemu-xhci,id=xhci,addr=5 -device usb-tablet,bus=xhci.0,port=3 -device piix3-usb-uhci,id=uhci,addr=8 -device usb-kbd,bus=uhci.0,port=1" \
   KBD_EXPECT="usb: xHCI at 00:05.0|usb: keyboard is device 2" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# A keyboard on xHCI: the controller keeps running from the EBDA. Twenty
# keys (40 reports) take its 15-TRB ring round more than twice. Then one
# behind two hubs, next to other devices whose slots are given back
if KBD_NAME=x86-usb-kbd-on-xhci KBD_IMG=$TEST_DIR/x86-kbd-many.bin KBD_KEYS=20 \
   KBD_ARGS="-device qemu-xhci,id=xhci,addr=5 -device usb-kbd,bus=xhci.0,port=1" \
   KBD_EXPECT="usb: device 1 on root port 5: 0627:0001, HID|usb: keyboard is device 1" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
if KBD_NAME=x86-usb-kbd-on-xhci-hub \
   KBD_ARGS="-device qemu-xhci,id=xhci,addr=5 -device usb-hub,bus=xhci.0,port=1 -device usb-mouse,bus=xhci.0,port=1.1 -device usb-hub,bus=xhci.0,port=1.2 -device usb-tablet,bus=xhci.0,port=1.2.1 -device usb-kbd,bus=xhci.0,port=1.2.3" \
   KBD_EXPECT="usb: device 5 on hub 3 port 3: 0627:0001, HID|usb: keyboard is device 5" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# A USB mouse beside the keyboard: INT 15h C2xx, then a motion reaches
# the handler set with C207h (QEMU moves the mouse added last)
for m in "x86-usb-mouse|-device piix3-usb-uhci,id=uhci,addr=5 -device usb-kbd,bus=uhci.0,port=1 -device usb-mouse,bus=uhci.0,port=2|usb: mouse is device 2" \
         "x86-usb-mouse-ohci|-device pci-ohci,id=ohci,addr=5 -device usb-kbd,bus=ohci.0,port=1 -device usb-mouse,bus=ohci.0,port=2|usb: mouse is device 2" \
         "x86-usb-mouse-xhci|-device qemu-xhci,id=xhci,addr=5 -device usb-kbd,bus=xhci.0,port=1 -device usb-hub,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=2.1|usb: mouse is device 3"; do
    old_ifs=$IFS
    IFS='|'
    set -- $m
    IFS=$old_ifs
    if KBD_NAME=$1 KBD_IMG=$TEST_DIR/x86-mouse.bin KBD_MOUSE=1 KBD_OK=mouse=ok \
       KBD_ARGS="$2" KBD_EXPECT="usb: keyboard is device 1|$3" tests/x86-kbd-run.sh; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done

# x86: B8, booting from a USB disk (bulk-only) on xHCI when there is no
# IDE, SATA or NVMe disk. The boot sector, a Squirrel-OS/2 kernel load
# on q35, the INT 13h calls its boot sector makes, and the 200 GiB EDD
# disk (LBA48-range reads, a write, an unaligned read over three pages;
# AH=48h names PCI 00:05.0, USB and device 1)
run x86-usbdisk "usb: device 1 on root port 1: 46F4:0001, mass storage|usb: disk is device 1|disk: USB disk (bulk-only)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-storage,bus=xhci.0,drive=ud
run x86-usbdisk-osload "disk: USB disk (bulk-only)|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok|!boot halted" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-osload.bin,snapshot=on \
    -device usb-storage,bus=xhci.0,drive=ud
run x86-usbdisk-int13 "disk: USB disk (bulk-only)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-storage,bus=xhci.0,drive=ud
run x86-usbdisk-edd "disk: USB disk (bulk-only)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-storage,bus=xhci.0,drive=ud
# The same on a UHCI (USB 1.1, full speed): the disk's TDs hang from its
# own queue head in the frame list the controller keeps after POST. The
# x86-boot sector, the INT 13h calls and the EDD disk (64 KB transfers in
# 16 KB passes)
run x86-usbdisk-uhci "usb: UHCI at 00:05.0|usb: device 1 on root port 1: 46F4:0001, mass storage|usb: disk is device 1|disk: USB disk (bulk-only)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device piix3-usb-uhci,id=uhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-storage,bus=uhci.0,drive=ud
run x86-usbdisk-uhci-int13 "disk: USB disk (bulk-only)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device piix3-usb-uhci,id=uhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-storage,bus=uhci.0,drive=ud
run x86-usbdisk-uhci-edd "disk: USB disk (bulk-only)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device piix3-usb-uhci,id=uhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-storage,bus=uhci.0,drive=ud
# A keyboard and the disk on one UHCI: twenty keys still arrive
if KBD_NAME=x86-usbdisk-kbd-uhci KBD_IMG=$TEST_DIR/x86-kbd-many.bin KBD_KEYS=20 KBD_USBDISK=uhci.0,port=2 \
   KBD_ARGS="-device piix3-usb-uhci,id=uhci,addr=5 -device usb-kbd,bus=uhci.0,port=1" \
   KBD_EXPECT="usb: keyboard is device 1|usb: disk is device 2|disk: USB disk (bulk-only)" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# The same on an OHCI: the disk's two bulk endpoint descriptors on the
# bulk list, general TDs of up to 8 KB, and a keyboard alongside
run x86-usbdisk-ohci "usb: OHCI at 00:05.0|usb: disk is device 1|disk: USB disk (bulk-only)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device pci-ohci,id=ohci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-storage,bus=ohci.0,drive=ud
run x86-usbdisk-ohci-int13 "disk: USB disk (bulk-only)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device pci-ohci,id=ohci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-storage,bus=ohci.0,drive=ud
run x86-usbdisk-ohci-edd "disk: USB disk (bulk-only)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device pci-ohci,id=ohci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-storage,bus=ohci.0,drive=ud
if KBD_NAME=x86-usbdisk-kbd-ohci KBD_IMG=$TEST_DIR/x86-kbd-many.bin KBD_KEYS=20 KBD_USBDISK=ohci.0,port=2 \
   KBD_ARGS="-device pci-ohci,id=ohci,addr=5 -device usb-kbd,bus=ohci.0,port=1" \
   KBD_EXPECT="usb: keyboard is device 1|usb: disk is device 2|disk: USB disk (bulk-only)" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# The same on an EHCI at high speed: a queue head on the asynchronous
# list, run only while a transfer is, and qTDs of up to 16 KB. On q35's
# ICH9 the full-speed keyboard goes to a UHCI companion, which keeps it
run x86-usbdisk-ehci "usb: EHCI at 00:05.0|usb: disk is device 1|disk: USB disk (bulk-only)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-storage,bus=ehci.0,drive=ud
run x86-usbdisk-ehci-int13 "disk: USB disk (bulk-only)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-storage,bus=ehci.0,drive=ud
run x86-usbdisk-ehci-edd "disk: USB disk (bulk-only)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-storage,bus=ehci.0,drive=ud
if KBD_NAME=x86-usbdisk-kbd-ehci KBD_IMG=$TEST_DIR/x86-kbd-many.bin KBD_KEYS=20 KBD_USBDISK=ehci.0,port=1 \
   KBD_ARGS="-M q35 -device ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on -device ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1,multifunction=on -device ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2,multifunction=on -device usb-kbd,bus=ehci.0,port=3,usb_version=1" \
   KBD_EXPECT="usb: EHCI at 00:1D.7|usb: disk is device 1|disk: USB disk (bulk-only)|usb: keyboard is device 2" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# A UAS disk (USB Attached SCSI) on EHCI: command, status, data-in and
# data-out pipes found by their pipe usage descriptors, a READ READY or
# WRITE READY IU before the data and a SENSE IU after it
run x86-usbdisk-uas "usb: disk is device 1|disk: USB disk (UAS)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-uas,id=uas,bus=ehci.0 -device scsi-hd,bus=uas.0,drive=ud
run x86-usbdisk-uas-int13 "disk: USB disk (UAS)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-uas,id=uas,bus=ehci.0 -device scsi-hd,bus=uas.0,drive=ud
run x86-usbdisk-uas-edd "disk: USB disk (UAS)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device usb-ehci,id=ehci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-uas,id=uas,bus=ehci.0 -device scsi-hd,bus=uas.0,drive=ud
# The same at super speed on xHCI, with streams: the status and data
# pipes each get a stream context array whose stream 1 is their ring, and
# every command is tag 1, so no READY IU comes before the data
run x86-usbdisk-uas-xhci "usb: device 1 on root port 1: 46F4:0003, mass storage|usb: disk is device 1|disk: USB disk (UAS)|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-boot.bin,snapshot=on \
    -device usb-uas,id=uas,bus=xhci.0 -device scsi-hd,bus=uas.0,drive=ud
run x86-usbdisk-uas-xhci-int13 "disk: USB disk (UAS)|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device usb-uas,id=uas,bus=xhci.0 -device scsi-hd,bus=uas.0,drive=ud
run x86-usbdisk-uas-xhci-edd "disk: USB disk (UAS)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device qemu-xhci,id=xhci,addr=5 \
    -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-usbdisk.img,snapshot=on \
    -device usb-uas,id=uas,bus=xhci.0 -device scsi-hd,bus=uas.0,drive=ud
# Stall recovery: a test build sends one command with a bad CBW
# signature, which the disk stalls. Reset recovery (Bulk-Only Mass
# Storage Reset, CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints, the
# host side reset too) and one retry keep INT 13h working, on each of the
# four controllers
for hc in xhci:qemu-xhci ehci:usb-ehci ohci:pci-ohci uhci:piix3-usb-uhci; do
    run x86-usbdisk-stall-${hc%%:*} "disk: USB disk (bulk-only)|via-boot-sector|int13=ok tty=ok|!boot halted" \
        "$QEMU_X86" -bios $TEST_DIR/x86-usbfault.rom \
        -device ${hc#*:},id=hc,addr=5 \
        -drive if=none,id=ud,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
        -device usb-storage,bus=hc.0,drive=ud
done
# The disk behind a hub, with a keyboard and a mouse on the same xHCI:
# all three are kept, and twenty keys still arrive after the boot
if KBD_NAME=x86-usbdisk-kbd KBD_IMG=$TEST_DIR/x86-kbd-many.bin KBD_KEYS=20 KBD_USBDISK=xhci.0,port=2.3 \
   KBD_ARGS="-device qemu-xhci,id=xhci,addr=5 -device usb-hub,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3 -device usb-kbd,bus=xhci.0,port=1" \
   KBD_EXPECT="usb: keyboard is device 1|usb: mouse is device 4|usb: disk is device 3|disk: USB disk (bulk-only)" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi

# x86: B10, VBE 3.0 on QEMU's standard VGA. 4F00h-4F03h, then a
# 640x480x32 linear frame buffer filled through unreal mode with a red
# square: QEMU's screen must show it. Then the same, back to 80x25 text
# with 4F02h BX=3 (720x400 on screen)
if VBE_NAME=x86-vbe VBE_SCREEN="640 480 0,0=336699 50,50=ff0000 109,109=ff0000 110,110=336699 639,479=336699" \
   tests/x86-vbe-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
if VBE_NAME=x86-vbe-text VBE_SCREEN="720 400" tests/x86-vbe-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
# VBE banked window and palette: 640x480x8 without the linear frame
# buffer, palette entries 1 (red) and 2 (blue) set and read back, the
# screen filled with 2 one 64 KB window at a time (4F05h), a red square
# and a red line on row 300 drawn through the far window function
# VBE 4F0Ah: the protected-mode interface table, copied and called from
# 32-bit protected mode (window, display start, palette), read back in
# real mode
run x86-vbe-pmi "pmi=ok|!pmi=bad" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-vbe-pmi.bin,if=ide,snapshot=on
if VBE_NAME=x86-vbe-bank VBE_SCREEN="640 480 0,0=0000ff 9,10=0000ff 50,50=ff0000 109,109=ff0000 110,110=0000ff 0,300=ff0000 639,300=ff0000 639,299=0000ff 639,479=0000ff" \
   tests/x86-vbe-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi

# x86: a USB keyboard behind a hub on OHCI; then a full-speed one on an
# ICH9 EHCI port, handed to its UHCI companion, which keeps it
if KBD_NAME=x86-usb-kbd-ohci \
   KBD_ARGS="-device pci-ohci,id=ohci,addr=6 -device usb-hub,bus=ohci.0,port=2 -device usb-kbd,bus=ohci.0,port=2.1" \
   KBD_EXPECT="usb: OHCI at 00:06.0|usb: device 2 on hub 1 port 1: 0627:0001, HID|usb: keyboard is device 2" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi
if KBD_NAME=x86-usb-kbd-ehci \
   KBD_ARGS="-M q35 -device ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on -device ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1,multifunction=on -device usb-kbd,bus=ehci.0,port=3,usb_version=1" \
   KBD_EXPECT="usb: UHCI at 00:1D.1|usb: device 1 on root port 1: 0627:0001, HID|usb: keyboard is device 1" tests/x86-kbd-run.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
fi

# x86: INT 1Ah AH=00 ticks from IRQ0; AH=02 RTC time in BCD
run x86-time "SquirrelBIOS x86|OS-ENTRY x86 time=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-time.bin,if=ide

# x86: INT 10h set/get cursor, scroll, get mode; AH=0E still works
run x86-vid "SquirrelBIOS x86|OS-ENTRY x86 vid=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-vid.bin,if=ide

# x86: INT 13h AH=43/03 write sectors that a later read returns
run x86-write "SquirrelBIOS x86|OS-ENTRY x86 write=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-write.bin,if=ide

# x86: INT 15h AX=2400/2401/2402 A20 gate; E820 still works
run x86-int15a20 "SquirrelBIOS x86|OS-ENTRY x86 int15a20=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-int15a20.bin,if=ide

# x86: with the image only on the primary slave, boot and DL=0x81
run x86-slave "SquirrelBIOS x86|OS-ENTRY x86 dl=ok memmap=ok flags=ok|!dl=bad" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=slave,format=raw,file=$TEST_DIR/x86-slave.bin \
    -device ide-hd,drive=slave,bus=ide.0,unit=1

# x86: El Torito no-emulation boot from -cdrom (secondary master) with no
# hard disk; the image reads the CD through INT 13h DL = E0h
run x86-cd "booting from CD-ROM|OS-ENTRY x86 cd cd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -cdrom $TEST_DIR/x86-cd.iso
# El Torito diskette emulation: the x86-floppy checks on a 1.44 MB image
# that a CD carries (media type 2), booted as drive 00h in place of the
# empty diskette drive; writes are refused as write-protected
# El Torito hard disk emulation: a 2016-sector disk image becomes C: (80h),
# read-only, with the geometry its MBR's partition ends on
run x86-cd-hdd "booting from CD-ROM (El Torito, hard disk emulation as C:)|OS-ENTRY x86 cdhd cdhd=ok|!cdhd=bad" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -cdrom $TEST_DIR/x86-cd-hdd.iso
run x86-cd-floppy "booting from CD-ROM (El Torito, diskette emulation as A:)|OS-ENTRY x86 floppy floppy=ok|!floppy=bad" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -cdrom $TEST_DIR/x86-cd-floppy.iso

# x86: the same CD as primary slave, with no disk on the master
run x86-cd-slave "booting from CD-ROM|OS-ENTRY x86 cd cd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-cd-slave.iso,if=ide,media=cdrom,index=1

# x86: EDD 3.0 on a 200 GiB disk: AH=41h/48h, LBA48 reads across 2^28
# and at 143 GiB, a write read back, AH=44h at and past the last sector
run x86-edd "SquirrelBIOS x86|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-edd.img,if=ide

# x86: the same 200 GiB disk on q35's AHCI controller, port 4 (q35's
# empty CD drive sits on port 2 and is passed over): READ/WRITE DMA EXT
# through INT 13h, and AH=48h names PCI 00:1F.2 and SATA port 4
run x86-ahci "SquirrelBIOS x86|disk: SATA disk on AHCI port 4|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive if=none,id=sata,format=raw,file=$TEST_DIR/x86-ahci.img \
    -device ide-hd,drive=sata,bus=ide.4

# x86: the same 200 GiB disk as namespace 1 of an NVMe controller in PCI
# slot 5 (i440fx): Read/Write (with Flush) through INT 13h, PRP lists,
# and AH=48h names PCI 00:05.0, NVMe and namespace 1
run x86-nvme "SquirrelBIOS x86|disk: NVMe disk (namespace 1)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=nvm,format=raw,file=$TEST_DIR/x86-nvme.img \
    -device nvme,serial=squirrel,drive=nvm,addr=5

# x86: B9, the same 200 GiB disk as a virtio-blk device in PCI slot 5
# (legacy interface): reads and writes through INT 13h, and AH=48h names
# PCI 00:05.0. Then the S2OK kernel load on q35 and the INT 13h boot
# sector from a virtio disk
run x86-virtio "SquirrelBIOS x86|disk: virtio disk|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-virtio.img,snapshot=on \
    -device virtio-blk-pci,drive=vd,addr=5
# The same disks on the modern interface only (device 1042h, its
# registers in a memory BAR found through the virtio PCI capabilities):
# on pc and on q35, and the INT 13h write path
run x86-virtio-modern "disk: virtio disk (modern interface)|OS-ENTRY x86 edd edd=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-virtio.img,snapshot=on \
    -device virtio-blk-pci,drive=vd,addr=5,disable-legacy=on,disable-modern=off
run x86-virtio-modern-q35 "disk: virtio disk (modern interface)|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok|!boot halted" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-osload.bin,snapshot=on \
    -device virtio-blk-pci,drive=vd,disable-legacy=on,disable-modern=off
run x86-virtio-modern-write "disk: virtio disk (modern interface)|OS-ENTRY x86 write=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-write.bin,snapshot=on \
    -device virtio-blk-pci,drive=vd,disable-legacy=on,disable-modern=off
run x86-virtio-osload "disk: virtio disk|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok|!boot halted" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-osload.bin,snapshot=on \
    -device virtio-blk-pci,drive=vd
run x86-virtio-int13 "disk: virtio disk|via-boot-sector|int13=ok tty=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive if=none,id=vd,format=raw,file=$TEST_DIR/x86-int13.bin,snapshot=on \
    -device virtio-blk-pci,drive=vd

# x86: B9, network boot. No disk; QEMU's user network gives an address
# and serves the boot file over TFTP, which is loaded at 0x7C00 and
# entered with DL = FFh: 5000 bytes, 2048 on q35 (an empty last block),
# 200000 (block numbers past 255). Then a file the server does not have,
# and one too large for the memory below the network buffers
NET_TFTP="tftp=$TEST_DIR/tftp"
run x86-netboot "net: virtio-net, MAC 52:54:00:12:34:56|net: address 10.0.2.15 from 10.0.2.2, boot file x86-netboot.bin|net: loaded 5000 bytes|OS-ENTRY x86 netboot=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev user,id=n0,$NET_TFTP,bootfile=x86-netboot.bin -device virtio-net-pci,netdev=n0,addr=5
# Routed network boot: on a simulated network (tests/netsim.py, joined
# through a QEMU UDP socket) the boot server 192.168.50.5 sits behind the
# router 10.0.2.1 that DHCP names (options 1 and 3). Only the router
# answers ARP and the server only takes frames sent to the router, so the
# 200000-byte file arrives only when the firmware routes. Without option
# 3 the firmware says why it cannot reach the server.
NETSIM_PORT=$((20000 + $$ % 20000))
python3 tests/netsim.py $NETSIM_PORT $((NETSIM_PORT + 1)) $TEST_DIR/tftp x86-netboot-big.bin &
netsim=$!
sleep 0.3
run x86-netboot-routed "net: address 10.0.2.15 from 192.168.50.5, boot file x86-netboot-big.bin|net: boot server on another network, through router 10.0.2.1|net: loaded 200000 bytes|OS-ENTRY x86 netboot=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev socket,id=n0,udp=127.0.0.1:$NETSIM_PORT,localaddr=127.0.0.1:$((NETSIM_PORT + 1)) \
    -device virtio-net-pci,netdev=n0
kill $netsim 2>/dev/null; wait $netsim 2>/dev/null
python3 tests/netsim.py $NETSIM_PORT $((NETSIM_PORT + 1)) $TEST_DIR/tftp x86-netboot.bin norouter &
netsim=$!
sleep 0.3
run x86-netboot-norouter "net: the boot server is on another network and DHCP gave no router|!netboot=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev socket,id=n0,udp=127.0.0.1:$NETSIM_PORT,localaddr=127.0.0.1:$((NETSIM_PORT + 1)) \
    -device virtio-net-pci,netdev=n0
kill $netsim 2>/dev/null; wait $netsim 2>/dev/null
run x86-netboot-q35 "net: loaded 2048 bytes|OS-ENTRY x86 netboot=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -netdev user,id=n0,$NET_TFTP,bootfile=x86-netboot-2k.bin -device virtio-net-pci,netdev=n0
run x86-netboot-big "net: loaded 200000 bytes|OS-ENTRY x86 netboot=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev user,id=n0,$NET_TFTP,bootfile=x86-netboot-big.bin -device virtio-net-pci,netdev=n0
run x86-netboot-missing "net: the TFTP server refused the file|boot halted|!OS-ENTRY" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev user,id=n0,$NET_TFTP,bootfile=no-such-file.bin -device virtio-net-pci,netdev=n0
run x86-netboot-huge "net: the boot file does not fit below 0x40000|boot halted|!OS-ENTRY" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -netdev user,id=n0,$NET_TFTP,bootfile=x86-netboot-huge.bin -device virtio-net-pci,netdev=n0

# x86: the 192-sector S2OK kernel load from NVMe on q35, where the AHCI
# controller is there but has no disk
run x86-nvme-osload "SquirrelBIOS x86|disk: NVMe disk (namespace 1)|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok|!boot halted" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive if=none,id=nvm,format=raw,file=$TEST_DIR/x86-osload.bin,snapshot=on \
    -device nvme,serial=squirrel,drive=nvm

# x86: USB on a PIIX3 UHCI controller: a hub on root port 1 with a
# keyboard and a USB disk behind it, a mouse on root port 2. Every device
# is listed with its address, place, IDs and class, and the IDE disk
# still boots afterwards with an intact memory map
run x86-usb "usb: UHCI at 00:05.0|usb: device 1 on root port 1: 0409:55AA, hub, 8 ports|usb: device 2 on hub 1 port 3: 0627:0001, HID|usb: device 3 on hub 1 port 5: 46F4:0001, mass storage|usb: device 4 on root port 2: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device piix3-usb-uhci,id=uhci,addr=5 \
    -device usb-hub,bus=uhci.0,port=1 \
    -device usb-kbd,bus=uhci.0,port=1.3 \
    -drive if=none,id=usbdisk,format=raw,file=$TEST_DIR/x86-one-sector.bin,snapshot=on \
    -device usb-storage,bus=uhci.0,port=1.5,drive=usbdisk \
    -device usb-mouse,bus=uhci.0,port=2

# x86: USB on an OHCI controller: a keyboard and a second hub behind a
# hub on root port 1, a tablet at the bottom, a mouse on root port 3
run x86-usb-ohci "usb: OHCI at 00:06.0|usb: device 1 on root port 1: 0409:55AA, hub, 8 ports|usb: device 2 on hub 1 port 2: 0627:0001, HID|usb: device 3 on hub 1 port 4: 0409:55AA, hub, 8 ports|usb: device 4 on hub 3 port 8: 0627:0001, HID|usb: device 5 on root port 3: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device pci-ohci,id=ohci,addr=6 \
    -device usb-hub,bus=ohci.0,port=1 -device usb-kbd,bus=ohci.0,port=1.2 \
    -device usb-hub,bus=ohci.0,port=1.4 -device usb-tablet,bus=ohci.0,port=1.4.8 \
    -device usb-mouse,bus=ohci.0,port=3

# x86: two ICH9 UHCI functions on q35, a hub behind a hub on the first
# and a tablet on the second; device addresses run on across controllers
run x86-usb-q35 "usb: UHCI at 00:1D.0|usb: device 2 on hub 1 port 4: 0409:55AA, hub, 8 ports|usb: device 3 on hub 2 port 1: 0627:0001, HID|usb: UHCI at 00:1D.1|usb: device 4 on root port 1: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device ich9-usb-uhci1,id=u1,addr=1d.0,multifunction=on \
    -device ich9-usb-uhci2,id=u2,addr=1d.1 \
    -device usb-hub,bus=u1.0,port=2 -device usb-hub,bus=u1.0,port=2.4 \
    -device usb-kbd,bus=u1.0,port=2.4.1 -device usb-tablet,bus=u2.0,port=1

# x86: ICH9 EHCI with its three UHCI companions on q35. EHCI keeps the
# high-speed disk and tablet; the full-speed keyboard and the (full-speed)
# hub with a mouse behind it are handed to the companions, which list
# them on their own root ports
run x86-usb-ehci "usb: EHCI at 00:1D.7|usb: device 1 on root port 1: 46F4:0001, mass storage|usb: device 2 on root port 6: 0627:0001, HID|usb: UHCI at 00:1D.1|usb: device 3 on root port 2: 0627:0001, HID|usb: UHCI at 00:1D.2|usb: device 4 on root port 1: 0409:55AA, hub, 8 ports|usb: device 5 on hub 4 port 2: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on \
    -device ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on \
    -device ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1,multifunction=on \
    -device ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2,multifunction=on \
    -drive if=none,id=usbdisk,format=raw,file=$TEST_DIR/x86-one-sector.bin,snapshot=on \
    -device usb-storage,bus=ehci.0,port=1,drive=usbdisk \
    -device usb-kbd,bus=ehci.0,port=4,usb_version=1 \
    -device usb-hub,bus=ehci.0,port=5 -device usb-mouse,bus=ehci.0,port=5.2 \
    -device usb-tablet,bus=ehci.0,port=6

# x86: a lone EHCI controller on i440fx with two high-speed devices
run x86-usb-ehci-alone "usb: EHCI at 00:07.0|usb: device 1 on root port 2: 46F4:0001, mass storage|usb: device 2 on root port 4: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device usb-ehci,id=ehci,addr=7 \
    -drive if=none,id=usbdisk,format=raw,file=$TEST_DIR/x86-one-sector.bin,snapshot=on \
    -device usb-storage,bus=ehci.0,port=2,drive=usbdisk \
    -device usb-tablet,bus=ehci.0,port=4

# x86: a QEMU xHCI controller. Its USB 3 ports are 1-4 and its USB 2
# ports 5-8: the disk comes up at super speed on port 2, the hub chain
# (mouse, second hub, keyboard) on port 5 and the tablet on port 7
run x86-usb-xhci "usb: xHCI at 00:08.0|usb: device 1 on root port 2: 46F4:0001, mass storage|usb: device 2 on root port 5: 0409:55AA, hub, 8 ports|usb: device 3 on hub 2 port 2: 0627:0001, HID|usb: device 4 on hub 2 port 4: 0409:55AA, hub, 8 ports|usb: device 5 on hub 4 port 8: 0627:0001, HID|usb: device 6 on root port 7: 0627:0001, HID|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device qemu-xhci,id=xhci,addr=8 \
    -device usb-hub,bus=xhci.0,port=1 -device usb-mouse,bus=xhci.0,port=1.2 \
    -device usb-hub,bus=xhci.0,port=1.4 -device usb-kbd,bus=xhci.0,port=1.4.8 \
    -drive if=none,id=usbdisk,format=raw,file=$TEST_DIR/x86-one-sector.bin,snapshot=on \
    -device usb-storage,bus=xhci.0,port=2,drive=usbdisk \
    -device usb-tablet,bus=xhci.0,port=3

# x86: ten devices on an xHCI that has eight slots for them: each device
# that is not a hub gives its slot back once it has been listed
run x86-usb-xhci-slots "usb: device 1 on root port 5: 0409:55AA, hub, 8 ports|usb: device 2 on hub 1 port 1: 0627:0001, HID|usb: device 9 on hub 1 port 8: 0627:0001, HID|usb: device 10 on root port 7: 0627:0001, HID|!usb: no answer|OS-ENTRY x86 dl=ok memmap=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device qemu-xhci,id=xhci,addr=8 -device usb-hub,bus=xhci.0,port=1 \
    -device usb-mouse,bus=xhci.0,port=1.1 -device usb-tablet,bus=xhci.0,port=1.2 \
    -device usb-mouse,bus=xhci.0,port=1.3 -device usb-tablet,bus=xhci.0,port=1.4 \
    -device usb-mouse,bus=xhci.0,port=1.5 -device usb-tablet,bus=xhci.0,port=1.6 \
    -device usb-mouse,bus=xhci.0,port=1.7 -device usb-tablet,bus=xhci.0,port=1.8 \
    -device usb-tablet,bus=xhci.0,port=3

# x86: xHCI on q35 with a USB Attached SCSI disk at super speed
run x86-usb-xhci-q35 "usb: xHCI at 00:03.0|usb: device 1 on root port 4: 46F4:0003, mass storage|disk: SATA disk on AHCI port 0|OS-ENTRY x86 dl=ok memmap=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on \
    -device qemu-xhci,id=xhci \
    -device usb-uas,id=uas,bus=xhci.0,port=4 \
    -drive if=none,id=uasdisk,format=raw,file=$TEST_DIR/x86-one-sector.bin,snapshot=on \
    -device scsi-hd,bus=uas.0,drive=uasdisk

# x86: the 192-sector S2OK kernel load from an AHCI disk (q35, port 0)
run x86-ahci-osload "SquirrelBIOS x86|disk: SATA disk on AHCI port 0|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok|!boot halted" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-osload.bin,if=ide,snapshot=on

# x86: PCI on i440fx: the PCI BIOS finds devices by ID and class, POST
# placed the BARs (I/O, 32- and 64-bit memory) and routed INTA through
# the PIIX3, and the $PIR table checks out
run x86-pci "SquirrelBIOS x86|OS-ENTRY x86 pci pci=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-pci.bin,if=ide \
    -device virtio-rng-pci

# x86: the same on q35 (no PIIX3: Interrupt Line is 0xFF), booting from
# a PIIX3 IDE controller since q35's own disks are AHCI
run x86-pci-q35 "SquirrelBIOS x86|OS-ENTRY x86 pci pci=ok" \
    "$QEMU_X86" -M q35 -bios x86/squirrel-bios.rom \
    -device piix3-ide,id=pide \
    -drive if=none,id=pcidisk,format=raw,file=$TEST_DIR/x86-pci.bin \
    -device ide-hd,drive=pcidisk,bus=pide.0 \
    -device virtio-rng-pci

# x86: ACPI RSDP in 0xE0000–0xFFFFF, checksum and RSDT→FACP
run x86-acpi "SquirrelBIOS x86|OS-ENTRY x86 acpi=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-acpi.bin,if=ide

# x86: SMBIOS _SM_ in 0xF0000–0xFFFFF with valid checksums
run x86-smbios "SquirrelBIOS x86|OS-ENTRY x86 smbios=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-smbios.bin,if=ide

# x86: Linux bzImage (HdrS) loaded at 1 MB; PM payload prints bz=ok
# The stand-in also checks the 32-bit entry segments and boot_params
# (setup header, type_of_loader, E820 table, alt_mem_k, cmd_line_ptr).
run x86-bzimage "SquirrelBIOS x86|bzImage loaded at 0x00100000, jumping|bz=ok seg=ok bp=ok tail=ok|!entered-boot-sector|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-bzimage.bin,if=ide

# x86: a 300 KB protected-mode kernel is copied whole (its last dword)
run x86-bzimage-big "bzImage loaded at 0x00100000, jumping|bz=ok seg=ok bp=ok tail=ok|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-bzimage-big.bin,if=ide

# x86: a kernel larger than the RAM at 1 MB is refused before the load
run x86-bzimage-toobig "disk: bzImage does not fit in RAM|boot halted|!bz=ok|!bzImage loaded" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom -m 32 \
    -drive format=raw,file=$TEST_DIR/x86-bzimage-toobig.bin,if=ide

# x86: INT 13h AH=08h fails (CF=1, AH=07h) for an absent slave; DL and
# BDA 0x475 count the disks that answered IDENTIFY
run x86-slave-absent "OS-ENTRY x86 params=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-params.bin,if=ide
run x86-params-slave "OS-ENTRY x86 params=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-params-slave.bin,if=ide \
    -drive if=none,id=slave,format=raw,file=$TEST_DIR/x86-boot.bin \
    -device ide-hd,drive=slave,bus=ide.0,unit=1

# x86: option ROM at 0xD0000 is scanned and its init prints optrom=ok
run x86-optrom "SquirrelBIOS x86|optrom=ok|OS-ENTRY x86 dl=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device loader,file=$TEST_DIR/x86-optrom.bin,addr=0xD0000,force-raw=on \
    -drive format=raw,file=$TEST_DIR/x86-one-sector.bin,if=ide

# x86: a 1.44 MB diskette in A: boots first, with DL = 0x00, and its boot
# sector reads, writes and sizes the diskette through INT 13h
run x86-floppy "SquirrelBIOS x86|booting from floppy A:|OS-ENTRY x86 floppy floppy=ok|!floppy=bad" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-floppy.img,if=floppy

# x86: a diskette without 0xAA55 is reported, then the hard disk boots
run x86-floppy-nosig "floppy: boot sector has no 0xAA55 signature, trying the hard disk|OS-ENTRY x86 dl=ok memmap=ok flags=ok|!booting from floppy" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-floppy-nosig.img,if=floppy \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: a CD-ROM as primary master with a disc too small to hold a boot
# record: the CD boot says so, then the ATAPI master is reported
run x86-atapi "cd: read of the CD failed|disk: primary master is ATAPI (CD/DVD), not a hard disk|boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,media=cdrom

# AArch64: -kernel image entered with the DTB in x0
run arm-boot "SquirrelBIOS ARM|OS-ENTRY arm dtb=ok regs=ok sctlr=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-kernel.bin

# AArch64: the same at EL2 (virtualization=on), so SCTLR_EL2 is the one set
run arm-boot-el2 "SquirrelBIOS ARM|OS-ENTRY arm dtb=ok regs=ok sctlr=ok" \
    "$QEMU_ARM" -M virt,virtualization=on -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-kernel.bin

# AArch64: an image that would run past the end of RAM is refused before
# the load. RAM size from fw_cfg 0x03, or the device tree on QEMU virt.
run arm-too-big "boot: kernel does not fit in RAM|boot halted|!kernel loaded" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -m 32 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-too-big.bin

# AArch64: an Image header with placement fixed loads at RAM base +
# text_offset; with bit 3 set it stays at 0x40200000 whatever text_offset
# says; an image_size below the file size is refused
run arm-image-fixed "kernel loaded at 0x40400000|OS-ENTRY arm image at=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-image-fixed.bin
run arm-image-reloc "kernel loaded at 0x40200000|OS-ENTRY arm image at=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-image-reloc.bin
run arm-image-short "boot: Image header image_size is smaller than the image|boot halted|!OS-ENTRY" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-image-short.bin

# AArch64: -initrd is loaded after the kernel and recorded in /chosen; the
# image still finds a valid device tree
run arm-initrd "~^initrd at 0x[0-9a-f]+, end 0x[0-9a-f]+|OS-ENTRY arm dtb=ok regs=ok sctlr=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-kernel.bin -initrd $TEST_DIR/arm-initrd.img

run arm-no-kernel "boot: no kernel image|boot halted" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin

# RISC-V: -kernel image entered in S-mode (QEMU's next_mode = 1) on hart 0
# with the DTB in a1, so the UART store and the DTB load pass PMP, and no trap
# reported (checked while the case runs, so a late trap may be missed)
run riscv-boot "SquirrelBIOS RISC-V|next_mode=1|OS-ENTRY riscv hart=ok dtb=ok a2=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-kernel.bin

# Build configs: the riscv64-virt/small build (8 harts tracked, from
# configs/riscv64-virt/small.cfg) boots the same image
run config-riscv-small "SquirrelBIOS RISC-V|next_mode=1|OS-ENTRY riscv hart=ok dtb=ok a2=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios build/riscv64-virt/small/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-kernel.bin

# Build configs: riscv64-virt/dt has its fixed UART and CLINT addresses
# pointing at nothing, so the SBI must find the NS16550A and the CLINT (or
# with aclint=on, the ACLINT MSWI and MTIMER) in the device tree: console,
# timer interrupts, IPIs and hart start all go through what it found
RV_DT=build/riscv64-virt/dt/squirrel-bios.bin
RV_DT_OK="dt: UART at 0x0000000010000000, CLINT msip at 0x0000000002000000, mtime at 0x000000000200bff8"
run config-riscv-dt-timer "$RV_DT_OK|OS-ENTRY riscv timer|timer=ok|!timer=again|!timer=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios $RV_DT -kernel $TEST_DIR/riscv-timer.bin
run config-riscv-dt-ipi "$RV_DT_OK|OS-ENTRY riscv ipi|ipi=ok|!ipi=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios $RV_DT -kernel $TEST_DIR/riscv-ipi.bin
run config-riscv-dt-hsm-aclint "$RV_DT_OK|OS-ENTRY riscv hsm|hart1=ok a1=5157000012345678|hsm=ok|!hsm=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt,aclint=on -smp 2 -bios $RV_DT -kernel $TEST_DIR/riscv-hsm.bin

# Build configs: x86-pc/minimal (no ACPI, SMBIOS, bzImage loader or
# option ROM scan) still boots the x86-boot image with every hand-off
# check, and really leaves ACPI out (the x86-acpi image finds no RSDP)
run config-x86-minimal "SquirrelBIOS x86|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86" -bios build/x86-pc/minimal/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide
run config-x86-minimal-noacpi "OS-ENTRY x86 acpi=bad|!acpi=ok" \
    "$QEMU_X86" -bios build/x86-pc/minimal/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-acpi.bin,if=ide

# RISC-V: an ecall from S-mode reaches the SBI in M-mode and returns
# SBI_ERR_NOT_SUPPORTED for an extension that does not exist
run riscv-ecall "OS-ENTRY riscv ecall|ecall=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ecall.bin

# RISC-V: an illegal instruction is delegated to the image's stvec, and its
# ecall from S-mode still reaches the SBI in M-mode
run riscv-deleg "OS-ENTRY riscv deleg|deleg=ok|ecall=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-deleg.bin

# RISC-V: SBI base extension answers, with only a0 and a1 changed
run riscv-sbi "OS-ENTRY riscv sbi|dbcn=ok|sbi=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-sbi.bin

# RISC-V: SSE (SBI 3.0 supervisor software events) for the software-
# injected local event: register, attributes and their errors, a masked
# inject that waits, unmask taking it, the handler's a6/a7 and saved
# state, complete resuming a0/a6/a7/sepc, one-shot; then hart 0 injects
# on hart 1, which takes it on the IPI. DBCN refuses firmware memory.
run riscv-sse "OS-ENTRY riscv sse|sse=ok|!sse=bad|!trap mcause=|!ras:" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-sse.bin

# RISC-V: sbi_set_timer raises the supervisor timer interrupt once
run riscv-timer "OS-ENTRY riscv timer|timer=ok|!timer=again|!timer=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-timer.bin

# RISC-V: sbi_send_ipi to itself raises a supervisor software interrupt;
# to hart 1, which is parked (stopped), it succeeds and raises nothing
# here; to hart 2, which the machine does not have, it fails (-smp 2)
run riscv-ipi "OS-ENTRY riscv ipi|ipi=ok|!ipi=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ipi.bin

# RISC-V: remote fences for the calling hart succeed, also with the
# stopped hart 1 in the mask; a mask that names hart 2, which the machine
# does not have, fails (-smp 2)
run riscv-rfence "OS-ENTRY riscv rfence|rfence=ok|!rfence=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-rfence.bin

# RISC-V: sbi_hart_start brings the parked hart 1 up in S-mode with the
# opaque value in a1, and hart_get_status follows it (-smp 2)
run riscv-hsm "OS-ENTRY riscv hsm|hart1=ok a1=5157000012345678|hsm=ok|!hsm=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-hsm.bin

# RISC-V: a started hart calls sbi_hart_stop, is reported stopped, and
# can be started again (-smp 2)
run riscv-hsm-stop "OS-ENTRY riscv hsm-stop|hart1 up a1=1|hart1 up a1=2|stop=ok|!stop=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-hsm-stop.bin

# RISC-V: with hart 1 started, IPIs reach it by mask bit, by mask base and
# with base -1 (which also reaches the caller), and remote fences on both
# harts return (-smp 2)
run riscv-ipi-smp "OS-ENTRY riscv ipi-smp|ipi-smp=ok|!ipi-smp=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ipi-smp.bin

# RISC-V: harts 0 and 1 both fence hart 2 many times at once, then IPI it
# (-smp 3)
run riscv-fence-smp3 "OS-ENTRY riscv fence-smp3|fence3=ok|!fence3=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 3 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-fence-smp3.bin

# RISC-V: on 64 harts, hart 0 starts the other 63 one at a time and fences
# all 64 with one mask
run riscv-harts "OS-ENTRY riscv harts|harts=64|!harts=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 64 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-harts.bin

# RISC-V: with AIA, the UART's interrupt reaches an image that drives the
# S-level APLIC, because the firmware delegated the sources to it
run riscv-aia "aia: APLIC sources delegated|OS-ENTRY riscv aia|aia=ok|!aia=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt,aia=aplic -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-aia.bin

# RISC-V: IPIs and fences still work on the AIA machine (still the CLINT)
run riscv-aia-ipi "OS-ENTRY riscv ipi-smp|ipi-smp=ok|!ipi-smp=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt,aia=aplic-imsic -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ipi-smp.bin

# RISC-V: two NUMA nodes are two sockets, each with its own CLINT; the
# harts of the second are started, and fenced, through theirs
run riscv-numa "OS-ENTRY riscv harts|harts=ok|!harts=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 4 -m 256M \
    -object memory-backend-ram,id=m0,size=128M -object memory-backend-ram,id=m1,size=128M \
    -numa node,cpus=0-1,memdev=m0 -numa node,cpus=2-3,memdev=m1 \
    -bios riscv/squirrel-bios.bin -kernel $TEST_DIR/riscv-numa.bin

# RISC-V: fw_dynamic_info names hart 2 as the boot hart (a shim stands in
# for QEMU's reset code, which always says 0); hart 2 enters the image
# and then starts hart 0
run riscv-boot-hart "OS-ENTRY riscv hart=ok dtb=ok a2=ok|hart0 started from the boot hart|!boot-hart=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 4 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-kernel-hart2.bin \
    -device loader,file=$TEST_DIR/riscv-boot-hart.bin,addr=0x81000000,force-raw=on \
    -device loader,addr=0x81000000,cpu-num=0 -device loader,addr=0x81000000,cpu-num=1 \
    -device loader,addr=0x81000000,cpu-num=2 -device loader,addr=0x81000000,cpu-num=3

# RISC-V: a store, a load and a fetch in the firmware region fault to the
# image's stvec; its own memory and the SBI still work
run riscv-pmp "OS-ENTRY riscv pmp|pmp=ok|!pmp=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-pmp.bin

# RISC-V: the device tree the image gets has the firmware region under
# /reserved-memory with no-map, and still parses (-smp 2)
run riscv-dt "dt: firmware region 0x80000000-0x801fffff reserved (no-map)|OS-ENTRY riscv dt|dt=ok|!dt=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-dt.bin

# RISC-V: cbo.zero, cbo.clean/flush/inval and a direct stimecmp timer work
# from S-mode, as Linux uses them on this QEMU hart
run riscv-envcfg "OS-ENTRY riscv envcfg|envcfg=ok|!envcfg=bad|!envcfg=trap|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-envcfg.bin

# RISC-V: sbi_system_reset shutdown powers the machine off: QEMU exits
# with status 0 after reset=ok, and the call never returns
run riscv-srst "OS-ENTRY riscv srst|reset=ok|!reset=bad|!trap mcause=|@exit=0" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-srst.bin

# RISC-V: a cold reboot resets the machine, which -no-reboot turns into
# an exit with status 0
run riscv-srst-reboot "OS-ENTRY riscv srst|reset=ok|!reset=bad|!trap mcause=|@exit=0" \
    "$QEMU_RISCV" -M virt -no-reboot -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-srst-reboot.bin

# RISC-V: a shutdown for a system failure makes QEMU exit with status 1
run riscv-srst-fail "OS-ENTRY riscv srst|reset=ok|!reset=bad|!trap mcause=|@exit=1" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-srst-fail.bin

# RISC-V: a fault in the SBI path prints a ras: line, parks the other
# started hart (hart1=still must not appear), prints boot halted, and
# shuts the machine down as a system failure (QEMU exit status 1)
run riscv-ras "OS-ENTRY riscv ras|hart1=up|~^ras: hart=0000000000000000 mcause=0000000000000005 mepc=[0-9a-f]{16} mtval=0000000000000000 fatal|boot halted|!hart1=still|!ras=bad|!trap mcause=|@exit=1" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ras.bin

run riscv-no-kernel "boot: no kernel image|boot halted" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin

# x86-64 UEFI-class firmware (C1): from the reset vector to long mode,
# the body copied to RAM and run there, and the memory map read from
# QEMU's fw_cfg (128 MB on q35; 4 GB, split around the PCI hole, with
# -m 4G; i440fx works the same). Then the EFI system table, the UEFI
# memory map with the firmware's own areas carved out, and the memory
# services checked through the boot services table
run x86_64-uefi "SquirrelBIOS 2.1.0 (x86_64-uefi)|~^cpu: .*, long mode, [0-9]+-bit physical addresses|fw: running at 0x0000000000200000-|mem: 0000000000000000-0000000007ffffff usable|mem: 128 MB usable (from fw_cfg)|~^efi: system table at 0x[0-9a-f]{16}, UEFI 2.70, boot services at|efi: 0000000000000000-0000000000006fff BS data|efi: 00000000000a0000-00000000000fffff reserved|efi: 0000000000200000-|~^efi: [0-9]+ descriptors of 48 bytes|efi: memory services ok|fw: nothing to boot (no opt/org.squirrelworks/boot.efi), trying the CSM" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd
run x86_64-uefi-4g "mem: 0000000000000000-000000007fffffff usable|mem: 0000000100000000-000000017fffffff usable|mem: 4096 MB usable (from fw_cfg)|efi: 0000000100000000-000000017fffffff free|efi: memory services ok|fw: nothing to boot (no opt/org.squirrelworks/boot.efi), trying the CSM" \
    "$QEMU_X86_64" -M q35 -m 4G -bios x86_64/squirrel-bios.fd
run x86_64-uefi-pc "SquirrelBIOS 2.1.0 (x86_64-uefi)|mem: 128 MB usable (from fw_cfg)|efi: memory services ok|fw: nothing to boot (no opt/org.squirrelworks/boot.efi), trying the CSM" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd

# x86-64 firmware: a PE32+ EFI application handed over through fw_cfg is
# loaded (relocated: its preferred base, 256 MB, is past the RAM), started
# with the system table, and checks the tables, its loaded image protocol,
# the relocation and the memory services. It returns 1234h; then the same
# image calls Exit(5678h). Either way it is unloaded and its pages come
# back. With 4 GB it loads at its preferred base. A file that is not PE
# is refused.
X64_BOOT=name=opt/org.squirrelworks/boot.efi
run x86_64-app "efi: image of 12288 bytes loaded at|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: image returned 0x1234|efi: image unloaded, its memory given back|fw: halting|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-app-exit "APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: image returned 0x5678|efi: image unloaded, its memory given back|!=bad|!exit=returned" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app-exit.efi
run x86_64-app-4g "efi: image of 12288 bytes loaded at 0x0000000010000000|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: image returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M q35 -m 4G -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
# x86-64 firmware console: an app writes through ConOut (UTF-8 and ANSI
# on COM1), checks its one mode, waits on a timer event and stalls, then
# reads keys typed on the serial line through WaitForKey and
# ReadKeyStroke: a, b, up, delete, q
SERIAL_IN=$TEST_DIR/x86_64-con.keys
run x86_64-con "efi: TSC at|APP con: hello, ☺|modes=ok|0;1;33;44m|[4;6H cursor=ok|timer=ok|key=0000/0061 key=0000/0062 key=0001/0000 key=0008/0000 key=0000/0071|efi: image returned 0x0|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-con.efi
SERIAL_IN=
run x86_64-app-junk "efi: LoadImage failed: 0x8000000000000001, halting" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86-boot.bin

# x86-64 firmware: Block I/O on virtio-blk, GPT ESP, FAT32 Simple File
# System, and boot of \\EFI\\BOOT\\BOOTX64.EFI (the same PE32+ app).
# q35 defaults disable-legacy=on for virtio; force the transitional I/O BAR
# our Block I/O driver programs (same interface as the x86 virtio-blk path).
run x86_64-fat "disk: virtio-blk at PCI|disk: GPT,|fs: FAT32 on partition|efi: booting \\EFI\\BOOT\\BOOTX64.EFI|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTX64.EFI returned 0x1234|fw: halting|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-esp.img,snapshot=on \
    -device virtio-blk-pci,drive=esp,disable-legacy=off,disable-modern=on

# x86-64 firmware: GOP (Bochs DISPI), GetTime, variables, ExitBootServices.
# The app shuts down via ResetSystem after EBS (isa-debug-exit → status 1).
run x86_64-gop "gop: 800x600x32 at|APP gop mode=ok fb=ok blt=ok time=ok var=ok|efi: ExitBootServices|APP ebs=ok|@exit=1|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-gop.efi

# x86-64 firmware CSM (C3): with nothing to boot the UEFI way, the legacy
# x86-pc BIOS built into the image goes to shadow RAM at F0000 (PAM on
# i440FX and Q35), the CPU leaves long mode for its reset vector, and it
# boots like qemu-system-i386 -bios x86/squirrel-bios.rom: the S2OK
# stand-in from an IDE disk, a boot sector from q35's AHCI port with VGA
# text mode back after GOP, the floppy A: and an El Torito CD
run x86_64-csm "csm: legacy BIOS (64 KB) in shadow RAM at F0000, PAM at 0x59|csm: leaving long mode for F000:FFF0|SquirrelBIOS x86|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok disk=absent|!boot halted" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd \
    -drive format=raw,file=$TEST_DIR/x86-osload.bin,if=ide,snapshot=on
# ... and with four CPUs, the APs parked (INIT) before long mode ends
run x86_64-csm-smp "smp: 4 CPUs, xAPIC|csm: leaving long mode for F000:FFF0|kernel loaded at 0x00008000, jumping|osload=ok dl=ok flags=ok memmap=ok sig=ok e820=ok disk=absent|!boot halted" \
    "$QEMU_X86_64" -M pc -smp 4 -bios x86_64/squirrel-bios.fd \
    -drive format=raw,file=$TEST_DIR/x86-osload.bin,if=ide,snapshot=on
run x86_64-csm-q35 "gop: 800x600x32 at|csm: legacy BIOS (64 KB) in shadow RAM at F0000, PAM at 0x90|SquirrelBIOS x86|disk: SATA disk on AHCI port 0|OS-ENTRY x86 dl=ok memmap=ok flags=ok vga=ok" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,snapshot=on
run x86_64-csm-floppy "csm: leaving long mode for F000:FFF0|booting from floppy A:|OS-ENTRY x86 floppy floppy=ok|!floppy=bad" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd \
    -drive format=raw,file=$TEST_DIR/x86-floppy.img,if=floppy,snapshot=on
run x86_64-csm-cd "csm: leaving long mode for F000:FFF0|booting from CD-ROM|OS-ENTRY x86 cd cd=ok" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd \
    -cdrom $TEST_DIR/x86-cd.iso

# x86-64 firmware: SMP (C4). INIT-SIPI-SIPI brings up every AP; an app
# drives them through the MP services protocol (all at once, one by one,
# one AP, WhoAmI on an AP, a timeout that restarts the AP, disabling one)
# and checks the topology from CPUID and the MPS table. Also 64 CPUs, the
# PC machine, one CPU, and the x2APIC build (QEMU's TCG before 9.0 has no
# x2APIC, so it may stay in xAPIC mode and says so)
MP_OK="flags=ok bsp=ok all=ok single=ok this=ok self=ok timeout=ok disable=ok mps=ok"
run x86_64-mp "smp: 4 CPUs, xAPIC, BSP APIC ID 0|~^smp: MP table at 0x[0-9a-f]{16}, 23 entries|APP mp n=4 en=4 who=0 info=0:0.0.0,1:0.1.0,2:1.0.0,3:1.1.0 $MP_OK|efi: image returned 0x0|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 4,sockets=2,cores=2 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
run x86_64-mp-smt "APP mp n=8 en=8 who=0 info=0:0.0.0,1:0.0.1,2:0.1.0,3:0.1.1,4:1.0.0,5:1.0.1,6:1.1.0,7:1.1.1 $MP_OK|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 8,sockets=2,cores=2,threads=2 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
run x86_64-mp-64 "smp: 64 CPUs, xAPIC|APP mp n=64 en=64 who=0|,63:3.15.0 $MP_OK|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 64,sockets=4,cores=16 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
run x86_64-mp-pc "smp: 2 CPUs, xAPIC|APP mp n=2 en=2 who=0 info=0:0.0.0,1:0.1.0 $MP_OK|!=bad" \
    "$QEMU_X86_64" -M pc -smp 2 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
run x86_64-mp-up "smp: 1 CPU, xAPIC, BSP APIC ID 0|APP mp n=1 en=1 who=0 info=0:0.0.0 flags=ok bsp=ok aps=none notstarted=ok mps=ok|efi: image returned 0x0|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
run x86_64-x2apic-cfg "~^smp: 4 CPUs, x2?APIC( \(the CPU has no x2APIC\))?, BSP APIC ID 0|APP mp n=4 en=4 who=0 info=|$MP_OK|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 4 -cpu max -bios build/x86_64-uefi/x2apic/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-mp.efi
# x86-64 firmware: ACPI 6.x and SMBIOS 3.x (C2). An acpidump-style app
# finds the RSDP and the SMBIOS 3 entry point among the configuration
# tables, checks every checksum, the FADT's DSDT (generated AML) and
# FACS, counts the MADT's CPUs and walks the SMBIOS structures. q35 has
# an MCFG for its ECAM window; i440fx has none.
run x86_64-acpi "acpi: FACP APIC HPET MCFG BERT HEST DSDT|~^acpi: RSDP at 0x[0-9a-f]+, XSDT at 0x[0-9a-f]+, 2 CPUs, q35 \(ICH9\), PM at 0x600|smbios: 3.0 entry point at|APP acpi rsdp=ok xsdt=ok sums=ok facp=ok dsdt=ok facs=ok madt=ok cpus=2|APP acpi hpet=ok mcfg=ok|APP smbios sm3=ok walk=ok types=ok|efi: image returned 0x0|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 2 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-acpi.efi
run x86_64-acpi-pc "acpi: FACP APIC HPET BERT HEST DSDT|i440fx (PIIX4), PM at 0x600|APP acpi rsdp=ok xsdt=ok sums=ok facp=ok dsdt=ok facs=ok madt=ok cpus=3|APP acpi hpet=ok mcfg=none|APP smbios sm3=ok walk=ok types=ok|!=bad" \
    "$QEMU_X86_64" -M pc -smp 3 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-acpi.efi
# Extra PCI root buses (F1): QEMU pxb-pcie (q35) and pxb (pc) expanders
# become DSDT host bridges PCxx with _BBN, their buses up to the next
# root, and I/O and memory windows taken out of PCI0's
run x86_64-pxb "pci: root bus 40-7f (PC40), I/O 6000-6fff, memory e0000000-e3ffffff|pci: root bus 80-ff (PC80), I/O 7000-7fff, memory e4000000-e7ffffff|APP acpi rsdp=ok xsdt=ok sums=ok facp=ok dsdt=ok facs=ok madt=ok cpus=2|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 2 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-acpi.efi \
    -device pxb-pcie,id=pxb1,bus_nr=0x80,bus=pcie.0 -device pcie-root-port,id=rp1,bus=pxb1,chassis=1 \
    -device pxb-pcie,id=pxb2,bus_nr=0x40,bus=pcie.0 -device pcie-root-port,id=rp2,bus=pxb2,chassis=2
run x86_64-pxb-pc "pci: root bus 20-ff (PC20), I/O 6000-6fff, memory e0000000-e3ffffff|APP acpi rsdp=ok xsdt=ok sums=ok facp=ok dsdt=ok facs=ok madt=ok cpus=1|!=bad" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-acpi.efi \
    -device pxb,id=b1,bus_nr=0x20,bus=pci.0 -device e1000,bus=b1
# ResetSystem(EfiResetShutdown) without isa-debug-exit: SLP_TYP from
# \_S5 on the PM block the firmware opened powers QEMU off (exit 0)
run x86_64-s5 "APP ebs=ok|@exit=0|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-gop.efi
run x86_64-s5-pc "APP ebs=ok|@exit=0|!=bad" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-gop.efi

# x86-64 firmware: verify-only Secure Boot (C4). Our test keys and lists
# (tests/mksigned.py) go in through fw_cfg as db and dbx. An Authenticode
# signature verifies against a db X.509 certificate or RSA-2048 key, or the
# image's SHA-256 is in db; unsigned, wrongly signed and tampered images,
# hashes and keys in dbx are refused with EFI_SECURITY_VIOLATION. The ESP
# path checks BOOTX64.EFI the same way, and the CSM stays off.
SB=$TEST_DIR/sb
SB_DB=name=opt/org.squirrelworks/db
SB_DBX=name=opt/org.squirrelworks/dbx
SB_APP="APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: image returned 0x1234"
SB_NO="efi: LoadImage failed: 0x800000000000001a, halting|!APP x86_64"
run x86_64-sb-x509 "secure: Secure Boot on (fixed keys)|secure: db: 0 SHA-256, 0 RSA-2048, 1 X.509|secure: image verified: signed by db entry 1 (X.509)|$SB_APP" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-rsa "secure: db: 0 SHA-256, 1 RSA-2048, 0 X.509|secure: image verified: signed by db entry 1 (RSA-2048)|$SB_APP" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-rsa.esl -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-mixed "secure: db: 1 SHA-256, 0 RSA-2048, 1 X.509|secure: image verified: signed by db entry 2 (X.509)|$SB_APP" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-mixed.esl -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-hash "secure: db: 1 SHA-256, 0 RSA-2048, 0 X.509|secure: image verified: its SHA-256 is in db|$SB_APP" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-hash.esl -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sb-unsigned "secure: image refused: not signed, and its SHA-256 is not in db|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sb-wrongkey "secure: image refused: signed by a key that is not in db|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $X64_BOOT,file=$SB/sb-wrongkey.efi
run x86_64-sb-tampered "secure: image refused: a signature for another image (hash mismatch)|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $X64_BOOT,file=$SB/sb-tampered.efi
run x86_64-sb-dbx-hash "secure: dbx: 1 SHA-256, 0 RSA-2048, 0 X.509|secure: image refused: its SHA-256 is in dbx|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $SB_DBX,file=$SB/sb-dbx-hash.esl -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-dbx-key "secure: image refused: signed by a key in dbx|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $SB_DBX,file=$SB/sb-dbx-x509.esl -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-esp "fs: FAT32 on partition|efi: booting \\EFI\\BOOT\\BOOTX64.EFI|secure: image verified: signed by db entry 1 (X.509)|efi: BOOTX64.EFI returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-sb-esp.img,snapshot=on \
    -device virtio-blk-pci,drive=esp,disable-legacy=off,disable-modern=on
run x86_64-sb-nocsm "secure: Secure Boot on (fixed keys)|csm: off while Secure Boot is on|!csm: leaving long mode" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl \
    -drive format=raw,file=$TEST_DIR/x86-osload.bin,if=ide,snapshot=on
run x86_64-sb-baddb "secure: opt/org.squirrelworks/db is not a list of EFI_SIGNATURE_LISTs|secure: Secure Boot on (fixed keys), but a key database is unreadable: every image is refused|secure: image refused: the db or dbx given could not be read, so nothing is trusted|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$TEST_DIR/x86-boot.bin -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sb-baddbx "secure: opt/org.squirrelworks/dbx is not a list of EFI_SIGNATURE_LISTs|secure: image refused: the db or dbx given could not be read, so nothing is trusted|$SB_NO" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $SB_DBX,file=$TEST_DIR/x86-boot.bin -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi

# x86-64 firmware: NUMA and error tables (F1/F2). With QEMU -numa the
# SRAT gives each CPU its node and each node's RAM (split around the PCI
# hole with 4 GB), the SLIT 10/20 or the distances given through fw_cfg;
# the MADT follows the CPUs SMP brought up; BERT and HEST are empty
# stubs. Without -numa there is no SRAT or SLIT. An app checks it all.
numa_nodes() {     # numa_nodes N MB_PER_NODE CPUS_PER_NODE
    i=0
    while [ "$i" -lt "$1" ]; do
        printf ' -object memory-backend-ram,id=nm%d,size=%dM -numa node,nodeid=%d,cpus=%d-%d,memdev=nm%d' \
            $i $2 $i $((i * $3)) $((i * $3 + $3 - 1)) $i
        i=$((i + 1))
    done
}
NUMA_APP="-fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-numa.efi"
run x86_64-numa "numa: 2 nodes: 96 MB 160 MB|acpi: FACP APIC HPET MCFG SRAT SLIT HMAT BERT HEST DSDT|APP numa sums=ok madt=4/0 srat=4 cpu=0,0,1,1 mem=0:0+96,1:96+160 slit=2:1020,2010 hmat=0>0,1>1 lb=0 cache=0|APP numa bert=0 hest=2/10|!=bad" \
    "$QEMU_X86_64" -M q35 -m 256 -smp 4,sockets=2,cores=2 \
    -object memory-backend-ram,id=nm0,size=96M -numa node,nodeid=0,cpus=0-1,memdev=nm0 \
    -object memory-backend-ram,id=nm1,size=160M -numa node,nodeid=1,cpus=2-3,memdev=nm1 \
    -bios x86_64/squirrel-bios.fd $NUMA_APP
# shellcheck disable=SC2046
run x86_64-numa-4g "numa: 4 nodes: 1024 MB 1024 MB 1024 MB 1024 MB|APP numa sums=ok madt=8/0 srat=8 cpu=0,0,1,1,2,2,3,3 mem=0:0+1024,1:1024+1024,2:4096+1024,3:5120+1024 slit=4:10202020,20102020,20201020,20202010|!=bad" \
    "$QEMU_X86_64" -M q35 -m 4G -smp 8 $(numa_nodes 4 1024 2) -bios x86_64/squirrel-bios.fd $NUMA_APP
# shellcheck disable=SC2046
run x86_64-numa-64 "smp: 64 CPUs|numa: 4 nodes|APP numa sums=ok madt=64/0 srat=64 cpu=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,|,3,3 mem=0:0+128,1:128+128,2:256+128,3:384+128 slit=4:|APP numa bert=0 hest=2/10|!=bad" \
    "$QEMU_X86_64" -M q35 -m 512 -smp 64 $(numa_nodes 4 128 16) -bios x86_64/squirrel-bios.fd $NUMA_APP
# shellcheck disable=SC2046
run x86_64-numa-dist "APP numa sums=ok madt=2/0 srat=2 cpu=0,1 mem=0:0+128,1:128+128 slit=2:1031,3110|!=bad" \
    "$QEMU_X86_64" -M pc -m 256 -smp 2 $(numa_nodes 2 128 1) -bios x86_64/squirrel-bios.fd $NUMA_APP \
    -fw_cfg name=opt/org.squirrelworks/numa-distance,file=$TEST_DIR/x86_64-numa-dist.bin
# HMAT (F1): QEMU's own HMAT and SLIT (-machine hmat=on, -numa hmat-lb,
# hmat-cache and dist) are taken from the tables it offers in
# etc/acpi/tables; node 2 has memory and no CPUs. Without hmat=on the
# HMAT names each node's initiator, the memory-only node getting the
# nearest node with CPUs by the SLIT.
NUMA3="-object memory-backend-ram,id=nm0,size=128M -object memory-backend-ram,id=nm1,size=128M -object memory-backend-ram,id=nm2,size=64M"
HMAT_LB="-numa hmat-lb,initiator=0,target=0,hierarchy=memory,data-type=access-latency,latency=5 -numa hmat-lb,initiator=0,target=0,hierarchy=memory,data-type=access-bandwidth,bandwidth=400M \
    -numa hmat-lb,initiator=0,target=1,hierarchy=memory,data-type=access-latency,latency=10 -numa hmat-lb,initiator=0,target=1,hierarchy=memory,data-type=access-bandwidth,bandwidth=200M \
    -numa hmat-lb,initiator=0,target=2,hierarchy=memory,data-type=access-latency,latency=30 -numa hmat-lb,initiator=0,target=2,hierarchy=memory,data-type=access-bandwidth,bandwidth=100M \
    -numa hmat-lb,initiator=1,target=0,hierarchy=memory,data-type=access-latency,latency=10 -numa hmat-lb,initiator=1,target=0,hierarchy=memory,data-type=access-bandwidth,bandwidth=200M \
    -numa hmat-lb,initiator=1,target=1,hierarchy=memory,data-type=access-latency,latency=5 -numa hmat-lb,initiator=1,target=1,hierarchy=memory,data-type=access-bandwidth,bandwidth=400M \
    -numa hmat-lb,initiator=1,target=2,hierarchy=memory,data-type=access-latency,latency=40 -numa hmat-lb,initiator=1,target=2,hierarchy=memory,data-type=access-bandwidth,bandwidth=100M"
run x86_64-numa-hmat "numa: HMAT from QEMU (-numa hmat-lb)|acpi: FACP APIC HPET MCFG SRAT SLIT HMAT BERT HEST DSDT|APP numa sums=ok madt=2/0 srat=2 cpu=0,1 mem=0:0+128,1:128+128,2:256+64 slit=3:103117,311028,172810 hmat=0>0,1>1,0>2 lb=2 cache=1|!=bad" \
    "$QEMU_X86_64" -M q35,hmat=on -m 320 -smp 2 $NUMA3 \
    -numa node,nodeid=0,cpus=0,memdev=nm0,initiator=0 -numa node,nodeid=1,cpus=1,memdev=nm1,initiator=1 \
    -numa node,nodeid=2,memdev=nm2,initiator=0 \
    -numa dist,src=0,dst=1,val=31 -numa dist,src=0,dst=2,val=17 -numa dist,src=1,dst=2,val=28 \
    $HMAT_LB -numa hmat-cache,node-id=2,size=8M,level=1,associativity=direct,policy=write-back,line=64 \
    -bios x86_64/squirrel-bios.fd $NUMA_APP
run x86_64-numa-initiator "numa: HMAT with the initiator of each node|APP numa sums=ok madt=2/0 srat=2 cpu=0,1 mem=0:0+128,1:128+128,2:256+64 slit=3:102140,211017,401710 hmat=0>0,1>1,1>2 lb=0 cache=0|!=bad" \
    "$QEMU_X86_64" -M pc -m 320 -smp 2 $NUMA3 \
    -numa node,nodeid=0,cpus=0,memdev=nm0 -numa node,nodeid=1,cpus=1,memdev=nm1 -numa node,nodeid=2,memdev=nm2 \
    -numa dist,src=0,dst=1,val=21 -numa dist,src=0,dst=2,val=40 -numa dist,src=1,dst=2,val=17 \
    -bios x86_64/squirrel-bios.fd $NUMA_APP
run x86_64-numa-none "APP numa sums=ok madt=2/0 srat=none slit=none hmat=none|APP numa bert=0 hest=2/10|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 2 -bios x86_64/squirrel-bios.fd $NUMA_APP
# With NUMA, an expander's numa_node becomes its _PXM, read from QEMU's
# own DSDT
run x86_64-pxb-numa "pci: root bus 40-7f (PC40), I/O|pci: root bus 80-ff (PC80), node 1, I/O 7000-7fff|APP numa sums=ok|!=bad" \
    "$QEMU_X86_64" -M q35 -m 256 -smp 2 $(numa_nodes 2 128 1) -bios x86_64/squirrel-bios.fd $NUMA_APP \
    -device pxb-pcie,id=pxb1,bus_nr=0x80,numa_node=1,bus=pcie.0 -device pcie-root-port,id=rp1,bus=pxb1,chassis=1 \
    -device pxb-pcie,id=pxb2,bus_nr=0x40,bus=pcie.0 -device pcie-root-port,id=rp2,bus=pxb2,chassis=2

# x86-64 firmware: machine checks left in the banks survive a reset and
# become BERT records on the next boot (F2). Once the app has seen an
# empty BERT, the monitor injects a corrected error on CPU 1 and a fatal
# one on CPU 0; the fatal one resets the machine (CR4.MCE is set and
# nothing handles #MC), and the next boot finds both.
MCE_SOCK=$TEST_DIR/x86_64-mce.sock
rm -f "$MCE_SOCK"
python3 tests/monitor.py "$MCE_SOCK" $TEST_DIR/x86_64-mce.log "APP numa bert=0" \
    "mce 1 2 0x9000000000000000 0 0 0" "mce 0 1 0xb600000000000000 0x5 0x123000 0" &
mce_mon=$!
run x86_64-mce "mce: 10 banks, CMCI no, 0 errors from before reset|APP numa bert=0 hest=2/10|mce: 10 banks, CMCI no, 2 errors from before reset|mce: APIC ID 0 bank 1 status 0xb600000000000000 addr 0x123000 misc 0x0 (fatal)|mce: APIC ID 1 bank 2 status 0x9000000000000000 addr 0x0 misc 0x0 (corrected)|APP numa bert=2 hest=2/10|!=bad" \
    "$QEMU_X86_64" -M q35 -smp 2 -bios x86_64/squirrel-bios.fd $NUMA_APP \
    -chardev socket,id=mon0,path=$MCE_SOCK,server=on,wait=off -mon chardev=mon0,mode=readline
kill "$mce_mon" 2>/dev/null; wait "$mce_mon" 2>/dev/null
rm -f "$MCE_SOCK"

# x86-64 firmware: only the runtime pages stay the OS's after
# ExitBootServices (runtime services code, then their data with the
# system and configuration tables); the rest of the image is boot
# services code. The GOP case above calls ResetSystem after exit.
run x86_64-rt-split "efi: 0000000000200000-|~^efi: 0000000000[0-9a-f]{6}-0000000000[0-9a-f]{6} RT code|~^efi: 0000000000[0-9a-f]{6}-0000000000[0-9a-f]{6} RT data|~^efi: 0000000000200000-0000000000[0-9a-f]{6} BS code|efi: memory services ok" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd

# AArch64 UEFI-class firmware (roadmap D1, PLATFORM=aarch64-uefi): the
# shared UEFI core in common/efi on QEMU virt. It copies itself from flash
# to RAM, maps memory 1:1 with caches on, finds the PL011, PL031, PSCI and
# the GIC in the device tree, builds the UEFI memory map from /memory,
# and halts with nothing to boot.
ARM_FD=arm/uefi/squirrel-bios.fd
run aarch64-uefi "SquirrelBIOS 2.1.0 (aarch64-uefi)|fdt: device tree at 0x0000000040000000|~^cpu: Arm part d07 .*, EL1, 44-bit physical addresses|mmu: identity map, caches on|psci: v1.1 via hvc|rtc: PL031 at 0x0000000009010000|gic: v2, distributor at 0x0000000008000000|fw: running at 0x0000000040100000-|mem: 0000000040000000-0000000047ffffff usable|mem: 128 MB usable (from the device tree)|fw_cfg: present|~^efi: system table at 0x[0-9a-f]{16}, UEFI 2.70|efi: 0000000040000000-00000000400fffff BS data|~^efi: 0000000040100000-[0-9a-f]{16} RT code|~^efi: 0000000009010000-0000000009010fff MMIO|efi: memory services ok|efi: device tree installed as a configuration table|fw: nothing to boot (no -kernel, no opt/org.squirrelworks/boot.efi, no boot disk), halting" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD
# The C stand-in app (tests/efi-app.c) through fw_cfg: relocated from 256
# MB, it checks the tables, its loaded image, the relocation, pool and
# memory map, ConOut, a timer event, GetTime, variables and the device
# tree configuration table; then returns, or calls Exit
run aarch64-uefi-app "efi: image of 32768 bytes loaded at|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP con=ok timer=ok time=ok var=ok dtb=ok acpi=ok FACP,APIC,GTDT,SPCR,DBG2,MCFG|efi: image returned 0x1234|efi: image unloaded, its memory given back|fw: halting|!=bad|!APP opts" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app.efi
run aarch64-uefi-app-exit "APP con=ok timer=ok time=ok var=ok dtb=ok|efi: image returned 0x5678|efi: image unloaded, its memory given back|!=bad|!exit=returned" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app-exit.efi
# As -kernel with -append and -initrd, the way Linux's EFI stub is
# started: LoadOptions, and the initrd through LocateDevicePath + LoadFile2
run aarch64-uefi-kernel "fw: -kernel image, 32768 bytes|fw: -initrd, 3000 bytes|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP opts=\"console=ttyAMA0 squirrel=1\" initrd=3000 data=ok|efi: image returned 0x1234|!=bad" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -kernel $TEST_DIR/arm-efi-app.efi \
    -initrd $TEST_DIR/arm-efi-initrd.img -append "console=ttyAMA0 squirrel=1"
# ExitBootServices, then ResetSystem(shutdown) through the runtime
# services: PSCI SYSTEM_OFF, so QEMU exits 0. At EL2 (virtualization=on)
# with GICv3 and 2 GB, PSCI goes through smc.
run aarch64-uefi-ebs "APP con=ok timer=ok time=ok var=ok dtb=ok|efi: ExitBootServices|@exit=0|!=bad" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app-ebs.efi
run aarch64-uefi-el2 "~^cpu: .*, EL2, |psci: v1.1 via smc|gic: v3 (arch 3), distributor at 0x0000000008000000, 288 interrupt lines, redistributor awake|mem: 2048 MB usable (from the device tree)|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP con=ok timer=ok time=ok var=ok dtb=ok|efi: ExitBootServices|@exit=0|!=bad" \
    "$QEMU_ARM" -M virt,virtualization=on,gic-version=3 -cpu max -m 2G -bios $ARM_FD \
    -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app-ebs.efi
# ACPI tables (SBBR set) built from the device tree: GICv3 with an ITS,
# 4 CPUs; the app follows RSDP -> XSDT -> each table and the FADT's DSDT
# and checks every signature and checksum
run aarch64-uefi-acpi "~^acpi: RSDP at 0x[0-9a-f]{16}: FACP APIC GTDT SPCR DBG2 MCFG IORT, DSDT of [0-9]+ bytes; 4 CPUs, GICv3|APP con=ok timer=ok time=ok var=ok dtb=ok acpi=ok FACP,APIC,GTDT,SPCR,DBG2,MCFG,IORT|efi: image returned 0x1234|!=bad" \
    "$QEMU_ARM" -M virt,gic-version=3 -cpu max -smp 4 -bios $ARM_FD \
    -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app.efi
# An x86-64 image is a PE32+ for another machine: refused
run aarch64-uefi-wrong-arch "efi: LoadImage failed: 0x8000000000000003, halting" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi

# From a virtio-blk disk on virtio-mmio (the legacy register layout, QEMU's
# default): GPT, the FAT32 ESP, \EFI\BOOT\BOOTAA64.EFI
run aarch64-uefi-disk "~^disk: virtio-blk at 0x[0-9a-f]{16} \(virtio-mmio v1\), 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|fs: FAT32 on partition|efi: booting \\EFI\\BOOT\\BOOTAA64.EFI (32768 bytes)|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP con=ok timer=ok time=ok var=ok dtb=ok|efi: BOOTAA64.EFI returned 0x1234|!=bad" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device virtio-blk-device,drive=d0

# RISC-V UEFI-class firmware (roadmap D3, PLATFORM=riscv64-uefi): the SBI
# in M-mode enters the shared UEFI core in S-mode at 2 MB. The core finds
# the NS16550A, the timebase and the goldfish RTC in the device tree,
# talks to the SBI (spec 2.0), keeps the SBI's region reserved, installs
# RISCV_EFI_BOOT_PROTOCOL, and halts with nothing to boot.
RV_UEFI=riscv/uefi/squirrel-bios.bin
run riscv64-uefi "SquirrelBIOS RISC-V|entering the UEFI core at 0x0000000080200000, next_mode=1|dt: firmware region 0x80000000-0x801fffff reserved (no-map)|SquirrelBIOS 2.1.0 (riscv64-uefi)|cpu: hart 0, S-mode, SBI 2.0 implementation 0x5351|uart: NS16550A at 0x0000000010000000|timer: time CSR at 10000000 Hz|rtc: goldfish at 0x0000000000101000|fw: running at 0x0000000080200000-|mem: 128 MB usable (from the device tree)|efi: 0000000080000000-00000000801fffff reserved|~^efi: 0000000080200000-[0-9a-f]{16} RT code|efi: memory services ok|efi: device tree installed as a configuration table|efi: RISCV_EFI_BOOT_PROTOCOL installed (boot hart 0)|fw: nothing to boot (no -kernel, no opt/org.squirrelworks/boot.efi, no boot disk), halting" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI
# The C stand-in app through fw_cfg, returning and calling Exit
run riscv64-uefi-app "efi: image of 28672 bytes loaded at|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP con=ok timer=ok time=ok var=ok dtb=ok acpi=none|efi: image returned 0x1234|efi: image unloaded, its memory given back|fw: halting|!=bad|!APP opts" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -fw_cfg $X64_BOOT,file=$TEST_DIR/riscv-efi-app.efi
run riscv64-uefi-app-exit "APP con=ok timer=ok time=ok var=ok dtb=ok|efi: image returned 0x5678|efi: image unloaded, its memory given back|!=bad|!exit=returned" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -fw_cfg $X64_BOOT,file=$TEST_DIR/riscv-efi-app-exit.efi
# As -kernel: QEMU loads it at the 2 MB boundary after the firmware file
# and the SBI passes it on; command line and initrd come from /chosen
run riscv64-uefi-kernel "fw: -kernel PE image at 0x0000000080400000, 28672 bytes|fw: -append \"console=ttyS0 squirrel=1\" (/chosen bootargs)|fw: -initrd, 3000 bytes|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP opts=\"console=ttyS0 squirrel=1\" initrd=3000 data=ok|efi: image returned 0x1234|!=bad" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -kernel $TEST_DIR/riscv-efi-app.efi \
    -initrd $TEST_DIR/arm-efi-initrd.img -append "console=ttyS0 squirrel=1"
# ExitBootServices, then ResetSystem(shutdown): SBI SRST, QEMU exits 0;
# with 4 harts, the secondaries stay parked in the SBI
run riscv64-uefi-ebs "APP con=ok timer=ok time=ok var=ok dtb=ok|efi: ExitBootServices|@exit=0|!=bad" \
    "$QEMU_RISCV" -M virt -smp 4 -bios $RV_UEFI -fw_cfg $X64_BOOT,file=$TEST_DIR/riscv-efi-app-ebs.efi
# An AArch64 image is refused
run riscv64-uefi-wrong-arch "efi: LoadImage failed: 0x8000000000000003, halting" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -fw_cfg $X64_BOOT,file=$TEST_DIR/arm-efi-app.efi
# From a virtio-blk disk with the version 2 registers: BOOTRISCV64.EFI is
# not 8.3, so it is found by its long name. A disk holding only
# BOOTAA64.EFI has nothing for RISC-V.
run riscv64-uefi-disk "disk: virtio-blk at 0x0000000010008000 (virtio-mmio v2), 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|efi: booting \\EFI\\BOOT\\BOOTRISCV64.EFI (28672 bytes)|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|APP con=ok timer=ok time=ok var=ok dtb=ok|efi: BOOTRISCV64.EFI returned 0x1234|!=bad" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -global virtio-mmio.force-legacy=false \
    -drive if=none,file=$TEST_DIR/riscv-esp.img,format=raw,id=d0 -device virtio-blk-device,drive=d0
run riscv64-uefi-disk-other-arch "disk: virtio-blk at 0x0000000010008000 (virtio-mmio v1)|fs: FAT32 on partition|fw: nothing to boot (no -kernel, no opt/org.squirrelworks/boot.efi, no boot disk), halting|!APP riscv64" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device virtio-blk-device,drive=d0
# x86-64 firmware: the runtime services after ExitBootServices, with
# every boot services page overwritten by the app (as an OS may reuse
# them) and the app's own page tables and stack. ResetSystem then powers
# off (isa-debug-exit -> status 1).
for m in q35 pc; do
run x86_64-rt-after-exit-$m "efi: ExitBootServices|APP rt pages=ok time=ok var=ok next=ok|@exit=1|!=bad" \
    "$QEMU_X86_64" -M $m -smp 2 -bios x86_64/squirrel-bios.fd \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-rt.efi
done

# PCI disks (roadmap D4): the core walks the device tree's ECAM host
# bridge, places BARs, and boots from NVMe or virtio-blk-pci; with two
# disks it takes the ESP that has this architecture's boot file.
run aarch64-uefi-nvme "disk: NVMe at PCI 00:02.0, 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|efi: booting \\EFI\\BOOT\\BOOTAA64.EFI|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTAA64.EFI returned 0x1234|!=bad" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device nvme,drive=d0,serial=sq1
run aarch64-uefi-virtio-pci "disk: virtio-blk at PCI 00:02.0 (virtio-pci), 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTAA64.EFI returned 0x1234|!=bad" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device virtio-blk-pci,drive=d0
run aarch64-uefi-two-disks "disk: NVMe at PCI 00:02.0, 131072 sectors|disk: virtio-blk at PCI 00:03.0 (virtio-pci), 131072 sectors|APP aarch64 st=ok|efi: BOOTAA64.EFI returned 0x1234|!APP riscv64" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD \
    -drive if=none,file=$TEST_DIR/riscv-esp.img,format=raw,id=d1 -device nvme,drive=d1,serial=sq1 \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device virtio-blk-pci,drive=d0
run riscv64-uefi-nvme "disk: NVMe at PCI 00:01.0, 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|efi: booting \\EFI\\BOOT\\BOOTRISCV64.EFI|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTRISCV64.EFI returned 0x1234|!=bad" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI \
    -drive if=none,file=$TEST_DIR/riscv-esp.img,format=raw,id=d0 -device nvme,drive=d0,serial=sq1
run riscv64-uefi-virtio-pci "disk: virtio-blk at PCI 00:01.0 (virtio-pci), 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTRISCV64.EFI returned 0x1234|!=bad" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI \
    -drive if=none,file=$TEST_DIR/riscv-esp.img,format=raw,id=d0 -device virtio-blk-pci,drive=d0

# AHCI SATA disks (D4) on every architecture, and the shared NVMe and
# modern virtio-blk-pci drivers on x86-64 (q35's own ICH9 AHCI; a CD on
# AHCI port 0 is passed over)
run aarch64-uefi-ahci "disk: AHCI at PCI 00:02.0 port 2, 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|APP aarch64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTAA64.EFI returned 0x1234|!=bad|!port 0" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios $ARM_FD -device ahci,id=ahci \
    -drive if=none,id=cd,media=cdrom -device ide-cd,drive=cd,bus=ahci.0 \
    -drive if=none,file=$TEST_DIR/arm-esp.img,format=raw,id=d0 -device ide-hd,drive=d0,bus=ahci.2
run riscv64-uefi-ahci "disk: AHCI at PCI 00:01.0 port 0, 131072 sectors|APP riscv64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTRISCV64.EFI returned 0x1234|!=bad" \
    "$QEMU_RISCV" -M virt -bios $RV_UEFI -device ahci,id=ahci \
    -drive if=none,file=$TEST_DIR/riscv-esp.img,format=raw,id=d0 -device ide-hd,drive=d0,bus=ahci.0
run x86_64-uefi-ahci-q35 "disk: AHCI at PCI 00:1f.2 port 1, 131072 sectors|disk: partition 1 LBA 2048..131038 (ESP)|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTX64.EFI returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-esp.img,snapshot=on -device ide-hd,drive=esp,bus=ide.1
run x86_64-uefi-nvme-q35 "disk: NVMe at PCI 00:03.0, 131072 sectors|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTX64.EFI returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-esp.img,snapshot=on -device nvme,drive=esp,serial=sq1
run x86_64-uefi-nvme-pc "disk: NVMe at PCI 00:04.0, 131072 sectors|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTX64.EFI returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M pc -bios x86_64/squirrel-bios.fd \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-esp.img,snapshot=on -device nvme,drive=esp,serial=sq1
run x86_64-uefi-virtio-modern "disk: virtio-blk at PCI 00:03.0 (virtio-pci), 131072 sectors|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: BOOTX64.EFI returned 0x1234|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd \
    -drive if=none,id=esp,format=raw,file=$TEST_DIR/x86_64-esp.img,snapshot=on -device virtio-blk-pci,drive=esp,disable-legacy=on

# Non-volatile variables (C5): the firmware as pflash unit 0 and a store
# as unit 1. Three boots against one store: a boot counter goes up, an
# 8000-byte variable set on the first is read back on the next ones, a
# volatile one is gone, and 200 rewrites of a 1000-byte variable fill a
# bank and move the store into the other. The runtime services then
# write flash after ExitBootServices with boot memory overwritten. With
# -bios there is no store and variables last until reset.
NV_STORE=$TEST_DIR/x86_64-vars.fd
NV_FLASH="-drive if=pflash,format=raw,unit=0,readonly=on,file=x86_64/squirrel-bios.fd -drive if=pflash,format=raw,unit=1,file=$NV_STORE"
rm -f "$NV_STORE"
truncate -s 256K "$NV_STORE"
run x86_64-nv-1 "vars: new store in 256 KB of flash at 0x00000000ffec0000|APP nv before=none after=1 big=none vol=none churn=ok|fw: halting|!=bad" \
    "$QEMU_X86_64" -M q35 $NV_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-nv.efi
run x86_64-nv-2 "~^vars: 3 variables from 256 KB of flash at 0x00000000ffec0000 \(bank 1, generation 2\)|APP nv before=1 after=2 big=ok vol=none churn=ok|fw: halting|!=bad" \
    "$QEMU_X86_64" -M pc $NV_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-nv.efi
run x86_64-nv-3 "APP nv before=2 after=3 big=ok vol=none churn=ok|fw: halting|!=bad" \
    "$QEMU_X86_64" -M q35 $NV_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-nv.efi
run x86_64-nv-after-exit "~^vars: 3 variables from|efi: ExitBootServices|APP rt pages=ok time=ok var=ok next=ok|@exit=1|!=bad" \
    "$QEMU_X86_64" -M q35 $NV_FLASH -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-rt.efi
run x86_64-nv-after-exit-kept "~^vars: 4 variables from|APP nv before=3 after=4 big=ok|!=bad" \
    "$QEMU_X86_64" -M q35 $NV_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-nv.efi
run x86_64-nv-none "vars: no flash store, variables are kept until reset|APP nv before=none after=1 big=none vol=none churn=ok|!=bad" \
    "$QEMU_X86_64" -M q35 -bios x86_64/squirrel-bios.fd -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-nv.efi

# x86-64 firmware: enrolled Secure Boot keys (C5). Without a fixed db the
# PK, KEK, db and dbx are variables changed only by authenticated writes
# (tests/mksigned.py: key A is the PK, key B the KEK). The fw_cfg files
# enroll/* are written at boot as an OS tool would: in setup mode db, KEK
# and PK go in and the machine is in user mode, kept in flash for the
# next boots. Then a db write signed by a key that is not the KEK or PK,
# or older than the db, is refused; an OS appends an image hash to db
# after ExitBootServices (a copy with a changed byte is refused) and the
# unsigned image now runs; an empty PK signed by the PK puts the machine
# back in setup mode. A fixed db still wins over the store.
SBE_STORE=$TEST_DIR/x86_64-sb-vars.fd
SBE_FLASH="-drive if=pflash,format=raw,unit=0,readonly=on,file=x86_64/squirrel-bios.fd -drive if=pflash,format=raw,unit=1,file=$SBE_STORE"
SBE=name=opt/org.squirrelworks/enroll
SBE_ON="secure: Secure Boot on (user mode: PK and 1 KEK key enrolled)"
rm -f "$SBE_STORE"
truncate -s 256K "$SBE_STORE"
run x86_64-sbe-enroll "secure: enroll db: enrolled|secure: enroll KEK: enrolled|secure: enroll PK: enrolled|$SBE_ON|secure: db: 0 SHA-256, 0 RSA-2048, 1 X.509|secure: image verified: signed by db entry 1 (X.509)|$SB_APP" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $SBE/db,file=$SB/auth-db.auth -fw_cfg $SBE/KEK,file=$SB/auth-KEK.auth \
    -fw_cfg $SBE/PK,file=$SB/auth-PK.auth -fw_cfg $X64_BOOT,file=$SB/sb-signed.efi
run x86_64-sbe-kept "~^vars: 3 variables from|$SBE_ON|secure: image refused: not signed, and its SHA-256 is not in db|$SB_NO" \
    "$QEMU_X86_64" -M pc $SBE_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-badsigner "secure: enroll db: refused: not signed by a key that may change it, or not newer|$SBE_ON|secure: db: 0 SHA-256, 0 RSA-2048, 1 X.509|$SB_NO" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $SBE/db,file=$SB/auth-db-badsigner.auth -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-old "secure: enroll db: refused: not signed by a key that may change it, or not newer|secure: db: 0 SHA-256, 0 RSA-2048, 1 X.509|$SB_NO" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $SBE/db,file=$SB/auth-db-old.auth -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-append-rt "secure: image verified: signed by db entry 1 (X.509)|efi: ExitBootServices|APP authrt pages=ok bad=ok append=ok grown=ok|@exit=1|!=bad" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg $X64_BOOT,file=$SB/sb-authrt.efi
run x86_64-sbe-appended "$SBE_ON|secure: db: 1 SHA-256, 0 RSA-2048, 1 X.509|secure: image verified: its SHA-256 is in db|$SB_APP" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-fixed-wins "secure: Secure Boot on (fixed keys)|secure: db: 0 SHA-256, 0 RSA-2048, 1 X.509|secure: image refused: not signed, and its SHA-256 is not in db|$SB_NO" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $SB_DB,file=$SB/sb-db-x509.esl -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-pk-delete "secure: enroll PK: enrolled|secure: setup mode (no PK): Secure Boot off|$SB_APP" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $SBE/PK,file=$SB/auth-PK-delete.auth -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi
run x86_64-sbe-setup-kept "~^vars: 2 variables from|secure: setup mode (no PK): Secure Boot off|$SB_APP" \
    "$QEMU_X86_64" -M q35 $SBE_FLASH -fw_cfg $X64_BOOT,file=$TEST_DIR/x86_64-app.efi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
