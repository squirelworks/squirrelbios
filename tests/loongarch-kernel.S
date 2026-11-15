/* A test kernel for the LoongArch64 firmware (loongarch/squirrel-bios.S).
 * Original code. Entered at 0x00200000 with a0 = RAM bytes, a1 = the
 * command line or 0, a2 = 0; prints on the NS16550
 *
 *   KERNEL loongarch64 pc=ok mem=ok a2=ok cmdline="..."
 *
 * (cmdline=none without one) and spins. */
        .equ UART, 0x1fe001e0

        .text
        .globl _start
_start:
        li.d    $s0, UART
        move    $s1, $a0
        move    $s2, $a1
        move    $s3, $a2
        la.pcrel $a0, s_head
        bl      puts
        /* where it runs */
        la.pcrel $t1, _start
        li.d    $t2, 0x00200000
        la.pcrel $a0, s_ok
        beq     $t1, $t2, 1f
        la.pcrel $a0, s_bad
1:      bl      puts
        la.pcrel $a0, s_mem
        bl      puts
        li.d    $t0, 64 << 20
        la.pcrel $a0, s_ok
        bgeu    $s1, $t0, 1f
        la.pcrel $a0, s_bad
1:      bl      puts
        la.pcrel $a0, s_a2
        bl      puts
        la.pcrel $a0, s_ok
        beqz    $s3, 1f
        la.pcrel $a0, s_bad
1:      bl      puts
        la.pcrel $a0, s_cmd
        bl      puts
        beqz    $s2, 2f
        la.pcrel $a0, s_q
        bl      puts
        move    $a0, $s2
        bl      puts
        la.pcrel $a0, s_q
        bl      puts
        b       3f
2:      la.pcrel $a0, s_none
        bl      puts
3:      la.pcrel $a0, s_nl
        bl      puts
4:      b       4b

puts:
1:      ld.bu   $t3, $a0, 0
        beqz    $t3, 3f
2:      ld.bu   $t4, $s0, 5
        andi    $t4, $t4, 0x20
        beqz    $t4, 2b
        st.b    $t3, $s0, 0
        addi.d  $a0, $a0, 1
        b       1b
3:      jr      $ra

s_head: .asciz "KERNEL loongarch64 pc="
s_mem:  .asciz " mem="
s_a2:   .asciz " a2="
s_cmd:  .asciz " cmdline="
s_ok:   .asciz "ok"
s_bad:  .asciz "bad"
s_q:    .asciz "\""
s_none: .asciz "none"
s_nl:   .asciz "\r\n"
