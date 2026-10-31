/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Calls the SBI base extension and prints sbi=ok when every answer is
 * right and no register other than a0 and a1 changed across any call.
 * On a wrong answer it prints sbi=bad and the number of the failed check.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_TIME,      0x54494d45
        .equ SBI_EXT_IPI,       0x735049
        .equ SBI_EXT_RFENCE,    0x52464e43
        .equ SBI_EXT_HSM,       0x48534d
        .equ SBI_EXT_SRST,      0x53525354
        .equ SBI_IMPL_ID,       0x5351      /* as recorded in common/handoff.md */
        .equ SBI_ERR_NOT_SUPPORTED, -2

/* ecall a7 = ext, a6 = fid, a0 = arg through checked_call */
.macro sbi ext, fid, arg=0
        li a7, \ext
        li a6, \fid
        li a0, \arg
        call checked_call
.endm

/* Fail as check \n unless reg == value */
.macro expect reg, value, n
        li t0, \value
        li t2, \n
        bne \reg, t0, bad
.endm

        .option norelax                     /* no gp-relative addressing: gp is not set up */

.global _start
_start:
        la a0, msg_entry
        call puts

        sbi SBI_EXT_BASE, 0                 /* get_spec_version */
        expect a0, 0, 1
        expect a1, 0x01000000, 1

        sbi SBI_EXT_BASE, 1                 /* get_impl_id */
        expect a0, 0, 2
        expect a1, SBI_IMPL_ID, 2

        sbi SBI_EXT_BASE, 3, SBI_EXT_BASE   /* probe_extension(base) */
        expect a0, 0, 3
        li t2, 3
        beqz a1, bad

        sbi SBI_EXT_BASE, 3, SBI_EXT_TIME   /* present */
        expect a0, 0, 4
        li t2, 4
        beqz a1, bad
        sbi SBI_EXT_BASE, 3, SBI_EXT_IPI    /* present */
        expect a0, 0, 5
        li t2, 5
        beqz a1, bad
        sbi SBI_EXT_BASE, 3, SBI_EXT_RFENCE /* present */
        expect a0, 0, 6
        li t2, 6
        beqz a1, bad
        sbi SBI_EXT_BASE, 3, SBI_EXT_HSM    /* present */
        expect a0, 0, 7
        li t2, 7
        beqz a1, bad
        sbi SBI_EXT_BASE, 3, SBI_EXT_SRST
        expect a0, 0, 8
        expect a1, 0, 8

        sbi 0x0a000000, 0                   /* no such extension */
        expect a0, SBI_ERR_NOT_SUPPORTED, 9

        la t0, clobbered
        ld t0, 0(t0)
        li t2, 0                            /* 0: a register changed */
        bnez t0, bad

        la a0, msg_ok
        call puts
        j done

bad:    la a0, msg_bad
        call puts
        addi t2, t2, '0'
        li t0, UART_BASE
        sb t2, 0(t0)
        li t2, '\n'
        sb t2, 0(t0)

done:   wfi
        j done

/* ecall with every register except sp, a0 and a1 set from regs_in, then
 * compare them all with regs_in. a0, a6 and a7 are the call's arguments.
 * Sets clobbered on a difference. Returns a0 and a1 from the call. */
checked_call:
        la t0, regs_in
        sd ra, 8 * 1(t0)
        sd a0, 8 * 10(t0)
        sd a6, 8 * 16(t0)
        sd a7, 8 * 17(t0)
        la sp, regs_out                     /* the SBI must preserve sp too */
        la gp, regs_in
        .irp n, 1,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        ld x\n, 8 * \n(gp)
        .endr
        ld gp, 8 * 3(gp)
        ecall
        .irp n, 1,3,4,5,6,7,8,9,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        sd x\n, 8 * \n(sp)
        .endr
        la t0, regs_out
        bne sp, t0, 3f
        la t1, regs_in
        li t2, 1
        li t3, 32
1:      li t4, 2                            /* sp, a0, a1 are not compared */
        beq t2, t4, 2f
        li t4, 10
        beq t2, t4, 2f
        li t4, 11
        beq t2, t4, 2f
        slli t4, t2, 3
        add t5, t0, t4
        add t6, t1, t4
        ld t5, 0(t5)
        ld t6, 0(t6)
        bne t5, t6, 3f
2:      addi t2, t2, 1
        bne t2, t3, 1b
        j 4f
3:      la t0, clobbered
        li t1, 1
        sd t1, 0(t0)
4:      la t0, regs_in
        ld ra, 8 * 1(t0)
        ret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv sbi\n"
msg_ok:         .asciz "sbi=ok\n"
msg_bad:        .asciz "sbi=bad "

        .data
        .align 3
clobbered:      .dword 0
/* Register n is regs_in[n]: a different pattern per register */
regs_in:
        .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        .dword 0x5a5a000000000000 + (\n << 32) + 0xc0de0000 + \n
        .endr
regs_out:
        .skip 8 * 32
