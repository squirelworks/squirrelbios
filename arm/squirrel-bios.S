/* SquirrelBIOS — AArch64 firmware for QEMU virt
 * Prints banner on PL011, loads the -kernel image through fw_cfg and
 * jumps to it with the device tree in x0.
 * Original code.
 *
 * Build (from repo root):
 *   make arm
 * Run:
 *   qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios arm/squirrel-bios.bin -kernel image.bin
 */

        .equ UART_BASE,     0x09000000
        .equ UART_DR,       0x00
        .equ UART_FR,       0x18
        .equ UART_CR,       0x30
        .equ UART_CR_ON,    0x301           /* UARTEN | TXE | RXE */
        .equ UART_TIMEOUT,  0x100000        /* FR polls per character */

        .equ FWCFG_BASE,    0x09020000
        .equ FWCFG_DATA,    0x00
        .equ FWCFG_SEL,     0x08
        .equ FWCFG_DMA,     0x10
        .equ FWCFG_SIGNATURE,   0x00
        .equ FWCFG_ID,          0x01
        .equ FWCFG_RAM_SIZE,    0x03        /* u64, bytes of RAM at RAM_BASE */
        .equ FWCFG_KERNEL_SIZE, 0x08
        .equ FWCFG_KERNEL_DATA, 0x11
        .equ FWCFG_ID_DMA,      (1 << 1)
        .equ FWCFG_DMA_ERROR,   0x01
        .equ FWCFG_DMA_READ,    0x02
        .equ FWCFG_DMA_SELECT,  0x08

        .equ RAM_BASE,      0x40000000      /* QEMU puts the DTB here */
        .equ FW_STACK_TOP,  0x40200000      /* firmware scratch: 1 MB below the image */
        .equ KERNEL_LOAD,   0x40200000      /* RAM + 2 MB, Linux arm64 friendly */
        .equ FDT_MAGIC,     0xd00dfeed

        /* SCTLR_ELx bits cleared before the jump: MMU, D-cache, and both
         * endianness bits (EE for this EL, E0E for EL0; E0E is RES0 at
         * EL2 without E2H and at EL3, where clearing it changes nothing) */
        .equ SCTLR_M,       (1 << 0)
        .equ SCTLR_C,       (1 << 2)
        .equ SCTLR_E0E,     (1 << 24)
        .equ SCTLR_EE,      (1 << 25)
        .equ SCTLR_CLEAR,   (SCTLR_M | SCTLR_C | SCTLR_E0E | SCTLR_EE)

.global _start
_start:
        msr daifset, #0xf
        ldr x0, =FW_STACK_TOP
        mov sp, x0

        bl uart_init
        adr x0, banner
        bl puts

        /* fw_cfg present and DMA capable? */
        ldr x19, =FWCFG_BASE
        mov w0, #FWCFG_SIGNATURE
        bl fwcfg_select
        ldr w0, [x19, #FWCFG_DATA]          /* data port is a byte stream */
        ldr w1, =0x554d4551                 /* "QEMU" read as little-endian */
        cmp w0, w1
        b.ne no_fwcfg
        mov w0, #FWCFG_ID
        bl fwcfg_select
        ldrb w0, [x19, #FWCFG_DATA]
        tbz w0, #1, no_fwcfg

        /* Kernel size, little-endian u32 */
        mov w0, #FWCFG_KERNEL_SIZE
        bl fwcfg_select
        ldr w20, [x19, #FWCFG_DATA]
        cbz w20, no_kernel

        /* x22 = DTB if QEMU left one at the start of RAM, otherwise 0 */
        ldr x1, =RAM_BASE
        ldr w2, [x1]
        rev w2, w2
        ldr w3, =FDT_MAGIC
        mov x22, #0
        cmp w2, w3
        csel x22, x1, x22, eq

        /* RAM size: fw_cfg key 0x03 (little-endian u64) when QEMU fills it
         * in, otherwise the device tree's /memory range at RAM_BASE.
         * QEMU's virt machine leaves the key at 0 (seen on QEMU 8.2). */
        mov w0, #FWCFG_RAM_SIZE
        bl fwcfg_select
        ldr x21, [x19, #FWCFG_DATA]
        cbnz x21, 1f
        cbz x22, no_ram_size
        mov x0, x22
        bl fdt_ram_size
        mov x21, x0
        cbz x21, no_ram_size
1:

        ldr x0, =KERNEL_LOAD
        mov w1, w20
        bl check_fit
        cbnz w0, too_big

        ldr x0, =KERNEL_LOAD
        mov w1, w20
        mov w2, #FWCFG_KERNEL_DATA
        bl fwcfg_dma_read
        cmp w0, #2
        b.eq load_timeout
        cbnz w0, load_failed

        adr x0, msg_loaded
        bl puts

        bl entry_state

        mov x0, x22                         /* DTB or 0 */

        mov x1, #0
        mov x2, #0
        mov x3, #0
        ldr x4, =KERNEL_LOAD
        dsb sy
        isb
        br x4

no_fwcfg:
        adr x0, msg_no_fwcfg
        b fail
no_kernel:
        adr x0, msg_no_kernel
        b fail
too_big:
        adr x0, msg_too_big
        b fail
no_ram_size:
        adr x0, msg_no_ram_size
        b fail
load_failed:
        adr x0, msg_load_failed
        b fail
load_timeout:
        adr x0, msg_load_timeout
fail:
        bl puts
        adr x0, msg_halted
        bl puts
halt:
        wfi
        b halt

/* x0 = load address, x1 = image size, x21 = RAM size from fw_cfg.
 * Returns w0 = 0 when [x0, x0 + x1) lies inside RAM and clear of the
 * device tree and firmware scratch below KERNEL_LOAD, otherwise 1. */
check_fit:
        ldr x2, =KERNEL_LOAD
        cmp x0, x2
        b.lo 1f                             /* on the DTB or firmware stack */
        adds x2, x0, x1                     /* end of the image */
        b.cs 1f
        ldr x3, =RAM_BASE
        adds x3, x3, x21                    /* end of RAM */
        b.cs 1f
        cmp x2, x3
        b.hi 1f
        mov w0, #0
        ret
1:      mov w0, #1
        ret

/* ------------------------------------------------------------------ */
/* Device tree                                                         */
/* ------------------------------------------------------------------ */

        .equ FDT_BEGIN_NODE, 1
        .equ FDT_END_NODE,   2
        .equ FDT_PROP,       3
        .equ FDT_NOP,        4

/* x0 = flattened device tree.  Returns x0 = length of the range in a
 * /memory node (a child of the root named "memory" or "memory@...")
 * whose base is RAM_BASE, or 0 if there is none. Cell counts come from
 * the root's #address-cells and #size-cells (default 2 and 1); only 1 or
 * 2 cells are understood. Never reads past the blob's totalsize. */
fdt_ram_size:
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        ldr w1, [x0, #8]                    /* off_dt_struct */
        rev w1, w1
        add x1, x0, x1                      /* x1 = cursor */
        ldr w2, [x0, #12]                   /* off_dt_strings */
        rev w2, w2
        add x2, x0, x2                      /* x2 = strings */
        ldr w3, [x0, #4]                    /* totalsize */
        rev w3, w3
        add x3, x0, x3                      /* x3 = end of blob */
        mov x4, #0                          /* depth */
        mov x5, #2                          /* #address-cells */
        mov x6, #1                          /* #size-cells */
        mov x7, #0                          /* 1 while directly in /memory */

.Ltok:  add x8, x1, #4
        cmp x8, x3
        b.hi .Lnone
        ldr w8, [x1], #4
        rev w8, w8
        cmp w8, #FDT_BEGIN_NODE
        b.eq .Lbegin
        cmp w8, #FDT_END_NODE
        b.eq .Lend
        cmp w8, #FDT_PROP
        b.eq .Lprop
        cmp w8, #FDT_NOP
        b.eq .Ltok
        b .Lnone                            /* FDT_END or not a token */

.Lbegin:
        add x4, x4, #1
        mov x7, #0
        cmp x4, #2                          /* a child of the root */
        b.ne .Lname
        adr x13, str_memory
        mov x12, x1
1:      ldrb w14, [x13], #1
        cbz w14, 2f
        ldrb w15, [x12], #1
        cmp w14, w15
        b.ne .Lname
        b 1b
2:      ldrb w15, [x12]
        cbz w15, 3f
        cmp w15, #'@'
        b.ne .Lname
3:      mov x7, #1
.Lname: cmp x1, x3                          /* skip the name, then pad */
        b.hs .Lnone
        ldrb w14, [x1], #1
        cbnz w14, .Lname
        add x1, x1, #3
        and x1, x1, #~3
        b .Ltok

.Lend:  cbz x4, .Lnone
        sub x4, x4, #1
        mov x7, #0
        b .Ltok

.Lprop: add x8, x1, #8
        cmp x8, x3
        b.hi .Lnone
        ldr w9, [x1], #4                    /* len */
        rev w9, w9
        ldr w10, [x1], #4                   /* nameoff */
        rev w10, w10
        mov x11, x1                         /* value */
        add x1, x1, x9
        cmp x1, x3
        b.hi .Lnone
        add x1, x1, #3
        and x1, x1, #~3
        add x12, x2, x10                    /* property name */
        cmp x12, x3
        b.hs .Ltok
        cmp x4, #1
        b.ne .Lmem
        cmp x9, #4                          /* root cell counts */
        b.ne .Ltok
        adr x13, str_addr_cells
        bl fdt_streq
        b.ne 4f
        ldr w5, [x11]
        rev w5, w5
        b .Ltok
4:      adr x13, str_size_cells
        bl fdt_streq
        b.ne .Ltok
        ldr w6, [x11]
        rev w6, w6
        b .Ltok

.Lmem:  cbz x7, .Ltok
        adr x13, str_reg
        bl fdt_streq
        b.ne .Ltok
        cmp x5, #1
        b.lo .Lnone
        cmp x5, #2
        b.hi .Lnone
        cmp x6, #1
        b.lo .Lnone
        cmp x6, #2
        b.hi .Lnone
        add x8, x11, x9                     /* end of reg */
        add x9, x5, x6
        lsl x9, x9, #2                      /* bytes per (base, size) */
5:      add x10, x11, x9
        cmp x10, x8
        b.hi .Ltok
        mov x16, x5
        bl fdt_cells                        /* x15 = base */
        mov x17, x15
        mov x16, x6
        bl fdt_cells                        /* x15 = size */
        ldr x10, =RAM_BASE
        cmp x17, x10
        b.ne 5b
        mov x0, x15
        b 6f

.Lnone: mov x0, #0
6:      ldp x29, x30, [sp], #16
        ret

/* x11 = big-endian cells, x16 = 1 or 2.  x15 = value, x11 advanced. */
fdt_cells:
        ldr w15, [x11], #4
        rev w15, w15
        cmp x16, #2
        b.ne 1f
        ldr w14, [x11], #4
        rev w14, w14
        orr x15, x14, x15, lsl #32
1:      ret

/* Compare the strings at x12 and x13.  Flags EQ when equal. Uses x13-x16. */
fdt_streq:
        mov x14, x12
1:      ldrb w15, [x14], #1
        ldrb w16, [x13], #1
        cmp w15, w16
        b.ne 2f
        cbnz w15, 1b
2:      ret

str_memory:     .asciz "memory"
str_addr_cells: .asciz "#address-cells"
str_size_cells: .asciz "#size-cells"
str_reg:        .asciz "reg"
        .balign 4

/* MMU and D-cache off, little-endian, at the EL QEMU started us in
 * (EL1, EL2 or EL3); interrupts stay masked; the I-cache holds nothing
 * stale from before the image was loaded. Uses x5-x6 only. */
entry_state:
        msr daifset, #0xf
        mrs x5, CurrentEL
        ubfx x5, x5, #2, #2
        ldr x6, =SCTLR_CLEAR
        cmp x5, #3
        b.eq 3f
        cmp x5, #2
        b.eq 2f
        mrs x5, sctlr_el1
        bic x5, x5, x6
        msr sctlr_el1, x5
        b 9f
2:      mrs x5, sctlr_el2
        bic x5, x5, x6
        msr sctlr_el2, x5
        b 9f
3:      mrs x5, sctlr_el3
        bic x5, x5, x6
        msr sctlr_el3, x5
9:      isb
        ic iallu
        dsb ish
        isb
        ret

/* ------------------------------------------------------------------ */
/* fw_cfg                                                              */
/* ------------------------------------------------------------------ */

/* w0 = selector.  The selector register is big-endian. */
fwcfg_select:
        rev16 w0, w0
        strh w0, [x19, #FWCFG_SEL]
        ret

/* x0 = destination, w1 = length, w2 = selector.
 * Returns w0 = 0 on success, 1 if the device reported an error, 2 if the
 * transfer had not completed after 1 s by the generic counter (CNTPCT),
 * so a device that never finishes cannot hang the firmware. */
fwcfg_dma_read:
        sub sp, sp, #16
        /* FWCfgDmaAccess { be32 control; be32 length; be64 address; } */
        lsl w3, w2, #16
        mov w4, #(FWCFG_DMA_SELECT | FWCFG_DMA_READ)
        orr w3, w3, w4
        rev w3, w3
        str w3, [sp]
        rev w1, w1
        str w1, [sp, #4]
        rev x0, x0
        str x0, [sp, #8]
        dsb sy
        mov x0, sp
        rev x0, x0
        isb
        mrs x5, cntpct_el0
        mrs x6, cntfrq_el0                  /* ticks per second */
        add x5, x5, x6                      /* deadline */
        str x0, [x19, #FWCFG_DMA]           /* starts the transfer */
1:      ldr w3, [sp]
        rev w3, w3
        tst w3, #FWCFG_DMA_ERROR
        b.ne 2f
        and w3, w3, #~FWCFG_DMA_ERROR
        cbz w3, 4f
        isb
        mrs x6, cntpct_el0
        cmp x6, x5
        b.lo 1b
        mov w0, #2
        b 3f
4:      mov w0, #0
        b 3f
2:      mov w0, #1
3:      add sp, sp, #16
        ret

/* ------------------------------------------------------------------ */
/* PL011                                                               */
/* ------------------------------------------------------------------ */

uart_init:
        ldr x1, =UART_BASE
        mov w2, #UART_CR_ON
        str w2, [x1, #UART_CR]
        ret

/* x0 = zero-terminated string.  Gives up on a character after
 * UART_TIMEOUT polls so a stuck FIFO cannot hang the firmware. */
puts:
        ldr x1, =UART_BASE
1:      ldrb w2, [x0], #1
        cbz w2, 3f
        ldr w4, =UART_TIMEOUT
2:      ldr w3, [x1, #UART_FR]
        tbz w3, #5, 4f                      /* TXFF clear */
        subs w4, w4, #1
        b.ne 2b
4:      str w2, [x1, #UART_DR]
        b 1b
3:      ret

        .ltorg

banner:         .asciz "SquirrelBIOS ARM\n"
msg_loaded:     .asciz "kernel loaded at 0x40200000, jumping\n"
msg_no_fwcfg:   .asciz "boot: fw_cfg with DMA not found\n"
msg_no_kernel:  .asciz "boot: no kernel image (pass -kernel to QEMU)\n"
msg_too_big:    .asciz "boot: kernel does not fit in RAM\n"
msg_no_ram_size: .asciz "boot: RAM size not found (fw_cfg or device tree /memory)\n"
msg_load_failed: .asciz "boot: fw_cfg DMA read of the kernel failed\n"
msg_load_timeout: .asciz "boot: fw_cfg DMA read of the kernel did not complete\n"
msg_halted:     .asciz "boot halted\n"
