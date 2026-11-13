/* SquirrelBIOS — MIPS32 firmware for QEMU malta (roadmap E3).
 * Original code, written from the MIPS32 architecture manuals and the
 * Malta board's documented memory map.
 *
 * Big-endian, from the boot flash at 0xbfc00000 (the reset vector).
 * Everything the firmware touches goes through kseg1 (uncached), and
 * kseg0 is made uncached too (Config.K0 = 2) before the hand-off, so
 * nothing depends on caches it never set up. Prints a banner on the
 * PIIX4's COM1 (port 0x3f8 in PCI I/O space, at 0x10000000 as the
 * GT-64120 leaves it after reset), measures RAM, looks for a kernel
 * stored in the flash after the firmware (mips/mkimage.py puts it
 * there; QEMU's -kernel on Malta bypasses -bios, so it cannot be used),
 * counts down an autoboot that any key stops, and gives a small prompt:
 *
 *   help    the commands
 *   info    RAM, timer, kernel, command line
 *   boot    jump to the kernel
 *
 * The kernel is copied to its load address and entered the way a YAMON
 * monitor starts one: a0 = argc, a1 = argv, a2 = envp ("memsize" and
 * its value in bytes), a3 = RAM bytes; argv is the firmware's name and,
 * when there is one, the command line.
 *
 * Flash payload (at 0xbfd00000, 1 MB into the boot flash): "SQKN", the
 * kernel's size, load address and entry point (big-endian words), the
 * command line (256 bytes, NUL-terminated), then at +0x200 the kernel.
 *
 * Build:  make mips
 * Run:    qemu-system-mips -M malta -nographic -bios mips/squirrel-bios.bin
 */
        .equ UART,          0xb00003f8      /* COM1 in PCI I/O space */
        .equ UART_THR,      0
        .equ UART_RBR,      0
        .equ UART_LCR,      3
        .equ UART_LSR,      5
        .equ LSR_DR,        0x01
        .equ LSR_THRE,      0x20

        .equ PAYLOAD,       0xbfd00000
        .equ PL_MAGIC,      0x53514b4e      /* "SQKN" */
        .equ PL_SIZE,       4
        .equ PL_LOAD,       8
        .equ PL_ENTRY,      12
        .equ PL_CMDLINE,    16
        .equ PL_KERNEL,     0x200
        .equ PL_MAX,        0x300000 - PL_KERNEL

        .equ KSEG1,         0xa0000000
        .equ LINE,          0xa0080000      /* the prompt's line */
        .equ LINE_MAX,      120
        .equ ARGS,          0xa0081000      /* argv, envp and their strings */
        .equ STACK_TOP,     0xa00f0000
        .equ RAM_MAX,       0x10000000      /* the 256 MB below the I/O */
        .equ COUNT_HZ,      160000000       /* QEMU's Malta: half of 320 MHz */
        .equ AUTOBOOT_S,    2

/* s0 RAM bytes, s1 kernel bytes (0: none), s2 command line or 0,
 * s3 load address, s4 entry point, s5 a key already read or -1 */

        .text
        .globl _start
_start:
        mtc0    $zero, $11              /* Compare: no timer interrupt */
        li      $t0, 0x10400000         /* CU0, BEV; ERL, EXL and IE off */
        mtc0    $t0, $12
        li      $sp, STACK_TOP
        li      $s5, -1
        li      $t0, UART
        li      $t1, 0x03               /* 8N1 */
        sb      $t1, UART_LCR($t0)

        la      $a0, s_banner
        jal     puts

        /* RAM: a marker every MB until one does not stick or shows up
         * again at 0 (the address wrapped) */
        li      $t0, KSEG1
        li      $t1, 0x51ea1e55
        sw      $t1, 0($t0)
        li      $s0, 0x100000
1:      li      $t2, RAM_MAX
        bgeu    $s0, $t2, 2f
        or      $t3, $s0, $t0
        xori    $t4, $s0, 0x5a5a
        sw      $t4, 0($t3)
        lw      $t5, 0($t3)
        bne     $t5, $t4, 2f
        lw      $t5, 0($t0)
        bne     $t5, $t1, 2f
        addu    $s0, $s0, 0x100000
        b       1b
2:
        /* The kernel in the flash, if there is one that fits */
        move    $s1, $zero
        move    $s2, $zero
        li      $t0, PAYLOAD
        lw      $t1, 0($t0)
        li      $t2, PL_MAGIC
        bne     $t1, $t2, no_kernel
        lw      $t1, PL_SIZE($t0)
        lw      $s3, PL_LOAD($t0)
        lw      $s4, PL_ENTRY($t0)
        beqz    $t1, no_kernel
        li      $t2, PL_MAX
        bgtu    $t1, $t2, too_big
        /* load..load+size inside kseg0's RAM */
        li      $t2, 0x80000000
        bltu    $s3, $t2, bad_load
        subu    $t3, $s3, $t2
        addu    $t3, $t3, $t1
        bgtu    $t3, $s0, bad_load
        move    $s1, $t1
        /* copy through kseg1 */
        addiu   $a1, $t0, PL_KERNEL
        li      $t2, 0x1fffffff
        and     $a0, $s3, $t2
        li      $t2, KSEG1
        or      $a0, $a0, $t2
        move    $a2, $s1
3:      lbu     $t3, 0($a1)
        sb      $t3, 0($a0)
        addiu   $a1, $a1, 1
        addiu   $a0, $a0, 1
        addiu   $a2, $a2, -1
        bnez    $a2, 3b
        /* the command line, if not empty */
        lbu     $t3, PL_CMDLINE($t0)
        beqz    $t3, report
        addiu   $s2, $t0, PL_CMDLINE
        b       report
too_big:
        la      $a0, s_toobig
        jal     puts
        b       report
bad_load:
        la      $a0, s_badload
        jal     puts
        b       report
no_kernel:
        la      $a0, s_nokernel
        jal     puts

report:
        jal     info

        /* Autoboot: AUTOBOOT_S seconds, any key stops it */
        beqz    $s1, prompt
        la      $a0, s_autoboot
        jal     puts
        mfc0    $s6, $9
        li      $s7, COUNT_HZ * AUTOBOOT_S
1:      jal     getc
        bgez    $v0, 2f
        mfc0    $t0, $9
        subu    $t0, $t0, $s6
        bltu    $t0, $s7, 1b
        la      $a0, s_nl
        jal     puts
        b       do_boot
2:      move    $s5, $v0                /* the key starts the first line */
        la      $a0, s_stopped
        jal     puts

prompt:
        la      $a0, s_prompt
        jal     puts
        jal     readline
        li      $s6, LINE
        lbu     $t0, 0($s6)
        beqz    $t0, prompt
        move    $a0, $s6
        la      $a1, c_help
        jal     streq
        bnez    $v0, cmd_help
        move    $a0, $s6
        la      $a1, c_info
        jal     streq
        bnez    $v0, cmd_info
        move    $a0, $s6
        la      $a1, c_boot
        jal     streq
        bnez    $v0, cmd_boot
        la      $a0, s_unknown1
        jal     puts
        move    $a0, $s6
        jal     puts
        la      $a0, s_unknown2
        jal     puts
        b       prompt
cmd_help:
        la      $a0, s_help
        jal     puts
        b       prompt
cmd_info:
        jal     info
        b       prompt
cmd_boot:
        bnez    $s1, do_boot
        la      $a0, s_noboot
        jal     puts
        b       prompt

/* argv = { "squirrelbios", command line } in ARGS, envp = { "memsize",
 * "<bytes>", NULL }; all passed as kseg0 addresses */
do_boot:
        la      $a0, s_jump
        jal     puts
        move    $a0, $s4
        jal     puthex
        la      $a0, s_nl
        jal     puts
        li      $s6, ARGS
        /* strings from ARGS + 64 */
        addiu   $a0, $s6, 64
        la      $a1, s_name
        jal     strcpy
        addiu   $t0, $s6, 64
        sw      $t0, 0($s6)             /* argv[0] (kseg1 for now) */
        li      $s7, 1                  /* argc */
        beqz    $s2, 1f
        addiu   $a0, $s6, 128
        move    $a1, $s2
        jal     strcpy
        addiu   $t0, $s6, 128
        sw      $t0, 4($s6)
        li      $s7, 2
1:      sll     $t0, $s7, 2
        addu    $t0, $t0, $s6
        sw      $zero, 0($t0)           /* argv[argc] */
        /* envp at ARGS + 16 */
        addiu   $a0, $s6, 512
        la      $a1, s_memsize
        jal     strcpy
        addiu   $t0, $s6, 512
        sw      $t0, 16($s6)
        addiu   $a0, $s6, 528
        move    $a1, $s0
        jal     utoa
        addiu   $t0, $s6, 528
        sw      $t0, 20($s6)
        sw      $zero, 24($s6)
        /* kseg1 -> kseg0 for the pointers */
        li      $t1, 0x20000000
        lw      $t0, 0($s6)
        subu    $t0, $t0, $t1
        sw      $t0, 0($s6)
        beqz    $s2, 2f
        lw      $t0, 4($s6)
        subu    $t0, $t0, $t1
        sw      $t0, 4($s6)
2:      lw      $t0, 16($s6)
        subu    $t0, $t0, $t1
        sw      $t0, 16($s6)
        lw      $t0, 20($s6)
        subu    $t0, $t0, $t1
        sw      $t0, 20($s6)
        /* kseg0 uncached: Config.K0 = 2 */
        mfc0    $t0, $16
        li      $t1, ~7
        and     $t0, $t0, $t1
        ori     $t0, $t0, 2
        mtc0    $t0, $16
        ehb
        move    $a0, $s7
        li      $t1, 0x20000000
        subu    $a1, $s6, $t1
        addiu   $a2, $a1, 16
        move    $a3, $s0
        jr      $s4

/* ------------------------------------------------------------ reports */

info:
        addiu   $sp, $sp, -8
        sw      $ra, 0($sp)
        la      $a0, s_mem
        jal     puts
        srl     $a0, $s0, 20
        jal     putdec
        la      $a0, s_mb
        jal     puts
        la      $a0, s_timer
        jal     puts
        li      $a0, COUNT_HZ
        jal     putdec
        la      $a0, s_hz
        jal     puts
        beqz    $s1, 1f
        la      $a0, s_kernel1
        jal     puts
        move    $a0, $s1
        jal     putdec
        la      $a0, s_kernel2
        jal     puts
        move    $a0, $s3
        jal     puthex
        la      $a0, s_kernel3
        jal     puts
        move    $a0, $s4
        jal     puthex
        la      $a0, s_nl
        jal     puts
1:      beqz    $s2, 2f
        la      $a0, s_cmd1
        jal     puts
        move    $a0, $s2
        jal     puts
        la      $a0, s_cmd2
        jal     puts
2:      lw      $ra, 0($sp)
        addiu   $sp, $sp, 8
        jr      $ra

/* ------------------------------------------------------------- the UART */

/* putc(a0) */
putc:
        li      $t8, UART
1:      lbu     $t9, UART_LSR($t8)
        andi    $t9, $t9, LSR_THRE
        beqz    $t9, 1b
        sb      $a0, UART_THR($t8)
        jr      $ra

/* puts(a0): "\n" goes out as CR LF */
puts:
        addiu   $sp, $sp, -8
        sw      $ra, 0($sp)
        sw      $s7, 4($sp)
        move    $s7, $a0
1:      lbu     $a0, 0($s7)
        beqz    $a0, 3f
        li      $t1, 10
        bne     $a0, $t1, 2f
        li      $a0, 13
        jal     putc
        li      $a0, 10
2:      jal     putc
        addiu   $s7, $s7, 1
        b       1b
3:      lw      $s7, 4($sp)
        lw      $ra, 0($sp)
        addiu   $sp, $sp, 8
        jr      $ra

/* getc: v0 = the next received byte, or -1 */
getc:
        li      $t8, UART
        lbu     $t9, UART_LSR($t8)
        andi    $t9, $t9, LSR_DR
        li      $v0, -1
        beqz    $t9, 1f
        lbu     $v0, UART_RBR($t8)
1:      jr      $ra

/* utoa(a0 buffer, a1 value): unsigned decimal, NUL-terminated */
utoa:
        addiu   $sp, $sp, -16
        move    $t0, $sp                /* digits backwards on the stack */
        li      $t1, 10
1:      divu    $zero, $a1, $t1
        mfhi    $t2
        mflo    $a1
        addiu   $t2, $t2, '0'
        sb      $t2, 0($t0)
        addiu   $t0, $t0, 1
        bnez    $a1, 1b
2:      addiu   $t0, $t0, -1
        lbu     $t2, 0($t0)
        sb      $t2, 0($a0)
        addiu   $a0, $a0, 1
        bne     $t0, $sp, 2b
        sb      $zero, 0($a0)
        addiu   $sp, $sp, 16
        jr      $ra

/* putdec(a0) */
putdec:
        addiu   $sp, $sp, -24
        sw      $ra, 20($sp)
        move    $a1, $a0
        move    $a0, $sp
        jal     utoa
        move    $a0, $sp
        jal     puts
        lw      $ra, 20($sp)
        addiu   $sp, $sp, 24
        jr      $ra

