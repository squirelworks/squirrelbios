/* SquirrelBIOS — RISC-V firmware for QEMU virt
 * Prints banner on UART then halts.
 * Original code.
 *
 * Build (from repo root):
 *   make riscv
 * Run:
 *   qemu-system-riscv64 -M virt -nographic -bios riscv/squirrel-bios.bin
 */

.global _start
_start:
    la sp, stack_top

    li t0, 0x10000000
    la t1, banner
1:
    lb t2, 0(t1)
    beqz t2, 2f
    sb t2, 0(t0)
    addi t1, t1, 1
    j 1b
2:
    wfi
    j 2b

.align 4
banner:
    .asciz "SquirrelBIOS RISC-V\n"

.align 12
    .skip 4096
stack_top:
