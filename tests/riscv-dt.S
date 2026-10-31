/* Smoke-test image for SquirrelBIOS RISC-V, linked at 0x80200000.
 * Walks the device tree in a1 and checks that the firmware added
 * /reserved-memory (#address-cells = #size-cells = 2, ranges) with a
 * child whose reg is <0 0x80000000 0 0x200000> and that has no-map, and
 * that the rest of the tree still parses (a /memory node, the header
 * sizes, END where the structure block ends). Prints dt=ok, or dt=bad
 * and the number of the failed check (4 is followed by the found-bits).
 * Original code.
 */

        .equ UART_BASE, 0x10000000
        .equ FDT_MAGIC, 0xd00dfeed
        .equ F_ADDR,  1                     /* #address-cells = 2 */
        .equ F_SIZE,  2                     /* #size-cells = 2 */
        .equ F_RNG,   4                     /* ranges, empty */
        .equ F_REG,   8                     /* child reg is the firmware region */
        .equ F_NOMAP, 16                    /* child no-map */
        .equ F_MEM,   32                    /* a /memory@... node */
        .equ F_RM,    64                    /* /reserved-memory */
        .equ F_ALL,   127

/* rd = big-endian word at off(base) */
.macro ldbe rd, off, base, tmp
        lbu \rd, \off(\base)
        slli \rd, \rd, 8
        lbu \tmp, \off+1(\base)
        or \rd, \rd, \tmp
        slli \rd, \rd, 8
        lbu \tmp, \off+2(\base)
        or \rd, \rd, \tmp
        slli \rd, \rd, 8
        lbu \tmp, \off+3(\base)
        or \rd, \rd, \tmp
.endm

/* Branch to \to if the name at s7 is \str (a .data label) */
.macro ifname str, to
        mv a0, s7
        la a1, \str
        call strcmp
        beqz a0, \to
.endm

        .option norelax                     /* no gp-relative addressing: gp is not set up */

.global _start
_start:
        mv s0, a1                           /* device tree */
        la sp, stack_top
        la a0, msg_entry
        call puts

        li s11, 1                           /* 1: magic */
        ldbe t0, 0, s0, t1
        li t1, FDT_MAGIC
        bne t0, t1, bad
        li s11, 2                           /* 2: header sizes */
        ldbe t0, 4, s0, t1                  /* totalsize */
        ldbe t2, 12, s0, t1                 /* off_dt_strings */
        ldbe t3, 32, s0, t1                 /* size_dt_strings */
        add t4, t2, t3
        bne t0, t4, bad
        add s1, s0, t2                      /* s1 = strings block */
        ldbe t0, 8, s0, t1                  /* off_dt_struct */
        ldbe t3, 36, s0, t1                 /* size_dt_struct */
        add s2, s0, t0                      /* s2 = token */
        add s3, s2, t3                      /* s3 = end of structure block */

        li s11, 3                           /* 3: walk */
        li s4, 0                            /* depth */
        li s5, 0                            /* found bits */
        li s6, 0                            /* depth of /reserved-memory, 0 = outside */
walk:   bgeu s2, s3, bad
        ldbe t0, 0, s2, t1
        addi s2, s2, 4
        li t1, 1
        beq t0, t1, node
        li t1, 2
        beq t0, t1, endnode
        li t1, 3
        beq t0, t1, prop
        li t1, 4
        beq t0, t1, walk
        li t1, 9
        bne t0, t1, bad
        bne s2, s3, bad                     /* END is the last token */
        li s11, 4                           /* 4: something missing */
        li t0, F_ALL
        bne s5, t0, bad
        la a0, msg_ok
        call puts
        j done

node:   addi s4, s4, 1
        mv s7, s2                           /* s7 = name */
        li t0, 2
        bne s4, t0, 2f
        ifname str_rm, 3f
        mv a0, s7
        la a1, str_memory
        call strprefix
        beqz a0, 2f
        ori s5, s5, F_MEM
        j 2f
3:      ori s5, s5, F_RM
        mv s6, s4
