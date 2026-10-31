/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Stands in for the OS: checks the hand-off state and reports on the UART.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

.global _start
_start:
        mv s0, a0                           /* hart ID */
        mv s1, a1                           /* DTB */
        mv s2, a2                           /* must be 0 */

        la a0, msg_entry
        call puts

        la a0, msg_hart_bad
        bnez s0, 1f
        la a0, msg_hart_ok
1:      call puts

        la a0, msg_dtb_bad
        beqz s1, 2f
        lwu t0, 0(s1)
        li t1, 0xedfe0dd0                   /* d00dfeed, read little-endian */
        bne t0, t1, 2f
        la a0, msg_dtb_ok
2:      call puts

        la a0, msg_a2_bad
        bnez s2, 3f
        la a0, msg_a2_ok
3:      call puts

4:      wfi
        j 4b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv"
msg_hart_ok:    .asciz " hart=ok"
msg_hart_bad:   .asciz " hart=bad"
msg_dtb_ok:     .asciz " dtb=ok"
msg_dtb_bad:    .asciz " dtb=bad"
msg_a2_ok:      .asciz " a2=ok\n"
msg_a2_bad:     .asciz " a2=bad\n"
