/* Smoke-test image for SquirrelBIOS AArch64, linked at 0x40200000.
 * Stands in for the OS: checks the hand-off state and reports on the PL011.
 * Original code.
 */

        .equ UART_BASE, 0x09000000

.global _start
_start:
        mov x20, x0                         /* DTB from the firmware */
        orr x21, x1, x2
        orr x21, x21, x3                    /* x1-x3 must be zero */

        adr x0, msg_entry
        bl puts

        adr x0, msg_dtb_bad
        cbz x20, 1f
        ldr w1, [x20]
        rev w1, w1
        ldr w2, =0xd00dfeed
        cmp w1, w2
        b.ne 1f
        adr x0, msg_dtb_ok
1:      bl puts

        adr x0, msg_regs_ok
        cbz x21, 2f
        adr x0, msg_regs_bad
2:      bl puts

        /* SCTLR of the current EL: M (0), C (2) and EE (25) clear;
         * DAIF fully masked */
        adr x0, msg_sctlr_bad
        mrs x1, CurrentEL
        ubfx x1, x1, #2, #2
        cmp x1, #2
        b.eq 4f
        b.hi 5f
        mrs x2, sctlr_el1
        b 6f
4:      mrs x2, sctlr_el2
        b 6f
5:      mrs x2, sctlr_el3
6:      ldr x3, =((1 << 0) | (1 << 2) | (1 << 25))
        tst x2, x3
        b.ne 7f
        mrs x2, daif
        cmp x2, #0x3c0
        b.ne 7f
        adr x0, msg_sctlr_ok
7:      bl puts

3:      wfi
        b 3b

puts:
        ldr x1, =UART_BASE
1:      ldrb w2, [x0], #1
        cbz w2, 2f
        str w2, [x1]
        b 1b
2:      ret

        .ltorg

msg_entry:      .asciz "OS-ENTRY arm"
msg_dtb_ok:     .asciz " dtb=ok"
msg_dtb_bad:    .asciz " dtb=bad"
msg_regs_ok:    .asciz " regs=ok"
msg_regs_bad:   .asciz " regs=bad"
msg_sctlr_ok:   .asciz " sctlr=ok\n"
msg_sctlr_bad:  .asciz " sctlr=bad\n"
