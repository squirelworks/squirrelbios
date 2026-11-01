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

set -u

TEST_DIR=build/tests
TIMEOUT=${SMOKE_TIMEOUT:-10}
QEMU_X86=${QEMU_X86:-qemu-system-i386}
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

    "$@" -display none -monitor none -serial "file:$log" >/dev/null 2>&1 &
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
# or run past 0xA0000, are refused before anything is read or jumped to
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

# x86: ACPI RSDP in 0xE0000–0xFFFFF, checksum and RSDT→FACP
run x86-acpi "SquirrelBIOS x86|OS-ENTRY x86 acpi=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-acpi.bin,if=ide

# x86: SMBIOS _SM_ in 0xF0000–0xFFFFF with valid checksums
run x86-smbios "SquirrelBIOS x86|OS-ENTRY x86 smbios=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-smbios.bin,if=ide

# x86: Linux bzImage (HdrS) loaded at 1 MB; PM payload prints bz=ok
run x86-bzimage "SquirrelBIOS x86|bzImage loaded at 0x00100000, jumping|bz=ok|!entered-boot-sector|!boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-bzimage.bin,if=ide

# x86: option ROM at 0xD0000 is scanned and its init prints optrom=ok
run x86-optrom "SquirrelBIOS x86|optrom=ok|OS-ENTRY x86 dl=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -device loader,file=$TEST_DIR/x86-optrom.bin,addr=0xD0000,force-raw=on \
    -drive format=raw,file=$TEST_DIR/x86-one-sector.bin,if=ide

# x86: a CD-ROM as primary master is rejected, not read
run x86-atapi "disk: primary master is ATAPI (CD/DVD), not supported|boot halted" \
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
run riscv-sbi "OS-ENTRY riscv sbi|sbi=ok|!trap mcause=" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-sbi.bin

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

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
