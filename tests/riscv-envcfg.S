/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Uses what the firmware turns on in menvcfg for this QEMU hart, the way
 * Linux does: cbo.zero clears a cache block (Zicboz), cbo.clean,
 * cbo.flush and cbo.inval run without trapping (Zicbom), and a timer
 * set by writing stimecmp directly raises the supervisor timer interrupt
 * (Sstc). Prints envcfg=ok, or envcfg=bad and the number of the failed
 * check; an illegal-instruction trap prints envcfg=trap.
 * Original code.
 */

        .equ UART_BASE, 0x10000000
        .equ CAUSE_S_TIMER,     (1 << 63) | 5
        .equ SIE_STIE,          (1 << 5)
        .equ SSTATUS_SIE,       (1 << 1)
        .equ CSR_STIMECMP,      0x14d
        .equ TICKS_10MS,        100000      /* QEMU virt timebase: 10 MHz */
        .equ BLOCK,             64          /* QEMU's cbo block size */

/* cbo.* on the block at a0: MISC-MEM, funct3 2, rs1 = a0 (x10) */
.macro cbo op
        .word (\op << 20) | (10 << 15) | (2 << 12) | 0x0f
.endm
        .equ CBO_INVAL, 0
        .equ CBO_CLEAN, 1
        .equ CBO_FLUSH, 2
        .equ CBO_ZERO,  4

        .option norelax                     /* no gp-relative addressing: gp is not set up */

.global _start
_start:
        la sp, stack_top
        la a0, msg_entry
        call puts
        la t0, strap
        csrw stvec, t0

        li s1, 1                            /* 1: cbo.zero left data */
        la a0, block
        li t0, -1
        li t1, 0
1:      add t2, a0, t1
        sd t0, 0(t2)
        addi t1, t1, 8
        li t2, BLOCK
        bltu t1, t2, 1b
        cbo CBO_ZERO
        li t1, 0
1:      add t2, a0, t1
        ld t0, 0(t2)
        bnez t0, bad
        addi t1, t1, 8
        li t2, BLOCK
        bltu t1, t2, 1b

        li s1, 2                            /* 2: Zicbom (a trap shows as envcfg=trap) */
        cbo CBO_CLEAN
        cbo CBO_FLUSH
        cbo CBO_INVAL

        li s1, 3                            /* 3: Sstc timer */
        li t0, SIE_STIE
        csrs sie, t0
        rdtime t0
        li t1, TICKS_10MS
        add t0, t0, t1
        csrw CSR_STIMECMP, t0
        csrsi sstatus, SSTATUS_SIE
1:      wfi
        j 1b

bad:    la a0, msg_bad
        call puts
        li t0, UART_BASE
        addi t1, s1, '0'
        sb t1, 0(t0)
        li t1, '\n'
        sb t1, 0(t0)
done:   wfi
        j done

        .align 2
strap:
        csrr t0, scause
        li t1, CAUSE_S_TIMER
        bne t0, t1, 1f
        li t0, -1                           /* push the timer out: STIP drops */
        csrw CSR_STIMECMP, t0
        csrr t0, sip
        andi t0, t0, SIE_STIE
        li s1, 4                            /* 4: still pending after that */
        bnez t0, bad
        la a0, msg_ok
        call puts
        j done
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
msg_entry:      .asciz "OS-ENTRY riscv envcfg\n"
msg_ok:         .asciz "envcfg=ok\n"
msg_bad:        .asciz "envcfg=bad "
msg_trap:       .asciz "envcfg=trap\n"
        .align 6
block:          .skip BLOCK
        .align 4
        .skip 1024
stack_top:
