/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Probes RFENCE, then calls remote_sfence_vma for itself and hart 1, which
 * is parked (stopped: valid, so this must succeed), for hart 2, which the
 * machine does not have (-smp 2; must fail), for itself alone (a0 = 1,
 * a1 = its hart id; must succeed), and remote_fence_i for itself. Prints
 * rfence=ok.
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_RFENCE,    0x52464e43

.global _start
_start:
        mv s0, a0                           /* hart ID */
        la a0, msg_entry
        call puts

        li a7, SBI_EXT_BASE                 /* probe_extension(RFENCE) */
        li a6, 3
        li a0, SBI_EXT_RFENCE
        ecall
        la a0, msg_probe_bad
        beqz a1, fail

        li a0, 3                            /* this hart and hart 1 */
        li a1, 0
        li a2, 0                            /* start */
        li a3, -1                           /* size: everything */
        li a7, SBI_EXT_RFENCE
        li a6, 1                            /* remote_sfence_vma */
        ecall
        mv t0, a0
        la a0, msg_stopped_bad
        bnez t0, fail

        li a0, 5                            /* this hart and hart 2 */
        li a1, 0
        li a2, 0
        li a3, -1
        li a7, SBI_EXT_RFENCE
        li a6, 1
        ecall
        mv t0, a0
        la a0, msg_absent_bad
        beqz t0, fail

        li a0, 1                            /* this hart only */
        mv a1, s0
        li a2, 0
        li a3, -1
        li a7, SBI_EXT_RFENCE
        li a6, 1
        ecall
        mv t0, a0
        la a0, msg_sfence_bad
        bnez t0, fail

        li a0, 1
        mv a1, s0
        li a7, SBI_EXT_RFENCE
        li a6, 0                            /* remote_fence_i */
        ecall
        mv t0, a0
        la a0, msg_fence_i_bad
        bnez t0, fail

        la a0, msg_ok
fail:   call puts
1:      wfi
        j 1b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv rfence\n"
msg_ok:         .asciz "rfence=ok\n"
msg_probe_bad:  .asciz "rfence=bad probe\n"
msg_stopped_bad: .asciz "rfence=bad stopped hart 1 was refused\n"
msg_absent_bad: .asciz "rfence=bad hart 2 was accepted\n"
msg_sfence_bad: .asciz "rfence=bad remote_sfence_vma failed\n"
msg_fence_i_bad: .asciz "rfence=bad remote_fence_i failed\n"
