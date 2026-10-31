/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Probes IPI, then asks for an IPI to hart 1, which is parked and not
 * started: that must fail and raise nothing. Then IPIs itself (a0 = 1,
 * a1 = its hart id) and prints ipi=ok from its software-interrupt handler.
 * Original code.
 */

        .option norelax

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_IPI,       0x735049
        .equ CAUSE_S_SOFT,      (1 << 63) | 1
        .equ SIP_SSIP,          (1 << 1)
        .equ SIE_SSIE,          (1 << 1)
        .equ SSTATUS_SIE,       (1 << 1)

.global _start
_start:
        mv s0, a0                           /* hart ID */
        la a0, msg_entry
        call puts

        li a7, SBI_EXT_BASE                 /* probe_extension(IPI) */
        li a6, 3
        li a0, SBI_EXT_IPI
        ecall
        la a0, msg_probe_bad
        beqz a1, fail

        li a0, 1                            /* hart 1 only: not started */
        li a1, 1
        li a7, SBI_EXT_IPI
        li a6, 0
        ecall
        mv t0, a0
        la a0, msg_other_bad
        beqz t0, fail
        csrr t0, sip
        andi t0, t0, SIP_SSIP
        bnez t0, fail

        la t0, strap
        csrw stvec, t0
        li t0, SIE_SSIE
        csrs sie, t0

        li a0, 1                            /* bit 0 above base = this hart */
        mv a1, s0
        li a7, SBI_EXT_IPI
        li a6, 0
        ecall
        mv t0, a0
        la a0, msg_self_bad
        bnez t0, fail
        csrsi sstatus, SSTATUS_SIE          /* now take it */
1:      wfi
        j 1b

fail:   call puts
2:      wfi
        j 2b

        .align 2
strap:
        csrr t0, scause
        li t1, CAUSE_S_SOFT
        la a0, msg_cause_bad
        bne t0, t1, fail
        li t0, SIP_SSIP
        csrc sip, t0
        la a0, msg_ok
        call puts
        sret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv ipi\n"
msg_ok:         .asciz "ipi=ok\n"
msg_probe_bad:  .asciz "ipi=bad probe\n"
msg_other_bad:  .asciz "ipi=bad hart 1 was accepted\n"
msg_self_bad:   .asciz "ipi=bad send_ipi failed\n"
msg_cause_bad:  .asciz "ipi=bad scause\n"
