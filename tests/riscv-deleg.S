/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Sets stvec, executes an illegal instruction and expects the trap in
 * S-mode, then executes ecall, which must still reach the firmware.
 * Original code.
 */

        .equ UART_BASE, 0x10000000
        .equ CAUSE_ILLEGAL, 2

.global _start
_start:
        la a0, msg_entry
        call puts
        la t0, strap
        csrw stvec, t0
        .word 0                             /* illegal instruction */
        la a0, msg_deleg_bad                /* not reached when delegated */
        call puts
        j done

        .align 2
strap:
        csrr t0, scause
        li t1, CAUSE_ILLEGAL
        la a0, msg_deleg_bad
        bne t0, t1, 1f
        la a0, msg_deleg_ok
1:      call puts
        ecall                               /* to the firmware, not to strap */

done:   wfi
        j done

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv deleg\n"
msg_deleg_ok:   .asciz "deleg=ok\n"
msg_deleg_bad:  .asciz "deleg=bad\n"
