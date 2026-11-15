/* A test kernel for the MIPS Malta firmware (mips/squirrel-bios.S).
 * Original code. Linked at 0x80100000 and entered there the YAMON way:
 * a0 = argc, a1 = argv, a2 = envp, a3 = RAM bytes. Prints on COM1
 *
 *   KERNEL mips pc=ok argc=N memsize=ok env=ok cmdline="..."
 *
 * (env=ok when envp holds "memsize" with a3's value in decimal;
 * cmdline=none when argc is 1) and spins. */
        .equ UART, 0xb00003f8

        .text
        .globl _start
_start:
        move    $s0, $a0
        move    $s1, $a1
        move    $s2, $a2
        move    $s3, $a3
        la      $a0, s_head
        jal     puts
        bal     1f
1:      li      $t0, 0x80100000
        la      $a0, s_ok
        sltu    $t1, $ra, $t0
        beqz    $t1, 2f
        la      $a0, s_bad
2:      li      $t0, 0x80101000
        sltu    $t1, $ra, $t0
        bnez    $t1, 3f
        la      $a0, s_bad
3:      jal     puts
        la      $a0, s_argc
        jal     puts
        addiu   $a0, $s0, '0'
        jal     putc
        la      $a0, s_mem
        jal     puts
        la      $a0, s_ok
        li      $t0, 64 << 20
        bgeu    $s3, $t0, 4f
        la      $a0, s_bad
4:      jal     puts
        /* envp[0] = "memsize", envp[1] = decimal of a3 */
        la      $a0, s_env
        jal     puts
        lw      $a0, 0($s2)
        la      $a1, s_memsize
        jal     streq
        beqz    $v0, 6f
        lw      $t0, 4($s2)
        move    $t1, $zero
5:      lbu     $t2, 0($t0)
        beqz    $t2, 7f
        addiu   $t2, $t2, -'0'
        li      $t3, 10
        mul     $t1, $t1, $t3
        addu    $t1, $t1, $t2
        addiu   $t0, $t0, 1
        b       5b
7:      bne     $t1, $s3, 6f
        la      $a0, s_ok
        b       8f
6:      la      $a0, s_bad
8:      jal     puts
        la      $a0, s_cmd
        jal     puts
        li      $t0, 2
        bne     $s0, $t0, 9f
        la      $a0, s_q
        jal     puts
        lw      $a0, 4($s1)
        jal     puts
        la      $a0, s_q
        jal     puts
        b       10f
9:      la      $a0, s_none
        jal     puts
10:     la      $a0, s_nl
        jal     puts
11:     b       11b

putc:
        li      $t8, UART
1:      lbu     $t9, 5($t8)
        andi    $t9, $t9, 0x20
        beqz    $t9, 1b
        sb      $a0, 0($t8)
        jr      $ra

puts:
        move    $t7, $ra
        move    $t6, $a0
1:      lbu     $a0, 0($t6)
        beqz    $a0, 2f
        jal     putc
        addiu   $t6, $t6, 1
        b       1b
2:      jr      $t7

streq:
1:      lbu     $t0, 0($a0)
        lbu     $t1, 0($a1)
        bne     $t0, $t1, 2f
        addiu   $a0, $a0, 1
        addiu   $a1, $a1, 1
        bnez    $t0, 1b
        li      $v0, 1
        jr      $ra
2:      move    $v0, $zero
        jr      $ra

s_head:    .asciz "KERNEL mips pc="
s_argc:    .asciz " argc="
s_mem:     .asciz " memsize="
s_env:     .asciz " env="
s_cmd:     .asciz " cmdline="
s_ok:      .asciz "ok"
s_bad:     .asciz "bad"
s_q:       .asciz "\""
s_none:    .asciz "none"
s_nl:      .asciz "\r\n"
s_memsize: .asciz "memsize"