2:      lbu t0, 0(s2)                       /* skip the name */
        addi s2, s2, 1
        bnez t0, 2b
        addi s2, s2, 3
        andi s2, s2, -4
        j walk

endnode:
        bne s4, s6, 1f
        li s6, 0                            /* leaving /reserved-memory */
1:      addi s4, s4, -1
        j walk

prop:   ldbe s8, 0, s2, t1                  /* s8 = length */
        ldbe t0, 4, s2, t1
        add s7, s1, t0                      /* s7 = name */
        addi s9, s2, 8                      /* s9 = value */
        addi t0, s8, 8 + 3
        andi t0, t0, -4
        add s2, s2, t0
        beqz s6, walk
        beq s4, s6, rmprop
        addi t0, s6, 1
        bne s4, t0, walk
        ifname str_reg, reg                 /* a property of a child */
        ifname str_nomap, nomap
        j walk
reg:    li t0, 16
        bne s8, t0, walk
        ldbe t0, 0, s9, t1
        bnez t0, walk
        ldbe t0, 4, s9, t1
        li t1, 0x80000000
        bne t0, t1, walk
        ldbe t0, 8, s9, t1
        bnez t0, walk
        ldbe t0, 12, s9, t1
        li t1, 0x200000
        bne t0, t1, walk
        ori s5, s5, F_REG
        j walk
nomap:  bnez s8, walk
        ori s5, s5, F_NOMAP
        j walk

rmprop: ifname str_addr, addr
        ifname str_size, size
        ifname str_rng, rng
        j walk
addr:   ldbe t0, 0, s9, t1
        li t1, 2
        bne t0, t1, walk
        ori s5, s5, F_ADDR
        j walk
size:   ldbe t0, 0, s9, t1
        li t1, 2
        bne t0, t1, walk
        ori s5, s5, F_SIZE
        j walk
rng:    bnez s8, walk
        ori s5, s5, F_RNG
        j walk

bad:    la a0, msg_bad
        call puts
        li t0, UART_BASE
        addi t1, s11, '0'
        sb t1, 0(t0)
        li t1, 4
        bne s11, t1, 1f
        li t1, ' '
        sb t1, 0(t0)
        srli t1, s5, 4                      /* found bits, two hex digits */
        addi t1, t1, '0'
        sb t1, 0(t0)
        andi t1, s5, 15
        li t2, 10
        blt t1, t2, 2f
        addi t1, t1, 'a' - '0' - 10
2:      addi t1, t1, '0'
        sb t1, 0(t0)
1:      li t1, '\n'
        sb t1, 0(t0)
done:   wfi
        j done

/* a0 = 0 if the strings at a0 and a1 are equal */
strcmp:
1:      lbu t0, 0(a0)
        lbu t1, 0(a1)
        bne t0, t1, 2f
        beqz t0, 3f
        addi a0, a0, 1
        addi a1, a1, 1
        j 1b
2:      li a0, 1
        ret
3:      li a0, 0
        ret

/* a0 = 1 if the string at a0 starts with the one at a1 */
strprefix:
1:      lbu t1, 0(a1)
        beqz t1, 2f
        lbu t0, 0(a0)
        bne t0, t1, 3f
        addi a0, a0, 1
        addi a1, a1, 1
        j 1b
2:      li a0, 1
        ret
3:      li a0, 0
        ret

puts:
        li t0, UART_BASE
1:      lbu t1, 0(a0)
        beqz t1, 2f
        sb t1, 0(t0)
        addi a0, a0, 1
        j 1b
2:      ret

        .data
msg_entry:      .asciz "OS-ENTRY riscv dt\n"
msg_ok:         .asciz "dt=ok\n"
msg_bad:        .asciz "dt=bad "
str_rm:         .asciz "reserved-memory"
str_memory:     .asciz "memory@"
str_addr:       .asciz "#address-cells"
str_size:       .asciz "#size-cells"
str_rng:        .asciz "ranges"
str_reg:        .asciz "reg"
str_nomap:      .asciz "no-map"
        .align 4
        .skip 1024
stack_top:
