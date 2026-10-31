/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run with -smp 64. Hart 0 starts harts 1 to 63 with sbi_hart_start, one
 * at a time, and waits until each has checked in. It then checks that
 * get_status reports every one started, and sends one remote fence whose
 * mask names all 64 harts (a0 = -1, a1 = 0), which only succeeds when the
 * firmware tracks all of them. It prints harts=64.
 * Built with --defsym HARTS=N it does the same on -smp N and prints
 * harts=ok (riscv-numa runs it with 4 harts in two NUMA nodes).
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_HSM,       0x48534d
        .equ SBI_EXT_RFENCE,    0x52464e43
        .equ HSM_STARTED,       0
        .ifndef HARTS
        .equ HARTS,             64
        .endif
        .equ WAIT,              50000000    /* ticks of time before giving up: 5 s at virt's 10 MHz */

.global _start
_start:
        bnez a0, fail_entry                 /* only hart 0 enters here */
        la a0, msg_entry
        call puts

        li s0, 1                            /* s0 = next hart to start */
1:      mv a0, s0                           /* hart_start(s0, secondary, s0) */
        la a1, secondary
        mv a2, s0
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
        la t1, msg_start_bad
        bnez a0, fail_hart

        la s2, checked_in                   /* wait for it to check in */
        rdtime s3
        li t4, WAIT
        add s3, s3, t4                      /* deadline */
2:      ld t0, 0(s2)
        bgeu t0, s0, 3f
        rdtime t4
        bltu t4, s3, 2b
        la t1, msg_timeout
        j fail_hart
3:      addi s0, s0, 1
        li t0, HARTS
        bltu s0, t0, 1b

        li s0, 1                            /* every one reports started */
4:      mv a0, s0
        li a7, SBI_EXT_HSM
        li a6, 2
        ecall
        la t1, msg_status_bad
        bnez a0, fail_hart
        li t0, HSM_STARTED
        bne a1, t0, fail_hart
        addi s0, s0, 1
        li t0, HARTS
        bltu s0, t0, 4b

.if HARTS == 64
        li a0, -1                           /* remote_fence_i, harts 0 to 63 */
.else
        li a0, (1 << HARTS) - 1             /* remote_fence_i, every hart */
.endif
        li a1, 0
        li a7, SBI_EXT_RFENCE
        li a6, 0
        ecall
        la t1, msg_fence_bad
        bnez a0, fail_t1

        la a0, msg_ok
        call puts
        j done

/* Harts 1 to 63 start here in S-mode: a0 = hart ID, a1 = the same ID
 * (the opaque value). Check in, in order, and wait. A remote fence
 * reaches them through the firmware's M-mode software interrupt. */
secondary:
        bne a0, a1, 2f
        la t0, checked_in
        fence
        sd a0, 0(t0)
1:      wfi
        j 1b
2:      la t1, msg_opaque_bad
        mv s0, a0
        j fail_hart

fail_entry:
        la t1, msg_entry_bad
fail_t1:
        mv a0, t1
        call puts
done:   wfi
        j done

/* Print t1, then " hart " and s0 in hex, then stop */
fail_hart:
        mv a0, t1
        call puts
        la a0, msg_hart
        call puts
        mv a0, s0
        call puthex
        li a0, '\n'
        call putc
        j done

putc:
        li t0, UART_BASE
        sb a0, 0(t0)
        ret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

/* a0 as 16 lowercase hex digits */
puthex:
        li t0, UART_BASE
        li t2, 60
1:      srl t1, a0, t2
        andi t1, t1, 0xf
        li t3, 10
        blt t1, t3, 2f
        addi t1, t1, 'a' - '0' - 10
2:      addi t1, t1, '0'
        sb t1, 0(t0)
        addi t2, t2, -4
        bgez t2, 1b
        ret

msg_entry:      .asciz "OS-ENTRY riscv harts\n"
.if HARTS == 64
msg_ok:         .asciz "harts=64\n"
.else
msg_ok:         .asciz "harts=ok\n"
.endif
msg_hart:       .asciz " hart "
msg_entry_bad:  .asciz "harts=bad image entered on a hart other than 0\n"
msg_start_bad:  .asciz "harts=bad hart_start failed"
msg_timeout:    .asciz "harts=bad never checked in"
msg_status_bad: .asciz "harts=bad not reported as started"
msg_opaque_bad: .asciz "harts=bad a1 is not the opaque value"
msg_fence_bad:  .asciz "harts=bad remote fence over 64 harts failed\n"

        .data
        .align 3
checked_in:     .dword 0
