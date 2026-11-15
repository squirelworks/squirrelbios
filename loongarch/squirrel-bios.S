/* SquirrelBIOS — LoongArch64 firmware for QEMU virt (roadmap E3).
 * Original code, written from the LoongArch reference manual (volume 1)
 * and QEMU's fw_cfg interface description.
 *
 * Runs from flash at 0x1c000000 in direct address mode (no paging, all
 * accesses strongly ordered and uncached). Prints a banner on the
 * NS16550 at 0x1fe001e0, reads the RAM size, the -kernel image and the
 * -append line through fw_cfg (MMIO at 0x1e020000), counts down an
 * autoboot that any key stops, and gives a small prompt:
 *
 *   help    the commands
 *   info    RAM, timer, kernel, command line
 *   boot    jump to the kernel
 *
 * The kernel is copied as raw bytes to 0x00200000 and entered there with
 * a0 = RAM bytes, a1 = the command line (NUL-terminated) or 0, a2 = 0.
 *
 * Build:  make loongarch
 * Run:    qemu-system-loongarch64 -M virt -nographic -bios loongarch/squirrel-bios.bin -kernel image.bin
 */

        .equ UART,          0x1fe001e0
        .equ UART_THR,      0
        .equ UART_RBR,      0
        .equ UART_LCR,      3
        .equ UART_LSR,      5
        .equ LSR_DR,        0x01
        .equ LSR_THRE,      0x20

        .equ FWCFG,         0x1e020000
        .equ FWCFG_DATA,    0
        .equ FWCFG_SEL,     8
        .equ FW_SIGNATURE,  0x00
        .equ FW_RAM_SIZE,   0x03
        .equ FW_KERNEL_SIZE, 0x08
        .equ FW_CMDLINE_SIZE, 0x14
        .equ FW_KERNEL_DATA, 0x11
        .equ FW_CMDLINE_DATA, 0x15

        .equ STACK_TOP,     0x00100000
        .equ CMDLINE,       0x00100000      /* 4 KB */
        .equ CMDLINE_MAX,   4096
        .equ LINE,          0x00101000      /* the prompt's line */
        .equ LINE_MAX,      120
        .equ KERNEL_LOAD,   0x00200000
        .equ LOW_RAM_END,   0x10000000      /* the first 256 MB at 0 */
        .equ AUTOBOOT_S,    2

/* s0 UART, s1 fw_cfg, s2 RAM bytes, s3 kernel bytes (0: none),
 * s4 command line or 0, s5 timer Hz, s6 a key already read or -1 */

        .text
        .globl _start
_start:
        li.d    $sp, STACK_TOP
        li.d    $s0, UART
        li.d    $s1, FWCFG
        li.w    $s6, -1
        li.w    $t0, 0x03               /* 8N1 */
        st.b    $t0, $s0, UART_LCR

        la.pcrel $a0, s_banner
        bl      puts

        /* The timer: CPUCFG word 4 is the base clock, word 5 its
         * multiplier (low half) and divider (high half) */
        li.w    $t0, 4
        cpucfg  $t1, $t0
        li.w    $t0, 5
        cpucfg  $t2, $t0
        bstrpick.d $t3, $t2, 15, 0
        bstrpick.d $t4, $t2, 31, 16
        bnez    $t3, 1f
        li.w    $t3, 1
1:      bnez    $t4, 1f
        li.w    $t4, 1
1:      mul.d   $t1, $t1, $t3
        div.du  $s5, $t1, $t4
        bnez    $s5, 1f
        li.d    $s5, 100000000
1:
        /* fw_cfg: "QEMU" at key 0, or nothing to load from */
        li.w    $a0, FW_SIGNATURE
        bl      fw_u32
        li.w    $t0, 0x554d4551         /* "QEMU" */
        beq     $a0, $t0, have_fwcfg
        la.pcrel $a0, s_nofwcfg
        bl      puts
        li.d    $s2, LOW_RAM_END
        move    $s3, $zero
        move    $s4, $zero
        b       report
have_fwcfg:
        li.w    $a0, FW_RAM_SIZE
        bl      fw_u64
        move    $s2, $a0
        bnez    $s2, 1f
        li.d    $s2, LOW_RAM_END
