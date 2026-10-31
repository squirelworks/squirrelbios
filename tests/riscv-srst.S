/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Probes the SBI SRST extension, checks that bad arguments are refused,
 * prints reset=ok and calls system_reset with RESET_TYPE and RESET_REASON
 * (both 0, a shutdown, unless set with --defsym). The call must not
 * return: if it does, or a check fails, the image prints reset=bad and
 * the number of the check.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

        .equ SBI_EXT_BASE,      0x10
        .equ SBI_EXT_SRST,      0x53525354
        .equ SBI_ERR_NOT_SUPPORTED, -2
        .equ SBI_ERR_INVALID_PARAM, -3

        .ifndef RESET_TYPE
        .equ RESET_TYPE, 0
        .endif
        .ifndef RESET_REASON
        .equ RESET_REASON, 0
        .endif

/* ecall a7 = ext, a6 = fid, a0 and a1 = arguments */
.macro sbi ext, fid, arg0=0, arg1=0
        li a7, \ext
        li a6, \fid
        li a0, \arg0
        li a1, \arg1
        ecall
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

        sbi SBI_EXT_BASE, 3, SBI_EXT_SRST   /* probe_extension */
        expect a0, 0, 1
        expect a1, 1, 1
        sbi SBI_EXT_SRST, 0, 3, 0           /* no such type */
        expect a0, SBI_ERR_INVALID_PARAM, 2
        sbi SBI_EXT_SRST, 0, 0, 2           /* no such reason */
        expect a0, SBI_ERR_INVALID_PARAM, 3
        sbi SBI_EXT_SRST, 1                 /* no such function */
        expect a0, SBI_ERR_NOT_SUPPORTED, 4

        la a0, msg_ok
        call puts
        sbi SBI_EXT_SRST, 0, RESET_TYPE, RESET_REASON
        li t2, 5                            /* 5: system_reset returned */

bad:    la a0, msg_bad
        call puts
        addi t2, t2, '0'
        li t0, UART_BASE
        sb t2, 0(t0)
        li t1, '\n'
        sb t1, 0(t0)
1:      wfi
        j 1b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

        .data
msg_entry:      .asciz "OS-ENTRY riscv srst\n"
msg_ok:         .asciz "reset=ok\n"
msg_bad:        .asciz "reset=bad "