/* puthex(a0): 0x and 8 hex digits */
puthex:
        addiu   $sp, $sp, -24
        sw      $ra, 20($sp)
        li      $t0, '0'
        sb      $t0, 0($sp)
        li      $t0, 'x'
        sb      $t0, 1($sp)
        addiu   $t1, $sp, 2
        li      $t2, 8
1:      srl     $t3, $a0, 28
        sll     $a0, $a0, 4
        sltiu   $t4, $t3, 10
        addiu   $t3, $t3, '0'
        bnez    $t4, 2f
        addiu   $t3, $t3, 'a' - '0' - 10
2:      sb      $t3, 0($t1)
        addiu   $t1, $t1, 1
        addiu   $t2, $t2, -1
        bnez    $t2, 1b
        sb      $zero, 0($t1)
        move    $a0, $sp
        jal     puts
        lw      $ra, 20($sp)
        addiu   $sp, $sp, 24
        jr      $ra

/* readline: a line into LINE, echoed, with backspace; leading blanks
 * skipped, trailing ones dropped, ended by CR or LF. A key in s5 comes
 * first. */
readline:
        addiu   $sp, $sp, -8
        sw      $ra, 0($sp)
        sw      $s7, 4($sp)
        li      $s7, LINE
1:      move    $v0, $s5
        li      $s5, -1
        bgez    $v0, 2f
        jal     getc
        bltz    $v0, 1b
2:      li      $t0, 13
        beq     $v0, $t0, 5f
        li      $t0, 10
        beq     $v0, $t0, 5f
        li      $t0, 8
        beq     $v0, $t0, 4f
        li      $t0, 127
        beq     $v0, $t0, 4f
        li      $t0, 32
        blt     $v0, $t0, 1b            /* other controls */
        li      $t1, LINE
        bne     $s7, $t1, 3f
        beq     $v0, $t0, 1b            /* a leading blank */
3:      li      $t1, LINE + LINE_MAX
        bgeu    $s7, $t1, 1b
        sb      $v0, 0($s7)
        addiu   $s7, $s7, 1
        move    $a0, $v0
        jal     putc
        b       1b
4:      li      $t1, LINE
        beq     $s7, $t1, 1b
        addiu   $s7, $s7, -1
        la      $a0, s_rubout
        jal     puts
        b       1b
5:      li      $t1, LINE
6:      beq     $s7, $t1, 7f
        lbu     $t0, -1($s7)
        li      $t2, 32
        bne     $t0, $t2, 7f
        addiu   $s7, $s7, -1
        b       6b
7:      sb      $zero, 0($s7)
        la      $a0, s_nl
        jal     puts
        lw      $s7, 4($sp)
        lw      $ra, 0($sp)
        addiu   $sp, $sp, 8
        jr      $ra

/* streq(a0, a1): v0 = 1 when the two strings are equal */
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

/* strcpy(a0 dst, a1 src) */
strcpy:
1:      lbu     $t0, 0($a1)
        sb      $t0, 0($a0)
        addiu   $a0, $a0, 1
        addiu   $a1, $a1, 1
        bnez    $t0, 1b
        jr      $ra

/* --------------------------------------------------------------- text */

s_banner:   .asciz "\nSquirrelBIOS MIPS (malta)\n"
s_mem:      .asciz "mem: "
s_mb:       .asciz " MB\n"
s_timer:    .asciz "timer: "
s_hz:       .asciz " Hz\n"
s_kernel1:  .asciz "boot: kernel of "
s_kernel2:  .asciz " bytes at "
s_kernel3:  .asciz ", entry "
s_cmd1:     .asciz "boot: command line \""
s_cmd2:     .asciz "\"\n"
s_nokernel: .asciz "boot: no kernel in the flash\n"
s_toobig:   .asciz "boot: the kernel in the flash is larger than the flash\n"
s_badload:  .asciz "boot: the kernel's load address is not in RAM (kseg0)\n"
s_autoboot: .asciz "autoboot in 2 s, press a key for the prompt"
s_stopped:  .asciz "\nautoboot stopped\n"
s_prompt:   .asciz "sq> "
s_rubout:   .asciz "\b \b"
s_help:     .asciz "commands: help, info, boot\n"
s_unknown1: .asciz "unknown command: "
s_unknown2: .asciz " (try help)\n"
s_noboot:   .asciz "boot: no kernel loaded\n"
s_jump:     .asciz "boot: jumping to "
s_nl:       .asciz "\n"
s_name:     .asciz "squirrelbios"
s_memsize:  .asciz "memsize"
c_help:     .asciz "help"
c_info:     .asciz "info"
c_boot:     .asciz "boot"
