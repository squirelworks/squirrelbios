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

        ldr x0, =KERNEL_LOAD
        mov w1, w20
        mov w2, #FWCFG_KERNEL_DATA
        bl fwcfg_dma_read
        cbnz w0, load_failed

        adr x0, msg_loaded
        bl puts

        /* x0 = DTB if QEMU left one at the start of RAM, otherwise 0 */
        ldr x1, =RAM_BASE
        ldr w2, [x1]
        rev w2, w2
        ldr w3, =FDT_MAGIC
        mov x0, #0
        cmp w2, w3
        csel x0, x1, x0, eq

        bl entry_state

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
load_failed:
        adr x0, msg_load_failed
fail:
        bl puts
        adr x0, msg_halted
        bl puts
halt:
        wfi
        b halt

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
 * Returns w0 = 0 on success, non-zero on error. */
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
        str x0, [x19, #FWCFG_DMA]           /* starts the transfer */
1:      ldr w3, [sp]
        rev w3, w3
        tst w3, #FWCFG_DMA_ERROR
        b.ne 2f
        and w3, w3, #~FWCFG_DMA_ERROR
        cbnz w3, 1b
        mov w0, #0
        add sp, sp, #16
        ret
2:      mov w0, #1
        add sp, sp, #16
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
msg_load_failed: .asciz "boot: fw_cfg DMA read of the kernel failed\n"
msg_halted:     .asciz "boot halted\n"
