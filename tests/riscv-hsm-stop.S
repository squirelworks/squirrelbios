/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run with -smp 2. Hart 0 starts hart 1, which prints "hart1 up a1=..."
 * and calls hart_stop. Hart 0 waits for hart_get_status to report hart 1
 * stopped, then starts it a second time with another opaque value, waits
 * for it to stop again and prints stop=ok. If hart_stop ever returns,
 * hart 1 prints stop=bad.
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_HSM,       0x48534d
        .equ HSM_STOPPED,       1
        .equ WAIT,              0x1000000   /* polls before giving up */

.global _start
_start:
        la a0, msg_entry
        call puts

        li s1, 1                            /* opaque of the first start */
        call start_and_wait
        li s1, 2                            /* and of the second */
        call start_and_wait

        la a0, msg_ok
fail:   call puts
done:   wfi
        j done

/* Start hart 1 with opaque s1 and wait until it reports stopped. */
start_and_wait:
        mv s2, ra
        li a0, 1
        la a1, hart1
        mv a2, s1
        li a7, SBI_EXT_HSM
        li a6, 0                            /* hart_start */
        ecall
        mv t0, a0
        la a0, msg_start_bad
        bnez t0, fail
        li s3, WAIT
1:      li a0, 1
        li a7, SBI_EXT_HSM
        li a6, 2                            /* hart_get_status */
        ecall
        mv t0, a0
        la a0, msg_status_bad
        bnez t0, fail
        li t0, HSM_STOPPED
        beq a1, t0, 2f
        addi s3, s3, -1
        bnez s3, 1b
        la a0, msg_timeout
        j fail
2:      mv ra, s2
        ret

/* Hart 1 starts here in S-mode: a0 = 1, a1 = opaque */
hart1:
        mv s1, a1
        la a0, msg_hart1
        call puts
        addi a0, s1, '0'
        li t0, UART_BASE
        sb a0, 0(t0)
        li a0, '\n'
        sb a0, 0(t0)
        li a7, SBI_EXT_HSM
        li a6, 1                            /* hart_stop: does not return */
        ecall
        la a0, msg_returned
        call puts
        j done

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv hsm-stop\n"
msg_ok:         .asciz "stop=ok\n"
msg_hart1:      .asciz "hart1 up a1="
msg_returned:   .asciz "stop=bad hart_stop returned\n"
msg_start_bad:  .asciz "stop=bad hart_start failed\n"
msg_status_bad: .asciz "stop=bad hart_get_status failed\n"
msg_timeout:    .asciz "stop=bad hart 1 never reported stopped\n"
