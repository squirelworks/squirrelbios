/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Executes ecall so the firmware's trap handler reports it.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

.global _start
_start:
        la a0, msg_entry
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ecall

3:      wfi                                 /* not reached */
        j 3b

msg_entry:      .asciz "OS-ENTRY riscv ecall\n"
