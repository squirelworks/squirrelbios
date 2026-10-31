/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Stands in for the OS: checks the hand-off state and reports on the UART.
 * Built with --defsym BOOT_HART=N, hart=ok means it entered on hart N.
 * Original code.
 */

        .equ UART_BASE, 0x10000000

.global _start
_start:
        mv s0, a0                           /* hart ID */
        mv s1, a1                           /* DTB */
        mv s2, a2                           /* must be 0 */

        la a0, msg_entry
        call puts

        la a0, msg_hart_bad
.ifdef BOOT_HART                            /* riscv-boot-hart: that hart, not 0 */
        li t0, BOOT_HART
        bne s0, t0, 1f
.else
        bnez s0, 1f
.endif
        la a0, msg_hart_ok
1:      call puts

        la a0, msg_dtb_bad
        beqz s1, 2f
        lwu t0, 0(s1)
        li t1, 0xedfe0dd0                   /* d00dfeed, read little-endian */
        bne t0, t1, 2f
        la a0, msg_dtb_ok
2:      call puts

        la a0, msg_a2_bad
        bnez s2, 3f
        la a0, msg_a2_ok
3:      call puts

.ifdef BOOT_HART
        /* HSM sees the boot hart as started and hart 0 as stopped: then
         * hart_start brings hart 0 up, and it reports from S-mode */
        la s3, msg_hsm_bad
        mv a0, s0
        li a7, 0x48534d                     /* HSM hart_get_status */
        li a6, 2
        ecall
        bnez a0, 5f
        bnez a1, 5f                         /* not STARTED */
        li a0, 0
        li a7, 0x48534d
        li a6, 2
        ecall
        bnez a0, 5f
        li t0, 1                            /* STOPPED */
        bne a1, t0, 5f
        li a0, 0
        la a1, hart0
        li a2, 0
        li a7, 0x48534d                     /* hart_start(0, hart0, 0) */
        li a6, 0
        ecall
        bnez a0, 5f
        j 4f
5:      mv a0, s3
        call puts
.endif
4:      wfi
        j 4b

.ifdef BOOT_HART
hart0:  bnez a0, 4b
        la a0, msg_hart0_ok
        call puts
        j 4b
.endif
        j 4b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

msg_entry:      .asciz "OS-ENTRY riscv"
msg_hart_ok:    .asciz " hart=ok"
msg_hart_bad:   .asciz " hart=bad"
msg_dtb_ok:     .asciz " dtb=ok"
msg_dtb_bad:    .asciz " dtb=bad"
msg_a2_ok:      .asciz " a2=ok\n"
msg_a2_bad:     .asciz " a2=bad\n"
msg_hsm_bad:    .asciz "boot-hart=bad hsm\n"
msg_hart0_ok:   .asciz "hart0 started from the boot hart\n"
