/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run with -smp 2. Hart 0 starts hart 1, which turns on its supervisor
 * software interrupt and waits. Hart 0 then sends IPIs to hart 1 (by
 * mask bit and by mask base), runs remote_fence_i and remote_sfence_vma
 * on both harts, and sends an IPI to every hart (base -1), which must
 * reach both. Each IPI is counted by the hart that takes it. Last, both
 * harts fence each other ROUNDS times at once, which must not deadlock
 * in the firmware. Prints
 * ipi-smp=ok, or ipi-smp=bad and the number of the failed check.
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
        .equ WAIT,              0x1000000   /* polls before giving up */
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
        li t2, WAIT
1:      lw t3, 0(t0)
        beq t3, t1, 2f
        addi t2, t2, -1
        bnez t2, 1b
        j bad
2:
.endm

.global _start
_start:
        mv tp, a0                           /* hart ID, for the handler */
        la t0, strap
        csrw stvec, t0
        la a0, msg_entry
        call puts

        li s11, 1                           /* 1: hart_start */
        li a0, 1
        la a1, hart1
        li a2, 0
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
        bnez a0, bad
        wait_for ready, 1, 1

        li s11, 2                           /* 2: IPI to hart 1 by mask bit */
        sbi SBI_EXT_IPI, 0, 2, 0
        bnez a0, bad
        wait_for count1, 1, 2
        li s11, 3                           /* 3: IPI to hart 1 by base */
        sbi SBI_EXT_IPI, 0, 1, 1
        bnez a0, bad
        wait_for count1, 2, 3

        li s11, 4                           /* 4: fences on both harts */
        sbi SBI_EXT_RFENCE, 0, 3, 0         /* remote_fence_i */
        bnez a0, bad
        sbi SBI_EXT_RFENCE, 1, 0, -1        /* remote_sfence_vma, every hart */
        bnez a0, bad

        li t0, SIE_SSIE                     /* hart 0 takes IPIs too now */
        csrs sie, t0
        csrsi sstatus, SSTATUS_SIE
        li s11, 5                           /* 5: IPI to every hart */
        sbi SBI_EXT_IPI, 0, 0, -1
        bnez a0, bad
        wait_for count1, 3, 5
        wait_for count0, 1, 6               /* 6: ... including hart 0 */

        la t0, go                           /* 7: fence each other at once */
        li t1, 1
        sw t1, 0(t0)
        sbi SBI_EXT_IPI, 0, 2, 0            /* wake hart 1 */
        li s11, 7
        li s10, ROUNDS
1:      sbi SBI_EXT_RFENCE, 1, 2, 0         /* remote_sfence_vma on hart 1 */
        bnez a0, bad
        addi s10, s10, -1
        bnez s10, 1b
        wait_for done1, 1, 8                /* 8: hart 1 finished its rounds */

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

/* Hart 1 starts here in S-mode, a0 = 1 */
hart1:
        mv tp, a0
        la t0, strap
        csrw stvec, t0
        li t0, SIE_SSIE
        csrs sie, t0
        csrsi sstatus, SSTATUS_SIE
        la t0, ready
        li t1, 1
        sw t1, 0(t0)
        la s0, go
1:      wfi
        lw t0, 0(s0)
        beqz t0, 1b
        li s10, ROUNDS
2:      sbi SBI_EXT_RFENCE, 1, 1, 0         /* remote_sfence_vma on hart 0 */
        bnez a0, 3f
        addi s10, s10, -1
        bnez s10, 2b
        la t0, done1
        li t1, 1
        sw t1, 0(t0)
3:      wfi
        j 3b

/* Supervisor software interrupt: count it for this hart (tp). Uses only
 * t5 and t6, which the code it interrupts does not. */
        .align 2
strap:
        csrr t5, scause
        li t6, CAUSE_S_SOFT
        bne t5, t6, 1f
        csrci sip, SIE_SSIE
        la t5, count0
        slli t6, tp, 2
        add t5, t5, t6
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
msg_entry:      .asciz "OS-ENTRY riscv ipi-smp\n"
msg_ok:         .asciz "ipi-smp=ok\n"
msg_bad:        .asciz "ipi-smp=bad "
msg_trap:       .asciz "ipi-smp=bad unexpected trap\n"
        .align 2
ready:          .word 0
count0:         .word 0                     /* IPIs taken by hart 0 */
count1:         .word 0                     /* and by hart 1 */
go:             .word 0                     /* hart 1: start fencing */
done1:          .word 0                     /* hart 1: done fencing */