1:
        /* The kernel, if it fits in low RAM above the load address */
        move    $s3, $zero
        li.w    $a0, FW_KERNEL_SIZE
        bl      fw_u32
        beqz    $a0, no_kernel
        li.d    $t0, LOW_RAM_END - KERNEL_LOAD
        bltu    $t0, $a0, too_big
        move    $s3, $a0
        li.w    $a0, FW_KERNEL_DATA
        bl      fw_select
        li.d    $a0, KERNEL_LOAD
        move    $a1, $s3
        bl      fw_read
        b       cmdline
too_big:
        move    $s7, $a0
        la.pcrel $a0, s_toobig1
        bl      puts
        move    $a0, $s7
        bl      putdec
        la.pcrel $a0, s_toobig2
        bl      puts
        b       cmdline
no_kernel:
        la.pcrel $a0, s_nokernel
        bl      puts

cmdline:
        move    $s4, $zero
        li.w    $a0, FW_CMDLINE_SIZE
        bl      fw_u32
        beqz    $a0, report
        li.w    $t0, CMDLINE_MAX
        bgeu    $a0, $t0, report
        move    $s7, $a0
        li.w    $a0, FW_CMDLINE_DATA
        bl      fw_select
        li.d    $a0, CMDLINE
        move    $a1, $s7
        bl      fw_read
        li.d    $s4, CMDLINE
        add.d   $t0, $s4, $s7
        st.b    $zero, $t0, 0           /* QEMU's includes the NUL; make sure */
        ld.bu   $t0, $s4, 0
        bnez    $t0, report
        move    $s4, $zero              /* an empty -append */

report:
        bl      info

        /* Autoboot: AUTOBOOT_S seconds, any key stops it */
        beqz    $s3, prompt
        la.pcrel $a0, s_autoboot
        bl      puts
        rdtime.d $s7, $zero
        li.w    $t0, AUTOBOOT_S
        mul.d   $s8, $s5, $t0
1:      bl      getc
        bge     $a0, $zero, 2f
        rdtime.d $t0, $zero
        sub.d   $t0, $t0, $s7
        bltu    $t0, $s8, 1b
        la.pcrel $a0, s_nl
        bl      puts
        b       do_boot
2:      move    $s6, $a0                /* the key starts the first line */
        la.pcrel $a0, s_stopped
        bl      puts

prompt:
        la.pcrel $a0, s_prompt
        bl      puts
        bl      readline
        li.d    $s7, LINE
        ld.bu   $t0, $s7, 0
        beqz    $t0, prompt
        move    $a0, $s7
        la.pcrel $a1, c_help
        bl      streq
        bnez    $a0, cmd_help
        move    $a0, $s7
        la.pcrel $a1, c_info
        bl      streq
        bnez    $a0, cmd_info
        move    $a0, $s7
        la.pcrel $a1, c_boot
        bl      streq
        bnez    $a0, cmd_boot
        la.pcrel $a0, s_unknown1
        bl      puts
        move    $a0, $s7
        bl      puts
        la.pcrel $a0, s_unknown2
        bl      puts
        b       prompt
cmd_help:
        la.pcrel $a0, s_help
        bl      puts
        b       prompt
cmd_info:
        bl      info
        b       prompt
cmd_boot:
        bnez    $s3, do_boot
        la.pcrel $a0, s_noboot
        bl      puts
        b       prompt

do_boot:
        la.pcrel $a0, s_jump
        bl      puts
        li.d    $a0, KERNEL_LOAD
        bl      puthex
        la.pcrel $a0, s_nl
        bl      puts
        ibar    0
        move    $a0, $s2
        move    $a1, $s4
        move    $a2, $zero
        li.d    $t0, KERNEL_LOAD
        jirl    $zero, $t0, 0

/* ------------------------------------------------------------ reports */

info:
        addi.d  $sp, $sp, -16
        st.d    $ra, $sp, 0
        la.pcrel $a0, s_mem
        bl      puts
        srli.d  $a0, $s2, 20
        bl      putdec
        la.pcrel $a0, s_mb
        bl      puts
        la.pcrel $a0, s_timer
        bl      puts
        move    $a0, $s5
        bl      putdec
        la.pcrel $a0, s_hz
        bl      puts
        beqz    $s3, 1f
        la.pcrel $a0, s_kernel1
        bl      puts
        move    $a0, $s3
        bl      putdec
        la.pcrel $a0, s_kernel2
        bl      puts
        li.d    $a0, KERNEL_LOAD
        bl      puthex
        la.pcrel $a0, s_nl
        bl      puts
1:      beqz    $s4, 2f
        la.pcrel $a0, s_cmd1
        bl      puts
        move    $a0, $s4
        bl      puts
        la.pcrel $a0, s_cmd2
        bl      puts
2:      ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 16
        jr      $ra

/* ------------------------------------------------------------- the UART */

/* putc(a0) */
putc:
1:      ld.bu   $t0, $s0, UART_LSR
        andi    $t0, $t0, LSR_THRE
        beqz    $t0, 1b
        st.b    $a0, $s0, UART_THR
        jr      $ra

/* puts(a0): "\n" goes out as CR LF */
puts:
        addi.d  $sp, $sp, -16
        st.d    $ra, $sp, 0
        st.d    $s7, $sp, 8
        move    $s7, $a0
1:      ld.bu   $a0, $s7, 0
        beqz    $a0, 3f
        li.w    $t1, 10
        bne     $a0, $t1, 2f
        li.w    $a0, 13
        bl      putc
        li.w    $a0, 10
2:      bl      putc
        addi.d  $s7, $s7, 1
        b       1b
3:      ld.d    $s7, $sp, 8
        ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 16
        jr      $ra

/* getc: a0 = the next received byte, or -1 */
getc:
        ld.bu   $t0, $s0, UART_LSR
        andi    $t0, $t0, LSR_DR
        li.w    $a0, -1
        beqz    $t0, 1f
        ld.bu   $a0, $s0, UART_RBR
1:      jr      $ra

/* putdec(a0): unsigned decimal */
putdec:
        addi.d  $sp, $sp, -48
        st.d    $ra, $sp, 0
        st.d    $s7, $sp, 8
        addi.d  $s7, $sp, 40
        st.b    $zero, $s7, 0
        li.w    $t1, 10
1:      mod.du  $t2, $a0, $t1
        div.du  $a0, $a0, $t1
        addi.d  $t2, $t2, '0'
        addi.d  $s7, $s7, -1
        st.b    $t2, $s7, 0
        bnez    $a0, 1b
        move    $a0, $s7
        bl      puts
        ld.d    $s7, $sp, 8
        ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 48
        jr      $ra

/* puthex(a0): 0x and 8 hex digits (16 when it needs them) */
puthex:
        addi.d  $sp, $sp, -48
        st.d    $ra, $sp, 0
        st.d    $s7, $sp, 8
        li.w    $t3, 8
        srli.d  $t0, $a0, 32
        beqz    $t0, 1f
        li.w    $t3, 16
1:      addi.d  $s7, $sp, 40
        st.b    $zero, $s7, 0
2:      andi    $t2, $a0, 15
        srli.d  $a0, $a0, 4
        li.w    $t1, 10
        blt     $t2, $t1, 3f
        addi.d  $t2, $t2, 'a' - '0' - 10
3:      addi.d  $t2, $t2, '0'
        addi.d  $s7, $s7, -1
        st.b    $t2, $s7, 0
        addi.d  $t3, $t3, -1
        bnez    $t3, 2b
        li.w    $t2, 'x'
        st.b    $t2, $s7, -1
        li.w    $t2, '0'
        st.b    $t2, $s7, -2
        addi.d  $a0, $s7, -2
        bl      puts
        ld.d    $s7, $sp, 8
        ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 48
        jr      $ra

/* readline: a line into LINE, echoed, with backspace; leading blanks
 * skipped, ended by CR or LF. A key in s6 comes first. */
readline:
        addi.d  $sp, $sp, -16
        st.d    $ra, $sp, 0
        st.d    $s7, $sp, 8
        li.d    $s7, LINE
1:      move    $a0, $s6
        li.w    $s6, -1
        bge     $a0, $zero, 2f
        bl      getc
        blt     $a0, $zero, 1b
