/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run with -smp 3. Hart 0 starts harts 1 and 2. Hart 2 only idles in wfi,
 * so it does its fences in the firmware's M-mode software interrupt.
 * Harts 0 and 1 then both call remote_sfence_vma on hart 2 ROUNDS times
 * at once, which is how two senders can race on one target's requests,
 * and then send an IPI each, one after the other, which hart 2 must take. Prints
 * fence3=ok, or fence3=bad and the number of the failed check.
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000
        .equ SBI_EXT_IPI,       0x735049
        .equ SBI_EXT_RFENCE,    0x52464e43
        .equ SBI_EXT_HSM,       0x48534d
        .equ CAUSE_S_SOFT,      (1 << 63) | 1
        .equ SIE_SSIE,          (1 << 1)
        .equ SSTATUS_SIE,       (1 << 1)
        .equ WAIT,              50000000    /* ticks of time before giving up: 5 s at virt's 10 MHz */
        .equ ROUNDS,            20000

/* ecall \ext function \fid with a0 = \mask, a1 = \base */
.macro sbi ext, fid, mask, base
        li a0, \mask
        li a1, \base
        li a7, \ext
        li a6, \fid
        ecall
.endm

/* Check \n: wait until the word at \label reaches \value */
.macro wait_for label, value, n
        li s11, \n
        la t0, \label
        li t1, \value
        rdtime t2
        li t4, WAIT
        add t2, t2, t4                      /* deadline */
1:      lw t3, 0(t0)
        bgeu t3, t1, 2f
        rdtime t4
        bltu t4, t2, 1b
        j bad
2:
.endm

/* hart_start(\hart, \entry, 0) */
.macro start hart, entry
        li a0, \hart
        la a1, \entry
        li a2, 0
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
.endm

.global _start
_start:
        la a0, msg_entry
        call puts
        li s11, 1                           /* 1: starting harts 1 and 2 */
        start 2, hart2
        bnez a0, bad
        wait_for ready2, 1, 1
        start 1, hart1
        bnez a0, bad
        wait_for ready1, 1, 2               /* 2: hart 1 came up */

        la t0, go                           /* 3: hart 0's rounds */
        li t1, 1
        sw t1, 0(t0)
        li s11, 3
        li s10, ROUNDS
1:      sbi SBI_EXT_RFENCE, 1, 4, 0         /* remote_sfence_vma on hart 2 */
        bnez a0, bad
        addi s10, s10, -1
        bnez s10, 1b
        wait_for done1, 1, 4                /* 4: hart 1's rounds */
        wait_for count2, 1, 5               /* 5: hart 1's IPI reached hart 2 */
        li s11, 6                           /* 6: and then hart 0's */
        sbi SBI_EXT_IPI, 0, 4, 0
        bnez a0, bad
        wait_for count2, 2, 6

        la a0, msg_ok
        call puts
        j done

bad:    la a0, msg_bad
        call puts
        li t0, UART_BASE
        addi t1, s11, '0'
        sb t1, 0(t0)
        li t1, '\n'
        sb t1, 0(t0)
done:   wfi
        j done

/* Hart 1: wait for go, fence hart 2 ROUNDS times, IPI it, report */
hart1:
        la t0, ready1
        li t1, 1
        sw t1, 0(t0)
        la s0, go
1:      lw t0, 0(s0)
        beqz t0, 1b
        li s10, ROUNDS
2:      sbi SBI_EXT_RFENCE, 1, 4, 0
        bnez a0, 3f
        addi s10, s10, -1
        bnez s10, 2b
        sbi SBI_EXT_IPI, 0, 1, 2            /* mask base 2: hart 2 */
        bnez a0, 3f
        la t0, done1
        li t1, 1
        sw t1, 0(t0)
3:      wfi
        j 3b

/* Hart 2: count IPIs, otherwise idle */
hart2:
        la t0, strap
        csrw stvec, t0
        li t0, SIE_SSIE
        csrs sie, t0
        csrsi sstatus, SSTATUS_SIE
        la t0, ready2
        li t1, 1
        sw t1, 0(t0)
1:      wfi
        j 1b

        .align 2
strap:
        csrr t5, scause
        li t6, CAUSE_S_SOFT
        bne t5, t6, 1f
        csrci sip, SIE_SSIE
        la t5, count2
        li t6, 1
        amoadd.w zero, t6, (t5)
        sret
1:      la a0, msg_trap
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

        .data
msg_entry:      .asciz "OS-ENTRY riscv fence-smp3\n"
msg_ok:         .asciz "fence3=ok\n"
msg_bad:        .asciz "fence3=bad "
msg_trap:       .asciz "fence3=bad unexpected trap\n"
        .align 2
ready1:         .word 0
ready2:         .word 0
go:             .word 0
done1:          .word 0
count2:         .word 0                     /* IPIs taken by hart 2 */
