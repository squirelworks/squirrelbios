/* Smoke-test image for SquirrelBIOS RISC-V SSE (supervisor software
 * events), linked at 0x80200000 and entered in S-mode on hart 0.
 * Original code.
 *
 * Registers the software-injected local event, checks attributes and
 * state errors, injects it while masked (nothing happens), enables and
 * unmasks it: the event is taken on the way back from hart_unmask. The
 * handler checks a6 = hart ID, a7 = entry argument and the interrupted
 * state the SBI saved, then completes; the interrupted code resumes with
 * a0, a6, a7 and sepc as they were. Then once more as a one-shot event
 * through inject, which leaves it REGISTERED. With -smp 2, hart 0 then
 * injects the event on hart 1, which takes it on the IPI. Prints sse=ok, or sse=bad
 * and the number of the failed check. Also checks that DBCN refuses a
 * buffer inside the firmware region.
 */

        .equ UART_BASE,         0x10000000
        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_DBCN,      0x4442434e
        .equ SBI_EXT_SSE,       0x535345
        .equ EV,                0xffff0000
        .equ ARG,               0x5353455f41524721
        .equ SEPC_MARK,         0x1234560
        .equ ERR_INVALID_PARAM, -3
        .equ ERR_DENIED,        -4
        .equ ERR_INVALID_ADDRESS, -5
        .equ ERR_ALREADY_STARTED, -7
        .equ ERR_INVALID_STATE, -10
        .equ ERR_BAD_RANGE,     -11

        .option norelax

/* a7 = SSE, a6 = fid; a0.. set by the caller */
.macro sse fid
        li a7, SBI_EXT_SSE
        li a6, \fid
        ecall
.endm

.macro expect reg, value, n
        li t0, \value
        li s11, \n
        bne \reg, t0, bad
.endm

/* read_attrs(EV, 0, 10, attrs) */
.macro read_all
        li a0, EV
        li a1, 0
        li a2, 10
        la a3, attrs
        li a4, 0
        sse 0
.endm

.global _start
_start:
        la sp, stack_top
        la a0, msg_entry
        call puts

        li a7, SBI_EXT_BASE                 /* probe_extension(SSE) */
        li a6, 3
        li a0, SBI_EXT_SSE
        ecall
        expect a0, 0, 1
        expect a1, 1, 1

        li a0, 0x8000                       /* an event this SBI has not */
        la a1, handler
        li a2, 0
        sse 2
        expect a0, -2, 2

        li a0, EV                           /* register */
        la a1, handler
        li a2, ARG
        sse 2
        expect a0, 0, 3
        li a0, EV                           /* twice */
        la a1, handler
        li a2, ARG
        sse 2
        expect a0, ERR_INVALID_STATE, 4

        read_all                            /* REGISTERED, injectable */
        expect a0, 0, 5
        la t1, attrs
        ld t2, 0(t1)
        expect t2, 9, 5
        ld t2, 32(t1)
        la t0, handler
        li s11, 6
        bne t2, t0, bad
        ld t2, 40(t1)
        expect t2, ARG, 7

        li a0, EV                           /* ranges and addresses */
        li a1, 8
        li a2, 3
        la a3, attrs
        li a4, 0
        sse 0
        expect a0, ERR_BAD_RANGE, 8
        li a0, EV
        li a1, 0
        li a2, 1
        li a3, 0x80001000                   /* the firmware's own memory */
        li a4, 0
        sse 0
        expect a0, ERR_INVALID_ADDRESS, 9
        li a0, EV
        li a1, 0
        li a2, 0
        la a3, attrs
        li a4, 0
        sse 0
        expect a0, ERR_INVALID_PARAM, 10

        la t1, attrs                        /* STATUS is read-only */
        sd zero, 0(t1)
        li a0, EV
        li a1, 0
        li a2, 1
        mv a3, t1
        li a4, 0
        sse 1
        expect a0, ERR_DENIED, 11
        li a0, EV                           /* INTERRUPTED_SEPC needs RUNNING */
        li a1, 6
        li a2, 1
        la a3, attrs
        li a4, 0
        sse 1
        expect a0, ERR_INVALID_STATE, 12

        li a0, EV                           /* inject while not enabled */
        li a1, 0
        sse 7
        expect a0, 0, 13
        li a0, EV
        sse 4                               /* enable: still masked */
        expect a0, 0, 14
        la t0, taken
        ld t0, 0(t0)
        expect t0, 0, 15
        read_all                            /* ENABLED, pending, injectable */
        la t1, attrs
        ld t2, 0(t1)
        expect t2, 14, 16

        li a0, EV                           /* inject on a hart that is not there */
        li a1, 3
        sse 7
        expect a0, ERR_INVALID_PARAM, 17

        li t0, SEPC_MARK                    /* unmask: taken on the way back */
        csrw sepc, t0
        li a7, SBI_EXT_SSE
        li a6, 8
        ecall
        mv s1, a6                           /* as the handler left them */
        mv s2, a7
        expect a0, 0, 18
        expect s1, 8, 19
        expect s2, SBI_EXT_SSE, 20
        csrr t2, sepc
        expect t2, SEPC_MARK, 21
        la t0, taken
        ld t0, 0(t0)
        expect t0, 1, 22
        la t0, hbad
        ld t0, 0(t0)
        bnez t0, bad_handler
        read_all                            /* ENABLED again, nothing pending */
        la t1, attrs
        ld t2, 0(t1)
        expect t2, 10, 24

        li a7, SBI_EXT_SSE                  /* unmask twice */
        li a6, 8
        ecall
        expect a0, ERR_ALREADY_STARTED, 25

        li a0, EV                           /* one-shot: disable, CONFIG = 1 */
        sse 5
        expect a0, 0, 26
        li a0, EV                           /* a misaligned buffer */
        li a1, 2
        li a2, 1
        la a3, attrs + 4
        li a4, 0
        sse 1
        expect a0, ERR_INVALID_ADDRESS, 27
        la a3, attrs                        /* CONFIG = 1, value 0 at offset 0 */
        li t0, 1
        sd t0, 0(a3)
        li a0, EV
        li a1, 2
        li a2, 1
        li a4, 0
        sse 1
        expect a0, 0, 28
        li a0, EV
        sse 4
        expect a0, 0, 29
        li a0, EV                           /* inject: taken on the way back */
        li a1, 0
        sse 7
        expect a0, 0, 30
        la t0, taken
        ld t0, 0(t0)
        expect t0, 2, 31
        read_all                            /* one-shot: REGISTERED */
        la t1, attrs
        ld t2, 0(t1)
        expect t2, 9, 32

        /* Across harts (-smp 2): hart 1 registers, enables and unmasks the
         * event itself, then waits; hart 0 injects it there, and hart 1's
         * handler (taken on the IPI) counts it */
        li a7, 0x48534d                     /* HSM hart_start(1, hart1, 0) */
        li a6, 0
        li a0, 1
        la a1, hart1
        li a2, 0
        ecall
        expect a0, 0, 34
        la t1, h1_ready
        li t2, 50000000
1:      ld t0, 0(t1)
        bnez t0, 2f
        addi t2, t2, -1
        bnez t2, 1b
        li s11, 35
        j bad
2:      li a0, EV
        li a1, 1
        sse 7
        expect a0, 0, 36
        la t1, h1_taken
        li t2, 50000000
3:      ld t0, 0(t1)
        bnez t0, 4f
        addi t2, t2, -1
        bnez t2, 3b
        li s11, 37
        j bad
4:      expect t0, 1, 38

        li a7, SBI_EXT_DBCN                 /* DBCN into the firmware: refused */
        li a6, 1
        li a0, 16
        li a1, 0x80000100
        li a2, 0
        ecall
        expect a0, ERR_INVALID_PARAM, 33

        la a0, msg_ok
        call puts
        j done

bad_handler:                                /* 40 + the handler's check */
        la t0, hbad
        ld s11, 0(t0)
        addi s11, s11, 40
bad:    la a0, msg_bad
        call puts
        li t0, UART_BASE
        li t1, 10
        divu t2, s11, t1
        remu s11, s11, t1
        addi t2, t2, '0'
        sb t2, 0(t0)
        addi s11, s11, '0'
        sb s11, 0(t0)
        li t2, '\n'
        sb t2, 0(t0)
done:   wfi
        j done

/* The event handler, in S-mode with SIE = 0. It keeps a0 and a1 (the
 * interrupted code's) in memory, checks what the SBI handed over, counts
 * the event and completes. */
        .align 2
handler:
        la t0, saved
        sd a0, 0(t0)
        sd a1, 8(t0)
        sd s11, 16(t0)
        li s11, 1                           /* a6 = hart ID: hart 0 */
        bnez a6, hfail
        li t1, ARG
        li s11, 2
        bne a7, t1, hfail
        csrr t1, sstatus
        andi t1, t1, 2                      /* SIE = 0 */
        li s11, 3
        bnez t1, hfail
        read_all                            /* RUNNING; the saved state */
        li s11, 4
        bnez a0, hfail
        la t1, attrs
        ld t2, 0(t1)
        andi t2, t2, 3
        li t0, 3
        bne t2, t0, hfail
        ld t2, 48(t1)                       /* INTERRUPTED_SEPC */
        li s11, 5
        li t0, SEPC_MARK
        bne t2, t0, hfail
        csrr t2, sstatus                    /* SPP = the interrupted mode, S */
        andi t2, t2, 0x100
        li s11, 6
        beqz t2, hfail
        ld t2, 72(t1)                       /* INTERRUPTED_A7 = the SSE call's */
        li t0, SBI_EXT_SSE
        li s11, 7
        bne t2, t0, hfail
        j 2f
hfail:  la t0, hbad
        sd s11, 0(t0)
2:      la t0, taken
        ld t1, 0(t0)
        addi t1, t1, 1
        sd t1, 0(t0)
        la t0, saved
        ld a0, 0(t0)
        ld a1, 8(t0)
        ld s11, 16(t0)
        li a7, SBI_EXT_SSE                  /* complete: does not return here */
        li a6, 6
        ecall
        la t0, hbad
        li t1, 99
        sd t1, 0(t0)
3:      wfi
        j 3b

/* Hart 1, in S-mode from hart_start */
        .align 2
hart1:
        li a0, EV
        la a1, handler1
        li a2, 1
        sse 2
        bnez a0, 9f
        li a0, EV
        sse 4
        bnez a0, 9f
        li a7, SBI_EXT_SSE
        li a6, 8
        ecall
        bnez a0, 9f
        la t0, h1_ready
        li t1, 1
        sd t1, 0(t0)
1:      j 1b                                /* the event comes on an IPI */
9:      j 9b
        .align 2
handler1:
        la t0, h1_taken                     /* a6 = 1, a7 = 1: hart 1, its arg */
        li t1, 1
        bne a6, t1, 1f
        bne a7, t1, 1f
        ld t1, 0(t0)
        addi t1, t1, 1
        sd t1, 0(t0)
1:      li a7, SBI_EXT_SSE
        li a6, 6
        ecall
2:      j 2b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv sse\n"
msg_ok:         .asciz "sse=ok\n"
msg_bad:        .asciz "sse=bad "

        .data
        .align 3
taken:  .dword 0
h1_ready: .dword 0
h1_taken: .dword 0
hbad:   .dword 0
saved:  .skip 24
attrs:  .skip 8 * 10
        .skip 4096
stack_top:
