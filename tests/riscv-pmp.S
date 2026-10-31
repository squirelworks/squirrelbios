/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * The firmware region (0x80000000-0x801fffff) must be closed to S-mode:
 * a store, a load and an instruction fetch there each take an access
 * fault at this image's stvec, with stval the address. A store into this
 * image's own memory still works, and an ecall still reaches the SBI in
 * M-mode. Prints pmp=ok, or pmp=bad and the number of the failed check.
 * Original code.
 */

        .equ UART_BASE, 0x10000000
        .equ FW_BASE,   0x80000000
        .equ FW_LAST,   0x801ffff8          /* last dword of the region */
        .equ CAUSE_FETCH_FAULT, 1
        .equ CAUSE_LOAD_FAULT,  5
        .equ CAUSE_STORE_FAULT, 7
        .equ SBI_ERR_NOT_SUPPORTED, -2

/* Expect an access fault with cause \cause at address \addr: s1 = cause,
 * s2 = address, s3 = where the handler resumes, s4 = check number. The
 * handler records the trap in s5 (cause) and s6 (stval). */
.macro arm cause, addr, n
        li s1, \cause
        li s2, \addr
        la s3, 9f
        li s4, \n
        li s5, -1
        li s6, -1
.endm
.macro check
9:      bne s5, s1, bad
        bne s6, s2, bad
.endm

        .option norelax                     /* no gp-relative addressing: gp is not set up */

.global _start
_start:
        la a0, msg_entry
        call puts
        la t0, strap
        csrw stvec, t0
        li s3, 0

        arm CAUSE_STORE_FAULT, FW_BASE, 1
        sd zero, 0(s2)
        check
        arm CAUSE_STORE_FAULT, FW_LAST, 2
        sd zero, 0(s2)
        check
        arm CAUSE_LOAD_FAULT, FW_BASE, 3
        ld t0, 0(s2)
        check
        arm CAUSE_FETCH_FAULT, FW_BASE, 4
        jr s2
        check

        li s4, 5                            /* 5: own memory not writable */
        la t0, scratch
        li t1, 0x5157
        sd t1, 0(t0)
        ld t2, 0(t0)
        bne t1, t2, bad

        li s4, 6                            /* 6: the SBI no longer answers */
        li a7, 0x0a000000                   /* no such extension */
        li a6, 0
        ecall
        li t0, SBI_ERR_NOT_SUPPORTED
        bne a0, t0, bad

        la a0, msg_ok
        call puts
        j done

bad:    la a0, msg_bad
        call puts
        addi t2, s4, '0'
        li t0, UART_BASE
        sb t2, 0(t0)
        li t1, '\n'
        sb t1, 0(t0)
done:   wfi
        j done

/* Record the trap and resume at s3. An unexpected trap (s3 = 0) fails. */
        .align 2
strap:
        csrr s5, scause
        csrr s6, stval
        beqz s3, bad
        csrw sepc, s3
        li s3, 0
        sret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

        .data
msg_entry:      .asciz "OS-ENTRY riscv pmp\n"
msg_ok:         .asciz "pmp=ok\n"
msg_bad:        .asciz "pmp=bad "
        .align 3
scratch:        .dword 0
