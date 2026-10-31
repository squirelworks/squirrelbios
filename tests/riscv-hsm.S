/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run with -smp 2. Hart 0 checks that hart 1 is stopped, that a hart that
 * does not exist is refused, starts hart 1 with an opaque value, and
 * checks that a second start is refused and that hart 1 is then started.
 * Hart 1 prints hart1=ok and the a1 it was given, which must be the
 * opaque value and not the device tree.
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_HSM,       0x48534d
        .equ HSM_STARTED,       0
        .equ HSM_STOPPED,       1
        .equ SBI_ERR_INVALID_PARAM, -3
        .equ SBI_ERR_ALREADY_AVAILABLE, -6
        .equ OPAQUE,            0x5157000012345678
        .equ WAIT,              0x1000000   /* polls before giving up */

/* ecall HSM function \fid with a0 = \hart, a1 = \addr, a2 = \opaque */
.macro hsm fid, hart, addr=0, opaque=0
        li a0, \hart
        li a1, \addr
        li a2, \opaque
        li a7, SBI_EXT_HSM
        li a6, \fid
        ecall
.endm

/* Fail with message \msg unless reg == value */
.macro expect reg, value, msg
        li t0, \value
        la t1, \msg
        bne \reg, t0, fail_t1
.endm

.global _start
_start:
        bnez a0, fail_entry                 /* only hart 0 enters here */
        mv s1, a1                           /* device tree */
        la a0, msg_entry
        call puts

        li a7, SBI_EXT_BASE                 /* probe_extension(HSM) */
        li a6, 3
        li a0, SBI_EXT_HSM
        ecall
        la t1, msg_probe_bad
        beqz a1, fail_t1

        hsm 2, 1                            /* get_status(1) */
        expect a0, 0, msg_status_bad
        expect a1, HSM_STOPPED, msg_status_bad

        hsm 0, 7, 0x80200000                /* -smp 2: no hart 7 */
        expect a0, SBI_ERR_INVALID_PARAM, msg_absent_bad

        li a0, 1                            /* hart_start(1, hart1, OPAQUE) */
        la a1, hart1
        li a2, OPAQUE
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
        expect a0, 0, msg_start_bad

        la a1, hart1                        /* again: already running */
        li a0, 1
        li a2, 0
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
        li t0, SBI_ERR_ALREADY_AVAILABLE
        la t1, msg_again_bad
        bne a0, t0, fail_t1

        la s2, hart1_done                   /* wait for hart 1 to report */
        li s3, WAIT
1:      ld t0, 0(s2)
        bnez t0, 2f
        addi s3, s3, -1
        bnez s3, 1b
        la t1, msg_timeout
        j fail_t1
2:
        hsm 2, 1
        expect a0, 0, msg_started_bad
        expect a1, HSM_STARTED, msg_started_bad
        la a0, msg_ok
        call puts
        j done

/* Hart 1 starts here in S-mode: a0 = 1, a1 = OPAQUE */
hart1:
        mv s0, a0
        mv s1, a1
        li t0, 1
        la t1, msg_hart1_bad
        bne s0, t0, fail_t1
        li t0, OPAQUE
        bne s1, t0, fail_t1
        la a0, msg_hart1_ok
        call puts
        mv a0, s1
        call puthex
        li a0, '\n'
        call putc
        fence
        la t0, hart1_done
        li t1, 1
        sd t1, 0(t0)
        j done

fail_entry:
        la t1, msg_entry_bad
fail_t1:
        mv a0, t1
        call puts
done:   wfi
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

msg_entry:      .asciz "OS-ENTRY riscv hsm\n"
msg_ok:         .asciz "hsm=ok\n"
msg_hart1_ok:   .asciz "hart1=ok a1="
msg_hart1_bad:  .asciz "hsm=bad hart 1 entry registers\n"
msg_entry_bad:  .asciz "hsm=bad image entered on a hart other than 0\n"
msg_probe_bad:  .asciz "hsm=bad probe\n"
msg_status_bad: .asciz "hsm=bad hart 1 not stopped before start\n"
msg_absent_bad: .asciz "hsm=bad hart 7 was accepted\n"
msg_start_bad:  .asciz "hsm=bad hart_start failed\n"
msg_again_bad:  .asciz "hsm=bad second hart_start was not refused\n"
msg_timeout:    .asciz "hsm=bad hart 1 never reported\n"
msg_started_bad: .asciz "hsm=bad hart 1 not reported as started\n"

        .data
        .align 3
hart1_done:     .dword 0