2:      li.w    $t0, 13
        beq     $a0, $t0, 5f
        li.w    $t0, 10
        beq     $a0, $t0, 5f
        li.w    $t0, 8
        beq     $a0, $t0, 4f
        li.w    $t0, 127
        beq     $a0, $t0, 4f
        li.w    $t0, 32
        blt     $a0, $t0, 1b            /* other controls */
        li.d    $t1, LINE
        bne     $s7, $t1, 3f
        beq     $a0, $t0, 1b            /* a leading blank */
3:      li.d    $t1, LINE + LINE_MAX
        bgeu    $s7, $t1, 1b
        st.b    $a0, $s7, 0
        addi.d  $s7, $s7, 1
        bl      putc
        b       1b
4:      li.d    $t1, LINE
        beq     $s7, $t1, 1b
        addi.d  $s7, $s7, -1
        la.pcrel $a0, s_rubout
        bl      puts
        b       1b
5:      /* trailing blanks off */
        li.d    $t1, LINE
6:      beq     $s7, $t1, 7f
        ld.bu   $t0, $s7, -1
        li.w    $t2, 32
        bne     $t0, $t2, 7f
        addi.d  $s7, $s7, -1
        b       6b
7:      st.b    $zero, $s7, 0
        la.pcrel $a0, s_nl
        bl      puts
        ld.d    $s7, $sp, 8
        ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 16
        jr      $ra

/* streq(a0, a1): 1 when the two strings are equal */
streq:
1:      ld.bu   $t0, $a0, 0
        ld.bu   $t1, $a1, 0
        bne     $t0, $t1, 2f
        addi.d  $a0, $a0, 1
        addi.d  $a1, $a1, 1
        bnez    $t0, 1b
        li.w    $a0, 1
        jr      $ra
2:      move    $a0, $zero
        jr      $ra

/* -------------------------------------------------------------- fw_cfg */

/* fw_select(a0 key): the selector is a big-endian 16-bit register */
fw_select:
        andi    $t0, $a0, 0xff
        slli.d  $t0, $t0, 8
        srli.d  $t1, $a0, 8
        andi    $t1, $t1, 0xff
        or      $t0, $t0, $t1
        st.h    $t0, $s1, FWCFG_SEL
        jr      $ra

/* fw_read(a0 dst, a1 bytes): from the selected item, a byte at a time */
fw_read:
        beqz    $a1, 2f
1:      ld.bu   $t0, $s1, FWCFG_DATA
        st.b    $t0, $a0, 0
        addi.d  $a0, $a0, 1
        addi.d  $a1, $a1, -1
        bnez    $a1, 1b
2:      jr      $ra

/* fw_u32(a0 key), fw_u64(a0 key): a little-endian item */
fw_u32:
        li.w    $t3, 4
        b       fw_le
fw_u64:
        li.w    $t3, 8
fw_le:
        addi.d  $sp, $sp, -16
        st.d    $ra, $sp, 0
        bl      fw_select
        move    $a0, $zero
        move    $t2, $zero
1:      ld.bu   $t0, $s1, FWCFG_DATA
        sll.d   $t0, $t0, $t2
        or      $a0, $a0, $t0
        addi.d  $t2, $t2, 8
        addi.d  $t3, $t3, -1
        bnez    $t3, 1b
        ld.d    $ra, $sp, 0
        addi.d  $sp, $sp, 16
        jr      $ra

/* --------------------------------------------------------------- text */

s_banner:   .asciz "\nSquirrelBIOS LoongArch64 (virt)\n"
s_nofwcfg:  .asciz "fw_cfg: not found, no kernel to load\n"
s_mem:      .asciz "mem: "
s_mb:       .asciz " MB\n"
s_timer:    .asciz "timer: "
s_hz:       .asciz " Hz\n"
s_kernel1:  .asciz "boot: kernel of "
s_kernel2:  .asciz " bytes at "
s_cmd1:     .asciz "boot: command line \""
s_cmd2:     .asciz "\"\n"
s_nokernel: .asciz "boot: no -kernel image\n"
s_toobig1:  .asciz "boot: a kernel of "
s_toobig2:  .asciz " bytes does not fit in low RAM\n"
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
c_help:     .asciz "help"
c_info:     .asciz "info"
c_boot:     .asciz "boot"
