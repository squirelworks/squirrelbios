/* Smoke-test image for SquirrelBIOS RISC-V ras: fault line.
 * Run with -smp 2. Hart 0 starts hart 1, which prints hart1=up and then
 * waits two seconds before printing hart1=still. Hart 0 then ecalls the
 * firmware's smoke-only SBI_EXT_TEST_FAULT so the SBI path faults in
 * M-mode. The firmware must print a ras: line, park hart 1 (so
 * hart1=still never appears), and halt.
 * Original code.
 */

        .option norelax

        .equ UART_BASE,         0x10000000
        .equ SBI_EXT_HSM,       0x48534d
        .equ SBI_EXT_TEST_FAULT, 0x53515241  /* "SQRA"; firmware smoke only */
        .equ WAIT,              50000000    /* 5 s at virt's 10 MHz */
        .equ STILL_DELAY,       20000000    /* 2 s: long enough that a park must beat it */

.global _start
_start:
        bnez a0, hart1

        la a0, msg_entry
        call puts

        li a0, 1
        la a1, hart1
        li a2, 0
        li a7, SBI_EXT_HSM
        li a6, 0
        ecall
        bnez a0, bad

        la t0, ready
        li t1, 1
        rdtime t2
        li t4, WAIT
        add t2, t2, t4
1:      lw t3, 0(t0)
        beq t3, t1, 2f
        rdtime t4
        bltu t4, t2, 1b
        j bad
2:
        /* Fault in the SBI handler: parks hart 1, prints ras:, halts. */
        li a7, SBI_EXT_TEST_FAULT
        li a6, 0
        ecall
        j bad                               /* must not return */

bad:
        la a0, msg_bad
        call puts
1:      wfi
        j 1b

hart1:
        la a0, msg_up
        call puts
        la t0, ready
        li t1, 1
        sw t1, 0(t0)
        fence w, w
        rdtime t0
        li t1, STILL_DELAY
        add t0, t0, t1
1:      rdtime t1
        bltu t1, t0, 1b
        la a0, msg_still
        call puts
2:      wfi
        j 2b

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

        .align 2
ready:  .word 0

msg_entry:      .asciz "OS-ENTRY riscv ras\n"
msg_up:         .asciz "hart1=up\n"
msg_still:      .asciz "hart1=still\n"
msg_bad:        .asciz "ras=bad\n"
