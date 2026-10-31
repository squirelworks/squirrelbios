#!/bin/sh
# SquirrelBIOS smoke tests: boot each firmware under QEMU and check what
# it prints on its serial console. Run through `make test`, which builds
# the firmware and the stand-in OS images in build/tests first.
#
# Each case passes when every expected string appears in the serial log
# before SMOKE_TIMEOUT seconds (default 10). Expected strings are separated
# by '|' and matched literally; one starting with '~' is an extended
# regular expression instead, and one starting with '!' must not appear.

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

    ok=0
    ticks=$((TIMEOUT * 10))
    while [ "$ticks" -gt 0 ] && kill -0 "$qemu" 2>/dev/null; do
        ok=1
        old_ifs=$IFS
        IFS='|'
        for want in $expect; do
            case $want in
            "~"*) grep -qE -- "${want#?}" "$log" || ok=0 ;;
            "!"*) grep -qF -- "${want#?}" "$log" && ok=0 ;;
            *)    grep -qF -- "$want" "$log" || ok=0 ;;
            esac
        done
        IFS=$old_ifs
        [ "$ok" -eq 1 ] && break
        sleep 0.1
        ticks=$((ticks - 1))
    done
    kill "$qemu" 2>/dev/null
    wait "$qemu" 2>/dev/null

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
# ES:DI that has the required regions, sorted and not overlapping
run x86-boot "SquirrelBIOS x86|OS-ENTRY x86 dl=ok memmap=ok flags=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: no disk is reported, not hung on
run x86-no-drive "disk: no drive on primary master|boot halted" \
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

# x86: a CD-ROM as primary master is rejected, not read
run x86-atapi "disk: primary master is ATAPI (CD/DVD), not supported|boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide,media=cdrom

# AArch64: -kernel image entered with the DTB in x0
run arm-boot "SquirrelBIOS ARM|OS-ENTRY arm dtb=ok regs=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-kernel.bin

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
# to hart 1, which is parked and not started, it fails (-smp 2)
run riscv-ipi "OS-ENTRY riscv ipi|ipi=ok|!ipi=bad|!trap mcause=" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-ipi.bin

run riscv-no-kernel "boot: no kernel image|boot halted" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
