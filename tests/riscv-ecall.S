/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Executes ecall for an extension that does not exist: the firmware's SBI
 * must return SBI_ERR_NOT_SUPPORTED (-2) to the next instruction.
 * Original code.
 */

        .equ UART_BASE, 0x10000000
        .equ SBI_ERR_NOT_SUPPORTED, -2

.global _start
_start:
        la a0, msg_entry
        call puts
        li a7, 0x0a000000                   /* no such extension */
        li a6, 0
        ecall
        mv t1, a0
        li t0, SBI_ERR_NOT_SUPPORTED
        la a0, msg_bad
        bne t1, t0, 1f
        la a0, msg_ok
1:      call puts

2:      wfi
        j 2b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv ecall\n"
msg_ok:         .asciz "ecall=ok\n"
msg_bad:        .asciz "ecall=bad\n"
