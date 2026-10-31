/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Run on -M virt,aia=aplic. Drives the S-level APLIC (direct mode) the
 * way an OS would: the UART's source 10, level high, to hart 0's IDC,
 * with the UART's transmit-empty interrupt on. Prints aia=ok from the
 * supervisor external interrupt handler once claimi names source 10.
 * Without the firmware's delegation the S domain's sourcecfg[10] stays
 * 0 and nothing arrives.
 * Original code.
 */

        .option norelax

        .equ UART_BASE,         0x10000000
        .equ UART_IER,          1
        .equ UART_IRQ,          10          /* QEMU virt UART0_IRQ */
        .equ APLIC_S,           0x0d000000  /* QEMU virt S-level APLIC, socket 0 */
        .equ APLIC_DOMAINCFG,   0x0000
        .equ APLIC_DOMAINCFG_IE, 0x100
        .equ APLIC_SOURCECFG,   0x0000      /* + 4 * source */
        .equ APLIC_SM_LEVEL_HIGH, 6
        .equ APLIC_SETIENUM,    0x1edc
        .equ APLIC_TARGET,      0x3000      /* + 4 * source */
        .equ APLIC_IDC,         0x4000      /* + 32 * hart index */
        .equ IDC_IDELIVERY,     0x00
        .equ IDC_ITHRESHOLD,    0x08
        .equ IDC_CLAIMI,        0x1c
        .equ SIE_SEIE,          0x200
        .equ SCAUSE_SEI,        0x8000000000000009
        .equ WAIT,              20000000    /* 2 s at virt's 10 MHz */

.global _start
_start:
        bnez a0, done                       /* hart 0 only */
        la a0, msg_entry
        call puts

        li s0, APLIC_S
        li t0, APLIC_SM_LEVEL_HIGH
        sw t0, APLIC_SOURCECFG + 4 * UART_IRQ(s0)
        lw t1, APLIC_SOURCECFG + 4 * UART_IRQ(s0)
        la a0, msg_not_delegated            /* reads 0 unless delegated */
        bne t0, t1, fail
        li t2, APLIC_S + APLIC_TARGET + 4 * UART_IRQ
        li t0, 1                            /* hart index 0, priority 1 */
        sw t0, 0(t2)
        li t2, APLIC_S + APLIC_IDC
        sw t0, IDC_IDELIVERY(t2)
        sw zero, IDC_ITHRESHOLD(t2)
        li t0, APLIC_DOMAINCFG_IE
        sw t0, APLIC_DOMAINCFG(s0)
        li t2, APLIC_S + APLIC_SETIENUM
        li t0, UART_IRQ
        sw t0, 0(t2)

        la t0, handler
        csrw stvec, t0
        li t0, SIE_SEIE
        csrs sie, t0
        csrsi sstatus, 2                    /* SIE */
        li t0, UART_BASE                    /* transmit empty: raises it now */
        li t1, 2
        sb t1, UART_IER(t0)

        rdtime s1
        li t0, WAIT
        add s1, s1, t0
1:      rdtime t0
        bltu t0, s1, 1b
        csrci sstatus, 2
        la a0, msg_timeout
        j fail

        .align 2
handler:
        csrci sstatus, 2
        li t0, UART_BASE
        sb zero, UART_IER(t0)               /* the source drops */
        csrr t0, scause
        li t1, SCAUSE_SEI
        la a0, msg_cause
        bne t0, t1, fail
        li t2, APLIC_S + APLIC_IDC
        lw t0, IDC_CLAIMI(t2)
        srli t0, t0, 16                     /* identity */
        li t1, UART_IRQ
        la a0, msg_claim
        bne t0, t1, fail
        la a0, msg_ok
fail:   call puts
done:   wfi
        j done

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv aia\n"
msg_ok:         .asciz "aia=ok\n"
msg_not_delegated: .asciz "aia=bad source 10 not delegated to the S domain\n"
msg_timeout:    .asciz "aia=bad no interrupt\n"
msg_cause:      .asciz "aia=bad scause is not a supervisor external interrupt\n"
msg_claim:      .asciz "aia=bad claimi did not name source 10\n"
