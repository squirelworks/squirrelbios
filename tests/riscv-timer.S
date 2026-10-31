/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Probes TIME, sets a timer 10 ms ahead through the SBI and waits for the
 * supervisor timer interrupt. The handler prints timer=ok, pushes the
 * timer out of reach and returns with interrupts still on: if the
 * interrupt stayed pending it would come back and print timer=again.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_TIME,      0x54494d45
        .equ CAUSE_S_TIMER,     (1 << 63) | 5
        .equ SIE_STIE,          (1 << 5)
        .equ SSTATUS_SIE,       (1 << 1)
        .equ TICKS_10MS,        100000      /* QEMU virt timebase: 10 MHz */

        .option norelax                     /* no gp-relative addressing: gp is not set up */

.global _start
_start:
        la sp, stack_top
        la a0, msg_entry
        call puts

        li a7, SBI_EXT_BASE                 /* probe_extension(TIME) */
        li a6, 3
        li a0, SBI_EXT_TIME
        ecall
        la a0, msg_probe_bad
        beqz a1, fail

        la t0, strap
        csrw stvec, t0
        li t0, SIE_STIE
        csrs sie, t0

        rdtime a0
        li t0, TICKS_10MS
        add a0, a0, t0
        li a7, SBI_EXT_TIME                 /* set_timer */
        li a6, 0
        ecall
        mv t0, a0
        la a0, msg_set_bad
        bnez t0, fail

        csrsi sstatus, SSTATUS_SIE
1:      wfi
        j 1b

fail:   call puts
2:      wfi
        j 2b

        .align 2
strap:
        csrr t0, scause
        li t1, CAUSE_S_TIMER
        la a0, msg_cause_bad
        bne t0, t1, fail
        la t0, seen
        ld t1, 0(t0)
        la a0, msg_again
        bnez t1, fail
        li t1, 1
        sd t1, 0(t0)
        la a0, msg_ok
        call puts
        li a0, -1                           /* never: clears the interrupt */
        li a7, SBI_EXT_TIME
        li a6, 0
        ecall
        sret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv timer\n"
msg_ok:         .asciz "timer=ok\n"
msg_again:      .asciz "timer=again\n"
msg_probe_bad:  .asciz "timer=bad probe\n"
msg_set_bad:    .asciz "timer=bad set_timer\n"
msg_cause_bad:  .asciz "timer=bad scause\n"

        .data
        .align 3
seen:   .dword 0
        .skip 1024
stack_top:
