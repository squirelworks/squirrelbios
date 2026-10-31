#!/bin/sh
# SquirrelBIOS smoke tests: boot each firmware under QEMU and check what
# it prints on its serial console. Run through `make test`, which builds
# the firmware and the stand-in OS images in build/tests first.
#
# Each case passes when every expected string appears in the serial log
# before SMOKE_TIMEOUT seconds (default 10).

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
            grep -qF -- "$want" "$log" || ok=0
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

# x86: boot sector reached with DL = 0x80 and the memory map in ES:DI
run x86-boot "SquirrelBIOS x86|OS-ENTRY x86 dl=ok memmap=ok" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom \
    -drive format=raw,file=$TEST_DIR/x86-boot.bin,if=ide

# x86: no disk is reported, not hung on
run x86-no-drive "disk: no drive on primary master|boot halted" \
    "$QEMU_X86" -bios x86/squirrel-bios.rom

# AArch64: -kernel image entered with the DTB in x0
run arm-boot "SquirrelBIOS ARM|OS-ENTRY arm dtb=ok regs=ok" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin \
    -kernel $TEST_DIR/arm-kernel.bin

run arm-no-kernel "boot: no kernel image|boot halted" \
    "$QEMU_ARM" -M virt -cpu cortex-a57 -bios arm/squirrel-bios.bin

# RISC-V: -kernel image entered on hart 0 with the DTB in a1
run riscv-boot "SquirrelBIOS RISC-V|OS-ENTRY riscv hart=ok dtb=ok" \
    "$QEMU_RISCV" -M virt -smp 2 -bios riscv/squirrel-bios.bin \
    -kernel $TEST_DIR/riscv-kernel.bin

run riscv-no-kernel "boot: no kernel image|boot halted" \
    "$QEMU_RISCV" -M virt -bios riscv/squirrel-bios.bin

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
